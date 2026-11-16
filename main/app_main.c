// main/app_main.c — MRRC terminal on FoloToy AI Passport (ESP32-C3).
//
// Stage 0 (this file): measure, don't guess.
//
// The whole V1 design hangs on one unknown: how much internal heap is left
// once the display, the codec, Wi-Fi and one TLS session are all up.  The BSP
// documentation quotes "system heap ~130 KB" for LVGL with no network, and
// ESP32-C3 has 400 KB SRAM and no PSRAM — every later decision (LVGL or hand
// drawn, one WebSocket or two, 8 KB or 16 KB pseudostack) depends on the real
// number.  So this firmware brings each subsystem up in order, prints
// free/largest/min at every step, and mirrors the same lines on the panel.
//
// Nothing here transmits yet; the audio path is opened at the one rate the
// hardware allows (see Kconfig: capture and playback share MCLK/BCLK/WS).
#include <stdio.h>
#include <string.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "opus.h"

static const char *TAG = "mrrc";

#define PROBE_LINES 9

/* ── stage instrumentation ───────────────────────────────────────────── */

typedef struct {
    const char *stage;
    uint32_t free_internal;
    uint32_t largest;
    uint32_t minimum;
} mem_line_t;

static mem_line_t s_lines[PROBE_LINES];
static int s_line_count;
static lv_obj_t *s_label;
static char s_ip[32] = "-";
static int s_login_http = -999;

static void mem_stage(const char *stage)
{
    const uint32_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const uint32_t minimum = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    if (s_line_count < PROBE_LINES) {
        s_lines[s_line_count++] = (mem_line_t){ stage, free_b, largest, minimum };
    }
    /* Largest block is the number that actually decides whether a 19.2 KB LVGL
     * buffer or an 18 KB Opus decoder can be allocated at all; free heap alone
     * hides fragmentation. */
    ESP_LOGW(TAG, "HEAP %-20s free=%6u largest=%6u min=%6u",
             stage, (unsigned)free_b, (unsigned)largest, (unsigned)minimum);
}

static void ui_refresh(void)
{
    if (!s_label || !bsp_lvgl_lock(200)) return;

    char text[PROBE_LINES * 40];
    size_t off = 0;
    for (int i = 0; i < s_line_count && off < sizeof(text) - 48; i++) {
        off += (size_t)snprintf(text + off, sizeof(text) - off, "%s\n%u / %u\n",
                                s_lines[i].stage,
                                (unsigned)(s_lines[i].free_internal / 1024),
                                (unsigned)(s_lines[i].largest / 1024));
    }
    if (off == 0) snprintf(text, sizeof(text), "booting");
    lv_label_set_text(s_label, text);
    bsp_lvgl_unlock();
}

/* ── Wi-Fi ───────────────────────────────────────────────────────────── */

static EventGroupHandle_t s_net_events;
#define NET_CONNECTED BIT0
#define NET_FAILED    BIT1

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "wifi disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ip = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "ip " IPSTR, IP2STR(&ip->ip_info.ip));
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip->ip_info.ip));
        xEventGroupSetBits(s_net_events, NET_CONNECTED);
    }
}

static esp_err_t wifi_up(void)
{
    if (strlen(CONFIG_MRRC_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "no SSID configured: idf.py menuconfig -> MRRC Terminal");
        xEventGroupSetBits(s_net_events, NET_FAILED);
        return ESP_ERR_INVALID_STATE;
    }

    s_net_events = xEventGroupCreate();
    if (!s_net_events) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    wifi_config_t cfg = { 0 };
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", CONFIG_MRRC_WIFI_SSID);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", CONFIG_MRRC_WIFI_PASSWORD);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    const EventBits_t bits = xEventGroupWaitBits(s_net_events, NET_CONNECTED | NET_FAILED,
                                                pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    return (bits & NET_CONNECTED) ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* ── TLS probe: one POST /api/auth/login ─────────────────────────────── */

static esp_err_t tls_probe(void)
{
    if (strlen(CONFIG_MRRC_SERVER_HOST) == 0) {
        ESP_LOGW(TAG, "no server configured, skipping TLS probe");
        s_login_http = -1;
        return ESP_ERR_INVALID_STATE;
    }

    char url[192];
    snprintf(url, sizeof(url), "https://%s:%d/api/auth/login",
             CONFIG_MRRC_SERVER_HOST, CONFIG_MRRC_SERVER_PORT);
    char body[96];
    snprintf(body, sizeof(body), "{\"password\":\"%s\"}", CONFIG_MRRC_SERVER_PASSWORD);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        /* V1 probe only measures memory; certificate validation adds a bundle
         * copy we are not budgeting yet.  Production must set this up. */
        .skip_cert_common_name_check = true,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_ERR_NO_MEM;

    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, body, (int)strlen(body));
    const esp_err_t err = esp_http_client_perform(c);
    const int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    ESP_LOGI(TAG, "login http=%d err=%s", status, esp_err_to_name(err));
    s_login_http = (err == ESP_OK) ? status : -1;
    return err;
}

/* ── keys ────────────────────────────────────────────────────────────── */

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    /* Runs on the shared esp_timer task: log only, never block. */
    ESP_LOGI(TAG, "key btn=%d ev=%d mv=%d glitch=%u",
             (int)btn, (int)ev, bsp_button_read_mv(),
             (unsigned)bsp_button_glitch_count());
}

/* ── UI: one label, so the probe costs as little as possible ─────────── */

static void ui_build(void)
{
    if (!bsp_lvgl_lock(500)) return;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_label, "booting");
    lv_obj_center(s_label);

    lv_screen_load(scr);
    bsp_lvgl_unlock();
}

void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_LOGI(TAG, "reset reason %d", (int)esp_reset_reason());
    mem_stage("boot");

    ESP_ERROR_CHECK(bsp_i2c_init());
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(70);
    mem_stage("display+lvgl");

    ui_build();

    ESP_ERROR_CHECK(bsp_button_init(on_key, NULL));
    mem_stage("buttons");

    if (bsp_audio_init() == ESP_OK) {
        mem_stage("audio init");
        if (bsp_audio_set_format(CONFIG_MRRC_PROBE_AUDIO_HZ, 16, 1) == ESP_OK) {
            bsp_audio_set_volume(60);
            mem_stage("codec open");
        } else {
            ESP_LOGE(TAG, "codec open failed at %d Hz", CONFIG_MRRC_PROBE_AUDIO_HZ);
        }
    } else {
        ESP_LOGE(TAG, "audio init failed");
    }

    if (bsp_battery_init() == ESP_OK) {
        ESP_LOGI(TAG, "battery %d mV %d%%", bsp_battery_mv(), bsp_battery_soc());
        mem_stage("battery");
    }

    ui_refresh();

    if (wifi_up() == ESP_OK) {
        mem_stage("wifi up");
        ui_refresh();
        tls_probe();
        mem_stage("after tls");
        ui_refresh();
    }

    /* Opus decoder is the last fixed cost: 18 KB of state plus its pseudostack,
     * allocated lazily on the first decode.  Allocating it here (not on the
     * first received frame) keeps the number comparable across builds. */
    int opus_err = 0;
    OpusDecoder *dec = opus_decoder_create(48000, 1, &opus_err);
    if (opus_err == OPUS_OK && dec) {
        mem_stage("opus decoder");
    } else {
        ESP_LOGE(TAG, "opus decoder create failed: %d", opus_err);
    }
    ui_refresh();

    const uint32_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "PROBE DONE free=%u KB largest=%u KB",
             (unsigned)(free_now / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        /* Replay every stage: the first two seconds after a reset are lost to
         * the USB-Serial-JTAG re-enumeration, so the boot-time lines can only
         * be read reliably if they are kept around. */
        for (int i = 0; i < s_line_count; i++) {
            ESP_LOGI(TAG, "HIST %-16s free=%5uK largest=%5uK",
                     s_lines[i].stage,
                     (unsigned)(s_lines[i].free_internal / 1024),
                     (unsigned)(s_lines[i].largest / 1024));
        }
        ESP_LOGI(TAG, "idle ip=%s login_http=%d free=%u largest=%u glitch=%u",
                 s_ip, s_login_http,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)bsp_button_glitch_count());
        /* Keep retrying the login: the first attempt usually races the router's
         * DHCP/ARP setup, and the answer is the only proof that TLS terminates. */
        if (s_net_events && strlen(CONFIG_MRRC_SERVER_HOST) > 0) {
            tls_probe();
            mem_stage("tls retry");
            ui_refresh();
        }
    }
}
