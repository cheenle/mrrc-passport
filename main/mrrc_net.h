// main/mrrc_net.h — Wi-Fi + /WSradio control channel.
//
// Deliberately the subset the terminal needs to draw a screen: frequency, mode,
// S-meter, TX state.  Audio is a separate stage (see docs/design-v1.md).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MRRC_MODE_NAME_LEN 12
#define MRRC_BAND_NAME_LEN 8
#define MRRC_S_UNIT_LEN    12

#define MRRC_MAX_CH 8

typedef struct {
    int32_t freq_hz;
    char    mode[8];
    char    label[20];
} mrrc_chan_t;

/* Channel list pushed by the server in every fullState ("memChannels"). */
extern mrrc_chan_t g_chans[MRRC_MAX_CH];
extern int g_chan_count;

typedef struct {
    int32_t freq_hz;
    int32_t vfo_a_hz;
    int32_t vfo_b_hz;
    char    vfo;
    char    mode_name[MRRC_MODE_NAME_LEN];
    char    band_name[MRRC_BAND_NAME_LEN];
    int     s_meter;        /* 0..255 */
    char    s_unit[MRRC_S_UNIT_LEN];
    /* TX meters, shown in place of the S-meter while transmitting.  The
     * server publishes these as power_meter.power_watts / swr_meter.swr_ratio;
     * -1 means "not reported" and the panel shows dashes. */
    float   power_w;        /* PEP watts */
    float   swr;            /* 1.0 .. 99.9 */
    int     af_gain;
    int     scope_span;         /* SS05 index into k_spans_hz; 6 = 100 kHz */
    int     filter_hz;
    int     tx_status;      /* 0 = RX, 1 = TX, 2 = TUNE */
    bool    radio_online;
    uint32_t seq;           /* bumped when a main field changes */
    uint32_t meter_seq;     /* bumped when the S-meter changes */
} mrrc_state_t;

/* Copy of the last known state.  Callers must hold the lock for a consistent
 * read of two or more fields. */
extern mrrc_state_t g_radio;
void mrrc_state_lock(void);
void mrrc_state_unlock(void);

void mrrc_net_start(void);
bool mrrc_net_up(void);
/* Current AP signal in dBm, or 0 when there is no station connection. */
int mrrc_net_rssi(void);
/* Payload bytes received on /WSterminal (audio + spectrum), for the diag line. */
uint32_t mrrc_net_ws_rx_bytes(void);

/* Control writes (server-side: {"type":"set","field":...,"value":...}). */
void mrrc_net_set_freq(int32_t hz);
void mrrc_net_set_af_gain(int gain);

/* Keying.  The server force-releases a session that stops sending {"type":
 * "txhb"} for 5 s, so the TX task must heartbeat every 1.5 s. */
void mrrc_net_set_ptt(bool on);

/* Jump to a stored channel: frequency and mode together. */
void mrrc_net_set_chan(int index);
/* Re-asserts the pending channel frequency once a mode transition has settled;
 * call periodically (the UI task's 5 Hz tick is a good place). */
void mrrc_net_chan_pump(void);
/* Settles the audio wire format after the pcm16 request; call from a periodic
 * tick.  A server that never answers is treated as a refusal. */
void mrrc_net_audio_pump(void);
/* True while the uplink/downlink carries 16 kHz PCM16 rather than 48 kHz. */
bool mrrc_net_audio_16k(void);
/* Cycle to the next working mode (LSB -> USB -> CW-U -> ...). */
void mrrc_net_next_mode(void);

/* Single-connection mode: /WSterminal carries control JSON, downlink audio and
 * uplink audio on one socket.  Binary on this socket is audio. */
bool mrrc_net_send_bin(const void *data, size_t len);

/* Tells the server that mic capture stopped ("s:"). */
void mrrc_net_send_text_stop(void);
void mrrc_net_send_txhb(void);

/* "Authorization: Bearer ...\r\n", valid after the first successful login. */
const char *mrrc_auth_header(void);

/* Raw token, for the ?token= query the server actually checks. */
const char *mrrc_token(void);
