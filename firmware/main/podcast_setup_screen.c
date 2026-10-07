#include "podcast_setup_screen.h"
#include "podcast_fonts.h"
#include "lvgl.h"

static lv_obj_t *screen, *state_label;
static lv_obj_t *label(const char *text, int y, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(screen); lv_obj_set_pos(obj, 16, y); lv_obj_set_width(obj, 208);
    lv_obj_set_style_text_font(obj, font, 0); lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP); lv_label_set_text(obj, text); return obj;
}
bool podcast_setup_screen_create(const char *hotspot, const char *password, bool can_cancel)
{
    if (screen) return false;
    screen = lv_obj_create(NULL); if (!screen) return false;
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xffffff), 0); lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    label("连接你的播客", 18, &app_cjk_18, 0x101010);
    label("手机连接以下无线热点", 58, &app_ui_16, 0x606875);
    label(hotspot, 83, &lv_font_montserrat_14, 0x0667f6);
    label("热点密码", 110, &app_ui_16, 0x606875);
    label(password, 135, &lv_font_montserrat_14, 0x101010);
    label("手机浏览器打开", 166, &app_ui_16, 0x606875);
    label("http://192.168.4.1", 191, &lv_font_montserrat_14, 0x0667f6);
    state_label = label("填写网络、后台和配对码", 225, &app_ui_16, 0x101010);
    label(can_cancel ? "长按确定取消 · 保留原设置" : "首次连接后自动进入节目库", 287, &app_ui_16, 0x7c838e);
    lv_scr_load(screen); return true;
}
void podcast_setup_screen_message(const char *message)
{ if (state_label) lv_label_set_text(state_label, message); }
void podcast_setup_screen_delete(void)
{ if (screen) { lv_obj_delete(screen); screen = NULL; state_label = NULL; } }
