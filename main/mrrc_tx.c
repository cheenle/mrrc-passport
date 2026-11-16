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
// The uplink rides on the same /WSterminal socket as control and downlink, so
// there is no socket to open per transmission and no TLS handshake on key-up.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bsp_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mrrc_net.h"
#include "mrrc_tx.h"

static const char *TAG = "tx";

#define TX_TASK_STACK   4096
#define TX_MIC_FRAME    320         /* 20 ms @ 16 kHz, what the codec captures */
#define TX_FRAME        960         /* 20 ms @ 48 kHz: same span after 3x upsample */
#define TX_MAX_HOLD_MS  180000
#define TX_HB_MS        1500

static volatile bool s_want_tx;
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

static void tx_task(void *arg)
{
    (void)arg;
    static int16_t mic[TX_MIC_FRAME];
    static int16_t up[TX_FRAME];
    static uint8_t frame[1 + sizeof up];
    frame[0] = 0x00;                      /* PCM tag */

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

        if (bsp_audio_read(mic, sizeof mic) != ESP_OK) {
            s_read_fail++;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

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

        /* 16 kHz -> 48 kHz by zero-order hold; the server resamples to 44.1 kHz
         * anyway and the passband is 3 kHz. */
        for (int i = 0; i < TX_MIC_FRAME; i++)
            up[i * 3] = up[i * 3 + 1] = up[i * 3 + 2] = mic[i];
        memcpy(frame + 1, up, sizeof up);

        if (mrrc_net_send_bin(frame, sizeof frame)) {
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
