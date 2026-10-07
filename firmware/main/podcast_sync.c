#include "podcast_sync.h"
#include "nvs.h"
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#define SYNC_WIRE_SIZE (24U+PODCAST_SYNC_SLOTS*256U+4U)
static uint8_t wire[SYNC_WIRE_SIZE]; /* Worker-only scratch; no task-stack bulk allocation. */
static void put(uint8_t *p,uint64_t value,unsigned n){for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(value>>(i*8));}
static uint64_t get(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(i*8);return v;}
static uint32_t crc(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;while(n--){c^=*p++;for(unsigned j=0;j<8;j++)c=(c>>1)^(0xedb88320U&(0U-(c&1U)));}return ~c;}
static bool id(const char *s,size_t cap){size_t n=0;for(;s[n];n++){if(n+1>=cap)return false;if(!((s[n]>='a'&&s[n]<='z')||(s[n]>='A'&&s[n]<='Z')||(s[n]>='0'&&s[n]<='9')||s[n]=='-'||s[n]=='_'))return false;}return n>0;}
static bool terminal(podcast_sync_state_t s){return s==PODCAST_SYNC_STOPPED||s==PODCAST_SYNC_ENDED;}
static bool changed(const podcast_sync_record_t *r){return r->used&&(!r->session_id[0]||r->pending||r->state!=r->acked_state||r->position_ms!=r->acked_position_ms||r->heard_ms!=r->acked_heard_ms||r->seek);}
static bool save(const podcast_sync_t *s)
{
    memset(wire,0,sizeof(wire));memcpy(wire,"PSY2",4);put(wire+4,2,2);put(wire+8,s->client_seed,8);put(wire+16,s->counter,8);
    size_t used=24;unsigned mask=0;
    for(unsigned i=0;i<PODCAST_SYNC_SLOTS;i++){
        const podcast_sync_record_t *r=&s->records[i];if(!r->used)continue;mask|=1U<<i;uint8_t *p=wire+used;used+=256;
        p[0]=r->used;p[1]=r->offline;p[2]=r->restart;p[3]=r->stale;p[4]=r->pending;p[5]=r->seek;p[6]=r->pending_seek;
        p[7]=(uint8_t)r->state;p[8]=(uint8_t)r->pending_state;p[9]=(uint8_t)r->acked_state;
        put(p+16,r->request_id,8);put(p+24,r->base_revision,8);put(p+32,r->created_position_ms,8);
        put(p+40,r->position_ms,8);put(p+48,r->heard_ms,8);put(p+56,r->acked_position_ms,8);put(p+64,r->acked_heard_ms,8);
        put(p+72,r->pending_position_ms,8);put(p+80,r->pending_heard_ms,8);put(p+88,r->next_seq,4);put(p+92,r->pending_seq,4);
        memcpy(p+96,r->session_id,33);memcpy(p+129,r->show_id,24);memcpy(p+153,r->episode_id,64);put(p+217,r->elapsed_ms,8);put(p+225,r->pending_elapsed_ms,8);put(p+233,r->heard_origin_ms,8);
    }
    put(wire+6,mask,2);put(wire+used,crc(wire,used),4);used+=4;
    nvs_handle_t h; if(nvs_open("podcast_sync",NVS_READWRITE,&h)!=ESP_OK)return false;
    esp_err_t e=nvs_set_blob(h,"outbox",wire,used);if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);return e==ESP_OK;
}
bool podcast_sync_init(podcast_sync_t *s,uint64_t random_seed)
{
    if(!s)return false;
    memset(s,0,sizeof(*s));
    nvs_handle_t h;
    esp_err_t e=nvs_open("podcast_sync",NVS_READONLY,&h);size_t n=sizeof(wire);
    if(e==ESP_OK){e=nvs_get_blob(h,"outbox",wire,&n);nvs_close(h);}
    if(e==ESP_ERR_NVS_NOT_FOUND){s->client_seed=random_seed?random_seed:1;s->ready=save(s);return s->ready;}
    if(e!=ESP_OK||n<28||n>sizeof(wire)||memcmp(wire,"PSY2",4)||get(wire+4,2)!=2||get(wire+6,2)>255||get(wire+n-4,4)!=crc(wire,n-4))return false;
    unsigned mask=(unsigned)get(wire+6,2),count=0;for(unsigned i=0;i<PODCAST_SYNC_SLOTS;i++)count+=(mask>>i)&1U;
    if(n!=28+count*256)return false;
    size_t offset=24;
    s->client_seed=get(wire+8,8);s->counter=get(wire+16,8);if(!s->client_seed)return false;
    for(unsigned i=0;i<PODCAST_SYNC_SLOTS;i++){
        podcast_sync_record_t *r=&s->records[i];if(!(mask&(1U<<i)))continue;const uint8_t *p=wire+offset;offset+=256;
        for(unsigned b=0;b<7;b++)if(p[b]>1)return false;
        r->used=p[0];r->offline=p[1];r->restart=p[2];r->stale=p[3];r->pending=p[4];r->seek=p[5];r->pending_seek=p[6];
        r->state=(podcast_sync_state_t)p[7];r->pending_state=(podcast_sync_state_t)p[8];r->acked_state=(podcast_sync_state_t)p[9];
        r->request_id=get(p+16,8);r->base_revision=get(p+24,8);r->created_position_ms=get(p+32,8);r->position_ms=get(p+40,8);r->heard_ms=get(p+48,8);r->acked_position_ms=get(p+56,8);r->acked_heard_ms=get(p+64,8);r->pending_position_ms=get(p+72,8);r->pending_heard_ms=get(p+80,8);r->next_seq=(uint32_t)get(p+88,4);r->pending_seq=(uint32_t)get(p+92,4);
        memcpy(r->session_id,p+96,33);memcpy(r->show_id,p+129,24);memcpy(r->episode_id,p+153,64);r->elapsed_ms=get(p+217,8);r->pending_elapsed_ms=get(p+225,8);r->heard_origin_ms=get(p+233,8);
        if(r->used&&(!id(r->show_id,24)||!id(r->episode_id,64)||!r->request_id||!r->next_seq||r->state<1||r->state>4||r->acked_state>4||r->pending_state>4||(r->session_id[0]&&!id(r->session_id,33))||(r->pending&&(!r->pending_seq||!r->session_id[0]))))return false;
        /* Audio generation is process-local; never reuse it across reboot. */
        r->audio_session=0;
        if(r->used&&!terminal(r->state))r->state=PODCAST_SYNC_STOPPED;
    }
    s->ready=true;return true;
}
void podcast_sync_clock(podcast_sync_t *s,uint64_t now)
{
    if(!s||!s->ready)return;
    uint64_t delta=s->clock_set&&now>=s->clock_ms?now-s->clock_ms:0;
    for(unsigned i=0;i<PODCAST_SYNC_SLOTS;i++){podcast_sync_record_t *r=&s->records[i];if(r->used&&!terminal(r->state)){r->elapsed_ms+=delta;if(r->elapsed_ms>604800000)r->elapsed_ms=604800000;}}
    s->clock_ms=now;s->clock_set=true;
}
int podcast_sync_begin(podcast_sync_t *s,const char *show,const char *episode,uint64_t pos,uint64_t revision,bool offline,bool restart)
{
    if(!s||!s->ready||!id(show,24)||!id(episode,64)||s->counter==UINT64_MAX)return -1;
    int at=-1;for(unsigned i=0;i<PODCAST_SYNC_SLOTS;i++){podcast_sync_record_t *r=&s->records[i];if(!r->used||(terminal(r->state)&&!changed(r)&&!r->audio_session)){at=(int)i;break;}}
    if(at<0)return -1;
    podcast_sync_record_t old=s->records[at];uint64_t old_count=s->counter;
    s->records[at]=(podcast_sync_record_t){.used=true,.offline=offline,.restart=restart,.request_id=++s->counter,.base_revision=revision,.created_position_ms=restart?0:pos,.position_ms=restart?0:pos,.next_seq=1,.state=PODCAST_SYNC_PAUSED};
    strcpy(s->records[at].show_id,show);strcpy(s->records[at].episode_id,episode);
    if(!save(s)){s->records[at]=old;s->counter=old_count;return -1;}return at;
}
bool podcast_sync_heard_origin(podcast_sync_t *s,int at,uint64_t heard)
{
    if(!s||at<0||at>=PODCAST_SYNC_SLOTS||!s->records[at].used)return false;
    podcast_sync_record_t *r=&s->records[at];uint64_t old=r->heard_origin_ms;r->heard_origin_ms=heard;
    if(!save(s)){r->heard_origin_ms=old;return false;}return true;
}
bool podcast_sync_creation_body(const podcast_sync_t *s,int at,char *out,size_t cap)
{
    if(!s||at<0||at>=PODCAST_SYNC_SLOTS||!s->records[at].used)return false;
    const podcast_sync_record_t *r=&s->records[at];char extra[144]="";
    if(!r->offline)snprintf(extra,sizeof(extra),",\"base_revision\":%llu",(unsigned long long)r->base_revision);
    if(r->offline)snprintf(extra,sizeof(extra),",\"position_ms\":%llu,\"base_revision\":%llu",(unsigned long long)r->created_position_ms,(unsigned long long)r->base_revision);
    int n=snprintf(out,cap,"{\"client_id\":\"device-%016llx\",\"show_id\":\"%s\",\"episode_id\":\"%s\",\"request_id\":\"%016llx-%016llx\",\"restart\":%s%s}",(unsigned long long)s->client_seed,r->show_id,r->episode_id,(unsigned long long)s->client_seed,(unsigned long long)r->request_id,r->restart?"true":"false",extra);return n>0&&(size_t)n<cap;
}
bool podcast_sync_opened(podcast_sync_t *s,int at,const char *session,uint64_t pos,uint32_t seq,bool stale)
{
    if(!s||at<0||at>=PODCAST_SYNC_SLOTS||!s->records[at].used||!id(session,33)||!seq)return false;
    podcast_sync_record_t *r=&s->records[at],old=*r;if(r->session_id[0])return !strcmp(r->session_id,session);
    strcpy(r->session_id,session);r->next_seq=seq;r->stale=stale;if(!r->offline&&!r->audio_session&&!r->heard_ms&&r->state==PODCAST_SYNC_PAUSED)r->position_ms=pos;
    if(!save(s)){*r=old;return false;}return true;
}
void podcast_sync_observe(podcast_sync_t *s,int at,uint32_t audio,uint64_t pos,uint64_t heard,podcast_sync_state_t state,bool seek)
{
    if(!s||at<0||at>=PODCAST_SYNC_SLOTS||!s->records[at].used||state<1||state>4)return;
    podcast_sync_record_t *r=&s->records[at];if(audio)r->audio_session=audio;
    if(heard>r->heard_ms)r->seek=false; /* Real audio after a seek permits a real EOF. */
    if(heard>=r->heard_ms){r->heard_ms=heard;r->position_ms=pos;r->state=state;}
    if(seek)r->seek=true;
}
bool podcast_sync_freeze(podcast_sync_t *s,int at)
{
    if(!s||at<0||at>=PODCAST_SYNC_SLOTS||!s->records[at].used)return false;
    podcast_sync_record_t *r=&s->records[at];if(r->pending)return save(s);
    if(!r->session_id[0])return save(s);
    if(!changed(r))return true;
    podcast_sync_record_t old=*r;r->pending=true;r->pending_seq=r->next_seq;r->pending_position_ms=r->position_ms;r->pending_heard_ms=r->heard_ms;r->pending_elapsed_ms=r->elapsed_ms;r->pending_state=r->state;r->pending_seek=r->seek;
    if(!save(s)){*r=old;return false;}return true;
}
static const char *state_name(podcast_sync_state_t s){return s==PODCAST_SYNC_PLAYING?"playing":s==PODCAST_SYNC_PAUSED?"paused":s==PODCAST_SYNC_ENDED?"ended":"stopped";}
bool podcast_sync_event_body(const podcast_sync_t *s,int at,char *out,size_t cap)
{
    if(!s||at<0||at>=PODCAST_SYNC_SLOTS||!s->records[at].pending)return false;
    const podcast_sync_record_t *r=&s->records[at];int n=snprintf(out,cap,"{\"session_id\":\"%s\",\"seq\":%" PRIu32 ",\"position_ms\":%llu,\"listened_ms\":%llu,\"elapsed_ms\":%llu,\"state\":\"%s\",\"seek\":%s}",r->session_id,r->pending_seq,(unsigned long long)r->pending_position_ms,(unsigned long long)r->pending_heard_ms,(unsigned long long)r->pending_elapsed_ms,state_name(r->pending_state),r->pending_seek?"true":"false");return n>0&&(size_t)n<cap;
}
bool podcast_sync_acknowledge(podcast_sync_t *s,int at,uint64_t revision)
{
    if(!s||at<0||at>=PODCAST_SYNC_SLOTS||!s->records[at].pending)return false;
    podcast_sync_record_t *r=&s->records[at],old=*r;if(r->pending_seq==UINT32_MAX)return false;
    r->acked_position_ms=r->pending_position_ms;r->acked_heard_ms=r->pending_heard_ms;r->acked_state=r->pending_state;r->next_seq=r->pending_seq+1;r->pending=false;r->base_revision=revision;
    if(r->position_ms==r->pending_position_ms&&r->heard_ms==r->pending_heard_ms)r->seek=false;
    if(terminal(r->state)&&!changed(r)&&!r->audio_session)memset(r,0,sizeof(*r));
    if(!save(s)){*r=old;return false;}return true;
}
int podcast_sync_pending(const podcast_sync_t *s)
{
    int at=-1;if(!s||!s->ready)return -1;for(unsigned i=0;i<PODCAST_SYNC_SLOTS;i++)if(changed(&s->records[i])&&(at<0||s->records[i].request_id<s->records[at].request_id))at=(int)i;return at;
}
bool podcast_sync_has_offline(const podcast_sync_t *s,const char *show,const char *episode)
{
    if(!s||!s->ready)return false;
    for(unsigned i=0;i<PODCAST_SYNC_SLOTS;i++){
        const podcast_sync_record_t *r=&s->records[i];
        if(changed(r)&&!strcmp(r->show_id,show)&&!strcmp(r->episode_id,episode))return true;
    }
    return false;
}
