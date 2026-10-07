#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PODCAST_POSITION_BYTES_PER_SECOND 32000U
typedef struct { uint32_t segment; uint64_t byte_offset; } podcast_position_cursor_t;

/* Lengths are complete, positive and even. End-of-episode uses the last segment
 * with offset == its length. All returned offsets lie on a 16-bit sample. */
bool podcast_position_absolute(const uint64_t *lengths, size_t count,
                               podcast_position_cursor_t cursor, uint64_t *absolute);
bool podcast_position_from_absolute(const uint64_t *lengths, size_t count,
                                    uint64_t absolute, podcast_position_cursor_t *cursor);
bool podcast_position_seek(const uint64_t *lengths, size_t count,
                           podcast_position_cursor_t cursor, int seconds,
                           podcast_position_cursor_t *target);
typedef enum {
    PODCAST_POSITION_INVALID = 0,
    PODCAST_POSITION_READY,
    PODCAST_POSITION_NEEDS_LENGTH,
} podcast_position_result_t;
/* Zero length means not yet fetched. Resolve only segments crossed by the seek,
 * reporting one required HEAD lookup at a time; no full-episode prefetch. */
podcast_position_result_t podcast_position_seek_partial(const uint64_t *lengths, size_t count,
    podcast_position_cursor_t cursor, int seconds, podcast_position_cursor_t *target,
    uint32_t *needed_segment);
uint64_t podcast_position_heard_offset(uint64_t start, uint64_t submitted,
                                      uint64_t dma_bytes);
bool podcast_position_range_valid(int status, const char *content_range,
                                  uint64_t requested_offset, uint64_t total,
                                  int64_t content_length);
