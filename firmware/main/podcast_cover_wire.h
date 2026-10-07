#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PODCAST_COVER_SIDE 52U
#define PODCAST_COVER_NATIVE_SIDE 48U
#define PODCAST_COVER_HEADER_BYTES 32U
#define PODCAST_COVER_PIXEL_BYTES (PODCAST_COVER_SIDE * PODCAST_COVER_SIDE * 2U)
#define PODCAST_COVER_WIRE_BYTES (PODCAST_COVER_HEADER_BYTES + PODCAST_COVER_PIXEL_BYTES)
#define PODCAST_COVER_SLOTS 3U

/* PDC1: LE width/height at 4/6, LE pixel length at 8, IEEE CRC32 of
 * pixels at 12, first 16 bytes of SHA256(pixels) at 16; pixels start at 32.
 * No dimensions, allocations or remote URLs are trusted from the response. */
bool podcast_cover_wire_valid(const uint8_t *wire, size_t size);
bool podcast_cover_id_valid(const char *id);
/* After validation only: shrink to native 48px in place, forward safely.
 * The immutable content version still identifies the original 52px payload.
 * This avoids scaled-image temporary allocations in the 24KiB LVGL pool. */
void podcast_cover_native_pixels(uint8_t *wire);
