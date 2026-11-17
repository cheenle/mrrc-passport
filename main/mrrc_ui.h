// main/mrrc_ui.h — the terminal screen.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Builds the screen and starts the refresh task.  Must be called after
 * bsp_lvgl_init(); reads state through mrrc_state_lock(). */
void mrrc_ui_start(void);

/* Which knob the UP/DOWN keys drive.  The badge in the top bar is the only
 * place this is visible, so it has to be obvious at a glance. */
typedef enum { UI_MODE_FREQ = 0, UI_MODE_CHANNEL = 1, UI_MODE_VOL = 2 } ui_mode_t;
void mrrc_ui_set_mode(ui_mode_t mode, int volume_pct);

/* 850 raw bins, one per column after downsampling.  Called from the WebSocket
 * task, so it must not touch LVGL directly — it only publishes the data. */
void mrrc_ui_spectrum(const uint8_t *bins, size_t n);

/* Channel name shown in the info row while the knob is in CHANNEL mode. */
void mrrc_ui_set_channel(const char *label);

/* Local status the UI shows but the server knows nothing about. */
void mrrc_ui_set_battery(int percent, bool charging);
void mrrc_ui_set_toast(const char *text);
