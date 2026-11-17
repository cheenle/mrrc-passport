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

#define WS_BUF_SIZE    4096
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

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "/WSterminal connected");
        s_ctrl_up = true;
        /* mid profile: 850 bins, 851-byte payloads — small enough to parse on
         * this chip and detailed enough for a 240-px scope. */
        esp_websocket_client_send_text(s_ws, "{\"type\":\"spectrumCaps\",\"profile\":\"mid\"}",
                                       (int)strlen("{\"type\":\"spectrumCaps\",\"profile\":\"mid\"}"),
                                       pdMS_TO_TICKS(500));
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "/WSterminal disconnected");
        s_ctrl_up = false;
        break;
    case WEBSOCKET_EVENT_DATA: {
        /* Only text frames carry control JSON.  A frame larger than the socket
         * buffer arrives in several events: payload_offset is the absolute
         * position inside the whole message, so reassemble first and parse
         * exactly once at the end.  Parsing a chunk yields "bad JSON (4096 B)". */
        /* Binary frames on this socket are downlink audio. */
        if (ev->op_code == 0x02) {
            if (!ev->data_ptr || ev->data_len <= 0) break;
            /* Two kinds of binary share this socket: 852-byte spectrum frames
             * ([0x02][0x01] + 850 bins) and ~130-byte audio frames
             * ([0x00][0x01] + Opus). */
            if (ev->data_len >= 852) {
                mrrc_ui_spectrum((const uint8_t *)ev->data_ptr + 2,
                                 (size_t)ev->data_len - 2);
            } else {
                mrrc_audio_feed((const uint8_t *)ev->data_ptr, (size_t)ev->data_len);
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
        handle_ctrl_text(s_acc, (int)s_acc_len);
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
    ESP_LOGI(TAG, "PTT %s", on ? "ON" : "OFF");
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
