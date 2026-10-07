#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Project implementation of the observed protocol-4 timing. No external
 * header is redistributed. Digital debounce is isolated from all board IO. */
typedef enum { PODCAST_BOOT_IDLE, PODCAST_BOOT_DOWN, PODCAST_BOOT_RELEASE_FILTER } podcast_boot_key_phase_t;
typedef struct {
    uint32_t boot_at, down_at, up_at;
    podcast_boot_key_phase_t phase;
} podcast_boot_key_t;
static inline int podcast_boot_key_sample(podcast_boot_key_t *key, uint32_t tick, bool down)
{
    if (down) {
        if (key->phase == PODCAST_BOOT_IDLE) key->down_at = tick;
        key->phase = PODCAST_BOOT_DOWN;
        return (uint32_t)(tick - key->down_at) >= 200 ? 1 : 0;
    }
    switch (key->phase) {
        case PODCAST_BOOT_DOWN:
            key->up_at = tick;
            key->phase = PODCAST_BOOT_RELEASE_FILTER;
            break;
        case PODCAST_BOOT_RELEASE_FILTER:
            if ((uint32_t)(tick - key->up_at) >= 50) key->phase = PODCAST_BOOT_IDLE;
            break;
        case PODCAST_BOOT_IDLE:
            break;
    }
    return key->phase == PODCAST_BOOT_IDLE && (uint32_t)(tick - key->boot_at) >= 1200 ? -1 : 0;
}
