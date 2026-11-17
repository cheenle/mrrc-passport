// main/mrrc_audio.c — downlink audio: 16 kHz Int16 PCM -> I2S -> ES8311.
// Frames arrive from mrrc_net on the shared /WSterminal socket.
//
// Wire format (same as the xz-02 firmware, verified against server.py):
//     1 byte marker + 1 byte codec + payload,   0x00 = PCM @ 16 kHz mono
//
// Why the codec runs at 48 kHz and never changes: the ES8311 shares
// MCLK/BCLK/WS between playback and capture, so both directions must run at one
// rate.  The server's downlink is 48 kHz Opus, so 48 kHz it is; S3 will decimate
// the microphone to 16 kHz in software before encoding.
//
// Audio is plain Int16 PCM at 16 kHz in both directions: no codec is linked in.
// The ESP32-C3 has no headroom to spare for Opus, and the wire format already
// carries a PCM tag, so the samples go to the codec unchanged.
#include <string.h>

#include "bsp_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "mrrc_audio.h"
#include "mrrc_net.h"
#include "mrrc_tx.h"

static const char *TAG = "audio";

#define AUD_WS_STACK   6144
#define AUD_TASK_STACK 8192
/* PCM is uncompressed: 16 kHz mono is 32 kB/s, so the ring holds ~0.26 s and a
 * frame slot must fit the largest packet the server sends.  The server
 * coalesces downlink in 20 ms units (640 B), but bursts are possible, so allow
 * 60 ms — the same span the Opus path used to cover. */
#define AUD_RB_BYTES   8192         /* ~0.26 s of 16 kHz PCM */
#define AUD_FRAME_MAX  1920         /* 60 ms @ 16 kHz mono */
/* The codec runs at 16 kHz, not 48 kHz.  Measured on this board: the ES8311
 * ADC delivers ~468 peak at 48 kHz and ~12708 at 16 kHz for the same speech —
 * the microphone path is effectively dead at 48 kHz (ai-passport's own demo
 * only ever uses 16 kHz).  Everything downstream is 16 kHz as a result. */

static uint8_t  s_rb[AUD_RB_BYTES];
static size_t   s_r, s_w, s_used;
static SemaphoreHandle_t s_rb_mtx;
TaskHandle_t s_audio_task_h;

static uint32_t s_frames, s_errors, s_drops;
static volatile bool s_muted;
static int s_feed_dbg;    /* first few frames: what do the bytes look like? */   /* downlink muted while transmitting */

/* ── jitter buffer ───────────────────────────────────────────────────── */

/* Frames are length-prefixed with a 2-byte big-endian size so the consumer
 * never splits a packet.  A full buffer drops the newest frame: losing the
 * newest packet is cheaper than tearing the stream. */
static void rb_put(const uint8_t *d, size_t n)
{
    if (n == 0 || n > AUD_FRAME_MAX) return;
    if (xSemaphoreTake(s_rb_mtx, 0) != pdTRUE) { s_drops++; return; }
    if (AUD_RB_BYTES - s_used < n + 2) {
        s_drops++;
        xSemaphoreGive(s_rb_mtx);
        return;
    }
    const uint8_t hdr[2] = { (uint8_t)(n >> 8), (uint8_t)(n & 0xFF) };
    for (int i = 0; i < 2; i++) { s_rb[s_w] = hdr[i]; s_w = (s_w + 1) % AUD_RB_BYTES; }
    for (size_t i = 0; i < n; i++) { s_rb[s_w] = d[i]; s_w = (s_w + 1) % AUD_RB_BYTES; }
    s_used += n + 2;
    xSemaphoreGive(s_rb_mtx);
}

static size_t rb_get(uint8_t *out, size_t max, TickType_t wait)
{
    for (;;) {
        if (xSemaphoreTake(s_rb_mtx, wait) != pdTRUE) return 0;
        if (s_used >= 2) {
            const size_t n = (size_t)((s_rb[s_r] << 8) | s_rb[(s_r + 1) % AUD_RB_BYTES]);
            if (n <= max && s_used >= n + 2) {
                s_r = (s_r + 2) % AUD_RB_BYTES;
                for (size_t i = 0; i < n; i++) {
                    out[i] = s_rb[s_r];
                    s_r = (s_r + 1) % AUD_RB_BYTES;
                }
                s_used -= n + 2;
                xSemaphoreGive(s_rb_mtx);
                return n;
            }
        }
        xSemaphoreGive(s_rb_mtx);
        return 0;
    }
}

/* ── decode + play ───────────────────────────────────────────────────── */

static void audio_task(void *arg)
{
    (void)arg;
    static uint8_t frame[AUD_FRAME_MAX];

    for (;;) {
        const size_t n = rb_get(frame, sizeof frame, pdMS_TO_TICKS(100));
        if (n == 0) {
            /* rb_get() returns immediately when the buffer is empty; without
             * this delay the loop spins at full speed and starves IDLE, which
             * trips the task watchdog (5 s) on a single-core C3. */
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        /* The uplink is plain 16 kHz Int16 PCM — no codec, no rate change.
         * The server downsamples for us, so the captured buffer goes to I2S
         * unchanged. */
        s_frames++;
        /* Half duplex: drop playback while the transmitter is keyed.  Feeding
         * our own uplink back through the speaker is a howl, not audio. */
        if (s_muted || mrrc_tx_active()) continue;

        const size_t samples = (n / sizeof(int16_t));
        if (samples == 0) continue;
        /* The ring buffer hands back whole bytes; a stray odd byte would
         * misalign every following sample, so drop it. */
        if (bsp_audio_write((const int16_t *)frame, samples * sizeof(int16_t)) != ESP_OK)
            s_errors++;
    }
}

int mrrc_audio_prewarm(void)
{
    /* Nothing to warm: with libopus gone there is no lazily-allocated
     * pseudostack to reserve.  Kept as a no-op so startup order is unchanged. */
    ESP_LOGI(TAG, "audio ready (PCM 16 kHz, no codec) free=%uK largest=%uK",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    return 0;
}

void mrrc_audio_feed(const uint8_t *data, size_t len)
{
    if (!data || len < 3) return;

    /* Two framings share this function:
     *
     *   /WSterminal  [0x00][codec][payload]
     *
     * Layer one is the transport marker, layer two the codec tag.  Only PCM is
     * accepted now; an Opus frame is dropped because there is no decoder. */
    if (data[0] == 0x00 && (data[1] == 0x00 || data[1] == 0x01)) {
        /* transport marker + codec byte.  0x00 = PCM (the only format this
         * build can play); 0x01 = Opus, which is no longer linked in. */
        if (data[1] == 0x00) rb_put(data + 2, len - 2);
        return;
    }
    if (data[0] == 0x00) rb_put(data + 1, len - 1);
}

void mrrc_audio_suspend(void)
{
    /* Single connection: no socket to free.  Stop playing so the uplink does
     * not howl, and drop whatever is already queued. */
    s_muted = true;
    if (s_rb_mtx && xSemaphoreTake(s_rb_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_r = s_w = s_used = 0;
        xSemaphoreGive(s_rb_mtx);
    }
}

void mrrc_audio_resume(void) { s_muted = false; }

void mrrc_audio_start(void)
{
    s_rb_mtx = xSemaphoreCreateMutex();
    if (!s_rb_mtx) {
        ESP_LOGE(TAG, "no mutex");
        return;
    }
    if (xTaskCreate(audio_task, "audio", AUD_TASK_STACK, NULL, 8, &s_audio_task_h) != pdPASS) {
        ESP_LOGE(TAG, "audio task failed");
        return;
    }
}

void mrrc_audio_stats(uint32_t *frames, uint32_t *errors, uint32_t *drops)
{
    if (frames) *frames = s_frames;
    if (errors) *errors = s_errors;
    if (drops)  *drops  = s_drops;
}

void mrrc_audio_profile(uint32_t *frames, int64_t *dec_us)
{
    if (frames) *frames = s_frames;
    if (dec_us) *dec_us = 0;      /* no decoder to time any more */
}
