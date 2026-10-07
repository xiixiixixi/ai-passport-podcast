#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PODCAST_SYNC_SLOTS 8
/* A durable bounded outbox. One record preserves both the newest observation
 * and an immutable in-flight request. All calls run on the catalogue worker. */
typedef enum { PODCAST_SYNC_PLAYING=1, PODCAST_SYNC_PAUSED,
               PODCAST_SYNC_STOPPED, PODCAST_SYNC_ENDED } podcast_sync_state_t;
typedef struct {
    bool used, offline, restart, stale, pending, seek;
    podcast_sync_state_t state, pending_state, acked_state;
    uint64_t request_id, base_revision, created_position_ms;
    uint64_t position_ms, heard_ms, acked_position_ms, acked_heard_ms;
    uint64_t elapsed_ms,pending_elapsed_ms,heard_origin_ms;
    uint64_t pending_position_ms, pending_heard_ms;
    uint32_t next_seq, pending_seq, audio_session;
    bool pending_seek;
    char session_id[33], show_id[24], episode_id[64];
} podcast_sync_record_t;
typedef struct {
    uint64_t client_seed, counter;
    podcast_sync_record_t records[PODCAST_SYNC_SLOTS];
    bool ready,clock_set;
    uint64_t clock_ms;
} podcast_sync_t;
/* Never erases storage. Corruption/write failures are surfaced to the caller.
 * Rebooted unclosed records become stopped; an existing pending body stays exact. */
bool podcast_sync_init(podcast_sync_t *sync, uint64_t random_seed);
void podcast_sync_clock(podcast_sync_t *sync,uint64_t monotonic_ms);
int podcast_sync_begin(podcast_sync_t *sync,const char *show,const char *episode,
                       uint64_t position_ms,uint64_t revision,bool offline,bool restart);
/* A resumed central session can reuse the audio generation without re-counting prior sound. */
bool podcast_sync_heard_origin(podcast_sync_t *sync,int at,uint64_t heard_ms);
bool podcast_sync_creation_body(const podcast_sync_t *sync,int at,char *body,size_t cap);
bool podcast_sync_opened(podcast_sync_t *sync,int at,const char *session,
                         uint64_t position_ms,uint32_t next_seq,bool stale);
void podcast_sync_observe(podcast_sync_t *sync,int at,uint32_t audio_session,
                          uint64_t position_ms,uint64_t heard_ms,podcast_sync_state_t state,bool seek);
bool podcast_sync_freeze(podcast_sync_t *sync,int at);
bool podcast_sync_event_body(const podcast_sync_t *sync,int at,char *body,size_t cap);
bool podcast_sync_acknowledge(podcast_sync_t *sync,int at,uint64_t revision);
/* Oldest unsynchronized record first. -1 means every current event is acknowledged. */
int podcast_sync_pending(const podcast_sync_t *sync);
bool podcast_sync_has_offline(const podcast_sync_t *sync,const char *show,const char *episode);
