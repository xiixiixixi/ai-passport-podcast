#pragma once
#include "controller_stubs.h"
/* demo_podcast.c includes podcast_cover_dynamic.h, whose prototypes use the
 * LVGL image descriptor. Mirror the minimal type from podcast_cover_stubs. */
#define LV_IMAGE_HEADER_MAGIC 0x19U
#define LV_COLOR_FORMAT_RGB565 18U
typedef struct {
    struct {unsigned magic,cf,flags,w,h,stride;} header;
    size_t data_size;const uint8_t *data;
} lv_image_dsc_t;
