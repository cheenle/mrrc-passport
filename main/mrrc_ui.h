// main/mrrc_ui.h — the terminal screen.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Builds the screen and starts the refresh task.  Must be called after
 * bsp_lvgl_init(); reads state through mrrc_state_lock(). */
void mrrc_ui_start(void);

/* Setup mode has no frequency to show, so reuse the main labels for the one
 * thing the operator needs: the SSID to join and the address to open. */
void mrrc_ui_setup_hint(const char *ssid, const char *pass, const char *url);

/* Which knob the UP/DOWN keys drive.  The badge in the top bar is the only
 * place this is visible, so it has to be obvious at a glance. */
/* Knob targets.  Moved by OK double-tap; the same key cycles them.
 *
 * UI_MODE_MIC adjusts the ES8311 input PGA (0..24 dB).  It is separate from
 * UI_MODE_VOL because the two halves of the link need different amounts of
 * help: the speaker is usually fine at the default, while a quiet microphone
 * is exactly what makes a transmission sound weak on the far end. */
typedef enum {
    UI_MODE_FREQ = 0,
    UI_MODE_CHANNEL = 1,
    UI_MODE_VOL = 2,
    UI_MODE_MIC = 3,
    UI_MODE_STEP = 4,     /* tuning step: 1K / 5K / 25K */
    UI_MODE_FILTER = 5,   /* IF filter: 1.8K / 2.4K / 2.7K / 3.0K */
    UI_MODE_COUNT = 6
} ui_mode_t;
void mrrc_ui_set_mode(ui_mode_t mode, int volume_pct);

/* 850 raw bins, one per column after downsampling.  Called from the WebSocket
 * task, so it must not touch LVGL directly — it only publishes the data. */
void mrrc_ui_spectrum(const uint8_t *bins, size_t n);

/* Channel name shown in the info row while the knob is in CHANNEL mode. */
void mrrc_ui_set_channel(const char *label);

/* Local status the UI shows but the server knows nothing about. */
void mrrc_ui_set_battery(int percent, bool charging);
void mrrc_ui_set_toast(const char *text);
