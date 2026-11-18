/*
 * M2 — the MRRC client, passport-shaped: ONE /WSterminal socket carries
 * control (JSON), spectrum (chan 0x02) and both audio directions
 * (chan 0x00 down, 0x01 up). PCM16 at 16 kHz, no codec on the wire.
 *
 * Why not Opus / four sockets (measured on this board, 2026-11-19):
 *   /WSradio + /WSspectrum + /WSaudioRX + /WSaudioTX leaves the internal heap
 *   at ~2.4 KB (largest block 2048) and the fourth socket's task cannot be
 *   created; shrinking the mbedtls record buffers to 4 KB breaks /WSradio
 *   with -0x7100 because a fullState frame is 5.5 KB. PSRAM task stacks crash
 *   at boot here. passport's single socket is the design that fits.
 */
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/message_buffer.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "mrrc_link.h"

#define WIFI_BIT   BIT0
#define WS_BUF     4096
#define FRAME_MAX  8192
#define TOKEN_MAX  160

/* /WSterminal channel byte (server: TERMINAL_CHAN_*) */
#define CH_RX_AUDIO 0x00
#define CH_TX_AUDIO 0x01
#define CH_SPECTRUM 0x02

/* audio payload tag (server: AUDIO_TAG_*) */
#define TAG_PCM48  0x00
#define TAG_OPUS   0x01
#define TAG_PCM16  0x02

static const char *TAG = "m2";

static mrrc_cfg_t s_cfg;
static mrrc_state_t s_st;
static EventGroupHandle_t s_net_ev;
static char s_token[TOKEN_MAX];
static char s_auth_hdr[TOKEN_MAX + 32];
static esp_websocket_client_handle_t s_ws;
static MessageBufferHandle_t s_abuf;
static char s_acc[FRAME_MAX];
static size_t s_acc_len;
static uint8_t s_spec[1024];
static int s_spec_len;

bool mrrc_link_up(void) { return s_st.up; }
const mrrc_state_t *mrrc_state(void) { return &s_st; }

int mrrc_link_spectrum(uint8_t *out, int max)
{
    int n = s_spec_len < max ? s_spec_len : max;
    if (n > 0) memcpy(out, s_spec, (size_t)n);
    return n;
}

/* ── JSON state ─────────────────────────────────────────────────────────── */

static void apply_field(const char *k, const cJSON *v)
{
    if (!strcmp(k, "active_freq") && cJSON_IsNumber(v)) {
        s_st.freq_hz = (int32_t)v->valuedouble;
    } else if ((!strcmp(k, "mode_display") || !strcmp(k, "mode_name")) && cJSON_IsString(v)) {
        snprintf(s_st.mode, sizeof s_st.mode, "%s", v->valuestring);
    } else if ((!strcmp(k, "band_name") || !strcmp(k, "band")) && cJSON_IsString(v)) {
        snprintf(s_st.band, sizeof s_st.band, "%s", v->valuestring);
    } else if (!strcmp(k, "filter_hz") && cJSON_IsNumber(v)) {
        s_st.filter_hz = (int)v->valuedouble;
    } else if (!strcmp(k, "scope_span") && cJSON_IsNumber(v)) {
        s_st.scope_span = (int)v->valuedouble;
    } else if (!strcmp(k, "smeter") && cJSON_IsNumber(v)) {
        s_st.smeter = (int)v->valuedouble;
    } else if (!strcmp(k, "serial_connected") && cJSON_IsBool(v)) {
        s_st.serial_connected = cJSON_IsTrue(v);
    }
}

static void apply_object(const cJSON *obj)
{
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, obj) {
        if (it->string && (cJSON_IsString(it) || cJSON_IsNumber(it) || cJSON_IsBool(it))) {
            apply_field(it->string, it);
        }
    }
}

static void handle_text(const char *payload, int len)
{
    if (len <= 0) return;
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) return;
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";

    if (!strcmp(t, "fullState")) {
        const cJSON *data = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsObject(data)) apply_object(data);
        s_st.state_updates++;
        ESP_LOGI(TAG, "fullState: %ld Hz %s %s span=%d serial=%d",
                 (long)s_st.freq_hz, s_st.mode, s_st.band, s_st.scope_span,
                 (int)s_st.serial_connected);
    } else if (!strcmp(t, "stateUpdate") || !strcmp(t, "value")) {
        const cJSON *f = cJSON_GetObjectItem(root, "field");
        const cJSON *v = cJSON_GetObjectItem(root, "value");
        if (cJSON_IsString(f) && v) {
            apply_field(f->valuestring, v);
        } else {
            const cJSON *data = cJSON_GetObjectItem(root, "data");
            if (cJSON_IsObject(data)) apply_object(data);
        }
        s_st.state_updates++;
    } else if (!strcmp(t, "audioCaps")) {
        const cJSON *acc = cJSON_GetObjectItem(root, "accepted");
        const cJSON *rate = cJSON_GetObjectItem(root, "rate");
        ESP_LOGI(TAG, "audioCaps accepted=%d rate=%d",
                 cJSON_IsTrue(acc), cJSON_IsNumber(rate) ? (int)rate->valuedouble : -1);
    } else if (!strcmp(t, "spectrumCaps")) {
        const cJSON *acc = cJSON_GetObjectItem(root, "accepted");
        ESP_LOGI(TAG, "spectrumCaps accepted=%d", cJSON_IsTrue(acc));
    } else if (!strcmp(t, "error")) {
        const cJSON *m = cJSON_GetObjectItem(root, "message");
        ESP_LOGW(TAG, "server error: %s", cJSON_IsString(m) ? m->valuestring : "?");
    }
    cJSON_Delete(root);
}

/* ── binary frames: [chan][tag][payload] ────────────────────────────────── */

static void handle_bin(const uint8_t *data, int len)
{
    if (len < 2) return;
    const uint8_t chan = data[0];

    if (chan == CH_SPECTRUM) {
        s_st.spectrum_frames++;
        s_spec_len = len - 1 < (int)sizeof s_spec ? len - 1 : (int)sizeof s_spec;
        memcpy(s_spec, data + 1, (size_t)s_spec_len);
        return;
    }
    if (chan != CH_RX_AUDIO) return;

    /* Only audio reaches the queue; the audio task owns playback. Dropping the
     * Opus tag here is deliberate: this build declares pcm16 and there is no
     * decoder linked, so an Opus frame would be noise. */
    const uint8_t tag = data[1];
    if (tag != TAG_PCM16 && tag != TAG_PCM48) {
        s_st.audio_drops++;
        return;
    }
    s_st.audio_frames++;
    if (!s_abuf || xMessageBufferSend(s_abuf, data, (size_t)len, 0) != (size_t)len) {
        s_st.audio_drops++;
    }
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "/WSterminal connected");
        {
            static const char ac[] = "{\"type\":\"audioCaps\",\"profile\":\"pcm16\"}";
            static const char sc[] = "{\"type\":\"spectrumCaps\",\"profile\":\"mid\"}";
            esp_websocket_client_send_text(s_ws, ac, (int)strlen(ac), pdMS_TO_TICKS(1000));
            esp_websocket_client_send_text(s_ws, sc, (int)strlen(sc), pdMS_TO_TICKS(1000));
        }
        s_st.up = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "/WSterminal disconnected");
        s_st.up = false;
        break;
    case WEBSOCKET_EVENT_DATA:
        if (ev->op_code == 0x08) break;
        if (ev->op_code == 0x02) {
            handle_bin((const uint8_t *)ev->data_ptr, ev->data_len);
        } else if (ev->op_code == 0x01) {
            if (s_acc_len + ev->data_len < sizeof s_acc) {
                memcpy(s_acc + s_acc_len, ev->data_ptr, ev->data_len);
                s_acc_len += ev->data_len;
            }
            if (ev->payload_offset + ev->data_len >= ev->payload_len) {
                handle_text(s_acc, (int)s_acc_len);
                s_acc_len = 0;
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "/WSterminal error");
        break;
    }
}

/* ── uplink + PTT ───────────────────────────────────────────────────────── */

static bool send_bin(const uint8_t *data, int len)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return false;
    return esp_websocket_client_send_bin(s_ws, (const char *)data, len, pdMS_TO_TICKS(200)) >= 0;
}

bool mrrc_link_send_pcm(const int16_t *pcm, int samples)
{
    static uint8_t frame[64 + 960 * 2];
    if (samples <= 0 || samples > 960) return false;
    frame[0] = CH_TX_AUDIO;
    frame[1] = TAG_PCM16;                 /* 16 kHz Int16, the server resamples */
    memcpy(frame + 2, pcm, (size_t)samples * 2);
    return send_bin(frame, 2 + samples * 2);
}

void mrrc_link_send_ptt_heartbeat(void)
{
    static const char hb[] = "{\"type\":\"txhb\"}";
    if (s_ws && esp_websocket_client_is_connected(s_ws)) {
        esp_websocket_client_send_text(s_ws, hb, (int)strlen(hb), pdMS_TO_TICKS(200));
    }
}

void mrrc_link_set_ptt(bool on)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return;
    char b[64];
    snprintf(b, sizeof b, "{\"type\":\"set\",\"field\":\"ptt\",\"value\":%s}", on ? "true" : "false");
    if (!on) {
        /* Stop capture first so the tail of our last frame is not chopped. */
        esp_websocket_client_send_text(s_ws, "s:", 2, pdMS_TO_TICKS(500));
    }
    esp_websocket_client_send_text(s_ws, b, (int)strlen(b), pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "ptt %s", on ? "on" : "off");
}

int mrrc_link_take_audio(uint8_t *buf, int max)
{
    if (!s_abuf) return 0;
    return (int)xMessageBufferReceive(s_abuf, buf, (size_t)max, 0);
}

/* ── login / wifi ───────────────────────────────────────────────────────── */

static bool mrrc_login(void)
{
    char url[160];
    snprintf(url, sizeof url, "https://%s:%d/api/auth/login", s_cfg.host, s_cfg.port);
    char body[128];
    snprintf(body, sizeof body, "{\"password\":\"%s\"}", s_cfg.server_pass);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;

    char resp[384] = {0};
    int code = -1;
    if (esp_http_client_open(cli, (int)strlen(body)) == ESP_OK) {
        esp_http_client_write(cli, body, (int)strlen(body));
        esp_http_client_fetch_headers(cli);
        int n = esp_http_client_read(cli, resp, sizeof(resp) - 1);
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
    bool ok = cJSON_IsString(tok) && tok->valuestring[0];
    if (ok) {
        snprintf(s_token, sizeof s_token, "%s", tok->valuestring);
        snprintf(s_auth_hdr, sizeof s_auth_hdr, "Authorization: Bearer %s\r\n", s_token);
        ESP_LOGI(TAG, "logged in");
    }
    cJSON_Delete(root);
    return ok;
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "wifi disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_net_ev, WIFI_BIT);
    }
}

static bool wifi_up(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    s_net_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL));

    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", s_cfg.ssid);
    snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", s_cfg.wifi_pass);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_net_ev, WIFI_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(20000));
    return (bits & WIFI_BIT) != 0;
}

static void ws_open(void)
{
    char uri[320];
    snprintf(uri, sizeof uri, "wss://%s:%d/WSterminal?token=%s", s_cfg.host, s_cfg.port, s_token);

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .headers = s_auth_hdr,
        .buffer_size = WS_BUF,
        .task_stack = 6144,
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
    ESP_LOGI(TAG, "/WSterminal -> wss://%s:%d/WSterminal", s_cfg.host, s_cfg.port);
}

static void link_task(void *arg)
{
    (void)arg;
    if (!wifi_up()) {
        ESP_LOGE(TAG, "wifi failed");
        vTaskDelete(NULL);
    }
    if (!s_abuf) s_abuf = xMessageBufferCreate(16 * 1024);

    uint32_t backoff_s = 30;
    for (;;) {
        if (!s_token[0]) {
            if (!mrrc_login()) {
                ESP_LOGW(TAG, "retry in %us", (unsigned)backoff_s);
                vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
                backoff_s = backoff_s * 2 > 600 ? 600 : backoff_s * 2;
                continue;
            }
            backoff_s = 30;
            ws_open();
            ESP_LOGI(TAG, "heap: internal free=%u largest=%u psram free=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void mrrc_link_start(const mrrc_cfg_t *cfg)
{
    s_cfg = *cfg;
    memset(&s_st, 0, sizeof s_st);
    xTaskCreatePinnedToCore(link_task, "mrrc_link", 7168, NULL, 5, NULL, 1);
}
