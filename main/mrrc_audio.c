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
#include "opus.h"

static const char *TAG = "audio";

#define AUD_WS_STACK   6144
#define AUD_TASK_STACK 8192
#define AUD_RB_BYTES   6144         /* ~0.8 s of 64 kbps Opus */
#define AUD_FRAME_MAX  576          /* 60 ms @ 48 kHz mono Opus is < 200 B */
#define AUD_PCM_MAX    2880         /* 60 ms @ 48 kHz — the server sends 60 ms frames */

static uint8_t  s_rb[AUD_RB_BYTES];
static size_t   s_r, s_w, s_used;
static SemaphoreHandle_t s_rb_mtx;
static esp_websocket_client_handle_t s_ws;
static OpusDecoder *s_dec;

static uint32_t s_frames, s_errors, s_drops;
static int16_t  s_pcm[AUD_PCM_MAX];

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

        const int got = opus_decode(s_dec, frame, (opus_int32)n, s_pcm, AUD_PCM_MAX, 0);
        if (got <= 0) {
            s_errors++;
            if ((s_errors & 0x1F) == 1) ESP_LOGW(TAG, "decode error %s", opus_strerror(got));
            continue;
        }
        s_frames++;
        /* Blocking on the I2S DMA is what paces this loop; the codec's
         * auto_clear_after_cb fills silence when we are late. */
        if (bsp_audio_write(s_pcm, (size_t)got * sizeof(int16_t)) != ESP_OK)
            s_errors++;
    }
}

void mrrc_audio_start(void)
{
    s_rb_mtx = xSemaphoreCreateMutex();
    if (!s_rb_mtx) {
        ESP_LOGE(TAG, "no mutex");
        return;
    }

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
    if (xTaskCreate(audio_task, "audio", AUD_TASK_STACK, NULL, 8, NULL) != pdPASS) {
        ESP_LOGE(TAG, "audio task failed");
        return;
    }
    esp_websocket_client_start(s_ws);
    ESP_LOGI(TAG, "/WSaudioRX -> %s", uri);
}

void mrrc_audio_stats(uint32_t *frames, uint32_t *errors, uint32_t *drops)
{
    if (frames) *frames = s_frames;
    if (errors) *errors = s_errors;
    if (drops)  *drops  = s_drops;
}
