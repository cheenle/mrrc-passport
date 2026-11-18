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
    bool up;
    int32_t freq_hz;
    char mode[12];
    char band[8];
    int filter_hz;
    int scope_span;               /* SS05 index */
    int smeter;
    bool serial_connected;
    uint32_t state_updates;
    uint32_t spectrum_frames;
    uint32_t audio_frames;
    uint32_t audio_drops;
} mrrc_state_t;

/* One socket (/WSterminal) carries control + spectrum + both audio directions.
 * That is the whole reason this design fits: passport already proved a single
 * ESP32 socket is enough, and four separate TLS sessions do not fit in this
 * chip's internal RAM (measured: 4.5 KB free, fourth socket's task fails). */

void mrrc_link_start(const mrrc_cfg_t *cfg);
bool mrrc_link_up(void);
const mrrc_state_t *mrrc_state(void);

/* Downlink: one decoded-ready frame at a time. Returns bytes written into buf,
 * 0 when nothing is pending. Frame layout is [chan][tag][payload]; only audio
 * frames are queued here, spectrum frames update mrrc_state() directly. */
int  mrrc_link_take_audio(uint8_t *buf, int max);

/* Uplink: 16 kHz mono Int16 -> [0x01][0x02][samples]. */
bool mrrc_link_send_pcm(const int16_t *pcm, int samples);
void mrrc_link_set_ptt(bool on);
void mrrc_link_send_ptt_heartbeat(void);

/* Latest spectrum row (0..255 per bin), 1 byte per bin. */
int mrrc_link_spectrum(uint8_t *out, int max);
