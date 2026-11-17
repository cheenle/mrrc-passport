// components/bsp/src/bsp_display_lvgl.c
// LVGL 接入单独成文件:不用 LVGL 的开发者删掉本文件 + idf_component.yml 里的两条依赖即可。
#include "bsp_display.h"
#include "bsp_display_rounding.h"
#include "bsp_pins.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "bsp_lvgl";

/* 8 lines = 3.8 KB.  Raising this to 20 to halve the SPI transactions looked
 * like a win on paper, but it costs 5.6 KB of contiguous internal RAM and PTT
 * needs ~16 KB in one piece for a TLS record — with the waterfall and audio
 * ring also enlarged the largest free block fell far enough that transmissions
 * dropped on their own.  Left at 8; the extra transfers are the cheaper price. */
/* 32 lines = 15.0 KB (7680 px).
 *
 * Espressif's own LVGL performance guide is explicit about this setting:
 * a frame buffer below 10% of the screen has a "severe negative effect", and
 * above ~25% brings no further gain.  On a 240x320 panel that puts the useful
 * window at 7680..19200 px.  8 lines was 3.8%, 12 lines 5.6%, 24 lines 7.5% —
 * all under the floor.  32 lines is exactly 10.0%, the first setting the guide
 * calls adequate.
 *
 * Measured: 8 lines -> 58% taskLVGL, 12 lines -> 43-49%, so the curve is real.
 * Costs 9.4 KB of contiguous internal RAM beyond the 12-line version; largest
 * free block was 38 KB and push-to-talk needs ~16 KB in one piece, so this
 * leaves ~28 KB — enough, but this is the largest buffer worth trying without
 * first shrinking the waterfall.
 *
 * If colours or the display come up wrong after this, suspect the RAMCTRL /
 * swap_bytes pairing in bsp_display.c before suspecting the size. */
#define BSP_LVGL_DRAW_BUFFER_LINES 32

static lv_display_t *s_disp;
static bool s_port_initialized;
static bool s_port_init_failed;

static void rounded_flush_event(lv_event_t *event)
{
    lv_display_t *disp = lv_event_get_target(event);
    const lv_area_t *area = lv_event_get_param(event);
    lv_draw_buf_t *draw_buf = lv_display_get_buf_active(disp);
    if (!area || !draw_buf || !draw_buf->data ||
        lv_display_get_color_format(disp) != LV_COLOR_FORMAT_RGB565) {
        return;
    }

    const int32_t width = lv_area_get_width(area);
    if (draw_buf->header.stride < (uint32_t)width * sizeof(uint16_t)) return;

    for (int32_t y = area->y1; y <= area->y2; ++y) {
        uint16_t *row = (uint16_t *)(draw_buf->data +
                                     (y - area->y1) * draw_buf->header.stride);
        int32_t visible_x1;
        int32_t visible_x2;
        if (!bsp_display_rounded_row_span(y, BSP_LCD_W, BSP_LCD_H,
                                          BSP_LVGL_SCREEN_RADIUS, &visible_x1,
                                          &visible_x2)) {
            memset(row, 0, (size_t)width * sizeof(uint16_t));
            continue;
        }
        // Only clear pixels outside the visible span. The port swaps RGB565
        // bytes after this event; black is 0 in either byte order.
        const int32_t clear_left_end = visible_x1 > area->x2 ? area->x2 : visible_x1 - 1;
        const int32_t clear_right_start = visible_x2 < area->x1 ? area->x1 : visible_x2 + 1;
        for (int32_t x = area->x1; x <= clear_left_end; ++x) {
            row[x - area->x1] = 0;
        }
        for (int32_t x = clear_right_start; x <= area->x2; ++x) {
            row[x - area->x1] = 0;
        }
    }

}

lv_display_t *bsp_lvgl_init(void) {
    if (s_disp) return s_disp;
    if (!bsp_display_panel()) {
        ESP_LOGE(TAG, "请先成功调用 bsp_display_init()");
        return NULL;
    }

    if (!s_port_initialized) {
        // Port 2.9.0 has no public completion handshake for asynchronous deinit.
        // Never overwrite a possibly live context after a partial port failure.
        if (s_port_init_failed) {
            ESP_LOGE(TAG, "LVGL port 初始化未完成，需重启后重试");
            return NULL;
        }
        const lvgl_port_cfg_t pc = ESP_LVGL_PORT_INIT_CONFIG();
        if (lvgl_port_init(&pc) != ESP_OK) {
            s_port_init_failed = true;
            ESP_LOGE(TAG, "lvgl_port_init 失败，需重启后重试");
            return NULL;
        }
        s_port_initialized = true;
    }

    const lvgl_port_display_cfg_t dc = {
        .panel_handle = bsp_display_panel(),
        .io_handle    = bsp_display_io(),
        // ⚠ C3 无 PSRAM,DMA 只能用内部 RAM。40 行单缓冲约 19.2KB，
        // 可减少窗口命令和队列提交次数；仍保留单缓冲，避免双缓冲挤压音频/Wi-Fi。
        .buffer_size   = (uint32_t)BSP_LCD_W * BSP_LVGL_DRAW_BUFFER_LINES,
        .double_buffer = false,
        .hres = BSP_LCD_W, .vres = BSP_LCD_H,
        // 旋转/镜像必须在这里配:esp_lvgl_port 注册显示时会重新下发 MADCTL,
        // 覆盖 bsp_display.c 里 esp_lcd_panel_mirror() 的设置。
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        /* swap_bytes OFF: the panel is told to read little-endian via RAMCTRL
         * (0xB0) in bsp_display.c, matching what LVGL writes.  Leaving it on
         * made the port run lv_draw_sw_rgb565_swap over every pixel of every
         * flush — 20400 swaps per refresh, ~67k/s — for no reason once the
         * panel agrees on the byte order.  If colours come out swapped, the
         * panel wanted big-endian and both this flag and RAMCTRL must flip
         * back together. */
        .flags = { .buff_dma = true, .swap_bytes = false },
    };
    // The port mutex is recursive. Keep registration and the mask callback in
    // one critical section, before the new display can produce its first flush.
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "LVGL 初始化加锁失败");
        return NULL;
    }
    lv_display_t *disp = lvgl_port_add_disp(&dc);
    bool mask_registered = false;
    if (disp) {
        // LVGL 9.5 returns void here; check the list while still holding the lock.
        const uint32_t count = lv_display_get_event_count(disp);
        lv_display_add_event_cb(disp, rounded_flush_event, LV_EVENT_FLUSH_START, NULL);
        const uint32_t after = lv_display_get_event_count(disp);
        ESP_LOGI(TAG, "display=%p events %u -> %u (expect %u)",
                 (void *)disp, (unsigned)count, (unsigned)after, (unsigned)(count + 1));
        /* Some LVGL 9 builds coalesce event_cb entries per event code, so the
         * count may not grow even though the callback was accepted.  Treat a
         * successful add as success unless the count went DOWN. */
        mask_registered = (after >= count);
    }
    if (!mask_registered) {
        ESP_LOGE(TAG, "LVGL display 或圆角回调注册失败");
        if (disp) lvgl_port_remove_disp(disp);
        lvgl_port_unlock();
        // Retain the initialized port for retry. Deinit is asynchronous and can
        // race the next init (or even run before the task sets running=true).
        return NULL;
    }

    // Mask the final RGB565 flush instead of using root-screen clip_corner.
    // Full-screen rounded clipping creates an ARGB layer that does not fit the
    // 24 KB LVGL pool reliably on this no-PSRAM target.
    s_disp = disp;
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL 就绪，全局圆角=%d，外部填充=黑色", BSP_LVGL_SCREEN_RADIUS);
    return s_disp;
}

bool bsp_lvgl_lock(int timeout_ms) {
    if (!s_disp) return false;
    return lvgl_port_lock(timeout_ms);
}
void bsp_lvgl_unlock(void) {
    if (s_disp) lvgl_port_unlock();
}
