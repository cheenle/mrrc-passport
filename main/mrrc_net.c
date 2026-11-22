// main/mrrc_net.c — Wi-Fi, login, /WSradio, JSON state.
//
// Protocol facts (all verified against mrrc_ft710/server.py and the xz-02
// firmware's probe runs):
//
//   POST /api/auth/login  {"password":...}  ->  {"ok":true,"token":"..."}
//   every WebSocket then carries   Authorization: Bearer <token>
//
//   /WSradio sends:  fullState {data:{...}}
//                    stateUpdate {fields:{...}}   (dirty fields only)
//                    value {field, value}
//                    error {message}
//   /WSradio accepts: {"type":"set","field":"freq","value":14074000}
//                     {"type":"set","field":"af_gain","value":128}
//
// The first frame after connecting is `onlineUsers`, not `fullState`; unknown
// types must be ignored rather than treated as errors.
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mrrc_audio.h"
#include "mrrc_net.h"
#include "mrrc_ui.h"
#include "nvs.h"

static const char *TAG = "net";

/* The client's rx buffer holds whatever arrived in one reads() — downlink audio
 * frames (642 B each, 50/s), spectrum frames (1701 B, ~5.3/s) and control
 * replies compete for it.  At 4096 a spectrum frame that lands on top of two
 * audio frames leaves too little room and esp_websocket_client blocks the
 * caller rather than dropping, which stalls the audio pump on the server side
 * (measured: `max pump stall 5791 ms`).  8 KB gives the reader room to drain a
 * burst without back-pressuring the socket. */
#define WS_BUF_SIZE    8192

/* Set while the operator is keyed.  Half duplex: the radio is not receiving
 * while it transmits, so any scope frame that arrives is stale before it is
 * drawn — and on this board every byte of it shares the one TCP connection
 * with the uplink audio. */
static volatile bool s_tx_busy;

/* Every payload byte that arrives on the socket, whatever its type.  Compared
 * with the audio-only counter in mrrc_audio.c this says whether a shortfall is
 * the device's receive ceiling (both numbers drop together) or the server
 * sending less audio than it sends spectrum. */
static uint32_t s_ws_rx_bytes;

/* Where the websocket task's CPU goes.  The three payload kinds it handles cost
 * wildly different amounts: audio and spectrum are memcpys, control JSON goes
 * through cJSON's allocator.  Split them so tuning is not guesswork. */
static uint32_t s_us_audio, s_us_spec, s_us_json;
static uint32_t s_n_audio, s_n_spec, s_n_json;
#define WS_TASK_STACK  6144
#define NET_TASK_STACK 6144
#define TOKEN_MAX      256
/* /WSradio fullState measures ~5.5 KB and arrives split across WS buffer
 * chunks; accumulate by payload_offset before parsing. */
#define WS_FRAME_MAX   6144   /* fullState measured 5662 B */

mrrc_state_t g_radio;
mrrc_chan_t g_chans[MRRC_MAX_CH];
int g_chan_count;

/* Channel frequency to re-assert after a mode change has settled. */
static int32_t s_pending_chan_freq;
static int64_t s_pending_chan_at;

static SemaphoreHandle_t s_lock;
static char s_token[TOKEN_MAX];
static char s_auth_hdr[TOKEN_MAX + 32];
static esp_websocket_client_handle_t s_ws;
static volatile bool s_ctrl_up;

/* Audio wire format actually in force.
 *
 * 16 kHz PCM16 is preferred: it is the native rate of this codec and of the
 * format the server calls "pcm16".  If the server does not confirm it — an
 * older box that never heard of the profile, or an explicit rejection — the
 * link falls back to the legacy 48 kHz raw PCM tag, which any server version
 * understands.  48 kHz costs bandwidth and needs downsampling here, so it is
 * strictly the fallback. */
typedef enum { AUD_RATE_UNKNOWN = 0, AUD_RATE_16K, AUD_RATE_48K } aud_rate_t;
static volatile aud_rate_t s_aud_rate = AUD_RATE_UNKNOWN;
static int64_t s_aud_ask_us;          /* when the profile was requested */
static char s_acc[WS_FRAME_MAX];
static size_t s_acc_len;

void mrrc_state_lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
void mrrc_state_unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

/* ── JSON → state ────────────────────────────────────────────────────── */

/* Returns which seq to bump: 1 = main, 2 = meter, 0 = nothing drawn. */
static int apply_field(const char *k, cJSON *v)
{
    mrrc_state_t *st = &g_radio;

    if (!strcmp(k, "vfo_a_freq") && cJSON_IsNumber(v)) {
        st->vfo_a_hz = (int32_t)v->valuedouble;
        return 1;
    }
    if (!strcmp(k, "vfo_b_freq") && cJSON_IsNumber(v)) {
        st->vfo_b_hz = (int32_t)v->valuedouble;
        return 1;
    }
    if (!strcmp(k, "active_freq") && cJSON_IsNumber(v)) {
        st->freq_hz = (int32_t)v->valuedouble;
        return 1;
    }
    if (!strcmp(k, "active_vfo") && cJSON_IsString(v)) {
        st->vfo = v->valuestring[0] ? v->valuestring[0] : 'A';
        return 1;
    }
    if ((!strcmp(k, "mode_display") || !strcmp(k, "mode_name")) && cJSON_IsString(v)) {
        snprintf(st->mode_name, sizeof st->mode_name, "%s", v->valuestring);
        return 1;
    }
    if ((!strcmp(k, "band_name") || !strcmp(k, "band")) && cJSON_IsString(v)) {
        snprintf(st->band_name, sizeof st->band_name, "%s", v->valuestring);
        return 1;
    }
    if (!strcmp(k, "filter_hz") && cJSON_IsNumber(v)) {
        st->filter_hz = (int)v->valuedouble;
        return 1;
    }
    /* SS05 index, not a frequency: 6 means 100 kHz.  The scope axis labels and
     * the waterfall's Hz-per-pixel both derive from this. */
    if (!strcmp(k, "scope_span") && cJSON_IsNumber(v)) {
        st->scope_span = (int)v->valuedouble;
        return 1;
    }
    if (!strcmp(k, "serial_connected") && cJSON_IsBool(v)) {
        st->radio_online = cJSON_IsTrue(v);
        return 1;
    }
    if (!strcmp(k, "tx_status") && cJSON_IsNumber(v)) {
        st->tx_status = (int)v->valuedouble;
        return 1;
    }
    if (!strcmp(k, "is_transmitting") && cJSON_IsBool(v)) {
        st->tx_status = cJSON_IsTrue(v) ? 1 : 0;
        return 1;
    }
    if (!strcmp(k, "s_meter") && cJSON_IsNumber(v)) {
        st->s_meter = (int)v->valuedouble;
        return 2;
    }
    if (!strcmp(k, "s_unit") && cJSON_IsString(v)) {
        snprintf(st->s_unit, sizeof st->s_unit, "%s", v->valuestring);
        return 2;
    }
    if (!strcmp(k, "af_gain") && cJSON_IsNumber(v)) {
        st->af_gain = (int)v->valuedouble;
        return 2;
    }
    /* TX meters.  The server nests them ("power_meter": {"power_watts": n}),
     * so these arrive both as flat keys from stateUpdate and as nested objects
     * inside fullState — handled by apply_object() below and by the explicit
     * meter pulls here. */
    if (!strcmp(k, "power_watts") && cJSON_IsNumber(v)) {
        st->power_w = (float)v->valuedouble;
        return 2;
    }
    if (!strcmp(k, "swr_ratio") && cJSON_IsNumber(v)) {
        st->swr = (float)v->valuedouble;
        return 2;
    }
    if (!strcmp(k, "power_meter") && cJSON_IsObject(v)) {
        const cJSON *w = cJSON_GetObjectItem(v, "power_watts");
        if (cJSON_IsNumber(w)) { st->power_w = (float)w->valuedouble; return 2; }
        return 0;
    }
    if (!strcmp(k, "swr_meter") && cJSON_IsObject(v)) {
        const cJSON *r = cJSON_GetObjectItem(v, "swr_ratio");
        if (cJSON_IsNumber(r)) { st->swr = (float)r->valuedouble; return 2; }
        return 0;
    }
    return 0;
}

static void apply_object(cJSON *obj, int *bump)
{
    if (!cJSON_IsObject(obj)) return;
    for (cJSON *it = obj->child; it; it = it->next) {
        if (!it->string) continue;
        const int b = apply_field(it->string, it);
        if (b) *bump |= b;
    }
}

static void handle_ctrl_text(const char *payload, int len)
{
    cJSON *root = cJSON_ParseWithLength(payload, (size_t)len);
    if (!root) {
        ESP_LOGW(TAG, "bad JSON (%d B)", len);
        return;
    }

    const cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";
    int bump = 0;

    /* The server's answer to our audioCaps request.  This must be read before
     * anything takes the state lock, and it settles the wire format for the
     * life of the connection. */
    if (!strcmp(t, "audioCaps")) {
        const cJSON *acc = cJSON_GetObjectItem(root, "accepted");
        if (cJSON_IsTrue(acc)) {
            const cJSON *rate = cJSON_GetObjectItem(root, "rate");
            s_aud_rate = AUD_RATE_16K;
            ESP_LOGI(TAG, "audio profile pcm16 accepted (%d Hz)",
                     cJSON_IsNumber(rate) ? (int)rate->valuedouble : 16000);
        } else {
            const cJSON *why = cJSON_GetObjectItem(root, "reason");
            ESP_LOGW(TAG, "pcm16 rejected (%s), falling back to 48k PCM",
                     cJSON_IsString(why) ? why->valuestring : "no reason given");
            s_aud_rate = AUD_RATE_48K;
        }
        cJSON_Delete(root);
        return;
    }

    mrrc_state_lock();
    if (!strcmp(t, "fullState")) {
        apply_object(cJSON_GetObjectItem(root, "data"), &bump);
        /* "memChannels": [{freq, mode, label}, ...] for the CHANNEL knob. */
        const cJSON *chans = cJSON_GetObjectItem(root, "memChannels");
        if (cJSON_IsArray(chans)) {
            g_chan_count = 0;
            int slots = 0;
            for (const cJSON *it = chans->child;
                 it && g_chan_count < MRRC_MAX_CH; it = it->next) {
                const cJSON *f = cJSON_GetObjectItem(it, "freq");
                const cJSON *m = cJSON_GetObjectItem(it, "mode");
                const cJSON *l = cJSON_GetObjectItem(it, "label");
                slots++;
                if (!cJSON_IsNumber(f)) continue;
                mrrc_chan_t *c = &g_chans[g_chan_count++];
                c->freq_hz = (int32_t)f->valuedouble;
                snprintf(c->mode, sizeof c->mode, "%s",
                         cJSON_IsString(m) ? m->valuestring : "");
                snprintf(c->label, sizeof c->label, "%s",
                         cJSON_IsString(l) ? l->valuestring : "");
            }
            ESP_LOGI(TAG, "memChannels: %d slots, %d usable", slots, g_chan_count);
        } else {
            ESP_LOGW(TAG, "fullState without memChannels array");
        }
        bump |= 1;
    } else if (!strcmp(t, "stateUpdate")) {
        cJSON *fields = cJSON_GetObjectItem(root, "fields");
        apply_object(fields, &bump);
        /* stateUpdate is dirty-field based; if it carried only the VFO pair we
         * still have to reconcile the active frequency the screen shows. */
        if (cJSON_GetObjectItem(fields, "active_freq") == NULL) {
            const int32_t want = (g_radio.vfo == 'B') ? g_radio.vfo_b_hz : g_radio.vfo_a_hz;
            if (want) g_radio.freq_hz = want;
        }
        bump |= 1;
    } else if (!strcmp(t, "value")) {
        const cJSON *f = cJSON_GetObjectItem(root, "field");
        if (cJSON_IsString(f))
            bump |= apply_field(f->valuestring, cJSON_GetObjectItem(root, "value"));
    } else if (!strcmp(t, "error")) {
        const cJSON *m = cJSON_GetObjectItem(root, "message");
        /* Force-RX notices arrive here too; they matter for PTT in S3. */
        ESP_LOGW(TAG, "server error: %s", cJSON_IsString(m) ? m->valuestring : "?");
    }
    if (bump & 1) g_radio.seq++;
    if (bump & 2) g_radio.meter_seq++;
    mrrc_state_unlock();

    cJSON_Delete(root);
}

/* ── WebSocket ───────────────────────────────────────────────────────── */

uint32_t mrrc_net_ws_rx_bytes(void) { return s_ws_rx_bytes; }

void mrrc_net_ws_cost(uint32_t *us_audio, uint32_t *us_spec, uint32_t *us_json,
                      uint32_t *n_audio, uint32_t *n_spec, uint32_t *n_json)
{
    if (us_audio) *us_audio = s_us_audio;
    if (us_spec)  *us_spec  = s_us_spec;
    if (us_json)  *us_json  = s_us_json;
    if (n_audio)  *n_audio  = s_n_audio;
    if (n_spec)   *n_spec   = s_n_spec;
    if (n_json)   *n_json   = s_n_json;
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "/WSterminal connected");
        s_ctrl_up = true;
        /* high = Full: one frame per radio sweep, 850 bins, ~5.3 frames/s.
         *
         * This was "mid" (divider 2, 425 bins, ~2.7 frames/s).  Each frame is
         * one row of waterfall history, so at half the sweep rate the display
         * could only ever advance in visible steps.  The 850-bin buffer this
         * firmware already allocates was sized for Full from the start; the
         * extra ~55 kbit/s is small next to the 256 kbit/s audio uplink. */
        esp_websocket_client_send_text(s_ws, "{\"type\":\"spectrumCaps\",\"profile\":\"high\"}",
                                       (int)strlen("{\"type\":\"spectrumCaps\",\"profile\":\"high\"}"),
                                       pdMS_TO_TICKS(500));
        /* Declare the PCM16 audio profile (server side: AD-038).
         *
         * This build links no decoder, so the legacy 0x01 Opus downlink would
         * be dropped on arrival, and the legacy 0x00 tag means *48 kHz* PCM —
         * playing that on a 16 kHz codec runs at 3x speed.  Profile "pcm16"
         * puts both directions on tag 0x02 at 16 kHz, which the server
         * resamples.  The field is "profile", and the server answers with its
         * accepted/rejected frame. */
        esp_websocket_client_send_text(s_ws, "{\"type\":\"audioCaps\",\"profile\":\"pcm16\"}",
                                       (int)strlen("{\"type\":\"audioCaps\",\"profile\":\"pcm16\"}"),
                                       pdMS_TO_TICKS(500));
        /* Ask, then wait to be told.  Until the server confirms, the uplink tag
         * stays 16 kHz too — a server that ignores the request never decodes
         * that tag either, so the failure is symmetric and visible rather than
         * half-working.  s_aud_ask_us drives the timeout in mrrc_net_audio_pump. */
        s_aud_rate = AUD_RATE_UNKNOWN;
        s_aud_ask_us = esp_timer_get_time();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "/WSterminal disconnected");
        s_ctrl_up = false;
        s_aud_rate = AUD_RATE_UNKNOWN;
        s_aud_ask_us = 0;
        break;
    case WEBSOCKET_EVENT_DATA: {
        /* Only text frames carry control JSON.  A frame larger than the socket
         * buffer arrives in several events: payload_offset is the absolute
         * position inside the whole message, so reassemble first and parse
         * exactly once at the end.  Parsing a chunk yields "bad JSON (4096 B)". */
        /* Binary frames on this socket are downlink audio. */
        if (ev->op_code == 0x02) {
            if (!ev->data_ptr || ev->data_len <= 0) break;
            s_ws_rx_bytes += (uint32_t)ev->data_len;
            /* Two kinds of binary share this socket, distinguished by the
             * channel byte the server puts in front of each frame:
             *   [0x00] rx audio, [0x02] spectrum (server: TERMINAL_CHAN_*).
             * Length was used to tell them apart, which worked only while audio
             * frames were ~130 B of Opus; 20 ms of 16 kHz PCM is 640 B and a
             * coalesced send can exceed the 852-byte scope frame, so a real
             * audio packet would have been handed to the spectrum parser. */
            const uint8_t chan = ((const uint8_t *)ev->data_ptr)[0];
            const int64_t t_ws = esp_timer_get_time();
            if (chan == 0x02) {
                /* Half duplex: drop scope while keyed.
                 *
                 * The server keeps sending until it processes our tier change,
                 * and the radio's own sweep is frozen anyway, so drawing these
                 * would put stale rows on the waterfall and spend CPU that the
                 * uplink needs.  Cheaper to throw the frame away than to
                 * redraw a picture of nothing. */
                if (s_tx_busy) break;
                mrrc_ui_spectrum((const uint8_t *)ev->data_ptr + 2,
                                 (size_t)ev->data_len - 2);
                s_us_spec += (uint32_t)(esp_timer_get_time() - t_ws);
                s_n_spec++;
            } else if (chan == 0x00) {
                /* Skip the channel byte: mrrc_audio_feed works from the codec
                 * tag onward. */
                mrrc_audio_feed((const uint8_t *)ev->data_ptr + 1,
                                (size_t)ev->data_len - 1);
                s_us_audio += (uint32_t)(esp_timer_get_time() - t_ws);
                s_n_audio++;
            }
            break;
        }
        if (ev->op_code != 0x01 || ev->payload_len <= 0 || !ev->data_ptr) break;
        if (ev->payload_len > WS_FRAME_MAX) {
            ESP_LOGW(TAG, "message too big: %d B", ev->payload_len);
            break;
        }
        if (ev->payload_offset == 0) s_acc_len = 0;
        memcpy(s_acc + ev->payload_offset, ev->data_ptr, (size_t)ev->data_len);
        if (ev->payload_offset + ev->data_len < ev->payload_len) break;  /* more to come */
        s_acc_len = (size_t)ev->payload_len;
        const int64_t t_json = esp_timer_get_time();
        handle_ctrl_text(s_acc, (int)s_acc_len);
        s_us_json += (uint32_t)(esp_timer_get_time() - t_json);
        s_n_json++;
        break;
    }
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "/WSterminal error");
        break;
    default:
        break;
    }
}

static void ws_send_text(const char *text)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return;
    esp_websocket_client_send_text(s_ws, text, (int)strlen(text), pdMS_TO_TICKS(1000));
}

const char *mrrc_auth_header(void) { return s_auth_hdr; }
const char *mrrc_token(void) { return s_token; }

void mrrc_net_set_freq(int32_t hz)
{
    char msg[64];
    snprintf(msg, sizeof msg, "{\"type\":\"set\",\"field\":\"freq\",\"value\":%ld}", (long)hz);
    ws_send_text(msg);
}

void mrrc_net_set_ptt(bool on)
{
    char msg[64];
    snprintf(msg, sizeof msg, "{\"type\":\"set\",\"field\":\"ptt\",\"value\":%s}",
             on ? "true" : "false");
    ws_send_text(msg);

    /* Drop the spectrum tier for the duration of the transmission.
     *
     * Everything shares one TCP connection on this board, so a Full spectrum
     * frame (1701 B) queued between two uplink audio frames is 1701 B of
     * latency in front of the operator's voice.  The waterfall is not worth
     * that while talking — it is already scrolling off-screen data.  "low" is
     * the server's cheapest tier; the display keeps updating, just coarser.
     *
     * Restored on unkey so the operator gets the full-resolution sweep back
     * the moment they stop talking. */
    s_tx_busy = on;
    const char *tier = on ? "low" : "high";
    char cap[64];
    snprintf(cap, sizeof cap, "{\"type\":\"spectrumCaps\",\"profile\":\"%s\"}", tier);
    esp_websocket_client_send_text(s_ws, cap, (int)strlen(cap), pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "PTT %s, spectrum %s", on ? "ON" : "OFF", tier);
}

/* Settle the wire format.  Called from the UI tick (5 Hz), which is far more
 * often than needed but costs two comparisons.  A server that never answers
 * would otherwise leave the terminal in AUD_RATE_UNKNOWN for ever — 16 kHz
 * frames out, nothing usable in — so silence is treated as "no", after a grace
 * period long enough for a slow but healthy link to reply. */
void mrrc_net_audio_pump(void)
{
    if (s_aud_rate != AUD_RATE_UNKNOWN || !s_aud_ask_us) return;
    if (esp_timer_get_time() - s_aud_ask_us < 3000000LL) return;   /* 3 s */
    ESP_LOGW(TAG, "no audioCaps reply in 3 s, falling back to 48k PCM");
    s_aud_rate = AUD_RATE_48K;
}

/* True while the link is carrying 16 kHz PCM16 (the preferred format). */
bool mrrc_net_audio_16k(void)
{
    return s_aud_rate != AUD_RATE_48K;   /* unconfirmed papers over as 16k */
}

void mrrc_net_set_chan(int index)
{
    if (index < 0 || index >= g_chan_count) return;
    const mrrc_chan_t *c = &g_chans[index];

    /* Mode FIRST, then frequency.
     *
     * Switching mode makes the rig apply its own mode-dependent offset (CW pitch
     * and friends), which moved the displayed frequency by about 1.4 kHz when
     * the order was freq-then-mode: the frequency we had just written got
     * re-quantised by the mode change.  The browser client sets mode first, and
     * the stored frequency is what the operator expects to come back, so the
     * same order is used here.
     */
    if (c->mode[0]) {
        char msg[64];
        snprintf(msg, sizeof msg,
                 "{\"type\":\"set\",\"field\":\"mode\",\"value\":\"%s\"}", c->mode);
        ws_send_text(msg);
    }
    mrrc_net_set_freq(c->freq_hz);

    /* Re-assert shortly after: the mode transition is asynchronous on the rig
     * side, so one more write guarantees the channel's own frequency is what
     * ends up active rather than whatever the transition left behind. */
    s_pending_chan_freq = c->freq_hz;
    s_pending_chan_at = esp_timer_get_time() + 400000;

    ESP_LOGI(TAG, "chan %d -> %ld %s", index, (long)c->freq_hz, c->mode);
}

/* Called from the network task's pump; re-asserts a channel frequency once the
 * mode transition has settled. */
void mrrc_net_chan_pump(void)
{
    if (!s_pending_chan_freq) return;
    if (esp_timer_get_time() < s_pending_chan_at) return;
    const int32_t hz = s_pending_chan_freq;
    s_pending_chan_freq = 0;
    mrrc_net_set_freq(hz);
    ESP_LOGI(TAG, "chan freq re-asserted %ld", (long)hz);
}

void mrrc_net_next_mode(void)
{
    static const char *MODES[] = {"LSB", "USB", "CW-U", "AM", "FM", "RTTY-L", "DATA-L"};
    const int n = sizeof MODES / sizeof MODES[0];
    int cur = 0;
    mrrc_state_lock();
    for (int i = 0; i < n; i++)
        if (!strcmp(g_radio.mode_name, MODES[i])) { cur = i; break; }
    mrrc_state_unlock();
    const char *next = MODES[(cur + 1) % n];
    char msg[64];
    snprintf(msg, sizeof msg,
             "{\"type\":\"set\",\"field\":\"mode\",\"value\":\"%s\"}", next);
    ws_send_text(msg);
    ESP_LOGI(TAG, "mode -> %s", next);
}

void mrrc_net_send_txhb(void) { ws_send_text("{\"type\":\"txhb\"}"); }

void mrrc_net_send_text_stop(void) { ws_send_text("s:"); }

bool mrrc_net_send_bin(const void *data, size_t len)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return false;
    return esp_websocket_client_send_bin(s_ws, (const char *)data, (int)len,
                                         pdMS_TO_TICKS(500)) >= 0;
}

void mrrc_net_set_af_gain(int gain)
{
    char msg[64];
    snprintf(msg, sizeof msg, "{\"type\":\"set\",\"field\":\"af_gain\",\"value\":%d}", gain);
    ws_send_text(msg);
}

/* ── token persistence ───────────────────────────────────────────────────
 * The server keeps `_auth_tokens` in process memory with no TTL, so a token
 * stays valid until the server restarts.  Caching it in NVS means a reboot
 * (which happens on every flash during development) does not consume one of
 * the 5 logins per 5 minutes that /api/auth/login allows per IP. */

static void token_store(void)
{
    nvs_handle_t h;
    if (nvs_open("mrrc", NVS_READWRITE, &h) != ESP_OK) return;
    if (s_token[0]) (void)nvs_set_str(h, "token", s_token);
    else (void)nvs_erase_key(h, "token");
    (void)nvs_commit(h);
    nvs_close(h);
}

static void token_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof s_token;
    if (nvs_open("mrrc", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_str(h, "token", s_token, &len) == ESP_OK && s_token[0]) {
        snprintf(s_auth_hdr, sizeof s_auth_hdr, "Authorization: Bearer %s\r\n", s_token);
        ESP_LOGI(TAG, "reusing cached token");
    }
    nvs_close(h);
}

/* ── login ───────────────────────────────────────────────────────────── */

static bool mrrc_login(void)
{
    char url[160];
    snprintf(url, sizeof url, "https://%s:%d/api/auth/login",
             CONFIG_MRRC_SERVER_HOST, CONFIG_MRRC_SERVER_PORT);
    char body[128];
    snprintf(body, sizeof body, "{\"password\":\"%s\"}", CONFIG_MRRC_SERVER_PASSWORD);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        /* The box serves a self-signed certificate; chain validation cannot
         * succeed.  Requires ESP_TLS_INSECURE + ESP_TLS_SKIP_SERVER_CERT_VERIFY. */
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;

    char resp[384] = {0};
    int code = -1;
    if (esp_http_client_open(cli, (int)strlen(body)) == ESP_OK) {
        esp_http_client_write(cli, body, (int)strlen(body));
        esp_http_client_fetch_headers(cli);
        const int n = esp_http_client_read(cli, resp, sizeof(resp) - 1);
        if (n > 0) resp[n] = 0;
        code = esp_http_client_get_status_code(cli);
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (code != 200) {
        ESP_LOGE(TAG, "login failed (HTTP %d)", code);
        return false;
    }
    cJSON *root = cJSON_Parse(resp);
    if (!root) return false;
    const cJSON *tok = cJSON_GetObjectItem(root, "token");
    const bool ok = cJSON_IsString(tok) && tok->valuestring[0];
    if (ok) {
        snprintf(s_token, sizeof s_token, "%s", tok->valuestring);
        snprintf(s_auth_hdr, sizeof s_auth_hdr, "Authorization: Bearer %s\r\n", s_token);
        token_store();
        ESP_LOGI(TAG, "logged in");
    }
    cJSON_Delete(root);
    return ok;
}

/* ── Wi-Fi + supervisor ──────────────────────────────────────────────── */

static esp_netif_t *s_sta_netif;
static EventGroupHandle_t s_net_ev;
#define NET_UP   BIT0
#define NET_FAIL BIT1

/* Print every IPv6 address the interface holds (link-local included).  No
 * fe80:: means the stack never came up; fe80:: without a global address means
 * the router's Router Advertisements are not being accepted. */
static void dump_ipv6(const char *when)
{
    if (!s_sta_netif) return;
    esp_ip6_addr_t v6[3];
    const int n = esp_netif_get_all_ip6(s_sta_netif, v6);
    ESP_LOGW(TAG, "%s: %d IPv6 address(es)", when, n);
    for (int i = 0; i < n; i++)
        ESP_LOGW(TAG, "  ipv6[%d] %08x:%08x:%08x:%08x zone=%d", i,
                 (unsigned)v6[i].addr[0], (unsigned)v6[i].addr[1],
                 (unsigned)v6[i].addr[2], (unsigned)v6[i].addr[3], v6[i].zone);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ctrl_up = false;
        ESP_LOGW(TAG, "wifi down, reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ip = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "ip " IPSTR, IP2STR(&ip->ip_info.ip));
#if CONFIG_LWIP_IPV6
        /* The interface must be UP before a link-local address can be
         * derived; calling this at create time silently does nothing.
         * SLAAC then follows from CONFIG_LWIP_IPV6_AUTOCONFIG once the router
         * sends a Router Advertisement. */
        esp_err_t e6 = esp_netif_create_ip6_linklocal(s_sta_netif);
        ESP_LOGW(TAG, "ip6 linklocal: %s", esp_err_to_name(e6));
#endif
        dump_ipv6("after IPv4");
        xEventGroupSetBits(s_net_ev, NET_UP);
    }
}



static bool wifi_up(void)
{
    if (!CONFIG_MRRC_WIFI_SSID[0]) {
        ESP_LOGE(TAG, "no SSID configured");
        return false;
    }
    s_net_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t cfg = { 0 };
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", CONFIG_MRRC_WIFI_SSID);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", CONFIG_MRRC_WIFI_PASSWORD);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* A talk terminal cannot tolerate modem sleep: it delays inbound audio by
     * 100 ms+ and bursts the downlink.  Cost is idle current. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    const bool up = (xEventGroupWaitBits(s_net_ev, NET_UP, pdFALSE, pdFALSE,
                                         pdMS_TO_TICKS(25000)) & NET_UP) != 0;
    if (up) {
        /* SLAAC completes on the next RA, usually within a few seconds. */
        vTaskDelay(pdMS_TO_TICKS(6000));
        dump_ipv6("6 s after IPv4");
    }
    return up;
}

static void start_ws(void)
{
    if (s_ws) return;
    /* server.py checks `?token=` (ws.query_params), NOT the Authorization
     * header: a browser WebSocket cannot set headers.  Sending both satisfies
     * either server version. */
    char uri[320];
    /* One socket for control, downlink audio and uplink audio.  The three
     * separate endpoints (/WSradio, /WSaudioRX, /WSaudioTX) each cost a TLS
     * session; on a C3 with no PSRAM that was the binding constraint. */
    snprintf(uri, sizeof uri, "wss://%s:%d/WSterminal?token=%s",
             CONFIG_MRRC_SERVER_HOST, CONFIG_MRRC_SERVER_PORT, s_token);

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .headers = s_auth_hdr,
        .buffer_size = WS_BUF_SIZE,
        .task_stack = WS_TASK_STACK,
        .reconnect_timeout_ms = 3000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 15,
        .pingpong_timeout_sec = 25,
        .skip_cert_common_name_check = true,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "ws init failed");
        return;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_websocket_client_start(s_ws);
    ESP_LOGI(TAG, "/WSterminal -> %s", uri);
}

static void net_task(void *arg)
{
    (void)arg;
    if (!wifi_up()) {
        ESP_LOGE(TAG, "wifi failed; /WSradio not started");
        vTaskDelete(NULL);
    }
    /* The server rate-limits /api/auth/login to 5 attempts per 5 minutes per
     * IP.  A fixed short retry parks this device inside the window forever,
     * so back off exponentially (30 s doubling to 10 min) and reset on
     * success. */
    uint32_t backoff_s = 30;
    for (;;) {
        if (!s_ws) {
            if (!s_token[0]) {
                if (!mrrc_login()) {
                    ESP_LOGW(TAG, "login failed, retry in %us", (unsigned)backoff_s);
                    vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
                    backoff_s = backoff_s * 2 > 600 ? 600 : backoff_s * 2;
                    continue;
                }
                backoff_s = 30;
            }
            start_ws();
            /* Give the socket time to come up; a cached token rejected by the
             * server shows up as "never connected", not as an error event. */
            vTaskDelay(pdMS_TO_TICKS(12000));
            if (!s_ctrl_up) {
                ESP_LOGW(TAG, "no control link with cached token; re-login");
                esp_websocket_client_destroy(s_ws);
                s_ws = NULL;
                s_token[0] = 0;
                s_auth_hdr[0] = 0;
                token_store();
                continue;
            }
            continue;
        } else if (!esp_websocket_client_is_connected(s_ws)) {
            /* The client reconnects on its own; only rebuild (and re-login)
             * when it has been down long enough that the token may be stale. */
            vTaskDelay(pdMS_TO_TICKS(20000));
            if (!esp_websocket_client_is_connected(s_ws)) {
                esp_websocket_client_destroy(s_ws);
                s_ws = NULL;
                ESP_LOGW(TAG, "rebuilding /WSterminal");
                continue;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void mrrc_net_start(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    memset(&g_radio, 0, sizeof g_radio);
    g_radio.vfo = 'A';
    g_radio.power_w = -1.0f;   /* -1 = not reported */
    g_radio.swr = 0.0f;
    if (!s_lock) {
        ESP_LOGE(TAG, "no mutex");
        return;
    }
    token_load();
    if (xTaskCreate(net_task, "net", NET_TASK_STACK, NULL, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "net task failed");
}

bool mrrc_net_up(void) { return s_ctrl_up; }

int mrrc_net_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}
