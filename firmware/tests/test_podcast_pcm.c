#include "podcast_pcm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    podcast_pcm_format_t format = {0};
    podcast_pcm_header(&format, "x-audio-sample-rate", "16000");
    podcast_pcm_header(&format, "X-Audio-Channels", "1");
    assert(!podcast_pcm_format_valid(&format, 32)); // Missing required header.
    podcast_pcm_header(&format, "X-Audio-Bits", "16");
    assert(!podcast_pcm_format_valid(&format, 32)); // Byte order is mandatory.
    podcast_pcm_header(&format, "X-Audio-Format", "s16le");
    assert(podcast_pcm_format_valid(&format, 32));
    assert(!podcast_pcm_format_valid(&format, 0));
    assert(!podcast_pcm_format_valid(&format, -1));
    assert(!podcast_pcm_format_valid(&format, 31));
    podcast_pcm_header(&format, "X-Audio-Bits", "16"); // Consistent duplicate.
    assert(podcast_pcm_format_valid(&format, 32));
    podcast_pcm_format_t valid = format;
    podcast_pcm_header(&format, "X-Audio-Format", "s16be");
    assert(!podcast_pcm_format_valid(&format, 32)); // Big-endian samples would distort.
    format = valid;
    podcast_pcm_header(&format, "X-Audio-Format", "f32le");
    assert(!podcast_pcm_format_valid(&format, 32)); // Float PCM is also rejected.
    format = valid;
    podcast_pcm_header(&format, "X-Audio-Channels", "2");
    assert(!podcast_pcm_format_valid(&format, 32));
    format = (podcast_pcm_format_t){0};
    podcast_pcm_header(&format, "X-Audio-Sample-Rate", "16000junk");
    podcast_pcm_header(&format, "X-Audio-Channels", "1");
    podcast_pcm_header(&format, "X-Audio-Bits", "16");
    podcast_pcm_header(&format, "X-Audio-Format", "s16le");
    assert(!podcast_pcm_format_valid(&format, 32));

    uint8_t input[4096], actual[4096], output[1026];
    for (size_t i = 0; i < sizeof(input); i++) input[i] = (uint8_t)(i * 17);
    const size_t boundaries[] = {1, 3, 511, 1024, 5, 2, 513};
    podcast_pcm_stream_t stream = {.expected = sizeof(input)};
    size_t read = 0, written = 0, turn = 0;
    while (read < sizeof(input)) {
        size_t n = boundaries[turn++ % (sizeof(boundaries)/sizeof(boundaries[0]))];
        if (n > sizeof(input) - read) n = sizeof(input) - read;
        size_t produced;
        assert(podcast_pcm_accept(&stream, input + read, n, output, sizeof(output), &produced));
        assert((produced & 1) == 0);
        memcpy(actual + written, output, produced);
        read += n; written += produced;
    }
    assert(written == sizeof(input) && memcmp(actual, input, sizeof(input)) == 0);
    assert(podcast_pcm_complete(&stream));
    size_t produced;
    assert(!podcast_pcm_accept(&stream, input, 1, output, sizeof(output), &produced));
    assert(podcast_pcm_complete(&stream)); // Overrun did not change the state.
    stream = (podcast_pcm_stream_t){.expected = 4};
    assert(!podcast_pcm_accept(&stream, input, 3, output, 2, &produced));
    assert(stream.received == 0);
    assert(podcast_pcm_accept(&stream, input, 3, output, sizeof(output), &produced));
    assert(produced == 2 && !podcast_pcm_complete(&stream)); // Truncated half-sample.
    assert(podcast_pcm_accept(&stream, input + 3, 1, output, sizeof(output), &produced));
    assert(produced == 2 && output[0] == input[2] && output[1] == input[3]);
    assert(podcast_pcm_complete(&stream));
    stream = (podcast_pcm_stream_t){.expected = 8, .received = 6};
    assert(!podcast_pcm_complete(&stream)); // Even-byte network truncation.

    // Reproduce the final-enqueue race: consumer's empty receive returns,
    // then the producer enqueues its tail and marks completion before inspection.
    stream = (podcast_pcm_stream_t){.expected = 8};
    assert(podcast_pcm_accept(&stream, input, 4, output, sizeof(output), &produced));
    memcpy(actual, output, produced);
    written = produced;
    assert(!podcast_pcm_end_ready(false, 0));
    assert(!podcast_pcm_end_ready(true, 4)); // The final queued bytes must drain.
    assert(podcast_pcm_accept(&stream, input + 4, 1, output, sizeof(output), &produced));
    assert(produced == 0 && !podcast_pcm_end_ready(true, 3));
    assert(podcast_pcm_accept(&stream, input + 5, 3, output, sizeof(output), &produced));
    memcpy(actual + written, output, produced);
    written += produced;
    assert(podcast_pcm_end_ready(true, 0));
    assert(podcast_pcm_complete(&stream) && written == 8 && memcmp(actual, input, 8) == 0);
    assert(podcast_pcm_duration_ms(32000) == 1000);
    assert(podcast_pcm_duration_ms(320000) == 10000);
    assert(podcast_pcm_duration_ms(16000) == 500);
    puts("podcast PCM: protocol, framing, tail-enqueue race, truncation and duration PASS");
    return 0;
}
