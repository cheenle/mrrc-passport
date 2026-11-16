// main/mrrc_audio.h — downlink audio (S2).
#pragma once

#include <stdint.h>

/* Connects /WSaudioRX and starts the decode+playback task.  Call after
 * bsp_audio_set_format() so the codec is already open at the shared rate. */
/* Creates the Opus decoder and encoder and runs one dummy call through each,
 * so the (lazily allocated, globally shared) pseudostack is reserved while the
 * heap is still unfragmented.  Call once after bsp_audio_set_format(). */
int mrrc_audio_prewarm(void);

void mrrc_audio_start(void);

/* Half duplex: the terminal only has room for one extra TLS session, so the
 * downlink socket is torn down while the transmitter is keyed and rebuilt on
 * release.  Suspend frees ~16 KB (socket + TLS context), which is exactly what
 * /WSaudioTX needs. */
void mrrc_audio_suspend(void);
void mrrc_audio_resume(void);

void mrrc_audio_stats(uint32_t *frames, uint32_t *errors, uint32_t *drops);

/* Profiling: total frames decoded and the accumulated time spent inside
 * opus_decode(), for computing real-time load on this core. */
void mrrc_audio_profile(uint32_t *frames, int64_t *dec_us);
