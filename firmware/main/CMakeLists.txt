idf_component_register(
    SRCS "main.c"
         "demo_navigation.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_podcast.c"
         "podcast_pcm.c"
         "podcast_player.c"
         "podcast_position.c"
         "podcast_bookmarks.c"
         "podcast_sync.c"
         "podcast_config.c"
         "podcast_setup.c"
         "podcast_setup_screen.c"
         "podcast_ui.c"
         "podcast_cover_dynamic.c"
         "podcast_cover_wire.c"
         "podcast_wifi_events.c"
         "app_cjk_18.c"
         "app_ui_16.c"
         "podcast_fonts.c"
         "podcast_font_codec.c"
         "demo_ble.c"
         "demo_low_power.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt json esp_http_client esp_http_server esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash mbedtls
    # podcast_cover_dynamic.c 直接使用 esp_flash.h/spi_flash_mmap.h（裸写未分配
    # 空区的封面缓存，见 partitions.csv），组件级依赖必须显式声明。
    PRIV_REQUIRES spi_flash
)
# 本组件采用紧凑单行风格；该警告只作提示，不阻断编译。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=misleading-indentation)
