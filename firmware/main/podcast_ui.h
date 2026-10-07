#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PODCAST_VISIBLE_ROWS 3
#define PODCAST_TITLE_BYTES 241
typedef enum { PODCAST_SHOWS, PODCAST_EPISODES, PODCAST_NOW, PODCAST_ACTIONS,
               PODCAST_VOLUME, PODCAST_SLEEP, PODCAST_SEEK } podcast_page_t;
typedef struct {
    char show_id[24];
    char title[PODCAST_TITLE_BYTES];
    char detail[80];
    char latest_date[11];
} podcast_ui_row_t;
typedef struct {
    uint32_t revision;
    podcast_page_t page;
    char show_id[24];
    char episode_id[64];
    uint32_t playback_session_floor;
    bool waiting_for_session;
    char heading[80], status[80], show[65], title[PODCAST_TITLE_BYTES];
    char hint[80], back_hint[80];
    char next_title[PODCAST_TITLE_BYTES];
    podcast_ui_row_t rows[PODCAST_VISIBLE_ROWS];
    int count, selected, total, absolute_selected, battery_percent;
    unsigned elapsed, duration, volume, sleep_minutes;
    unsigned seek_target;
    int next_position, next_total;
    /* In NOW this is the frozen playback sequence, not a browsing preference. */
    bool oldest_first, has_next, auto_next;
    bool playing, busy, resuming;
    bool has_recent, recent_is_current;
    char recent_show_id[24], recent_name[65], recent_title[PODCAST_TITLE_BYTES];
    unsigned recent_elapsed, recent_duration;
} podcast_view_t;

/* Caller holds the LVGL lock, or runs in the LVGL timer context. */
void podcast_ui_create(void);
void podcast_ui_render(const podcast_view_t *view);
void podcast_ui_delete(void);

typedef struct {
    unsigned labels, characters, chinese, missing, invalid_utf8, invisible, wrong_font;
    uint32_t first_missing;
} podcast_ui_text_report_t;

/* Inspect actual visible labels in their current state, without drawing or IO. */
bool podcast_ui_inspect_text(podcast_ui_text_report_t *report);
