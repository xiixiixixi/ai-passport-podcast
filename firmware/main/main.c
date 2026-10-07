// Dedicated three-button podcast appliance; boot directly into its own UI.
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "demo.h"
#include "podcast_fonts.h"
#include "podcast_config.h"
#include "podcast_setup.h"
#include "esp_log.h"
#include "nvs_flash.h"
static const char *TAG = "main";

static void on_key(bsp_btn_t button, bsp_btn_ev_t event, void *unused)
{
    (void)unused;
    if (podcast_setup_active()) podcast_setup_key(button, event);
    else demo_podcast_key(button, event); // Only a bounded, zero-wait queue send.
}
void app_main(void)
{
    ESP_LOGI(TAG, "Podcast player startup");
    if (bsp_i2c_init() != ESP_OK || bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "Display initialization failed");
        return;
    }
    bsp_display_backlight(85);
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "Battery indicator unavailable");
    /* Never erase NVS to recover from a configuration/storage error. */
    if (nvs_flash_init() != ESP_OK) { ESP_LOGE(TAG, "Saved storage unavailable; records preserved"); return; }
    podcast_config_result_t connection = podcast_config_init();
    bool setup_requested = podcast_config_take_setup_request();
    if (bsp_button_init(on_key, NULL) != ESP_OK) { ESP_LOGE(TAG, "Button initialization failed"); return; }
    if (connection != PODCAST_CONFIG_OK || setup_requested) {
        if (!podcast_setup_run()) ESP_LOGE(TAG, "Connection setup failed; saved records preserved");
        return;
    }
    if (bsp_audio_init() != ESP_OK) ESP_LOGE(TAG, "Audio initialization failed");
    if (!bsp_lvgl_lock(1000)) return;
    uint32_t font_checksum = 0;
    bool font_ok = podcast_fonts_probe(&font_checksum);
    ESP_LOGI(TAG, "Chinese font probe: %s bitmap=%08lx, 18px/4bpp, line=%u",
             font_ok ? "PASS" : "FAIL", (unsigned long)font_checksum,
             (unsigned)app_cjk_18.line_height);
    if (!font_ok) ESP_LOGE(TAG, "Chinese glyph data failed validation");
    podcast_font_render_report_t drawn;
    bool draw_ok = podcast_fonts_render_probe(&drawn);
    ESP_LOGI(TAG, "Chinese software draw: %s decoded=%u a8=%08lx rgb565=%08lx pixels18=%u pixels16=%u",
             draw_ok ? "PASS" : "FAIL", drawn.decoded_glyphs,
             (unsigned long)drawn.decoded_checksum, (unsigned long)drawn.rendered_checksum,
             drawn.pixels_18, drawn.pixels_16);
    if (!draw_ok) ESP_LOGE(TAG, "Chinese software drawing failed; LCD output is not tested here");
    demo_podcast_enter();
    bsp_lvgl_unlock();
    if (demo_podcast_start() != ESP_OK) {
        ESP_LOGE(TAG, "Podcast service initialization failed");
        return;
    }
    ESP_LOGI(TAG, "Podcast catalogue ready; physical OK selects an episode");
}
