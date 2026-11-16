// main/mrrc_net.h — Wi-Fi + /WSradio control channel.
//
// Deliberately the subset the terminal needs to draw a screen: frequency, mode,
// S-meter, TX state.  Audio is a separate stage (see docs/design-v1.md).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MRRC_MODE_NAME_LEN 12
#define MRRC_BAND_NAME_LEN 8
#define MRRC_S_UNIT_LEN    12

typedef struct {
    int32_t freq_hz;
    int32_t vfo_a_hz;
    int32_t vfo_b_hz;
    char    vfo;
    char    mode_name[MRRC_MODE_NAME_LEN];
    char    band_name[MRRC_BAND_NAME_LEN];
    int     s_meter;        /* 0..255 */
    char    s_unit[MRRC_S_UNIT_LEN];
    int     af_gain;
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

/* Control writes (server-side: {"type":"set","field":...,"value":...}). */
void mrrc_net_set_freq(int32_t hz);
void mrrc_net_set_af_gain(int gain);

/* "Authorization: Bearer ...\r\n", valid after the first successful login. */
const char *mrrc_auth_header(void);

/* Raw token, for the ?token= query the server actually checks. */
const char *mrrc_token(void);
