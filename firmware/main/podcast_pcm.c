#include "podcast_pcm.h"

#include <string.h>

static bool ascii_equal(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return *a == *b;
}

void podcast_pcm_header(podcast_pcm_format_t *format, const char *key, const char *value)
{
    unsigned bit = 0;
    const char *expected = NULL;
    if (ascii_equal(key, "X-Audio-Sample-Rate")) { bit = 1; expected = "16000"; }
    else if (ascii_equal(key, "X-Audio-Channels")) { bit = 2; expected = "1"; }
    else if (ascii_equal(key, "X-Audio-Bits")) { bit = 4; expected = "16"; }
    else if (ascii_equal(key, "X-Audio-Format")) { bit = 8; expected = "s16le"; }
    if (!bit) return;
    format->headers |= bit;
    if (!value || strcmp(value, expected) != 0) format->invalid = true;
}

bool podcast_pcm_format_valid(const podcast_pcm_format_t *format, int64_t length)
{
    return !format->invalid && format->headers == 15 && length > 0 && (length & 1) == 0;
}

bool podcast_pcm_accept(podcast_pcm_stream_t *stream, const uint8_t *input, size_t size,
                        uint8_t *output, size_t capacity, size_t *produced)
{
    *produced = 0;
    if (stream->received > stream->expected ||
        size > stream->expected - stream->received ||
        size > SIZE_MAX - (size_t)stream->has_pending ||
        capacity < size + (size_t)stream->has_pending) return false;
    size_t count = size + (size_t)stream->has_pending;
    if (stream->has_pending) output[0] = stream->pending;
    if (size) memcpy(output + (size_t)stream->has_pending, input, size);
    stream->received += size;
    stream->has_pending = (count & 1) != 0;
    if (stream->has_pending) stream->pending = output[count - 1];
    *produced = count & ~(size_t)1;
    return true;
}

bool podcast_pcm_complete(const podcast_pcm_stream_t *stream)
{
    return stream->received == stream->expected && !stream->has_pending;
}

bool podcast_pcm_end_ready(bool producer_done, size_t queued_bytes)
{
    return producer_done && queued_bytes == 0;
}

uint64_t podcast_pcm_duration_ms(uint64_t bytes)
{
    /* Avoid multiplying an arbitrary stream length by 1000. */
    return (bytes / PODCAST_PCM_BYTES_PER_SECOND) * 1000 +
           (bytes % PODCAST_PCM_BYTES_PER_SECOND) * 1000 / PODCAST_PCM_BYTES_PER_SECOND;
}
