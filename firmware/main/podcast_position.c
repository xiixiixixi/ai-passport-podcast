#include "podcast_position.h"
#include <limits.h>
#include <string.h>

static bool total_length(const uint64_t *lengths, size_t count, uint64_t *total)
{
    if (!lengths || !count) return false;
    *total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!lengths[i] || (lengths[i] & 1) || lengths[i] > UINT64_MAX - *total) return false;
        *total += lengths[i];
    }
    return true;
}

bool podcast_position_absolute(const uint64_t *lengths, size_t count,
                               podcast_position_cursor_t cursor, uint64_t *absolute)
{
    uint64_t total;
    if (!absolute || !total_length(lengths, count, &total) || cursor.segment >= count ||
        (cursor.byte_offset & 1) || cursor.byte_offset > lengths[cursor.segment]) return false;
    *absolute = cursor.byte_offset;
    for (size_t i = 0; i < cursor.segment; ++i) *absolute += lengths[i];
    return true;
}

bool podcast_position_from_absolute(const uint64_t *lengths, size_t count,
                                    uint64_t absolute, podcast_position_cursor_t *cursor)
{
    uint64_t total;
    if (!cursor || !total_length(lengths, count, &total)) return false;
    if (absolute > total) absolute = total;
    absolute &= ~(uint64_t)1;
    for (size_t i = 0; i < count; ++i) {
        if (absolute < lengths[i] || i == count - 1) {
            *cursor = (podcast_position_cursor_t){(uint32_t)i, absolute};
            return true;
        }
        absolute -= lengths[i];
    }
    return false;
}

bool podcast_position_seek(const uint64_t *lengths, size_t count,
                           podcast_position_cursor_t cursor, int seconds,
                           podcast_position_cursor_t *target)
{
    uint64_t absolute, total;
    if (!podcast_position_absolute(lengths, count, cursor, &absolute) ||
        !total_length(lengths, count, &total)) return false;
    uint64_t distance = (uint64_t)(seconds < 0 ? -(int64_t)seconds : seconds) *
                        PODCAST_POSITION_BYTES_PER_SECOND;
    if (seconds < 0) absolute = distance > absolute ? 0 : absolute - distance;
    else absolute = distance > total - absolute ? total : absolute + distance;
    return podcast_position_from_absolute(lengths, count, absolute, target);
}

uint64_t podcast_position_heard_offset(uint64_t start, uint64_t submitted,
                                      uint64_t dma_bytes)
{
    uint64_t heard = submitted > dma_bytes ? submitted - dma_bytes : 0;
    if (heard > UINT64_MAX - start) return UINT64_MAX & ~(uint64_t)1;
    return (start + heard) & ~(uint64_t)1;
}

podcast_position_result_t podcast_position_seek_partial(const uint64_t *lengths, size_t count,
    podcast_position_cursor_t cursor, int seconds, podcast_position_cursor_t *target,
    uint32_t *needed_segment)
{
    if (!lengths || !count || !target || !needed_segment || cursor.segment >= count ||
        (cursor.byte_offset & 1)) return PODCAST_POSITION_INVALID;
    uint64_t remaining = (uint64_t)(seconds < 0 ? -(int64_t)seconds : seconds) *
                         PODCAST_POSITION_BYTES_PER_SECOND;
    for (;;) {
        uint64_t length = lengths[cursor.segment];
        if (!length) { *needed_segment = cursor.segment; return PODCAST_POSITION_NEEDS_LENGTH; }
        if ((length & 1) || cursor.byte_offset > length) return PODCAST_POSITION_INVALID;
        if (seconds < 0) {
            if (remaining <= cursor.byte_offset) {
                cursor.byte_offset -= remaining;
                *target = cursor;
                return PODCAST_POSITION_READY;
            }
            remaining -= cursor.byte_offset;
            if (!cursor.segment) {
                *target = (podcast_position_cursor_t){0, 0};
                return PODCAST_POSITION_READY;
            }
            --cursor.segment;
            if (!lengths[cursor.segment]) {
                *needed_segment = cursor.segment;
                return PODCAST_POSITION_NEEDS_LENGTH;
            }
            cursor.byte_offset = lengths[cursor.segment];
        } else {
            uint64_t available = length - cursor.byte_offset;
            if (remaining < available || cursor.segment == count - 1) {
                cursor.byte_offset += remaining < available ? remaining : available;
                *target = cursor;
                return PODCAST_POSITION_READY;
            }
            remaining -= available;
            ++cursor.segment;
            cursor.byte_offset = 0;
            if (!remaining) { *target = cursor; return PODCAST_POSITION_READY; }
        }
    }
}

static bool number(const char **text, uint64_t *value)
{
    const char *p = *text;
    if (*p < '0' || *p > '9') return false;
    *value = 0;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p++ - '0');
        if (*value > (UINT64_MAX - digit) / 10) return false;
        *value = *value * 10 + digit;
    }
    *text = p;
    return true;
}

bool podcast_position_range_valid(int status, const char *content_range,
                                  uint64_t requested_offset, uint64_t total,
                                  int64_t content_length)
{
    if (!total || (total & 1) || (requested_offset & 1)) return false;
    if (status == 200)
        return requested_offset == 0 && (!content_range || !*content_range) &&
               content_length > 0 && (uint64_t)content_length == total;
    if (!content_range || strncmp(content_range, "bytes ", 6) != 0) return false;
    const char *p = content_range + 6;
    uint64_t start, end, advertised_total;
    if (status == 416) {
        if (strncmp(p, "*/", 2) != 0) return false;
        p += 2;
        return number(&p, &advertised_total) && !*p && advertised_total == total &&
               requested_offset >= total;
    }
    if (status != 206 || requested_offset >= total || content_length <= 0) return false;
    if (!number(&p, &start) || *p++ != '-' || !number(&p, &end) || *p++ != '/' ||
        !number(&p, &advertised_total) || *p) return false;
    return start == requested_offset && end == total - 1 && advertised_total == total &&
           (uint64_t)content_length == total - requested_offset;
}
