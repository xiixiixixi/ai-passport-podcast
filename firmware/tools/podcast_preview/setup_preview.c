/* Actual boot-setup widgets in the same 24 KiB LVGL pool and 40-row buffer.
 * This is software rendering evidence, never a photograph of the device. */
#include "lvgl.h"
#include "src/misc/lv_text_private.h"
#include "podcast_setup_screen.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

static uint16_t pixels[240 * 320], buffer[240 * 40];
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    uint16_t *in = (uint16_t *)data;
    for (int y = area->y1; y <= area->y2; ++y) for (int x = area->x1; x <= area->x2; ++x) pixels[y * 240 + x] = *in++;
    lv_display_flush_ready(display);
}
static void inspect(lv_obj_t *obj)
{
    lv_area_t box; lv_obj_get_coords(obj, &box);
    assert(box.x1 >= 0 && box.y1 >= 0 && box.x2 < 240 && box.y2 < 320);
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *text = lv_label_get_text(obj); const lv_font_t *font = lv_obj_get_style_text_font(obj, 0);
        uint32_t offset = 0;
        while (text[offset]) {
            uint32_t cp = lv_text_encoded_next(text, &offset); lv_font_glyph_dsc_t glyph = {0};
            if (cp >= 32) assert(lv_font_get_glyph_dsc(font, &glyph, cp, 0) && !glyph.is_placeholder);
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i) inspect(lv_obj_get_child(obj, i));
}
static void save(const char *name)
{
    lv_obj_update_layout(lv_screen_active()); inspect(lv_screen_active()); lv_refr_now(NULL);
    char path[128]; snprintf(path, sizeof(path), "%s.ppm", name); FILE *out = fopen(path, "wb"); assert(out);
    fprintf(out, "P6\n240 320\n255\n");
    for (unsigned i = 0; i < 240 * 320; ++i) {
        uint16_t c = pixels[i]; unsigned char rgb[] = {(unsigned char)(((c >> 11) & 31) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63), (unsigned char)((c & 31) * 255 / 31)};
        assert(fwrite(rgb, 1, 3, out) == 3);
    }
    fclose(out);
}
int main(void)
{
    lv_init(); lv_display_t *display = lv_display_create(240, 320); assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush); lv_obj_t *idle = lv_screen_active();
    assert(podcast_setup_screen_create("Podcast-abcd", "0123456789abcdef", false)); save("setup-first-use");
    const char *errors[] = {"正在验证无线网络", "正在验证后台和配对码", "加密连接需要校时，请稍候", "网络连接失败，请检查密码和距离", "后台连接失败，请检查地址和可信证书", "配对码无效或已过期，请重新生成", "保存失败，原设置仍保留，请重试", "连接成功，即将进入节目库"};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(*errors); ++i) {
        podcast_setup_screen_message(errors[i]); char name[40]; snprintf(name, sizeof(name), "setup-state-%u", i); save(name);
    }
    lv_screen_load(idle); podcast_setup_screen_delete();
    for (unsigned cycle = 0; cycle < 32; ++cycle) {
        assert(podcast_setup_screen_create("Podcast-abcd", "0123456789abcdef", true));
        podcast_setup_screen_message(errors[cycle % (sizeof(errors) / sizeof(*errors))]);
        lv_obj_update_layout(lv_screen_active()); inspect(lv_screen_active()); lv_refr_now(NULL);
        lv_screen_load(idle); podcast_setup_screen_delete();
    }
    lv_mem_monitor_t m; lv_mem_monitor(&m); assert(LV_MEM_SIZE == 24576);
    /* Allocator bookkeeping reduces the monitored usable total slightly. */
    assert(m.total_size <= 24576 && m.total_size >= 20000);
    printf("Actual setup widgets: glyph coverage and screen bounds for all states, 32 create/delete cycles, 24 KiB pool free=%u largest=%u peak=%u PASS\n", (unsigned)m.free_size, (unsigned)m.free_biggest_size, (unsigned)m.max_used);
}
