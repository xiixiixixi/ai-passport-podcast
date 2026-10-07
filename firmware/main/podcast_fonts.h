#pragma once
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

LV_FONT_DECLARE(app_cjk_18);
LV_FONT_DECLARE(app_ui_16);

/* Call after lv_init, under the UI lock. Checks real glyph data, not source text. */
bool podcast_fonts_probe(uint32_t *bitmap_checksum);

typedef struct {
    uint32_t decoded_checksum, rendered_checksum;
    unsigned decoded_glyphs, pixels_18, pixels_16;
} podcast_font_render_report_t;

/* Decode real A8 glyphs and exercise LVGL's RGB565 text draw path offscreen.
 * This tests software drawing, never the physical LCD. Run before creating UI. */
bool podcast_fonts_render_probe(podcast_font_render_report_t *report);
