// main/mrrc_audio.c — downlink audio: Opus decode -> 48k->16k -> I2S -> ES8311.
// Frames arrive from mrrc_net on the shared /WSterminal socket.
//
// Wire format (same as the xz-02 firmware, verified against server.py):
//     1 byte tag + payload,   0x00 = raw PCM, 0x01 = Opus @ 48 kHz mono
//
// Why the codec runs at 48 kHz and never changes: the ES8311 shares
// MCLK/BCLK/WS between playback and capture, so both directions must run at one
// rate.  The server's downlink is 48 kHz Opus, so 48 kHz it is; S3 will decimate
// the microphone to 16 kHz in software before encoding.
//
// The decoder is created lazily inside the audio task, which also owns the I2S
// writes.  micro-opus 0.4.1 allocates its pseudostack on first use (16 KB here,
// see sdkconfig), and keeping one task on both sides means a jitter-buffer
// stall can never race a decode.
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
#include "opus.h"

static const char *TAG = "audio";

#define AUD_WS_STACK   6144
#define AUD_TASK_STACK 12288   /* -O2 inlines deeply: 8 KB overflowed */
#define AUD_RB_BYTES   4096         /* ~0.8 s of 64 kbps Opus */
#define AUD_FRAME_MAX  576          /* 60 ms @ 48 kHz mono Opus is < 200 B */
#define AUD_PCM48_MAX  2880         /* 60 ms @ 48 kHz: what Opus decodes to */
#define AUD_PCM16_MAX   960         /* 60 ms @ 16 kHz: what the codec plays */

/* The codec runs at 16 kHz, not 48 kHz.  Measured on this board: the ES8311
 * ADC delivers ~468 peak at 48 kHz and ~12708 at 16 kHz for the same speech —
 * the microphone path is effectively dead at 48 kHz (ai-passport's own demo
 * only ever uses 16 kHz).  The downlink therefore decimates 3:1 after decode. */

static uint8_t  s_rb[AUD_RB_BYTES];
static size_t   s_r, s_w, s_used;
static SemaphoreHandle_t s_rb_mtx;
static OpusDecoder *s_dec;
TaskHandle_t s_audio_task_h;

static uint32_t s_frames, s_errors, s_drops;
static volatile bool s_muted;
static int s_feed_dbg;    /* first few frames: what do the bytes look like? */   /* downlink muted while transmitting */
static int64_t  s_dec_us;      /* accumulated opus_decode time */
static int16_t  s_pcm48[AUD_PCM48_MAX];
static int16_t  s_pcm16[AUD_PCM16_MAX];

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

        if (!s_dec) {
            /* micro-opus allocates its pseudostack lazily on the first decode
             * and does not check the result; a NULL there shows up as
             * "Store access fault, MTVAL=0" deep inside libopus.  Print the
             * state right before so the failure is attributable. */
            ESP_LOGI(TAG, "before decode: free=%uK largest=%uK",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
            int err = 0;
            s_dec = opus_decoder_create(48000, 1, &err);
            if (err != OPUS_OK || !s_dec) {
                ESP_LOGE(TAG, "decoder create failed: %d", err);
                s_dec = NULL;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(TAG, "opus decoder ready (48 kHz mono)");
        }

#if CONFIG_MRRC_PROFILE
        const int64_t t0 = esp_timer_get_time();
#endif
        const int got = opus_decode(s_dec, frame, (opus_int32)n, s_pcm48, AUD_PCM48_MAX, 0);
#if CONFIG_MRRC_PROFILE
        s_dec_us += esp_timer_get_time() - t0;
#endif
        if (got <= 0) {
            s_errors++;
            if ((s_errors & 0x7F) == 1) ESP_LOGW(TAG, "decode error %s", opus_strerror(got));
            continue;
        }
        s_frames++;
        /* Half duplex: drop playback while the transmitter is keyed.  Feeding
         * our own uplink back through the speaker is a howl, not audio. */
        if (s_muted || mrrc_tx_active()) continue;
        /* 48 kHz -> 16 kHz.  A 3-tap average is both the decimation filter
         * and a cheap anti-alias; the SSB passband is 3 kHz, so nothing above
         * 8 kHz matters. */
        int out = 0;
        for (int i = 0; i + 2 < got; i += 3) {
            const int32_t v = (s_pcm48[i] + s_pcm48[i + 1] + s_pcm48[i + 2]) / 3;
            s_pcm16[out++] = (int16_t)v;
        }
        if (out <= 0) continue;
        /* Blocking on the I2S DMA is what paces this loop; the codec's
         * auto_clear_after_cb fills silence when we are late. */
        if (bsp_audio_write(s_pcm16, (size_t)out * sizeof(int16_t)) != ESP_OK)
            s_errors++;
    }
}

int mrrc_audio_prewarm(void)
{
    int err = 0;

    if (!s_dec) {
        s_dec = opus_decoder_create(48000, 1, &err);
        if (err != OPUS_OK || !s_dec) {
            ESP_LOGE(TAG, "decoder create failed: %d", err);
            s_dec = NULL;
            return -1;
        }
        /* PLC path runs the full decode prologue, which is what actually
         * reserves the pseudostack.  Result ignored; reuse the decode buffer
         * rather than reserving a second 5.7 KB static array for one call. */
        const int warm = opus_decode(s_dec, NULL, 0, s_pcm48, AUD_PCM48_MAX, 0);
        (void)warm;
    }

    ESP_LOGI(TAG, "opus prewarm ok: free=%uK largest=%uK",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    return 0;
}

void mrrc_audio_feed(const uint8_t *data, size_t len)
{
    if (!data || len < 3) return;

    /* Two framings share this function:
     *
     *   /WSterminal  [0x00][codec][payload]   -- codec 0x01 = Opus
     *   /WSaudioRX   [codec][payload]
     *
     * Layer one is the transport marker; layer two is what the standalone
     * audio socket used alone.  Feeding the codec byte into libopus is what
     * produced "corrupted stream" on a third of the frames. */
    if (data[0] == 0x00 && (data[1] == 0x00 || data[1] == 0x01)) {
        if (data[1] == 0x01) rb_put(data + 2, len - 2);   /* Opus */
        /* codec 0x00 would be raw PCM; this profile never sends it. */
        return;
    }
    if (data[0] == 0x01) rb_put(data + 1, len - 1);
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
    if (dec_us) *dec_us = s_dec_us;
}
