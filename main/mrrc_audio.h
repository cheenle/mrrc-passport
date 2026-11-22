// main/mrrc_audio.h — downlink audio (S2).
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Ring capacity in bytes, so the once-a-minute diag line can print occupancy
 * without reaching into the implementation.  Keep in step with AUD_RB_BYTES. */
#define AUD_RB_BYTES_CONST 16384

/* Connects /WSaudioRX and starts the decode+playback task.  Call after
 * bsp_audio_set_format() so the codec is already open at the shared rate. */
/* Creates the Opus decoder and encoder and runs one dummy call through each,
 * so the (lazily allocated, globally shared) pseudostack is reserved while the
 * heap is still unfragmented.  Call once after bsp_audio_set_format(). */
int mrrc_audio_prewarm(void);

void mrrc_audio_start(void);

/* Downlink audio arrives on the shared /WSterminal socket.  The tag byte
 * differs between transports (0x01 on /WSaudioRX, 0x00 on /WSterminal), so both
 * are accepted. */
void mrrc_audio_feed(const uint8_t *data, size_t len);

/* Half duplex: the terminal only has room for one extra TLS session, so the
 * downlink socket is torn down while the transmitter is keyed and rebuilt on
 * release.  Suspend frees ~16 KB (socket + TLS context), which is exactly what
 * /WSaudioTX needs. */
void mrrc_audio_suspend(void);
void mrrc_audio_resume(void);

void mrrc_audio_stats(uint32_t *frames, uint32_t *errors, uint32_t *drops);
/* Stutter root-cause counters; see the comment on s_rx_bytes in mrrc_audio.c. */
void mrrc_audio_link_stats(uint32_t *rx_bytes, uint32_t *underruns,
                           uint32_t *wr_max_us, size_t *used_max);
/* How many times playback started after accumulating the pre-buffer cushion. */
uint32_t mrrc_audio_rebuffers(void);
/* Which drop path fired: mutex timeout vs ring full. */
void mrrc_audio_drop_split(uint32_t *lock, uint32_t *full);
/* Frames and samples written to I2S since boot (the consumer's real rate). */
void mrrc_audio_written(uint32_t *frames, uint64_t *samples);
/* Underruns, current/high-water ring occupancy, and the mute/TX state. */
/* Stall recoveries: the stream was quiet for over a second and the queued audio
 * was discarded instead of being replayed stale. */
uint32_t mrrc_audio_stall_flushes(void);
void mrrc_audio_state(uint32_t *underruns, size_t *used, size_t *used_max,
                      bool *muted, bool *tx);

/* Profiling: total frames played and the accumulated time spent in the audio
 * path.  dec_us is always 0 now that no decoder is linked in; the parameter is
 * kept so callers do not change. */
void mrrc_audio_profile(uint32_t *frames, int64_t *dec_us);
