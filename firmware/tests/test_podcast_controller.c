#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Execute the real controller. Only hardware/network/storage boundaries and
 * input JSON trees are fixtures. This does not prove device UI or audio. */
#include "../main/demo_podcast.c"
#include "../main/podcast_sync.c"
static const podcast_config_t fixture_connection = {
    .ssid = "test-network", .password = "test-only-password", .server = "http://relay.test:8899",
    .device_id = "device-5345535353455353", .token = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
const podcast_config_t *podcast_config_get(void) { return &fixture_connection; }
const char *podcast_config_authorization(void) { return "Bearer aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"; }
static bool setup_request_ok = true;
bool podcast_config_request_setup(void) { return setup_request_ok; }
void esp_restart(void) { assert(false); }
uint32_t esp_random(void){return 0x53455353U;}

/* Use the production UTF-8 copier rather than replacing text handling. Rename
 * its NVS entry points because this test observes controller storage requests. */
#define podcast_bookmarks_init unused_bookmarks_init
#define podcast_bookmarks_load_recent unused_bookmarks_load_recent
#define podcast_bookmarks_find unused_bookmarks_find
#define podcast_bookmarks_save unused_bookmarks_save
#define podcast_bookmarks_save_volume unused_bookmarks_save_volume
#define podcast_bookmarks_load_volume unused_bookmarks_load_volume
#include "../main/podcast_bookmarks.c"
#undef podcast_bookmarks_init
#undef podcast_bookmarks_load_recent
#undef podcast_bookmarks_find
#undef podcast_bookmarks_save
#undef podcast_bookmarks_save_volume
#undef podcast_bookmarks_load_volume

static bool sync_nvs_available,sync_nvs_present,sync_nvs_staged;
static uint8_t sync_disk[sizeof(wire)],sync_staging[sizeof(wire)];
static size_t sync_disk_size,sync_staging_size;
static int64_t test_now = 100000000;
const char controller_wifi_event_base[] = "wifi", controller_ip_event_base[] = "ip";
static unsigned starts, stops, pauses, resumes, saves, volume_saves, receive_count, loop_budget;
static unsigned backlight_calls, backlight_value;
static unsigned art_prepare_calls,art_start_calls,art_stop_calls,art_poll_calls;
static char art_wanted[PODCAST_COVER_SLOTS][24];static size_t art_wanted_count;
static bool art_start_ok=true,art_stop_ok=true;
static unsigned saves_before_first_input;
static int last_seek;
static uint64_t last_seek_to_ms;
static unsigned seek_to_calls;
static const char *cancel_during_path;
static unsigned seek_calls, relative_volume_calls, input_queue_sends, render_calls;
static int relative_deltas[32];
static char request_bodies[32][512];
static int64_t request_posted_at[32];
static podcast_view_t rendered_view;
static podcast_player_snapshot_t iteration_snapshots[8];
static unsigned iteration_snapshot_count;
static bool simulate_receive_clock;
static bool ui_during_http, return_during_publish, press_before_snapshot;
static void observe_ui_during_http(void);
static bool command_ok = true, resume_ok = true;
static podcast_bookmark_result_t save_result = PODCAST_BOOKMARK_OK;
static podcast_bookmark_t stored;
static podcast_player_snapshot_t input_snapshot;
static char started_show[24], started_episode[64];
static podcast_player_cursor_t started_cursor;
static struct {
    const char *path;
    int status;
    int64_t content_length;
    cJSON *json;
} responses[32];
static unsigned response_count, response_read;
struct controller_http { char url[256]; unsigned fixture; int position; };
static struct controller_http active_http;

static char *duplicate(const char *s)
{
    char *result = malloc(strlen(s) + 1);
    assert(result);
    strcpy(result, s);
    return result;
}
static cJSON *node(int type)
{
    cJSON *result = calloc(1, sizeof(*result));
    assert(result);
    result->type = type;
    return result;
}
static void add(cJSON *parent, const char *key, cJSON *child)
{
    if (key) child->string = duplicate(key);
    cJSON **tail = &parent->child;
    while (*tail) tail = &(*tail)->next;
    *tail = child;
}
static void text_item(cJSON *parent, const char *key, const char *value)
{
    cJSON *item = node(JSON_STRING); item->valuestring = duplicate(value); add(parent, key, item);
}
static void number_item(cJSON *parent, const char *key, int value)
{
    cJSON *item = node(JSON_NUMBER); item->valueint = value; item->valuedouble = value; add(parent, key, item);
}
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *parent, const char *key)
{
    for (cJSON *item = parent ? parent->child : NULL; item; item = item->next)
        if (item->string && !strcmp(item->string, key)) return item;
    return NULL;
}
int cJSON_IsString(const cJSON *j) { return j && j->type == JSON_STRING; }
int cJSON_IsNumber(const cJSON *j) { return j && j->type == JSON_NUMBER; }
int cJSON_IsArray(const cJSON *j) { return j && j->type == JSON_ARRAY; }
int cJSON_IsTrue(const cJSON *j) { return j && j->type == JSON_TRUE; }
int cJSON_GetArraySize(const cJSON *j)
{
    int count = 0; for (cJSON *child = j ? j->child : NULL; child; child = child->next) ++count; return count;
}
void cJSON_Delete(cJSON *item)
{
    while (item) {
        cJSON *next = item->next;
        cJSON_Delete(item->child);
        free(item->string); free(item->valuestring); free(item); item = next;
    }
}
cJSON *cJSON_Parse(const char *body)
{
    assert(!strcmp(body, "{}"));
    cJSON *result = responses[active_http.fixture].json;
    responses[active_http.fixture].json = NULL;
    return result;
}
static void respond(const char *path, int status, cJSON *json)
{
    assert(response_count < 32);
    responses[response_count].path = path; responses[response_count].status = status;
    responses[response_count].content_length = 2;
    responses[response_count++].json = json;
}
static cJSON *shows_json(void)
{
    cJSON *json = node(JSON_OBJECT), *array = node(JSON_ARRAY); add(json, "s", array);
    for (unsigned i = 0; i < 2; ++i) {
        cJSON *show = node(JSON_OBJECT); add(array, NULL, show);
        text_item(show, "i", i ? "show_b" : "show_a"); text_item(show, "n", i ? "节目乙" : "节目甲");
    }
    return json;
}
static cJSON *episodes_json(unsigned offset)
{
    cJSON *json = node(JSON_OBJECT), *array = node(JSON_ARRAY); add(json, "episodes", array);
    number_item(json, "total", 9);
    for (unsigned i = 0; i < 3; ++i) {
        char id[16]; (void)snprintf(id, sizeof(id), "ep%u", offset + i);
        cJSON *ep = node(JSON_OBJECT); add(array, NULL, ep);
        text_item(ep, "id", id); text_item(ep, "title", "完整单集标题"); text_item(ep, "pub_date", "2026-10-03");
        number_item(ep, "duration", 600); number_item(ep, "segments", 2); add(ep, "ready", node(JSON_TRUE));
    }
    return json;
}
static cJSON *details_json(bool ready, bool failed)
{
    cJSON *json = node(JSON_OBJECT), *segments = node(JSON_ARRAY); add(json, "segments", segments);
    text_item(json, "title", "完整单集标题"); number_item(json, "duration", 600);
    add(json, "ready", node(ready ? JSON_TRUE : JSON_FALSE));
    text_item(json, "status", failed ? "failed" : ready ? "ready" : "pending");
    if (ready) { add(segments, NULL, node(JSON_OBJECT)); add(segments, NULL, node(JSON_OBJECT)); }
    return json;
}
static cJSON *neighbor_json(const char *id)
{
    cJSON *ep = node(JSON_OBJECT);
    text_item(ep, "id", id); text_item(ep, "title", id);
    text_item(ep, "pub_date", "2026-10-03");
    number_item(ep, "duration", 600); number_item(ep, "segments", 2);
    add(ep, "ready", node(JSON_TRUE));
    return ep;
}
static cJSON *details_with_neighbors(const char *newer, const char *older)
{
    cJSON *json = details_json(true, false);
    number_item(json, "total", 9); number_item(json, "newest_index", 3); number_item(json, "oldest_index", 5);
    if (newer) add(json, "newer", neighbor_json(newer));
    if (older) add(json, "older", neighbor_json(older));
    return json;
}
static cJSON *ordered_episodes_json(unsigned offset, bool oldest, int focus)
{
    cJSON *json = node(JSON_OBJECT), *array = node(JSON_ARRAY); add(json, "episodes", array);
    number_item(json, "total", 9); number_item(json, "offset", (int)offset);
    if (focus >= 0) number_item(json, "focus", focus);
    for (unsigned i = 0; i < 3; ++i) {
        char id[16]; snprintf(id, sizeof(id), "ep%u", oldest ? 8U - offset - i : offset + i);
        add(array, NULL, neighbor_json(id));
    }
    return json;
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    if(strstr(config->url,"/api/listening/"))assert(config->timeout_ms==350);
    if (response_read >= response_count) fprintf(stderr, "Unexpected controller request: %s (fixture %u/%u)\n", config->url, response_read, response_count);
    assert(response_read < response_count);
    memset(&active_http, 0, sizeof(active_http));
    strcpy(active_http.url, config->url); active_http.fixture = response_read++;
    assert(strstr(active_http.url, responses[active_http.fixture].path));
    return &active_http;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int bytes) { (void)client; (void)bytes; return ESP_OK; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v) { (void)c; (void)k; (void)v; return ESP_OK; }
int esp_http_client_write(esp_http_client_handle_t c, const char *body, int size)
{
    assert(size >= 0 && (size_t)size < sizeof(request_bodies[0]));
    memcpy(request_bodies[c->fixture], body, (size_t)size);
    request_bodies[c->fixture][size] = 0;
    request_posted_at[c->fixture] = test_now;
    return size;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    if(ui_during_http){ui_during_http=false;observe_ui_during_http();}
    return responses[c->fixture].content_length;
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) {
    if(cancel_during_path&&strstr(c->url,cancel_during_path)){cancel_during_path=NULL;demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);}
    return responses[c->fixture].status;
}
int esp_http_client_read(esp_http_client_handle_t c, char *out, int size)
{
    int count = 2 - c->position; if (count > size) count = size;
    if (count) memcpy(out, &"{}"[c->position], (size_t)count);
    c->position += count; return count;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    cJSON_Delete(responses[c->fixture].json); responses[c->fixture].json = NULL; return ESP_OK;
}

bool podcast_player_init(const char *base) { (void)base; return true; }
void podcast_player_poll(void) { /* 冻结监视器在宿主上无事可做 */ }
bool podcast_player_snapshot(podcast_player_snapshot_t *snapshot)
{
    if(press_before_snapshot){press_before_snapshot=false;demo_podcast_key(BSP_BTN_UP,BSP_BTN_PRESS);}
    if (snapshot == &player && receive_count < iteration_snapshot_count)
        input_snapshot = iteration_snapshots[receive_count];
    *snapshot = input_snapshot; return true;
}
bool podcast_player_start(const char *show, const char *episode, podcast_player_cursor_t cursor, uint32_t count)
{
    assert(count > 0 && count <= 256); ++starts;
    strcpy(started_show, show); strcpy(started_episode, episode); started_cursor = cursor; return command_ok;
}
bool podcast_player_stop(void) { ++stops; return command_ok; }
bool podcast_player_pause(void) { ++pauses; return command_ok; }
bool podcast_player_resume(void) { ++resumes; return command_ok && resume_ok; }
bool podcast_player_seek_to_ms(uint64_t position_ms) { ++seek_to_calls;last_seek_to_ms=position_ms;return command_ok; }
bool podcast_player_seek_relative(int seconds) { ++seek_calls; last_seek = seconds; return command_ok; }
bool podcast_player_set_volume(uint8_t percent) { (void)percent; return command_ok; }
bool podcast_player_adjust_volume(int delta)
{
    assert(relative_volume_calls < 32);
    relative_deltas[relative_volume_calls++] = delta;
    return command_ok;
}
podcast_bookmark_result_t podcast_bookmarks_init(void) { return PODCAST_BOOKMARK_OK; }
podcast_bookmark_result_t podcast_bookmarks_load_recent(podcast_bookmark_t *record) { (void)record; return PODCAST_BOOKMARK_NOT_FOUND; }
podcast_bookmark_result_t podcast_bookmarks_load_volume(uint8_t *percent) { (void)percent; return PODCAST_BOOKMARK_NOT_FOUND; }
podcast_bookmark_result_t podcast_bookmarks_find(const char *show, const char *episode, podcast_bookmark_t *record)
{ (void)show; (void)episode; (void)record; return PODCAST_BOOKMARK_NOT_FOUND; }
podcast_bookmark_result_t podcast_bookmarks_save(const podcast_bookmark_t *record) { ++saves; stored = *record; return save_result; }
podcast_bookmark_result_t podcast_bookmarks_save_volume(uint8_t percent) { (void)percent; ++volume_saves; return PODCAST_BOOKMARK_OK; }

void controller_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test"; }
int64_t esp_timer_get_time(void) { return test_now; }
void bsp_audio_set_volume(uint8_t p) { (void)p; }
void bsp_display_backlight(uint8_t p) { ++backlight_calls; backlight_value=p; }
bool podcast_dynamic_covers_start(void){art_start_calls++;return art_start_ok;}
bool podcast_dynamic_covers_stop(void){art_stop_calls++;return art_stop_ok;}
void podcast_dynamic_covers_prepare_audio(void){art_prepare_calls++;}
void podcast_dynamic_covers_want(const char *const *ids,size_t count)
{
    assert(count<=PODCAST_COVER_SLOTS);memset(art_wanted,0,sizeof(art_wanted));art_wanted_count=count;
    for(size_t i=0;i<count;i++)strcpy(art_wanted[i],ids[i]);
}
bool podcast_dynamic_covers_ui_poll(void){art_poll_calls++;return false;}
uint32_t podcast_dynamic_covers_revision(void){return 0;}
int bsp_battery_soc(void) { return 80; }
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_get_stats(const char *name, nvs_stats_t *stats) { (void)name; memset(stats, 0, sizeof(*stats)); return ESP_OK; }
esp_err_t nvs_open(const char *n, nvs_open_mode_t m, nvs_handle_t *h) { if(!sync_nvs_available||strcmp(n,"podcast_sync"))return ESP_FAIL;if(m==NVS_READONLY&&!sync_nvs_present)return ESP_ERR_NVS_NOT_FOUND;*h=1;return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h;sync_nvs_staged=false; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *b, size_t *s) { (void)h;if(!sync_nvs_available||strcmp(k,"outbox"))return ESP_FAIL;if(!sync_nvs_present)return ESP_ERR_NVS_NOT_FOUND;if(*s<sync_disk_size)return ESP_FAIL;memcpy(b,sync_disk,sync_disk_size);*s=sync_disk_size;return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *b, size_t s) { (void)h;if(!sync_nvs_available||strcmp(k,"outbox")||s<28||s>sizeof(sync_disk))return ESP_FAIL;memcpy(sync_staging,b,s);sync_staging_size=s;sync_nvs_staged=true;return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v) { (void)h; (void)k; (void)v; return ESP_FAIL; }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v) { (void)h; (void)k; (void)v; return ESP_FAIL; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h;if(!sync_nvs_staged)return ESP_FAIL;memcpy(sync_disk,sync_staging,sync_staging_size);sync_disk_size=sync_staging_size;sync_nvs_present=true;return ESP_OK; }
esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
void *esp_netif_create_default_wifi_sta(void) { return (void *)1; }
esp_err_t esp_wifi_init(const wifi_init_config_t *c) { (void)c; return ESP_OK; }
esp_err_t esp_wifi_set_mode(int m) { (void)m; return ESP_OK; }
esp_err_t esp_wifi_set_config(int m, const wifi_config_t *c) { (void)m; (void)c; return ESP_OK; }
esp_err_t esp_wifi_start(void) { return ESP_OK; }
esp_err_t esp_wifi_connect(void) { return ESP_OK; }
esp_err_t esp_wifi_set_ps(int m) { (void)m; return ESP_OK; }
esp_err_t podcast_wifi_events_register(podcast_wifi_events_t *e, void (*f)(void *, esp_event_base_t, int32_t, void *))
{ (void)e; (void)f; return ESP_OK; }
BaseType_t xTaskCreate(TaskFunction_t f, const char *n, unsigned s, void *a, unsigned p, TaskHandle_t *t)
{ (void)f; (void)n; (void)s; (void)a; (void)p; *t = (void *)1; return pdPASS; }
void vTaskSuspend(TaskHandle_t t) { (void)t; }
void vTaskDelete(TaskHandle_t t) { (void)t; }
void vTaskDelay(TickType_t t) { test_now += (int64_t)t * 1000; }
QueueHandle_t xQueueCreate(unsigned c, unsigned s) { (void)c; (void)s; return (void *)1; }
BaseType_t xQueueSend(QueueHandle_t q, const void *v, TickType_t t) { (void)q; (void)v; assert(t == 0); ++input_queue_sends; return pdTRUE; }
BaseType_t xQueueReceive(QueueHandle_t q, void *v, TickType_t t)
{
    (void)q; (void)v; (void)t; assert(loop_budget);
    if (!receive_count) saves_before_first_input = saves;
    if (simulate_receive_clock) test_now += (int64_t)t * 1000;
    if (++receive_count >= loop_budget) s_quit = true;
    return pdFALSE;
}
void vQueueDelete(QueueHandle_t q) { (void)q; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (void *)1; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t)
{
    (void)s;
    /* Interrupt an already drafted worker publish with the real UI timer,
     * before it can acquire the view lock and replace the displayed page. */
    if(return_during_publish&&t==pdMS_TO_TICKS(10)){
        return_during_publish=false;ui_tick(NULL);
    }
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
lv_timer_t *lv_timer_create(void (*f)(lv_timer_t *), unsigned t, void *u) { (void)f; (void)t; (void)u; return (lv_timer_t *)1; }
void lv_timer_delete(lv_timer_t *t) { (void)t; }
void podcast_ui_create(void) {}
void podcast_ui_render(const podcast_view_t *v) { ++render_calls; rendered_view = *v; }
void podcast_ui_delete(void) {}

static void reset_controller(void)
{
    for (unsigned i = 0; i < response_count; ++i) cJSON_Delete(responses[i].json);
    memset(responses, 0, sizeof(responses)); response_count = response_read = 0;
    memset(shows, 0, sizeof(shows)); memset(episodes, 0, sizeof(episodes));
    sync_nvs_available=sync_nvs_present=sync_nvs_staged=false;
    atomic_store(&direct_transport,0);atomic_store(&resume_intent,false);resume_record=-1;resume_seek_floor=0;local_resume_session=0;sync_offline=false;seek_to_calls=0;last_seek_to_ms=0;cancel_during_path=NULL;force_catalogue=false;legacy_slot=0;last_import_request[0]=0;
    memset(&sync_state,0,sizeof(sync_state));sync_active=-1;next_sync=next_recent=0;current_revision=0;sync_seek_pending=false;
    memset(&current, 0, sizeof(current)); memset(&recent, 0, sizeof(recent));
    memset(&player, 0, sizeof(player)); memset(&input_snapshot, 0, sizeof(input_snapshot));
    memset(&view, 0, sizeof(view)); memset(&stored, 0, sizeof(stored));
    memset(&return_view,0,sizeof(return_view));
    memset(&newer_episode, 0, sizeof(newer_episode)); memset(&older_episode, 0, sizeof(older_episode));
    browse_oldest_first = play_oldest_first = finish_handled = auto_suspended = seek_was_playing = awaiting_session = false;
    auto_next = true; seek_target = current_newest_index = current_oldest_index = current_total = start_session = start_completion = 0;
    volume_changed_at = next_prefetch_attempt = 0; stored_volume = 55; queued_next_id[0] = 0;
    memset(relative_deltas, 0, sizeof(relative_deltas));
    memset(request_bodies, 0, sizeof(request_bodies));
    memset(request_posted_at, 0, sizeof(request_posted_at));
    memset(&rendered_view, 0, sizeof(rendered_view));
    memset(iteration_snapshots, 0, sizeof(iteration_snapshots)); iteration_snapshot_count = 0; simulate_receive_clock = false;
    ui_during_http=return_during_publish=press_before_snapshot=false;
    art_prepare_calls=art_start_calls=art_stop_calls=art_poll_calls=0;art_start_ok=art_stop_ok=true;
    art_wanted_count=0;memset(art_wanted,0,sizeof(art_wanted));
    seek_calls = relative_volume_calls = input_queue_sends = render_calls = 0;
    show_count = show_selection = browse_show = episode_count = episode_total = episode_offset = episode_selection = 0;
    library_had_recent=false;
    action_selection = sleep_selection = 0; episode_retry = -1; page = PODCAST_SHOWS;
    has_current = has_recent = preparing = false; ready_storage = true; current_duration = current_segments = 0;
    sleep_deadline = last_save = last_poll = notice_until = 0; saved_state = PODCAST_PLAYER_IDLE;
    s_quit = false; s_wifi_ok = true; test_now = 100000000; atomic_store(&input_state,input_stamp(test_now)); memset(&wake_gesture,0,sizeof(wake_gesture)); applied_screen_blank=false; backlight_calls=backlight_value=0; volume = 55; input_snapshot.volume = 55;
    pairing_invalid=restart_setup=false;
    view_lock = keys = service_done = (void *)1; service = (void *)1;
    starts = stops = pauses = resumes = saves = volume_saves = receive_count = loop_budget = 0;
    saves_before_first_input = 0;
    last_seek = 0; command_ok = resume_ok = true; save_result = PODCAST_BOOKMARK_OK;
    strcpy(notice, "ready");
}
static void key(bsp_btn_t button, bsp_btn_ev_t event) { handle_key((podcast_key_event_t){button, event}); }
static void select_current(void)
{
    has_current = true; strcpy(current.show_id, "show_a"); strcpy(current.episode_id, "ep0");
    strcpy(current.name, "节目甲"); strcpy(current.title, "这一集"); current_segments = 2;
    strcpy(input_snapshot.show_id, current.show_id); strcpy(input_snapshot.episode_id, current.episode_id);
}
static void test_show_to_episodes(void)
{
    reset_controller(); respond("/api/shows_lite", 200, shows_json());
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(show_count == 2 && page == PODCAST_SHOWS);
    key(BSP_BTN_DOWN, BSP_BTN_CLICK); assert(show_selection == 1);
    respond("/api/shows/show_b/episodes?offset=0&limit=3", 200, episodes_json(0));
    key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(page == PODCAST_EPISODES && browse_show == 1 && episode_count == 3 && episode_total == 9);
    key(BSP_BTN_OK, BSP_BTN_LONG); assert(page == PODCAST_SHOWS && starts == 0);
}
static void test_full_show_capacity_and_bounded_response(void)
{
    reset_controller();
    cJSON *json = node(JSON_OBJECT), *array = node(JSON_ARRAY); add(json, "s", array);
    for (unsigned i = 0; i < MAX_SHOWS+2; ++i) {
        char id[24]; snprintf(id, sizeof(id), "catalogue_%u", i);
        cJSON *show = node(JSON_OBJECT); add(array, NULL, show);
        text_item(show, "i", id);
        text_item(show, "n", i == 15 ? "中文中文中文中文中文中文中文中文中文中文中文中文中文中文" : "节目完整名称");
    }
    respond("/api/shows_lite", 200, json);
    assert(fetch_shows() && show_count == MAX_SHOWS && sizeof(shows) == MAX_SHOWS * sizeof(show_t));
    assert(!strcmp(shows[0].id, "catalogue_0") && !strcmp(shows[15].id, "catalogue_15"));
    /* Truncation retains whole UTF-8 characters: 21 Chinese characters/63 bytes. */
    assert(strlen(shows[15].name) == 63);
    char copied[65]; assert(podcast_bookmark_copy_text(copied, sizeof(copied), shows[15].name));
    for (unsigned i = 0; i < MAX_SHOWS-1; ++i) key(BSP_BTN_DOWN, BSP_BTN_CLICK);
    publish();
    assert(show_selection == MAX_SHOWS-1 && view.total == MAX_SHOWS && view.selected == (MAX_SHOWS-1)%PAGE_SIZE);
    assert(!strcmp(view.rows[view.selected].title, shows[MAX_SHOWS-1].name));
    respond("/api/shows/catalogue_31/episodes?offset=0&limit=3", 200, episodes_json(0));
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(page == PODCAST_EPISODES && browse_show == MAX_SHOWS-1);

    reset_controller(); show_count = 1; strcpy(shows[0].id, "show_a");
    json = episodes_json(0);
    cJSON *title_field = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json, "episodes")->child, "title");
    char long_title[247];
    for (unsigned i = 0; i < 79; ++i) memcpy(long_title + i * 3, "中", 3);
    memcpy(long_title + 237, "｜文", sizeof("｜文"));
    free(title_field->valuestring); title_field->valuestring = duplicate(long_title);
    respond("offset=0&limit=3", 200, json); assert(fetch_episodes(0));
    assert(strlen(episodes[0].title) == 240 && !strcmp(episodes[0].title + 237, "｜"));
    char title_copy[241]; assert(podcast_bookmark_copy_text(title_copy, sizeof(title_copy), episodes[0].title));

    reset_controller(); respond("/api/shows_lite", 200, shows_json());
    responses[0].content_length = MAX_JSON + 1;
    assert(!fetch_shows() && !show_count && active_http.position == 0);
    assert(strstr(notice, "加载失败"));
}
static void test_failed_page_retries_instead_of_playing_old_episode(void)
{
    reset_controller(); show_count = 1; strcpy(shows[0].id, "show_a"); page = PODCAST_EPISODES;
    respond("offset=0&limit=3", 200, episodes_json(0)); assert(fetch_episodes(2));
    respond("offset=3&limit=3", 503, NULL); key(BSP_BTN_DOWN, BSP_BTN_CLICK);
    assert(episode_retry == 3 && episode_selection == 2 && starts == 0);
    respond("offset=3&limit=3", 200, episodes_json(3)); key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(episode_retry == -1 && episode_selection == 3 && starts == 0 && page == PODCAST_EPISODES);
    respond("/api/episodes/show_a/ep3", 200, details_json(true, false)); key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(starts == 1 && page == PODCAST_NOW && !strcmp(started_episode, "ep3"));
}
static void test_actual_three_button_actions(void)
{
    reset_controller(); select_current(); page = PODCAST_NOW; player.state = PODCAST_PLAYER_PLAYING;
    key(BSP_BTN_OK, BSP_BTN_PRESS); assert(pauses == 0);
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(pauses == 1);
    key(BSP_BTN_OK, BSP_BTN_DOUBLE); assert(pauses == 2); // One action, not two.
    player.state = PODCAST_PLAYER_PAUSED; key(BSP_BTN_OK, BSP_BTN_CLICK); assert(resumes == 1);
    key(BSP_BTN_UP, BSP_BTN_CLICK); assert(relative_volume_calls == 1 && relative_deltas[0] == 5 && volume == 60 && seek_calls == 0);
    key(BSP_BTN_DOWN, BSP_BTN_CLICK); assert(relative_volume_calls == 2 && relative_deltas[1] == -5 && volume == 55 && seek_calls == 0);
    key(BSP_BTN_UP, BSP_BTN_LONG); assert(page == PODCAST_SEEK && seek_calls == 0);
    key(BSP_BTN_OK, BSP_BTN_LONG); assert(page == PODCAST_NOW && seek_calls == 0);
    key(BSP_BTN_OK, BSP_BTN_LONG); assert(page == PODCAST_ACTIONS && action_selection == 0);
    key(BSP_BTN_OK, BSP_BTN_LONG); assert(page == PODCAST_NOW);
    page = PODCAST_SLEEP; key(BSP_BTN_DOWN, BSP_BTN_CLICK); key(BSP_BTN_DOWN, BSP_BTN_CLICK);
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(page == PODCAST_NOW && sleep_deadline == test_now + 1800000000LL);
}
static void test_checkpoint_identity_and_paused_displacement(void)
{
    reset_controller(); select_current(); input_snapshot.state = PODCAST_PLAYER_PAUSED;
    input_snapshot.cursor = (podcast_player_cursor_t){1, 64000};
    strcpy(input_snapshot.episode_id, "old_episode"); checkpoint(true);
    assert(saves == 0 && current.segment == 0 && current.byte_offset == 0);
    strcpy(input_snapshot.episode_id, current.episode_id); last_save = test_now;
    checkpoint(false); assert(saves == 0); // 60-second period not yet due.
    checkpoint(true); assert(saves == 1 && stored.segment == 1 && stored.byte_offset == 64000 && !stored.finished);
    input_snapshot.state = PODCAST_PLAYER_FINISHED; checkpoint(true); assert(stored.finished);
    // The service worker must save a new paused seek position even when the
    // state remains PAUSED and the periodic checkpoint is not due.
    reset_controller(); select_current(); input_snapshot.state = saved_state = PODCAST_PLAYER_PAUSED;
    input_snapshot.cursor = (podcast_player_cursor_t){1, 96000}; last_save = test_now;
    show_count = 1; strcpy(shows[0].id, "show_a"); page = PODCAST_NOW; loop_budget = 1;
    service_task(NULL);
    // Observe the save before the loop exits: the unconditional shutdown save
    // must not let a broken paused-displacement branch pass this test.
    assert(saves_before_first_input == 1 && stored.segment == 1 && stored.byte_offset == 96000);
}
static void test_failed_save_is_throttled(void)
{
    reset_controller(); select_current(); input_snapshot.state = PODCAST_PLAYER_PLAYING;
    input_snapshot.cursor.byte_offset = 32000; save_result = PODCAST_BOOKMARK_STORAGE_ERROR;
    checkpoint(false); assert(saves == 1);
    for (unsigned i = 0; i < 100; ++i) { test_now += 100000; checkpoint(false); }
    assert(saves == 1);
    test_now += 60000000; checkpoint(false); assert(saves == 2);
}
static void test_expired_sleep_prevents_ready_preparation_auto_start(void)
{
    reset_controller(); select_current(); preparing = true; sleep_deadline = test_now - 1; last_poll = 0;
    show_count = 1; strcpy(shows[0].id, "show_a"); page = PODCAST_NOW; loop_budget = 2;
    respond("/api/episodes/show_a/ep0", 200, details_json(true, false));
    service_task(NULL);
    assert(pauses == 1 && starts == 0 && !preparing && sleep_deadline == 0);
    assert(response_read == 0); // Actual loop did not poll and auto-start ready audio.
}
static void test_prepare_failure_can_be_retried(void)
{
    reset_controller(); select_current(); page = PODCAST_NOW;
    respond("/api/episodes/show_a/ep0", 200, details_json(false, false));
    respond("/api/prepare", 200, node(JSON_OBJECT)); assert(details(true) && preparing);
    respond("/api/episodes/show_a/ep0", 200, details_json(false, true));
    assert(!details(false) && !preparing && starts == 0);
    respond("/api/episodes/show_a/ep0", 200, details_json(true, false)); key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(starts == 1 && !preparing);
}
static void test_artwork_identity_and_separate_sleep_display(void)
{
    reset_controller(); show_count = 2; has_recent = true;
    strcpy(shows[0].id, "gushifen"); strcpy(shows[1].id, "huzuoyou");
    /* Names deliberately match: artwork must follow IDs, never name or order. */
    strcpy(shows[0].name, "节目同名"); strcpy(shows[1].name, "节目同名");
    strcpy(recent.show_id, "zhangxiaojun"); strcpy(recent.title, "最近一集");
    publish();
    assert(view.count == 2 && !strcmp(view.rows[0].show_id, "gushifen"));
    assert(!strcmp(view.rows[1].show_id, "huzuoyou"));
    show_t temporary = shows[0]; shows[0] = shows[1]; shows[1] = temporary;
    publish();
    assert(!strcmp(view.rows[0].show_id, "huzuoyou"));
    assert(!strcmp(view.rows[1].show_id, "gushifen"));
    page = PODCAST_EPISODES; browse_show = 1; episode_count = 1;
    publish();
    assert(!strcmp(view.show_id, "gushifen") && !strcmp(view.rows[0].show_id, "gushifen"));
    select_current(); page = PODCAST_NOW; strcpy(current.show_id, "zhangxiaojun");
    strcpy(current.name, "张小珺Jùn｜商业访谈录");
    sleep_deadline = test_now + 29LL * 60000000 + 1;
    publish();
    assert(!strcmp(view.show_id, "zhangxiaojun") && !strcmp(view.show, current.name));
    assert(view.sleep_minutes == 30 && sleep_deadline == test_now + 29LL * 60000000 + 1);
}
static void test_latest_update_sort_and_identity_focus(void)
{
    reset_controller(); has_recent = true; strcpy(recent.show_id, "unknown_recent");
    cJSON *json = node(JSON_OBJECT), *array = node(JSON_ARRAY); add(json, "s", array);
    const char *ids[] = {"old", "new", "tie", "unknown"};
    const int dates[] = {100, 300, 300, 0};
    for (unsigned i = 0; i < 4; ++i) {
        cJSON *show = node(JSON_OBJECT); add(array, NULL, show);
        text_item(show, "i", ids[i]); text_item(show, "n", ids[i]);
        number_item(show, "p", dates[i]); text_item(show, "d", i == 3 ? "" : "2026-10-03");
    }
    respond("/api/shows_lite", 200, json); assert(fetch_shows()); publish();
    assert(!strcmp(shows[0].id, "new") && !strcmp(shows[1].id, "tie"));
    assert(!strcmp(shows[2].id, "old") && !strcmp(shows[3].id, "unknown"));
    assert(view.total == 4 && !strcmp(view.rows[0].show_id, "new"));
    assert(!strcmp(view.rows[0].latest_date, "2026-10-03"));
    show_selection = 2; browse_show = 1;
    json = node(JSON_OBJECT); array = node(JSON_ARRAY); add(json, "s", array);
    for (unsigned i = 0; i < 3; ++i) {
        cJSON *show = node(JSON_OBJECT); add(array, NULL, show);
        text_item(show, "i", ids[i]); text_item(show, "n", ids[i]); number_item(show, "p", i ? 300 : 400);
    }
    respond("/api/shows_lite", 200, json); assert(fetch_shows());
    assert(show_selection == 0 && !strcmp(shows[show_selection].id, "old"));
    assert(!strcmp(shows[browse_show].id, "tie"));
    puts("Controller: newest-update ordering, stable ties, unknown dates last, recent excluded, identity focus retained PASS");
}

static void test_order_toggle_preserves_same_episode_and_frozen_play_order(void)
{
    reset_controller(); show_count = 2; strcpy(shows[0].id, "show_a"); strcpy(shows[1].id, "show_b");
    browse_show = 0; page = PODCAST_EPISODES;
    respond("order=newest", 200, ordered_episodes_json(0, false, -1)); assert(fetch_episodes(1));
    assert(!strcmp(episodes[episode_selection - episode_offset].id, "ep1"));
    respond("episode_id=ep1&limit=3&order=oldest", 200, ordered_episodes_json(6, true, 7));
    key(BSP_BTN_UP, BSP_BTN_LONG);
    assert(browse_oldest_first && episode_selection == 7 && episode_offset == 6);
    assert(!strcmp(episodes[episode_selection - episode_offset].id, "ep1"));
    respond("/api/episodes/show_a/ep1", 200, details_with_neighbors("ep0", "ep2"));
    respond("/api/prepare", 200, node(JSON_OBJECT));
    key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(play_oldest_first && starts == 1 && !strcmp(started_episode, "ep1"));
    assert(!strcmp(queued_next_id, "ep0"));
    assert(strstr(request_bodies[3], "\"episode_id\":\"ep0\"") && strstr(request_bodies[3], "\"prefetch\":true"));
    /* Changing another show's browsing direction must not redefine next audio. */
    browse_show = 1; browse_oldest_first = false; page = PODCAST_EPISODES;
    respond("/api/episodes/show_a/ep0", 200, details_with_neighbors(NULL, "ep1"));
    assert(adjacent(1, true));
    assert(starts == 2 && !strcmp(started_show, "show_a") && !strcmp(started_episode, "ep0"));
    assert(play_oldest_first && !browse_oldest_first && browse_show == 1 && page == PODCAST_EPISODES);
    assert(!next_episode()->id[0]);
    assert(!adjacent(1, true) && starts == 2 && !strcmp(current.show_id, "show_a"));
    puts("Controller: order reversal retains same ID; frozen chronological next remains same show and stops at end PASS");
}

static void eof_setup(uint32_t session)
{
    select_current(); show_count = 1; strcpy(shows[0].id, "show_a"); strcpy(shows[0].name, "节目甲");
    page = PODCAST_NOW; current_duration = 600;
    input_snapshot.state = PODCAST_PLAYER_FINISHED; input_snapshot.session_id = session;
    input_snapshot.completion_id = 41; start_completion = 40;
    input_snapshot.cursor = (podcast_player_cursor_t){1, 9600000};
    input_snapshot.elapsed_seconds = input_snapshot.total_seconds = 600;
    player = input_snapshot; start_session = session - 1;
    strcpy(older_episode.id, "ep1"); strcpy(older_episode.title, "下一集");
}
static void test_actual_service_eof_policies(void)
{
    reset_controller(); eof_setup(11); loop_budget = 3;
    iteration_snapshot_count = 3; iteration_snapshots[0] = input_snapshot;
    iteration_snapshots[1] = input_snapshot; iteration_snapshots[1].state = PODCAST_PLAYER_BUFFERING;
    strcpy(iteration_snapshots[1].episode_id, "ep1"); iteration_snapshots[1].session_id = 12;
    iteration_snapshots[1].cursor = (podcast_player_cursor_t){0};
    iteration_snapshots[2] = iteration_snapshots[1];
    respond("/api/episodes/show_a/ep1", 200, details_with_neighbors(NULL, NULL));
    service_task(NULL);
    assert(starts == 1 && !strcmp(started_episode, "ep1") && response_read == 1);
    assert(!strcmp(started_show, "show_a") && !stored.finished);

    reset_controller(); eof_setup(11); start_session = 11; awaiting_session = true; loop_budget = 3;
    service_task(NULL);
    assert(starts == 0 && response_read == 0 && !finish_handled && !stored.finished && saves == 0);

    reset_controller(); eof_setup(11); start_completion = input_snapshot.completion_id; loop_budget = 3;
    service_task(NULL);
    assert(starts == 0 && response_read == 0 && !finish_handled);

    reset_controller(); eof_setup(11); page = PODCAST_SEEK; loop_budget = 3;
    service_task(NULL);
    assert(starts == 0 && response_read == 0 && !finish_handled);

    reset_controller(); eof_setup(11); older_episode.id[0] = 0; loop_budget = 3;
    service_task(NULL);
    assert(starts == 0 && response_read == 0 && finish_handled && stored.finished);

    reset_controller(); eof_setup(11); auto_next = false; loop_budget = 3;
    service_task(NULL);
    assert(starts == 0 && response_read == 0 && finish_handled);

    reset_controller(); eof_setup(11); sleep_deadline = test_now - 1; loop_budget = 3;
    service_task(NULL);
    assert(starts == 0 && response_read == 0 && pauses == 1 && auto_suspended && sleep_deadline == 0);

    reset_controller(); eof_setup(11); auto_suspended = true; loop_budget = 3;
    service_task(NULL);
    assert(starts == 0 && response_read == 0);
    puts("Controller service loop: EOF advances once; prior session/completion ignored; seek preview/list end/auto off/sleep suspension stop succession PASS");
}

static void test_volume_fast_path_and_direct_snapshot_render(void)
{
    reset_controller(); select_current(); page = PODCAST_NOW; preparing = true;
    int64_t before = test_now;
    demo_podcast_key(BSP_BTN_UP, BSP_BTN_CLICK);
    demo_podcast_key(BSP_BTN_DOWN, BSP_BTN_DOUBLE);
    demo_podcast_key(BSP_BTN_DOWN, BSP_BTN_CLICK);
    assert(relative_volume_calls == 3 && relative_deltas[0] == 5 && relative_deltas[1] == -5 && relative_deltas[2] == -5);
    assert(input_queue_sends == 0 && response_read == 0 && volume_saves == 0 && test_now == before);
    view.page = PODCAST_NOW; view.volume = 55; view.revision = 10001;
    input_snapshot.volume = 70; ui_tick(NULL);
    assert(render_calls == 1 && rendered_view.volume == 70);
    input_snapshot.volume = 75; ui_tick(NULL);
    assert(render_calls == 2 && rendered_view.volume == 75 && rendered_view.revision == 10001);
    assert(response_read == 0 && volume_saves == 0);
    copy(view.show_id, sizeof(view.show_id), current.show_id);
    copy(view.episode_id, sizeof(view.episode_id), current.episode_id);
    input_snapshot.elapsed_seconds = 140; input_snapshot.total_seconds = 600;
    input_snapshot.state = PODCAST_PLAYER_PLAYING; input_snapshot.session_id = 7;
    ui_tick(NULL); assert(rendered_view.elapsed == 140 && rendered_view.duration == 600 && rendered_view.playing);
    view.waiting_for_session = true; view.playback_session_floor = 7; view.elapsed = 0; ++view.revision;
    ui_tick(NULL); assert(rendered_view.elapsed == 0);
    input_snapshot.session_id = 8; ui_tick(NULL); assert(rendered_view.elapsed == 140);
    reset_controller(); select_current(); show_count = 1; strcpy(shows[0].id, "show_a");
    input_snapshot.state = PODCAST_PLAYER_PLAYING; input_snapshot.volume = 70; page = PODCAST_NOW;
    last_save = test_now; loop_budget = 8; simulate_receive_clock = true;
    service_task(NULL);
    assert(volume_saves == 1 && stored_volume == 70 && response_read == 0);
    puts("Controller: volume bypasses catalogue/service queue and storage; rendering follows audio snapshot without metadata response PASS");
}

static void test_volume_direction_limits_captions_and_navigation(void)
{
    reset_controller(); select_current(); page = PODCAST_NOW; volume = 95;
    publish(); assert(!strcmp(view.hint, "上加音量  下减音量"));
    key(BSP_BTN_UP, BSP_BTN_CLICK); assert(volume == 100 && relative_deltas[0] == 5);
    key(BSP_BTN_UP, BSP_BTN_DOUBLE); assert(volume == 100 && relative_deltas[1] == 5);
    key(BSP_BTN_DOWN, BSP_BTN_CLICK); assert(volume == 95 && relative_deltas[2] == -5);

    page = PODCAST_VOLUME; volume = 0;
    publish(); assert(!strcmp(view.title, "上键增大，下键减小"));
    key(BSP_BTN_DOWN, BSP_BTN_CLICK); assert(volume == 0 && relative_deltas[3] == -5);
    key(BSP_BTN_UP, BSP_BTN_CLICK); assert(volume == 5 && relative_deltas[4] == 5);
    key(BSP_BTN_DOWN, BSP_BTN_DOUBLE); assert(volume == 0 && relative_deltas[5] == -5);
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(page == PODCAST_NOW);

    page = PODCAST_ACTIONS; action_selection = 2;
    key(BSP_BTN_UP, BSP_BTN_CLICK); assert(action_selection == 1);
    key(BSP_BTN_DOWN, BSP_BTN_CLICK); assert(action_selection == 2);
    page = PODCAST_SEEK; seek_target = 120; current_duration = 600;
    key(BSP_BTN_UP, BSP_BTN_CLICK); assert(seek_target == 105);
    key(BSP_BTN_DOWN, BSP_BTN_CLICK); assert(seek_target == 120);
    assert(relative_volume_calls == 6 && seek_calls == 0);
    puts("Controller: Up raises and Down lowers volume on both pages; captions and limits agree, navigation/progress directions unchanged PASS");
}

static void test_progress_apply_cancel_and_limits(void)
{
    reset_controller(); select_current(); page = PODCAST_NOW;
    player = input_snapshot; player.state = PODCAST_PLAYER_PLAYING; player.elapsed_seconds = 120;
    current_duration = 600;
    key(BSP_BTN_DOWN, BSP_BTN_LONG);
    assert(page == PODCAST_SEEK && pauses == 1 && seek_target == 120 && seek_was_playing);
    key(BSP_BTN_DOWN, BSP_BTN_CLICK); key(BSP_BTN_DOWN, BSP_BTN_LONG); key(BSP_BTN_UP, BSP_BTN_CLICK);
    assert(seek_target == 180 && seek_calls == 0);
    key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(page == PODCAST_NOW && seek_calls == 1 && last_seek == 60 && resumes == 1);

    key(BSP_BTN_UP, BSP_BTN_LONG); key(BSP_BTN_DOWN, BSP_BTN_CLICK);
    key(BSP_BTN_OK, BSP_BTN_LONG);
    assert(page == PODCAST_NOW && seek_calls == 1 && resumes == 2);
    player.state = PODCAST_PLAYER_PAUSED; key(BSP_BTN_UP, BSP_BTN_LONG);
    key(BSP_BTN_UP, BSP_BTN_LONG); key(BSP_BTN_UP, BSP_BTN_LONG); key(BSP_BTN_UP, BSP_BTN_CLICK);
    assert(seek_target == 0); key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(last_seek == -120 && resumes == 2);
    key(BSP_BTN_UP, BSP_BTN_LONG);
    for (unsigned i = 0; i < 20; ++i) key(BSP_BTN_DOWN, BSP_BTN_LONG);
    assert(seek_target == 600); auto_suspended = true; seek_was_playing = true;
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(last_seek == 480 && resumes == 2);
    reset_controller(); select_current(); page = PODCAST_NOW;
    player = input_snapshot; player.state = PODCAST_PLAYER_PLAYING; player.elapsed_seconds = 120; current_duration = 600;
    key(BSP_BTN_UP, BSP_BTN_LONG); key(BSP_BTN_DOWN, BSP_BTN_CLICK); resume_ok = false;
    key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(page == PODCAST_NOW && seek_calls == 1 && last_seek == 15 && strstr(notice, "续播"));
    assert(notice_until > test_now);
    player.state = PODCAST_PLAYER_PAUSED; resume_ok = true; key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(resumes == 2 && notice_until == 0);
    player.state = PODCAST_PLAYER_PLAYING; key(BSP_BTN_UP, BSP_BTN_LONG); resume_ok = false;
    key(BSP_BTN_OK, BSP_BTN_LONG); assert(strstr(notice, "续播") && seek_calls == 1);
    puts("Controller: progress paused preview, one confirmed seek, cancel preserves position, clamps, sleep and partial-resume errors PASS");
}

static void test_prefetch_retry_dedup_and_recent_failure_identity(void)
{
    reset_controller(); select_current(); strcpy(older_episode.id, "ep1");
    respond("/api/prepare", 503, NULL); prepare_next();
    assert(!queued_next_id[0] && starts == 0 && response_read == 1);
    respond("/api/prepare", 200, node(JSON_OBJECT)); prepare_next();
    assert(!strcmp(queued_next_id, "ep1") && starts == 0 && response_read == 2);
    assert(strstr(request_bodies[1], "\"prefetch\":true") && strstr(request_bodies[1], "\"show_id\":\"show_a\""));
    prepare_next(); assert(response_read == 2); /* Successful enqueue is idempotent. */
    auto_next = false; strcpy(older_episode.id, "ep2"); prepare_next(); assert(response_read == 2);

    reset_controller(); select_current(); has_recent = true;
    strcpy(recent.show_id, "show_b"); strcpy(recent.episode_id, "ep_b");
    strcpy(recent.name, "节目乙"); strcpy(recent.title, "最近那集");
    strcpy(older_episode.id, "ep_from_old_show"); strcpy(newer_episode.id, "new_ep_from_old_show");
    current_total = 99; current_newest_index = current_oldest_index = 20;
    respond("/api/episodes/show_b/ep_b", 503, NULL); continue_recent();
    assert(!strcmp(current.show_id, "show_b") && !strcmp(current.episode_id, "ep_b"));
    assert(!older_episode.id[0] && !newer_episode.id[0] && !queued_next_id[0] && current_total == 0);
    assert(!next_episode()->id[0] && starts == 0 && response_read == 1);
    publish(); assert(!view.has_next && !view.next_title[0]);
    puts("Controller: failed prefetch can retry, successful enqueue deduplicates; failed recent details never borrow old show's next ID PASS");
}

static void test_serviceloop_retries_prefetch_after_30_seconds_then_deduplicates(void)
{
    reset_controller(); select_current(); show_count = 1; strcpy(shows[0].id, "show_a");
    page = PODCAST_NOW; input_snapshot.state = PODCAST_PLAYER_PLAYING;
    input_snapshot.session_id = 10; input_snapshot.completion_id = 3;
    strcpy(older_episode.id, "ep1"); strcpy(older_episode.title, "下一集");
    respond("/api/prepare", 503, NULL);
    respond("/api/prepare", 200, node(JSON_OBJECT));
    int64_t began = test_now;
    simulate_receive_clock = true; loop_budget = 800; /* 80 seconds of real service-loop deadlines. */
    service_task(NULL);
    assert(receive_count == 800 && test_now - began == 80000000);
    assert(response_read == 2 && !strcmp(queued_next_id, "ep1") && starts == 0);
    assert(request_posted_at[1] - request_posted_at[0] >= 30000000);
    assert(request_posted_at[1] - request_posted_at[0] <= 32000000);
    assert(next_prefetch_attempt > began + 60000000); /* A third period ran but did not POST again. */
    assert(strstr(request_bodies[0], "\"prefetch\":true") && strstr(request_bodies[1], "\"episode_id\":\"ep1\""));
    puts("Controller service loop: failed next prefetch retries after 30s; successful enqueue survives later periods without repeated POST PASS");
}

static cJSON *sync_session_json(unsigned position)
{
    cJSON *j=node(JSON_OBJECT);text_item(j,"session_id","abcdef1234567890abcdef1234567890");number_item(j,"position_ms",(int)position);number_item(j,"next_seq",1);add(j,"stale",node(JSON_FALSE));return j;
}
static cJSON *sync_ack_json(unsigned revision,const char *state)
{
    cJSON *j=node(JSON_OBJECT),*p=node(JSON_OBJECT);add(j,"accepted",node(JSON_TRUE));add(j,"progress",p);number_item(p,"revision",(int)revision);text_item(p,"status",state);return j;
}
static void test_sync_http_and_reboot(void)
{
    reset_controller();sync_nvs_available=true;assert(podcast_sync_init(&sync_state,123));select_current();current_revision=42;
    respond("/api/listening/sessions",200,sync_session_json(91000));assert(sync_begin_current(false));int at=sync_active;
    assert(current.segment==0&&current.byte_offset==91000*32&&strstr(request_bodies[0],"\"base_revision\":42"));assert(!strstr(request_bodies[0],"\"position_ms\""));
    input_snapshot.session_id=7;input_snapshot.state=PODCAST_PLAYER_PLAYING;input_snapshot.elapsed_seconds=101;input_snapshot.elapsed_ms=101000;input_snapshot.heard_ms=10000;
    test_now+=10000000;podcast_sync_clock(&sync_state,(uint64_t)test_now/1000);sync_observe_snapshot(&input_snapshot,false);
    respond("/api/listening/events",200,sync_ack_json(43,"in_progress"));assert(sync_send_record(at));assert(strstr(request_bodies[1],"\"listened_ms\":10000")&&strstr(request_bodies[1],"\"elapsed_ms\":10000"));
    sync_seek_pending=true;sync_seek_position=500;sync_seek_heard=10000;input_snapshot.elapsed_seconds=500;input_snapshot.elapsed_ms=500000;input_snapshot.state=PODCAST_PLAYER_PAUSED;
    sync_observe_snapshot(&input_snapshot,false);char before[512],after[512];assert(podcast_sync_event_body(&sync_state,at,before,sizeof(before)));assert(strstr(before,"\"seek\":true")&&strstr(before,"\"listened_ms\":10000"));
    respond("/api/listening/events",503,NULL);assert(!sync_send_record(at));
    test_now+=2000000;podcast_sync_clock(&sync_state,(uint64_t)test_now/1000);input_snapshot.state=PODCAST_PLAYER_PLAYING;input_snapshot.elapsed_seconds=502;input_snapshot.elapsed_ms=502000;input_snapshot.heard_ms=12000;sync_observe_snapshot(&input_snapshot,false);
    assert(podcast_sync_event_body(&sync_state,at,after,sizeof(after))&&!strcmp(before,after));assert(podcast_sync_init(&sync_state,999));assert(podcast_sync_event_body(&sync_state,at,after,sizeof(after))&&!strcmp(before,after));
    assert(sync_state.records[at].heard_ms==12000&&sync_state.records[at].position_ms==502000&&sync_state.records[at].state==PODCAST_SYNC_STOPPED);
    respond("/api/listening/events",200,sync_ack_json(44,"in_progress"));assert(sync_send_record(at));assert(!strcmp(request_bodies[2],request_bodies[3]));
    assert(podcast_sync_freeze(&sync_state,at));assert(podcast_sync_event_body(&sync_state,at,after,sizeof(after))&&strstr(after,"\"listened_ms\":12000")&&strstr(after,"\"state\":\"stopped\""));
    puts("Controller sync: real 350ms HTTP protocol, central resume, exact heard/elapsed counters, immutable retry through new audio/reboot, seek and monotonic sequence PASS");
}
static void test_sync_eof_and_closed_tail(void)
{
    reset_controller();sync_nvs_available=true;assert(podcast_sync_init(&sync_state,654));select_current();
    respond("/api/listening/sessions",200,sync_session_json(0));assert(sync_begin_current(false));int at=sync_active;
    test_now+=600000000;podcast_sync_clock(&sync_state,(uint64_t)test_now/1000);
    input_snapshot.session_id=9;input_snapshot.elapsed_ms=600000;input_snapshot.elapsed_seconds=600;input_snapshot.heard_ms=600000;input_snapshot.state=PODCAST_PLAYER_FINISHED;
    sync_observe_snapshot(&input_snapshot,false);sync_observe_snapshot(&input_snapshot,true);
    assert(sync_state.records[at].state==PODCAST_SYNC_ENDED&&sync_state.records[at].pending_state==PODCAST_SYNC_ENDED);
    char body[512];assert(podcast_sync_event_body(&sync_state,at,body,sizeof(body))&&strstr(body,"\"state\":\"ended\""));
    sync_active=-1;input_snapshot.closed_session_id=9;input_snapshot.closed_elapsed_ms=600032;input_snapshot.closed_heard_ms=600032;input_snapshot.session_id=10;strcpy(input_snapshot.episode_id,"ep1");sync_observe_snapshot(&input_snapshot,false);
    assert(sync_state.records[at].heard_ms==600032&&sync_state.records[at].audio_session==0&&sync_state.records[at].state==PODCAST_SYNC_ENDED);
    respond("/api/listening/events",200,sync_ack_json(1,"completed"));assert(sync_send_record(at));assert(podcast_sync_freeze(&sync_state,at));assert(podcast_sync_event_body(&sync_state,at,body,sizeof(body))&&strstr(body,"\"listened_ms\":600032")&&strstr(body,"\"state\":\"ended\""));
    respond("/api/listening/events",200,sync_ack_json(2,"completed"));assert(sync_send_record(at)&&!sync_state.records[at].used&&sync_disk_size==28);
    puts("Controller sync: EOF remains ended through auto-next/stop, final audio-worker tail survives identity replacement, fully ACKed terminal records compact outbox PASS");
}
static void test_full_outbox_keeps_current_audio(void)
{
    reset_controller();sync_nvs_available=true;assert(podcast_sync_init(&sync_state,987));select_current();
    respond("/api/listening/sessions",200,sync_session_json(0));assert(sync_begin_current(false));int active=sync_active;
    for(unsigned i=1;i<PODCAST_SYNC_SLOTS;i++){char id[16];snprintf(id,sizeof(id),"offline%u",i);assert(podcast_sync_begin(&sync_state,"show_b",id,0,0,true,false)==(int)i);}
    input_snapshot.session_id=7;input_snapshot.state=PODCAST_PLAYER_PLAYING;input_snapshot.elapsed_ms=10000;input_snapshot.elapsed_seconds=10;input_snapshot.heard_ms=10000;sync_observe_snapshot(&input_snapshot,false);
    assert(!start_current(true)&&sync_active==active&&starts==0);
    input_snapshot.elapsed_ms=11000;input_snapshot.elapsed_seconds=11;input_snapshot.heard_ms=11000;sync_observe_snapshot(&input_snapshot,false);
    assert(sync_state.records[active].heard_ms==11000&&sync_state.records[active].state==PODCAST_SYNC_PLAYING);
    assert(strstr(notice,"联网重试"));
    puts("Controller sync: eight full unsent sessions block only a new start, retain current playing audio and continue its exact heard counter PASS");
}
static void test_recent_and_episode_listen_states(void)
{
    reset_controller();sync_nvs_available=true;assert(podcast_sync_init(&sync_state,321));
    cJSON *j=node(JSON_OBJECT),*array=node(JSON_ARRAY),*r=node(JSON_OBJECT);add(j,"s",array);add(array,NULL,r);
    text_item(r,"i","show_a");text_item(r,"e","ep0");text_item(r,"n","节目甲");text_item(r,"t","上次听到这里");number_item(r,"p",91000);number_item(r,"d",600);number_item(r,"c",1);
    respond("/api/listening/recent?limit=1&lite=1",200,j);fetch_recent_remote();assert(has_recent&&recent_position_ms==91000&&recent_duration==600);publish();assert(view.has_recent&&!strcmp(view.recent_title,"上次听到这里"));
    j=shows_json();respond("/api/shows_lite",200,j);assert(fetch_shows());show_selection=1;publish();assert(view.count==2&&view.selected==1&&view.has_recent);
    page=PODCAST_EPISODES;browse_show=0;j=episodes_json(0);array=cJSON_GetObjectItemCaseSensitive(j,"episodes");unsigned state=0;
    for(cJSON *ep=array->child;ep;ep=ep->next){number_item(ep,"c",(int)state);number_item(ep,"p",state?91000:0);number_item(ep,"l",state?10000:0);number_item(ep,"v",7);state++;}
    respond("/api/shows/show_a/episodes?offset=0&limit=3&order=newest&lite=1",200,j);assert(fetch_episodes(0));publish();
    assert(strstr(view.rows[0].detail,"未播")&&strstr(view.rows[1].detail,"听过")&&strstr(view.rows[2].detail,"听完"));
    assert(episodes[1].position_ms==91000&&episodes[1].progress_revision==7);
    puts("Controller sync: central recent visible with unchanged long-OK, two-row library paging, actual lite episode unplayed/in-progress/completed metadata rendered PASS");
}
static void test_immediate_local_transport(void)
{
    reset_controller();select_current();page=PODCAST_NOW;input_snapshot.state=player.state=PODCAST_PLAYER_PLAYING;player=input_snapshot;publish();
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);assert(pauses==1&&resumes==0&&input_queue_sends==0&&response_read==0&&atomic_load(&direct_transport)==1);
    input_snapshot.state=PODCAST_PLAYER_PAUSED;demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);assert(resumes==1&&input_queue_sends==0&&atomic_load(&direct_transport)==2);
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_LONG);assert(input_queue_sends==1); /* Menu semantics still pass through the controller. */
    preparing=true;view.busy=true;demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);assert(resumes==1&&input_queue_sends==2);
    puts("Controller: pause/resume and volume queue directly to audio during catalogue/sync waits; no callback HTTP/NVS; long-OK and preparation actions unchanged PASS");
}
static cJSON *progress_json(unsigned position,unsigned revision)
{
    cJSON *j=node(JSON_OBJECT);number_item(j,"p",(int)position);number_item(j,"v",(int)revision);return j;
}
static cJSON *resume_session_json(unsigned position,bool stale)
{
    cJSON *j=node(JSON_OBJECT);text_item(j,"session_id","22222222222222222222222222222222");number_item(j,"position_ms",(int)position);number_item(j,"next_seq",1);add(j,"stale",node(stale?JSON_TRUE:JSON_FALSE));return j;
}
static void respond_paused_ack(unsigned revision,bool stale)
{
    cJSON *j=sync_ack_json(revision,"in_progress");add(j,"stale",node(stale?JSON_TRUE:JSON_FALSE));respond("/api/listening/events",200,j);
}
static int prepare_paused_sync_with_earlier_pending(bool earlier)
{
    reset_controller();sync_nvs_available=true;assert(podcast_sync_init(&sync_state,789));podcast_sync_clock(&sync_state,(uint64_t)test_now/1000);
    select_current();current_revision=1;page=PODCAST_NOW;
    respond("/api/listening/sessions",200,sync_session_json(0));assert(sync_begin_current(false));int old=sync_active;
    if(earlier){
        input_snapshot.session_id=7;input_snapshot.state=PODCAST_PLAYER_PLAYING;input_snapshot.elapsed_ms=2000;input_snapshot.elapsed_seconds=2;input_snapshot.heard_ms=2000;
        test_now+=2000000;podcast_sync_clock(&sync_state,(uint64_t)test_now/1000);sync_observe_snapshot(&input_snapshot,false);
    }
    input_snapshot.session_id=7;input_snapshot.state=PODCAST_PLAYER_PAUSED;input_snapshot.elapsed_ms=10000;input_snapshot.elapsed_seconds=10;input_snapshot.heard_ms=10000;
    test_now+=earlier?8000000:10000000;podcast_sync_clock(&sync_state,(uint64_t)test_now/1000);sync_observe_snapshot(&input_snapshot,false);player=input_snapshot;publish();
    return old;
}
static int prepare_paused_sync(void){return prepare_paused_sync_with_earlier_pending(false);}
static void test_paused_cross_device_resume(void)
{
    int old=prepare_paused_sync();char old_event[512];assert(podcast_sync_event_body(&sync_state,old,old_event,sizeof(old_event)));
    respond_paused_ack(8,true);
    respond("/api/listening/progress/show_a/ep0?lite=1",200,progress_json(40031,8));respond("/api/listening/sessions",200,resume_session_json(40031,false));
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);assert(resumes==0&&seek_to_calls==0&&atomic_load(&resume_intent)&&response_read==1);
    assert(atomic_exchange(&direct_transport,0)==2);resume_current_async();int resumed=sync_active;
    assert(resumed!=old&&resumes==1&&seek_to_calls==1&&last_seek_to_ms==40031);
    assert(!strcmp(sync_state.records[resumed].session_id,"22222222222222222222222222222222"));
    assert(strstr(request_bodies[3],"\"base_revision\":8")&&!strstr(request_bodies[3],"position_ms"));
    assert(!strcmp(request_bodies[1],old_event)); /* Existing pending body was acknowledged without rewriting it. */
    assert(sync_state.records[old].state==PODCAST_SYNC_STOPPED&&sync_state.records[old].audio_session==0&&sync_state.records[resumed].heard_origin_ms==10000);
    sync_observe_snapshot(&input_snapshot,false);unsigned before=saves;checkpoint(true);assert(saves==before&&current.byte_offset==40031*32);assert(sync_state.records[resumed].position_ms==40031&&sync_state.records[resumed].heard_ms==0); /* Old paused cursor is never posted as new position. */
    input_snapshot.state=PODCAST_PLAYER_PLAYING;input_snapshot.elapsed_ms=42031;input_snapshot.elapsed_seconds=42;input_snapshot.heard_ms=12000;
    test_now+=2000000;podcast_sync_clock(&sync_state,(uint64_t)test_now/1000);sync_observe_snapshot(&input_snapshot,false);
    assert(sync_state.records[resumed].position_ms==42031&&sync_state.records[resumed].heard_ms==2000&&!atomic_load(&resume_intent));
    input_snapshot.closed_session_id=7;input_snapshot.closed_elapsed_ms=42063;input_snapshot.closed_heard_ms=12032;input_snapshot.session_id=8;strcpy(input_snapshot.episode_id,"ep1");
    sync_observe_snapshot(&input_snapshot,false);assert(sync_state.records[old].heard_ms==10000&&sync_state.records[resumed].heard_ms==2032);
    assert(podcast_sync_event_body(&sync_state,old,request_bodies[7],sizeof(request_bodies[7]))&&strstr(request_bodies[7],"\"seq\":2")&&strstr(request_bodies[7],"\"state\":\"stopped\"")&&strstr(request_bodies[7],"\"listened_ms\":10000")); /* Old close retains its original 10s total; new audio is counted only under the resumed session. */
    podcast_sync_t rebooted;assert(podcast_sync_init(&rebooted,0)&&rebooted.records[resumed].heard_origin_ms==10000&&rebooted.records[resumed].heard_ms==2032);
    puts("Controller central resume: 10s device pause -> 40.031s web position, new session before audio, exact millisecond seek, no old-tail overwrite or double-counted heard audio, durable origin PASS");
}
static void test_resume_cancel_and_offline(void)
{
    prepare_paused_sync();respond_paused_ack(8,true);respond("/api/listening/progress/show_a/ep0?lite=1",200,progress_json(40000,8));cancel_during_path="/api/listening/progress/";
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);resume_current_async();assert(resumes==0&&seek_to_calls==0&&pauses==1&&response_read==3&&!atomic_load(&resume_intent));
    prepare_paused_sync();respond_paused_ack(8,true);respond("/api/listening/progress/show_a/ep0?lite=1",200,progress_json(40000,8));respond("/api/listening/sessions",200,resume_session_json(40000,false));cancel_during_path="/api/listening/sessions";
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);resume_current_async();assert(resumes==0&&seek_to_calls==0&&sync_active==-1&&resume_record==-1&&pauses==1);
    assert(sync_state.records[1].position_ms==40000&&sync_state.records[1].state==PODCAST_SYNC_STOPPED&&sync_state.records[1].heard_ms==0);
    int offline=prepare_paused_sync();respond_paused_ack(2,false);respond("/api/listening/progress/show_a/ep0?lite=1",503,NULL);
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);resume_current_async();assert(resumes==1&&seek_to_calls==0&&sync_active==offline&&sync_offline);
    assert(sync_state.records[sync_active].base_revision==2&&sync_state.counter==1&&strstr(notice,"离线续播"));
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);assert(pauses==1&&!atomic_load(&resume_intent));settle_resume_cancel();assert(sync_active==offline&&resume_record==-1&&resumes==1);
    assert(sync_state.records[offline].state==PODCAST_SYNC_PAUSED&&sync_state.records[offline].heard_ms==10000);
    prepare_paused_sync();respond_paused_ack(8,true);respond("/api/listening/progress/show_a/ep0?lite=1",200,progress_json(40000,8));respond("/api/listening/sessions",200,resume_session_json(50000,true));
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);resume_current_async();assert(resumes==0&&seek_to_calls==0&&sync_active==-1&&strstr(notice,"进度已更新"));
    prepare_paused_sync();respond_paused_ack(8,true);respond("/api/listening/progress/show_a/ep0?lite=1",200,progress_json(40000,8));respond("/api/listening/sessions",503,NULL);
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);resume_current_async();assert(resumes==1&&last_seek_to_ms==40000&&sync_offline);char original[512],retry[512];
    assert(podcast_sync_creation_body(&sync_state,sync_active,original,sizeof(original))&&!strcmp(original,request_bodies[3]));
    podcast_sync_t rebooted;assert(podcast_sync_init(&rebooted,0)&&podcast_sync_creation_body(&rebooted,sync_active,retry,sizeof(retry))&&!strcmp(original,retry));
    puts("Controller resume failures: second middle press during GET/POST cancels before sound, offline resume retains base revision and visible warning, immediate re-pause cannot secretly reopen, stale owner never starts, lost-ACK creation remains immutable on reboot PASS");
}
static void test_silent_resume_position_ack(void)
{
    prepare_paused_sync();respond_paused_ack(8,true);respond("/api/listening/progress/show_a/ep0?lite=1",200,progress_json(40000,8));respond("/api/listening/sessions",200,resume_session_json(40000,false));
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);resume_current_async();int at=sync_active;
    input_snapshot.state=PODCAST_PLAYER_PLAYING;input_snapshot.volume=0;input_snapshot.absolute_seek_id=1;input_snapshot.elapsed_ms=45000;input_snapshot.elapsed_seconds=45;
    sync_observe_snapshot(&input_snapshot,false);assert(!atomic_load(&resume_intent)&&resume_record==-1&&sync_state.records[at].position_ms==45000&&sync_state.records[at].heard_ms==0);
    input_snapshot.state=PODCAST_PLAYER_PAUSED;sync_observe_snapshot(&input_snapshot,false);assert(sync_state.records[at].position_ms==45000&&sync_state.records[at].state==PODCAST_SYNC_PAUSED&&sync_state.records[at].heard_ms==0);
    puts("Controller silent resume: audio-worker absolute-position ACK clears waiting even at volume zero, retains advancing position and zero heard duration; old pre-ACK paused bookmark cannot overwrite selected central position PASS");
}
static void test_resume_flush_bound_and_full_local_outbox(void)
{
    int old=prepare_paused_sync_with_earlier_pending(true);char original[512],after[512];
    assert(podcast_sync_event_body(&sync_state,old,original,sizeof(original))&&strstr(original,"\"position_ms\":2000")&&strstr(original,"\"state\":\"playing\""));
    respond_paused_ack(2,false);respond_paused_ack(3,false);
    /* Until these two POSTs are accepted the central cursor is still 2s,
     * while the physical player is paused at 10s. The fixture enforces that
     * the old immutable body and newest pause precede the central GET. */
    respond("/api/listening/progress/show_a/ep0?lite=1",200,progress_json(10000,3));respond("/api/listening/sessions",200,resume_session_json(10000,false));
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);resume_current_async();
    assert(response_read==5&&resumes==1&&seek_to_calls==1&&last_seek_to_ms==10000&&sync_active!=old);
    assert(!strcmp(request_bodies[1],original));
    assert(strstr(request_bodies[2],"\"seq\":2")&&strstr(request_bodies[2],"\"state\":\"paused\"")&&strstr(request_bodies[2],"\"position_ms\":10000")&&strstr(request_bodies[2],"\"listened_ms\":10000"));
    assert(sync_state.records[sync_active].heard_origin_ms==10000&&sync_state.records[sync_active].heard_ms==0);

    old=prepare_paused_sync_with_earlier_pending(true);
    for(unsigned i=1;i<PODCAST_SYNC_SLOTS;i++){char id[16];snprintf(id,sizeof(id),"offline%u",i);assert(podcast_sync_begin(&sync_state,"show_b",id,0,0,true,false)==(int)i);}
    uint64_t count=sync_state.counter;assert(podcast_sync_event_body(&sync_state,old,original,sizeof(original)));
    respond("/api/listening/events",503,NULL);
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);resume_current_async();
    assert(response_read==2&&resumes==1&&seek_to_calls==0&&sync_active==old&&sync_state.counter==count&&sync_offline);
    assert(local_resume_session==7&&atomic_load(&resume_intent)&&resume_record==-1&&sync_state.records[old].heard_origin_ms==0);
    assert(podcast_sync_event_body(&sync_state,old,after,sizeof(after))&&!strcmp(original,after));
    input_snapshot.state=PODCAST_PLAYER_PLAYING;input_snapshot.elapsed_ms=11000;input_snapshot.elapsed_seconds=11;input_snapshot.heard_ms=11000;
    test_now+=1000000;podcast_sync_clock(&sync_state,(uint64_t)test_now/1000);sync_observe_snapshot(&input_snapshot,false);
    assert(!atomic_load(&resume_intent)&&local_resume_session==0&&sync_state.records[old].heard_ms==11000&&sync_state.counter==count);
    assert(podcast_sync_event_body(&sync_state,old,after,sizeof(after))&&!strcmp(original,after));

    old=prepare_paused_sync_with_earlier_pending(true);respond_paused_ack(2,false);cancel_during_path="/api/listening/events";
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);resume_current_async();
    assert(response_read==2&&resumes==0&&seek_to_calls==0&&pauses==1&&!atomic_load(&resume_intent)&&sync_state.counter==1&&sync_active==old);
    assert(sync_state.records[old].position_ms==10000&&sync_state.records[old].heard_ms==10000);
    puts("Controller resume local authority: at most two immutable/current-pause flushes before central GET; failed sync with eight full records resumes same physical generation without a slot/seek/origin reset, original body remains exact, first POST cancellation starts no sound PASS");
}
static void test_local_resume_never_revives_terminal_or_mismatched_generation(void)
{
    for(unsigned invalid=0;invalid<3;invalid++){
        int old=prepare_paused_sync();
        for(unsigned i=1;i<PODCAST_SYNC_SLOTS;i++){char id[16];snprintf(id,sizeof(id),"offline%u",i);assert(podcast_sync_begin(&sync_state,"show_b",id,0,0,true,false)==(int)i);}
        if(invalid==0)sync_state.records[old].state=PODCAST_SYNC_STOPPED;
        else if(invalid==1)sync_state.records[old].state=PODCAST_SYNC_ENDED;
        else input_snapshot.session_id=8;
        podcast_sync_record_t before=sync_state.records[old];
        demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);resume_current_async();
        assert(resumes==0&&seek_to_calls==0&&!atomic_load(&resume_intent)&&response_read==1&&sync_state.counter==8);
        assert(!memcmp(&before,&sync_state.records[old],sizeof(before)));
    }
    puts("Controller local resume guard: full outbox never revives stopped/ended or mismatched audio generations, saved records remain intact PASS");
}
static cJSON *show_page_json(unsigned offset,unsigned total)
{
    cJSON *j=node(JSON_OBJECT),*array=node(JSON_ARRAY);add(j,"s",array);number_item(j,"offset",(int)offset);number_item(j,"total",(int)total);
    for(unsigned i=offset;i<offset+8&&i<total;i++){cJSON *item=node(JSON_OBJECT);char id[24];snprintf(id,sizeof(id),"show_%02u",i);text_item(item,"i",id);text_item(item,"n","节目甲");number_item(item,"p",(int)(1000+i));number_item(item,"u",(int)i);number_item(item,"c",(int)i/2);number_item(item,"l",(int)i*1000);add(array,NULL,item);}return j;
}
static void test_paged_show_exchange(void)
{
    reset_controller();show_count=1;strcpy(shows[0].id,"old");
    respond("/api/shows_lite?offset=0&limit=8",200,show_page_json(0,32));respond("/api/shows_lite?offset=8&limit=8",503,NULL);
    assert(!fetch_shows()&&show_count==1&&!strcmp(shows[0].id,"old")); /* Failed page cannot erase the visible library. */
    reset_controller();for(unsigned i=0;i<4;i++){static const char *paths[]={"/api/shows_lite?offset=0&limit=8","/api/shows_lite?offset=8&limit=8","/api/shows_lite?offset=16&limit=8","/api/shows_lite?offset=24&limit=8"};respond(paths[i],200,show_page_json(i*8,32));}
    assert(fetch_shows()&&show_count==32&&response_read==4&&!strcmp(shows[0].id,"show_31")&&!strcmp(shows[31].id,"show_00"));
    assert(shows[0].played==31&&shows[0].completed==15&&shows[0].heard_ms==31000);
    puts("Controller catalogue: four bounded 8-show pages atomically replace 32 subscriptions; failed later page preserves old library; update sort and real listening stats retained PASS");
}
static void test_offline_revision_resume_choice(void)
{
    for(unsigned newer=0;newer<2;newer++){
        reset_controller();sync_nvs_available=true;assert(podcast_sync_init(&sync_state,991));select_current();
        int old=podcast_sync_begin(&sync_state,current.show_id,current.episode_id,100000,4,true,false);assert(old>=0);
        podcast_sync_observe(&sync_state,old,0,110000,10000,PODCAST_SYNC_STOPPED,false);assert(podcast_sync_freeze(&sync_state,old));
        current.byte_offset=8000*32;
        cJSON *j=details_json(true,false);number_item(j,"p",40031);number_item(j,"v",newer?7:4);number_item(j,"c",1);respond("/api/episodes/show_a/ep0?lite=1",200,j);
        respond("/api/listening/sessions",200,sync_session_json(newer?40031:110000));assert(details(true));
        assert(started_cursor.segment==0&&started_cursor.byte_offset==(newer?40031:110000)*32);
        assert(strstr(request_bodies[1],newer?"\"position_ms\":40031":"\"position_ms\":110000"));
        assert(sync_state.records[old].position_ms==110000&&sync_state.records[old].heard_ms==10000); /* Neither choice deletes the unsent listening total. */
    }
    puts("Controller resume authority: newer central revision wins over old offline position; unchanged revision retains newest durable local progress even when legacy bookmark is older; unsent heard history survives both choices PASS");
}
static void test_new_outbox_never_imported_as_legacy(void)
{
    reset_controller();sync_nvs_available=true;assert(podcast_sync_init(&sync_state,111));select_current();
    int at=podcast_sync_begin(&sync_state,current.show_id,current.episode_id,0,0,true,false);assert(at>=0);
    podcast_sync_observe(&sync_state,at,0,5000,0,PODCAST_SYNC_ENDED,false);assert(podcast_sync_freeze(&sync_state,at));
    current.byte_offset=5000*32;current.finished=true;
    assert(import_bookmark(&current)&&response_read==0&&sync_state.records[at].heard_ms==0&&sync_state.records[at].base_revision==0);
    assert(!last_import_request[0]); /* No invented legacy completion or revision ahead of the real outbox. */
    puts("Controller migration: new offline mute/seek-to-EOF bookmarks never enter legacy import; known real ledger alone reports completion/actual heard time, original base revision survives reconnect PASS");
}
static void test_legacy_zero_revision_resume(void)
{
    reset_controller();sync_nvs_available=true;assert(podcast_sync_init(&sync_state,456));select_current();current.byte_offset=123000*32;
    respond("/api/listening/import",503,NULL);cJSON *j=details_json(true,false);number_item(j,"p",0);number_item(j,"v",0);number_item(j,"c",0);
    respond("/api/episodes/show_a/ep0?lite=1",200,j);respond("/api/listening/sessions",200,sync_session_json(123000));assert(details(true));
    assert(started_cursor.segment==0&&started_cursor.byte_offset==123000*32&&strstr(request_bodies[2],"\"position_ms\":123000")&&strstr(request_bodies[2],"\"base_revision\":0"));
    assert(strstr(request_bodies[0],"\"position_ms\":123000")&&!strstr(request_bodies[0],"listened_ms"));
    puts("Controller migration: failed legacy import never erases local checkpoint; empty central revision preserves prior resume, no fabricated listened duration PASS");
}
static void test_live_return_and_screen_idle(void)
{
    reset_controller();select_current();input_snapshot.state=PODCAST_PLAYER_PLAYING;
    input_snapshot.elapsed_seconds=123;player=input_snapshot;has_recent=true;
    strcpy(recent.show_id,"show_b");strcpy(recent.episode_id,"old");recent_position_ms=1000;
    page=PODCAST_SHOWS;publish();assert(view.recent_is_current&&view.recent_elapsed==123&&!strcmp(view.recent_title,current.title));
    continue_recent();assert(page==PODCAST_NOW&&stops==0&&starts==0&&resumes==0&&seek_calls==0);
    page=PODCAST_ACTIONS;publish();test_now+=6999000;ui_tick(NULL);assert(page==PODCAST_ACTIONS);
    test_now+=1000;ui_tick(NULL);assert(page==PODCAST_NOW&&pauses==0&&stops==0&&seek_calls==0);
    page=PODCAST_SEEK;publish();test_now+=8000000;ui_tick(NULL);assert(page==PODCAST_SEEK);
    player.state=input_snapshot.state=PODCAST_PLAYER_PAUSED;page=PODCAST_SLEEP;publish();ui_tick(NULL);assert(page==PODCAST_SLEEP);
    player.state=input_snapshot.state=PODCAST_PLAYER_PLAYING;page=PODCAST_ACTIONS;action_selection=0;key(BSP_BTN_OK,BSP_BTN_CLICK);assert(page==PODCAST_NOW);
    /* Independently crossed stamp rollover, not a 24-day false timeout. */
    test_now=(INT64_C(1)<<31)*1000+5000;atomic_store(&input_state,input_stamp(test_now-6999000));page=PODCAST_ACTIONS;publish();
    ui_tick(NULL);assert(page==PODCAST_ACTIONS);test_now+=1000;ui_tick(NULL);assert(page==PODCAST_NOW);

    reset_controller();select_current();input_snapshot.state=PODCAST_PLAYER_PLAYING;player=input_snapshot;page=PODCAST_NOW;publish();
    test_now+=14999000;ui_tick(NULL);assert(backlight_calls==0);
    test_now+=1000;unsigned before=render_calls;ui_tick(NULL);assert(backlight_value==0&&render_calls==before&&pauses==0);
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_PRESS);assert(backlight_value==0&&pauses==0); /* Callback does no screen IO. */
    input_snapshot.elapsed_seconds=222;ui_tick(NULL);assert(backlight_value==85&&rendered_view.elapsed==222);
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);assert(pauses==0&&input_queue_sends==0);
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_PRESS);demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);assert(pauses==1);

    reset_controller();select_current();input_snapshot.state=PODCAST_PLAYER_PLAYING;player=input_snapshot;page=PODCAST_NOW;publish();
    atomic_fetch_or(&input_state,INPUT_SLEEP_BIT);ui_tick(NULL);
    demo_podcast_key(BSP_BTN_UP,BSP_BTN_PRESS);test_now+=100000;demo_podcast_key(BSP_BTN_UP,BSP_BTN_PRESS);
    demo_podcast_key(BSP_BTN_UP,BSP_BTN_DOUBLE);assert(relative_volume_calls==0&&input_queue_sends==0);
    demo_podcast_key(BSP_BTN_UP,BSP_BTN_PRESS);demo_podcast_key(BSP_BTN_UP,BSP_BTN_CLICK);assert(relative_volume_calls==1);
    atomic_fetch_or(&input_state,INPUT_SLEEP_BIT);demo_podcast_key(BSP_BTN_OK,BSP_BTN_PRESS);test_now+=500000;
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_LONG);assert(page==PODCAST_NOW&&input_queue_sends==0);
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_PRESS);demo_podcast_key(BSP_BTN_OK,BSP_BTN_LONG);assert(input_queue_sends==1);
    /* Old inactivity snapshot cannot win after concurrent PRESS. */
    uint32_t stale=atomic_load(&input_state);test_now+=10000;demo_podcast_key(BSP_BTN_UP,BSP_BTN_PRESS);
    assert(!atomic_compare_exchange_strong(&input_state,&stale,stale|INPUT_SLEEP_BIT));
    page=PODCAST_ACTIONS;action_selection=10;key(BSP_BTN_OK,BSP_BTN_CLICK);assert(page==PODCAST_NOW&&(atomic_load(&input_state)&INPUT_SLEEP_BIT));
    assert(stops==0&&starts==0&&seek_calls==0);
    puts("Controller idle: live progress/return without audio change, 7s boundary/protected edits, 15s actual backlight and redraw, whole wake gesture/CAS rollover and manual screen-off PASS");
}
static void observe_ui_during_http(void)
{
    /* esp_http_client_fetch_headers has not returned yet: the catalogue
     * worker cannot run idle_navigation or publish a new playback page. */
    test_now+=6999000;ui_tick(NULL);
    assert(page==PODCAST_ACTIONS&&rendered_view.page==PODCAST_ACTIONS);
    input_snapshot.elapsed_seconds=130;test_now+=1000;ui_tick(NULL);
    assert(page==PODCAST_NOW&&view.page==PODCAST_NOW&&rendered_view.page==PODCAST_NOW&&rendered_view.elapsed==130);
    assert(stops==0&&starts==0&&seek_calls==0&&seek_to_calls==0&&pauses==0);
    test_now+=8000000;unsigned before=render_calls;ui_tick(NULL);
    assert(backlight_value==0&&render_calls==before&&page==PODCAST_NOW);
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_PRESS);input_snapshot.elapsed_seconds=138;ui_tick(NULL);
    assert(backlight_value==85&&rendered_view.page==PODCAST_NOW&&rendered_view.elapsed==138);
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);assert(pauses==0&&input_queue_sends==0);
}
static void test_ui_return_during_network_wait(void)
{
    reset_controller();select_current();input_snapshot.state=PODCAST_PLAYER_PLAYING;input_snapshot.elapsed_seconds=123;
    player=input_snapshot;page=PODCAST_ACTIONS;publish();
    respond("/api/listening/events",503,NULL);ui_during_http=true;
    assert(request("/api/listening/events","{}") == NULL);
    assert(page==PODCAST_NOW&&view.page==PODCAST_NOW&&rendered_view.elapsed==138);
    demo_podcast_key(BSP_BTN_OK,BSP_BTN_CLICK);assert(pauses==1&&input_queue_sends==0);

    /* Another show's list must not become the current player's cover or
     * identity when returning independently of the catalogue worker. */
    reset_controller();select_current();input_snapshot.state=PODCAST_PLAYER_PLAYING;player=input_snapshot;
    strcpy(shows[0].id,"show_b");strcpy(shows[0].name,"另外一个节目");show_count=1;
    page=PODCAST_EPISODES;publish();assert(!strcmp(view.show_id,"show_b"));
    test_now+=7000000;input_snapshot.elapsed_seconds=222;ui_tick(NULL);
    assert(page==PODCAST_NOW&&view.page==PODCAST_NOW&&!strcmp(view.show_id,"show_a"));
    assert(!strcmp(rendered_view.show,current.name)&&!strcmp(rendered_view.title,current.title)&&rendered_view.elapsed==222);

    /* A worker publication drafted before this UI return cannot restore the
     * old action page when it eventually obtains the shared view lock. */
    reset_controller();select_current();input_snapshot.state=PODCAST_PLAYER_PLAYING;player=input_snapshot;
    page=PODCAST_ACTIONS;publish();test_now+=7000000;return_during_publish=true;publish();
    assert(page==PODCAST_NOW&&view.page==PODCAST_NOW&&rendered_view.page==PODCAST_NOW);
    publish();assert(view.page==PODCAST_NOW);

    /* Use actual audio state rather than the worker's cached PLAYING state;
     * protected seek/preparation and a fresh physical PRESS keep their page. */
    reset_controller();select_current();input_snapshot.state=PODCAST_PLAYER_PLAYING;player=input_snapshot;
    page=PODCAST_ACTIONS;publish();test_now+=7000000;input_snapshot.state=PODCAST_PLAYER_PAUSED;ui_tick(NULL);
    assert(page==PODCAST_ACTIONS);
    input_snapshot.state=PODCAST_PLAYER_PLAYING;press_before_snapshot=true;ui_tick(NULL);assert(page==PODCAST_ACTIONS);
    test_now+=7000000;page=PODCAST_SEEK;publish();ui_tick(NULL);assert(page==PODCAST_SEEK);
    page=PODCAST_ACTIONS;preparing=true;publish();ui_tick(NULL);assert(page==PODCAST_ACTIONS);
    preparing=false;pairing_invalid=true;publish();ui_tick(NULL);assert(page==PODCAST_ACTIONS);
    pairing_invalid=false;awaiting_session=true;start_session=input_snapshot.session_id;publish();ui_tick(NULL);assert(page==PODCAST_ACTIONS);
    awaiting_session=false;publish();input_snapshot.state=PODCAST_PLAYER_BUFFERING;ui_tick(NULL);assert(page==PODCAST_NOW);

    /* A tick arriving after both deadlines must still commit the return while
     * blank, so its first wake immediately shows current progress. */
    reset_controller();select_current();input_snapshot.state=PODCAST_PLAYER_PLAYING;player=input_snapshot;
    page=PODCAST_SLEEP;publish();test_now+=16000000;ui_tick(NULL);
    assert(backlight_value==0&&page==PODCAST_NOW&&view.page==PODCAST_NOW);
    demo_podcast_key(BSP_BTN_UP,BSP_BTN_PRESS);input_snapshot.elapsed_seconds=333;ui_tick(NULL);
    assert(backlight_value==85&&rendered_view.page==PODCAST_NOW&&rendered_view.elapsed==333);
    demo_podcast_key(BSP_BTN_UP,BSP_BTN_CLICK);assert(relative_volume_calls==0&&input_queue_sends==0);

    reset_controller();select_current();input_snapshot.state=PODCAST_PLAYER_PLAYING;player=input_snapshot;
    show_count=3;page=PODCAST_SHOWS;publish();assert(!has_recent&&view.has_recent&&view.count==2);
    puts("Controller UI idle: real HTTP wait spans 7s return/15s blank/wake, live progress and current identity, immediate next controls, stale worker publish and protected views PASS");
}
static void test_selectable_recent_card(void)
{
    reset_controller();select_current();player.state=input_snapshot.state=PODCAST_PLAYER_PLAYING;
    input_snapshot.elapsed_seconds=222;player=input_snapshot;show_count=2;has_recent=true;
    strcpy(recent.show_id,"show_b");strcpy(recent.episode_id,"older");recent_position_ms=1000;
    page=PODCAST_SHOWS;publish();assert(show_selection==-1&&view.selected==-1&&view.recent_is_current&&view.recent_elapsed==222);
    key(BSP_BTN_UP,BSP_BTN_CLICK);assert(show_selection==-1);
    key(BSP_BTN_DOWN,BSP_BTN_CLICK);publish();assert(show_selection==0&&view.selected==0);
    key(BSP_BTN_UP,BSP_BTN_CLICK);publish();assert(show_selection==-1&&view.selected==-1);
    key(BSP_BTN_OK,BSP_BTN_CLICK);
    assert(page==PODCAST_NOW&&!strcmp(current.show_id,"show_a")&&!strcmp(current.episode_id,"ep0"));
    assert(starts==0&&stops==0&&resumes==0&&seek_calls==0&&seek_to_calls==0&&response_read==0);

    /* Selecting a paused card explicitly asks to continue listening. Ordinary
     * return-to-player and automatic return remain separate navigation paths. */
    reset_controller();select_current();player.state=input_snapshot.state=PODCAST_PLAYER_PAUSED;player=input_snapshot;
    page=PODCAST_SHOWS;publish();assert(view.selected==-1);
    key(BSP_BTN_OK,BSP_BTN_CLICK);assert(page==PODCAST_NOW&&resumes==1&&stops==0&&starts==0&&seek_to_calls==0);
    reset_controller();sync_nvs_available=true;assert(podcast_sync_init(&sync_state,456));select_current();
    player.state=input_snapshot.state=PODCAST_PLAYER_PAUSED;player=input_snapshot;page=PODCAST_SHOWS;publish();
    key(BSP_BTN_OK,BSP_BTN_CLICK);assert(page==PODCAST_NOW&&atomic_load(&resume_intent)&&atomic_load(&direct_transport)==2);
    assert(resumes==0&&response_read==0&&stops==0&&starts==0&&seek_to_calls==0);

    /* The card works even when there are no subscribed shows. A historical
     * card goes through the normal single-episode load using its own cursor. */
    reset_controller();has_recent=true;strcpy(recent.show_id,"show_b");strcpy(recent.episode_id,"older");
    strcpy(recent.name,"节目乙");strcpy(recent.title,"上次听到这里");recent.byte_offset=91000*32;recent_position_ms=91000;
    page=PODCAST_SHOWS;publish();assert(view.has_recent&&view.selected==-1&&view.count==0);
    key(BSP_BTN_DOWN,BSP_BTN_CLICK);assert(show_selection==-1);
    respond("/api/episodes/show_b/older?lite=1",200,details_json(true,false));key(BSP_BTN_OK,BSP_BTN_CLICK);
    assert(page==PODCAST_NOW&&starts==1&&!strcmp(started_show,"show_b")&&!strcmp(started_episode,"older"));
    assert(started_cursor.segment==0&&started_cursor.byte_offset==91000*32);

    reset_controller();has_recent=true;strcpy(recent.show_id,"show_b");strcpy(recent.episode_id,"older");
    for(unsigned i=0;i<4;i++){
        static const char *paths[]={"/api/shows_lite?offset=0&limit=8","/api/shows_lite?offset=8&limit=8","/api/shows_lite?offset=16&limit=8","/api/shows_lite?offset=24&limit=8"};
        respond(paths[i],200,show_page_json(i*8,32));
    }
    assert(fetch_shows());publish();assert(show_selection==-1&&view.count==2&&view.total==32);
    for(int i=0;i<MAX_SHOWS;i++){
        key(BSP_BTN_DOWN,BSP_BTN_CLICK);publish();assert(show_selection==i&&view.selected==i%2&&view.absolute_selected==i);
        assert(!strcmp(view.rows[view.selected].show_id,shows[i].id));
    }
    key(BSP_BTN_DOWN,BSP_BTN_CLICK);assert(show_selection==31);
    for(unsigned i=0;i<MAX_SHOWS;i++)key(BSP_BTN_UP,BSP_BTN_CLICK);
    publish();assert(show_selection==-1&&view.selected==-1);
    key(BSP_BTN_DOWN,BSP_BTN_LONG);assert(show_selection==31);
    respond("/api/shows/show_00/episodes?offset=0&limit=3",200,episodes_json(0));key(BSP_BTN_OK,BSP_BTN_CLICK);
    assert(page==PODCAST_EPISODES&&browse_show==31);
    key(BSP_BTN_OK,BSP_BTN_LONG);key(BSP_BTN_UP,BSP_BTN_LONG);assert(page==PODCAST_SHOWS&&show_selection==-1);
    respond("/api/shows_lite",200,shows_json());assert(fetch_shows());publish();assert(show_selection==-1&&view.selected==-1);
    puts("Controller library: selectable recent first item, up/down/OK and explicit paused resume, current audio untouched, empty catalogue/history cursor, all 32 shows paged and recent focus retained across refresh PASS");
}
static void test_optional_cover_controller_integration(void)
{
    reset_controller();select_current();player.state=input_snapshot.state=PODCAST_PLAYER_PLAYING;player=input_snapshot;
    page=PODCAST_NOW;publish();ui_tick(NULL);assert(art_wanted_count==1&&!strcmp(art_wanted[0],"show_a"));
    assert(art_poll_calls==1&&response_read==0&&starts==0&&pauses==0);
    show_count=3;for(unsigned i=0;i<3;i++)snprintf(shows[i].id,sizeof(shows[i].id),"visible_%u",i);
    page=PODCAST_SHOWS;publish();ui_tick(NULL);
    assert(art_wanted_count==3&&!strcmp(art_wanted[0],"show_a")&&!strcmp(art_wanted[1],"visible_0")&&!strcmp(art_wanted[2],"visible_1"));
    page=PODCAST_ACTIONS;publish();ui_tick(NULL);assert(art_wanted_count==0);
    page=PODCAST_NOW;publish();test_now+=15000000;ui_tick(NULL);assert(art_wanted_count==0&&backlight_value==0&&pauses==0);

    reset_controller();select_current();assert(start_current(false));assert(starts==1&&art_prepare_calls==1);
    /* Artwork task/allocation failure is optional: the normal audio service
     * still initializes, owns its loop and exits cleanly. */
    reset_controller();show_count=1;page=PODCAST_ACTIONS;art_start_ok=false;loop_budget=1;
    service_task(NULL);assert(art_start_calls==1&&receive_count==1);
    assert(demo_podcast_stop()==ESP_OK&&art_stop_calls==1);
    reset_controller();art_stop_ok=false;assert(demo_podcast_stop()==ESP_ERR_TIMEOUT&&art_stop_calls==1);
    puts("Controller artwork: only current/visible IDs, no callback/UI HTTP, screen-off releases wants, audio START yields optional RAM, optional-worker failure never blocks audio and teardown waits PASS");
}

int main(void)
{
    test_optional_cover_controller_integration();
    test_selectable_recent_card();
    test_ui_return_during_network_wait();
    test_live_return_and_screen_idle();
    setvbuf(stdout, NULL, _IONBF, 0);
    test_paused_cross_device_resume();
    test_resume_cancel_and_offline();
    test_silent_resume_position_ack();
    test_resume_flush_bound_and_full_local_outbox();
    test_local_resume_never_revives_terminal_or_mismatched_generation();
    test_paged_show_exchange();
    test_sync_http_and_reboot();
    test_sync_eof_and_closed_tail();
    test_full_outbox_keeps_current_audio();
    test_recent_and_episode_listen_states();
    test_immediate_local_transport();
    test_offline_revision_resume_choice();
    test_new_outbox_never_imported_as_legacy();
    test_legacy_zero_revision_resume();
    test_show_to_episodes();
    test_full_show_capacity_and_bounded_response();
    test_failed_page_retries_instead_of_playing_old_episode();
    test_actual_three_button_actions();
    test_checkpoint_identity_and_paused_displacement();
    test_failed_save_is_throttled();
    test_expired_sleep_prevents_ready_preparation_auto_start();
    test_prepare_failure_can_be_retried();
    test_artwork_identity_and_separate_sleep_display();
    test_latest_update_sort_and_identity_focus();
    test_order_toggle_preserves_same_episode_and_frozen_play_order();
    test_actual_service_eof_policies();
    test_volume_fast_path_and_direct_snapshot_render();
    test_volume_direction_limits_captions_and_navigation();
    test_progress_apply_cancel_and_limits();
    test_prefetch_retry_dedup_and_recent_failure_identity();
    test_serviceloop_retries_prefetch_after_30_seconds_then_deduplicates();
    reset_controller(); pairing_invalid = false; has_current = has_recent = false; page = PODCAST_SHOWS;
    handle_key((podcast_key_event_t){BSP_BTN_OK,BSP_BTN_LONG});
    assert(page == PODCAST_ACTIONS && action_selection == ACTION_CONNECTION);
    setup_request_ok = false; restart_setup = false; s_quit = false;
    handle_key((podcast_key_event_t){BSP_BTN_OK,BSP_BTN_CLICK});
    assert(!restart_setup && !s_quit && strstr(notice,"保存失败"));
    setup_request_ok = true;
    handle_key((podcast_key_event_t){BSP_BTN_OK,BSP_BTN_CLICK});
    assert(restart_setup && s_quit);
    reset_controller(); pairing_invalid = true; has_recent = true; page = PODCAST_SHOWS;
    handle_key((podcast_key_event_t){BSP_BTN_OK,BSP_BTN_LONG});
    assert(page == PODCAST_ACTIONS && action_selection == ACTION_CONNECTION);
    has_current = true; ready_storage = false; restart_setup = false; s_quit = false;
    handle_key((podcast_key_event_t){BSP_BTN_OK,BSP_BTN_CLICK});
    assert(!restart_setup && !s_quit && strstr(notice,"进度保存失败"));
    puts("Controller settings: empty library and revoked pairing reach setup, failed config/progress persistence does not reboot, existing controls retained PASS");
    reset_controller(); pairing_invalid = false; restart_setup = false;
    puts("podcast controller: 32 shows, UTF-8 name/title bounds, oversized response rejection, navigation, retry, buttons, checkpoint and sleep PASS");
    return 0;
}
