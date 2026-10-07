#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PODCAST_PCM_RATE 16000U
#define PODCAST_PCM_BYTES_PER_SECOND (PODCAST_PCM_RATE * 2U)

typedef struct {
    unsigned headers;
    bool invalid;
} podcast_pcm_format_t;

/* Unknown headers are ignored; required headers must be exact and consistent. */
void podcast_pcm_header(podcast_pcm_format_t *format, const char *key, const char *value);
bool podcast_pcm_format_valid(const podcast_pcm_format_t *format, int64_t length);

typedef struct {
    uint64_t expected;
    uint64_t received;
    uint8_t pending;
    bool has_pending;
} podcast_pcm_stream_t;

/* Reassemble 16-bit little-endian samples across arbitrary network boundaries.
 * Output needs size + 1 bytes; input and output must not overlap. Failure leaves
 * state unchanged. No allocation and no platform dependencies. */
bool podcast_pcm_accept(podcast_pcm_stream_t *stream, const uint8_t *input, size_t size,
                        uint8_t *output, size_t capacity, size_t *produced);
bool podcast_pcm_complete(const podcast_pcm_stream_t *stream);
/* An empty receive can race the producer's final enqueue. Only finish once
 * production has ended AND the queue is still empty. */
bool podcast_pcm_end_ready(bool producer_done, size_t queued_bytes);
uint64_t podcast_pcm_duration_ms(uint64_t bytes);
