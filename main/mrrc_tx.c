// main/mrrc_tx.c — PTT uplink: microphone PCM into the shared /WSterminal socket.
//
//   OK long-press  -> ptt:true, stream [0x01][48 kHz PCM]
//   every 1.5 s    -> txhb  (server force-releases after 5 s without it)
//   release        -> "s:", ptt:false
//   180 s held     -> force release locally
//
// Everything rides the one Control+downlink+uplink socket, so keying costs no
// TLS handshake and there is no ownership ordering to respect.
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
#define TX_MIC_FRAME    320         /* 20 ms @ 16 kHz captured by the codec */
#define TX_FRAME        320         /* 20 ms @ 16 kHz — sent as-is, no upsampling */
/* 0x02 = raw Int16 PCM captured at 16 kHz.  A distinct tag (not 0x00, which
 * means 48 kHz) so the server knows to resample; the browser's own uplink is
 * untouched. */
#define AUDIO_TAG_PCM_16K  0x02
#define TX_MAX_HOLD_MS  180000
#define TX_HB_MS        1500

static volatile bool s_want_tx;
static TaskHandle_t s_task;

static char s_last_evt[80] = "none";
static portMUX_TYPE s_note_mux = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_keys, s_fails, s_tx_frames, s_send_fail, s_read_fail;
static int s_peak_max;
static int s_peak_avg_sum, s_peak_avg_n;

/* Written from the key task and the transmit task; the copy into the shared
 * line is inside a critical section so -O2 cannot interleave two vsnprintf. */
static void note(const char *fmt, ...)
{
    char tmp[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    taskENTER_CRITICAL(&s_note_mux);
    memcpy(s_last_evt, tmp, sizeof s_last_evt);
    taskEXIT_CRITICAL(&s_note_mux);
    ESP_LOGI(TAG, "%s", tmp);
}

static void tx_task(void *arg)
{
    (void)arg;
    /* One buffer: byte 0 is the codec tag, the rest is 20 ms of 16 kHz PCM.
     *
     * The tag used to be 0x01 (Opus) while the payload was already raw PCM —
     * the server tried to Opus-decode uncompressed samples.  With libopus gone
     * from this build the payload is native 16 kHz PCM tagged 0x00, and the
     * 3x upsampling to 48 kHz is unnecessary: the server resamples.
     */
    static int16_t buf[1 + TX_FRAME];
    uint8_t *tag = (uint8_t *)buf;
    int16_t *pcm = buf + 1;
    *tag = AUDIO_TAG_PCM_16K;

    int64_t last_hb = 0;
    const int64_t started = esp_timer_get_time();

    while (s_want_tx) {
        const int64_t now = esp_timer_get_time();
        if (now - last_hb > TX_HB_MS * 1000LL) {
            last_hb = now;
            mrrc_net_send_txhb();
        }
        if (now - started > TX_MAX_HOLD_MS * 1000LL) {
            ESP_LOGW(TAG, "TX timeout (180 s)");
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

        /* No rate conversion: the captured 16 kHz samples go out unchanged. */
        (void)0;

        if (mrrc_net_send_bin(buf, 1 + TX_FRAME * sizeof(int16_t))) {
            s_tx_frames++;
        } else {
            s_send_fail++;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    mrrc_net_send_text_stop();
    note("tx done: frames=%u sendfail=%u readfail=%u peak=%d",
         s_tx_frames, s_send_fail, s_read_fail, s_peak_max);

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
    note("unkeyed peak=%d frames=%u", s_peak_max, s_tx_frames);
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
    if (link_downs)  *link_downs = 0;
}
