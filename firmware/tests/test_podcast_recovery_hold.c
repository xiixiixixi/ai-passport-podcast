#include "../bootloader_components/recovery_boot_hook/podcast_boot_key.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

int main(void)
{
    podcast_boot_key_t h = {.boot_at = 0};
    assert(podcast_boot_key_sample(&h, 0, false) == 0);
    assert(podcast_boot_key_sample(&h, 1199, false) == 0);
    assert(podcast_boot_key_sample(&h, 1200, false) == -1);
    h = (podcast_boot_key_t){.boot_at = 0};
    assert(podcast_boot_key_sample(&h, 10, true) == 0);
    assert(podcast_boot_key_sample(&h, 209, true) == 0);
    assert(podcast_boot_key_sample(&h, 210, true) == 1);
    h = (podcast_boot_key_t){.boot_at = 0};
    assert(podcast_boot_key_sample(&h, 0, true) == 0);
    assert(podcast_boot_key_sample(&h, 80, false) == 0);
    assert(podcast_boot_key_sample(&h, 129, true) == 0);
    assert(podcast_boot_key_sample(&h, 200, true) == 1);
    h = (podcast_boot_key_t){.boot_at = 0};
    assert(podcast_boot_key_sample(&h, 0, true) == 0);
    assert(podcast_boot_key_sample(&h, 80, false) == 0);
    assert(podcast_boot_key_sample(&h, 130, false) == 0 && h.phase == PODCAST_BOOT_IDLE);
    assert(podcast_boot_key_sample(&h, 150, true) == 0);
    assert(podcast_boot_key_sample(&h, 349, true) == 0);
    assert(podcast_boot_key_sample(&h, 350, true) == 1);
    h = (podcast_boot_key_t){.boot_at = UINT32_MAX - 100};
    assert(podcast_boot_key_sample(&h, UINT32_MAX - 100, true) == 0);
    assert(podcast_boot_key_sample(&h, 98, true) == 0);
    assert(podcast_boot_key_sample(&h, 99, true) == 1);
    puts("Protocol-4 boot sampling: 200ms press, 50ms release debounce, 1200ms normal boot, timer wrap preserved PASS");
}
