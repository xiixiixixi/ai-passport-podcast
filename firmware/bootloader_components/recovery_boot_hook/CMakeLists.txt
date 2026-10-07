idf_component_register(
    SRCS "recovery_boot_hook.c"
    PRIV_REQUIRES bootloader_support hal esp_rom
    PRIV_INCLUDE_DIRS "$ENV{IDF_PATH}/components/bootloader_support/private_include"
                      "${CMAKE_CURRENT_LIST_DIR}/../../components/bsp/include"
)
