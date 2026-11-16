// main/mrrc_audio.h — downlink audio (S2).
#pragma once

#include <stdint.h>

/* Connects /WSaudioRX and starts the decode+playback task.  Call after
 * bsp_audio_set_format() so the codec is already open at the shared rate. */
void mrrc_audio_start(void);

void mrrc_audio_stats(uint32_t *frames, uint32_t *errors, uint32_t *drops);
