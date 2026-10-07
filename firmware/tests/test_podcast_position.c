#include "podcast_position.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

#define BPS PODCAST_POSITION_BYTES_PER_SECOND

static void same(podcast_position_cursor_t cursor, uint32_t segment, uint64_t offset)
{ assert(cursor.segment == segment && cursor.byte_offset == offset); }

int main(void)
{
    uint64_t lengths[] = {300ULL * BPS, 300ULL * BPS, 20ULL * BPS};
    podcast_position_cursor_t target;
    uint64_t absolute;
    assert(podcast_position_seek(lengths, 3, (podcast_position_cursor_t){0, 295ULL * BPS}, 15, &target));
    same(target, 1, 10ULL * BPS);
    assert(podcast_position_seek(lengths, 3, target, -15, &target));
    same(target, 0, 295ULL * BPS);
    assert(podcast_position_seek(lengths, 3, (podcast_position_cursor_t){1, 10ULL * BPS}, -15, &target));
    same(target, 0, 295ULL * BPS);
    assert(podcast_position_seek(lengths, 3, (podcast_position_cursor_t){0, 0}, INT_MIN, &target));
    same(target, 0, 0);
    assert(podcast_position_seek(lengths, 3, target, INT_MAX, &target));
    same(target, 2, 20ULL * BPS);
    assert(podcast_position_absolute(lengths, 3, target, &absolute));
    assert(absolute == 620ULL * BPS);
    assert(podcast_position_from_absolute(lengths, 3, 300ULL * BPS, &target));
    same(target, 1, 0);
    assert(podcast_position_from_absolute(lengths, 3, 300ULL * BPS + 3, &target));
    same(target, 1, 2);
    assert(!podcast_position_absolute(lengths, 3, (podcast_position_cursor_t){0, 3}, &absolute));
    assert(!podcast_position_absolute(lengths, 3, (podcast_position_cursor_t){3, 0}, &absolute));
    assert(!podcast_position_absolute(lengths, 3, (podcast_position_cursor_t){2, 21ULL * BPS}, &absolute));
    uint64_t odd[] = {3}, overflow[] = {UINT64_MAX - 1, 2};
    assert(!podcast_position_from_absolute(odd, 1, 0, &target));
    assert(!podcast_position_from_absolute(overflow, 2, 0, &target));

    // A two-hour resume/seek does not need the lengths of its earlier segments.
    uint64_t partial[32] = {0};
    partial[24] = 300ULL * BPS;
    uint32_t needed;
    podcast_position_cursor_t base = {24, 100ULL * BPS};
    assert(podcast_position_seek_partial(partial, 32, base, 15, &target, &needed) == PODCAST_POSITION_READY);
    same(target, 24, 115ULL * BPS);
    base.byte_offset = 295ULL * BPS;
    assert(podcast_position_seek_partial(partial, 32, base, 15, &target, &needed) == PODCAST_POSITION_NEEDS_LENGTH);
    assert(needed == 25); // Only the crossed segment, never segments 0..23.
    partial[25] = 300ULL * BPS;
    assert(podcast_position_seek_partial(partial, 32, base, 15, &target, &needed) == PODCAST_POSITION_READY);
    same(target, 25, 10ULL * BPS);
    assert(podcast_position_seek_partial(partial, 32, target, -15, &target, &needed) == PODCAST_POSITION_READY);
    same(target, 24, 295ULL * BPS);
    base = (podcast_position_cursor_t){24, 10ULL * BPS};
    assert(podcast_position_seek_partial(partial, 32, base, -15, &target, &needed) == PODCAST_POSITION_NEEDS_LENGTH);
    assert(needed == 23);
    partial[23] = 270ULL * BPS; // Nonstandard segment lengths are mapped exactly.
    assert(podcast_position_seek_partial(partial, 32, base, -15, &target, &needed) == PODCAST_POSITION_READY);
    same(target, 23, 265ULL * BPS);
    base = (podcast_position_cursor_t){24, 285ULL * BPS};
    partial[25] = 0;
    assert(podcast_position_seek_partial(partial, 32, base, 15, &target, &needed) == PODCAST_POSITION_READY);
    same(target, 25, 0); // Exact boundary needs no speculative next HEAD.

    // Pause cursor is based on submitted samples minus queued DMA, not downloaded bytes.
    uint64_t submitted = 10ULL * BPS, dma = 7680;
    uint64_t paused = podcast_position_heard_offset(5ULL * BPS, submitted, dma);
    assert(paused == 15ULL * BPS - dma);
    assert(podcast_position_heard_offset(paused, 0, dma) == paused); // Resume starts there.
    assert(podcast_position_heard_offset(4, 100, dma) == 4);
    assert(podcast_position_heard_offset(UINT64_MAX - 3, 100, 0) == UINT64_MAX - 1);

    assert(podcast_position_range_valid(200, "", 0, 9600000, 9600000));
    assert(!podcast_position_range_valid(200, "", 32000, 9600000, 9600000)); // Ignored Range.
    assert(podcast_position_range_valid(206, "bytes 32000-9599999/9600000", 32000, 9600000, 9568000));
    assert(!podcast_position_range_valid(206, NULL, 32000, 9600000, 9568000));
    assert(!podcast_position_range_valid(206, "bytes 0-9599999/9600000", 32000, 9600000, 9568000));
    assert(!podcast_position_range_valid(206, "bytes 32000-9599998/9600000", 32000, 9600000, 9568000));
    assert(!podcast_position_range_valid(206, "bytes 32000-9599999/9600002", 32000, 9600000, 9568000));
    assert(!podcast_position_range_valid(206, "bytes 32000-9599999/9600000 junk", 32000, 9600000, 9568000));
    assert(!podcast_position_range_valid(206, "bytes 32000-9599999/9600000", 32000, 9600000, 9567998));
    assert(!podcast_position_range_valid(206, "bytes 3-9599999/9600000", 3, 9600000, 9599997));
    assert(!podcast_position_range_valid(206, "bytes 184467440737095516160-9599999/9600000", 0, 9600000, 9600000));
    assert(podcast_position_range_valid(416, "bytes */9600000", 9600000, 9600000, 0));
    assert(!podcast_position_range_valid(416, "bytes */9600000", 32000, 9600000, 0));
    assert(!podcast_position_range_valid(416, "bytes */9600002", 9600000, 9600000, 0));
    puts("Podcast positions: cross-segment seek, bounded HEAD, pause cursor and strict Range PASS");
    return 0;
}
