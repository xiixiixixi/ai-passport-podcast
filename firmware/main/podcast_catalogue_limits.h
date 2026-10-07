#pragma once

/* Keep catalogue RAM bounded; episodes are still fetched three at a time.
 * 32 is a hard RAM ceiling on ESP32-C3: shows[] plus its fetch scratch keep
 * 136 bytes per show (~4.3KB more at 48), and that permanently shrinks the
 * single 32KB contiguous block the first audio START must allocate. */
#define PODCAST_MAX_SHOWS 32
#define PODCAST_SHOW_PAGE_SIZE 8
#define PODCAST_MAX_JSON_BYTES 8192
