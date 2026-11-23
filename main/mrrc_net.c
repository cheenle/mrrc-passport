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
#include "mrrc_cfg.h"
#include "mrrc_net.h"
#include "mrrc_prov.h"
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
/* Spectrum tier the link is currently judged able to carry.  Declared up here
 * because the session handshake, the PTT path and the link-health watcher all
 * read or write it. */
static const char *s_link_tier = "high";

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

#if CONFIG_LWIP_IPV6
static void dump_ipv6(const char *when);
#endif

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
         * The tier is whatever the link was last judged able to carry (see
         * mrrc_net_spectrum_tier); "high" until the governor demotes it.
         * This was "mid" (divider 2, 425 bins, ~2.7 frames/s).  Each frame is
         * one row of waterfall history, so at half the sweep rate the display
         * could only ever advance in visible steps.  The 850-bin buffer this
         * firmware already allocates was sized for Full from the start; the
         * extra ~55 kbit/s is small next to the 256 kbit/s audio uplink. */
        char caps_msg[64];
        snprintf(caps_msg, sizeof caps_msg,
                 "{\"type\":\"spectrumCaps\",\"profile\":\"%s\"}", s_link_tier);
        esp_websocket_client_send_text(s_ws, caps_msg, (int)strlen(caps_msg),
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
        /* When the socket dies, the useful question is whether the IPv6 path is
         * still there: the server is IPv6-only, so a vanished prefix shows up
         * here as EHOSTUNREACH rather than as a Wi-Fi problem. */
        dump_ipv6("on ws error");
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
    /* Unkeying restores the tier the link was judged able to carry, not
     * unconditionally "high" — a starved link gets "high" back only after the
     * governor has seen it recover. */
    mrrc_net_spectrum_tier(on ? "low" : s_link_tier);
}

/* Which spectrum tier the link can carry.  Measured 2026-11-23 on a WAN path
 * (terminal on one ISP, server on another): the downlink collapsed to 17 KB/s
 * while the device kept asking for "high" — a 1701 B full frame at ~11 fps is
 * ~18 KB/s, i.e. more than the whole link, competing with the 32 KB/s of audio
 * that actually matters.  The terminal has no lower audio tier (AD-002: PCM16
 * only), so the only bandwidth it can give back is the waterfall's. */
void mrrc_net_spectrum_tier(const char *tier)
{
    if (!s_ws || !tier || !tier[0]) return;
    if (strcmp(tier, s_link_tier) != 0) s_link_tier = tier;
    char cap[64];
    snprintf(cap, sizeof cap, "{\"type\":\"spectrumCaps\",\"profile\":\"%s\"}", tier);
    esp_websocket_client_send_text(s_ws, cap, (int)strlen(cap), pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "spectrum tier -> %s", tier);
}

const char *mrrc_net_link_tier(void) { return s_link_tier; }

/* Settle the wire format.  Called from the UI tick (5 Hz), which is far more
 * often than needed but costs two comparisons.
 *
 * 2026-11-24: a missed ack used to set AUD_RATE_48K after 3 s.  That is not a
 * graceful fallback, it is a bandwidth trap: the 48 kHz tag makes the transmit
 * path upsample 3:1, so the uplink goes from 32 to ~96 KB/s — on the very link
 * that was already too slow to deliver one small text frame.  Measured on a
 * cellular hotspot, that sequence took 2.1 s to fail:
 *
 *   selftest: keyed 5000 ms
 *   transport_base: Poll timeout or error, timeout_ms=100
 *   websocket_client: esp_transport_write() returned 0
 *   net: /WSterminal error -> disconnected -> Failed to send the buffer
 *   tx done: frames=104 sendfail=146 read_max=110 ms
 *
 * So silence is no longer treated as "the server is legacy".  The declaration is
 * simply re-sent (the server's terminal handler ignores a repeat), and the wire
 * format stays 16 kHz: every reachable server speaks pcm16 (AD-038/AD-039), and
 * a dropped ack says nothing about that. */
#define AUD_CAPS_TRIES 5
void mrrc_net_audio_pump(void)
{
    static int tries;
    if (s_aud_rate != AUD_RATE_UNKNOWN || !s_aud_ask_us) return;
    if (esp_timer_get_time() - s_aud_ask_us < 3000000LL) return;   /* 3 s */
    s_aud_ask_us = esp_timer_get_time();
    if (tries >= AUD_CAPS_TRIES) {
        ESP_LOGW(TAG, "no audioCaps reply after %d asks; staying 16k "
                      "(a 48k uplink is 3x the bytes and cannot help)", tries);
        s_aud_rate = AUD_RATE_16K;
        return;
    }
    tries++;
    ESP_LOGW(TAG, "no audioCaps reply; re-sending declaration (%d/%d)", tries, AUD_CAPS_TRIES);
    mrrc_net_send_text_stop();     /* no-op if nothing is pending */
    static const char caps[] = "{\"type\":\"audioCaps\",\"profile\":\"pcm16\"}";
    if (s_ws) esp_websocket_client_send_text(s_ws, caps, (int)strlen(caps), pdMS_TO_TICKS(200));
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
    /* 100 ms, not 500.  Uplink frames are 20 ms of audio: if the socket cannot
     * take one in 100 ms it cannot take the next four either, and waiting half a
     * second per frame turns a congested link into 2 frames/s of TX while the
     * microphone keeps producing 50.  Dropping the frame is the honest answer —
     * the sender is already 5 frames behind.  Measured on a mobile link
     * 2026-11-22: 71 of 78 sends timed out at 500 ms; the 1 s transport timeout
     * that follows is what tore the websocket down. */
    /* 400 ms, not 100.  100 ms was chosen on 2026-11-22 to stop a congested
     * uplink from turning the TX loop into 2 frames/s — and that was right for
     * the failure it was aimed at.  It is wrong for the failure measured on
     * 2026-11-24: the websocket client serves TLS in *both* directions from one
     * task, so while it is draining a downlink burst the uplink write can wait
     * ~105 ms (`send_max=105269 us`), the 100 ms timeout expires, and
     * esp_websocket_client treats a failed write as fatal — the whole session is
     * torn down mid-transmission (`sendfail=137`, `/WSterminal error`, the server
     * logs "disconnected during TX", and the radio is forced to RX).
     *
     * A 400 ms wait costs at most ~20 dropped frames of audio in the worst case.
     * Losing the session costs the whole transmission plus a ~5 s reconnect.  So
     * the timeout is sized for "do not kill the socket", and the frame drop
     * policy stays: a write that still fails is a dropped frame, never a resend. */
    return esp_websocket_client_send_bin(s_ws, (const char *)data, (int)len,
                                         pdMS_TO_TICKS(400)) >= 0;
}

/* IF filter, as a position on the radio's own ladder.
 *
 * `filter_hz` is derived by the server (get_filter_hz(mode, filter_width)) and is
 * read-only; the settable field is `filter_width`, an index.  Sending Hz was why
 * the knob appeared to do nothing.  The ladder differs per mode group, so the
 * caller passes the index that belongs to the current mode. */
void mrrc_net_set_filter_width(int index)
{
    char msg[64];
    snprintf(msg, sizeof msg, "{\"type\":\"set\",\"field\":\"filter_width\",\"value\":%d}", index);
    ws_send_text(msg);
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
             cfg_srv_host(), cfg_srv_port());
    char body[128];
    snprintf(body, sizeof body, "{\"password\":\"%s\"}", cfg_srv_pass());

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

/* ── multiple networks ──────────────────────────────────────────────────
 * ESP-IDF configures one AP per station interface, so a list is an application
 * concern: scan once, connect to the strongest configured SSID that is present,
 * and on repeated failures move to the next candidate instead of retrying a
 * network that is not there.
 *
 * With only MRRC_WIFI_SSID set the behaviour is exactly what it was: one
 * candidate, and the scan only decides whether it is visible. */
#define WIFI_AP_MAX 3
typedef struct { const char *ssid; const char *password; } ap_cred_t;

static ap_cred_t s_ap[WIFI_AP_MAX];
static size_t s_ap_count;
static size_t s_ap_cur;              /* index being tried */
static uint32_t s_disc_streak;       /* consecutive no-IP disconnects */
static volatile bool s_switching;    /* a deliberate AP change is in flight */
static int s_ap_bad = -1;            /* demoted for this session after a health kick */

static void wifi_build_ap_list(void)
{
    /* Entry 0 is whatever NVS holds (provisioned or Kconfig), the rest are the
     * Kconfig alternates.  Same SSID twice is dropped below. */
    const char *ssid[WIFI_AP_MAX] = { cfg_wifi_ssid(), CONFIG_MRRC_WIFI_SSID2,
                                      CONFIG_MRRC_WIFI_SSID3 };
    const char *pass[WIFI_AP_MAX] = { cfg_wifi_pass(), CONFIG_MRRC_WIFI_PASSWORD2,
                                      CONFIG_MRRC_WIFI_PASSWORD3 };
    s_ap_count = 0;
    for (int i = 0; i < WIFI_AP_MAX; i++) {
        if (!ssid[i][0]) continue;
        bool dup = false;
        for (size_t k = 0; k < s_ap_count; k++)
            if (strcmp(s_ap[k].ssid, ssid[i]) == 0) dup = true;
        if (dup) continue;
        s_ap[s_ap_count].ssid = ssid[i];
        s_ap[s_ap_count].password = pass[i];
        s_ap_count++;
    }
    s_ap_cur = 0;
}

/* Index of the network that last obtained an IP, or -1.  Signal strength is a
 * poor proxy for quality — a strong AP can be the congested one — so a network
 * that worked before wins ties against a louder stranger. */
static int wifi_last_good(void)
{
    nvs_handle_t h;
    int32_t idx = -1;
    if (nvs_open("mrrc", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "wifi_last", &idx);
        nvs_close(h);
    }
    return (idx >= 0 && (size_t)idx < s_ap_count) ? (int)idx : -1;
}

static void wifi_note_good(size_t idx)
{
    nvs_handle_t h;
    if (nvs_open("mrrc", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "wifi_last", (int32_t)idx);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Scan and return the index of the strongest configured SSID that is present,
 * or 0 when nothing matches (so a single-network setup still tries its only
 * entry, and a hidden SSID still gets a chance). Blocking: ~1-2 s, once. */
static size_t wifi_pick_ap(void)
{
    if (s_ap_count <= 1) return 0;

    wifi_scan_config_t sc = { .show_hidden = true };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed; falling back to \"%s\"", s_ap[0].ssid);
        return 0;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return 0;
    if (n > 24) n = 24;
    wifi_ap_record_t rec[24];
    if (esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK) return 0;

    bool visible[WIFI_AP_MAX] = { false };
    int rssi[WIFI_AP_MAX];
    for (int k = 0; k < (int)s_ap_count; k++) rssi[k] = -127;
    for (int i = 0; i < n; i++) {
        for (size_t k = 0; k < s_ap_count; k++) {
            if (strcmp((const char *)rec[i].ssid, s_ap[k].ssid) != 0) continue;
            visible[k] = true;
            if (rec[i].rssi > rssi[k]) rssi[k] = rec[i].rssi;
        }
    }

    /* 1. the one that worked last time, if it is here and not demoted;
     * 2. otherwise the strongest visible that is not demoted;
     * 3. otherwise anything visible at all. */
    const int last = wifi_last_good();
    if (last >= 0 && visible[last] && last != s_ap_bad) {
        ESP_LOGI(TAG, "picked \"%s\" (rssi %d) of %u: last known good",
                 s_ap[last].ssid, rssi[last], (unsigned)s_ap_count);
        return (size_t)last;
    }
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; pass++) {
        for (size_t k = 0; k < s_ap_count; k++) {
            if (!visible[k]) continue;
            if (pass == 0 && (int)k == s_ap_bad) continue;   /* demoted */
            if (best < 0 || rssi[k] > rssi[best]) best = (int)k;
        }
    }
    if (best < 0) {
        ESP_LOGW(TAG, "none of %u configured SSIDs visible; trying \"%s\"",
                 (unsigned)s_ap_count, s_ap[0].ssid);
        return 0;
    }
    ESP_LOGI(TAG, "picked \"%s\" (rssi %d) of %u configured",
             s_ap[best].ssid, rssi[best], (unsigned)s_ap_count);
    return (size_t)best;
}

/* Strongest visible candidate that is NOT the one we are on. Returns `skip`
 * when there is nothing better to move to. */
static size_t wifi_pick_ap_except(size_t skip)
{
    if (s_ap_count < 2) return skip;
    wifi_scan_config_t sc = { .show_hidden = true };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return skip;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return skip;
    if (n > 24) n = 24;
    wifi_ap_record_t rec[24];
    if (esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK) return skip;

    int best = -1, best_rssi = -127;
    for (int i = 0; i < n; i++) {
        for (size_t k = 0; k < s_ap_count; k++) {
            if (k == skip) continue;
            if (strcmp((const char *)rec[i].ssid, s_ap[k].ssid) != 0) continue;
            if (rec[i].rssi > best_rssi) { best_rssi = rec[i].rssi; best = (int)k; }
        }
    }
    return best < 0 ? skip : (size_t)best;
}

static void wifi_apply_ap(size_t idx)
{
    s_ap_cur = idx;
    wifi_config_t cfg = { 0 };
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", s_ap[idx].ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", s_ap[idx].password);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    ESP_LOGI(TAG, "connecting to \"%s\" (%u/%u)", s_ap[idx].ssid,
             (unsigned)(idx + 1), (unsigned)s_ap_count);
}

/* Called when the *audio* says the link is unusable, not when the AP vanished.
 * A hotspot can stay associated while delivering 10% loss and multi-second
 * stalls, which `esp_wifi` never reports as a disconnect. */
void mrrc_net_wifi_kick(const char *reason)
{
    if (s_ap_count < 2) return;
    const size_t nxt = wifi_pick_ap_except(s_ap_cur);
    if (nxt == s_ap_cur) {
        ESP_LOGW(TAG, "wifi kick (%s): no other configured SSID visible, staying", reason);
        return;
    }
    ESP_LOGW(TAG, "wifi kick (%s): \"%s\" -> \"%s\"", reason,
             s_ap[s_ap_cur].ssid, s_ap[nxt].ssid);
    s_ap_bad = (int)s_ap_cur;      /* do not drift back to it this session */
    s_switching = true;
    s_disc_streak = 0;
    wifi_apply_ap(nxt);
    esp_wifi_disconnect();
    esp_wifi_connect();
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* Nothing: with several candidate networks the station must scan and pick
         * before it connects, so wifi_up() drives the first connect itself. */
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ctrl_up = false;
        if (s_switching) {
            s_switching = false;
            s_disc_streak = 0;
            esp_wifi_connect();
            return;
        }
        s_disc_streak++;
        if (s_ap_count > 1 && s_disc_streak >= 3) {
            /* This network is not coming back — the operator moved, or the AP
             * went away. Rescan rather than retrying it forever. */
            const uint32_t tries = s_disc_streak;
            s_disc_streak = 0;
            ESP_LOGW(TAG, "wifi down (%u tries on \"%s\"); rescanning",
                     (unsigned)tries, s_ap[s_ap_cur].ssid);
            wifi_apply_ap(wifi_pick_ap());
            esp_wifi_connect();
            return;
        }
        ESP_LOGW(TAG, "wifi down, reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ip = (const ip_event_got_ip_t *)data;
        s_disc_streak = 0;
        wifi_note_good(s_ap_cur);
        ESP_LOGI(TAG, "ip " IPSTR " on \"%s\"", IP2STR(&ip->ip_info.ip), s_ap[s_ap_cur].ssid);
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
    if (!cfg_has_wifi()) {
        /* Nothing to connect to and no way to type it in: become the access
         * point and let a phone hand us the credentials.  Does not return. */
        ESP_LOGW(TAG, "no credentials configured -> setup AP");
        mrrc_prov_run();
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

    wifi_build_ap_list();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* The scan needs the driver running, and the station does not auto-connect
     * until esp_wifi_connect(), so pick before asking to connect. */
    wifi_apply_ap(wifi_pick_ap());
    ESP_ERROR_CHECK(esp_wifi_connect());
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
             cfg_srv_host(), cfg_srv_port(), s_token);

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
        /* One retry absorbs a router that is rebooting; if it still cannot join,
         * the operator is somewhere none of the configured networks exist and
         * the only way out is the setup page.  Provisioning never returns: it
         * reboots into station mode once the phone has submitted the form. */
        ESP_LOGW(TAG, "wifi failed; retrying once in 30 s");
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (!wifi_up()) {
            ESP_LOGW(TAG, "still no network -> setup mode");
            mrrc_prov_run();
        }
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
