[简体中文](README.zh_CN.md) · **English**

# Firmware dependency licenses and notices

These files preserve the actual dependencies' terms rather than applying the project's root MIT license to them. They come from ESP-IDF 5.5.3, the components in `firmware/dependencies.lock`, and the GCC 14.2.0 toolchain recorded by the linker. Standalone license files are copied verbatim. Files named SOURCE-NOTICE are verbatim source-header notices, without implementation code.

Apache 2.0 section 4 requires a license copy and retention of applicable notices when distributing. The MIT, BSD and OFL texts below also specify retention of licenses/copyright notices. These files accompany both source and firmware installation bundles.

| Actual material | Version/source | Retained text |
| --- | --- | --- |
| ESP-IDF, including I2S driver | 5.5.3 | [Apache 2.0](esp-idf/LICENSE), [driver notice](esp-idf/I2S-SOURCE-NOTICE.txt) |
| Wi-Fi, PHY and coexistence libraries | Bundled with ESP-IDF 5.5.3 | [Wi-Fi license](esp-idf/esp_wifi.LICENSE), [PHY license](esp-idf/esp_phy.LICENSE), [coexistence license](esp-idf/esp_coex.LICENSE) |
| Button driver and LVGL port | button 4.2.0, esp_lvgl_port 2.9.0 | Source SPDX identifiers specify Apache-2.0; the complete text above applies. Retained [button notice](espressif/button-4.2.0-SOURCE-NOTICE.txt) and [LVGL port notice](espressif/esp_lvgl_port-2.9.0-SOURCE-NOTICE.txt) |
| Codec device library and ES8311 driver | esp_codec_dev 1.6.2 | [License](espressif/esp_codec_dev-1.6.2.LICENSE), [driver notice](espressif/esp_codec_dev-1.6.2-SOURCE-NOTICE.txt) |
| LVGL | 9.5.0 | [MIT](lvgl/LICENCE.txt) |
| LVGL built-in allocator and formatting | TLSF 3.1, printf | [Original allocator note](lvgl/LICENSE_TLSF.txt), [complete source license notice](lvgl/TLSF-SOURCE-NOTICE.txt), [formatting license](lvgl/LICENSE_SPRINTF.txt) |
| LVGL built-in Latin fonts and icon glyphs | Montserrat, Font Awesome 5 | [Montserrat OFL](lvgl-fonts/Montserrat-OFL.txt), [Font Awesome license](lvgl-fonts/FontAwesome5-LICENSE.txt); generated-font headers identify both inputs |
| FreeRTOS | ESP-IDF's modified V10.5.1 | [MIT](freertos/LICENSE.md), [source copyright notice](freertos/SOURCE-NOTICE.txt) |
| lwIP | ESP-IDF bundled version | [BSD terms and copyright](lwip/COPYING) |
| cJSON and HTTP parser | ESP-IDF bundled versions | [cJSON license](cjson/LICENSE), [HTTP parser license](http_parser/LICENSE.txt) |
| Mbed TLS | ESP-IDF bundled version | [Original dual-license text](mbedtls/LICENSE), [copyright notice](mbedtls/SOURCE-NOTICE.txt); this distribution uses the Apache-2.0 option |
| Newlib | SDK-supplied collection notice | [Per-source notices and terms](newlib/COPYING.NEWLIB) |
| wpa_supplicant | ESP-IDF bundled version | [Copyright statement](wpa_supplicant/COPYING), [original README containing complete BSD terms](wpa_supplicant/README) |
| GCC runtime libraries | 14.2.0, esp-14.2.0_20251107 toolchain release | [GPLv3](gcc/COPYING3), [Runtime Library Exception 3.1](gcc/COPYING.RUNTIME), [source notice](gcc/SOURCE-NOTICE.txt); the exception is retained separately without rewriting its terms |

Link records distinguish actual libraries from optional materials merely present in dependency directories. Disabled LVGL image decoders, other chip drivers and whole development-tool directories are not added to the public bundle. Locked `cmake_utilities` 1.1.1 is a build dependency with no standalone license file in the local component. This index does not infer its entire component's terms from its test-file notices, and its implementation directory is not bundled.

Upstream provenance: [ESP-IDF 5.5.3](https://github.com/espressif/esp-idf/tree/v5.5.3), [locked LVGL commit](https://github.com/lvgl/lvgl/tree/85aa60d18b3d5e5588d7b247abf90198f07c8a63), [locked codec-device commit](https://github.com/espressif/esp-adf/tree/af7b72fb2b73d4f513d3cede01c13518ab216735/components/esp_codec_dev), [locked button commit](https://github.com/espressif/esp-iot-solution/tree/5f9cb98ae4d0e8153c4b4d1accf471214e5b6fe8/components/button), and [locked LVGL-port commit](https://github.com/espressif/esp-bsp/tree/f0ef9497efce684997ce391edd19733483e250a5/components/esp_lvgl_port). These identifiers come from the installed locked components' upstream metadata; retained texts are linked in the table.

This directory supplements dependency notices. Project/board code, Chinese fonts, browser icons and the audio decoder remain documented in [Third-party materials](../third-party.md). Update this index against the new lockfile and linked materials whenever build dependencies change.
