**简体中文** · [English（英文）](README.md)

# 固件依赖许可证与版权声明

本目录保留实际依赖原文，不把它们改成工程根目录的 MIT（宽松开源许可）。文件来自 ESP-IDF（乐鑫开发库）5.5.3、`firmware/dependencies.lock`（依赖锁定清单）中的组件，以及链接记录所用的 GCC（编译器）14.2.0工具链。独立许可证文件逐字复制；标为 SOURCE-NOTICE（源码版权声明）的文件逐字摘录源码头部，未复制实现代码。

Apache 2.0（阿帕奇许可）第4节要求分发时附许可证并保留适用声明；MIT（宽松开源许可）、下列 BSD（宽松再分发许可）及 OFL（开放字体许可）原文也规定了保留许可／版权声明。以下文件随源码包和固件安装包一同保留。

| 实际材料 | 版本／来源 | 保留原文 |
| --- | --- | --- |
| ESP-IDF（乐鑫开发库），含 I2S（音频连接）驱动 | 5.5.3 | [Apache 2.0（阿帕奇许可）](esp-idf/LICENSE)、[驱动声明](esp-idf/I2S-SOURCE-NOTICE.txt) |
| 无线、射频与共存库 | ESP-IDF（乐鑫开发库）5.5.3随附库 | [无线许可](esp-idf/esp_wifi.LICENSE)、[射频许可](esp-idf/esp_phy.LICENSE)、[共存许可](esp-idf/esp_coex.LICENSE) |
| 按键驱动与界面移植层 | button（按键）4.2.0、esp_lvgl_port（界面移植层）2.9.0 | 源码标注 Apache-2.0（阿帕奇许可），采用上面的完整许可文本；保留[按键声明](espressif/button-4.2.0-SOURCE-NOTICE.txt)和[界面移植声明](espressif/esp_lvgl_port-2.9.0-SOURCE-NOTICE.txt) |
| 音频设备库与 ES8311（音频芯片）驱动 | esp_codec_dev（音频设备库）1.6.2 | [许可](espressif/esp_codec_dev-1.6.2.LICENSE)、[芯片驱动声明](espressif/esp_codec_dev-1.6.2-SOURCE-NOTICE.txt) |
| LVGL（界面库） | 9.5.0 | [MIT（宽松开源许可）](lvgl/LICENCE.txt) |
| LVGL（界面库）内置内存管理与文字格式化 | TLSF（内存管理）3.1、printf（文字格式化） | [内存管理原说明](lvgl/LICENSE_TLSF.txt)、[完整源码许可声明](lvgl/TLSF-SOURCE-NOTICE.txt)、[文字格式化许可](lvgl/LICENSE_SPRINTF.txt) |
| LVGL（界面库）内置西文字体与图标字形 | Montserrat（蒙特塞拉特字库）、Font Awesome 5（图标字库） | [Montserrat（蒙特塞拉特字库）许可](lvgl-fonts/Montserrat-OFL.txt)、[Font Awesome（图标字库）许可](lvgl-fonts/FontAwesome5-LICENSE.txt)；生成字库头部标明这两个来源 |
| FreeRTOS（任务系统） | ESP-IDF（乐鑫开发库）随附 V10.5.1修改版 | [MIT（宽松开源许可）](freertos/LICENSE.md)、[源码版权声明](freertos/SOURCE-NOTICE.txt) |
| lwIP（网络协议库） | ESP-IDF（乐鑫开发库）随附版本 | [BSD（宽松再分发许可）与版权](lwip/COPYING) |
| cJSON（数据解析库）与 HTTP（网络请求）解析库 | ESP-IDF（乐鑫开发库）随附版本 | [cJSON（数据解析库）许可](cjson/LICENSE)、[HTTP（网络请求）解析许可](http_parser/LICENSE.txt) |
| Mbed TLS（加密库） | ESP-IDF（乐鑫开发库）随附版本 | [双许可原文](mbedtls/LICENSE)、[版权声明](mbedtls/SOURCE-NOTICE.txt)；本分发采用其 Apache-2.0（阿帕奇许可）选项 |
| Newlib（C基础运行库） | SDK（开发库）所附合集声明 | [逐来源版权与许可原文](newlib/COPYING.NEWLIB) |
| wpa_supplicant（无线认证程序） | ESP-IDF（乐鑫开发库）随附版本 | [版权说明](wpa_supplicant/COPYING)、[含完整BSD（宽松再分发许可）的原说明](wpa_supplicant/README) |
| GCC（编译器）运行库 | 14.2.0，工具链发布 esp-14.2.0_20251107 | [GPLv3（通用公共许可第三版）](gcc/COPYING3)、[运行库例外3.1](gcc/COPYING.RUNTIME)、[源码声明](gcc/SOURCE-NOTICE.txt)；例外原文单独保留，不与 GPL（通用公共许可）合并改写 |

链接记录用于区分实际库和仅在依赖目录中存在的可选材料；未把禁用的 LVGL（界面库）图片解码器、其他芯片驱动或整个开发工具目录加入公开包。`cmake_utilities`（构建辅助工具）1.1.1是锁定的构建依赖，本地组件没有独立许可证文件；本目录不据它的测试文件声明推断整个组件许可，也不附带该依赖的实现目录。

上游来源：[ESP-IDF（乐鑫开发库）5.5.3](https://github.com/espressif/esp-idf/tree/v5.5.3)、[LVGL（界面库）锁定提交](https://github.com/lvgl/lvgl/tree/85aa60d18b3d5e5588d7b247abf90198f07c8a63)、[音频设备库锁定提交](https://github.com/espressif/esp-adf/tree/af7b72fb2b73d4f513d3cede01c13518ab216735/components/esp_codec_dev)、[按键库锁定提交](https://github.com/espressif/esp-iot-solution/tree/5f9cb98ae4d0e8153c4b4d1accf471214e5b6fe8/components/button)、[界面移植层锁定提交](https://github.com/espressif/esp-bsp/tree/f0ef9497efce684997ce391edd19733483e250a5/components/esp_lvgl_port)。这些版本来自锁定组件自带的上游元数据；原文具体位置见表中保留文件。

本目录补充依赖材料。原板级代码、项目代码、中文字体、网页图标与音频解码器的许可位置仍见[第三方材料说明](../third-party.zh_CN.md)。以后改变构建依赖时，应按新的锁定版本和实际链接材料更新本索引。
