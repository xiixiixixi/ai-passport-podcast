#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "podcast_config.h"
#include "podcast_sync.h"
#include "podcast_controller_stubs/cJSON.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
typedef enum { BSP_BTN_UP, BSP_BTN_DOWN, BSP_BTN_OK } bsp_btn_t;
typedef enum { BSP_BTN_PRESS, BSP_BTN_CLICK, BSP_BTN_DOUBLE, BSP_BTN_LONG } bsp_btn_ev_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef unsigned TickType_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
QueueHandle_t xQueueCreate(unsigned, unsigned);
void vQueueDelete(QueueHandle_t);
int xQueueSend(QueueHandle_t, const void *, unsigned);
int xQueueReceive(QueueHandle_t, void *, unsigned);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
void vTaskDelay(unsigned);
int64_t esp_timer_get_time(void);
uint32_t esp_random(void);
void esp_restart(void);
bool bsp_lvgl_lock(int);
void bsp_lvgl_unlock(void);
typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
extern const char setup_wifi_event_base[], setup_ip_event_base[];
#define WIFI_EVENT setup_wifi_event_base
#define IP_EVENT setup_ip_event_base
#define WIFI_EVENT_STA_DISCONNECTED 1
#define IP_EVENT_STA_GOT_IP 2
#define ESP_EVENT_ANY_ID -1
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t,int,void (*)(void*,esp_event_base_t,int32_t,void*),void*,esp_event_handler_instance_t*);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t,int,esp_event_handler_instance_t);
typedef struct { int value; } esp_netif_t;
esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
void esp_netif_destroy_default_wifi(void *);
typedef struct { int value; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() {0}
typedef struct { struct { uint8_t ssid[32],password[64]; } sta; struct { uint8_t ssid[32],password[64];unsigned ssid_len,authmode,channel,max_connection;struct{bool required;}pmf_cfg; } ap; } wifi_config_t;
#define WIFI_MODE_APSTA 3
#define WIFI_IF_STA 0
#define WIFI_IF_AP 1
#define WIFI_STORAGE_RAM 0
#define WIFI_AUTH_WPA2_PSK 3
esp_err_t esp_wifi_init(const wifi_init_config_t*);
/* wifi_get() scans nearby networks for the setup page JSON. */
typedef struct { int unused; } wifi_scan_config_t;
typedef struct { uint8_t ssid[32]; int8_t rssi; } wifi_ap_record_t;
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t*, bool);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t*, wifi_ap_record_t*);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_set_config(int,const wifi_config_t*);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_connect(void);
typedef void *httpd_handle_t;
typedef struct { size_t content_len; } httpd_req_t;
typedef struct { int max_open_sockets,backlog_conn,stack_size,recv_wait_timeout,send_wait_timeout;bool lru_purge_enable;}httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() {0}
#define HTTP_GET 0
#define HTTP_POST 1
#define HTTPD_RESP_USE_STRLEN -1
typedef struct{const char*uri;int method;esp_err_t(*handler)(httpd_req_t*);}httpd_uri_t;
esp_err_t httpd_start(httpd_handle_t*,const httpd_config_t*);
esp_err_t httpd_stop(httpd_handle_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t,const httpd_uri_t*);
esp_err_t httpd_resp_set_status(httpd_req_t*,const char*);
esp_err_t httpd_resp_set_type(httpd_req_t*,const char*);
esp_err_t httpd_resp_set_hdr(httpd_req_t*,const char*,const char*);
esp_err_t httpd_resp_send(httpd_req_t*,const char*,int);
esp_err_t httpd_resp_send_chunk(httpd_req_t*,const char*,int);
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t*,const char*);
int httpd_req_recv(httpd_req_t*,char*,size_t);
typedef void *esp_http_client_handle_t;
#define HTTP_METHOD_POST 1
typedef struct{const char*url;int method,timeout_ms,buffer_size;void(*crt_bundle_attach)(void);bool disable_auto_redirect;}esp_http_client_config_t;
void esp_crt_bundle_attach(void);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t,const char*,const char*);
esp_err_t esp_http_client_open(esp_http_client_handle_t,int);
int esp_http_client_write(esp_http_client_handle_t,const char*,int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t,char*,int);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
typedef struct{const char*server;}esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(x) {x}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t*);
void esp_netif_sntp_deinit(void);
cJSON *cJSON_CreateObject(void);
void cJSON_AddStringToObject(cJSON*,const char*,const char*);
void cJSON_AddBoolToObject(cJSON*,const char*,bool);
char *cJSON_PrintUnformatted(const cJSON*);
