/* Independently implemented modern permanent-Recovery interface, observed in
 * SHLcy's installer, pinned
 * at 127c56bc97b7ea85bf4bfd2b52ce4e2d80a30566. Runtime behavior and exact layout
 * are documented in docs/podcast-recovery.md. No Recovery image is bundled. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bootloader_common.h"
#include "bootloader_config.h"
#include "bootloader_utility.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "podcast_boot_key.h"
#define BSP_BOOTLOADER_ONLY
#include "bsp_pins.h"

/* The private header also imports application-only SPI APIs; use the actual
 * ESP-IDF 5.5.3 bootloader function signature without those dependencies. */
extern esp_err_t bootloader_flash_read(size_t, void *, size_t, bool);
enum { RECOVERY_OFFSET = 0x6c0000, RECOVERY_BYTES = 0x140000,
       APPLICATION_OFFSET = 0x10000, INSTALL_MARKER_OFFSET = 0x6b8000 };
static const char *TAG = "podcast_recovery";
void bootloader_hooks_include(void) {}
static bool recovery_available(void)
{
    uint32_t header;
    return bootloader_flash_read(RECOVERY_OFFSET, &header, sizeof(header), true) == ESP_OK && (header & 0xff) == 0xe9;
}
static void start_recovery(void)
{
    if (!recovery_available()) {
        ESP_LOGW(TAG, "Permanent Recovery missing; initialize the official installer over USB first");
        return; /* Do not jump into a blank region on a USB development board. */
    }
    bootloader_state_t state = {0}; state.factory.offset = RECOVERY_OFFSET; state.factory.size = RECOVERY_BYTES;
    bootloader_utility_load_boot_image(&state, FACTORY_INDEX);
}
void bootloader_after_init(void)
{
    uint32_t value;
    if (bootloader_flash_read(INSTALL_MARKER_OFFSET, &value, sizeof(value), true) == ESP_OK && value != UINT32_MAX) {
        ESP_LOGI(TAG, "Interrupted installation: requesting permanent Recovery"); start_recovery();
    }
    if (bootloader_flash_read(APPLICATION_OFFSET, &value, sizeof(value), true) == ESP_OK && (value & 0xff) != 0xe9) {
        ESP_LOGI(TAG, "Application missing: requesting permanent Recovery"); start_recovery();
    }
    esp_rom_gpio_pad_select_gpio(BSP_BTN_ADC_GPIO); gpio_ll_input_enable(&GPIO, BSP_BTN_ADC_GPIO);
    gpio_ll_pullup_dis(&GPIO, BSP_BTN_ADC_GPIO); gpio_ll_pulldown_dis(&GPIO, BSP_BTN_ADC_GPIO);
    podcast_boot_key_t key = {.boot_at = esp_log_early_timestamp()};
    int decision;
    do {
        decision = podcast_boot_key_sample(&key, esp_log_early_timestamp(), gpio_ll_get_level(&GPIO, BSP_BTN_ADC_GPIO) == 0);
        esp_rom_delay_us(10000);
    } while (!decision);
    if (decision == 1) { ESP_LOGI(TAG, "Boot key held: requesting permanent Recovery"); start_recovery(); }
}
