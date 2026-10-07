#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PODCAST_SETUP_TEST
#include "../main/podcast_setup.c"

static unsigned queues,locks,webs,handlers,netifs,wifi_live,http_live,saves,restarts;
const char setup_wifi_event_base[] = "wifi", setup_ip_event_base[] = "ip";
static bool have_old,wifi_success=true,save_success=true,stop_success=true,inject, injected;
static bool cancel_after_error; static int response_code=200;
static int64_t clock_us; static jmp_buf restarted;
static podcast_config_t old_config; static setup_input_t form;
static const char *returned_device="device-1234567890abcdef";
static const char *returned_token="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static cJSON response, id_item, token_item;
static int queue_value,lock_value,web_value,client_value;
static esp_netif_t sta_value,ap_value;
static char last_message[128];

QueueHandle_t xQueueCreate(unsigned n,unsigned size){assert(n==1&&size==sizeof(setup_input_t));queues++;return &queue_value;}
void vQueueDelete(QueueHandle_t q){assert(q==&queue_value&&queues==1);queues--;}
int xQueueSend(QueueHandle_t q,const void*in,unsigned timeout){assert(q&&timeout==0);form=*(const setup_input_t*)in;inject=true;return pdTRUE;}
int xQueueReceive(QueueHandle_t q,void*out,unsigned timeout)
{
    assert(q&&timeout==100);clock_us+=100000;
    if(inject&&!injected){memcpy(out,&form,sizeof(form));injected=true;return pdTRUE;}
    if(cancel_after_error&&injected&&!atomic_load(&busy))podcast_setup_key(BSP_BTN_OK,BSP_BTN_LONG);
    return 0;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void){locks++;return &lock_value;}
int xSemaphoreTake(SemaphoreHandle_t s,unsigned ms){(void)ms;assert(s&&locks);return pdTRUE;}
int xSemaphoreGive(SemaphoreHandle_t s){assert(s&&locks);return pdTRUE;}
void vSemaphoreDelete(SemaphoreHandle_t s){assert(s&&locks==1);locks--;}
void vTaskDelay(unsigned ms){clock_us+=(int64_t)ms*1000;}
int64_t esp_timer_get_time(void){return clock_us;}
uint32_t esp_random(void){return 0x12345678;}
void esp_restart(void){restarts++;longjmp(restarted,1);}
bool bsp_lvgl_lock(int ms){(void)ms;return true;} void bsp_lvgl_unlock(void){}
bool podcast_setup_screen_create(const char*h,const char*p,bool cancel){(void)cancel;assert(!strncmp(h,"Podcast-",8)&&strlen(p)==8);return true;}
void podcast_setup_screen_message(const char*s){snprintf(last_message,sizeof(last_message),"%s",s);}
void podcast_setup_screen_delete(void){}
const podcast_config_t *podcast_config_get(void){return have_old?&old_config:NULL;}
bool podcast_config_save(const podcast_config_t*c){saves++;assert(!strcmp(c->device_id,returned_device));return save_success;}
bool podcast_sync_init(podcast_sync_t*s,uint64_t seed){(void)seed;memset(s,0,sizeof(*s));s->client_seed=0x1234567890abcdefULL;s->ready=true;return true;}
/* Production validators are exercised separately by test_podcast_config. */
bool podcast_config_network(const char*a,const char*b){return a&&*a&&b;}
bool podcast_config_server(char*out,size_t cap,const char*s){if(!s||strlen(s)>=cap)return false;strcpy(out,s);return true;}
bool podcast_config_code(const char*s){return s&&strlen(s)==6;}
bool podcast_config_claim(podcast_config_t*c,const char*i,const char*t){if(!i||!t||strlen(i)!=23||strlen(t)!=64)return false;strcpy(c->device_id,i);strcpy(c->token,t);return true;}
esp_err_t esp_event_loop_create_default(void){return ESP_OK;}
esp_err_t esp_event_handler_instance_register(esp_event_base_t b,int i,void(*fn)(void*,esp_event_base_t,int32_t,void*),void*a,esp_event_handler_instance_t*out)
{(void)b;(void)i;assert(fn==network_event&&!a);handlers++;*out=(void*)(uintptr_t)handlers;return ESP_OK;}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t b,int i,esp_event_handler_instance_t h){(void)b;(void)i;assert(h&&handlers);handlers--;return ESP_OK;}
esp_err_t esp_netif_init(void){return ESP_OK;}
esp_netif_t *esp_netif_create_default_wifi_sta(void){netifs++;return &sta_value;}
esp_netif_t *esp_netif_create_default_wifi_ap(void){netifs++;return &ap_value;}
void esp_netif_destroy_default_wifi(void*n){assert(n&&netifs);netifs--;}
esp_err_t esp_wifi_init(const wifi_init_config_t*c){assert(c&&!wifi_live);wifi_live++;return ESP_OK;}
esp_err_t esp_wifi_set_storage(int m){assert(m==WIFI_STORAGE_RAM);return ESP_OK;}
esp_err_t esp_wifi_set_mode(int m){assert(m==WIFI_MODE_APSTA);return ESP_OK;}
esp_err_t esp_wifi_set_config(int iface,const wifi_config_t*c){assert(c);if(iface==WIFI_IF_AP)assert(c->ap.authmode==WIFI_AUTH_WPA2_PSK&&c->ap.max_connection==1);return ESP_OK;}
esp_err_t esp_wifi_start(void){return ESP_OK;}
/* wifi_get() 扫描结果桩：一台固定 SSID 的热点，仅验证转义与分组的编译契约。 */
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t*c,bool block){(void)c;assert(block);return ESP_OK;}
esp_err_t esp_wifi_scan_get_ap_records(uint16_t*n,wifi_ap_record_t*out)
{assert(n&&out&&*n>=1);memcpy(out[0].ssid,"stub-ap",8);out[0].rssi=-40;*n=1;return ESP_OK;}
esp_err_t esp_wifi_stop(void){return ESP_OK;}
esp_err_t esp_wifi_deinit(void){assert(wifi_live==1);wifi_live--;return ESP_OK;}
esp_err_t esp_wifi_disconnect(void){network_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);return ESP_OK;}
esp_err_t esp_wifi_connect(void){if(wifi_success)network_event(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,NULL);return ESP_OK;}
esp_err_t httpd_start(httpd_handle_t*out,const httpd_config_t*c){assert(c->max_open_sockets==3&&c->stack_size==6144);webs++;*out=&web_value;return ESP_OK;}
esp_err_t httpd_stop(httpd_handle_t w){assert(w&&webs==1);if(!stop_success)return ESP_FAIL;webs--;return ESP_OK;}
esp_err_t httpd_register_uri_handler(httpd_handle_t w,const httpd_uri_t*u){assert(w&&u->handler);return ESP_OK;}
esp_err_t httpd_resp_set_status(httpd_req_t*r,const char*s){(void)r;(void)s;return ESP_OK;}
esp_err_t httpd_resp_set_type(httpd_req_t*r,const char*s){(void)r;(void)s;return ESP_OK;}
esp_err_t httpd_resp_set_hdr(httpd_req_t*r,const char*k,const char*v){(void)r;(void)k;(void)v;return ESP_OK;}
esp_err_t httpd_resp_send(httpd_req_t*r,const char*s,int n){(void)r;(void)s;(void)n;return ESP_OK;}
esp_err_t httpd_resp_send_chunk(httpd_req_t*r,const char*s,int n){(void)r;(void)s;(void)n;return ESP_OK;}
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t*r,const char*s){(void)r;(void)s;return ESP_OK;}
int httpd_req_recv(httpd_req_t*r,char*s,size_t n){(void)r;(void)s;(void)n;return -1;}
void esp_crt_bundle_attach(void){}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*c)
{assert(c->crt_bundle_attach==esp_crt_bundle_attach&&c->disable_auto_redirect&&strstr(c->url,"/api/devices/claim")&&!strstr(c->url,"aaaa"));http_live++;return &client_value;}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t h,const char*k,const char*v){assert(h&&!strcmp(k,"Content-Type")&&!strcmp(v,"application/json"));return ESP_OK;}
esp_err_t esp_http_client_open(esp_http_client_handle_t h,int n){assert(h&&n>0);return ESP_OK;}
int esp_http_client_write(esp_http_client_handle_t h,const char*s,int n){assert(h&&strstr(s,"device-1234567890abcdef"));return n;}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h){assert(h);return 5;}
int esp_http_client_get_status_code(esp_http_client_handle_t h){assert(h);return response_code;}
int esp_http_client_read(esp_http_client_handle_t h,char*s,int n){assert(h&&n==5);memcpy(s,"claim",5);return 5;}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h){assert(h&&http_live==1);http_live--;return ESP_OK;}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t*c){assert(c);return ESP_OK;}
void esp_netif_sntp_deinit(void){}
cJSON *cJSON_Parse(const char*s)
{assert(!strcmp(s,"claim"));response=(cJSON){.child=&id_item};id_item=(cJSON){.next=&token_item,.string="device_id",.valuestring=(char*)returned_device,.type=JSON_STRING};token_item=(cJSON){.string="token",.valuestring=(char*)returned_token,.type=JSON_STRING};return &response;}
void cJSON_Delete(cJSON*j){(void)j;}
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON*j,const char*k){for(cJSON*v=j?j->child:NULL;v;v=v->next)if(!strcmp(v->string,k))return v;return NULL;}
int cJSON_IsString(const cJSON*j){return j&&j->type==JSON_STRING;}
cJSON *cJSON_CreateObject(void){return &response;}
void cJSON_AddStringToObject(cJSON*j,const char*k,const char*v){(void)j;(void)k;(void)v;}
void cJSON_AddBoolToObject(cJSON*j,const char*k,bool v){(void)j;(void)k;(void)v;}
char *cJSON_PrintUnformatted(const cJSON*j){(void)j;char*p=malloc(3);memcpy(p,"{}",3);return p;}
static void assert_clean(void){assert(!queues&&!locks&&!webs&&!handlers&&!netifs&&!wifi_live&&!http_live&&!podcast_setup_active());}
static void prepare(bool old)
{
    assert_clean();have_old=old;inject=true;injected=false;cancel_after_error=false;wifi_success=true;save_success=true;response_code=200;
    form=(setup_input_t){.cfg={.ssid="test-network",.password="test-only-password",.server="http://relay.test:8899"},.code="012345"};
    strcpy(form.cfg.device_id,"device-1234567890abcdef");screen_ready=false;clock_us=0;
}
int main(void)
{
    prepare(false);
    if(!setjmp(restarted)){assert(podcast_setup_run());assert(false);} assert(saves==1);assert_clean();
    assert(!strcmp(last_message,"连接成功，即将进入节目库"));
    unsigned before=saves;prepare(true);wifi_success=false;cancel_after_error=true;
    if(!setjmp(restarted)){podcast_setup_run();assert(false);}assert(saves==before);assert_clean();
    assert(clock_us>=25000000);assert(!strcmp(last_message,"网络连接失败，请检查密码和距离"));
    prepare(true);response_code=401;cancel_after_error=true;
    if(!setjmp(restarted)){podcast_setup_run();assert(false);}assert(saves==before);assert_clean();
    assert(!strcmp(last_message,"配对码无效或已过期，请重新生成"));
    prepare(true);save_success=false;cancel_after_error=true;
    if(!setjmp(restarted)){podcast_setup_run();assert(false);}assert(saves==before+1);assert_clean();
    assert(!strcmp(last_message,"保存失败，原设置仍保留，请重试"));
    prepare(true);returned_device="device-fedcba0987654321";cancel_after_error=true;
    if(!setjmp(restarted)){podcast_setup_run();assert(false);}assert(saves==before+1);assert_clean();returned_device="device-1234567890abcdef";
    for(unsigned cycle=0;cycle<32;cycle++){prepare(true);if(!setjmp(restarted)){podcast_setup_run();assert(false);}assert_clean();}
    submissions=xQueueCreate(1,sizeof(setup_input_t));status_lock=xSemaphoreCreateMutex();assert(start_network()&&start_web());
    stop_success=false;assert(!cleanup());assert(submissions&&status_lock&&web&&queues==1&&locks==1&&webs==1);
    stop_success=true;assert(cleanup());assert_clean();
    puts("Real setup lifecycle: first-use success, wrong Wi-Fi timeout, rejected code, wrong device response, save failure preserves settings, physical cancel, 32 restarts, failed server stop retains callbacks' queue/lock until safe cleanup PASS");
}
