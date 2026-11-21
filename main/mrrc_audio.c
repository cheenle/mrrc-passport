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
/* PCM is uncompressed: 16 kHz mono is 32 kB/s, so the ring holds ~0.5 s and a
 * frame slot must fit the largest packet the server sends.  The server
 * coalesces downlink in 20 ms units (640 B), but bursts are possible, so allow
 * 60 ms — the same span the Opus path used to cover.
 *
 * 16 KB, not 8 KB.  Measured against the live server (2026-11-20): the first
 * seconds of a session arrive at 92 frames/s against a 50/s nominal rate, i.e.
 * TCP delivers a backlog in one burst, and 0.26 s of ring was not deep enough
 * to absorb it — 253 frames were dropped over six minutes and the delivered
 * rate settled at 41/s, heard as steady stutter.  Doubling the ring costs 8 KB
 * of .bss and buys 0.5 s of slack; `largest` was 46 KB before the change, so
 * the PTT handshake (16 KB contiguous) still has room. */
#define AUD_RB_BYTES   16384        /* ~0.51 s of 16 kHz PCM */
#define AUD_FRAME_MAX  1920         /* 60 ms @ 16 kHz mono */
/* Playback used to start on the very first frame, so the ring sat near empty
 * and every network gap became an audible dropout (measured: 3-5 ring-empty
 * wakeups per second).  Hold playback until this much audio is queued, and
 * rebuild the cushion if the ring stays dry — 160 ms at 32 KB/s. */
#define AUD_PREBUF_BYTES 5120       /* ~160 ms */
#define AUD_REBUFFER_US  200000LL   /* dry for 200 ms -> buffer again */
/* The codec runs at 16 kHz, not 48 kHz.  Measured on this board: the ES8311
 * ADC delivers ~468 peak at 48 kHz and ~12708 at 16 kHz for the same speech —
 * the microphone path is effectively dead at 48 kHz (ai-passport's own demo
 * only ever uses 16 kHz).  Everything downstream is 16 kHz as a result. */

static uint8_t  s_rb[AUD_RB_BYTES];
static size_t   s_r, s_w, s_used;
static SemaphoreHandle_t s_rb_mtx;
TaskHandle_t s_audio_task_h;

static uint32_t s_frames, s_errors, s_drops;
/* Root-cause counters for the stutter investigation (CONFIG_MRRC_PROFILE only).
 *
 *   rx_bytes   payload bytes the socket actually delivered — compare with the
 *              31.3 KB/s a healthy link carries; if it matches, frames are only
 *              being coalesced into fewer rb_put() calls.
 *   underruns  times the audio task woke to an empty ring while it should have
 *              been playing.  Each one is a gap the DMA fills with silence.
 *   wr_max_us  worst single bsp_audio_write() block.  One 20 ms frame should
 *              block about 20 ms; much less means the DMA is not pacing us, much
 *              more means we are waiting on the codec.
 *   used_max   ring high-water mark; sitting at AUD_RB_BYTES means the producer
 *              outruns the consumer. */
static uint32_t s_rx_bytes, s_underruns, s_wr_max_us;
static size_t   s_used_max;
/* Pre-buffer state: false means "accumulating", true means "playing". */
static bool     s_playing;
static uint32_t s_rebuffers;
static int64_t  s_dry_since;
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
    s_rx_bytes += n;
    if (s_used > s_used_max) s_used_max = s_used;
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
        /* Accumulate a cushion before the first write, and rebuild it after a
         * sustained dry spell.  Without this the DMA is fed frame-by-frame from
         * an almost empty ring and every network gap is heard as a dropout. */
        if (!s_playing) {
            bool ready;
            if (xSemaphoreTake(s_rb_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
                ready = s_used >= AUD_PREBUF_BYTES;
                xSemaphoreGive(s_rb_mtx);
            } else {
                ready = false;
            }
            if (!ready) {
                if (s_muted || mrrc_tx_active()) {
                    /* Nothing will arrive while keyed; do not start the clock. */
                    s_dry_since = 0;
                }
                /* Must yield: this loop runs at priority 8 and a bare spin
                 * starves IDLE and trips the task watchdog (it did). */
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            s_playing = true;
            s_rebuffers++;
            s_dry_since = 0;
        }

        const size_t n = rb_get(frame, sizeof frame, pdMS_TO_TICKS(100));
        if (n == 0) {
            /* An empty ring while we should be playing is an underrun: the DMA
             * has nothing to send and fills silence (auto_clear_after_cb). */
            if (!s_muted && !mrrc_tx_active()) {
                s_underruns++;
                        if (s_dry_since == 0) s_dry_since = esp_timer_get_time();
                else if (esp_timer_get_time() - s_dry_since > AUD_REBUFFER_US) {
                    s_playing = false;      /* fell too far behind: re-buffer */
                    s_dry_since = 0;
                }
            }
            /* rb_get() returns immediately when the buffer is empty; without
             * this delay the loop spins at full speed and starves IDLE, which
             * trips the task watchdog (5 s) on a single-core C3. */
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        s_dry_since = 0;
        /* The uplink is plain 16 kHz Int16 PCM — no codec, no rate change.
         * The server downsamples for us, so the captured buffer goes to I2S
         * unchanged. */
        s_frames++;
        /* Half duplex: drop playback while the transmitter is keyed.  Feeding
         * our own uplink back through the speaker is a howl, not audio. */
        if (s_muted || mrrc_tx_active()) continue;

        const int16_t *in = (const int16_t *)frame;
        size_t samples = n / sizeof(int16_t);
        if (samples == 0) continue;

        if (!mrrc_net_audio_16k()) {
            /* 48 kHz fallback: decimate 3:1 in place.  A 3-tap average is both
             * the decimation filter and a cheap anti-alias, which is all a 3 kHz
             * SSB passband needs.  The write index always trails the read index,
             * so no second buffer is required. */
            size_t out = 0;
            int16_t *w = (int16_t *)frame;          /* safe: out <= i */
            for (size_t i = 0; i + 2 < samples; i += 3) {
                w[out++] = (int16_t)((in[i] + in[i + 1] + in[i + 2]) / 3);
            }
            samples = out;
            if (samples == 0) continue;
        }
        const int64_t t_wr = esp_timer_get_time();
        if (bsp_audio_write((const int16_t *)frame, samples * sizeof(int16_t)) != ESP_OK)
            s_errors++;
        const uint32_t wr = (uint32_t)(esp_timer_get_time() - t_wr);
        if (wr > s_wr_max_us) s_wr_max_us = wr;
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
    /* [channel][codec tag][payload].  The channel byte (0x00 = rx audio) is
     * stripped by the caller in mrrc_net.c, so what arrives here starts at the
     * codec tag.  Accepted tags:
     *   0x02  16 kHz Int16 PCM  — the negotiated format
     *   0x00  48 kHz Int16 PCM  — the fallback when the server has no pcm16
     *   0x01  Opus              — not linked in; dropped on the floor
     */
    if (data[0] == 0x02 || data[0] == 0x00) {
        rb_put(data + 1, len - 1);
    }
}

void mrrc_audio_suspend(void)
{
    /* Single connection: no socket to free.  Stop playing so the uplink does
     * not howl, and drop whatever is already queued. */
    s_muted = true;
    s_playing = false;
    s_dry_since = 0;
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

void mrrc_audio_link_stats(uint32_t *rx_bytes, uint32_t *underruns,
                           uint32_t *wr_max_us, size_t *used_max)
{
    if (rx_bytes)  *rx_bytes  = s_rx_bytes;
    if (underruns) *underruns = s_underruns;
    if (wr_max_us) *wr_max_us = s_wr_max_us;
    if (used_max)  *used_max  = s_used_max;
}

uint32_t mrrc_audio_rebuffers(void) { return s_rebuffers; }

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
