// main/mrrc_tx.h — PTT uplink (S3).
#pragma once

#include <stdbool.h>

/* Keying the transmitter opens /WSaudioTX, streams the codec's 48 kHz PCM with
 * the AUDIO_TAG_PCM (0x00) prefix, and closes the socket again on release.
 *
 * Why raw PCM and not Opus: server.py's AudioHandler.feed_tx_audio() documents
 * its input as "48 kHz Int16 PCM (from Opus decoder or browser)" and resamples
 * to 44.1 kHz for the radio itself.  The ES8311 already runs at 48 kHz, so the
 * encoder would only cost memory (15 KB instance + 24 KB pseudostack) and add
 * a resampling step for no benefit.  Uplink is ~768 kbit/s, which is nothing on
 * a LAN.
 *
 * Requires the codec to be open (S2 does that at boot). */
bool mrrc_tx_key(bool on);
bool mrrc_tx_active(void);
