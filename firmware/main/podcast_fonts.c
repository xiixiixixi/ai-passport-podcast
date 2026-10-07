#include "podcast_fonts.h"

static uint32_t checksum_byte(uint32_t checksum, uint8_t byte)
{
    return (checksum ^ byte) * 16777619u;
}

bool podcast_fonts_probe(uint32_t *bitmap_checksum)
{
    static const uint32_t codepoints[] = {
        0x4e2d, 0x6587, 0x6545, 0x4e8b, 0x5ffd, 0x5de6, 0x5b9c, 0x7845,
        0x8c37, 0x6682, 0x505c, 0x7eed, 0x64ad, 0x3400, 0x9f98, 0xff0c
    };
    uint32_t checksum = 2166136261u;
    for (unsigned i = 0; i < sizeof(codepoints) / sizeof(codepoints[0]); ++i) {
        lv_font_glyph_dsc_t glyph = {0};
        if (!lv_font_get_glyph_dsc(&app_cjk_18, &glyph, codepoints[i], 0)
            || glyph.is_placeholder || glyph.resolved_font != &app_cjk_18
            || glyph.format != LV_FONT_GLYPH_FORMAT_A4 || !glyph.box_w || !glyph.box_h)
            return false;
        glyph.req_raw_bitmap = 1;
        const uint8_t *bitmap = app_cjk_18.get_glyph_bitmap(&glyph, NULL);
        if (!bitmap) return false;
        size_t bytes = ((size_t)glyph.box_w * glyph.box_h + 1) / 2;
        for (size_t j = 0; j < bytes; ++j) checksum = (checksum ^ bitmap[j]) * 16777619u;
    }
    if (bitmap_checksum) *bitmap_checksum = checksum;
    lv_font_glyph_dsc_t unsupported = {0};
    bool found = lv_font_get_glyph_dsc(&app_cjk_18, &unsupported, 0x1f984, 0);
    return checksum == 0x6296d092u && (!found || unsupported.is_placeholder);
}

static bool decoded_glyph(const lv_font_t *font, uint32_t codepoint,
                          lv_draw_buf_t *buffer, podcast_font_render_report_t *report)
{
    lv_font_glyph_dsc_t glyph;
    if (!lv_font_get_glyph_dsc(font, &glyph, codepoint, 0) || glyph.is_placeholder ||
        glyph.resolved_font != font || glyph.format != LV_FONT_GLYPH_FORMAT_A4 ||
        !glyph.box_w || !glyph.box_h || glyph.box_w > 32 || glyph.box_h > 32) return false;
    if (!lv_draw_buf_reshape(buffer, LV_COLOR_FORMAT_A8, glyph.box_w, glyph.box_h, LV_STRIDE_AUTO)) return false;
    glyph.req_raw_bitmap = 1;
    const uint8_t *packed = font->get_glyph_bitmap(&glyph, NULL);
    glyph.req_raw_bitmap = 0;
    const lv_draw_buf_t *decoded = lv_font_get_glyph_bitmap(&glyph, buffer);
    if (!packed || !decoded || decoded->header.cf != LV_COLOR_FORMAT_A8 ||
        decoded->header.stride < glyph.box_w) return false;
    for (unsigned y = 0; y < glyph.box_h; ++y) {
        const uint8_t *line = decoded->data + y * decoded->header.stride;
        for (unsigned x = 0; x < glyph.box_w; ++x) {
            unsigned pixel = y * glyph.box_w + x;
            uint8_t expected = (uint8_t)(((packed[pixel / 2] >> ((pixel & 1) ? 0 : 4)) & 15) * 17);
            if (line[x] != expected) return false;
            report->decoded_checksum = checksum_byte(report->decoded_checksum, line[x]);
        }
    }
    ++report->decoded_glyphs;
    return true;
}

bool podcast_fonts_render_probe(podcast_font_render_report_t *report)
{
    if (!report) return false;
    *report = (podcast_font_render_report_t){ .decoded_checksum = 2166136261u,
                                            .rendered_checksum = 2166136261u };
    lv_draw_buf_t *a8 = lv_draw_buf_create(32, 32, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    if (!a8) return false;
    static const uint32_t full[] = { 0x4e2d, 0x6587, 0x64ad, 0x5ba2, 0x3400, 0x9f98 };
    static const uint32_t fixed[] = { 0x6682, 0x505c, 0x64ad, 0x5ba2 };
    bool valid = true;
    for (unsigned i = 0; i < sizeof(full) / sizeof(full[0]) && valid; ++i)
        valid = decoded_glyph(&app_cjk_18, full[i], a8, report);
    for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]) && valid; ++i)
        valid = decoded_glyph(&app_ui_16, fixed[i], a8, report);
    lv_draw_buf_destroy(a8);
    if (!valid) return false;

    /* Temporary buffer is released before the catalogue, WiFi or audio starts. */
    lv_draw_buf_t *rgb = lv_draw_buf_create(64, 48, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (!rgb) return false;
    lv_obj_t *canvas = lv_canvas_create(lv_screen_active());
    if (!canvas) { lv_draw_buf_destroy(rgb); return false; }
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_draw_buf(canvas, rgb);
    lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_draw_label_dsc_t draw;
    lv_draw_label_dsc_init(&draw);
    draw.color = lv_color_black(); draw.opa = LV_OPA_COVER;
    draw.font = &app_cjk_18; draw.text = "中文";
    lv_area_t upper = { 0, 0, 63, 22 };
    lv_draw_label(&layer, &draw, &upper);
    draw.font = &app_ui_16; draw.text = "暂停";
    lv_area_t lower = { 0, 24, 63, 43 };
    lv_draw_label(&layer, &draw, &lower);
    lv_canvas_finish_layer(canvas, &layer);
    for (unsigned y = 0; y < 48; ++y) {
        const uint8_t *line = rgb->data + y * rgb->header.stride;
        for (unsigned x = 0; x < 64; ++x) {
            uint8_t low = line[x * 2], high = line[x * 2 + 1];
            report->rendered_checksum = checksum_byte(checksum_byte(report->rendered_checksum, low), high);
            if (low != 255 || high != 255) {
                if (y < 23) ++report->pixels_18;
                else if (y >= 24 && y < 44) ++report->pixels_16;
            }
        }
    }
    lv_obj_delete(canvas);
    lv_draw_buf_destroy(rgb);
    /* Locked Noto assets/LVGL 9.5 software renderer: a placeholder rectangle
     * also has painted pixels, so require the complete expected images. */
    return report->decoded_glyphs == 10 && report->decoded_checksum == 0x6be31a46u &&
           report->rendered_checksum == 0xb704ea88u &&
           report->pixels_18 == 227 && report->pixels_16 == 258;
}
