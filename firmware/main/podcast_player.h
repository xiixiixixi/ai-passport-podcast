#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "podcast_position.h"

typedef podcast_position_cursor_t podcast_player_cursor_t;
typedef enum {
    PODCAST_PLAYER_IDLE = 0,
    PODCAST_PLAYER_BUFFERING,
    PODCAST_PLAYER_PLAYING,
    PODCAST_PLAYER_PAUSED,
    PODCAST_PLAYER_FINISHED,
    PODCAST_PLAYER_ERROR,
} podcast_player_state_t;

typedef struct {
    podcast_player_state_t state;
    char show_id[24];
    char episode_id[64]; /* Immutable playback identity for safe checkpointing. */
    podcast_player_cursor_t cursor;
    uint64_t elapsed_seconds; /* Exact PCM duration once the length table is loaded. */
    uint32_t closed_session_id;
    uint64_t closed_elapsed_ms, closed_heard_ms; /* Final heard position before STOP/START replaces a session. */
    uint64_t elapsed_ms, heard_ms; /* Exact PCM position and heard audio, excluding seeks and queued DMA. */
    uint64_t total_seconds; /* Exact after the one whole-stream HEAD response. */
    uint32_t segment_count;
    uint32_t absolute_seek_id; /* Changes only after a central absolute position is actually applied. */
    uint32_t session_id; /* Increments per dispatched start; unchanged by seek/resume. */
    uint32_t completion_id; /* Increments only when a new whole-episode FINISHED is published. */
    uint8_t volume;
    uint32_t buffered_ms; /* Network queue only; excludes bounded hardware DMA. */
    uint32_t underruns; /* Recoverable queue exhaustion events in this episode. */
    uint32_t segments_crossed; /* Heard position crossing physical cache segments. */
    const char *error; /* Static Chinese text; NULL unless an error occurred. */
} podcast_player_snapshot_t;

/* Initialize once after networking setup; base URL example: http://host:8899.
 * Copies URL. No network I/O here. Persistent worker owns every audio operation. */
bool podcast_player_init(const char *relay_base_url);
/* Commands copy identity/cursor and never block UI/input. false means invalid
 * arguments, unavailable worker, or full command queue. IDs fit 23/63 bytes.
 * segment_count is authoritative (1..256), rather than guessing via HTTP 404.
 * Relay must expose a complete stream.pcm with consistent segment-length headers.
 * One bounded queue and one GET cover every segment; segment boundaries do not
 * close the audio path or clear buffered samples. */
bool podcast_player_start(const char *show_id, const char *episode_id,
                          podcast_player_cursor_t cursor, uint32_t segment_count);
bool podcast_player_pause(void);
bool podcast_player_resume(void);
bool podcast_player_seek_relative(int seconds);
/* Central resume uses actual complete length metadata, independent of cache segment size. */
bool podcast_player_seek_to_ms(uint64_t position_ms);
bool podcast_player_stop(void);
bool podcast_player_set_volume(uint8_t percent);
/* Relative commands preserve every ordered key press; worker clamps to 0..100.
 * delta must be -100..100. Never blocks and performs no I/O in the caller. */
bool podcast_player_adjust_volume(int delta);
/* Both functions are safe from UI/input and perform no blocking I/O. Worker
 * reaps its producers itself. Pause may conservatively replay <=240ms on resume. */
void podcast_player_poll(void);
bool podcast_player_snapshot(podcast_player_snapshot_t *snapshot);

/* 起播窗口进行中（计划打开音源到出声/失败为止）：低优先级后台活动应避让。 */
bool podcast_player_start_pending(void);
