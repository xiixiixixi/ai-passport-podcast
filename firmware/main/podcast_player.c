#include "podcast_player.h"
#include "podcast_pcm.h"
#ifdef PODCAST_PLAYER_TEST
#include "podcast_player_test_adapter.h"
#else
#include "bsp_audio.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#endif

#include <limits.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "podcast_http.h"

#define PLAYER_QUEUE_BYTES 32768U
#define PLAYER_PREBUFFER_BYTES 24576U
#define PLAYER_HTTP_HEADER_BYTES 4096U
#define PLAYER_CHUNK_BYTES 1024U
#define PLAYER_DMA_BYTES (8U * 480U * 2U)
#define PLAYER_HEADER_TIMEOUT_MS 5000
#define PLAYER_READ_TIMEOUT_MS 4000 /* 2.4G 链路抖动可达秒级；0.6s 会把瞬时卡顿误判成断流 */
#define PLAYER_MAX_SEGMENTS 256U
#define PLAYER_COMMAND_DEPTH 8U
#define PLAYER_NOMINAL_SEGMENT_BYTES (300ULL * PODCAST_POSITION_BYTES_PER_SECOND)
/* ①-b 防御阈值：缓冲门最长等待（24.5KB 预缓冲在局域网下 <2s 可达，25s 是
 * 12 倍余量）；worker 心跳冻结判定（30s=外层监视器 / 45s=生产者侧）。 */
#define PLAYER_BUFFERING_TIMEOUT_US 25000000LL
#define PLAYER_HEARTBEAT_FROZEN_US  30000000LL
#define PLAYER_PRODUCER_FROZEN_US   45000000LL

static const char *TAG = "podcast_player";

typedef enum { CMD_START, CMD_PAUSE, CMD_RESUME, CMD_SEEK, CMD_SEEK_TO, CMD_STOP, CMD_VOLUME, CMD_ADJUST_VOLUME } command_kind_t;
typedef struct {
    command_kind_t kind;
    char show_id[24], episode_id[64];
    podcast_player_cursor_t cursor;
    uint32_t count;
    int value;
} player_command_t;

typedef struct {
    podcast_pcm_format_t format;
    char range[96];
    bool invalid_range, invalid_segments;
    bool has_lengths, has_count;
    uint32_t count, parsed_count;
    uint64_t lengths[PLAYER_MAX_SEGMENTS];
} response_metadata_t;

typedef struct {
    esp_http_client_handle_t client;
    StreamBufferHandle_t queue;
    SemaphoreHandle_t finished;
    TaskHandle_t task;
    response_metadata_t metadata;
    podcast_pcm_stream_t framing;
    uint64_t accounted_heard;
    uint64_t start_offset, submitted; /* Whole-episode PCM byte positions. */
    int64_t last_submit_us;
    uint32_t generation;
    atomic_bool cancel, done, complete;
} player_source_t;

typedef enum { PLAN_NONE, PLAN_START, PLAN_SEEK, PLAN_SEEK_TO } player_plan_t;
typedef struct {
    char show_id[24], episode_id[64];
    uint64_t *lengths;
    uint32_t count;
    uint32_t generation, completion_id, absolute_seek_id;
    podcast_player_cursor_t cursor, seek_base;
    podcast_player_state_t state;
    const char *error;
    uint8_t volume;
    player_plan_t plan;
    int seek_seconds;
    bool plan_play, draining;
    int64_t drain_until_us, timing_start_us, last_timing_us;
    unsigned underruns, segments_crossed;
    uint64_t heard_bytes, seek_to_ms;
    int64_t buffering_since_us;
} player_session_t;

/* 起播内存关键路径修复：PCM 环形队列改为编译期静态分配、进程内只创建一次，
 * 起播不再向堆要 32KB 连续块。此前每次按播放都要 xStreamBufferCreate(32768)，
 * 叠加封面缓存(3×5440B)后堆余量不足且碎片化，导致"内存不足"式起播必败并
 * 触发 6 连败重启循环。FreeRTOS 约定存储区须比容量多 1 字节。 */
static uint8_t s_queue_store[PLAYER_QUEUE_BYTES + 1];
static StaticStreamBuffer_t s_queue_object;
static StreamBufferHandle_t s_queue; /* Exclusive ownership: player worker. */

static QueueHandle_t s_commands;
static TaskHandle_t s_worker;
static char s_relay[192];
static player_session_t s_session = {.state = PODCAST_PLAYER_IDLE, .volume = 55};
/* 连续起播失败计数：无一次成功播放的失败累积到阈值就整机重启自愈，
 * 避免"反复重试→资源耗尽→彻底卡死要拔电"。任一次成功进入播放即清零。 */
static uint8_t s_fail_run;
/* 起播窗口时间戳：从计划打开音源到真正出声期间，封面等低优先级活动必须避让，
 * 否则在起播最脆弱的几秒里抢内存/闪存会让 START 反复失败。30 秒兜底自动过期。 */
static volatile int64_t s_start_pending_us;
/* ①-b 防御：worker 心跳。每圈循环自增一次（空闲阻塞最长 100ms 也算活着）。
 * 2026-10-06 21:10 事故中 worker 在一处调用里永久停摆，之后 B 的五次按键
 * 全部无声地堆在命令队列里（无 HEAD/GET、无失败日志、不重启）。外层
 * （demo 主循环的 podcast_player_poll）与生产者任务共同观测此心跳，
 * 冻结即记录并重启自愈。 */
static atomic_uint s_heartbeat;

bool podcast_player_start_pending(void)
{
    int64_t at = s_start_pending_us;
    return at != 0 && esp_timer_get_time() - at < 30000000;
}
static player_source_t *s_source; /* Exclusive ownership: player worker. */
/* ①-b 防御：流缓冲状态一致性。事故现场 182,386,100 字节"成功"塞进了 32KB
 * 的静态队列（服务端全部发出、生产者全程无阻塞），说明内部状态已写坏、
 * 生产者与消费者对队列的认知脱钩。"可用字节数 ≤ 容量"是最硬的不变量，
 * 单核下即使读写并发造成撕裂读，两个分量各自合法时差值也不会超容量。 */
static bool queue_intact(void)
{
    return !s_queue || xStreamBufferBytesAvailable(s_queue) <= PLAYER_QUEUE_BYTES;
}
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static podcast_player_snapshot_t s_snapshot = {.state = PODCAST_PLAYER_IDLE, .volume = 55};

static uint32_t s_closed_session;
static uint64_t s_closed_position, s_closed_heard;
static uint64_t position_bytes(void)
{
    uint64_t bytes=s_session.cursor.byte_offset;
    for(uint32_t i=0;i<s_session.cursor.segment&&i<s_session.count;i++)
        bytes+=s_session.lengths&&s_session.lengths[i]?s_session.lengths[i]:PLAYER_NOMINAL_SEGMENT_BYTES;
    return bytes;
}
static void close_heard_session(void)
{
    if(!s_session.generation||!s_session.count)return;
    s_closed_session=s_session.generation;s_closed_position=position_bytes()/32;s_closed_heard=s_session.heard_bytes/32;
}
static void publish(void)
{
    podcast_player_snapshot_t snapshot = {
        .state = s_session.state, .cursor = s_session.cursor,
        .segment_count = s_session.count, .volume = s_session.volume,
        .session_id = s_session.generation, .completion_id = s_session.completion_id,
        .error = s_session.error, .heard_ms = s_session.heard_bytes / 32,.absolute_seek_id=s_session.absolute_seek_id,
        .closed_session_id=s_closed_session,.closed_elapsed_ms=s_closed_position,.closed_heard_ms=s_closed_heard,
        .underruns = s_session.underruns, .segments_crossed = s_session.segments_crossed,
        .buffered_ms = s_source && s_source->generation == s_session.generation ?
            (uint32_t)podcast_pcm_duration_ms(
            xStreamBufferBytesAvailable(s_source->queue)) : 0,
    };
    memcpy(snapshot.show_id, s_session.show_id, sizeof(snapshot.show_id));
    memcpy(snapshot.episode_id, s_session.episode_id, sizeof(snapshot.episode_id));
    uint64_t elapsed = s_session.cursor.byte_offset, total = 0;
    bool total_known = s_session.count > 0;
    for (uint32_t i = 0; i < s_session.count; ++i) {
        uint64_t length = s_session.lengths ? s_session.lengths[i] : 0;
        if (!length) {
            total_known = false;
        }
        if (i < s_session.cursor.segment)
            elapsed += length ? length : PLAYER_NOMINAL_SEGMENT_BYTES;
        total += length;
    }
    snapshot.elapsed_seconds = elapsed / PODCAST_POSITION_BYTES_PER_SECOND;
    snapshot.elapsed_ms = elapsed / 32;
    if (total_known) snapshot.total_seconds = total / PODCAST_POSITION_BYTES_PER_SECOND;
    portENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot = snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static void heard_cursor(void)
{
    if (!s_source || s_source->generation != s_session.generation) return;
    uint64_t dma_bytes = PLAYER_DMA_BYTES;
    int64_t idle_us = esp_timer_get_time() - s_source->last_submit_us;
    if (s_source->last_submit_us && idle_us > 0) {
        uint64_t drained = (uint64_t)idle_us * PODCAST_PCM_BYTES_PER_SECOND / 1000000U;
        dma_bytes = drained < dma_bytes ? dma_bytes - drained : 0;
    }
    uint64_t absolute = podcast_position_heard_offset(
        s_source->start_offset, s_source->submitted, dma_bytes);
    uint64_t heard = absolute >= s_source->start_offset ? absolute - s_source->start_offset : 0;
    if (heard > s_source->accounted_heard) {
        if (s_session.volume) s_session.heard_bytes += heard - s_source->accounted_heard;
        s_source->accounted_heard = heard;
    }
    podcast_player_cursor_t cursor;
    if (!podcast_position_from_absolute(s_session.lengths, s_session.count, absolute, &cursor)) return;
    if (cursor.segment > s_session.cursor.segment)
        s_session.segments_crossed += cursor.segment - s_session.cursor.segment;
    s_session.cursor = cursor;
}

static bool segment_number(const char **text, uint64_t *value)
{
    const char *p = *text;
    if (*p < '0' || *p > '9') return false;
    *value = 0;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p++ - '0');
        if (*value > (UINT64_MAX - digit) / 10U) return false;
        *value = *value * 10U + digit;
    }
    *text = p;
    return true;
}

static bool segment_lengths(response_metadata_t *metadata, const char *value)
{
    if (!value || strlen(value) >= PLAYER_HTTP_HEADER_BYTES - 512U) return false;
    const char *p = value;
    uint32_t count = 0;
    uint64_t total = 0;
    do {
        uint64_t length;
        if (count >= PLAYER_MAX_SEGMENTS || !segment_number(&p, &length) ||
            !length || (length & 1U) || length > UINT64_MAX - total) return false;
        if (metadata->has_lengths && metadata->lengths[count] != length) return false;
        metadata->lengths[count++] = length;
        total += length;
        if (!*p) break;
        if (*p++ != ',' || !*p) return false;
    } while (*p);
    if (metadata->has_lengths && metadata->parsed_count != count) return false;
    metadata->parsed_count = count;
    metadata->has_lengths = true;
    return true;
}

static bool segment_metadata_valid(const response_metadata_t *metadata, uint32_t count,
                                   uint64_t *total)
{
    if (metadata->invalid_segments || !metadata->has_count || !metadata->has_lengths ||
        metadata->count != count || metadata->parsed_count != count) return false;
    *total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!metadata->lengths[i] || (metadata->lengths[i] & 1U) ||
            metadata->lengths[i] > UINT64_MAX - *total) return false;
        *total += metadata->lengths[i];
    }
    return true;
}

static esp_err_t response_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    response_metadata_t *metadata = event->user_data;
    podcast_pcm_header(&metadata->format, event->header_key, event->header_value);
    if (strcasecmp(event->header_key, "X-Audio-Segment-Lengths") == 0) {
        if (!segment_lengths(metadata, event->header_value)) metadata->invalid_segments = true;
    } else if (strcasecmp(event->header_key, "X-Audio-Segment-Count") == 0) {
        const char *p = event->header_value;
        uint64_t count;
        if (!p || !segment_number(&p, &count) || *p || !count || count > PLAYER_MAX_SEGMENTS ||
            (metadata->has_count && metadata->count != count)) metadata->invalid_segments = true;
        else { metadata->count = (uint32_t)count; metadata->has_count = true; }
    } else if (strcasecmp(event->header_key, "Content-Range") == 0) {
        size_t length = strlen(event->header_value);
        if (length >= sizeof(metadata->range) ||
            (metadata->range[0] && strcmp(metadata->range, event->header_value) != 0))
            metadata->invalid_range = true;
        else memcpy(metadata->range, event->header_value, length + 1);
    }
    return ESP_OK;
}

static esp_http_client_handle_t make_client(response_metadata_t *metadata,
                                             esp_http_client_method_t method)
{
    char url[384];
    int length = snprintf(url, sizeof(url), "%s/pcm/%s/%s/stream.pcm", s_relay,
                          s_session.show_id, s_session.episode_id);
    if (length < 0 || (size_t)length >= sizeof(url)) return NULL;
    esp_http_client_config_t config = {
        .url = url, .method = method, .timeout_ms = PLAYER_HEADER_TIMEOUT_MS,
        .event_handler = response_event, .user_data = metadata,
        .buffer_size = PLAYER_HTTP_HEADER_BYTES,
    };
    return podcast_http_client(config);
}

/* ①-b 防御（生产者侧）：worker 可能先于生产者死亡。生产者每次外圈循环
 * 检查 worker 心跳与队列不变量，两者任一异常都直接重启——这是 21:10 事故
 * 里"生产者活着把 182MB 灌进坏队列而无人报警"的直接补丁。 */
static bool worker_beat_alive(void)
{
    static uint32_t watched;
    static int64_t since_us;
    uint32_t beat = atomic_load(&s_heartbeat);
    int64_t now = esp_timer_get_time();
    if (beat != watched || !since_us) {
        watched = beat;
        since_us = now;
        return true;
    }
    return now - since_us <= PLAYER_PRODUCER_FROZEN_US;
}

static void producer_task(void *arg)
{
    player_source_t *source = arg;
    uint8_t input[PLAYER_CHUNK_BYTES];
    uint64_t received = 0;
    unsigned retries = 0;
    while (!source->cancel && received < source->framing.expected) {
        size_t want = sizeof(input);
        if (source->framing.expected - received < want) want = source->framing.expected - received;
        int n = esp_http_client_read(source->client, (char *)input, (int)want);
        if (n <= 0) {
            if (source->cancel || esp_http_client_is_complete_data_received(source->client) ||
                ++retries > 5) break;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        retries = 0;
        size_t sent = 0;
        unsigned stalled = 0;
        while (sent < (size_t)n && !source->cancel) {
            size_t pushed = xStreamBufferSend(source->queue, input + sent,
                                              (size_t)n - sent, pdMS_TO_TICKS(50));
            sent += pushed;
            if (pushed) { stalled = 0; continue; }
            /* 零进展发送循环必须有界：消费者消失时队列永远满，原实现会
             * 无限重试、done 永不置位（2026-10-06 事故形态之一）。
             * 200 轮 × 50ms ≈ 10s 无一字节入队才放弃。 */
            if (++stalled >= 200) {
                source->cancel = true;
                ESP_LOGE(TAG, "producer stalled: consumer gone for ~10s (queued=%u received=%llu)",
                         (unsigned)xStreamBufferBytesAvailable(source->queue),
                         (unsigned long long)received);
                break;
            }
        }
        if (source->cancel) break;
        if (!worker_beat_alive()) {
            ESP_LOGE(TAG, "player worker heartbeat frozen; rebooting (received=%llu queued=%u)",
                     (unsigned long long)received,
                     (unsigned)xStreamBufferBytesAvailable(source->queue));
            vTaskDelay(pdMS_TO_TICKS(150));
            esp_restart();
        }
        if (xStreamBufferBytesAvailable(source->queue) > PLAYER_QUEUE_BYTES) {
            ESP_LOGE(TAG, "PCM queue state corrupted (available=%u capacity=%u); rebooting",
                     (unsigned)xStreamBufferBytesAvailable(source->queue),
                     (unsigned)PLAYER_QUEUE_BYTES);
            vTaskDelay(pdMS_TO_TICKS(150));
            esp_restart();
        }
        received += sent;
    }
    source->complete = !source->cancel && received == source->framing.expected;
    source->done = true;
    xSemaphoreGive(source->finished);
    vTaskSuspend(NULL); /* No shared access after acknowledgement. */
}

static bool reap_source(TickType_t timeout)
{
    if (!s_source) return true;
    if (xSemaphoreTake(s_source->finished, timeout) != pdTRUE) return false;
    vTaskDelete(s_source->task);
    esp_http_client_cleanup(s_source->client);
    /* s_queue is process-lifetime static storage: keep it, reset on next start. */
    vSemaphoreDelete(s_source->finished);
    free(s_source);
    s_source = NULL;
    return true;
}

static bool halt_audio(void)
{
    heard_cursor();
    if (s_source) s_source->cancel = true;
    /* Muting first bounds tail replay to the 240ms hardware queue. */
    esp_err_t muted = bsp_audio_sleep();
    if (!reap_source(pdMS_TO_TICKS(2000))) return false;
    s_session.draining = false;
    return muted == ESP_OK;
}

static void fail(const char *message)
{
    /* 失败原因必须上串口：此前只显示在屏幕上，现场排查无从下手。 */
    ESP_LOGE(TAG, "playback failure: %s (free_heap=%u largest_block=%u fail_run=%u)",
             message, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)(s_fail_run + 1));
    s_session.plan = PLAN_NONE;
    if (++s_fail_run >= 6) {
        /* 先计数后清理：即使后续清理路径卡死也会在这里救回来。 */
        ESP_LOGE(TAG, "playback failed %u times in a row; rebooting to recover",
                 (unsigned)s_fail_run);
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
    }
    if (!halt_audio()) message = "停止尚未完成";
    s_start_pending_us = 0; /* 失败收场，恢复封面活动 */
    s_session.state = PODCAST_PLAYER_ERROR;
    s_session.error = message;
    publish();
}

static void stop_error(void)
{
    s_session.plan = PLAN_NONE;
    s_session.state = PODCAST_PLAYER_ERROR;
    s_session.error = "停止尚未完成";
    publish();
}

static bool head_lengths(void)
{
    response_metadata_t *metadata = calloc(1, sizeof(*metadata));
    if (!metadata) { fail("内存不足"); return false; }
    esp_http_client_handle_t client = make_client(metadata, HTTP_METHOD_HEAD);
    if (!client) { free(metadata); fail("内存不足"); return false; }
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        free(metadata);
        fail("服务连接失败");
        return false;
    }
    int64_t length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    uint64_t total = 0;
    bool valid = status == 200 && podcast_pcm_format_valid(&metadata->format, length) &&
                 segment_metadata_valid(metadata, s_session.count, &total) && total == (uint64_t)length;
    if (valid) memcpy(s_session.lengths, metadata->lengths, s_session.count * sizeof(*s_session.lengths));
    free(metadata);
    if (!valid) {
        fail(status == 404 ? "更新播客服务" : status == 503 ? "准备音频" : "音频格式错误");
        return false;
    }
    publish();
    return true;
}

static void destroy_unstarted(player_source_t *source)
{
    if (source->client) esp_http_client_cleanup(source->client);
    /* The PCM queue is static process storage, never per-source. */
    if (source->finished) vSemaphoreDelete(source->finished);
    free(source);
}

static void finish_episode(void)
{
    s_session.plan = PLAN_NONE;
    s_session.draining = false;
    if (!halt_audio()) { stop_error(); return; }
    s_session.cursor = (podcast_player_cursor_t){s_session.count - 1,
                                               s_session.lengths[s_session.count - 1]};
    if (s_session.state != PODCAST_PLAYER_FINISHED) ++s_session.completion_id;
    s_session.state = PODCAST_PLAYER_FINISHED;
    close_heard_session();
    s_session.error = NULL;
    publish();
}

static void open_source(void)
{
    uint64_t total = 0, absolute = 0;
    for (uint32_t i = 0; i < s_session.count; ++i) total += s_session.lengths[i];
    if (!podcast_position_absolute(s_session.lengths, s_session.count, s_session.cursor, &absolute)) {
        fail("位置不可用"); return;
    }
    if (absolute >= total) { finish_episode(); return; }
    ESP_LOGI(TAG, "opening source: offset=%llu total=%llu free_heap=%u largest_block=%u",
             (unsigned long long)absolute, (unsigned long long)total,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    player_source_t *source = calloc(1, sizeof(*source));
    if (!source) { fail("内存不足"); return; }
    source->start_offset = absolute;
    source->generation = s_session.generation;
    /* 队列是进程级静态存量（见 podcast_player_init），起播堆需求从 ~42KB
     * （32K 队列+4K 任务栈+HTTP 客户端）降到 ~9KB：只复位上一会话残留字节，
     * 再借给本会话。此前此处向堆要 32KB 连续块，叠加封面缓存与碎片化后
     * 100% 失败（"内存不足"只上屏幕，串口不可见）。 */
    if (xStreamBufferReset(s_queue) != pdPASS) {
        /* 复位失败=仍有任务阻塞在该缓冲上（待决读/写）。带着旧状态继续借用
         * 正是 2026-10-06 状态脱钩的入口之一，宁可失败重试也不带伤起播。 */
        destroy_unstarted(source); fail("队列复位失败"); return;
    }
    source->queue = s_queue;
    source->finished = xSemaphoreCreateBinary();
    if (!source->queue || !source->finished) {
        destroy_unstarted(source); fail("内存不足"); return;
    }
    source->client = make_client(&source->metadata, HTTP_METHOD_GET);
    if (!source->client) { destroy_unstarted(source); fail("内存不足"); return; }
    if (source->start_offset) {
        char range[64];
        snprintf(range, sizeof(range), "bytes=%llu-", (unsigned long long)source->start_offset);
        if (esp_http_client_set_header(source->client, "Range", range) != ESP_OK) {
            destroy_unstarted(source); fail("服务连接失败"); return;
        }
    }
    if (esp_http_client_open(source->client, 0) != ESP_OK) {
        destroy_unstarted(source); fail("服务连接失败"); return;
    }
    int64_t length = esp_http_client_fetch_headers(source->client);
    int status = esp_http_client_get_status_code(source->client);
    bool range_valid = !source->metadata.invalid_range && podcast_position_range_valid(
        status, source->metadata.range, source->start_offset, total, length);
    if (status == 416 && range_valid) {
        destroy_unstarted(source);
        finish_episode();
        return;
    }
    uint64_t advertised_total = 0;
    bool segments_valid = segment_metadata_valid(&source->metadata, s_session.count, &advertised_total) &&
        advertised_total == total && memcmp(source->metadata.lengths, s_session.lengths,
                                            s_session.count * sizeof(*s_session.lengths)) == 0;
    if (!range_valid || !segments_valid || !podcast_pcm_format_valid(&source->metadata.format, length)) {
        ESP_LOGE(TAG, "range/format rejected: status=%d offset=%llu length=%lld",
                 status, (unsigned long long)source->start_offset, (long long)length);
        destroy_unstarted(source); fail("音频格式错误"); return;
    }
    source->framing.expected = (uint64_t)length;
    if (bsp_audio_init() != ESP_OK || bsp_audio_wake() != ESP_OK ||
        bsp_audio_set_format(PODCAST_PCM_RATE, 16, 1) != ESP_OK) {
        destroy_unstarted(source); fail("音频设备错误"); return;
    }
    bsp_audio_set_volume(s_session.volume);
    if (esp_http_client_set_timeout_ms(source->client, PLAYER_READ_TIMEOUT_MS) != ESP_OK) {
        destroy_unstarted(source); fail("服务连接失败"); return;
    }
    if (xTaskCreate(producer_task, "podcast_http", 4096, source, 4, &source->task) != pdPASS) {
        destroy_unstarted(source); fail("内存不足"); return;
    }
    s_source = source;
    s_session.plan = PLAN_NONE;
    s_session.draining = false;
    s_session.buffering_since_us = 0;
    s_session.timing_start_us = s_session.last_timing_us = 0;
    ESP_LOGI(TAG, "continuous PCM buffer: bytes=%u prebuffer=%u free_heap=%u largest_block=%u",
             (unsigned)PLAYER_QUEUE_BYTES, (unsigned)PLAYER_PREBUFFER_BYTES,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    s_session.error = NULL;
    s_session.state = PODCAST_PLAYER_BUFFERING;
    publish();
}

static void plan_step(void)
{
    s_start_pending_us = esp_timer_get_time(); /* 起播/跳转窗口开始，封面工人避让 */
    if (!s_session.lengths[0]) {
        (void)head_lengths();
        return;
    }
    if (s_session.plan == PLAN_SEEK || s_session.plan == PLAN_SEEK_TO) {
        podcast_player_cursor_t target;
        uint64_t absolute=s_session.seek_to_ms>UINT64_MAX/32?UINT64_MAX:s_session.seek_to_ms*32;
        uint64_t total=0;for(uint32_t i=0;i<s_session.count;i++)total+=s_session.lengths[i];
        if(absolute>total)absolute=total;
        bool valid=s_session.plan==PLAN_SEEK_TO?podcast_position_from_absolute(s_session.lengths,s_session.count,absolute,&target):
            podcast_position_seek(s_session.lengths, s_session.count, s_session.seek_base,s_session.seek_seconds,&target);
        if (!valid) {
            fail("位置不可用"); return;
        }
        if(s_session.plan==PLAN_SEEK_TO)++s_session.absolute_seek_id;
        s_session.cursor = target;
        s_session.plan = PLAN_START;
        s_session.seek_seconds = 0;
        if (!s_session.plan_play) {
            s_session.plan = PLAN_NONE;
            s_session.state = PODCAST_PLAYER_PAUSED;
            publish();
            return;
        }
    }
    open_source();
}

static void consume_source(void)
{
    if (s_session.draining) {
        if (esp_timer_get_time() < s_session.drain_until_us) {
            vTaskDelay(pdMS_TO_TICKS(10)); return;
        }
        heard_cursor();
        if (!reap_source(pdMS_TO_TICKS(100))) { fail("停止尚未完成"); return; }
        finish_episode();
        return;
    }
    size_t queued = xStreamBufferBytesAvailable(s_source->queue);
    if (s_session.state == PODCAST_PLAYER_BUFFERING) {
        if (!s_source->done && queued < PLAYER_PREBUFFER_BYTES) {
            /* ①-b 防御：缓冲门等待必须有界。原实现里消费者可以在
             * vTaskDelay(10) 上无限自旋（worker 表面活着、心跳意义上却
             * 再也不处理任何命令）。25s 攒不满 24.5KB 即按失败走自愈。 */
            if (!s_session.buffering_since_us) s_session.buffering_since_us = esp_timer_get_time();
            else if (esp_timer_get_time() - s_session.buffering_since_us > PLAYER_BUFFERING_TIMEOUT_US) {
                fail("缓冲超时"); return;
            }
            vTaskDelay(pdMS_TO_TICKS(10)); return;
        }
        s_session.state = PODCAST_PLAYER_PLAYING;
        s_session.buffering_since_us = 0;
        s_fail_run = 0;
        s_start_pending_us = 0; /* 已出声，封面活动恢复 */
        if (!s_session.timing_start_us)
            s_session.timing_start_us = s_session.last_timing_us = esp_timer_get_time();
        publish();
    }
    uint8_t input[PLAYER_CHUNK_BYTES];
    int16_t output[(PLAYER_CHUNK_BYTES + 2) / 2];
    size_t received = xStreamBufferReceive(s_source->queue, input, sizeof(input), pdMS_TO_TICKS(20));
    if (!received) {
        if (podcast_pcm_end_ready(s_source->done, xStreamBufferBytesAvailable(s_source->queue))) {
            if (!s_source->complete || !podcast_pcm_complete(&s_source->framing)) {
                fail("网络音频中断"); return;
            }
            s_session.draining = true;
            s_session.drain_until_us = esp_timer_get_time() + 250000;
        } else if (!s_source->done) {
            heard_cursor();
            ++s_session.underruns;
            s_session.state = PODCAST_PLAYER_BUFFERING;
            s_session.buffering_since_us = 0; /* 重缓冲从新窗口起算 */
            publish();
        }
        return;
    }
    size_t produced;
    if (!podcast_pcm_accept(&s_source->framing, input, received, (uint8_t *)output,
                            sizeof(output), &produced)) { fail("音频数据错误"); return; }
    if (produced && bsp_audio_write(output, produced) != ESP_OK) { fail("播放失败"); return; }
    s_source->submitted += produced;
    if (produced) s_source->last_submit_us = esp_timer_get_time();
    heard_cursor();
    publish();
    int64_t now = esp_timer_get_time();
    if (now - s_session.last_timing_us >= 10000000) {
        ESP_LOGI(TAG, "PCM timing: audio_ms=%llu wall_ms=%lld underruns=%u buffered_ms=%llu segments_crossed=%u "
                 "free_heap=%u min_heap=%u largest_block=%u worker_stack_min=%u http_stack_min=%u",
                 (unsigned long long)podcast_pcm_duration_ms(s_source->submitted),
                 (long long)((now - s_session.timing_start_us) / 1000), s_session.underruns,
                 (unsigned long long)podcast_pcm_duration_ms(xStreamBufferBytesAvailable(s_source->queue)),
                 s_session.segments_crossed,
                 (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 (unsigned)uxTaskGetStackHighWaterMark(s_source->task));
        s_session.last_timing_us = now;
    }
}

static void command(const player_command_t *request)
{
    if (request->kind == CMD_VOLUME || request->kind == CMD_ADJUST_VOLUME) {
        /* Settle old gain first: unmuting must never count a silent backlog. */
        heard_cursor();
        int target = request->kind == CMD_VOLUME ? request->value :
                     (int)s_session.volume + request->value;
        s_session.volume = (uint8_t)(target < 0 ? 0 : target > 100 ? 100 : target);
        bsp_audio_set_volume(s_session.volume);
        publish();
        return;
    }
    if (request->kind == CMD_START) {
        bool halted = halt_audio();
        close_heard_session();
        free(s_session.lengths);
        s_session.lengths = NULL;
        ++s_session.generation; // A retained old source cannot mutate the new bookmark.
        memcpy(s_session.show_id, request->show_id, sizeof(s_session.show_id));
        memcpy(s_session.episode_id, request->episode_id, sizeof(s_session.episode_id));
        s_session.count = request->count;
        s_session.cursor = request->cursor;
        s_session.plan = PLAN_NONE;
        s_session.underruns = s_session.segments_crossed = 0;
        s_session.heard_bytes = 0;
        if (!halted) {
            s_session.state = PODCAST_PLAYER_ERROR;
            s_session.error = "停止尚未完成";
            publish();
            return;
        }
        s_session.lengths = calloc(request->count, sizeof(*s_session.lengths));
        if (!s_session.lengths) {
            s_session.state = PODCAST_PLAYER_ERROR;
            s_session.error = "内存不足";
            publish();
            return;
        }
        s_session.error = NULL;
        s_session.plan = PLAN_START;
        s_session.plan_play = true;
        s_session.state = PODCAST_PLAYER_BUFFERING;
        publish();
        return;
    }
    if (!s_session.count) return;
    if (request->kind == CMD_PAUSE) {
        if (s_session.state != PODCAST_PLAYER_PLAYING &&
            s_session.state != PODCAST_PLAYER_BUFFERING) return;
        if (!halt_audio()) { stop_error(); return; }
        s_session.plan = PLAN_NONE;
        s_session.state = PODCAST_PLAYER_PAUSED;
        s_session.error = NULL;
        publish();
    } else if (request->kind == CMD_RESUME) {
        /* UI commits seek and resume together. The worker drains command input
         * before resolving that seek, so resume must update its pending intent. */
        if (s_session.plan == PLAN_SEEK || s_session.plan == PLAN_SEEK_TO) {
            s_session.plan_play = true;
            s_session.state = PODCAST_PLAYER_BUFFERING;
            s_session.error = NULL;
            publish();
            return;
        }
        if (s_session.state != PODCAST_PLAYER_PAUSED) return;
        s_session.plan = PLAN_START;
        s_session.plan_play = true;
        s_session.state = PODCAST_PLAYER_BUFFERING;
        s_session.error = NULL;
        publish();
    } else if (request->kind == CMD_STOP) {
        if (!halt_audio()) { stop_error(); return; }
        close_heard_session();
        s_session.plan = PLAN_NONE;
        s_session.state = PODCAST_PLAYER_IDLE;
        s_session.error = NULL;
        publish();
    } else if (request->kind == CMD_SEEK_TO) {
        if (!s_session.lengths) return;
        bool was_playing=s_session.state==PODCAST_PLAYER_PLAYING||s_session.state==PODCAST_PLAYER_BUFFERING;
        if(!halt_audio()){stop_error();return;}
        s_session.seek_to_ms=request->cursor.byte_offset;
        s_session.plan_play=was_playing;s_session.plan=PLAN_SEEK_TO;
        s_session.state=PODCAST_PLAYER_BUFFERING;s_session.error=NULL;publish();
    } else if (request->kind == CMD_SEEK) {
        if (!s_session.lengths) return;
        bool accumulating = s_session.plan == PLAN_SEEK;
        bool was_playing = s_session.state == PODCAST_PLAYER_PLAYING ||
                           s_session.state == PODCAST_PLAYER_BUFFERING;
        if (!halt_audio()) { stop_error(); return; }
        if (!accumulating) {
            s_session.seek_base = s_session.cursor;
            s_session.seek_seconds = 0;
            s_session.plan_play = was_playing;
        }
        int64_t sum = (int64_t)s_session.seek_seconds + request->value;
        s_session.seek_seconds = sum > INT_MAX ? INT_MAX : sum < INT_MIN ? INT_MIN : (int)sum;
        s_session.plan = PLAN_SEEK;
        s_session.state = PODCAST_PLAYER_BUFFERING;
        s_session.error = NULL;
        publish();
    }
}

static void player_worker(void *arg)
{
    (void)arg;
    player_command_t request;
    for (;;) {
        atomic_fetch_add(&s_heartbeat, 1); /* 心跳：见 s_heartbeat 注释 */
        if (!queue_intact()) {
            ESP_LOGE(TAG, "PCM queue state corrupted (available=%u capacity=%u); rebooting",
                     (unsigned)xStreamBufferBytesAvailable(s_queue),
                     (unsigned)PLAYER_QUEUE_BYTES);
            vTaskDelay(pdMS_TO_TICKS(150));
            esp_restart();
        }
        for (unsigned i = 0; i < PLAYER_COMMAND_DEPTH &&
             xQueueReceive(s_commands, &request, 0) == pdTRUE; ++i) command(&request);
        if (s_source && s_source->cancel) {
            (void)reap_source(0);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (s_session.plan != PLAN_NONE) plan_step();
        else if (s_source) consume_source();
        else if (xQueueReceive(s_commands, &request, pdMS_TO_TICKS(100)) == pdTRUE) command(&request);
    }
}

bool podcast_player_init(const char *relay_base_url)
{
    if (!relay_base_url || !*relay_base_url || strlen(relay_base_url) >= sizeof(s_relay)) return false;
    size_t length = strlen(relay_base_url);
    while (length && relay_base_url[length - 1] == '/') --length;
    if (!length) return false;
    if (s_worker) return strlen(s_relay) == length && strncmp(s_relay, relay_base_url, length) == 0;
    memcpy(s_relay, relay_base_url, length);
    s_relay[length] = '\0';
    /* Boot-time creation from static storage: the 32KB queue never depends on
     * runtime heap state again. Fails only if called twice with a new URL. */
    if (!s_queue) s_queue = xStreamBufferCreateStatic(
        PLAYER_QUEUE_BYTES, 1, s_queue_store, &s_queue_object);
    s_commands = xQueueCreate(PLAYER_COMMAND_DEPTH, sizeof(player_command_t));
    if (!s_commands || !s_queue) return false;
    if (xTaskCreate(player_worker, "podcast_player", 6144, NULL, 4, &s_worker) != pdPASS) {
        vQueueDelete(s_commands);
        s_commands = NULL;
        s_worker = NULL;
        return false;
    }
    return true;
}

static bool submit(const player_command_t *request)
{
    return s_worker && s_commands && xQueueSend(s_commands, request, 0) == pdTRUE;
}

static bool valid_id(const char *id, size_t capacity)
{
    if (!id || !*id || strlen(id) >= capacity) return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    return true;
}

bool podcast_player_start(const char *show_id, const char *episode_id,
                          podcast_player_cursor_t cursor, uint32_t segment_count)
{
    player_command_t request = {.kind = CMD_START, .cursor = cursor, .count = segment_count};
    if (!valid_id(show_id, sizeof(request.show_id)) || !valid_id(episode_id, sizeof(request.episode_id)) ||
        !segment_count || segment_count > PLAYER_MAX_SEGMENTS || cursor.segment >= segment_count ||
        (cursor.byte_offset & 1)) return false;
    memcpy(request.show_id, show_id, strlen(show_id) + 1);
    memcpy(request.episode_id, episode_id, strlen(episode_id) + 1);
    return submit(&request);
}

bool podcast_player_pause(void) { const player_command_t c = {.kind = CMD_PAUSE}; return submit(&c); }
bool podcast_player_resume(void) { const player_command_t c = {.kind = CMD_RESUME}; return submit(&c); }
bool podcast_player_seek_relative(int seconds)
{ const player_command_t c = {.kind = CMD_SEEK, .value = seconds}; return submit(&c); }
bool podcast_player_seek_to_ms(uint64_t position_ms)
{ const player_command_t c = {.kind=CMD_SEEK_TO,.cursor={.byte_offset=position_ms}}; return submit(&c); }
bool podcast_player_stop(void) { const player_command_t c = {.kind = CMD_STOP}; return submit(&c); }
bool podcast_player_set_volume(uint8_t percent)
{
    if (percent > 100) return false;
    const player_command_t c = {.kind = CMD_VOLUME, .value = percent};
    return submit(&c);
}

bool podcast_player_adjust_volume(int delta)
{
    if (delta < -100 || delta > 100) return false;
    const player_command_t c = {.kind = CMD_ADJUST_VOLUME, .value = delta};
    return submit(&c);
}

void podcast_player_poll(void)
{
    /* ①-b 防御（外层）：worker 冻结检测。仅在"连续轮询"的语境下计时：
     * 两次 poll 相隔 >5s（休眠唤醒、UI 长时间停摆）时重新装订基线，
     * 避免 worker 与监视者一起睡眠后被误判为冻结。连续 30s 心跳纹丝不动
     * = worker 死在一次调用里，按键会永远堆在队列里 → 记录并重启自愈。 */
    static uint32_t watched;
    static int64_t since_beat_us, since_poll_us;
    if (!s_worker) return;
    int64_t now = esp_timer_get_time();
    if (since_poll_us && now - since_poll_us > 5000000LL) since_beat_us = 0;
    since_poll_us = now;
    uint32_t beat = atomic_load(&s_heartbeat);
    if (beat != watched || !since_beat_us) {
        watched = beat;
        since_beat_us = now;
        return;
    }
    if (now - since_beat_us > PLAYER_HEARTBEAT_FROZEN_US) {
        ESP_LOGE(TAG, "player worker frozen for %lld ms (state=%d plan=%d source=%d); rebooting",
                 (long long)((now - since_beat_us) / 1000),
                 (int)s_session.state, (int)s_session.plan, s_source ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
    }
}

bool podcast_player_snapshot(podcast_player_snapshot_t *snapshot)
{
    if (!snapshot || !s_worker) return false;
    portENTER_CRITICAL(&s_snapshot_lock);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);
    return true;
}
