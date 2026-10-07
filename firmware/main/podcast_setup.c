#ifdef PODCAST_SETUP_TEST
#include "podcast_setup_test_adapter.h"
#else
#include "podcast_setup.h"
#include "podcast_config.h"
#include "podcast_sync.h"
#include "podcast_setup_screen.h"
#include "bsp_display.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#endif
#include "podcast_config.h"
#include "podcast_sync.h"
#include "podcast_setup_screen.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A boot-only, one-client service: the audio decoder and player have not been
 * started. Reconfiguration restarts into this same service after checkpointing.
 * Wi-Fi trial uses RAM storage. Only a successful claim is committed to NVS. */
typedef struct { podcast_config_t cfg; char code[7]; } setup_input_t;
static atomic_bool active, cancelled, connected, busy;
static QueueHandle_t submissions;
static SemaphoreHandle_t status_lock;
static char status[128], setup_key[33], hotspot[32], hotspot_password[17], device_id[24];
static httpd_handle_t web;
static esp_netif_t *sta_netif, *ap_netif;
static esp_event_handler_instance_t wifi_handler, ip_handler;
static bool screen_ready;
static bool wifi_initialized, wifi_started, can_cancel, sntp_initialized;
static podcast_sync_t identity_store;

static const char html_start[] =
"<!doctype html><html lang=zh-CN><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>播客机连接设置</title><style>*{scrollbar-width:none}*::-webkit-scrollbar{display:none}html{font-family:system-ui;color:#151515;background:#f7f8fa}body{max-width:440px;margin:0 auto;padding:32px 22px}"
"h1{font-size:30px;margin:0 0 12px}p{line-height:1.7;color:#626874}label{display:block;margin:22px 0 8px}input{box-sizing:border-box;width:100%;padding:14px;border:1px solid #ced2da;border-radius:10px;font-size:16px;background:#fff}"
"button{width:100%;padding:15px;margin-top:28px;border:0;border-radius:10px;background:#0667f6;color:#fff;font-size:17px}button:disabled{opacity:.5}#status{min-height:48px}small{display:block;line-height:1.7;color:#626874}</style>"
"<h1>让播客机连接你的后台</h1><p>先在自己的播客后台‘设备’页面生成配对码，再填写下面的信息。设备会验证连接，成功后才保存。</p>"
"<form id=f><label>2.4 GHz 无线网络名称</label><input id=ssid required maxlength=32 autocomplete=off list=wifi><datalist id=wifi></datalist>"
"<button type=button id=scan style=margin-top:10px>扫描附近网络</button>"
"<label>无线网络密码</label><input id=password type=password maxlength=63 autocomplete=off><small>开放网络可以留空；其他网络密码至少 8 个字符。</small>"
"<label>播客后台地址</label><input id=server type=url required maxlength=160 placeholder='http://nas.local:8899' autocomplete=off>"
"<small>填写服务器在局域网中的地址，或有可信证书的 HTTPS（加密网址）。localhost（本机）指的是播客机自己，不能使用。手机暂时留在此热点，不需要它能上网。</small>"
"<label>后台生成的六位配对码</label><input id=code inputmode=numeric pattern='[0-9]{6}' minlength=6 maxlength=6 required autocomplete=off>"
"<button id=submit>连接并保存</button></form><p id=status aria-live=polite></p><small>修改失败保留原设置与收听记录。已有设置时，长按设备确定键可取消。</small>"
"<script>const setupKey='";
static const char html_end[] =
"';let poll;const state=document.getElementById('status'),button=document.getElementById('submit'),scanButton=document.getElementById('scan');"
"scanButton.onclick=async()=>{scanButton.disabled=true;const old=scanButton.textContent;scanButton.textContent='正在扫描…';try{const r=await fetch('/api/wifi',{cache:'no-store'}),j=await r.json();const list=document.getElementById('wifi');list.innerHTML='';const n=(j.networks||[]).length;n.forEach(w=>{const o=document.createElement('option');o.value=w.ssid;list.appendChild(o);});state.textContent=n?('找到 '+n.length+' 个网络，点名称框选择。'):'没扫到网络，请手填名称。';}catch(e){state.textContent='扫描失败，请手填网络名称。';}scanButton.disabled=false;scanButton.textContent=old;};"
"async function refresh(){try{const r=await fetch('/api/status',{cache:'no-store'}),j=await r.json();state.textContent=j.message;button.disabled=j.busy;if(j.saved){clearInterval(poll);state.textContent='连接成功，播客机正在重启。手机可以切回原来的无线网络。';}}catch(e){state.textContent='连接暂时中断，手机可能正在重新连接设置热点；正在重试。';}}"
"document.getElementById('f').onsubmit=async(e)=>{e.preventDefault();button.disabled=true;state.textContent='正在提交连接信息…';"
"try{const r=await fetch('/api/setup',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({setup_key:setupKey,ssid:document.getElementById('ssid').value,password:document.getElementById('password').value,server:document.getElementById('server').value,code:document.getElementById('code').value})});const j=await r.json();state.textContent=j.message;if(!r.ok)button.disabled=false;}catch(e){button.disabled=false;state.textContent='提交未确认，请连接设备热点后重试。';}};poll=setInterval(refresh,1500);refresh();</script></html>";

static void draw(const char *message)
{
    if (status_lock && xSemaphoreTake(status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        snprintf(status, sizeof(status), "%s", message); xSemaphoreGive(status_lock);
    }
    if (screen_ready && bsp_lvgl_lock(200)) {
        podcast_setup_screen_message(message); bsp_lvgl_unlock();
    }
}
static bool create_screen(void)
{
    if (!bsp_lvgl_lock(1000)) return false;
    screen_ready = podcast_setup_screen_create(hotspot, hotspot_password, can_cancel);
    bsp_lvgl_unlock(); return screen_ready;
}
static esp_err_t json_reply(httpd_req_t *req, const char *message, const char *http_status)
{
    cJSON *j = cJSON_CreateObject(); if (!j) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(j, "message", message); char *body = cJSON_PrintUnformatted(j); cJSON_Delete(j);
    if (!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_status(req, http_status); httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store"); esp_err_t e = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN); free(body); return e;
}
static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8"); httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    if (httpd_resp_send_chunk(req, html_start, sizeof(html_start) - 1) != ESP_OK ||
        httpd_resp_send_chunk(req, setup_key, strlen(setup_key)) != ESP_OK ||
        httpd_resp_send_chunk(req, html_end, sizeof(html_end) - 1) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}
static esp_err_t status_get(httpd_req_t *req)
{
    char message[sizeof(status)] = "请填写连接信息";
    if (xSemaphoreTake(status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(message, status, sizeof(message)); xSemaphoreGive(status_lock);
    }
    cJSON *j = cJSON_CreateObject(); if (!j) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(j, "message", message); cJSON_AddBoolToObject(j, "busy", atomic_load(&busy));
    cJSON_AddBoolToObject(j, "saved", !strcmp(message, "连接成功，即将进入节目库"));
    char *body = cJSON_PrintUnformatted(j); cJSON_Delete(j); if (!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json; charset=utf-8"); httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN); free(body); return e;
}
static esp_err_t wifi_get(httpd_req_t *req)
{
    if (!wifi_started) return json_reply(req, "网络未就绪，请手填名称", "503 Service Unavailable");
    /* 扫描期间热点会短暂抖动；状态轮询已在页面容忍单次失败。 */
    wifi_scan_config_t scan = {0};
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) return json_reply(req, "扫描失败，请手填名称", "500 Internal Server Error");
    enum { MAX_APS = 16 };
    wifi_ap_record_t aps[MAX_APS];
    uint16_t count = MAX_APS;
    if (esp_wifi_scan_get_ap_records(&count, aps) != ESP_OK) count = 0;
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "{\"networks\":[");
    const char *prev = ""; bool first = true;
    for (uint16_t i = 0; i < count; i++) {
        if (!aps[i].ssid[0] || !strncmp(prev, (const char *)aps[i].ssid, sizeof(aps[i].ssid))) continue;
        prev = (const char *)aps[i].ssid;
        char safe[67] = ""; size_t out = 0;
        for (size_t in = 0; in < sizeof(aps[i].ssid) && out + 2 < sizeof(safe); in++) {
            unsigned char ch = aps[i].ssid[in];
            if (!ch) break;
            if (ch == '"' || ch == '\\') safe[out++] = '\\';
            if (ch < 0x20) ch = '?';
            safe[out++] = (char)ch;
        }
        char item[96];
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d}", first ? "" : ",", safe, aps[i].rssi);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_send_chunk(req, NULL, 0);
}
static const char *field(cJSON *j, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key); return cJSON_IsString(v) ? v->valuestring : NULL;
}
static esp_err_t setup_post(httpd_req_t *req)
{
    if (!atomic_load(&active)) return json_reply(req, "设置服务已关闭，请重启", "503 Service Unavailable");
    if (req->content_len <= 0 || req->content_len > 768) return json_reply(req, "填写内容过长", "413 Payload Too Large");
    char body[769]; size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return json_reply(req, "提交中断，请重试", "400 Bad Request");
        received += (size_t)n;
    }
    body[received] = 0; cJSON *j = cJSON_Parse(body); memset(body, 0, sizeof(body));
    if (!j) return json_reply(req, "填写内容无效", "400 Bad Request");
    const char *key = field(j, "setup_key"), *ssid = field(j, "ssid"), *password = field(j, "password");
    const char *server = field(j, "server"), *code = field(j, "code"); setup_input_t input = {0};
    bool valid = key && !strcmp(key, setup_key) && podcast_config_network(ssid, password) &&
        podcast_config_server(input.cfg.server, sizeof(input.cfg.server), server) && podcast_config_code(code);
    if (valid) {
        memcpy(input.cfg.ssid, ssid, strlen(ssid) + 1); memcpy(input.cfg.password, password, strlen(password) + 1);
        memcpy(input.code, code, 7); memcpy(input.cfg.device_id, device_id, sizeof(device_id));
    }
    cJSON_Delete(j);
    if (!valid) return json_reply(req, "请检查网络密码、后台地址和六位配对码", "400 Bad Request");
    bool expected = false;
    if (!atomic_compare_exchange_strong(&busy, &expected, true)) return json_reply(req, "正在连接，请稍候", "409 Conflict");
    if (xQueueSend(submissions, &input, 0) != pdTRUE) { atomic_store(&busy, false); memset(&input, 0, sizeof(input)); return json_reply(req, "稍后再试", "503 Service Unavailable"); }
    memset(&input, 0, sizeof(input)); return json_reply(req, "正在验证网络和后台连接", "202 Accepted");
}
static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) atomic_store(&connected, false);
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) atomic_store(&connected, true);
}
static bool start_network(void)
{
    if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK) return false;
    sta_netif = esp_netif_create_default_wifi_sta(); ap_netif = esp_netif_create_default_wifi_ap();
    if (!sta_netif || !ap_netif) return false;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) return false;
    wifi_initialized = true;
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL, &wifi_handler) != ESP_OK ||
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, NULL, &ip_handler) != ESP_OK) return false;
    wifi_config_t ap = {0}; memcpy(ap.ap.ssid, hotspot, strlen(hotspot)); ap.ap.ssid_len = strlen(hotspot);
    memcpy(ap.ap.password, hotspot_password, sizeof(hotspot_password)); ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.channel = 1; ap.ap.max_connection = 1; ap.ap.pmf_cfg.required = false;
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK || esp_wifi_start() != ESP_OK) return false;
    wifi_started = true; return true;
}
static bool start_web(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG(); cfg.max_open_sockets = 3; cfg.backlog_conn = 2;
    cfg.lru_purge_enable = true; cfg.stack_size = 6144; cfg.recv_wait_timeout = 3; cfg.send_wait_timeout = 3;
    if (httpd_start(&web, &cfg) != ESP_OK) return false;
    const httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = index_get};
    const httpd_uri_t setup = {.uri = "/api/setup", .method = HTTP_POST, .handler = setup_post};
    const httpd_uri_t state = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get};
    const httpd_uri_t networks = {.uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_get};
    return httpd_register_uri_handler(web, &index) == ESP_OK && httpd_register_uri_handler(web, &setup) == ESP_OK &&
        httpd_register_uri_handler(web, &state) == ESP_OK && httpd_register_uri_handler(web, &networks) == ESP_OK;
}
static bool trial_wifi(const setup_input_t *input)
{
    (void)esp_wifi_disconnect(); atomic_store(&connected, false);
    wifi_config_t trial = {0}; memcpy(trial.sta.ssid, input->cfg.ssid, strlen(input->cfg.ssid));
    memcpy(trial.sta.password, input->cfg.password, strlen(input->cfg.password));
    if (esp_wifi_set_config(WIFI_IF_STA, &trial) != ESP_OK || esp_wifi_connect() != ESP_OK) return false;
    int64_t deadline = esp_timer_get_time() + 25000000;
    while (!atomic_load(&connected) && !atomic_load(&cancelled) && esp_timer_get_time() < deadline) vTaskDelay(pdMS_TO_TICKS(100));
    return atomic_load(&connected) && !atomic_load(&cancelled);
}
static const char *claim(setup_input_t *input)
{
    if (!strncmp(input->cfg.server, "https://", 8)) {
        draw("加密连接需要校时，请稍候");
        esp_sntp_config_t clock = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        if (!sntp_initialized && esp_netif_sntp_init(&clock) == ESP_OK) sntp_initialized = true;
        int64_t deadline = esp_timer_get_time() + 15000000;
        while (time(NULL) < 1735689600 && !atomic_load(&cancelled) && esp_timer_get_time() < deadline)
            vTaskDelay(pdMS_TO_TICKS(100));
        if (time(NULL) < 1735689600 || atomic_load(&cancelled)) return "校时失败，请检查网络后重试";
    }
    char url[PODCAST_SERVER_MAX + 24], request[128], body[513];
    snprintf(url, sizeof(url), "%s/api/devices/claim", input->cfg.server);
    snprintf(request, sizeof(request), "{\"code\":\"%s\",\"device_id\":\"%s\"}", input->code, device_id);
    esp_http_client_config_t cfg = {.url = url, .method = HTTP_METHOD_POST, .timeout_ms = 8000,
        .buffer_size = 512, .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true};
    esp_http_client_handle_t client = esp_http_client_init(&cfg); if (!client) return "内存不足，请重试";
    const char *error = "后台连接失败，请检查地址和可信证书";
    if (esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK ||
        esp_http_client_open(client, strlen(request)) != ESP_OK ||
        esp_http_client_write(client, request, strlen(request)) != (int)strlen(request)) goto done;
    int64_t n = esp_http_client_fetch_headers(client); int code = esp_http_client_get_status_code(client);
    if (code == 401) { error = "配对码无效或已过期，请重新生成"; goto done; }
    if (code == 429) { error = "尝试太多，请稍后重新配对"; goto done; }
    if (code < 200 || code >= 300 || n <= 0 || n > 512) goto done;
    int received = 0;
    while (received < n && !atomic_load(&cancelled)) {
        int got = esp_http_client_read(client, body + received, (int)n - received); if (got <= 0) goto done;
        received += got;
    }
    if (received != n || atomic_load(&cancelled)) goto done;
    body[received] = 0; cJSON *j = cJSON_Parse(body);
    if (j) {
        const char *returned_id = field(j, "device_id"), *token = field(j, "token");
        if (returned_id && !strcmp(returned_id, device_id) && podcast_config_claim(&input->cfg, returned_id, token)) error = NULL;
        else error = "后台配对响应无效，请重试";
        cJSON_Delete(j);
    }
done:
    esp_http_client_cleanup(client); memset(request, 0, sizeof(request)); memset(body, 0, sizeof(body)); return error;
}
static bool cleanup(void)
{
    atomic_store(&active, false);
    /* A failed stop retains the queue/lock beneath live HTTP callbacks. An
     * explicit restart may terminate the process; a retry can safely stop it. */
    if (web) { if (httpd_stop(web) != ESP_OK) return false; web = NULL; }
    if (sntp_initialized) { esp_netif_sntp_deinit(); sntp_initialized = false; }
    if (wifi_handler) { esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler); wifi_handler = NULL; }
    if (ip_handler) { esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler); ip_handler = NULL; }
    if (wifi_started) { if (esp_wifi_stop() != ESP_OK) return false; wifi_started = false; }
    if (wifi_initialized) { if (esp_wifi_deinit() != ESP_OK) return false; wifi_initialized = false; }
    if (sta_netif) { esp_netif_destroy_default_wifi(sta_netif); sta_netif = NULL; }
    if (ap_netif) { esp_netif_destroy_default_wifi(ap_netif); ap_netif = NULL; }
    if (submissions) { vQueueDelete(submissions); submissions = NULL; }
    if (status_lock) { vSemaphoreDelete(status_lock); status_lock = NULL; }
    memset(setup_key, 0, sizeof(setup_key)); memset(hotspot_password, 0, sizeof(hotspot_password));
    return true;
}
bool podcast_setup_active(void) { return atomic_load(&active); }
void podcast_setup_key(bsp_btn_t button, bsp_btn_ev_t event)
{ if (atomic_load(&active) && button == BSP_BTN_OK && event == BSP_BTN_LONG) atomic_store(&cancelled, true); }
bool podcast_setup_run(void)
{
    can_cancel = podcast_config_get() != NULL; atomic_store(&active, true);
    atomic_store(&cancelled, false); atomic_store(&connected, false); atomic_store(&busy, false);
    uint64_t seed = ((uint64_t)esp_random() << 32) | esp_random();
    if (!podcast_sync_init(&identity_store, seed)) { atomic_store(&active, false); return false; }
    snprintf(device_id, sizeof(device_id), "device-%016llx", (unsigned long long)identity_store.client_seed);
    snprintf(hotspot, sizeof(hotspot), "Podcast-%.4s", device_id + 19);
    snprintf(hotspot_password, sizeof(hotspot_password), "%08lu", (unsigned long)(esp_random() % 100000000u));
    snprintf(setup_key, sizeof(setup_key), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
    submissions = xQueueCreate(1, sizeof(setup_input_t)); status_lock = xSemaphoreCreateMutex();
    if (!submissions || !status_lock || !create_screen()) { cleanup(); return false; }
    draw("填写网络、后台和配对码");
    if (!start_network() || !start_web()) { draw("设置服务启动失败，请重启"); cleanup(); return false; }
    setup_input_t input = {0};
    for (;;) {
        if (atomic_load(&cancelled)) {
            if (can_cancel) { cleanup(); memset(&input, 0, sizeof(input)); esp_restart(); return true; }
            atomic_store(&cancelled, false); draw("首次使用需要完成连接设置");
        }
        if (xQueueReceive(submissions, &input, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        draw("正在验证无线网络");
        const char *error = trial_wifi(&input) ? NULL : "网络连接失败，请检查密码和距离";
        if (!error) { draw("正在验证后台和配对码"); error = claim(&input); }
        if (!error && !atomic_load(&cancelled)) {
            if (podcast_config_save(&input.cfg)) {
                draw("连接成功，即将进入节目库"); memset(&input, 0, sizeof(input));
                vTaskDelay(pdMS_TO_TICKS(1800)); cleanup(); esp_restart(); return true;
            }
            error = "保存失败，原设置仍保留，请重试";
        }
        if (error) draw(error);
        memset(&input, 0, sizeof(input)); atomic_store(&busy, false);
    }
}
