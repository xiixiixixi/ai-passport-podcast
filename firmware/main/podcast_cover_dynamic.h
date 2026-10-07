#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "podcast_cover_wire.h"
#include "podcast_player.h"

/* Optional low-priority artwork worker. Failure preserves the placeholder
 * and must never prevent player startup. Covers are stored in the fixed
 * unallocated flash gap after `store` (see .c and partitions.csv) as the
 * backend's original 52px PDC1 wires, and the whole region is mapped once
 * via spi_flash_mmap and kept forever: descriptors point straight at flash
 * bytes, so visible artwork holds no pixel RAM in any player state. */
bool podcast_dynamic_covers_start(void);
bool podcast_dynamic_covers_stop(void);
/* UI/LVGL context only: publish the bounded visible ID set, evict off-page
 * slots, purge re-bound descriptors from the LVGL image cache, and report a
 * global revision for a targeted redraw. No network IO. */
void podcast_dynamic_covers_want(const char *const *ids,size_t count);
bool podcast_dynamic_covers_ui_poll(void);
uint32_t podcast_dynamic_covers_revision(void);
/* The returned descriptor (and ->data) is backed by immutable flash storage:
 * draw from it freely, never free or mutate it. */
const lv_image_dsc_t *podcast_dynamic_cover_get(const char *id);
uint32_t podcast_dynamic_cover_stamp(const char *id);
void podcast_dynamic_covers_clear_ui(void);
#ifndef ESP_PLATFORM
/* Host preview only: adopt an externally owned, validated sample wire through
 * the production registry. This performs no download and is absent on device. */
bool podcast_dynamic_covers_host_adopt(const char *id,const uint8_t *wire,size_t size);
#endif
/* Controller worker only, before START: place a short hold so in-flight
 * artwork downloads abort within one 512-byte read and return their transient
 * HTTP/staging heap to the start path. Visible covers keep displaying. */
void podcast_dynamic_covers_prepare_audio(void);
/* Host-testable gate for every artwork-worker activity (flash reads, adopts,
 * downloads). Only BUFFERING is forbidden — the start/seek window's stream
 * and transient heap must stay exclusive. Pixel storage is zero-RAM now, so
 * idle cover activity no longer competes with the first START. */
bool podcast_dynamic_cover_activity_allowed(podcast_player_state_t state, uint32_t buffered_ms);
