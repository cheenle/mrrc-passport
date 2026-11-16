// main/mrrc_ui.h — the terminal screen.
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Builds the screen and starts the refresh task.  Must be called after
 * bsp_lvgl_init(); reads state through mrrc_state_lock(). */
void mrrc_ui_start(void);

/* Local status the UI shows but the server knows nothing about. */
void mrrc_ui_set_battery(int percent, bool charging);
void mrrc_ui_set_toast(const char *text);
