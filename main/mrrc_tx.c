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
#include <stdarg.h>
#include <stdio.h>
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

#define TX_TASK_STACK   3072
#define TX_WS_STACK     4096
#define TX_MIC_FRAME    320         /* 20 ms @ 16 kHz, what the codec captures */
#define TX_FRAME        960         /* 20 ms @ 48 kHz: same span after 3x upsample */
#define TX_MAX_HOLD_MS  180000
#define TX_HB_MS        1500
#define TX_SETTLE_MS    200

static esp_websocket_client_handle_t s_ws;
static volatile bool s_want_tx;      /* key is held */
static volatile bool s_link_up;      /* socket accepted */
static TaskHandle_t s_task;
static char s_last_evt[72] = "none";
static unsigned s_keys, s_fails, s_tx_frames, s_send_fail, s_read_fail, s_link_downs;
static int s_peak_max;      /* loudest sample seen in the last keying */
static int s_peak_avg_sum, s_peak_avg_n;

static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_evt, sizeof s_last_evt, fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", s_last_evt);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "/WSaudioTX connected");
        s_link_up = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "/WSaudioTX disconnected");
        s_link_up = false;
        s_link_downs++;
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "/WSaudioTX error");
        break;
    default:
        break;
    }
}

static void tx_task(void *arg)
{
    (void)arg;
    static int16_t mic[TX_MIC_FRAME];
    static int16_t up[TX_FRAME];
    static uint8_t frame[1 + sizeof up];
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
            s_read_fail++;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        /* Prove the microphone actually carries signal: a silent codec path
         * (wrong ADC ref, RX channel disabled, 0 dB wrong gain) reads fine but
         * delivers zeros, and then "no audio" is a device bug, not a server
         * one. */
        {
            int pk = 0;
            long sum = 0;
            for (int i = 0; i < TX_MIC_FRAME; i++) {
                const int a = mic[i] < 0 ? -mic[i] : mic[i];
                if (a > pk) pk = a;
                sum += a;
            }
            if (pk > s_peak_max) s_peak_max = pk;
            s_peak_avg_sum += (int)(sum / TX_MIC_FRAME);
            s_peak_avg_n++;

        }
        /* 16 kHz -> 48 kHz by zero-order hold.  The server resamples 48->44.1
         * anyway and the passband is 3 kHz, so the images this leaves far
         * above the voice band are harmless. */
        for (int i = 0; i < TX_MIC_FRAME; i++) {
            up[i * 3] = up[i * 3 + 1] = up[i * 3 + 2] = mic[i];
        }
        memcpy(frame + 1, up, sizeof up);
        const int sent = esp_websocket_client_send_bin(
            s_ws, (const char *)frame, sizeof frame, pdMS_TO_TICKS(1000));
        if (sent >= 0) s_tx_frames++;
        if (sent < 0) {
            s_send_fail++;
            if ((s_send_fail & 0x0F) == 1)
                ESP_LOGW(TAG, "send failed (%u)", s_send_fail);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /* Order matters: stop the stream with "s:", then release PTT, then close. */
    if (s_link_up) {
        esp_websocket_client_send_text(s_ws, "s:", 2, pdMS_TO_TICKS(500));
    }
    note("tx done: frames=%u sendfail=%u readfail=%u downs=%u peak=%d avg=%d",
         s_tx_frames, s_send_fail, s_read_fail, s_link_downs, s_peak_max,
         s_peak_avg_n ? s_peak_avg_sum / s_peak_avg_n : 0);
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
            /* Must be >= one frame: 960 samples * 2 B + tag = 1921 B.  A
             * smaller buffer makes every send_bin() fail while the socket
             * stays connected (sendfail climbs, frames stays ~0). */
            .buffer_size = 4096,
            .task_stack = TX_WS_STACK,
            .network_timeout_ms = 8000,
            .ping_interval_sec = 15,
            .pingpong_timeout_sec = 25,
            .skip_cert_common_name_check = true,
        };
        s_ws = esp_websocket_client_init(&cfg);
        if (!s_ws) return false;
        esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
        esp_websocket_client_start(s_ws);

        /* Block until the uplink socket is actually accepted, BEFORE the caller
         * sends ptt:true.  server.py claims the TX owner by scanning
         * audio_tx_clients for this token; if we key first, that scan finds
         * nothing, the browser keeps ownership, and every frame we send is
         * dropped as non-owner — radio keys, no modulation, no sound. */
        for (int i = 0; i < 60 && !s_link_up; i++) vTaskDelay(pdMS_TO_TICKS(50));
        if (!s_link_up) {
            note("uplink never connected");
            close_socket();
            return false;
        }
        note("uplink ready free=%uK largest=%uK",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));

        s_want_tx = true;
        if (xTaskCreate(tx_task, "txtask", TX_TASK_STACK, NULL, 7, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "tx task failed");
            s_fails++;
            s_want_tx = false;
            close_socket();
            return false;
        }
        s_keys++;
        s_tx_frames = 0;
        s_peak_max = 0;
        s_peak_avg_sum = 0;
        s_peak_avg_n = 0;
        note("keyed: free=%uK largest=%uK",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        return true;
    }

    /* Unkey: let the task flush "s:" and exit, then tear the socket down. */
    s_want_tx = false;
    for (int i = 0; i < 30 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(50));
    close_socket();
    note("unkeyed: peak=%d avg=%d frames=%u free=%uK",
         s_peak_max, s_peak_avg_n ? s_peak_avg_sum / s_peak_avg_n : 0,
         s_tx_frames,
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return true;
}

bool mrrc_tx_active(void) { return s_want_tx; }

const char *mrrc_tx_last_event(void) { return s_last_evt; }

void mrrc_tx_stats(unsigned *keys, unsigned *fails, unsigned *frames,
                   unsigned *send_fail, unsigned *read_fail, unsigned *link_downs)
{
    if (keys)        *keys = s_keys;
    if (fails)       *fails = s_fails;
    if (frames)      *frames = s_tx_frames;
    if (send_fail)   *send_fail = s_send_fail;
    if (read_fail)   *read_fail = s_read_fail;
    if (link_downs)  *link_downs = s_link_downs;
}
