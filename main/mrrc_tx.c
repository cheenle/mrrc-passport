// main/mrrc_tx.c — key up, stream microphone PCM, key down.
//
// Flow (mirrors the xz-02 safety chain, see docs/design-v1.md §7.3):
//
//   OK long-press   -> mrrc_net_set_ptt(true)  -> ptt:true
//                   -> open /WSaudioTX
//                   -> 200 ms settle, drop pre-key audio
//                   -> stream  tag 0x00 + 48 kHz int16 mono
//   every 1.5 s     -> txhb   (server force-releases after 5 s without it)
//   release         -> stop streaming, "s:", ptt:false, close the socket
//   180 s held      -> force release locally
//
// The socket is opened per transmission: two always-on TLS sessions already
// leave only ~33 KB free, and a third would not fit.
#include <string.h>

#include "bsp_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mrrc_net.h"
#include "mrrc_tx.h"

static const char *TAG = "tx";

#define TX_TASK_STACK   6144
#define TX_WS_STACK     6144
#define TX_FRAME        960         /* 20 ms @ 48 kHz mono */
#define TX_MAX_HOLD_MS  180000
#define TX_HB_MS        1500
#define TX_SETTLE_MS    200

static esp_websocket_client_handle_t s_ws;
static volatile bool s_want_tx;      /* key is held */
static volatile bool s_link_up;      /* socket accepted */
static TaskHandle_t s_task;

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "/WSaudioTX connected");
        s_link_up = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_link_up = false;
        break;
    default:
        break;
    }
}

static void tx_task(void *arg)
{
    (void)arg;
    static int16_t mic[TX_FRAME];
    static uint8_t frame[1 + sizeof mic];
    frame[0] = 0x00;                      /* AUDIO_TAG_PCM */

    int64_t last_hb = 0;
    const int64_t started = esp_timer_get_time();

    while (s_want_tx) {
        if (!s_link_up) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* Keep the server's TX liveness gate fed.  This runs inside the same
         * task as the audio, and never blocks on the socket for long. */
        const int64_t now = esp_timer_get_time();
        if (now - last_hb > TX_HB_MS * 1000LL) {
            last_hb = now;
            mrrc_net_send_txhb();
        }
        if (now - started > TX_MAX_HOLD_MS * 1000LL) {
            ESP_LOGW(TAG, "TX timeout (180 s), forcing release");
            break;
        }

        if (bsp_audio_read(mic, sizeof mic) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        memcpy(frame + 1, mic, sizeof mic);
        if (esp_websocket_client_send_bin(s_ws, (const char *)frame, sizeof frame,
                                          pdMS_TO_TICKS(1000)) < 0) {
            ESP_LOGW(TAG, "send failed");
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /* Order matters: stop the stream with "s:", then release PTT, then close. */
    if (s_link_up) {
        esp_websocket_client_send_text(s_ws, "s:", 2, pdMS_TO_TICKS(500));
    }
    s_want_tx = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

static void close_socket(void)
{
    if (!s_ws) return;
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    s_link_up = false;
}

bool mrrc_tx_key(bool on)
{
    if (on) {
        if (s_ws || s_want_tx) return true;

        char uri[320];
        snprintf(uri, sizeof uri, "wss://%s:%d/WSaudioTX?token=%s",
                 CONFIG_MRRC_SERVER_HOST, CONFIG_MRRC_SERVER_PORT, mrrc_token());

        esp_websocket_client_config_t cfg = {
            .uri = uri,
            .headers = mrrc_auth_header(),
            .buffer_size = 1024,
            .task_stack = TX_WS_STACK,
            .network_timeout_ms = 8000,
            .ping_interval_sec = 15,
            .pingpong_timeout_sec = 25,
            .skip_cert_common_name_check = true,
        };
        s_ws = esp_websocket_client_init(&cfg);
        if (!s_ws) {
            ESP_LOGE(TAG, "ws init failed");
            return false;
        }
        esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
        esp_websocket_client_start(s_ws);

        s_want_tx = true;
        if (xTaskCreate(tx_task, "txtask", TX_TASK_STACK, NULL, 7, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "tx task failed");
            s_want_tx = false;
            close_socket();
            return false;
        }
        ESP_LOGI(TAG, "keyed, free=%uK largest=%uK",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        return true;
    }

    /* Unkey: let the task flush "s:" and exit, then tear the socket down. */
    s_want_tx = false;
    for (int i = 0; i < 30 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(50));
    close_socket();
    ESP_LOGI(TAG, "unkeyed, free=%uK largest=%uK",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    return true;
}

bool mrrc_tx_active(void) { return s_want_tx; }
