#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 24 records leave room for Wi-Fi settings and NVS copy-on-write collection
 * in this board's existing 24 KiB partition without moving stored data. */
#define PODCAST_BOOKMARK_SLOTS 24U
#define PODCAST_BOOKMARK_SHOW_CAP 24U
#define PODCAST_BOOKMARK_EPISODE_CAP 64U
#define PODCAST_BOOKMARK_TITLE_CAP 241U
#define PODCAST_BOOKMARK_NAME_CAP 65U
#define PODCAST_BOOKMARK_MAX_SEGMENT 100000U
#define PODCAST_BOOKMARK_MAX_BYTE_OFFSET UINT32_MAX

typedef struct {
    char show_id[PODCAST_BOOKMARK_SHOW_CAP];
    char episode_id[PODCAST_BOOKMARK_EPISODE_CAP];
    char title[PODCAST_BOOKMARK_TITLE_CAP];
    char name[PODCAST_BOOKMARK_NAME_CAP];
    uint32_t segment;
    uint64_t byte_offset;
    bool finished;
} podcast_bookmark_t;

typedef enum {
    PODCAST_BOOKMARK_OK = 0,
    PODCAST_BOOKMARK_NOT_FOUND,
    PODCAST_BOOKMARK_INVALID,
    PODCAST_BOOKMARK_STORAGE_ERROR,
} podcast_bookmark_result_t;

/* NVS must already be initialized by the application. Never erases NVS.
 * All storage calls are blocking; serialize them in the service worker, never
 * call them from button callbacks. No persistent heap or open handle is kept.
 * Read operations do not change recency. Saving updates recency, with identical
 * saves of the already-most-recent record skipped to limit Flash writes.
 * On failure, load/find leave their output unchanged. */
podcast_bookmark_result_t podcast_bookmarks_init(void);
podcast_bookmark_result_t podcast_bookmarks_load_recent(podcast_bookmark_t *record);
podcast_bookmark_result_t podcast_bookmarks_find(const char *show_id,
                                               const char *episode_id,
                                               podcast_bookmark_t *record);
/* Read-only bounded migration/export; physical slot order carries no recency claim. */
podcast_bookmark_result_t podcast_bookmarks_load_slot(unsigned slot,podcast_bookmark_t *record);
podcast_bookmark_result_t podcast_bookmarks_save(const podcast_bookmark_t *record);
podcast_bookmark_result_t podcast_bookmarks_save_volume(uint8_t volume);
podcast_bookmark_result_t podcast_bookmarks_load_volume(uint8_t *volume);

/* Platform-independent validation and UTF-8-safe text copying. Copy truncates
 * only at a complete Unicode scalar, zero-fills the destination, and returns
 * false for invalid UTF-8 encountered before truncation. IDs must fit in full;
 * do not truncate them. */
bool podcast_bookmark_valid(const podcast_bookmark_t *record);
bool podcast_bookmark_copy_text(char *destination, size_t capacity, const char *source);
