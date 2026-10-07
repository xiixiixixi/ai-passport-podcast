#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_TYPE_MISMATCH 0x1103
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
void controller_log(const char *, const char *, ...);
const char *esp_err_to_name(esp_err_t);
#define ESP_LOGI(...) controller_log(__VA_ARGS__)
#define ESP_LOGW(...) controller_log(__VA_ARGS__)
#define ESP_LOGE(...) controller_log(__VA_ARGS__)
typedef enum { BSP_BTN_UP, BSP_BTN_DOWN, BSP_BTN_OK } bsp_btn_t;
typedef enum { BSP_BTN_PRESS, BSP_BTN_CLICK, BSP_BTN_DOUBLE, BSP_BTN_LONG } bsp_btn_ev_t;
void bsp_audio_set_volume(uint8_t);
int bsp_battery_soc(void);
typedef int BaseType_t;
typedef unsigned TickType_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef void (*TaskFunction_t)(void *);
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
BaseType_t xTaskCreate(TaskFunction_t, const char *, unsigned, void *, unsigned, TaskHandle_t *);
void vTaskSuspend(TaskHandle_t);
void vTaskDelete(TaskHandle_t);
void vTaskDelay(TickType_t);
QueueHandle_t xQueueCreate(unsigned, unsigned);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
void vQueueDelete(QueueHandle_t);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
void vSemaphoreDelete(SemaphoreHandle_t);
typedef struct { int unused; } lv_timer_t;
lv_timer_t *lv_timer_create(void (*)(lv_timer_t *), unsigned, void *);
void lv_timer_delete(lv_timer_t *);
int64_t esp_timer_get_time(void);
typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
extern const char controller_wifi_event_base[], controller_ip_event_base[];
#define WIFI_EVENT controller_wifi_event_base
#define IP_EVENT controller_ip_event_base
#define WIFI_EVENT_STA_DISCONNECTED 1
#define IP_EVENT_STA_GOT_IP 1
#define WIFI_MODE_STA 1
#define WIFI_IF_STA 1
#define WIFI_PS_NONE 0
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(x) (unsigned)((x)->addr & 255), 0U, 0U, 0U
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { struct { esp_ip4_addr_t ip; } ip_info; } ip_event_got_ip_t;
typedef struct { int unused; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() {0}
typedef struct { struct { uint8_t ssid[32], password[64]; } sta; } wifi_config_t;
esp_err_t esp_netif_init(void);
esp_err_t esp_event_loop_create_default(void);
void *esp_netif_create_default_wifi_sta(void);
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_set_config(int, const wifi_config_t *);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_set_ps(int);
typedef struct { size_t used_entries, free_entries, available_entries, total_entries, namespace_count; } nvs_stats_t;
esp_err_t nvs_flash_init(void);
esp_err_t nvs_get_stats(const char *, nvs_stats_t *);
typedef unsigned nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
esp_err_t nvs_open(const char *, nvs_open_mode_t, nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_commit(nvs_handle_t);
typedef struct controller_http *esp_http_client_handle_t;
typedef enum { HTTP_METHOD_GET, HTTP_METHOD_POST } esp_http_client_method_t;
typedef struct { const char *url; int timeout_ms; esp_http_client_method_t method; int buffer_size; } esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_open(esp_http_client_handle_t, int);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
int esp_http_client_write(esp_http_client_handle_t, const char *, int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t, char *, int);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
