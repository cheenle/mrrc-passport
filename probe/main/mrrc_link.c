/*
 * M2a — the smallest MRRC client that proves the wire: Wi-Fi → login →
 * /WSradio (JSON state) → /WSpectrum (binary 850 bins).
 *
 * Deliberately narrower than passport's mrrc_net.c:
 *   - no token persistence (probe reboots often, one login per boot)
 *   - no PTT, no audio, no channel table (M2b/M5)
 *   - only the fields the probe actually prints
 * Everything here is lifted from passport's mrrc_net.c, including the two
 * non-obvious bits: the token rides in the query string (?token=), and the box
 * serves a self-signed certificate, so chain validation must be off.
 */
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "mrrc_link.h"

#define WIFI_BIT  BIT0
#define WS_BUF    8192
#define FRAME_MAX 8192
#define TOKEN_MAX 160

static const char *TAG = "m2a";

static mrrc_cfg_t s_cfg;
static mrrc_state_t s_st;
static EventGroupHandle_t s_net_ev;
static char s_token[TOKEN_MAX];
static char s_auth_hdr[TOKEN_MAX + 32];
static esp_websocket_client_handle_t s_ctrl, s_spec;
static char s_acc[FRAME_MAX];
static size_t s_acc_len;

bool mrrc_link_up(void) { return s_st.up; }
const mrrc_state_t *mrrc_state(void) { return &s_st; }

/* ── /WSradio payload ────────────────────────────────────────────────────── */

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
        if (cJSON_IsString(it) || cJSON_IsNumber(it) || cJSON_IsBool(it)) {
            apply_field(it->string, it);
        }
    }
}

static void handle_ctrl_text(const char *payload, int len)
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
        const cJSON *data = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsObject(data)) {
            apply_object(data);
        } else {
            /* "value" carries field+value directly */
            const cJSON *f = cJSON_GetObjectItem(root, "field");
            const cJSON *v = cJSON_GetObjectItem(root, "value");
            if (cJSON_IsString(f) && v) apply_field(f->valuestring, v);
        }
        s_st.state_updates++;
    } else if (!strcmp(t, "error")) {
        const cJSON *m = cJSON_GetObjectItem(root, "message");
        ESP_LOGW(TAG, "server error: %s", cJSON_IsString(m) ? m->valuestring : "?");
    }
    cJSON_Delete(root);
}

/* ── websocket plumbing ──────────────────────────────────────────────────── */

static void spec_frame(const uint8_t *data, int len)
{
    if (len < 2) return;
    /* First byte is the frame kind (0x01 = waterfall row). Payload is 850
     * bins, optionally doubled when the server sends two rows in one frame. */
    s_st.spectrum_frames++;
    s_st.spectrum_bytes += (uint32_t)len;
    if ((s_st.spectrum_frames % 50) == 1) {
        uint32_t sum = 0;
        for (int i = 1; i < len; i++) sum += data[i];
        ESP_LOGI(TAG, "spectrum #%lu len=%d kind=0x%02x mean=%lu",
                 (unsigned long)s_st.spectrum_frames, len, data[0],
                 (unsigned long)(sum / (uint32_t)(len - 1)));
    }
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;
    const bool is_spec = (ev->client == s_spec);

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        if (is_spec) {
            ESP_LOGI(TAG, "/WSspectrum connected");
            /* Declaring "mid" halves the frame: 851 B instead of 1701 B. */
            static const char caps[] = "{\"type\":\"spectrumCaps\",\"profile\":\"mid\"}";
            esp_websocket_client_send_text(s_spec, caps, (int)strlen(caps), pdMS_TO_TICKS(1000));
        } else {
            ESP_LOGI(TAG, "/WSradio connected");
            s_st.up = true;
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        if (!is_spec) s_st.up = false;
        ESP_LOGW(TAG, "%s disconnected", is_spec ? "spec" : "radio");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (ev->op_code == 0x08) break;            /* close frame */
        if (is_spec) {
            spec_frame((const uint8_t *)ev->data_ptr, ev->data_len);
            break;
        }
        /* Text arrives fragmented on a 5.5 KB fullState: accumulate until the
         * final frame (payload_offset + data_len reaches payload_len). */
        if (ev->op_code == 0x01) {
            if (s_acc_len + ev->data_len < sizeof s_acc) {
                memcpy(s_acc + s_acc_len, ev->data_ptr, ev->data_len);
                s_acc_len += ev->data_len;
            }
            if (ev->payload_offset + ev->data_len >= ev->payload_len) {
                handle_ctrl_text(s_acc, (int)s_acc_len);
                s_acc_len = 0;
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "%s error", is_spec ? "spec" : "radio");
        break;
    }
}

static void ws_open(esp_websocket_client_handle_t *out, const char *path, bool spec)
{
    char uri[320];
    snprintf(uri, sizeof uri, "wss://%s:%d%s?token=%s", s_cfg.host, s_cfg.port, path, s_token);

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .headers = s_auth_hdr,
        .buffer_size = WS_BUF,
        .reconnect_timeout_ms = 3000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 15,
        .pingpong_timeout_sec = 25,
        .skip_cert_common_name_check = true,
    };
    *out = esp_websocket_client_init(&cfg);
    if (!*out) {
        ESP_LOGE(TAG, "ws init failed for %s", path);
        return;
    }
    esp_websocket_register_events(*out, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    ESP_LOGI(TAG, "%s -> wss://%s:%d%s", spec ? "spec" : "radio", s_cfg.host, s_cfg.port, path);
    esp_websocket_client_start(*out);
}

/* ── login ───────────────────────────────────────────────────────────────── */

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

/* ── Wi-Fi ───────────────────────────────────────────────────────────────── */

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

static void link_task(void *arg)
{
    (void)arg;
    if (!wifi_up()) {
        ESP_LOGE(TAG, "wifi failed; MRRC link not started");
        vTaskDelete(NULL);
    }
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
            ws_open(&s_ctrl, "/WSradio", false);
            ws_open(&s_spec, "/WSspectrum", true);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void mrrc_link_start(const mrrc_cfg_t *cfg)
{
    s_cfg = *cfg;
    memset(&s_st, 0, sizeof s_st);
    xTaskCreatePinnedToCore(link_task, "mrrc_link", 8192, NULL, 5, NULL, 1);
}
