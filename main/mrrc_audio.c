// main/mrrc_audio.c — /WSaudioRX -> Opus decode -> I2S -> ES8311.
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
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mrrc_audio.h"
#include "mrrc_net.h"
#include "mrrc_tx.h"
#include "opus.h"

static const char *TAG = "audio";

#define AUD_WS_STACK   6144
#define AUD_TASK_STACK 8192
#define AUD_RB_BYTES   6144         /* ~0.8 s of 64 kbps Opus */
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
static esp_websocket_client_handle_t s_ws;
static OpusDecoder *s_dec;
static OpusEncoder *s_enc;
#define TX_RATE 16000
#define TX_FRAME 960        /* 60 ms at 16 kHz, the rate the server expects */

static uint32_t s_frames, s_errors, s_drops;
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

/* ── WebSocket ───────────────────────────────────────────────────────── */

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "/WSaudioRX connected");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "/WSaudioRX disconnected");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (ev->op_code == 0x02 && ev->data_len > 1 && ev->data_ptr) {
            const uint8_t tag = ev->data_ptr[0];
            if (tag == 0x01) {
                rb_put((const uint8_t *)ev->data_ptr + 1, (size_t)ev->data_len - 1);
            }
            /* tag 0x00 (raw PCM) is not used by this server profile. */
        }
        break;
    default:
        break;
    }
}

/* ── decode + play ───────────────────────────────────────────────────── */

static void audio_task(void *arg)
{
    (void)arg;
    static uint8_t frame[AUD_FRAME_MAX];

    uint32_t idle_rounds = 0;
    for (;;) {
        const size_t n = rb_get(frame, sizeof frame, pdMS_TO_TICKS(100));
        if (n == 0) {
            /* rb_get() returns immediately when the buffer is empty; without
             * this delay the loop spins at full speed and starves IDLE, which
             * trips the task watchdog (5 s) on a single-core C3. */
            vTaskDelay(pdMS_TO_TICKS(2));
            if ((++idle_rounds % 500) == 0)
                ESP_LOGI(TAG, "waiting for audio (frames=%u err=%u drop=%u)",
                         (unsigned)s_frames, (unsigned)s_errors, (unsigned)s_drops);
            continue;
        }
        idle_rounds = 0;

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

        const int got = opus_decode(s_dec, frame, (opus_int32)n, s_pcm48, AUD_PCM48_MAX, 0);
        if (got <= 0) {
            s_errors++;
            if ((s_errors & 0x1F) == 1) ESP_LOGW(TAG, "decode error %s", opus_strerror(got));
            continue;
        }
        s_frames++;
        /* Half duplex: drop playback while the transmitter is keyed.  Feeding
         * our own uplink back through the speaker is a howl, not audio. */
        if (mrrc_tx_active()) continue;
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
         * reserves the pseudostack. */
        static int16_t pcm[AUD_PCM48_MAX];
        (void)opus_decode(s_dec, NULL, 0, pcm, AUD_PCM48_MAX, 0);
    }

    /* No encoder: the uplink is raw 48 kHz PCM (server resamples), so the
     * 15 KB instance and the 24 KB encode pseudostack are not needed. */
    if (0 && !s_enc) {
        s_enc = opus_encoder_create(TX_RATE, 1, OPUS_APPLICATION_AUDIO, &err);
        if (err != OPUS_OK || !s_enc) {
            ESP_LOGE(TAG, "encoder create failed: %d", err);
            s_enc = NULL;
            return -1;
        }
        opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(16000));
        opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(0));
        opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(s_enc, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(s_enc, OPUS_SET_PACKET_LOSS_PERC(10));

        static int16_t silence[TX_FRAME];
        static uint8_t probe[512];
        const int n = opus_encode(s_enc, silence, TX_FRAME, probe, sizeof probe);
        if (n < 0) {
            ESP_LOGE(TAG, "encoder warmup failed: %s", opus_strerror(n));
            opus_encoder_destroy(s_enc);
            s_enc = NULL;
            return -1;
        }
    }

    ESP_LOGI(TAG, "opus prewarm ok: free=%uK largest=%uK",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    return 0;
}

static void audio_ws_open(void)
{
    if (s_ws) return;
    char uri[320];
    snprintf(uri, sizeof uri, "wss://%s:%d/WSaudioRX?token=%s",
             CONFIG_MRRC_SERVER_HOST, CONFIG_MRRC_SERVER_PORT, mrrc_token());

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .headers = mrrc_auth_header(),
        .buffer_size = 2048,
        .task_stack = AUD_WS_STACK,
        .reconnect_timeout_ms = 3000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 15,
        .pingpong_timeout_sec = 25,
        .skip_cert_common_name_check = true,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "ws init failed");
        return;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_websocket_client_start(s_ws);
    ESP_LOGI(TAG, "/WSaudioRX -> %s", uri);
}

void mrrc_audio_suspend(void)
{
    if (!s_ws) return;
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    if (xSemaphoreTake(s_rb_mtx, portMAX_DELAY) == pdTRUE) {
        s_r = s_w = s_used = 0;  /* drop stale audio */
        xSemaphoreGive(s_rb_mtx);
    }
    ESP_LOGI(TAG, "downlink suspended, free=%uK largest=%uK",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

void mrrc_audio_resume(void)
{
    audio_ws_open();
}

void mrrc_audio_start(void)
{
    s_rb_mtx = xSemaphoreCreateMutex();
    if (!s_rb_mtx) {
        ESP_LOGE(TAG, "no mutex");
        return;
    }
    if (xTaskCreate(audio_task, "audio", AUD_TASK_STACK, NULL, 8, NULL) != pdPASS) {
        ESP_LOGE(TAG, "audio task failed");
        return;
    }
    audio_ws_open();
}

void mrrc_audio_stats(uint32_t *frames, uint32_t *errors, uint32_t *drops)
{
    if (frames) *frames = s_frames;
    if (errors) *errors = s_errors;
    if (drops)  *drops  = s_drops;
}
