#pragma once
#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A glyph keeps LVGL's exact packed 4bpp pixels and all original metrics.
 * With LV_FONT_FMT_TXT_LARGE, its otherwise-unused index high bit marks a
 * canonical byte-Huffman stream; the low 31 bits remain the byte offset. */
#define PODCAST_FONT_HUFFMAN_FLAG UINT32_C(0x80000000)
#define PODCAST_FONT_OFFSET_MASK UINT32_C(0x7fffffff)
#define PODCAST_FONT_RAW_LIMIT 512u

typedef struct {
    uint32_t glyph_count, bitmap_bytes;
    uint16_t symbol_count;
    uint8_t max_code_bits;
    const uint16_t *counts, *first_codes, *first_symbols;
    const uint8_t *symbols;
} podcast_font_codec_t;

/* Bounded raw reconstruction for probes and codec verification. No heap. */
bool podcast_font_decode_packed(const podcast_font_codec_t *codec,
    const uint8_t *input,size_t input_bytes,bool compressed,
    uint8_t *output,size_t output_bytes);

/* Caller holds LVGL's existing lock. req_raw returns one shared 512-byte
 * workspace, valid until the next raw request. Normal draws do not touch it:
 * bytes are decoded directly to the caller's existing A8 buffer. */
const void *podcast_font_get_bitmap_lossless(lv_font_glyph_dsc_t *glyph,
                                            lv_draw_buf_t *draw_buf);
