// main/mrrc_tx.c — PTT uplink: microphone PCM into the shared socket.
//
// Flow (mirrors the xz-02 safety chain, see docs/design-v1.md §7.3):
//
//   OK long-press   -> mrrc_net_set_ptt(true)      -> ptt:true
//                   -> stream  tag 0x00 + 48 kHz int16 mono
//   every 1.5 s     -> txhb   (server force-releases after 5 s without it)
//   release         -> stop streaming, "s:", ptt:false
//   180 s held      -> force release locally
//
// The uplink uses its own /WSaudioTX socket.  Control (ptt/txhb) still goes out
// on /WSterminal; only the audio needs the dedicated endpoint, because
// /WSterminal's binary uplink is not wired to the server's TX audio path
// (verified: ptt flips tx_status to 1 while frames produce no tx_mic_db).
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bsp_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_websocket_client.h"
#include "mrrc_net.h"
#include "mrrc_tx.h"

static const char *TAG = "tx";

#define TX_TASK_STACK   4096
#define TX_MIC_FRAME    320         /* 20 ms @ 16 kHz, what the codec captures */
#define TX_FRAME        960         /* 20 ms @ 48 kHz: same span after 3x upsample */
#define TX_MAX_HOLD_MS  180000
#define TX_HB_MS        1500

static esp_websocket_client_handle_t s_ws;
static volatile bool s_want_tx;
static volatile bool s_link_up;
static TaskHandle_t s_task;

static char s_last_evt[80] = "none";
static portMUX_TYPE s_note_mux = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_keys, s_fails, s_tx_frames, s_send_fail, s_read_fail;
static int s_peak_max;
static int s_peak_avg_sum, s_peak_avg_n;

static char s_evt_buf[80];

/* Called from the key task and the transmit task; the shared line is guarded so
 * a -O2 build cannot interleave two vsnprintf() calls into one buffer. */
static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_evt_buf, sizeof s_evt_buf, fmt, ap);
    va_end(ap);
    taskENTER_CRITICAL(&s_note_mux);
    memcpy(s_last_evt, s_evt_buf, sizeof s_last_evt);
    taskEXIT_CRITICAL(&s_note_mux);
    ESP_LOGI(TAG, "%s", s_evt_buf);
}

static void __attribute__((unused)) ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
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
        break;
    default:
        break;
    }
}

static void __attribute__((unused)) close_socket(void)
{
    if (!s_ws) return;
    esp_websocket_client_stop(s_ws);
    vTaskDelay(pdMS_TO_TICKS(100));   /* let queued events drain */
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    s_link_up = false;
}

bool mrrc_tx_open(void)
{
    return true;   /* single socket: the uplink is always connected */
}

static bool __attribute__((unused)) tx_open_unused(void)
{
    if (s_ws) return s_link_up;
    char uri[320];
    snprintf(uri, sizeof uri, "wss://%s:%d/WSaudioTX?token=%s",
             CONFIG_MRRC_SERVER_HOST, CONFIG_MRRC_SERVER_PORT, mrrc_token());
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .headers = mrrc_auth_header(),
        .buffer_size = 4096,        /* one frame is 1 + 1920 B */
        .task_stack = 4096,
        .network_timeout_ms = 8000,
        .ping_interval_sec = 15,
        .pingpong_timeout_sec = 25,
        .skip_cert_common_name_check = true,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) { ESP_LOGE(TAG, "ws init failed"); return false; }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_websocket_client_start(s_ws);
    for (int i = 0; i < 60 && !s_link_up; i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (!s_link_up) { note("uplink never connected"); close_socket(); return false; }
    return true;
}

static void tx_task(void *arg)
{
    (void)arg;
    /* One buffer for the whole path: byte 0 is the codec tag, samples 1..960
     * hold the 48 kHz stream.  The 320 captured samples are expanded to 960
     * from the back so they are never overwritten. */
    static int16_t buf[1 + TX_FRAME];
    uint8_t *tag = (uint8_t *)buf;
    int16_t *pcm = buf + 1;
    *tag = 0x01;

    int64_t last_hb = 0;
    const int64_t started = esp_timer_get_time();

    while (s_want_tx) {
        const int64_t now = esp_timer_get_time();

        /* Keep the server's TX liveness gate fed. */
        if (now - last_hb > TX_HB_MS * 1000LL) {
            last_hb = now;
            mrrc_net_send_txhb();
        }
        if (now - started > TX_MAX_HOLD_MS * 1000LL) {
            ESP_LOGW(TAG, "TX timeout (180 s), forcing release");
            break;
        }

        if (bsp_audio_read(pcm, TX_MIC_FRAME * sizeof(int16_t)) != ESP_OK) {
            s_read_fail++;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int pk = 0;
        long sum = 0;
        for (int i = 0; i < TX_MIC_FRAME; i++) {
            const int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
            if (a > pk) pk = a;
            sum += a;
        }
        if (pk > s_peak_max) s_peak_max = pk;
        s_peak_avg_sum += (int)(sum / TX_MIC_FRAME);
        s_peak_avg_n++;

        /* 16 kHz -> 48 kHz by zero-order hold, expanded from the back. */
        for (int i = TX_MIC_FRAME - 1; i >= 0; i--) {
            const int16_t v = pcm[i];
            pcm[i * 3] = pcm[i * 3 + 1] = pcm[i * 3 + 2] = v;
        }
        /* /WSterminal uplink: single layer [0x01][PCM]. */
        *tag = 0x01;

        if (mrrc_net_send_bin(buf, 1 + TX_FRAME * sizeof(int16_t))) {
            s_tx_frames++;
        } else {
            s_send_fail++;
            if ((s_send_fail & 0x0F) == 1)
                ESP_LOGW(TAG, "send failed (%u)", s_send_fail);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /* "s:" tells the server capture stopped; the PTT release follows from the
     * caller so the order is stop-stream -> unkey. */
    mrrc_net_send_text_stop();
    note("tx done: frames=%u sendfail=%u readfail=%u peak=%d avg=%d",
         s_tx_frames, s_send_fail, s_read_fail, s_peak_max,
         s_peak_avg_n ? s_peak_avg_sum / s_peak_avg_n : 0);

    s_want_tx = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

bool mrrc_tx_key(bool on)
{
    if (on) {
        if (s_want_tx) return true;
        s_keys++;
        s_tx_frames = 0;
        s_send_fail = 0;
        s_read_fail = 0;
        s_peak_max = 0;
        s_peak_avg_sum = 0;
        s_peak_avg_n = 0;
        s_want_tx = true;
        if (xTaskCreate(tx_task, "txtask", TX_TASK_STACK, NULL, 7, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "tx task failed");
            s_fails++;
            s_want_tx = false;
            return false;
        }
        note("keyed free=%uK largest=%uK",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        return true;
    }

    if (!s_want_tx) return true;
    s_want_tx = false;
    for (int i = 0; i < 30 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(50));
    note("unkeyed peak=%d frames=%u free=%uK",
         s_peak_max, s_tx_frames,
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
    if (link_downs)  *link_downs = 0;   /* single socket: no per-key link */
}
