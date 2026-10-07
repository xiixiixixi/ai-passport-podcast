#include "podcast_cover_dynamic.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "podcast_player.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef ESP_PLATFORM
#include "podcast_http.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "spi_flash_mmap.h"
#include <stdatomic.h>
#endif

/* ② 方案：封面像素零 RAM 常驻。闪存里保存服务端原始 52px PDC1 wire
 * （与下载字节逐一致，CRC/SHA/内容版本完整），设备侧用 spi_flash_mmap 把
 * 整个封面区一次性映射进数据地址空间并永久持有，LVGL 描述符直接指向
 * 映射字节（不缩放：52px 缩到 48px 会破坏 wire 自带的 CRC/SHA，并使
 * wire+16 的内容版本脱离后台 ETag）。收益：
 *  (a) 可见面板不再占用 3×5440B 堆，列表页任何播放状态（含空闲）都能显示；
 *  (b) 查找/校验/绘制全部走只读映射，不再逐槽 esp_partition_read 冻结芯片；
 *  (c) 闪存字节只被 spi_flash API 写入，IDF 写后自动失效对应 cache 行
 *      （flash_mmap.c: spi_flash_check_and_flush_cache），写后经映射读即新值。
 * 回归背景：v2 起封面常驻 RAM 16.3KB，把起播堆预算挤到必败；v6 一度
 * 空闲全禁封面换取起播——本方案两者兼得。 */

typedef struct {
    char id[24];
    const uint8_t *wire; /* Immutable storage owned elsewhere: mapping or test fixture. */
    lv_image_dsc_t image;
    uint32_t stamp;
    int64_t checked_us;
} cover_entry_t;
static cover_entry_t entries[PODCAST_COVER_SLOTS];
static char wanted[PODCAST_COVER_SLOTS][24];
static int64_t retry_at[PODCAST_COVER_SLOTS];
static unsigned failures[PODCAST_COVER_SLOTS];
static uint32_t generation,cover_revision;
static int64_t audio_hold_until;
#ifdef ESP_PLATFORM
static SemaphoreHandle_t cover_lock,worker_done;
static TaskHandle_t cover_worker;
static atomic_bool cover_quit;
static bool take_lock(void){return cover_lock&&xSemaphoreTake(cover_lock,0)==pdTRUE;}
static void give_lock(void){xSemaphoreGive(cover_lock);}
static int64_t now_us(void){return esp_timer_get_time();}
#else
/* Preview/tests have the same registry/adoption code but no network worker. */
static bool take_lock(void){return true;}
static void give_lock(void){}
static int64_t now_us(void){return 0;}
#endif
static int wanted_index(const char *id)
{
    for(unsigned i=0;i<PODCAST_COVER_SLOTS;i++)if(wanted[i][0]&&!strcmp(wanted[i],id))return (int)i;
    return -1;
}
bool podcast_dynamic_cover_activity_allowed(podcast_player_state_t state, uint32_t buffered_ms)
{
    /* 封面像素零 RAM 常驻后不再与起播竞争堆，空闲态允许封面活动（列表页
     * 显示封面是本轮回归目标）。唯一仍需完全避让的窗口是 BUFFERING：
     * 起播/跳转的 HTTP 流与瞬时堆预算不允许任何并发低优先级传输插队。 */
    (void)buffered_ms;
    return state!=PODCAST_PLAYER_BUFFERING;
}
static int entry_index(const char *id)
{
    if(!id)return -1;
    for(unsigned i=0;i<PODCAST_COVER_SLOTS;i++)if(entries[i].wire&&!strcmp(entries[i].id,id))return (int)i;
    return -1;
}
/* Bind a validated, externally owned wire into the visible registry. Pixels
 * are never copied and never freed: the caller guarantees the storage outlives
 * the entry (flash mapping on device, test fixture on host). Same content
 * version refreshes only the recheck timestamp so the UI does not flash. */
static bool adopt(const char *id,const uint8_t *wire)
{
    if(!podcast_cover_id_valid(id)||!podcast_cover_wire_valid(wire,PODCAST_COVER_WIRE_BYTES))return false;
    int at=entry_index(id);
    if(at<0)for(unsigned i=0;i<PODCAST_COVER_SLOTS;i++)if(!entries[i].wire){at=(int)i;break;}
    if(at<0||wanted_index(id)<0)return false;
    if(entries[at].wire&&!memcmp(entries[at].wire+16,wire+16,16)){
        /* wire+16 = 内容版本（sha256 前 16 字节）＝后台 ETag。同版本只刷新复查时间。 */
        entries[at].checked_us=now_us();
        return true;
    }
    cover_entry_t fresh={0};
    strcpy(fresh.id,id);
    fresh.wire=wire;
    fresh.image=(lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RGB565,
        .w=PODCAST_COVER_SIDE,.h=PODCAST_COVER_SIDE,.stride=PODCAST_COVER_SIDE*2},
        .data_size=PODCAST_COVER_PIXEL_BYTES,.data=wire+PODCAST_COVER_HEADER_BYTES};
    fresh.stamp=++cover_revision;
    fresh.checked_us=now_us();
    entries[at]=fresh;
    return true;
}
void podcast_dynamic_covers_want(const char *const *ids,size_t count)
{
    char next[PODCAST_COVER_SLOTS][24]={{0}};unsigned kept=0;
    for(size_t i=0;ids&&i<count&&kept<PODCAST_COVER_SLOTS;i++){
        if(!podcast_cover_id_valid(ids[i]))continue;
        bool duplicate=false;for(unsigned j=0;j<kept;j++)if(!strcmp(next[j],ids[i]))duplicate=true;
        if(!duplicate)strcpy(next[kept++],ids[i]);
    }
    if(!take_lock())return;
    if(memcmp(next,wanted,sizeof(wanted))){
        memcpy(wanted,next,sizeof(wanted));memset(retry_at,0,sizeof(retry_at));memset(failures,0,sizeof(failures));generation++;
    }
    give_lock();
}
bool podcast_dynamic_covers_ui_poll(void)
{
    /* All lv_image_cache calls stay on the UI thread. The demo loop runs this
     * before every render, so a slot rebound by the worker always has its
     * decoded cache purged before the first draw that could show it. */
    static uint32_t dropped_stamp[PODCAST_COVER_SLOTS];
    static uint32_t seen_revision; /* Zero until the first poll: revision 0 = nothing adopted yet. */
    if(!take_lock())return false;
    uint32_t baseline=seen_revision;
    for(unsigned i=0;i<PODCAST_COVER_SLOTS;i++){
        if(!entries[i].wire)continue;
        if(wanted_index(entries[i].id)<0){
            /* Off-page artwork immediately yields its registry slot. */
            lv_image_cache_drop(&entries[i].image);
            memset(&entries[i],0,sizeof(entries[i]));dropped_stamp[i]=0;
            cover_revision++;
        }else if(entries[i].stamp!=dropped_stamp[i]){
            /* Re-bound slot: purge any image decoded for this descriptor address
             * so the next draw reads the new flash bytes. */
            lv_image_cache_drop(&entries[i].image);
            dropped_stamp[i]=entries[i].stamp;
        }
    }
    /* Adoptions/re-binds happen in the worker between polls (they no longer
     * pass through this function), so "changed" compares the revision the UI
     * last consumed against the final one of this poll. */
    bool changed=cover_revision!=baseline;
    seen_revision=cover_revision;
    give_lock();return changed;
}
const lv_image_dsc_t *podcast_dynamic_cover_get(const char *id)
{
    int at=entry_index(id);return at<0?NULL:&entries[at].image;
}
#ifndef ESP_PLATFORM
bool podcast_dynamic_covers_host_adopt(const char *id,const uint8_t *wire,size_t size)
{
    return podcast_cover_wire_valid(wire,size)&&adopt(id,wire);
}
#endif
uint32_t podcast_dynamic_cover_stamp(const char *id)
{
    int at=entry_index(id);return at<0?0:entries[at].stamp;
}
uint32_t podcast_dynamic_covers_revision(void){return cover_revision;}
void podcast_dynamic_covers_clear_ui(void)
{
    if(!take_lock())return;
    generation++;memset(wanted,0,sizeof(wanted));memset(retry_at,0,sizeof(retry_at));
    for(unsigned i=0;i<PODCAST_COVER_SLOTS;i++)if(entries[i].wire){
        lv_image_cache_drop(&entries[i].image);memset(&entries[i],0,sizeof(entries[i]));
    }
    cover_revision++;give_lock();
}
#ifdef ESP_PLATFORM
/* 封面持久缓存：分区表里 store(0x660000+0x4000) 与 netcfg(0x6BC000) 之间
 * 有一块 352KB 未分配空区（0x664000–0x6BC000），见 partitions.csv 注释。
 * 必须用这块空区而不是 factory 尾部（0x610000–0x660000，v2 以来的老地址）：
 * CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS=y 时 IDF 把"整个正在运行的 app
 * 分区"视为保护区（partition_target.c: esp_partition_main_flash_region_safe
 * 按 p->address+p->size 判断，与镜像实际长度无关），向 factory 内部写入会
 * 直接 abort() 重启——这也是 v2-v5 的闪存封面缓存从未生效的根因之一。
 * 空区不属于任何分区：写入检查只保护分区表区和运行中的 app 分区，放行。
 * 区基址必须 64KB 对齐：esp_mmu_map 硬性要求 paddr 是 CONFIG_MMU_PAGE_SIZE
 * (0x10000) 的整数倍（esp_mmu_map.c "paddr must be rounded up..."），而
 * spi_flash_mmap 不做圆整直接透传，0x664000 这类只按 4KB 对齐的地址会被
 * ESP_ERR_INVALID_ARG 拒绝。故取空区内部的对齐区间 0x670000–0x6B0000
 * （256KB＝4 个 MMU 页，32 槽），映射不会外溢到 netcfg 的页。
 * 每槽 8KB：16B 头{magic,hash,seq,pad}+5440B wire（格式与历史版本兼容，
 * 但老地址的存量数据已不可达，等效于冷启动）。整区一次性 mmap 并永久持有：
 * 查找/校验/绘制全部走只读映射（不冻结芯片、零堆）；写入走裸 spi_flash API
 * （同样会自动失效被写页的 cache 行）。 */
#define COVER_FLASH_BASE 0x670000u
#define COVER_FLASH_LIMIT 0x6b0000u
_Static_assert(COVER_FLASH_BASE%0x10000u==0&&COVER_FLASH_LIMIT%0x10000u==0,
               "cover cache must sit on 64KB MMU pages (esp_mmu_map paddr rule)");
#define COVER_FLASH_SLOT 0x2000u
#define COVER_FLASH_COUNT ((COVER_FLASH_LIMIT-COVER_FLASH_BASE)/COVER_FLASH_SLOT)
#define COVER_FLASH_MAGIC 0x31464350u /* "PCF1" */
typedef struct {uint32_t magic,hash,seq,pad;} cover_flash_head_t;
static const uint8_t *cover_map;
static spi_flash_mmap_handle_t cover_map_handle;
static uint32_t cover_flash_seq=1;
static bool cover_region_map(void)
{
    if(cover_map)return true;
    return spi_flash_mmap(COVER_FLASH_BASE,COVER_FLASH_LIMIT-COVER_FLASH_BASE,
        SPI_FLASH_MMAP_DATA,(const void**)&cover_map,&cover_map_handle)==ESP_OK;
}
static uint32_t cover_flash_hash(const char *id)
{
    uint32_t h=2166136261u;
    for(const unsigned char *p=(const unsigned char *)id;*p;p++){h^=*p;h*=16777619u;}
    return h?h:1;
}
static const uint8_t *cover_slot_wire(uint32_t slot)
{
    return cover_map+slot*COVER_FLASH_SLOT+sizeof(cover_flash_head_t);
}
static bool cover_slot_head(uint32_t slot,cover_flash_head_t *head)
{
    memcpy(head,cover_map+slot*COVER_FLASH_SLOT,sizeof(*head));
    return head->magic==COVER_FLASH_MAGIC;
}
/* Locate a stored, still-valid wire for id. Worst case hashes 32 slots through
 * the mapping (~3ms at worker priority 2); no heap, no flash freeze. */
static int cover_flash_find(const char *id)
{
    if(!cover_region_map())return -1;
    uint32_t hash=cover_flash_hash(id);cover_flash_head_t head;
    for(uint32_t s=0;s<COVER_FLASH_COUNT;s++)
        if(cover_slot_head(s,&head)&&head.hash==hash&&
           podcast_cover_wire_valid(cover_slot_wire(s),PODCAST_COVER_WIRE_BYTES))return (int)s;
    return -1;
}
static bool cover_flash_store(const char *id,const uint8_t *wire)
{
    if(!cover_region_map()||!podcast_cover_wire_valid(wire,PODCAST_COVER_WIRE_BYTES))return false;
    /* Never erase a slot whose mapped bytes are still bound in the visible
     * registry: the erase would pull live pixels out from under a descriptor. */
    const uint8_t *in_use[PODCAST_COVER_SLOTS]={0};
    if(take_lock()){
        for(unsigned i=0;i<PODCAST_COVER_SLOTS;i++)in_use[i]=entries[i].wire;
        give_lock();
    }
    uint32_t hash=cover_flash_hash(id);int slot=-1,free_slot=-1,oldest_slot=-1;uint32_t oldest=0;
    cover_flash_head_t head;
    for(uint32_t s=0;s<COVER_FLASH_COUNT;s++){
        bool live=false;
        for(unsigned i=0;i<PODCAST_COVER_SLOTS;i++)if(in_use[i]&&in_use[i]==cover_slot_wire(s))live=true;
        if(!cover_slot_head(s,&head)){if(free_slot<0)free_slot=(int)s;continue;}
        if(head.hash==hash){slot=live?-1:(int)s;break;}
        if(!live&&(oldest_slot<0||head.seq<oldest)){oldest=head.seq;oldest_slot=(int)s;}
    }
    if(slot<0)slot=free_slot>=0?free_slot:oldest_slot;
    if(slot<0)return false;
    uint32_t rel=COVER_FLASH_BASE+(uint32_t)slot*COVER_FLASH_SLOT;
    cover_flash_head_t out={.magic=COVER_FLASH_MAGIC,.hash=hash,.seq=cover_flash_seq++,.pad=0};
    /* IDF v5 无 spi_flash_write/erase_region 裸符号（那是 v4 头文件），
     * 用 esp_flash_* + NULL=主片；空区在运行分区之外，
     * esp_partition_main_flash_region_safe 放行，不会被危险写守卫拦下。 */
    if(esp_flash_erase_region(NULL,rel,COVER_FLASH_SLOT)!=ESP_OK)return false;
    if(esp_flash_write(NULL,&out,rel,sizeof(out))!=ESP_OK)return false;
    return esp_flash_write(NULL,wire,rel+sizeof(out),PODCAST_COVER_WIRE_BYTES)==ESP_OK;
}
static bool download_allowed(void)
{
    if(!take_lock())return false;
    bool held=now_us()<audio_hold_until;give_lock();
    podcast_player_snapshot_t audio;
    /* BUFFERING（含起播窗口，见 worker 的 starting 分支）期间完全静默。
     * 其余状态（含空闲）封面活动不再触碰持久 RAM，放行。 */
    if(atomic_load(&cover_quit)||held||!podcast_player_snapshot(&audio)||
       !podcast_dynamic_cover_activity_allowed(audio.state,audio.buffered_ms))return false;
    /* 下载完成后要擦写闪存（芯片缓存短暂冻结，worst-case 擦除时间可超过
     * 240ms 的 DMA 余量）。读取/收养在 PLAYING 仍然允许（纯 mmap，不冻结），
     * 但"发起下载"额外避开 PLAYING，把擦写挪到空闲/暂停：任何潜在的音频
     * 毛刺都比晚几分钟拿到新封面严重。 */
    if(audio.state==PODCAST_PLAYER_PLAYING)return false;
    return true;
}
static bool memory_available(bool secure)
{
    /* Only the transient download budget (HTTP client + 5440B wire staging)
     * is at stake now — the PCM queue is boot-time static storage. */
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)>=
               PODCAST_COVER_WIRE_BYTES+(secure?32768U:8192U)&&
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)>=(secure?32768U:8192U);
}
static bool still_wanted(const char *id,uint32_t epoch)
{
    if(!download_allowed()||!take_lock())return false;
    bool yes=generation==epoch&&wanted_index(id)>=0;give_lock();return yes;
}
static int download(const char *id,uint32_t epoch,const uint8_t *version)
{
    const podcast_config_t *connection=podcast_config_get();if(!connection||!download_allowed())return 0;
    bool secure=!strncmp(connection->server,"https://",8);
    if(!memory_available(secure))return 0;
    uint8_t *wire=malloc(PODCAST_COVER_WIRE_BYTES);if(!wire)return 0;
    char url[256];int n=snprintf(url,sizeof(url),"%s/art/%s/device.bin",connection->server,id);
    esp_http_client_handle_t client=NULL;int result=0;
    if(n<0||(size_t)n>=sizeof(url))goto done;
    esp_http_client_config_t cfg={.url=url,.method=HTTP_METHOD_GET,.timeout_ms=secure?600:350,.buffer_size=512};
    client=podcast_http_client(cfg);if(!client)goto done;
    /* 封面接口要求设备身份；带上配对时取得的令牌，否则后台返回 401。 */
    {
        const char *auth=podcast_config_authorization();
        if(auth&&esp_http_client_set_header(client,"Authorization",auth)!=ESP_OK)goto done;
    }
    if(version){
        static const char hex[]="0123456789abcdef";char etag[35];etag[0]='"';
        for(unsigned i=0;i<16;i++){etag[1+i*2]=hex[version[i]>>4];etag[2+i*2]=hex[version[i]&15];}
        etag[33]='"';etag[34]=0;(void)esp_http_client_set_header(client,"If-None-Match",etag);
    }
    if(!still_wanted(id,epoch)||esp_http_client_open(client,0)!=ESP_OK)goto done;
    int64_t length=esp_http_client_fetch_headers(client);int status=esp_http_client_get_status_code(client);
    if(status==304&&version){result=2;goto done;}
    if(status!=200||length!=PODCAST_COVER_WIRE_BYTES)goto done;
    size_t received=0;int64_t deadline=now_us()+1200000;
    while(received<PODCAST_COVER_WIRE_BYTES&&now_us()<deadline&&still_wanted(id,epoch)){
        size_t remain=PODCAST_COVER_WIRE_BYTES-received;int take=(int)(remain>512?512:remain);
        int got=esp_http_client_read(client,(char *)wire+received,take);if(got<=0)break;received+=(size_t)got;
        /* Give the priority-4 audio producer/player every opportunity to fill
         * its queue; artwork is optional and always loses to buffering. */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if(received==PODCAST_COVER_WIRE_BYTES&&cover_flash_store(id,wire))result=1;
 done:
    if(client)esp_http_client_cleanup(client);
    free(wire);return result;
}
static void artwork_worker(void *unused)
{
    (void)unused;int64_t next_request=0;
    while(!atomic_load(&cover_quit)){
        vTaskDelay(pdMS_TO_TICKS(250));int64_t now=now_us();
        /* 起播窗口(计划打开音源→出声)与整个 BUFFERING 期完全静默；其余
         * 状态（含空闲列表页）允许封面活动：像素走只读映射，零堆常驻。 */
        podcast_player_snapshot_t audio;
        bool have_audio=podcast_player_snapshot(&audio);
        bool starting=podcast_player_start_pending()||(have_audio&&audio.state==PODCAST_PLAYER_BUFFERING);
        if(now<next_request||starting||
           !podcast_dynamic_cover_activity_allowed(have_audio?audio.state:PODCAST_PLAYER_IDLE,
                                                   have_audio?audio.buffered_ms:0)||
           !take_lock())continue;
        char id[24]={0};uint32_t epoch=generation;
        for(unsigned i=0;i<PODCAST_COVER_SLOTS;i++){
            int at=entry_index(wanted[i]);
            if(wanted[i][0]&&now>=retry_at[i]&&(at<0||now-entries[at].checked_us>=INT64_C(600000000))){
                strcpy(id,wanted[i]);break;
            }
        }
        bool in_registry=false;
        if(id[0]){in_registry=entry_index(id)>=0;}
        give_lock();
        if(!id[0])continue;
        int slot=cover_flash_find(id);
        if(slot>=0&&!in_registry){
            /* Registry miss, flash hit: bind the mapped bytes, no network.
               2 秒节流防止连续绑定干扰音频。 */
            if(take_lock()){(void)adopt(id,cover_slot_wire((uint32_t)slot));give_lock();}
            next_request=now_us()+2000000;continue;
        }
        uint8_t version[16];const uint8_t *have=NULL;
        if(slot>=0)have=cover_slot_wire((uint32_t)slot);
        else if(take_lock()){
            int at=entry_index(id);
            if(at>=0)have=entries[at].wire;
            give_lock();
        }
        if(have)memcpy(version,have+PODCAST_COVER_HEADER_BYTES-16,16);
        int outcome=download(id,epoch,have?version:NULL);next_request=now_us()+2000000;
        if(outcome==1){
            /* Adopt straight from the mapping: the bytes the UI will draw are
             * exactly the flash bytes just written and cache-invalidated. */
            int stored=cover_flash_find(id);
            if(stored>=0&&take_lock()){(void)adopt(id,cover_slot_wire((uint32_t)stored));give_lock();}
        }else if(outcome==2&&have&&take_lock()){
            /* 304：版本未变，仅推进复查时间，避免 600s 周期反复探测。 */
            (void)adopt(id,have);give_lock();
        }
        if(!take_lock())continue;
        int index=wanted_index(id);
        if(index>=0&&generation==epoch){
            if(outcome){failures[index]=0;retry_at[index]=now_us()+600000000;}
            else{if(failures[index]<4)failures[index]++;retry_at[index]=now_us()+(INT64_C(10000000)<<failures[index]);}
        }
        give_lock();
    }
    xSemaphoreGive(worker_done);vTaskSuspend(NULL);
}
bool podcast_dynamic_covers_start(void)
{
    if(cover_worker)return true;
    if(!cover_lock)cover_lock=xSemaphoreCreateMutex();
    if(!worker_done)worker_done=xSemaphoreCreateBinary();
    if(!cover_lock||!worker_done)return false;
    atomic_store(&cover_quit,false);
    return xTaskCreate(artwork_worker,"podcast_art",4096,NULL,2,&cover_worker)==pdPASS;
}
bool podcast_dynamic_covers_stop(void)
{
    atomic_store(&cover_quit,true);
    if(cover_worker){if(xSemaphoreTake(worker_done,pdMS_TO_TICKS(2000))!=pdTRUE)return false;vTaskDelete(cover_worker);cover_worker=NULL;}
    return true;
}
void podcast_dynamic_covers_prepare_audio(void)
{
    /* 封面零 RAM 常驻：无可回收内存、无需 UI 让路。只需短时 hold，令在途
     * 下载（still_wanted 每 512B 检查一次）毫秒级中止、把 HTTP/暂存堆还给
     * 起播。可见封面在起播期间继续显示。 */
    if(take_lock()){audio_hold_until=now_us()+2000000;give_lock();}
}
#else
bool podcast_dynamic_covers_start(void){return true;}
bool podcast_dynamic_covers_stop(void){return true;}
void podcast_dynamic_covers_prepare_audio(void){audio_hold_until=0;}
#endif
