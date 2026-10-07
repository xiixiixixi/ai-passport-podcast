#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/podcast_sync.c"
static uint8_t disk[SYNC_WIRE_SIZE],staging[SYNC_WIRE_SIZE];
static bool present, staged, fail_write, fail_commit;
static unsigned commits;
static size_t disk_size,staging_size;
esp_err_t nvs_open(const char *name,nvs_open_mode_t mode,nvs_handle_t *h){assert(!strcmp(name,"podcast_sync"));if(mode==NVS_READONLY&&!present)return ESP_ERR_NVS_NOT_FOUND;*h=1;return ESP_OK;}
void nvs_close(nvs_handle_t h){assert(h==1);staged=false;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *k,void *out,size_t *n){assert(h==1&&!strcmp(k,"outbox"));if(!present)return ESP_ERR_NVS_NOT_FOUND;if(*n<disk_size)return ESP_FAIL;memcpy(out,disk,disk_size);*n=disk_size;return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *k,const void *p,size_t n){assert(h==1&&!strcmp(k,"outbox")&&n>=28&&n<=sizeof(disk));if(fail_write)return ESP_FAIL;memcpy(staging,p,n);staging_size=n;staged=true;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){assert(h==1&&staged);if(fail_commit)return ESP_FAIL;memcpy(disk,staging,staging_size);disk_size=staging_size;present=true;commits++;return ESP_OK;}
int main(void)
{
    podcast_sync_t s,rebooted;char first[512],retry[512];
    assert(podcast_sync_init(&s,0x0123456789abcdefULL));podcast_sync_clock(&s,1000);
    assert(disk_size==28);
    int a=podcast_sync_begin(&s,"show1","ep1",123000,7,true,false);assert(a==0&&disk_size==284);
    assert(podcast_sync_creation_body(&s,a,first,sizeof(first))&&strstr(first,"\"base_revision\":7")&&strstr(first,"\"position_ms\":123000"));
    assert(podcast_sync_opened(&s,a,"abcdef1234567890abcdef1234567890",123000,1,false));
    podcast_sync_clock(&s,11000);podcast_sync_observe(&s,a,9,133000,10000,PODCAST_SYNC_PLAYING,false);assert(podcast_sync_freeze(&s,a));
    assert(podcast_sync_event_body(&s,a,first,sizeof(first))&&strstr(first,"\"seq\":1")&&strstr(first,"\"elapsed_ms\":10000"));
    podcast_sync_clock(&s,16000);podcast_sync_observe(&s,a,9,500000,10000,PODCAST_SYNC_PAUSED,true);assert(podcast_sync_freeze(&s,a));
    assert(podcast_sync_event_body(&s,a,retry,sizeof(retry))&&!strcmp(first,retry));
    assert(podcast_sync_init(&rebooted,999));assert(rebooted.client_seed==s.client_seed&&rebooted.records[a].audio_session==0);
    assert(podcast_sync_event_body(&rebooted,a,retry,sizeof(retry))&&!strcmp(first,retry));
    assert(rebooted.records[a].position_ms==500000&&rebooted.records[a].heard_ms==10000);
    fail_commit=true;assert(!podcast_sync_acknowledge(&s,a,8));assert(podcast_sync_event_body(&s,a,retry,sizeof(retry))&&!strcmp(first,retry));fail_commit=false;
    assert(podcast_sync_acknowledge(&s,a,8));assert(podcast_sync_freeze(&s,a));
    assert(podcast_sync_event_body(&s,a,retry,sizeof(retry))&&strstr(retry,"\"seq\":2")&&strstr(retry,"\"seek\":true")&&strstr(retry,"\"listened_ms\":10000"));
    assert(podcast_sync_acknowledge(&s,a,9));podcast_sync_clock(&s,18000);podcast_sync_observe(&s,a,9,502000,12000,PODCAST_SYNC_PLAYING,false);assert(podcast_sync_freeze(&s,a));
    assert(podcast_sync_event_body(&s,a,retry,sizeof(retry))&&strstr(retry,"\"seq\":3")&&strstr(retry,"\"seek\":false")&&strstr(retry,"\"listened_ms\":12000"));
    assert(podcast_sync_acknowledge(&s,a,10));
    fail_write=true;uint64_t count=s.counter;assert(podcast_sync_begin(&s,"show2","ep2",0,0,true,false)==-1&&s.counter==count);fail_write=false;
    for(unsigned i=1;i<PODCAST_SYNC_SLOTS;i++){char ep[16];snprintf(ep,sizeof(ep),"ep%u",i);assert(podcast_sync_begin(&s,"show2",ep,0,0,true,false)==(int)i);}
    assert(podcast_sync_begin(&s,"show3","full",0,0,true,false)==-1);
    /* Even a fully acknowledged terminal report cannot be reused while the
     * old audio generation may still deliver its final heard snapshot. */
    podcast_sync_observe(&s,a,9,502000,12000,PODCAST_SYNC_STOPPED,false);assert(podcast_sync_freeze(&s,a));assert(podcast_sync_acknowledge(&s,a,11));
    assert(podcast_sync_begin(&s,"show3","too_early",0,11,false,true)==-1);
    s.records[a].audio_session=0; /* Final audio-worker tail has now been observed. */
    int reused=podcast_sync_begin(&s,"show3","available",0,11,false,true);assert(reused==a&&s.records[a].request_id>count&&s.records[a].restart);
    fail_commit=true;assert(!podcast_sync_heard_origin(&s,a,12000)&&s.records[a].heard_origin_ms==0);fail_commit=false;
    assert(podcast_sync_heard_origin(&s,a,12000));
    assert(podcast_sync_init(&rebooted,888));assert(rebooted.counter==s.counter&&rebooted.records[a].state==PODCAST_SYNC_STOPPED&&rebooted.records[a].heard_origin_ms==12000);
    disk[30]^=1;assert(!podcast_sync_init(&rebooted,777));assert(present); /* Corruption never erases the saved ledger. */
    printf("Sync outbox: immutable retry/reboot body, monotonic seq, seek excludes heard time, 8-session bound, NVS write/commit rollback, stable client and CRC corruption PASS (%u commits)\n",commits);
    return 0;
}
