#pragma once
#include <stddef.h>
#include <stdint.h>
#define LV_IMAGE_HEADER_MAGIC 0x19U
#define LV_COLOR_FORMAT_RGB565 18U
typedef struct {
    struct {unsigned magic,cf,flags,w,h,stride;} header;
    size_t data_size;const uint8_t *data;
} lv_image_dsc_t;
void lv_image_cache_drop(const void *image);
