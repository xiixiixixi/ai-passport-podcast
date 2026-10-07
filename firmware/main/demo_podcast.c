// Podcast controller: the service worker owns catalogue/network/storage state.
// The button callback only queues intent; LVGL only renders a copied view.
#include "demo.h"
#include "podcast_ui.h"
#include "podcast_idle.h"
#include "podcast_cover_dynamic.h"
#include "bsp_display.h"
#include "podcast_player.h"
#include "podcast_bookmarks.h"
#include "podcast_sync.h"
#include "podcast_config.h"
#include "podcast_http.h"
#include "esp_system.h"
#include "podcast_wifi_events.h"
#include "podcast_catalogue_limits.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "nvs.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#ifdef ESP_PLATFORM
#include "esp_netif_sntp.h"
#endif
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "podcast";
#define MAX_SHOWS PODCAST_MAX_SHOWS
#define PAGE_SIZE PODCAST_VISIBLE_ROWS
#define MAX_JSON PODCAST_MAX_JSON_BYTES

typedef struct { char id[24], name[65], date[11]; uint64_t published; unsigned episodes, played, completed; uint64_t heard_ms; } show_t;
typedef struct { char id[64], title[241], date[16]; unsigned duration, segments, listen_state; uint64_t position_ms, heard_ms, progress_revision; bool ready; } episode_t;
typedef struct { bsp_btn_t button; bsp_btn_ev_t event; } podcast_key_event_t;
static show_t shows[MAX_SHOWS];
static episode_t episodes[PAGE_SIZE];
static int show_count, show_selection, browse_show, episode_count, episode_total, episode_offset, episode_selection;
/* -1 selects the recent/current card; nonnegative values remain show indices. */
static bool library_had_recent;
static int action_selection, sleep_selection;
static int episode_retry = -1;
static _Atomic podcast_page_t page = PODCAST_SHOWS;
static podcast_bookmark_t current, recent;
static bool has_current, has_recent, preparing, ready_storage;
static unsigned current_duration, current_segments, recent_duration;
static podcast_sync_t sync_state;
static int sync_active=-1;
static int64_t next_sync, next_recent;
static uint64_t current_revision;
static unsigned legacy_slot;
static char last_import_request[48];
static bool sync_seek_pending;
static unsigned sync_seek_position;
static uint64_t sync_seek_heard;
static uint64_t recent_position_ms;
static episode_t newer_episode, older_episode;
static bool browse_oldest_first, play_oldest_first, auto_next = true;
static bool finish_handled, auto_suspended, seek_was_playing, awaiting_session;
static unsigned seek_target;
static unsigned current_newest_index, current_oldest_index, current_total;
static uint32_t start_session, start_completion;
static int64_t volume_changed_at, next_prefetch_attempt;
static uint8_t stored_volume = 55;
static char queued_next_id[64];
static uint8_t volume = 55;
static int battery_percent = -1;
static int64_t sleep_deadline, last_save, last_poll, notice_until;
static podcast_player_snapshot_t player;
static podcast_player_state_t saved_state;
static char notice[80] = "正在连接无线网络";
static volatile bool s_quit;
static QueueHandle_t keys;
static SemaphoreHandle_t view_lock, service_done;
static TaskHandle_t service;
static lv_timer_t *timer;
static podcast_view_t view;
/* Published with view_lock. Browsing may replace view.show_id/show, and the
 * volume page replaces its title. Keep only that small playback context so
 * the UI can return during a blocked HTTP request without reading workers'
 * mutable catalogue/bookmark state or duplicating the full row-heavy view. */
static struct {
    char show_id[24], show[65], title[PODCAST_TITLE_BYTES];
    int64_t sleep_deadline;
    bool available, offline, storage_warning, oldest_first;
} return_view;
static uint32_t revision;
static atomic_int direct_transport;
static atomic_bool resume_intent;
static int resume_record=-1;
static uint32_t resume_seek_floor;
static uint32_t local_resume_session; /* Worker-only confirmation of a queued local resume. */
static bool sync_offline;
#ifdef ESP_PLATFORM
static portMUX_TYPE transport_lock=portMUX_INITIALIZER_UNLOCKED;
#define TRANSPORT_ENTER() portENTER_CRITICAL(&transport_lock)
#define TRANSPORT_EXIT() portEXIT_CRITICAL(&transport_lock)
#else
#define TRANSPORT_ENTER() ((void)0)
#define TRANSPORT_EXIT() ((void)0)
#endif
static bool force_catalogue;
static bool restart_setup, pairing_invalid;
/* One lock-free word couples the 31-bit millisecond stamp and screen intent.
 * A CAS cannot blank a screen after a concurrent button updates its stamp. */
#define INPUT_SLEEP_BIT UINT32_C(0x80000000)
#define INPUT_TIME_MASK UINT32_C(0x7fffffff)
static atomic_uint input_state;
static podcast_idle_wake_t wake_gesture; /* Shared button timer owns this. */
static bool applied_screen_blank;
static uint32_t input_stamp(int64_t now) { return (uint32_t)(now / 1000) & INPUT_TIME_MASK; }
static int64_t input_time(int64_t now, uint32_t word)
{
    uint32_t age = (input_stamp(now) - (word & INPUT_TIME_MASK)) & INPUT_TIME_MASK;
    return now - (int64_t)age * 1000;
}

static const char *actions[] = { "回到播放", "节目库", "找单集", "下一集", "上一集", "调整进度", "连续播放", "从头播放", "睡眠定时", "连接设置", "立即息屏" };
#define ACTION_COUNT ((int)(sizeof(actions) / sizeof(actions[0])))
#define ACTION_CONNECTION 9
static const unsigned sleep_minutes[] = { 0, 15, 30, 60 };

static void copy(char *dst, size_t cap, const char *src)
{
    if (!podcast_bookmark_copy_text(dst, cap, src ? src : "")) dst[0] = 0;
}
static void message(const char *text) { copy(notice, sizeof(notice), text); notice_until = esp_timer_get_time() + 4000000; }
static void publish(void);
static bool matching_player(void);
static bool library_recent_available(void)
{
    return has_recent || (matching_player() && (player.state == PODCAST_PLAYER_PLAYING ||
        player.state == PODCAST_PLAYER_BUFFERING || player.state == PODCAST_PLAYER_PAUSED));
}
static volatile bool s_wifi_ok;
static bool s_net_ready;
static bool s_wifi_initialized, s_wifi_configured, s_wifi_started, s_wifi_power_ready, s_wifi_requested;
static podcast_wifi_events_t s_wifi_events;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_ok = false;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_wifi_ok = true;
    }
}

static bool wifi_connect(void)
{
    if (s_wifi_ok) return !s_quit;

    // WiFi 需要 NVS 存参数,主程序没初始化过,这里必须自己来。
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS unavailable; keeping saved data: %s", esp_err_to_name(nvs));
        return false; /* Never erase saved listening positions automatically. */
    }
    if (nvs != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init 失败: %s", esp_err_to_name(nvs));
        return false;
    }

    if (!s_net_ready) {
        if (esp_netif_init() != ESP_OK) { ESP_LOGE(TAG, "esp_netif_init 失败"); return false; }
        if (esp_event_loop_create_default() != ESP_OK) { ESP_LOGE(TAG, "事件循环失败"); return false; }
        if (!esp_netif_create_default_wifi_sta()) { ESP_LOGE(TAG, "创建 STA 失败"); return false; }
#ifdef ESP_PLATFORM
        const podcast_config_t *connection = podcast_config_get();
        if (connection && !strncmp(connection->server, "https://", 8)) {
            esp_sntp_config_t clock = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            if (esp_netif_sntp_init(&clock) != ESP_OK) { ESP_LOGE(TAG, "Network clock initialization failed"); return false; }
        }
#endif
        s_net_ready = true;
    }

    if (!s_wifi_initialized) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(err));
            return false;
        }
        s_wifi_initialized = true;
    }

    esp_err_t handlers = podcast_wifi_events_register(&s_wifi_events, wifi_event);
    if (handlers != ESP_OK) {
        ESP_LOGE(TAG, "WiFi event registration failed: %s", esp_err_to_name(handlers));
        return false;
    }

    if (!s_wifi_configured) {
        const podcast_config_t *connection = podcast_config_get();
        if (!connection) return false;
        wifi_config_t wc = { 0 };
        memcpy(wc.sta.ssid, connection->ssid, strlen(connection->ssid));
        memcpy(wc.sta.password, connection->password, strlen(connection->password));
#ifdef ESP_PLATFORM
        if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) return false;
#endif

        if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) { ESP_LOGE(TAG, "set_mode 失败"); return false; }
        if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) { ESP_LOGE(TAG, "set_config 失败"); return false; }
        s_wifi_configured = true;
    }
    if (!s_wifi_started) {
        if (esp_wifi_start() != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_start 失败"); return false; }
        s_wifi_started = true;
    }

    // 省电模式会造成数百毫秒的收包停顿 -> 播放断续,播客页全程关掉。
    if (!s_wifi_power_ready) {
        if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) return false;
        s_wifi_power_ready = true;
    }

    if (!s_wifi_requested) {
        if (esp_wifi_connect() != ESP_OK) return false;
        s_wifi_requested = true;
    }
    return s_wifi_ok && !s_quit;
}


/* Bounded catalogue responses: three episodes per page, never a whole feed. */
static cJSON *request(const char *path, const char *body)
{
    if (!s_wifi_ok) return NULL;
    char url[256];
    const podcast_config_t *connection = podcast_config_get();
    if (!connection) return NULL;
    int length = snprintf(url, sizeof(url), "%s%s", connection->server, path);
    if (length < 0 || (size_t)length >= sizeof(url)) return NULL;
    /* A fresh TLS handshake on ESP32-C3 needs a larger bound than plain LAN
     * HTTP. Direct audio transport/volume callbacks still bypass this worker. */
    bool secure = !strncmp(connection->server, "https://", 8);
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = !strncmp(path,"/api/listening/",15)?(secure?2500:350):5000,
        .method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET, .buffer_size = 512 };
    esp_http_client_handle_t client = podcast_http_client(cfg);
    if (!client) return NULL;
    int body_size = body ? (int)strlen(body) : 0;
    if (body) esp_http_client_set_header(client, "Content-Type", "application/json");
    cJSON *json = NULL;
    char *text = NULL;
    if (esp_http_client_open(client, body_size) != ESP_OK) goto done;
    if (body && esp_http_client_write(client, body, body_size) != body_size) goto done;
    int64_t size = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status == 401 || status == 403) { pairing_invalid = true; message("配对失效，长按确定重新设置"); }
    if (status < 200 || status >= 300 || size <= 0 || size > MAX_JSON) goto done;
    text = malloc((size_t)size + 1);
    if (!text) goto done;
    int received = 0;
    while (!s_quit && received < size) {
        int n = esp_http_client_read(client, text + received, (int)size - received);
        if (n <= 0) break;
        received += n;
    }
    if (received == size) { text[received] = 0; json = cJSON_Parse(text); }
 done:
    free(text);
    esp_http_client_cleanup(client);
    return json;
}
static const char *string(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}
static unsigned number(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) && item->valuedouble > 0 && item->valuedouble <= 10000000
        ? (unsigned)item->valuedouble : 0;
}
static uint64_t timestamp_number(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) && item->valuedouble > 0 && item->valuedouble <= 4102444800.0
        ? (uint64_t)item->valuedouble : 0;
}
static uint64_t whole_number(cJSON *obj,const char *key)
{
    cJSON *n=cJSON_GetObjectItemCaseSensitive(obj,key);
    return cJSON_IsNumber(n)&&n->valuedouble>0&&n->valuedouble<=9007199254740991.0?(uint64_t)n->valuedouble:0;
}
static bool identity(char *dst, size_t capacity, const char *src)
{
    size_t length = strlen(src);
    if (!length || length >= capacity) return false;
    for (size_t i = 0; i < length; ++i)
        if (!((src[i] >= 'a' && src[i] <= 'z') || (src[i] >= 'A' && src[i] <= 'Z') ||
              (src[i] >= '0' && src[i] <= '9') || src[i] == '-' || src[i] == '_')) return false;
    memcpy(dst, src, length + 1);
    return true;
}
static bool fetch_shows(void)
{
    char selected_id[24] = {0}, browsing_id[24] = {0};
    bool selected_recent = show_selection < 0 && library_recent_available();
    if (show_selection >= 0 && show_selection < show_count) copy(selected_id, sizeof(selected_id), shows[show_selection].id);
    if (browse_show >= 0 && browse_show < show_count) copy(browsing_id, sizeof(browsing_id), shows[browse_show].id);
    static show_t fresh[MAX_SHOWS];
    memset(fresh,0,sizeof(fresh));int count=0,offset=0,expected=-1;
    do {
        char path[96];snprintf(path,sizeof(path),"/api/shows_lite?offset=%d&limit=%d",offset,PODCAST_SHOW_PAGE_SIZE);
        cJSON *json=request(path,NULL);if(!json){message("节目加载失败，确定重试");return false;}
        cJSON *array=cJSON_GetObjectItemCaseSensitive(json,"s"),*item;
        if(!cJSON_IsArray(array)){cJSON_Delete(json);message("节目目录格式有误");return false;}
        cJSON *total=cJSON_GetObjectItemCaseSensitive(json,"total");bool paged=cJSON_IsNumber(total);
        int available=paged?total->valueint:-1;
        if(paged&&(available<0||available>MAX_SHOWS||(expected>=0&&available!=expected)||number(json,"offset")!=(unsigned)offset)){
            cJSON_Delete(json);message("节目列表已变化，请重试");return false;
        }
        if(paged)expected=available;
        int response_count=cJSON_GetArraySize(array);
        if(paged&&(response_count>PODCAST_SHOW_PAGE_SIZE||(!response_count&&offset<available))){cJSON_Delete(json);message("节目目录格式有误");return false;}
        cJSON_ArrayForEach(item,array){
            if(count==MAX_SHOWS)break;
            if(!identity(fresh[count].id,sizeof(fresh[count].id),string(item,"i")))continue;
            copy(fresh[count].name,sizeof(fresh[count].name),string(item,"n"));
            copy(fresh[count].date,sizeof(fresh[count].date),string(item,"d"));
            fresh[count].published=timestamp_number(item,"p");fresh[count].episodes=number(item,"e");
            fresh[count].heard_ms=whole_number(item,"l");fresh[count].played=number(item,"u");fresh[count].completed=number(item,"c");++count;
        }
        offset+=response_count;cJSON_Delete(json);
        if(!paged||offset>=available)break;
    }while(count<MAX_SHOWS);
    /* Stable insertion keeps same-time shows in server order, unknown dates last. */
    for (int i = 1; i < count; ++i) {
        show_t value = fresh[i]; int at = i;
        while (at > 0 && fresh[at - 1].published < value.published) { fresh[at] = fresh[at - 1]; --at; }
        fresh[at] = value;
    }
    memcpy(shows, fresh, sizeof(shows));
    show_count = count; show_selection = selected_recent ? -1 : 0;
    for (int i = 0; i < count; ++i) {
        if (!strcmp(selected_id, shows[i].id)) show_selection = i;
        if (!strcmp(browsing_id, shows[i].id)) browse_show = i;
    }
    message(count ? "最近更新的节目在前" : "还没有节目，确定重试");
    return count > 0;
}
static bool read_episode(cJSON *item, episode_t *ep)
{
    memset(ep, 0, sizeof(*ep));
    if (!identity(ep->id, sizeof(ep->id), string(item, "id"))) return false;
    copy(ep->title, sizeof(ep->title), string(item, "title"));
    copy(ep->date, sizeof(ep->date), string(item, "pub_date"));
    ep->duration = number(item, "duration");
    ep->segments = number(item, "segments");
    ep->position_ms=whole_number(item,"p");ep->heard_ms=whole_number(item,"l");ep->progress_revision=whole_number(item,"v");ep->listen_state=number(item,"c");
    ep->ready = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "ready"));
    return true;
}
static bool import_bookmark(const podcast_bookmark_t *record)
{
    if(!ready_storage||!sync_state.ready||!s_wifi_ok||!record->show_id[0]||!record->episode_id[0])return false;
    /* Only legacy bookmarks are imported. A known outbox session carries real
     * heard evidence, including zero at mute/seek EOF, and must report itself. */
    for(unsigned i=0;i<PODCAST_SYNC_SLOTS;i++){
        podcast_sync_record_t *r=&sync_state.records[i];
        if(r->used&&!strcmp(r->show_id,record->show_id)&&!strcmp(r->episode_id,record->episode_id))return true;
    }
    uint64_t pos=(uint64_t)record->segment*300000+record->byte_offset/32;
    if(!pos&&!record->finished)return true;
    uint32_t hash=2166136261U;
    const char *ids[]={record->show_id,record->episode_id};
    for(unsigned i=0;i<2;i++){for(const char *p=ids[i];*p;p++)hash=(hash^(uint8_t)*p)*16777619U;hash=(hash^0xff)*16777619U;}
    for(unsigned i=0;i<8;i++)hash=(hash^(uint8_t)(pos>>(8*i)))*16777619U;
    hash=(hash^(record->finished?1U:0U))*16777619U;
    char request_id[48];snprintf(request_id,sizeof(request_id),"legacy-%016llx-%08lx",(unsigned long long)sync_state.client_seed,(unsigned long)hash);
    if(!strcmp(last_import_request,request_id))return true;
    char body[512];snprintf(body,sizeof(body),"{\"client_id\":\"device-%016llx\",\"request_id\":\"%s\",\"episodes\":[{\"show_id\":\"%s\",\"episode_id\":\"%s\",\"position_ms\":%llu,\"completed\":%s}]}",(unsigned long long)sync_state.client_seed,request_id,record->show_id,record->episode_id,(unsigned long long)pos,record->finished?"true":"false");
    cJSON *json=request("/api/listening/import",body);if(!json)return false;
    cJSON *results=cJSON_GetObjectItemCaseSensitive(json,"results");bool ok=cJSON_IsArray(results)&&results->child;
    if(ok)copy(last_import_request,sizeof(last_import_request),request_id);
    cJSON_Delete(json);return ok;
}
static bool sync_send_record(int at)
{
    if(!sync_state.ready||!s_wifi_ok||at<0)return false;
    podcast_sync_record_t *record=&sync_state.records[at];char body[512];cJSON *json;
    if(!record->session_id[0]){
        if(!podcast_sync_creation_body(&sync_state,at,body,sizeof(body)))return false;
        json=request("/api/listening/sessions",body);if(!json)return false;
        bool ok=podcast_sync_opened(&sync_state,at,string(json,"session_id"),whole_number(json,"position_ms"),number(json,"next_seq"),cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json,"stale")));
        cJSON_Delete(json);if(!ok)return false;
    }
    if(!podcast_sync_freeze(&sync_state,at))return false;
    if(!record->pending)return true;
    if(!podcast_sync_event_body(&sync_state,at,body,sizeof(body)))return false;
    json=request("/api/listening/events",body);if(!json)return false;
    bool accepted=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json,"accepted"));
    uint64_t v=whole_number(cJSON_GetObjectItemCaseSensitive(json,"progress"),"revision");
    if(accepted){
        record->stale=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json,"stale"));
        if(at==sync_active)sync_offline=record->stale;
        cJSON *progress=cJSON_GetObjectItemCaseSensitive(json,"progress");
        const char *status=string(progress,"status");unsigned c=!strcmp(status,"completed")?2:!strcmp(status,"in_progress")?1:0;
        if(has_current&&!strcmp(record->show_id,current.show_id)&&!strcmp(record->episode_id,current.episode_id))current_revision=v;
        if(browse_show>=0&&browse_show<show_count&&!strcmp(shows[browse_show].id,record->show_id))
            for(int i=0;i<episode_count;i++)if(!strcmp(episodes[i].id,record->episode_id)){episodes[i].listen_state=c;episodes[i].position_ms=whole_number(progress,"position_ms");episodes[i].heard_ms=whole_number(progress,"listened_ms");episodes[i].progress_revision=v;}
        accepted=podcast_sync_acknowledge(&sync_state,at,v);if(accepted&&!sync_state.records[at].used&&sync_active==at)sync_active=-1;force_catalogue=true;
    }
    cJSON_Delete(json);return accepted;
}
static podcast_sync_state_t sync_audio_state(podcast_player_state_t state)
{
    return state==PODCAST_PLAYER_FINISHED?PODCAST_SYNC_ENDED:state==PODCAST_PLAYER_PLAYING?PODCAST_SYNC_PLAYING:
        state==PODCAST_PLAYER_IDLE||state==PODCAST_PLAYER_ERROR?PODCAST_SYNC_STOPPED:PODCAST_SYNC_PAUSED;
}
static void sync_observe_snapshot(const podcast_player_snapshot_t *s,bool close)
{
    if(!sync_state.ready)return;
    if(local_resume_session&&(!atomic_load(&resume_intent)||s->session_id!=local_resume_session||s->state!=PODCAST_PLAYER_PAUSED)){
        local_resume_session=0;atomic_store(&resume_intent,false);
    }
    for(int i=0;i<PODCAST_SYNC_SLOTS;i++){
        podcast_sync_record_t *r=&sync_state.records[i];
        if(r->used&&r->audio_session&&r->audio_session==s->closed_session_id){
            podcast_sync_state_t ended=r->state==PODCAST_SYNC_ENDED?PODCAST_SYNC_ENDED:PODCAST_SYNC_STOPPED;
            if(s->closed_heard_ms>r->heard_origin_ms+r->heard_ms||s->closed_elapsed_ms!=r->position_ms||r->state!=ended||r->audio_session){
                uint64_t heard=s->closed_heard_ms>=r->heard_origin_ms?s->closed_heard_ms-r->heard_origin_ms:0;
                podcast_sync_observe(&sync_state,i,r->audio_session,s->closed_elapsed_ms,heard,ended,false);
                r->audio_session=0;
                if(!podcast_sync_freeze(&sync_state,i))message("同步记录保存失败");
            }
        }
    }
    if(sync_active<0||!has_current||strcmp(s->show_id,current.show_id)||strcmp(s->episode_id,current.episode_id)||
       (awaiting_session&&s->session_id==start_session))return;
    bool seek=false;
    uint64_t expected=(uint64_t)sync_seek_position*1000+(s->heard_ms>sync_seek_heard?s->heard_ms-sync_seek_heard:0);
    uint64_t difference=s->elapsed_ms>expected?s->elapsed_ms-expected:expected-s->elapsed_ms;
    if(sync_seek_pending&&difference<=1500){seek=true;sync_seek_pending=false;}
    podcast_sync_record_t *r=&sync_state.records[sync_active];if(!r->used)return;
    uint64_t heard=s->heard_ms>=r->heard_origin_ms?s->heard_ms-r->heard_origin_ms:0;
    if(resume_record==sync_active){
        /* The audio worker has not yet committed the precise new position. Do
         * not report its old paused cursor under the new central session. */
        if(!heard&&s->absolute_seek_id==resume_seek_floor){
            if(close||!atomic_load(&resume_intent)||s->state==PODCAST_PLAYER_ERROR||s->state==PODCAST_PLAYER_IDLE){
                podcast_sync_observe(&sync_state,sync_active,0,r->position_ms,0,PODCAST_SYNC_STOPPED,false);
                r->audio_session=0;(void)podcast_sync_freeze(&sync_state,sync_active);
                sync_active=-1;resume_record=-1;atomic_store(&resume_intent,false);
            }
            return;
        }
        resume_record=-1;atomic_store(&resume_intent,false);
    }
    podcast_sync_state_t state=close?(s->state==PODCAST_PLAYER_FINISHED||r->state==PODCAST_SYNC_ENDED?PODCAST_SYNC_ENDED:PODCAST_SYNC_STOPPED):sync_audio_state(s->state);
    bool changed=state!=r->state||seek;
    podcast_sync_observe(&sync_state,sync_active,s->session_id==s->closed_session_id&&s->state==PODCAST_PLAYER_FINISHED?0:s->session_id,s->elapsed_ms,heard,state,seek);
    if(close||changed||esp_timer_get_time()>=next_sync){
        if(!podcast_sync_freeze(&sync_state,sync_active))message("同步记录保存失败");
        next_sync=esp_timer_get_time()+10000000;
        /* Transport runs in the service worker, never in a key callback. */
    }
}
static bool sync_begin_current(bool restart)
{
    if(!sync_state.ready)return true;
    local_resume_session=0;atomic_store(&resume_intent,false);
    podcast_sync_clock(&sync_state,(uint64_t)(esp_timer_get_time()/1000));
    int previous_sync=sync_active;
    if(sync_active>=0){podcast_player_snapshot_t old;if(podcast_player_snapshot(&old))sync_observe_snapshot(&old,true);}
    uint64_t position=(uint64_t)current.segment*300000+current.byte_offset/32;
    bool offline=!s_wifi_ok||podcast_sync_has_offline(&sync_state,current.show_id,current.episode_id)||(!current_revision&&position);
    sync_active=podcast_sync_begin(&sync_state,current.show_id,current.episode_id,position,current_revision,offline,restart);
    if(sync_active<0){sync_active=previous_sync;podcast_player_snapshot_t live;if(podcast_player_snapshot(&live))sync_observe_snapshot(&live,false);message("离线记录待同步，请联网重试");return false;}
    if(s_wifi_ok){
        /* Create once and durably retain its request ID even if the ACK is lost. */
        podcast_sync_record_t *r=&sync_state.records[sync_active];char body[512];
        if(podcast_sync_creation_body(&sync_state,sync_active,body,sizeof(body))){
            cJSON *json=request("/api/listening/sessions",body);
            if(json){bool ok=podcast_sync_opened(&sync_state,sync_active,string(json,"session_id"),whole_number(json,"position_ms"),number(json,"next_seq"),cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json,"stale")));
                cJSON_Delete(json);if(ok&&!offline){current.segment=(uint32_t)(r->position_ms/300000);current.byte_offset=(r->position_ms%300000)*32;current.finished=false;}}
        }
    }
    next_sync=esp_timer_get_time()+10000000;sync_seek_pending=false;return true;
}
static bool request_resume(void)
{
    if(!sync_state.ready){bool ok=podcast_player_resume();if(ok)auto_suspended=false;return ok;}
    TRANSPORT_ENTER();atomic_store(&resume_intent,true);atomic_store(&direct_transport,2);TRANSPORT_EXIT();return true;
}
static void cancel_resume_record(void)
{
    local_resume_session=0;
    if(resume_record>=0){
        podcast_sync_record_t *r=&sync_state.records[resume_record];
        if(r->used){podcast_sync_observe(&sync_state,resume_record,0,r->position_ms,r->heard_ms,PODCAST_SYNC_STOPPED,false);r->audio_session=0;(void)podcast_sync_freeze(&sync_state,resume_record);}
        if(sync_active==resume_record)sync_active=-1;
    }
    resume_record=-1;atomic_store(&resume_intent,false);
}
static void settle_resume_cancel(void)
{
    podcast_player_snapshot_t now;
    if(resume_record>=0&&podcast_player_snapshot(&now)){
        podcast_sync_record_t *r=&sync_state.records[resume_record];
        if(now.session_id==r->audio_session&&(now.heard_ms>r->heard_origin_ms||now.absolute_seek_id!=resume_seek_floor)){
            resume_record=-1;atomic_store(&resume_intent,false);sync_observe_snapshot(&now,false);return;
        }
    }
    cancel_resume_record();
}
static bool paused_record_acknowledged(const podcast_sync_record_t *r,const podcast_player_snapshot_t *paused)
{
    uint64_t heard=paused->heard_ms>=r->heard_origin_ms?paused->heard_ms-r->heard_origin_ms:0;
    return r->used&&r->session_id[0]&&!r->pending&&!r->seek&&r->audio_session==paused->session_id&&
        r->state==PODCAST_SYNC_PAUSED&&r->acked_state==PODCAST_SYNC_PAUSED&&
        r->position_ms==paused->elapsed_ms&&r->acked_position_ms==r->position_ms&&
        r->heard_ms==heard&&r->acked_heard_ms==r->heard_ms;
}
static bool locally_resumable_record(const podcast_sync_record_t *r,const podcast_player_snapshot_t *paused)
{
    return r->used&&r->state!=PODCAST_SYNC_STOPPED&&r->state!=PODCAST_SYNC_ENDED&&r->audio_session==paused->session_id&&
        r->acked_state!=PODCAST_SYNC_STOPPED&&r->acked_state!=PODCAST_SYNC_ENDED&&
        (!r->pending||(r->pending_state!=PODCAST_SYNC_STOPPED&&r->pending_state!=PODCAST_SYNC_ENDED))&&
        !strcmp(r->show_id,paused->show_id)&&!strcmp(r->episode_id,paused->episode_id);
}
static void resume_current_async(void)
{
    if(!atomic_load(&resume_intent)||!has_current||s_quit)return;
    podcast_player_snapshot_t paused;
    if(!podcast_player_snapshot(&paused)||paused.state!=PODCAST_PLAYER_PAUSED||strcmp(paused.show_id,current.show_id)||strcmp(paused.episode_id,current.episode_id)){
        atomic_store(&resume_intent,false);return;
    }
    local_resume_session=0;
    uint64_t position=paused.elapsed_ms,central_revision=current_revision;bool online=false;
    int previous=sync_active;
    bool previous_live=previous>=0&&sync_state.records[previous].used&&
        sync_state.records[previous].state!=PODCAST_SYNC_STOPPED&&sync_state.records[previous].state!=PODCAST_SYNC_ENDED&&
        (!sync_state.records[previous].audio_session||sync_state.records[previous].audio_session==paused.session_id)&&
        !strcmp(sync_state.records[previous].show_id,paused.show_id)&&!strcmp(sync_state.records[previous].episode_id,paused.episode_id);
    bool local_owned=previous_live&&locally_resumable_record(&sync_state.records[previous],&paused);
    message("正在同步续播，再按取消");publish();
    podcast_sync_clock(&sync_state,(uint64_t)(esp_timer_get_time()/1000));
    if(previous_live)sync_observe_snapshot(&paused,false);
    /* A central cursor can lag behind our own immutable in-flight event. Send
     * that event and, at most once more, the newest paused observation before
     * consulting it. A stale ACK proves that another session owns the cursor.
     * Failed delivery keeps the local cursor and every old outbox body intact. */
    bool central_safe=false;
    if(s_wifi_ok&&previous_live&&previous==sync_active){
        for(unsigned attempt=0;attempt<=2;attempt++){
            if(!atomic_load(&resume_intent)||s_quit)return;
            podcast_sync_record_t *old=&sync_state.records[previous];
            if(!old->used||strcmp(old->show_id,current.show_id)||strcmp(old->episode_id,current.episode_id))break;
            if(old->stale||paused_record_acknowledged(old,&paused)){central_safe=true;break;}
            if(attempt==2||!sync_send_record(previous))break;
        }
    }
    central_revision=current_revision;
    if(!atomic_load(&resume_intent)||s_quit)return;
    if(central_safe&&s_wifi_ok){
        char path[160];snprintf(path,sizeof(path),"/api/listening/progress/%s/%s?lite=1",current.show_id,current.episode_id);
        cJSON *j=request(path,NULL);
        if(j){uint64_t revision=whole_number(j,"v");
            /* An empty server must never clear the legacy local bookmark. */
            if(revision||!position)position=whole_number(j,"p");
            central_revision=revision;online=revision||!position;cJSON_Delete(j);}
    }
    if(!atomic_load(&resume_intent)||s_quit)return;
    podcast_sync_clock(&sync_state,(uint64_t)(esp_timer_get_time()/1000));
    if(!online&&local_owned&&previous==sync_active&&resume_record<0&&sync_state.records[previous].state==PODCAST_SYNC_PAUSED&&locally_resumable_record(&sync_state.records[previous],&paused)){
        /* The same physical generation can continue without another outbox
         * slot or a rebased heard total. Never reopen a terminal generation. */
        bool committed=false;
        TRANSPORT_ENTER();
        if(atomic_load(&resume_intent)&&!s_quit&&(!sleep_deadline||esp_timer_get_time()<sleep_deadline)){
            committed=podcast_player_resume();
            if(committed)local_resume_session=paused.session_id;
        }
        if(!committed)atomic_store(&resume_intent,false);
        TRANSPORT_EXIT();
        if(!committed){message("已暂停，确定可重试续播");return;}
        sync_offline=true;auto_suspended=false;next_sync=esp_timer_get_time()+10000000;
        current.segment=(uint32_t)(paused.elapsed_ms/300000);current.byte_offset=(paused.elapsed_ms%300000)*32;current.finished=false;
        message("离线续播，进度待同步");publish();return;
    }
    int at=podcast_sync_begin(&sync_state,current.show_id,current.episode_id,position,central_revision,!online,false);
    if(at<0||!podcast_sync_heard_origin(&sync_state,at,paused.heard_ms)){
        if(at>=0){podcast_sync_observe(&sync_state,at,0,position,0,PODCAST_SYNC_STOPPED,false);(void)podcast_sync_freeze(&sync_state,at);}
        atomic_store(&resume_intent,false);message("离线记录待同步，请联网重试");return;
    }
    /* Preserve the complete old listening total before detaching its physical
     * generation. The new central session starts with zero newly heard sound. */
    if(previous>=0){sync_active=previous;sync_observe_snapshot(&paused,true);sync_state.records[previous].audio_session=0;(void)podcast_sync_freeze(&sync_state,previous);}
    sync_active=resume_record=at;podcast_sync_record_t *r=&sync_state.records[at];
    bool connected=false;
    if(online&&s_wifi_ok&&atomic_load(&resume_intent)){
        char body[512];if(podcast_sync_creation_body(&sync_state,at,body,sizeof(body))){
            cJSON *j=request("/api/listening/sessions",body);
            if(j){connected=podcast_sync_opened(&sync_state,at,string(j,"session_id"),whole_number(j,"position_ms"),number(j,"next_seq"),cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"stale")));cJSON_Delete(j);}
        }
    }
    if(!atomic_load(&resume_intent)||s_quit||(sleep_deadline&&esp_timer_get_time()>=sleep_deadline)){
        cancel_resume_record();message("已取消续播");return;
    }
    if(connected&&r->stale){cancel_resume_record();message("进度已更新，请再按确定续播");return;}
    position=r->position_ms;current_revision=central_revision;sync_offline=!connected;sync_seek_pending=false;
    bool committed=false;resume_seek_floor=paused.absolute_seek_id;
    /* Serialize this tiny, nonblocking audio enqueue with a middle-key cancel.
     * HTTP and NVS are never inside this critical section. */
    TRANSPORT_ENTER();
    if(atomic_load(&resume_intent)&&!s_quit){
        committed=podcast_player_seek_to_ms(position)&&podcast_player_resume();
        if(!committed){(void)podcast_player_pause();atomic_store(&resume_intent,false);}
    }
    TRANSPORT_EXIT();
    if(!committed){cancel_resume_record();message("已暂停，确定可重试续播");return;}
    r->audio_session=paused.session_id;auto_suspended=false;next_sync=esp_timer_get_time()+10000000;
    current.segment=(uint32_t)(position/300000);current.byte_offset=(position%300000)*32;current.finished=false;
    if(sync_offline)message("离线续播，进度待同步");else notice_until=0;
    publish();
}
static void fetch_recent_remote(void)
{
    if(!sync_state.ready||!s_wifi_ok||has_current)return;
    if(has_recent&&podcast_sync_has_offline(&sync_state,recent.show_id,recent.episode_id))return;
    cJSON *json=request("/api/listening/recent?limit=1&lite=1",NULL);if(!json)return;
    cJSON *array=cJSON_GetObjectItemCaseSensitive(json,"s");cJSON *item=cJSON_IsArray(array)?array->child:NULL;
    podcast_bookmark_t remote={0};
    if(item&&identity(remote.show_id,sizeof(remote.show_id),string(item,"i"))&&identity(remote.episode_id,sizeof(remote.episode_id),string(item,"e"))){
        if(!podcast_sync_has_offline(&sync_state,remote.show_id,remote.episode_id)){
            copy(remote.name,sizeof(remote.name),string(item,"n"));copy(remote.title,sizeof(remote.title),string(item,"t"));
            recent_position_ms=whole_number(item,"p");recent_duration=number(item,"d");remote.segment=(uint32_t)(recent_position_ms/300000);remote.byte_offset=(recent_position_ms%300000)*32;remote.finished=number(item,"c")==2;
            recent=remote;has_recent=true;
        }
    }
    cJSON_Delete(json);
}
static bool fetch_episode_page(int selected, const char *focus, bool oldest)
{
    if (browse_show < 0 || browse_show >= show_count || selected < 0) return false;
    char path[240];
    int offset = selected / PAGE_SIZE * PAGE_SIZE;
    if (focus && *focus)
        snprintf(path, sizeof(path), "/api/shows/%s/episodes?episode_id=%s&limit=%d&order=%s&lite=1", shows[browse_show].id, focus, PAGE_SIZE, oldest ? "oldest" : "newest");
    else
        snprintf(path, sizeof(path), "/api/shows/%s/episodes?offset=%d&limit=%d&order=%s&lite=1", shows[browse_show].id, offset, PAGE_SIZE, oldest ? "oldest" : "newest");
    message("正在加载单集"); publish();
    cJSON *json = request(path, NULL);
    if (!json) { episode_retry = selected; message("单集加载失败，确定重试"); return false; }
    cJSON *array = cJSON_GetObjectItemCaseSensitive(json, "episodes"), *item;
    if (!cJSON_IsArray(array)) { cJSON_Delete(json); message("服务需要更新"); return false; }
    episode_t fresh[PAGE_SIZE] = {0}; int count = 0;
    cJSON_ArrayForEach(item, array) {
        if (count >= PAGE_SIZE) break;
        if (read_episode(item, &fresh[count])) ++count;
    }
    int total = (int)number(json, "total");
    cJSON *response_offset = cJSON_GetObjectItemCaseSensitive(json, "offset");
    if (cJSON_IsNumber(response_offset) && response_offset->valueint >= 0) offset = response_offset->valueint;
    if (focus && *focus) {
        cJSON *response_focus = cJSON_GetObjectItemCaseSensitive(json, "focus");
        if (!cJSON_IsNumber(response_focus) || response_focus->valueint < 0) { cJSON_Delete(json); message("单集位置已变化，请重试"); return false; }
        selected = response_focus->valueint;
        int at = selected - offset;
        if (at < 0 || at >= count || strcmp(fresh[at].id, focus)) { cJSON_Delete(json); message("单集位置已变化，请重试"); return false; }
    }
    cJSON_Delete(json);
    memcpy(episodes, fresh, sizeof(episodes));
    episode_total = total; episode_count = count; episode_retry = -1;
    episode_offset = offset; episode_selection = count ? selected : 0;
    if (episode_selection >= offset + count) episode_selection = offset;
    browse_oldest_first = oldest;
    message(count ? "长按上键切换时间顺序" : "这个节目还没有单集");
    return count > 0;
}
static bool fetch_episodes(int selected) { return fetch_episode_page(selected, NULL, browse_oldest_first); }
static void toggle_episode_order(void)
{
    int selected = episode_selection - episode_offset;
    if (selected < 0 || selected >= episode_count) return;
    char id[64]; copy(id, sizeof(id), episodes[selected].id);
    (void)fetch_episode_page(episode_selection, id, !browse_oldest_first);
}
static bool checkpoint(bool force)
{
    if (!has_current) return true;
    if (!ready_storage) return false;
    podcast_player_snapshot_t snapshot;
    if (!podcast_player_snapshot(&snapshot) || snapshot.state == PODCAST_PLAYER_IDLE ||
        strcmp(snapshot.show_id, current.show_id) || strcmp(snapshot.episode_id, current.episode_id)) return true;
    if (awaiting_session && snapshot.session_id == start_session) return true;
    if(resume_record>=0&&sync_active==resume_record&&snapshot.absolute_seek_id==resume_seek_floor&&snapshot.heard_ms<=sync_state.records[resume_record].heard_origin_ms)return true;
    sync_observe_snapshot(&snapshot,false);
    int64_t now = esp_timer_get_time();
    if (!force && now - last_save < 60000000) return true;
    current.segment = snapshot.cursor.segment;
    current.byte_offset = snapshot.cursor.byte_offset;
    current.finished = snapshot.state == PODCAST_PLAYER_FINISHED;
    last_save = now;
    if (podcast_bookmarks_save(&current) == PODCAST_BOOKMARK_OK) {
        recent = current; has_recent = true; recent_position_ms=snapshot.elapsed_seconds*1000;recent_duration=current_duration; last_save = now;
        return true;
    }
    message("进度保存失败，可继续播放"); return false;
}
static bool start_current(bool restart)
{
    if(!sync_begin_current(restart||current.finished))return false;
    podcast_player_cursor_t cursor = { current.segment, current.byte_offset };
    if (restart || current.finished || cursor.segment >= current_segments) cursor = (podcast_player_cursor_t){0};
    podcast_player_snapshot_t live={0};
    if (podcast_player_snapshot(&live)) { start_session = live.session_id; start_completion = live.completion_id; }
    podcast_dynamic_covers_prepare_audio();
    if (!podcast_player_start(current.show_id, current.episode_id, cursor, current_segments)) {
        message("操作忙，请稍后再按"); return false;
    }
    current.segment = cursor.segment; current.byte_offset = cursor.byte_offset; current.finished = false;
    preparing = false; finish_handled = false; auto_suspended = false;
    awaiting_session = true;
    message("正在缓冲音频");
    if (ready_storage && podcast_bookmarks_save(&current) == PODCAST_BOOKMARK_OK) {
        recent = current; has_recent = true;recent_position_ms=(uint64_t)current.segment*300000+current.byte_offset/32;recent_duration=current_duration; last_save = esp_timer_get_time();
    }
    return true;
}
static bool details(bool begin_prepare)
{
    if(sync_active<0)(void)import_bookmark(&current);
    char path[160];
    snprintf(path, sizeof(path), "/api/episodes/%s/%s?lite=1", current.show_id, current.episode_id);
    cJSON *json = request(path, NULL);
    if (!json) {
        message(preparing ? "连接中断，正在自动重试" : "连接失败，确定重试");
        return false;
    }
    current_duration = number(json, "duration");
    current_revision=whole_number(json,"v");
    if(sync_state.ready&&sync_active<0){
        /* An unreported local position is useful only while the server revision
         * has not advanced beyond its base. Older offline sessions may still
         * supplement heard time, but must not pull a newer web resume backward. */
        podcast_sync_record_t *local=NULL;
        for(unsigned i=0;i<PODCAST_SYNC_SLOTS;i++){
            podcast_sync_record_t *r=&sync_state.records[i];
            bool dirty=r->pending||!r->session_id[0]||r->position_ms!=r->acked_position_ms||r->heard_ms!=r->acked_heard_ms||r->state!=r->acked_state;
            if(r->used&&!r->stale&&dirty&&r->base_revision>=current_revision&&!strcmp(r->show_id,current.show_id)&&!strcmp(r->episode_id,current.episode_id)&&(!local||r->request_id>local->request_id))local=r;
        }
        if(local){current.segment=(uint32_t)(local->position_ms/300000);current.byte_offset=(local->position_ms%300000)*32;current.finished=local->state==PODCAST_SYNC_ENDED;}
        else if(current_revision||(!current.segment&&!current.byte_offset&&!current.finished)){
            uint64_t central=whole_number(json,"p");current.segment=(uint32_t)(central/300000);current.byte_offset=(central%300000)*32;current.finished=number(json,"c")==2;
        }
    }
    (void)read_episode(cJSON_GetObjectItemCaseSensitive(json, "newer"), &newer_episode);
    (void)read_episode(cJSON_GetObjectItemCaseSensitive(json, "older"), &older_episode);
    current_newest_index = number(json, "newest_index");
    current_oldest_index = number(json, "oldest_index");
    current_total = number(json, "total");
    if (string(json, "title")[0]) copy(current.title, sizeof(current.title), string(json, "title"));
    cJSON *segments = cJSON_GetObjectItemCaseSensitive(json, "segments");
    current_segments = number(json, "segment_count");
    if (!current_segments && cJSON_IsArray(segments)) current_segments = (unsigned)cJSON_GetArraySize(segments);
    bool ready = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ready"));
    bool failed = strcmp(string(json, "status"), "failed") == 0;
    cJSON_Delete(json);
    if (ready && current_segments && current_segments <= 256) return start_current(false);
    if (ready && current_segments > 256) { preparing = false; message("单集过长，暂时无法播放"); return false; }
    if (!begin_prepare) {
        if (failed) { preparing = false; message("准备失败，确定重试"); }
        else message("正在准备音频");
        return false;
    }
    char body[192];
    snprintf(body, sizeof(body), "{\"show_id\":\"%s\",\"episode_id\":\"%s\"}", current.show_id, current.episode_id);
    json = request("/api/prepare", body);
    if (!json) { message("准备失败，确定重试"); preparing = false; return false; }
    cJSON_Delete(json);
    preparing = true;
    message("正在准备音频");
    last_poll = esp_timer_get_time();
    return true;
}
static const episode_t *next_episode(void) { return play_oldest_first ? &newer_episode : &older_episode; }
static void prepare_next(void)
{
    const episode_t *next = next_episode();
    if (!auto_next || !next->id[0] || !strcmp(queued_next_id, next->id)) return;
    char body[220];
    snprintf(body, sizeof(body), "{\"show_id\":\"%s\",\"episode_id\":\"%s\",\"prefetch\":true}", current.show_id, next->id);
    cJSON *json = request("/api/prepare", body);
    if (json) { copy(queued_next_id, sizeof(queued_next_id), next->id); cJSON_Delete(json); }
}
static void choose_episode_for(const episode_t *ep, const show_t *show, bool oldest, bool automatic)
{
    episode_t target = *ep;
    show_t source = *show;
    podcast_page_t previous_page = page;
    checkpoint(true);
    podcast_player_snapshot_t closing;if(podcast_player_snapshot(&closing))sync_observe_snapshot(&closing,true);
    if (!podcast_player_stop()) { message("操作忙，请稍后再按"); return; }
    podcast_bookmark_t selected = {0};
    if (!ready_storage || podcast_bookmarks_find(source.id, target.id, &selected) != PODCAST_BOOKMARK_OK) {
        copy(selected.show_id, sizeof(selected.show_id), source.id);
        copy(selected.episode_id, sizeof(selected.episode_id), target.id);
    }
    copy(selected.name, sizeof(selected.name), source.name);
    copy(selected.title, sizeof(selected.title), target.title);
    sync_active=-1;current_revision=target.progress_revision;
    current = selected; has_current = true; play_oldest_first = oldest;
    current_duration = target.duration; current_segments = target.segments;
    preparing = false; finish_handled = false; awaiting_session = false; queued_next_id[0] = 0;
    current_newest_index = current_oldest_index = current_total = 0;
    memset(&newer_episode, 0, sizeof(newer_episode)); memset(&older_episode, 0, sizeof(older_episode));
    page = automatic && previous_page != PODCAST_NOW && previous_page != PODCAST_SEEK ? previous_page : PODCAST_NOW;
    message(automatic ? "接着播放下一集" : "正在读取播放进度"); publish();
    (void)details(true);
    if (!preparing) prepare_next();
}
static void choose_episode(const episode_t *ep)
{
    if (browse_show >= 0 && browse_show < show_count) choose_episode_for(ep, &shows[browse_show], browse_oldest_first, false);
}
static void continue_recent(void)
{
    if (matching_player() && (player.state == PODCAST_PLAYER_PLAYING ||
        player.state == PODCAST_PLAYER_BUFFERING || player.state == PODCAST_PLAYER_PAUSED)) {
        page = PODCAST_NOW; notice_until = 0;
        if(player.state==PODCAST_PLAYER_PAUSED)(void)request_resume();
        return;
    }
    if (!has_recent) { message("先选一集，开始后会记住进度"); return; }
    if (has_current && strcmp(current.show_id, recent.show_id) == 0 &&
        strcmp(current.episode_id, recent.episode_id) == 0 &&
        player.state != PODCAST_PLAYER_IDLE && player.state != PODCAST_PLAYER_ERROR &&
        player.state != PODCAST_PLAYER_FINISHED) {
        page = PODCAST_NOW;
        if (player.state == PODCAST_PLAYER_PAUSED) (void)request_resume();
        return;
    }
    podcast_bookmark_t target = recent;
    checkpoint(true);
    podcast_player_snapshot_t closing;if(podcast_player_snapshot(&closing))sync_observe_snapshot(&closing,true);
    if (!podcast_player_stop()) { message("操作忙，请稍后再按"); return; }
    sync_active=-1;current_revision=0;
    current = target; has_current = true; preparing = false; finish_handled = false; awaiting_session = false;
    memset(&newer_episode, 0, sizeof(newer_episode)); memset(&older_episode, 0, sizeof(older_episode));
    current_newest_index = current_oldest_index = current_total = 0;
    queued_next_id[0] = 0; play_oldest_first = browse_oldest_first;
    page = PODCAST_NOW;
    message("正在恢复上次播放"); publish();
    (void)details(true);
    if (!preparing) prepare_next();
}
static bool adjacent(int direction, bool automatic)
{
    if (!has_current) return false;
    const episode_t *target = direction > 0 ? next_episode() : (play_oldest_first ? &older_episode : &newer_episode);
    if (!target->id[0]) { message("已到所选顺序的末尾"); return false; }
    for (int i = 0; i < show_count; ++i) {
        if (!strcmp(shows[i].id, current.show_id)) {
            choose_episode_for(target, &shows[i], play_oldest_first, automatic);
            return true;
        }
    }
    message("这个节目已不在订阅列表"); return false;
}
static void open_current_episodes(void)
{
    for (int i = 0; i < show_count; ++i) if (!strcmp(shows[i].id, current.show_id)) {
        browse_show = i; page = PODCAST_EPISODES;
        (void)fetch_episode_page(0, current.episode_id, play_oldest_first); return;
    }
    message("这个节目已不在订阅列表");
}
static unsigned elapsed(void)
{
    return has_current && !strcmp(current.show_id, player.show_id) && !strcmp(current.episode_id, player.episode_id)
        ? (unsigned)player.elapsed_seconds : current.segment * 300 + (unsigned)(current.byte_offset / 32000);
}
static void begin_seek(void)
{
    if (preparing || !has_current || player.state == PODCAST_PLAYER_IDLE || player.state == PODCAST_PLAYER_ERROR || player.state == PODCAST_PLAYER_FINISHED) { message("准备好音频后可调进度"); return; }
    seek_was_playing = player.state == PODCAST_PLAYER_PLAYING || player.state == PODCAST_PLAYER_BUFFERING;
    if (seek_was_playing && !podcast_player_pause()) { message("操作忙，请稍后再按"); return; }
    seek_target = elapsed(); page = PODCAST_SEEK; notice_until = 0;
}
static void adjust_volume(int direction)
{
    if (!podcast_player_adjust_volume(direction * 5)) message("操作忙，请稍后再按");
    else {
        int target = (int)volume + direction * 5;
        volume = (uint8_t)(target < 0 ? 0 : target > 100 ? 100 : target);
        volume_changed_at = esp_timer_get_time();
        notice_until = 0;
    }
}
static void handle_key(podcast_key_event_t key)
{
    if (key.event != BSP_BTN_CLICK && key.event != BSP_BTN_LONG && key.event != BSP_BTN_DOUBLE) return;
    int direction = key.button == BSP_BTN_UP ? -1 : key.button == BSP_BTN_DOWN ? 1 : 0;
    if (key.event == BSP_BTN_LONG) {
        if (direction) {
            if (page == PODCAST_NOW) begin_seek();
            else if (page == PODCAST_SEEK) {
                int64_t target = (int64_t)seek_target + direction * 60;
                seek_target = (unsigned)(target < 0 ? 0 : current_duration && target > current_duration ? current_duration : target);
            } else if (page == PODCAST_EPISODES) {
                if (direction < 0) toggle_episode_order(); else (void)fetch_episodes(0);
            } else if (page == PODCAST_SHOWS && (show_count || library_recent_available()))
                show_selection = direction < 0 ? (library_recent_available() ? -1 : 0) : show_count - 1;
            return;
        }
        if (key.button != BSP_BTN_OK) return;
        switch (page) {
            case PODCAST_EPISODES: page = PODCAST_SHOWS; break;
            case PODCAST_NOW: page = PODCAST_ACTIONS; action_selection = 0; break;
            case PODCAST_SEEK:
                if (seek_was_playing && !auto_suspended && !podcast_player_resume()) message("操作忙，确定可续播");
                seek_was_playing = false; page = PODCAST_NOW; break;
            case PODCAST_ACTIONS: case PODCAST_VOLUME: case PODCAST_SLEEP: page = has_current ? PODCAST_NOW : PODCAST_SHOWS; break;
            case PODCAST_SHOWS:
                if (pairing_invalid || (!has_current && !has_recent)) { page = PODCAST_ACTIONS; action_selection = ACTION_CONNECTION; }
                else if (has_current) page = PODCAST_NOW;
                else if (has_recent) continue_recent();
                break;
        }
        return;
    }
    switch (page) {
        case PODCAST_SHOWS:
            if (direction) {
                int next = show_selection + direction;
                int first = library_recent_available() ? -1 : 0;
                if (next >= first && next < show_count) show_selection = next;
            } else if (key.button == BSP_BTN_OK) {
                if (show_selection < 0 && library_recent_available()) { continue_recent(); break; }
                if (!show_count) { (void)fetch_shows(); break; }
                browse_show = show_selection;
                episode_selection = episode_offset = episode_count = episode_total = 0; episode_retry = -1;
                page = PODCAST_EPISODES; (void)fetch_episodes(0);
            }
            break;
        case PODCAST_EPISODES:
            if (direction && episode_total) {
                int next = (episode_retry >= 0 ? episode_retry : episode_selection) + direction;
                if (next < 0 || next >= episode_total) break;
                if (next < episode_offset || next >= episode_offset + episode_count) (void)fetch_episodes(next);
                else episode_selection = next;
            } else if (key.button == BSP_BTN_OK) {
                if (episode_retry >= 0) { (void)fetch_episodes(episode_retry); break; }
                int selected = episode_selection - episode_offset;
                if (selected >= 0 && selected < episode_count) choose_episode(&episodes[selected]);
                else (void)fetch_episodes(episode_selection);
            }
            break;
        case PODCAST_NOW:
            if (!has_current) { page = PODCAST_SHOWS; break; }
            if (direction) { adjust_volume(-direction); break; }
            if (preparing) { message("正在准备，可长按选择其它节目"); break; }
            if (key.button == BSP_BTN_OK) {
                bool ok = true;
                if(atomic_load(&resume_intent)){atomic_store(&resume_intent,false);ok=podcast_player_pause();settle_resume_cancel();}
                else if (player.state == PODCAST_PLAYER_PLAYING || player.state == PODCAST_PLAYER_BUFFERING) ok = podcast_player_pause();
                else if (player.state == PODCAST_PLAYER_PAUSED) { ok = request_resume(); }
                else { message("正在重新连接"); publish(); (void)details(true); }
                if (!ok) message("操作忙，请稍后再按"); else notice_until = 0;
            }
            break;
        case PODCAST_SEEK:
            if (direction) {
                int64_t target = (int64_t)seek_target + direction * 15;
                seek_target = (unsigned)(target < 0 ? 0 : current_duration && target > current_duration ? current_duration : target);
            } else if (key.button == BSP_BTN_OK) {
                int delta = (int)seek_target - (int)elapsed();
                podcast_player_snapshot_t live={0};
                uint32_t completion = podcast_player_snapshot(&live) ? live.completion_id : player.completion_id;
                if (podcast_player_seek_relative(delta)) {
                    sync_seek_pending=true;sync_seek_position=seek_target;sync_seek_heard=live.heard_ms;
                    start_completion = completion;
                    finish_handled = false;
                    bool resumed = !seek_was_playing || auto_suspended || podcast_player_resume();
                    seek_was_playing = false; page = PODCAST_NOW;
                    if (resumed) notice_until = 0; else message("跳转已提交，确定续播");
                }
                else message("操作忙，请稍后再按");
            }
            break;
        case PODCAST_ACTIONS:
            if (direction) {
                int next = action_selection + direction;
                if (next >= 0 && next < ACTION_COUNT) action_selection = next;
            } else if (key.button == BSP_BTN_OK) {
                switch (action_selection) {
                    case 0: page = has_current ? PODCAST_NOW : PODCAST_SHOWS; notice_until = 0; break;
                    case 1: page = PODCAST_SHOWS;force_catalogue=true; message("播放会继续，可选择其它节目"); break;
                    case 2: open_current_episodes(); break;
                    case 3: (void)adjacent(1, false); break;
                    case 4: (void)adjacent(-1, false); break;
                    case 5: begin_seek(); break;
                    case 6: auto_next = !auto_next; if (auto_next) prepare_next(); message(auto_next ? "连续播放已开启" : "本集播完后停止"); break;
                    case 7:
                        if (!preparing && current_segments) { if (start_current(true)) page = PODCAST_NOW; }
                        else message("准备完成后可以从头播放");
                        break;
                    case 8: page = PODCAST_SLEEP; sleep_selection = 0; break;
                    case 9:
                        if (!checkpoint(true)) { message("进度保存失败，稍后再设置"); break; }
                        if (sync_state.ready && sync_active >= 0 && !podcast_sync_freeze(&sync_state, sync_active)) { message("同步记录保存失败，稍后再设置"); break; }
                        if (podcast_config_request_setup()) { restart_setup = true; s_quit = true; }
                        else message("设置请求保存失败，请重试");
                        break;
                    case 10:
                        page = has_current ? PODCAST_NOW : PODCAST_SHOWS;
                        atomic_fetch_or(&input_state, INPUT_SLEEP_BIT);
                        break;
                }
            }
            break;
        case PODCAST_VOLUME:
            if (direction) adjust_volume(-direction); else if (key.button == BSP_BTN_OK) page = PODCAST_NOW;
            break;
        case PODCAST_SLEEP:
            if (direction) {
                int next = sleep_selection + direction;
                if (next >= 0 && next < 4) sleep_selection = next;
            } else if (key.button == BSP_BTN_OK) {
                unsigned minutes = sleep_minutes[sleep_selection];
                sleep_deadline = minutes ? esp_timer_get_time() + (int64_t)minutes * 60000000 : 0;
                page = PODCAST_NOW; message(minutes ? "到时间会暂停，并保存进度" : "睡眠定时已关闭");
            }
            break;
    }
}
static bool matching_player(void)
{
    return has_current && !strcmp(current.show_id, player.show_id) && !strcmp(current.episode_id, player.episode_id)
        && !(awaiting_session && player.session_id == start_session);
}
static const char *play_status(void)
{
    if (preparing || !matching_player() || esp_timer_get_time() < notice_until) return notice;
    switch (player.state) {
        case PODCAST_PLAYER_BUFFERING: return "正在缓冲";
        case PODCAST_PLAYER_PLAYING: return sync_offline?"正在播放，进度待同步":"正在播放";
        case PODCAST_PLAYER_PAUSED: return sync_offline?"已暂停，进度待同步":"已暂停，确定接着听";
        case PODCAST_PLAYER_FINISHED: return "本集已播完";
        case PODCAST_PLAYER_ERROR: return player.error ? player.error : "播放失败，确定重试";
        default: return notice;
    }
}
static void publish(void)
{
    static podcast_view_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    const podcast_page_t published_page = atomic_load(&page);
    fresh.recent_is_current = matching_player() && (player.state == PODCAST_PLAYER_PLAYING ||
        player.state == PODCAST_PLAYER_BUFFERING || player.state == PODCAST_PLAYER_PAUSED);
    fresh.has_recent = has_recent || fresh.recent_is_current;
    if (fresh.recent_is_current) {
        copy(fresh.recent_show_id,sizeof(fresh.recent_show_id),current.show_id);
        copy(fresh.recent_name,sizeof(fresh.recent_name),current.name);
        copy(fresh.recent_title,sizeof(fresh.recent_title),current.title);
        fresh.recent_elapsed=(unsigned)player.elapsed_seconds; fresh.recent_duration=current_duration;
    } else if(has_recent){
        copy(fresh.recent_show_id,sizeof(fresh.recent_show_id),recent.show_id);
        copy(fresh.recent_name,sizeof(fresh.recent_name),recent.name);
        copy(fresh.recent_title,sizeof(fresh.recent_title),recent.title);
        fresh.recent_elapsed=(unsigned)(recent_position_ms/1000);fresh.recent_duration=recent_duration;
    }
    fresh.page = published_page; fresh.volume = volume; fresh.battery_percent = battery_percent;
    fresh.duration = current_duration;
    fresh.seek_target = seek_target; fresh.oldest_first = published_page == PODCAST_EPISODES ? browse_oldest_first : play_oldest_first;
    fresh.auto_next = auto_next; fresh.has_next = next_episode()->id[0] != 0;
    copy(fresh.next_title, sizeof(fresh.next_title), next_episode()->title);
    fresh.next_position = (int)(play_oldest_first ? current_oldest_index : current_newest_index) + 2;
    fresh.next_total = (int)current_total;
    fresh.elapsed = matching_player() ? (unsigned)player.elapsed_seconds : current.segment * 300 + (unsigned)(current.byte_offset / 32000);
    fresh.playing = matching_player() && player.state == PODCAST_PLAYER_PLAYING;
    fresh.busy = preparing;fresh.resuming=atomic_load(&resume_intent);
    copy(fresh.show_id, sizeof(fresh.show_id), current.show_id);
    copy(fresh.episode_id, sizeof(fresh.episode_id), current.episode_id);
    fresh.playback_session_floor = start_session; fresh.waiting_for_session = awaiting_session;
    copy(fresh.show, sizeof(fresh.show), current.name);
    copy(fresh.title, sizeof(fresh.title), current.title);
    copy(fresh.status, sizeof(fresh.status), notice);
    switch (published_page) {
        case PODCAST_SHOWS: {
            copy(fresh.heading, sizeof(fresh.heading), "最近更新");
            if(fresh.has_recent&&!library_had_recent)show_selection=-1;
            library_had_recent=fresh.has_recent;
            int first=fresh.has_recent?-1:0;
            if(show_selection<first)show_selection=first;
            if (show_selection >= show_count) show_selection = show_count ? show_count - 1 : first;
            int visible_shows=fresh.has_recent?2:PAGE_SIZE;
            int offset = show_selection<0?0:show_selection / visible_shows * visible_shows;
            fresh.total = show_count; fresh.absolute_selected = show_selection;
            fresh.selected = show_selection<0?-1:show_selection - offset;
            for (int i = 0; i < visible_shows && offset + i < show_count; ++i) {
                int index = offset + i;
                copy(fresh.rows[i].show_id, sizeof(fresh.rows[i].show_id), shows[index].id);
                copy(fresh.rows[i].title, sizeof(fresh.rows[i].title), shows[index].name);
                copy(fresh.rows[i].latest_date, sizeof(fresh.rows[i].latest_date), shows[index].date);
                if (shows[index].date[0]) snprintf(fresh.rows[i].detail, sizeof(fresh.rows[i].detail), "%s · 已听%u集", strlen(shows[index].date)>=10?shows[index].date+5:shows[index].date,shows[index].played);
                else copy(fresh.rows[i].detail, sizeof(fresh.rows[i].detail), "日期未知");
                ++fresh.count;
            }
            copy(fresh.hint, sizeof(fresh.hint), "上下选择  确定进入");
            if (pairing_invalid) copy(fresh.status, sizeof(fresh.status), "配对失效，长按确定重新设置");
            copy(fresh.back_hint, sizeof(fresh.back_hint), pairing_invalid || (!has_current && !has_recent) ? "长按确定：连接设置" : has_current || has_recent ? "长按确定：继续收听" : "长按上键：最近更新");
            break;
        }
        case PODCAST_EPISODES:
            copy(fresh.show_id, sizeof(fresh.show_id), shows[browse_show].id);
            copy(fresh.show, sizeof(fresh.show), shows[browse_show].name);
            copy(fresh.heading, sizeof(fresh.heading), shows[browse_show].name);
            if (episode_retry >= 0 || !episode_count) copy(fresh.status, sizeof(fresh.status), notice);
            else snprintf(fresh.status, sizeof(fresh.status), "第 %d / %d 集 · %s", episode_selection + 1, episode_total, browse_oldest_first ? "旧到新" : "新到旧");
            fresh.total = episode_total; fresh.absolute_selected = episode_selection;
            fresh.selected = episode_selection - episode_offset;
            for (int i = 0; i < episode_count; ++i) {
                copy(fresh.rows[i].show_id, sizeof(fresh.rows[i].show_id), shows[browse_show].id);
                copy(fresh.rows[i].title, sizeof(fresh.rows[i].title), episodes[i].title);
                copy(fresh.rows[i].latest_date, sizeof(fresh.rows[i].latest_date), episodes[i].date);
                snprintf(fresh.rows[i].detail, sizeof(fresh.rows[i].detail), "%s %u分 %s", episodes[i].date, episodes[i].duration / 60,
                    episodes[i].listen_state==2?"听完":episodes[i].listen_state==1?"听过":"未播");
                ++fresh.count;
            }
            copy(fresh.hint, sizeof(fresh.hint), "上下选择  确定播放");
            copy(fresh.back_hint, sizeof(fresh.back_hint), "长按上键：切换时间顺序");
            break;
        case PODCAST_NOW:
            copy(fresh.heading, sizeof(fresh.heading), "正在收听");
            copy(fresh.status, sizeof(fresh.status), play_status());
            copy(fresh.hint, sizeof(fresh.hint), "上加音量  下减音量");
            const char *ok_hint;
            if (preparing) ok_hint = "长按确定：操作菜单";
            else if (!matching_player() || player.state == PODCAST_PLAYER_ERROR || player.state == PODCAST_PLAYER_IDLE) ok_hint = "确定重试 长按菜单";
            else if (player.state == PODCAST_PLAYER_FINISHED) ok_hint = "确定重播 长按菜单";
            else if (player.state == PODCAST_PLAYER_PAUSED) ok_hint = "确定续播 长按菜单";
            else ok_hint = "确定暂停 长按菜单";
            copy(fresh.back_hint, sizeof(fresh.back_hint), ok_hint);
            if (sleep_deadline) {
                int64_t left = sleep_deadline - esp_timer_get_time();
                fresh.sleep_minutes = (unsigned)((left > 0 ? left : 0) / 60000000 + 1);
            }
            break;
        case PODCAST_ACTIONS: {
            snprintf(fresh.heading, sizeof(fresh.heading), "播放操作 %d/%d", action_selection + 1, ACTION_COUNT);
            copy(fresh.status, sizeof(fresh.status), play_status());
            int offset = action_selection / PAGE_SIZE * PAGE_SIZE;
            fresh.selected = action_selection - offset;
            for (int i = 0; i < PAGE_SIZE && offset + i < ACTION_COUNT; ++i) {
                copy(fresh.rows[i].title, sizeof(fresh.rows[i].title), actions[offset + i]);
                const char *detail[] = {"回到当前这集，不改进度", "按最近更新排序", "定位正在播放的单集", "按当前时间方向接着听", "按相反时间方向选择", "先选位置，再确定跳转", auto_next ? "已开启，到列表末尾停止" : "已关闭，本集播完停止", "重新从零开始", "到时间暂停并记住进度", "重新连接网络与后台", "声音继续，任意键唤醒"};
                copy(fresh.rows[i].detail, sizeof(fresh.rows[i].detail), detail[offset + i]);
                ++fresh.count;
            }
            copy(fresh.hint, sizeof(fresh.hint), "上下选择  确定执行");
            copy(fresh.back_hint, sizeof(fresh.back_hint), "长按确定：回到播放");
            break;
        }
        case PODCAST_VOLUME:
            copy(fresh.heading, sizeof(fresh.heading), "调整音量");
            copy(fresh.status, sizeof(fresh.status), "音量会自动记住");
            copy(fresh.title, sizeof(fresh.title), "上键增大，下键减小");
            copy(fresh.hint, sizeof(fresh.hint), "上下调节  每次5");
            copy(fresh.back_hint, sizeof(fresh.back_hint), "确定：回到播放");
            break;
        case PODCAST_SEEK:
            copy(fresh.heading, sizeof(fresh.heading), "调整进度");
            copy(fresh.status, sizeof(fresh.status), "确定跳转，长按确定取消");
            copy(fresh.hint, sizeof(fresh.hint), "上退15秒  下进15秒");
            copy(fresh.back_hint, sizeof(fresh.back_hint), "长按两侧：每次60秒");
            break;
        case PODCAST_SLEEP: {
            snprintf(fresh.heading, sizeof(fresh.heading), "睡眠定时 %d/4", sleep_selection + 1);
            copy(fresh.status, sizeof(fresh.status), "到时间暂停，进度会保存");
            int offset = sleep_selection / PAGE_SIZE * PAGE_SIZE;
            fresh.selected = sleep_selection - offset;
            for (int i = 0; i < PAGE_SIZE && offset + i < 4; ++i) {
                unsigned minutes = sleep_minutes[offset + i];
                if (minutes) snprintf(fresh.rows[i].title, sizeof(fresh.rows[i].title), "%u 分钟后暂停", minutes);
                else copy(fresh.rows[i].title, sizeof(fresh.rows[i].title), "关闭定时");
                ++fresh.count;
            }
            copy(fresh.hint, sizeof(fresh.hint), "上下选择  确定设置");
            copy(fresh.back_hint, sizeof(fresh.back_hint), "长按确定：回到播放");
            break;
        }
    }
    if (!ready_storage && service) {
        char warning[160];
        snprintf(warning, sizeof(warning), "%s · 无法存进度", fresh.status);
        copy(fresh.status, sizeof(fresh.status), warning);
    }
    if (xSemaphoreTake(view_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        /* The independent UI timer may already have returned to NOW while
         * this worker was drafting another page. Never overwrite that return
         * with a stale settings/list view, even after the HTTP wait ends. */
        if (published_page == atomic_load(&page)) {
            copy(return_view.show_id,sizeof(return_view.show_id),current.show_id);
            copy(return_view.show,sizeof(return_view.show),current.name);
            copy(return_view.title,sizeof(return_view.title),current.title);
            return_view.sleep_deadline=sleep_deadline;
            return_view.available=has_current&&!preparing&&!pairing_invalid;
            return_view.offline=sync_offline;
            return_view.storage_warning=!ready_storage&&service;
            return_view.oldest_first=play_oldest_first;
            fresh.revision = view.revision;
            if (memcmp(&view, &fresh, sizeof(view))) { fresh.revision = ++revision; view = fresh; }
        }
        xSemaphoreGive(view_lock);
    }
}
static void service_task(void *arg)
{
    (void)arg;
    esp_err_t nvs = nvs_flash_init();
    ready_storage = nvs == ESP_OK && podcast_bookmarks_init() == PODCAST_BOOKMARK_OK;
    if(ready_storage){
        uint64_t seed=((uint64_t)esp_random()<<32)|esp_random();
        if(!podcast_sync_init(&sync_state,seed))message("同步记录保存失败");
    }
    if (ready_storage) {
        has_recent = podcast_bookmarks_load_recent(&recent) == PODCAST_BOOKMARK_OK;
        if(has_recent)recent_position_ms=(uint64_t)recent.segment*300000+recent.byte_offset/32;
        (void)podcast_bookmarks_load_volume(&volume);
        stored_volume = volume;
    }
    nvs_stats_t stats;
    if (nvs == ESP_OK && nvs_get_stats(NULL, &stats) == ESP_OK)
        ESP_LOGI(TAG, "NVS used=%u free=%u available=%u total=%u", (unsigned)stats.used_entries, (unsigned)stats.free_entries, (unsigned)stats.available_entries, (unsigned)stats.total_entries);
    const podcast_config_t *connection = podcast_config_get();
    char expected_id[24]; snprintf(expected_id, sizeof(expected_id), "device-%016llx", (unsigned long long)sync_state.client_seed);
    if (!connection || (sync_state.ready && strcmp(connection->device_id, expected_id))) {
        pairing_invalid = true; message("配对信息不匹配，请重新连接"); publish();
        /* Keep the recovery menu alive without sending another identity's
         * token or deleting the outbox that exposed this mismatch. */
        while (!s_quit) { podcast_key_event_t key; if (xQueueReceive(keys, &key, pdMS_TO_TICKS(100)) == pdTRUE) handle_key(key); publish(); }
        if (restart_setup) esp_restart();
        goto finish;
    }
    if (!podcast_player_init(connection->server)) { message("播放器启动失败"); publish(); goto finish; }
    (void)podcast_dynamic_covers_start(); /* Optional artwork must not gate sound. */
    (void)podcast_player_set_volume(volume);
    publish();
    int64_t next_network = 0, next_battery = 0, next_catalogue = 0;
    while (!s_quit) {
        podcast_player_poll(); /* worker 冻结检测（每圈 ~100ms，见 podcast_player.c） */
        podcast_sync_clock(&sync_state,(uint64_t)(esp_timer_get_time()/1000));
        int transport=atomic_exchange(&direct_transport,0);
        if(transport==1){notice_until=0;if(!atomic_load(&resume_intent)&&resume_record>=0)settle_resume_cancel();}
        else if(transport==2)resume_current_async();
        (void)podcast_player_snapshot(&player);
        if (awaiting_session && player.session_id != start_session && !strcmp(current.show_id, player.show_id) && !strcmp(current.episode_id, player.episode_id)) awaiting_session = false;
        if (player.volume != volume) { volume = player.volume; volume_changed_at = esp_timer_get_time(); }
        if (matching_player() && player.total_seconds && player.total_seconds <= 10000000) current_duration = (unsigned)player.total_seconds;
        sync_observe_snapshot(&player,false);
        if (matching_player()) {
            if (player.state == PODCAST_PLAYER_PLAYING && !strcmp(notice, "正在缓冲音频")) notice_until = 0;
            bool ended = player.state == PODCAST_PLAYER_PAUSED || player.state == PODCAST_PLAYER_FINISHED || player.state == PODCAST_PLAYER_ERROR;
            bool moved = current.segment != player.cursor.segment || current.byte_offset != player.cursor.byte_offset;
            checkpoint(ended && (player.state != saved_state || moved));
            saved_state = player.state;
        }
        podcast_key_event_t key;
        if (xQueueReceive(keys, &key, pdMS_TO_TICKS(100)) == pdTRUE) { handle_key(key); publish(); }
        int64_t now = esp_timer_get_time();
        /* Local controls and sleep deadlines work even while the network is down. */
        if (sleep_deadline && now >= sleep_deadline) {
            atomic_store(&resume_intent,false);settle_resume_cancel();
            preparing = false; // Prevent a queued preparation from auto-starting later.
            auto_suspended = true; seek_was_playing = false;
            if (podcast_player_pause()) { sleep_deadline = 0; message("定时已暂停，确定可接着听"); }
        }
        if (ready_storage && stored_volume != volume && now - volume_changed_at >= 500000) {
            if (podcast_bookmarks_save_volume(volume) == PODCAST_BOOKMARK_OK) stored_volume = volume;
            else volume_changed_at = now;
        }
        /* A sleep deadline wins over EOF: never start another episode afterward. */
        if (matching_player() && player.state == PODCAST_PLAYER_FINISHED && player.session_id != start_session
            && player.completion_id != start_completion && !finish_handled && page != PODCAST_SEEK) {
            finish_handled = true;
            if (auto_next && !auto_suspended) (void)adjacent(1, true);
        }
        if (now >= next_battery) { battery_percent = bsp_battery_soc(); next_battery = now + 30000000; }
        if (now >= next_network) {
            next_network = now + 2000000;
            if (!s_wifi_ok) { (void)wifi_connect(); if (!s_wifi_ok && !has_current) message("正在连接无线网络"); }
            if (s_wifi_ok && (!show_count || ((force_catalogue||now >= next_catalogue) && page == PODCAST_SHOWS))) {
                if (fetch_shows()){next_catalogue = now + 180000000LL;force_catalogue=false;}
            }
            if(s_wifi_ok&&sync_state.ready){
                if(legacy_slot<PODCAST_BOOKMARK_SLOTS){
                    podcast_bookmark_t old;podcast_bookmark_result_t loaded=podcast_bookmarks_load_slot(legacy_slot,&old);
                    if(loaded==PODCAST_BOOKMARK_NOT_FOUND||loaded==PODCAST_BOOKMARK_INVALID||(loaded==PODCAST_BOOKMARK_OK&&import_bookmark(&old)))++legacy_slot;
                }
                int pending=podcast_sync_pending(&sync_state);
                if(pending>=0){
                    podcast_sync_record_t *r=&sync_state.records[pending];
                    if(r->pending||r->state==PODCAST_SYNC_STOPPED||r->state==PODCAST_SYNC_ENDED||!r->session_id[0]){
                        (void)sync_send_record(pending);
                    }
                }
                if(now>=next_recent&&page==PODCAST_SHOWS){fetch_recent_remote();next_recent=now+10000000;}
            }
            if (s_wifi_ok && preparing && now - last_poll >= 2000000) {
                last_poll = now; (void)details(false); if (!preparing) prepare_next();
            }
            if (s_wifi_ok && !preparing && matching_player() && player.state == PODCAST_PLAYER_PLAYING
                && auto_next && now >= next_prefetch_attempt) {
                next_prefetch_attempt = now + 30000000;
                prepare_next();
            }
        }
        publish();
    }
    checkpoint(true);
    podcast_player_snapshot_t exiting;if(podcast_player_snapshot(&exiting))sync_observe_snapshot(&exiting,true);
    (void)podcast_player_stop();
    if (restart_setup) esp_restart();
 finish:
    xSemaphoreGive(service_done);
    vTaskSuspend(NULL);
}
/* Caller holds view_lock. This operates only on published context and a real
 * audio snapshot; no catalogue state, publish(), storage or network access. */
static void ui_idle_return(int64_t now, uint32_t activity, podcast_view_t *snapshot,
                           const podcast_player_snapshot_t *audio)
{
    if(!return_view.available||snapshot->busy||snapshot->page==PODCAST_NOW||snapshot->page==PODCAST_SEEK||
        strcmp(return_view.show_id,audio->show_id)||strcmp(snapshot->episode_id,audio->episode_id)||
        (snapshot->waiting_for_session&&audio->session_id==snapshot->playback_session_floor))return;
    podcast_idle_decision_t idle=podcast_idle_decide((podcast_idle_input_t){
        .now_us=now,.last_input_us=input_time(now,activity),
        .live_playback=audio->state==PODCAST_PLAYER_PLAYING||audio->state==PODCAST_PLAYER_BUFFERING,
        .away_from_player=true});
    if(!idle.return_to_player)return;

    podcast_page_t previous=snapshot->page;
    snapshot->page=PODCAST_NOW;
    copy(snapshot->show_id,sizeof(snapshot->show_id),return_view.show_id);
    copy(snapshot->show,sizeof(snapshot->show),return_view.show);
    copy(snapshot->title,sizeof(snapshot->title),return_view.title);
    copy(snapshot->heading,sizeof(snapshot->heading),"正在收听");
    const char *status=audio->state==PODCAST_PLAYER_BUFFERING?"正在缓冲":
        return_view.offline?"正在播放，进度待同步":"正在播放";
    if(return_view.storage_warning)snprintf(snapshot->status,sizeof(snapshot->status),"%s · 无法存进度",status);
    else copy(snapshot->status,sizeof(snapshot->status),status);
    copy(snapshot->hint,sizeof(snapshot->hint),"上加音量  下减音量");
    copy(snapshot->back_hint,sizeof(snapshot->back_hint),"确定暂停 长按菜单");
    snapshot->oldest_first=return_view.oldest_first;
    snapshot->sleep_minutes=return_view.sleep_deadline?
        (unsigned)((return_view.sleep_deadline>now?return_view.sleep_deadline-now:0)/60000000+1):0;
    snapshot->elapsed=(unsigned)audio->elapsed_seconds;
    snapshot->volume=audio->volume;snapshot->playing=audio->state==PODCAST_PLAYER_PLAYING;
    if(audio->total_seconds)snapshot->duration=(unsigned)audio->total_seconds;
    /* Coordinate just the clock/page/view commit with the callback's activity
     * update. A fresh PRESS wins; no IO or LVGL runs in this short section. */
    TRANSPORT_ENTER();
    podcast_page_t actual=atomic_load(&page);
    if(atomic_load(&input_state)==activity&&(actual==previous||actual==PODCAST_NOW)){
        snapshot->revision=++revision;view=*snapshot;atomic_store(&page,PODCAST_NOW);
    }else *snapshot=view;
    TRANSPORT_EXIT();
}
static void ui_tick(lv_timer_t *unused)
{
    (void)unused;
    static uint32_t rendered = UINT32_MAX;
    static unsigned rendered_volume = UINT32_MAX;
    static unsigned rendered_elapsed = UINT32_MAX;
    static uint32_t rendered_covers = UINT32_MAX;
    int64_t now=esp_timer_get_time(); uint32_t activity=atomic_load(&input_state);
    podcast_idle_decision_t idle=podcast_idle_decide((podcast_idle_input_t){
        .now_us=now,.last_input_us=input_time(now,activity),.screen_off_seconds=PODCAST_IDLE_SCREEN_OFF_SECONDS,
        .screen_awake=!(activity&INPUT_SLEEP_BIT)});
    if(idle.turn_screen_off) (void)atomic_compare_exchange_strong(&input_state,&activity,activity|INPUT_SLEEP_BIT);
    bool blank=!!(atomic_load(&input_state)&INPUT_SLEEP_BIT);
    if(blank!=applied_screen_blank){
        bsp_display_backlight(blank?0:85); applied_screen_blank=blank;
        rendered=rendered_volume=rendered_elapsed=UINT32_MAX;
    }
    podcast_view_t snapshot;
    if (xSemaphoreTake(view_lock, 0) != pdTRUE) return;
    snapshot = view;
    podcast_player_snapshot_t audio_snapshot;
    bool have_audio=podcast_player_snapshot(&audio_snapshot);
    if(have_audio)ui_idle_return(now,atomic_load(&input_state),&snapshot,&audio_snapshot);
    xSemaphoreGive(view_lock);
    const char *art_ids[PODCAST_COVER_SLOTS];size_t art_count=0;
    if(!blank){
        if(snapshot.page==PODCAST_NOW||snapshot.page==PODCAST_EPISODES)art_ids[art_count++]=snapshot.show_id;
        else if(snapshot.page==PODCAST_SHOWS){
            if(snapshot.has_recent)art_ids[art_count++]=snapshot.recent_show_id;
            for(int i=0;i<snapshot.count&&art_count<PODCAST_COVER_SLOTS;i++)art_ids[art_count++]=snapshot.rows[i].show_id;
        }
    }
    podcast_dynamic_covers_want(art_ids,art_count);
    (void)podcast_dynamic_covers_ui_poll();
    uint32_t artwork_revision=podcast_dynamic_covers_revision();
    /* Return also commits while blank: waking after a stalled HTTP request
     * must reveal the current player, rather than restarting a seven-second
     * wait on the old settings page. Rendering remains suspended while off. */
    if(blank)return;
    if ((snapshot.page == PODCAST_NOW || snapshot.page == PODCAST_SEEK || (snapshot.page == PODCAST_SHOWS && snapshot.recent_is_current)) && have_audio) {
        snapshot.volume = audio_snapshot.volume;
        if (!strcmp(snapshot.show_id, audio_snapshot.show_id) && !strcmp(snapshot.episode_id, audio_snapshot.episode_id)
            && !(snapshot.waiting_for_session && audio_snapshot.session_id == snapshot.playback_session_floor)) {
            snapshot.elapsed = (unsigned)audio_snapshot.elapsed_seconds;
            if(snapshot.recent_is_current) snapshot.recent_elapsed=snapshot.elapsed;
            if (audio_snapshot.total_seconds) snapshot.duration = (unsigned)audio_snapshot.total_seconds;
            snapshot.playing = audio_snapshot.state == PODCAST_PLAYER_PLAYING;
        }
    }
    if (snapshot.revision != rendered || snapshot.volume != rendered_volume || snapshot.elapsed != rendered_elapsed || artwork_revision != rendered_covers) {
        podcast_ui_render(&snapshot); rendered = snapshot.revision; rendered_volume = snapshot.volume; rendered_elapsed = snapshot.elapsed;
        rendered_covers=artwork_revision;
    }
}
void demo_podcast_enter(void)
{
    s_quit = false; restart_setup = false; pairing_invalid = false;
    atomic_store(&input_state,input_stamp(esp_timer_get_time())); memset(&wake_gesture,0,sizeof(wake_gesture));
    applied_screen_blank=false; bsp_display_backlight(85);
    view_lock = xSemaphoreCreateMutex();
    service_done = xSemaphoreCreateBinary();
    keys = xQueueCreate(8, sizeof(podcast_key_event_t));
    if (!view_lock || !service_done || !keys) return;
    podcast_ui_create();
    publish();
    podcast_ui_render(&view);
    timer = lv_timer_create(ui_tick, 200, NULL);
}
void demo_podcast_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (!keys || s_quit) return;
    int64_t now=esp_timer_get_time();
    TRANSPORT_ENTER();
    uint32_t old=atomic_exchange(&input_state,input_stamp(now));
    TRANSPORT_EXIT();
    podcast_idle_key_t mapped=event==BSP_BTN_PRESS?PODCAST_IDLE_KEY_PRESS:
        event==BSP_BTN_CLICK?PODCAST_IDLE_KEY_CLICK:event==BSP_BTN_DOUBLE?PODCAST_IDLE_KEY_DOUBLE:
        event==BSP_BTN_LONG?PODCAST_IDLE_KEY_LONG:PODCAST_IDLE_KEY_OTHER;
    podcast_idle_key_decision_t wake=podcast_idle_wake_input(&wake_gesture,mapped,!(old&INPUT_SLEEP_BIT),now);
    if(wake.consume || event==BSP_BTN_PRESS)return;
    /* Audio volume bypasses slow catalogue HTTP and storage; worker owns gain. */
    if (page == PODCAST_NOW && (event == BSP_BTN_CLICK || event == BSP_BTN_DOUBLE) && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        if (podcast_player_adjust_volume(btn == BSP_BTN_UP ? 5 : -5)) return;
    }
    /* Local transport remains immediate even while catalogue/sync HTTP waits.
     * The audio worker owns playback; this callback only queues its command. */
    if(page==PODCAST_NOW&&btn==BSP_BTN_OK&&(event==BSP_BTN_CLICK||event==BSP_BTN_DOUBLE)&&view_lock){
        char show_id[24]={0},episode_id[64]={0};bool allowed=false,waiting=false;uint32_t floor=0;
        if(xSemaphoreTake(view_lock,0)==pdTRUE){
            allowed=view.page==PODCAST_NOW&&!view.busy;waiting=view.waiting_for_session;floor=view.playback_session_floor;
            memcpy(show_id,view.show_id,sizeof(show_id));memcpy(episode_id,view.episode_id,sizeof(episode_id));xSemaphoreGive(view_lock);
        }
        podcast_player_snapshot_t local;
        if(allowed&&podcast_player_snapshot(&local)&&!(waiting&&local.session_id==floor)&&!strcmp(show_id,local.show_id)&&!strcmp(episode_id,local.episode_id)){
            int action=local.state==PODCAST_PLAYER_PLAYING||local.state==PODCAST_PLAYER_BUFFERING?1:local.state==PODCAST_PLAYER_PAUSED?2:0;
            if(action){
                TRANSPORT_ENTER();
                if(atomic_load(&resume_intent))action=1;
                bool ok=true;
                if(action==1){atomic_store(&resume_intent,false);ok=podcast_player_pause();}
                else if(sync_state.ready)atomic_store(&resume_intent,true);
                else ok=podcast_player_resume();
                if(ok)atomic_store(&direct_transport,action);
                TRANSPORT_EXIT();if(ok)return;
            }
        }
    }
    podcast_key_event_t key = {btn, event};
    if (event == BSP_BTN_CLICK || event == BSP_BTN_LONG || event == BSP_BTN_DOUBLE)
        (void)xQueueSend(keys, &key, 0);
}
esp_err_t demo_podcast_start(void)
{
    if (!keys || !view_lock || !service_done) return ESP_ERR_NO_MEM;
    if (xTaskCreate(service_task, "podcast_catalog", 8192, NULL, 3, &service) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
esp_err_t demo_podcast_stop(void)
{
    s_quit = true;
    if (service) {
        if (xSemaphoreTake(service_done, pdMS_TO_TICKS(6000)) != pdTRUE) return ESP_ERR_TIMEOUT;
        vTaskDelete(service); service = NULL;
    }
    if(!podcast_dynamic_covers_stop())return ESP_ERR_TIMEOUT;
    return ESP_OK;
}
void demo_podcast_exit(void)
{
    if (service) return;
    bsp_display_backlight(85); applied_screen_blank=false;
    if (timer) { lv_timer_delete(timer); timer = NULL; }
    podcast_ui_delete();
    if (keys) { vQueueDelete(keys); keys = NULL; }
    if (view_lock) { vSemaphoreDelete(view_lock); view_lock = NULL; }
    if (service_done) { vSemaphoreDelete(service_done); service_done = NULL; }
}
