#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "player_stubs/podcast_player_test_adapter.h"

static bool fail_next_allocation, producer_ack = true, ignored_range;
static unsigned restarts;
static unsigned deleted_tasks, http_live, head_calls, get_calls;
static unsigned authenticated_heads, authenticated_gets;
static uint64_t range_requested;
static TickType_t waited;
static uint64_t fixture_lengths[256];
static uint32_t fixture_count;
static bool pump_audio, jitter_injected;
static uint64_t jitter_at, cancel_at, truncate_at;
static TickType_t jitter_ms;
static uint64_t bytes_written, expected_audio_start;
static unsigned sleeps, wakes, format_calls;
static bool observe_eof;
static uint32_t completion_baseline;
static uint64_t audio_clock_remainder;
static uint8_t captured_audio[320000];
static void consume_source(void);
static uint8_t sample_byte(uint64_t at) { return (uint8_t)(at * 37U + (at >> 8)); }
static void *test_calloc(size_t count, size_t size)
{
    if (fail_next_allocation) { fail_next_allocation = false; return NULL; }
    return calloc(count, size);
}
#define PODCAST_PLAYER_TEST
#define calloc test_calloc
#include "../main/podcast_player.c"
#undef calloc

const char *podcast_config_authorization(void)
{ return "Bearer aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"; }

struct test_player_task { bool acknowledged; };
struct test_player_semaphore { unsigned count; };
struct test_player_queue { size_t item_size; unsigned count; uint8_t items[8][256]; };
struct test_player_buffer { size_t capacity, count; uint8_t bytes[32768]; };
struct test_player_http {
    esp_http_client_config_t config;
    char url[384];
    uint64_t offset, total, read_at;
    int status;
};

void test_player_log(const char *tag, const char *format, ...)
{ (void)tag; (void)format; }
void esp_restart(void) { ++restarts; }
BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *task)
{
    assert(entry && priority == 4);
    bool producer = strcmp(name, "podcast_http") == 0;
    assert(stack == (producer ? 4096U : 6144U));
    *task = malloc(sizeof(**task));
    (*task)->acknowledged = producer && producer_ack;
    if (producer && producer_ack) ((player_source_t *)arg)->finished->count = 1;
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task)
{ assert(task && task->acknowledged); ++deleted_tasks; free(task); }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 1024; }
void vTaskSuspend(TaskHandle_t task) { (void)task; }
void vTaskDelay(TickType_t ticks) { waited += ticks; }
QueueHandle_t xQueueCreate(unsigned depth, size_t item_size)
{
    assert(depth == 8 && item_size <= 256);
    QueueHandle_t queue = malloc(sizeof(*queue));
    queue->item_size = item_size; queue->count = 0;
    return queue;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout)
{
    assert(timeout == 0); // Public button APIs must never block.
    if (queue->count == 8) return 0;
    memcpy(queue->items[queue->count++], item, queue->item_size);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout)
{
    (void)timeout;
    if (!queue->count) return 0;
    memcpy(item, queue->items[0], queue->item_size);
    --queue->count;
    memmove(queue->items[0], queue->items[1], queue->count * sizeof(queue->items[0]));
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t queue) { free(queue); }
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{ SemaphoreHandle_t sem = malloc(sizeof(*sem)); sem->count = 0; return sem; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) { sem->count = 1; return pdTRUE; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout)
{
    if (!sem->count) { waited += timeout; return 0; }
    sem->count = 0; return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t sem) { free(sem); }
/* The PCM queue lives for the whole process: one static instance, borrowed by
 * every source and reset at each START. heap_queue_created flags any attempt
 * to go back to per-play heap allocation (the v2-v5 100% start-failure mode). */
static struct test_player_buffer s_static_queue;
static bool heap_queue_created;
StreamBufferHandle_t xStreamBufferCreate(size_t capacity, size_t trigger)
{
    assert(capacity == PLAYER_QUEUE_BYTES && trigger == 1);
    heap_queue_created = true; /* Regression guard: checked at the end of main. */
    StreamBufferHandle_t buffer = malloc(sizeof(*buffer));
    buffer->capacity = capacity; buffer->count = 0; return buffer;
}
StreamBufferHandle_t xStreamBufferCreateStatic(size_t capacity, size_t trigger,
                                               uint8_t *storage, StaticStreamBuffer_t *object)
{
    (void)storage; (void)object;
    assert(capacity == PLAYER_QUEUE_BYTES && trigger == 1);
    s_static_queue.capacity = capacity; s_static_queue.count = 0;
    return &s_static_queue;
}
BaseType_t xStreamBufferReset(StreamBufferHandle_t buffer)
{ buffer->count = 0; return pdTRUE; }
size_t xStreamBufferSend(StreamBufferHandle_t buffer, const void *data, size_t size, TickType_t timeout)
{
    (void)timeout;
    if (pump_audio && !buffer->capacity) assert(false);
    if (pump_audio && buffer->count == buffer->capacity) consume_source();
    if (size > buffer->capacity - buffer->count) size = buffer->capacity - buffer->count;
    memcpy(buffer->bytes + buffer->count, data, size); buffer->count += size; return size;
}
size_t xStreamBufferReceive(StreamBufferHandle_t buffer, void *data, size_t size, TickType_t timeout)
{
    if (!buffer->count) waited += timeout;
    if (size > buffer->count) size = buffer->count;
    memcpy(data, buffer->bytes, size);
    buffer->count -= size; memmove(buffer->bytes, buffer->bytes + size, buffer->count); return size;
}
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t buffer) { return buffer->count; }
void vStreamBufferDelete(StreamBufferHandle_t buffer) { free(buffer); }
int64_t esp_timer_get_time(void) { return (int64_t)waited * 1000 + 1; }
uint32_t esp_get_free_heap_size(void) { return 80000; }
uint32_t esp_get_minimum_free_heap_size(void) { return 60000; }
size_t heap_caps_get_largest_free_block(unsigned flags) { assert(flags == MALLOC_CAP_8BIT); return 50000; }
esp_err_t bsp_audio_init(void) { return ESP_OK; }
esp_err_t bsp_audio_wake(void) { ++wakes; return ESP_OK; }
esp_err_t bsp_audio_set_format(uint32_t rate, uint8_t bits, uint8_t channels)
{ assert(rate == 16000 && bits == 16 && channels == 1); ++format_calls; return ESP_OK; }
esp_err_t bsp_audio_sleep(void) { ++sleeps; return ESP_OK; }
void bsp_audio_set_volume(uint8_t percent) { assert(percent <= 100); }
esp_err_t bsp_audio_write(const void *data, size_t size)
{
    assert(data && !(size & 1));
    if (pump_audio) {
        assert(bytes_written + size <= sizeof(captured_audio));
        const uint8_t *pcm = data;
        for (size_t i = 0; i < size; ++i)
            assert(pcm[i] == sample_byte(expected_audio_start + bytes_written + i));
        memcpy(captured_audio + bytes_written, data, size);
        bytes_written += size;
        audio_clock_remainder += size * 1000U;
        waited += (TickType_t)(audio_clock_remainder / PODCAST_PCM_BYTES_PER_SECOND);
        audio_clock_remainder %= PODCAST_PCM_BYTES_PER_SECOND;
    }
    return ESP_OK;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    esp_http_client_handle_t client = malloc(sizeof(*client));
    memset(client, 0, sizeof(*client));
    client->config = *config;
    snprintf(client->url, sizeof(client->url), "%s", config->url);
    assert(config->buffer_size == 4096 && strstr(client->url, "/stream.pcm"));
    client->total = 0;
    if (fixture_count) assert(s_session.count == fixture_count);
    for (uint32_t i = 0; i < s_session.count; ++i)
        client->total += fixture_count ? fixture_lengths[i] : 300ULL * 32000;
    ++http_live;
    return client;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{ assert(http_live); --http_live; free(client); return ESP_OK; }
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int size)
{ (void)client; assert(size == 0); return ESP_OK; }
static void header(esp_http_client_handle_t client, const char *key, const char *value)
{
    esp_http_client_event_t event = {.event_id = HTTP_EVENT_ON_HEADER,
        .header_key = key, .header_value = value, .user_data = client->config.user_data};
    assert(client->config.event_handler(&event) == ESP_OK);
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
    header(client, "X-Audio-Sample-Rate", "16000"); header(client, "X-Audio-Channels", "1");
    header(client, "X-Audio-Bits", "16"); header(client, "X-Audio-Format", "s16le");
    char count[32], lengths[4096];
    snprintf(count, sizeof(count), "%u", s_session.count);
    lengths[0] = '\0';
    for (uint32_t i = 0; i < s_session.count; ++i) {
        size_t n = strlen(lengths);
        snprintf(lengths + n, sizeof(lengths) - n, "%s%llu", i ? "," : "",
                 (unsigned long long)(fixture_count ? fixture_lengths[i] : 300ULL * 32000));
    }
    header(client, "X-Audio-Segment-Count", count);
    header(client, "X-Audio-Segment-Lengths", lengths);
    client->status = 200;
    if (client->config.method == HTTP_METHOD_HEAD) {
        ++head_calls;
    } else {
        ++get_calls;
    }
    if (client->config.method == HTTP_METHOD_GET && client->offset && !ignored_range) {
        client->status = 206;
        char range[96];
        snprintf(range, sizeof(range), "bytes %llu-%llu/%llu",
                 (unsigned long long)client->offset, (unsigned long long)(client->total - 1),
                 (unsigned long long)client->total);
        header(client, "Content-Range", range);
        return (int64_t)(client->total - client->offset);
    }
    return (int64_t)client->total;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client) { return client->status; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value)
{
    if (!strcmp(key, "Authorization")) {
        assert(!strcmp(value, "Bearer aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
        assert(strstr(client->url, "aaaaaaaa") == NULL);
        if (client->config.method == HTTP_METHOD_HEAD) ++authenticated_heads;
        else { assert(client->config.method == HTTP_METHOD_GET); ++authenticated_gets; }
        return ESP_OK;
    }
    assert(strcmp(key, "Range") == 0 && strncmp(value, "bytes=", 6) == 0);
    client->offset = strtoull(value + 6, NULL, 10); range_requested = client->offset;
    client->read_at = client->offset;
    return ESP_OK;
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout_ms)
{ (void)client; assert(timeout_ms == PLAYER_READ_TIMEOUT_MS); return ESP_OK; }
static uint64_t unmute_at,silent_at_unmute;
int esp_http_client_read(esp_http_client_handle_t client, char *data, int size)
{
    if(unmute_at&&client->read_at>=unmute_at){unmute_at=0;const player_command_t c={.kind=CMD_VOLUME,.value=55};command(&c);silent_at_unmute=s_source->accounted_heard;}
    if (!pump_audio) return 0;
    if (cancel_at && client->read_at >= cancel_at) { s_source->cancel = true; return 0; }
    if (truncate_at && client->read_at >= truncate_at) return 0;
    if (jitter_ms && !jitter_injected && client->read_at >= jitter_at) {
        jitter_injected = true;
        TickType_t end = waited + jitter_ms;
        while (waited < end) consume_source();
    }
    uint64_t available = client->total - client->read_at;
    if ((uint64_t)size > available) size = (int)available;
    /* Real network chunks may split 16-bit PCM samples and cache boundaries. */
    if (size > 777) size = 777;
    for (int i = 0; i < size; ++i) data[i] = (char)sample_byte(client->read_at + (unsigned)i);
    client->read_at += (unsigned)size;
    return size;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client)
{ return client->read_at == client->total || (truncate_at && client->read_at >= truncate_at); }

static void dispatch(void)
{
    player_command_t request;
    assert(xQueueReceive(s_commands, &request, 0) == pdTRUE);
    command(&request);
}

static void begin_fixture(uint32_t count)
{
    fixture_count = count;
    fixture_lengths[0] = 64000;
    fixture_lengths[1] = 64000;
    fixture_lengths[2] = 32000;
    pump_audio = true;
    jitter_at = cancel_at = truncate_at = 0;
    jitter_ms = 0;
    jitter_injected = false;
    bytes_written = expected_audio_start = audio_clock_remainder = 0;
    producer_ack = true;
    assert(podcast_player_start("fixture", "continuous", (podcast_player_cursor_t){0, 0}, count));
    dispatch();
    plan_step();
}

static void run_to_end(void)
{
    assert(s_source);
    producer_task(s_source);
    for (unsigned steps = 0; s_source && steps < 1000; ++steps) {
        bool was_draining = s_session.draining;
        int64_t deadline = s_session.drain_until_us;
        if (observe_eof && was_draining && esp_timer_get_time() < deadline) {
            assert(s_session.state == PODCAST_PLAYER_PLAYING);
            assert(s_session.completion_id == completion_baseline);
        }
        consume_source();
        if (observe_eof && !was_draining && s_source && s_session.draining) {
            assert(s_session.state == PODCAST_PLAYER_PLAYING);
            assert(s_session.drain_until_us > esp_timer_get_time());
            assert(s_session.completion_id == completion_baseline);
        }
    }
    assert(!s_source);
}

static void test_headers(void)
{
    response_metadata_t metadata = {0};
    assert(!segment_lengths(&metadata, ""));
    assert(!segment_lengths(&metadata, "2,"));
    assert(!segment_lengths(&metadata, "3"));
    assert(!segment_lengths(&metadata, "0"));
    assert(!segment_lengths(&metadata, "18446744073709551616"));
    assert(!segment_lengths(&metadata, "18446744073709551614,2"));
    memset(&metadata, 0, sizeof(metadata));
    assert(segment_lengths(&metadata, "64000,64000,32000"));
    metadata.has_count = true; metadata.count = 3;
    uint64_t total;
    assert(segment_metadata_valid(&metadata, 3, &total) && total == 160000);
    assert(!segment_metadata_valid(&metadata, 2, &total));
    assert(segment_lengths(&metadata, "64000,64000,32000"));
    assert(!segment_lengths(&metadata, "64000,64000,32002"));
    memset(&metadata, 0, sizeof(metadata));
    char text[4096] = {0};
    for (unsigned i = 0; i < 256; ++i) strcat(text, i ? ",9600000" : "9600000");
    assert(segment_lengths(&metadata, text));
    metadata.has_count = true; metadata.count = 256;
    assert(segment_metadata_valid(&metadata, 256, &total) && total == 256ULL * 9600000);
    strcat(text, ",9600000");
    assert(!segment_lengths(&metadata, text));
    memset(text, '2', sizeof(text) - 1); text[sizeof(text) - 1] = '\0';
    assert(!segment_lengths(&metadata, text));
}

static void test_volume_commands(void)
{
    TickType_t before = waited;
    uint32_t session_id = s_session.generation;
    assert(s_session.heard_bytes==0);
    assert(podcast_player_set_volume(55));
    assert(podcast_player_adjust_volume(5));
    assert(podcast_player_adjust_volume(5));
    assert(podcast_player_adjust_volume(-5));
    assert(waited == before && s_commands->count == 4);
    while (s_commands->count) dispatch();
    assert(s_session.volume == 60 && s_session.generation == session_id);
    assert(podcast_player_adjust_volume(100)); dispatch(); assert(s_session.volume == 100);
    assert(podcast_player_adjust_volume(5)); dispatch(); assert(s_session.volume == 100);
    assert(podcast_player_adjust_volume(-100)); dispatch(); assert(s_session.volume == 0);
    assert(podcast_player_adjust_volume(-5)); dispatch(); assert(s_session.volume == 0);
    assert(!podcast_player_adjust_volume(INT_MIN));
    assert(!podcast_player_adjust_volume(101));
    assert(podcast_player_set_volume(55)); dispatch();
    podcast_player_snapshot_t snapshot;
    assert(podcast_player_snapshot(&snapshot) && snapshot.volume == 55 && snapshot.session_id == session_id);
    puts("Volume commands: ordered relative presses, zero callback wait, 0/100 clamp, stable playback session PASS");
}

static void test_continuous_stream(void)
{
    completion_baseline = s_session.completion_id;
    begin_fixture(3);
    assert(s_session.completion_id == completion_baseline);
    plan_step(); assert(s_source && s_source->queue->capacity == 32768);
    unsigned heads = head_calls, gets = get_calls, stopped = sleeps;
    unsigned woke = wakes, formats = format_calls;
    jitter_at = 70000; jitter_ms = 500;
    observe_eof = true;
    run_to_end();
    observe_eof = false;
    podcast_player_snapshot_t snapshot;
    assert(podcast_player_snapshot(&snapshot) && snapshot.state == PODCAST_PLAYER_FINISHED);
    assert(jitter_injected && bytes_written == 160000);
    assert(snapshot.completion_id == completion_baseline + 1);
    completion_baseline = snapshot.completion_id;
    assert(snapshot.cursor.segment == 2 && snapshot.cursor.byte_offset == 32000);
    assert(snapshot.elapsed_seconds == 5 && snapshot.total_seconds == 5);
    assert(snapshot.elapsed_ms==5000&&snapshot.heard_ms==5000);
    assert(snapshot.underruns == 0 && snapshot.segments_crossed == 2);
    assert(head_calls == heads && get_calls == gets); /* No request at either cache boundary. */
    assert(sleeps == stopped + 1 && wakes == woke && format_calls == formats);
    assert(http_live == 0 && snapshot.buffered_ms == 0);
    puts("Continuous PCM: one GET, exact 160000 sample bytes, 2 segment boundaries, 500ms jitter, zero queue underruns PASS");

    uint32_t closed_id=snapshot.session_id;
    begin_fixture(3); plan_step();
    assert(podcast_player_snapshot(&snapshot)&&snapshot.closed_session_id==closed_id&&snapshot.closed_heard_ms==5000&&snapshot.heard_ms==0);
    jitter_at = 70000; jitter_ms = 1500;
    run_to_end();
    assert(podcast_player_snapshot(&snapshot) && snapshot.state == PODCAST_PLAYER_FINISHED);
    assert(snapshot.underruns == 1 && bytes_written == 160000);
    assert(snapshot.completion_id == completion_baseline + 1);
    completion_baseline = snapshot.completion_id;
    puts("Continuous PCM: 1500ms outage honestly re-buffers once; recovery has no lost/repeated samples PASS");

    begin_fixture(3); plan_step();
    cancel_at = 120000;
    producer_task(s_source);
    assert(s_source && s_source->done && !s_source->complete);
    uint64_t downloaded = s_source->client->read_at;
    uint64_t submitted = s_source->submitted;
    assert(downloaded > submitted && s_source->queue->count);
    assert(podcast_player_pause()); dispatch();
    assert(podcast_player_snapshot(&snapshot) && snapshot.state == PODCAST_PLAYER_PAUSED);
    uint64_t paused;
    assert(podcast_position_absolute(s_session.lengths, s_session.count, snapshot.cursor, &paused));
    assert(paused <= submitted && submitted - paused <= PLAYER_DMA_BYTES);
    assert(snapshot.cursor.segment == 1 && paused < downloaded);
    bytes_written = audio_clock_remainder = 0;
    expected_audio_start = paused;
    cancel_at = 0;
    heads = head_calls;
    assert(podcast_player_resume()); dispatch(); plan_step();
    assert(range_requested == paused && head_calls == heads);
    assert(s_session.completion_id == completion_baseline);
    run_to_end();
    assert(bytes_written == 160000 - paused);
    assert(s_session.completion_id == completion_baseline + 1);
    completion_baseline = s_session.completion_id;
    puts("Continuous PCM: pause while producer is ahead saves heard segment; Range resume discards queued future bytes PASS");

    begin_fixture(3); plan_step();
    uint32_t session_id = s_session.generation;
    assert(podcast_player_pause()); dispatch();
    assert(podcast_player_seek_relative(3)); dispatch(); plan_step();
    assert(s_session.cursor.segment == 1 && s_session.cursor.byte_offset == 32000);
    assert(s_session.state == PODCAST_PLAYER_PAUSED && s_session.generation == session_id);
    assert(s_session.heard_bytes==0);
    assert(podcast_player_resume()); dispatch(); plan_step();
    assert(range_requested == 96000 && s_session.generation == session_id);
    expected_audio_start = 96000;
    run_to_end();
    assert(bytes_written == 64000 && s_session.generation == session_id);
    assert(podcast_player_snapshot(&snapshot)&&snapshot.heard_ms==2000&&snapshot.elapsed_ms==5000);
    assert(s_session.completion_id == completion_baseline + 1);
    completion_baseline = s_session.completion_id;
    puts("Continuous PCM: cross-segment seek uses exact lengths/global Range; session survives seek/resume PASS");

    begin_fixture(3); plan_step();
    session_id = s_session.generation;
    assert(podcast_player_pause()); dispatch();
    assert(podcast_player_seek_relative(3));
    assert(podcast_player_resume());
    dispatch(); dispatch(); /* Same real worker command batch: no plan_step between them. */
    assert(s_session.plan == PLAN_SEEK && s_session.plan_play);
    assert(s_session.completion_id == completion_baseline && s_session.generation == session_id);
    plan_step();
    assert(s_source && s_source->start_offset == 96000);
    expected_audio_start = 96000;
    run_to_end();
    assert(bytes_written == 64000 && s_session.completion_id == completion_baseline + 1);
    completion_baseline = s_session.completion_id;
    puts("Continuous PCM: adjacent SEEK/RESUME commands resolve the paused seek then actually play, never silently stay paused PASS");

    begin_fixture(3); plan_step();
    truncate_at = 90000;
    run_to_end();
    assert(podcast_player_snapshot(&snapshot) && snapshot.state == PODCAST_PLAYER_ERROR);
    assert(snapshot.cursor.segment < 2 && bytes_written < 160000);
    assert(snapshot.completion_id == completion_baseline);
    assert(strcmp(snapshot.error, "网络音频中断") == 0);
    pump_audio = false; fixture_count = 0;
    puts("Completion identity: each real episode end increments once, never before tail drain or on start/seek/resume/error PASS");
    puts("Continuous PCM: truncated body is an error, never a false FINISHED or automatic-next trigger PASS");
}

static void test_exact_central_resume_and_silent_heard(void)
{
    begin_fixture(3);plan_step();uint32_t generation=s_session.generation;
    assert(podcast_player_pause());dispatch();
    assert(podcast_player_seek_to_ms(3031));assert(podcast_player_resume());dispatch();dispatch();
    uint32_t applied=s_session.absolute_seek_id;
    assert(s_session.plan==PLAN_SEEK_TO&&s_session.plan_play&&s_session.heard_bytes==0);plan_step();assert(s_session.absolute_seek_id==applied+1);
    assert(s_session.cursor.segment==1&&s_session.cursor.byte_offset==32992&&s_source->start_offset==96992&&s_session.generation==generation);
    expected_audio_start=96992;run_to_end();podcast_player_snapshot_t snapshot;assert(podcast_player_snapshot(&snapshot)&&snapshot.elapsed_ms==5000&&snapshot.heard_ms==1969&&bytes_written==63008);
    begin_fixture(3);plan_step();assert(podcast_player_set_volume(0));dispatch();run_to_end();
    assert(podcast_player_snapshot(&snapshot)&&snapshot.volume==0&&snapshot.elapsed_ms==5000&&snapshot.heard_ms==0&&bytes_written==160000);
    begin_fixture(3);plan_step();unmute_at=120000;silent_at_unmute=0;run_to_end();
    assert(silent_at_unmute>0&&silent_at_unmute<160000);
    assert(podcast_player_snapshot(&snapshot)&&snapshot.elapsed_ms==5000&&snapshot.heard_ms==(160000-silent_at_unmute)/32&&bytes_written==160000);
    puts("Central resume audio: precise 3031ms position uses actual lengths, seek adds no heard time; silent playback advances position with zero heard time, unmute never back-counts silent PCM PASS");
}
/* 起播失败风暴自愈：连续失败计数到 6 才整机重启；任一次真正出声即清零。
 * 回归背景：v2-v5 曾因堆碎片让 open_source 的 32KB 队列分配 100% 失败，
 * 六连败重启循环就是从这里来的。 */
static void test_start_failure_self_heal(void)
{
    s_fail_run = 0;
    for (unsigned i = 1; i <= 5; ++i) {
        assert(podcast_player_start("selfHeal", "ep1", (podcast_player_cursor_t){0, 0}, 3));
        dispatch();                     /* CMD_START: lengths calloc succeeds, plan=PLAN_START. */
        fail_next_allocation = true;    /* Next calloc: head_lengths metadata. */
        plan_step();                    /* head_lengths fails -> fail("内存不足"). */
        assert(s_fail_run == i && restarts == 0 && http_live == 0);
        podcast_player_snapshot_t snapshot;
        assert(podcast_player_snapshot(&snapshot) && snapshot.state == PODCAST_PLAYER_ERROR);
    }
    /* 第六次连续失败触发重启自愈（宿主环境只计数，不真重启）。 */
    assert(podcast_player_start("selfHeal", "ep1", (podcast_player_cursor_t){0, 0}, 3));
    dispatch();
    fail_next_allocation = true;
    plan_step();
    assert(s_fail_run == 6 && restarts == 1 && http_live == 0);
    /* 真正出声一次即清零：高水位后再失败不会立即重启。 */
    s_fail_run = 5;
    begin_fixture(3);
    plan_step();
    run_to_end();
    assert(s_fail_run == 0 && restarts == 1);
    puts("Start self-heal: five failed starts recover in place, the sixth reboots, real playback resets the streak PASS");
}
/* ①-b 防御回归：缓冲门等待必须有界。2026-10-06 21:10 事故中 worker 在
 * vTaskDelay(10) 上无限自旋（生产者存在但再也不供数），命令队列无人处理、
 * 无失败日志、不重启。25s 攒不满 24.5KB 预缓冲必须走 fail("缓冲超时")。 */
static void test_buffering_starvation_watchdog(void)
{
    unsigned before = restarts;
    begin_fixture(3);
    plan_step(); /* 第一次只做 HEAD；第二次 open_source（生产者宿主上不跑）。 */
    assert(s_source && !s_source->done);
    unsigned guard = 0;
    while (s_session.state != PODCAST_PLAYER_ERROR && ++guard < 5000) consume_source();
    assert(guard < 5000); /* 必须在 ~25s 处生效，绝不允许无限等 */
    assert(s_session.state == PODCAST_PLAYER_ERROR);
    assert(s_session.error && strcmp(s_session.error, "缓冲超时") == 0);
    assert(s_fail_run == 1 && restarts == before && !s_source && http_live == 0);
    puts("Buffering watchdog: 25s prebuffer starvation fails loudly instead of spinning forever PASS");
}
/* ①-b 防御回归：worker 冻结检测。心跳纹丝不动超过 30s → 重启自愈；
 * 心跳在走、或长间隔（休眠唤醒）后重新装订基线 → 绝不误杀。
 * 监视者按主循环节奏 <5s 连续轮询，单次大跳会触发重新装订（休眠语义）。 */
static void test_worker_freeze_monitor(void)
{
    unsigned before = restarts;
    podcast_player_poll();                                       /* 装订基线 */
    for (unsigned i = 0; i < 7; ++i) { waited += 4000; podcast_player_poll(); }
    assert(restarts == before);                                  /* 28s：仍容忍 */
    waited += 4000; podcast_player_poll();                       /* 32s：冻结判定成立 */
    assert(restarts == before + 1);
    for (unsigned i = 0; i < 8; ++i) {                           /* worker 活着：绝不误杀 */
        atomic_fetch_add(&s_heartbeat, 1);
        waited += 4000; podcast_player_poll();
    }
    assert(restarts == before + 1);
    waited += 6000000; podcast_player_poll();                    /* >5s 轮询间隙（休眠唤醒）：重新装订 */
    for (unsigned i = 0; i < 7; ++i) { waited += 4000; podcast_player_poll(); }
    assert(restarts == before + 1);                              /* 新基线后 28s 内不累计误判 */
    waited += 4000; podcast_player_poll();                       /* 重新装订后真正冻结 32s → 重启 */
    assert(restarts == before + 2);
    puts("Worker freeze monitor: 30s frozen heartbeat reboots; live heartbeat and sleep gaps never do PASS");
}
int main(void)
{
    assert(podcast_player_init("http://relay:8899"));
    assert(podcast_player_init("http://relay:8899/")); // Equivalent init never creates another worker.
    char show[] = "showA", episode[] = "episodeA";
    assert(podcast_player_start(show, episode, (podcast_player_cursor_t){24, 160000}, 32));
    strcpy(show, "showB"); strcpy(episode, "episodeB");
    dispatch();
    podcast_player_snapshot_t snapshot;
    assert(podcast_player_snapshot(&snapshot));
    assert(strcmp(snapshot.show_id, "showA") == 0 && strcmp(snapshot.episode_id, "episodeA") == 0);
    plan_step(); assert(head_calls == 1);
    for (unsigned i = 0; i < 32; ++i) assert(s_session.lengths[i] == 300ULL * 32000);
    plan_step(); assert(s_source && range_requested == 24 * 300ULL * 32000 + 160000);
    s_source->submitted = 320000;
    s_source->framing.received = 9000000; // Downloaded progress must never become cursor.
    s_session.state = PODCAST_PLAYER_PLAYING;
    assert(podcast_player_pause()); dispatch();
    assert(!s_source && http_live == 0);
    assert(podcast_player_snapshot(&snapshot) && snapshot.state == PODCAST_PLAYER_PAUSED);
    uint64_t paused = 480000 - 7680;
    assert(snapshot.cursor.byte_offset == paused && snapshot.cursor.segment == 24);
    assert(snapshot.elapsed_seconds == 24 * 300 + paused / 32000);
    assert(podcast_player_resume()); dispatch(); plan_step();
    assert(s_source && range_requested == 24 * 300ULL * 32000 + paused); // Real API/GET uses the paused sample cursor.
    assert(podcast_player_pause()); dispatch();

    ignored_range = true;
    assert(podcast_player_resume()); dispatch(); plan_step();
    assert(!s_source && http_live == 0);
    assert(podcast_player_snapshot(&snapshot) && snapshot.state == PODCAST_PLAYER_ERROR);
    assert(snapshot.cursor.byte_offset == paused); // 200 never fakes a successful resume.
    ignored_range = false;

    fail_next_allocation = true;
    assert(podcast_player_start("newShow", "newEpisode", (podcast_player_cursor_t){1, 100}, 64));
    dispatch();
    assert(podcast_player_snapshot(&snapshot) && snapshot.state == PODCAST_PLAYER_ERROR);
    assert(strcmp(snapshot.show_id, "newShow") == 0 && strcmp(snapshot.episode_id, "newEpisode") == 0);
    assert(snapshot.cursor.segment == 1 && snapshot.cursor.byte_offset == 100 && snapshot.segment_count == 64);

    assert(podcast_player_start("old", "oldEpisode", (podcast_player_cursor_t){0, 0}, 3));
    dispatch(); plan_step();
    producer_ack = false; plan_step(); assert(s_source);
    s_source->submitted = 320000;
    s_session.state = PODCAST_PLAYER_PLAYING;
    unsigned deleted_before = deleted_tasks;
    waited = 0;
    assert(podcast_player_start("target", "targetEpisode", (podcast_player_cursor_t){1, 32000}, 3));
    dispatch();
    assert(waited == 2000 && s_source && deleted_tasks == deleted_before);
    assert(podcast_player_snapshot(&snapshot) && snapshot.state == PODCAST_PLAYER_ERROR);
    assert(strcmp(snapshot.show_id, "target") == 0 && snapshot.cursor.byte_offset == 32000);
    heard_cursor(); publish(); // Retained old producer cannot corrupt the new bookmark.
    assert(podcast_player_snapshot(&snapshot) && snapshot.cursor.segment == 1 && snapshot.cursor.byte_offset == 32000);
    assert(!reap_source(0) && deleted_tasks == deleted_before);
    s_source->task->acknowledged = true; s_source->finished->count = 1;
    assert(reap_source(0) && !s_source && http_live == 0 && deleted_tasks == deleted_before + 1);

    for (unsigned i = 0; i < 8; ++i) assert(podcast_player_pause());
    assert(!podcast_player_pause()); // Full command queue returns immediately.
    assert(!podcast_player_set_volume(101));
    assert(!podcast_player_start("../bad", "ep", (podcast_player_cursor_t){0, 0}, 1));
    assert(!podcast_player_start("show", "ep", (podcast_player_cursor_t){0, 1}, 1));
    while (s_commands->count) dispatch();
    test_headers();
    test_volume_commands();
    test_continuous_stream();
    test_exact_central_resume_and_silent_heard();
    test_start_failure_self_heal();
    test_buffering_starvation_watchdog();
    test_worker_freeze_monitor();
    /* 回归底线：任何播放路径都不得再向堆申请 32KB PCM 队列。 */
    assert(!heap_queue_created);
    puts("PCM queue is boot-time static storage: no play path heap-allocates the 32KB buffer PASS");
    assert(authenticated_heads == head_calls && authenticated_gets == get_calls);
    puts("Podcast player: every audio HEAD/GET authenticated through header, no token in URL PASS");
    puts("Podcast player runtime: immutable identity, pause/Range resume, failure ACK and bounded teardown PASS");
    free(s_session.lengths);
    vQueueDelete(s_commands);
    free(s_worker); // Fake worker was never scheduled in this host harness.
    return 0;
}
