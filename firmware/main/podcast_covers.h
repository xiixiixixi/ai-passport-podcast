#pragma once
#include <stdbool.h>
#include "lvgl.h"

/* Native-size RGB565 artwork is immutable and lives in flash. No IO or scaling.
 * Stable show IDs select the correct artwork; unknown IDs use a neutral tile. */
const lv_image_dsc_t *podcast_cover_get(const char *show_id, bool thumbnail);
/* Dynamic images are native 48px RGB565. Only the LVGL context may borrow
 * them; adoption/eviction also occurs there between frames, never in HTTP. */
const lv_image_dsc_t *podcast_dynamic_cover_get(const char *show_id);
uint32_t podcast_dynamic_cover_stamp(const char *show_id);
void podcast_dynamic_covers_clear_ui(void);
