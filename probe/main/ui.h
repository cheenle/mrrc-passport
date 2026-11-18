#pragma once
#include <stdint.h>

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#include "mrrc_link.h"

/* 240x320 portrait, LVGL 9. Layout budget (see the design doc, §7):
 *   top bar 26 | freq 56 | info 18 | spectrum 96 | waterfall 76 | status 22 */
void ui_init(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel, esp_lcd_touch_handle_t tp);
void ui_update(const mrrc_state_t *st, const uint8_t *spec, int spec_len);
/* Called from the audio task: 1 = transmitting (draw a red F on the status bar). */
void ui_set_tx(int on);
