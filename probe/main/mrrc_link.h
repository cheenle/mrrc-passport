#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *ssid;
    const char *wifi_pass;
    const char *host;             /* 192.168.1.217 */
    int port;                     /* 8888 */
    const char *server_pass;
} mrrc_cfg_t;

typedef struct {
    bool up;                      /* /WSradio connected */
    int32_t freq_hz;
    char mode[12];
    char band[8];
    int filter_hz;
    int scope_span;               /* SS05 index */
    int smeter;                   /* raw meter value, 0..255 */
    bool serial_connected;
    uint32_t state_updates;
    uint32_t spectrum_frames;
    uint32_t audio_frames;
    uint32_t audio_drops;
    uint32_t spectrum_bytes;
} mrrc_state_t;

/* Decoded downlink, 16 kHz mono Int16. Called from the WebSocket task. */
typedef void (*mrrc_audio_sink_t)(const int16_t *pcm, int samples);
void mrrc_link_set_audio_sink(mrrc_audio_sink_t fn);

void mrrc_link_start(const mrrc_cfg_t *cfg);
bool mrrc_link_up(void);
const mrrc_state_t *mrrc_state(void);
