#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
typedef unsigned TickType_t;
typedef int BaseType_t;
typedef void (*TaskFunction_t)(void *);
typedef struct test_player_task *TaskHandle_t;
typedef struct test_player_queue *QueueHandle_t;
typedef struct test_player_semaphore *SemaphoreHandle_t;
typedef struct test_player_buffer *StreamBufferHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(value) (value)
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
void test_player_log(const char *tag, const char *format, ...);
#define ESP_LOGI(...) test_player_log(__VA_ARGS__)
#define ESP_LOGE(...) test_player_log(__VA_ARGS__)
BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *task);
void vTaskDelete(TaskHandle_t task);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task);
void vTaskSuspend(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
QueueHandle_t xQueueCreate(unsigned depth, size_t item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout);
void vQueueDelete(QueueHandle_t queue);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
StreamBufferHandle_t xStreamBufferCreate(size_t capacity, size_t trigger);
/* Static queue contract: the PCM queue is boot-time static storage, and the
 * play path must only reset/borrow it (see podcast_player_init/open_source). */
typedef struct test_player_stream_object { char opaque[32]; } StaticStreamBuffer_t;
StreamBufferHandle_t xStreamBufferCreateStatic(size_t capacity, size_t trigger,
                                               uint8_t *storage, StaticStreamBuffer_t *object);
BaseType_t xStreamBufferReset(StreamBufferHandle_t buffer);
size_t xStreamBufferSend(StreamBufferHandle_t buffer, const void *data, size_t size, TickType_t timeout);
size_t xStreamBufferReceive(StreamBufferHandle_t buffer, void *data, size_t size, TickType_t timeout);
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t buffer);
void vStreamBufferDelete(StreamBufferHandle_t buffer);
int64_t esp_timer_get_time(void);
#define MALLOC_CAP_8BIT 1
uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_minimum_free_heap_size(void);
/* Fatal self-heal path: the host harness counts calls instead of restarting. */
void esp_restart(void);
size_t heap_caps_get_largest_free_block(unsigned flags);
esp_err_t bsp_audio_init(void);
esp_err_t bsp_audio_wake(void);
esp_err_t bsp_audio_set_format(uint32_t rate, uint8_t bits, uint8_t channels);
esp_err_t bsp_audio_sleep(void);
void bsp_audio_set_volume(uint8_t percent);
esp_err_t bsp_audio_write(const void *data, size_t size);

typedef enum { HTTP_METHOD_GET, HTTP_METHOD_HEAD } esp_http_client_method_t;
typedef struct test_player_http *esp_http_client_handle_t;
typedef struct {
    int event_id;
    void *user_data;
    const char *header_key, *header_value;
} esp_http_client_event_t;
#define HTTP_EVENT_ON_HEADER 1
typedef struct {
    const char *url;
    esp_http_client_method_t method;
    int timeout_ms, buffer_size;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int size);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout_ms);
int esp_http_client_read(esp_http_client_handle_t client, char *data, int size);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
