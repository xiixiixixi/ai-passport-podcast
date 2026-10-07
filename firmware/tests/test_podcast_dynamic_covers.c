#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/podcast_cover_wire.c"
#include "../main/podcast_cover_dynamic.c"
static unsigned dropped;
void lv_image_cache_drop(const void *image){assert(image);dropped++;}
static uint8_t *fixture(unsigned seed)
{
    uint8_t *p=calloc(1,PODCAST_COVER_WIRE_BYTES);assert(p);memcpy(p,"PDC1",4);p[4]=p[6]=52;
    p[8]=PODCAST_COVER_PIXEL_BYTES&255;p[9]=(PODCAST_COVER_PIXEL_BYTES>>8)&255;
    for(unsigned i=0;i<PODCAST_COVER_PIXEL_BYTES;i++)p[32+i]=(uint8_t)(i*13+seed);
    uint32_t crc=crc32(p+32,PODCAST_COVER_PIXEL_BYTES);
    for(unsigned i=0;i<4;i++)p[12+i]=(uint8_t)(crc>>(i*8));
    uint8_t hash[32];sha256(p+32,PODCAST_COVER_PIXEL_BYTES,hash);memcpy(p+16,hash,16);return p;
}
static void independently_verified_protocol(void)
{
    /* hashlib.sha256/zlib.crc32 independently computed for bytes i*13%256. */
    static const uint8_t expected[32]={
        0x50,0xe2,0x91,0xcf,0xcf,0xf7,0x00,0x8e,0x97,0x37,0x54,0xb7,0x3d,0xa1,0xa9,0x85,
        0xe0,0x7c,0x8c,0xfc,0xb6,0x64,0xaf,0x85,0x96,0x1a,0x0a,0xb6,0x7d,0x2f,0x35,0xed};
    uint8_t *p=fixture(0);uint8_t hash[32];sha256(p+32,PODCAST_COVER_PIXEL_BYTES,hash);
    assert(!memcmp(hash,expected,sizeof(hash)));
    assert(crc32(p+32,PODCAST_COVER_PIXEL_BYTES)==UINT32_C(0xa6ca7e4b));
    assert(podcast_cover_wire_valid(p,PODCAST_COVER_WIRE_BYTES));
    assert(!podcast_cover_wire_valid(p,PODCAST_COVER_WIRE_BYTES-1)&&!podcast_cover_wire_valid(p,PODCAST_COVER_WIRE_BYTES+1));
    unsigned offsets[]={0,4,5,6,7,8,9,12,16,32,5439};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++){p[offsets[i]]^=1;assert(!podcast_cover_wire_valid(p,PODCAST_COVER_WIRE_BYTES));p[offsets[i]]^=1;}
    assert(podcast_cover_id_valid("src_1234-LateTalk")&&!podcast_cover_id_valid("https://other/art"));
    assert(!podcast_cover_id_valid("../private")&&!podcast_cover_id_valid("")&&!podcast_cover_id_valid("abcdefghijklmnopqrstuvwx"));
    free(p);
}
/* ② 契约：注册表描述符直接指向外部不可变字节（设备=闪存映射，测试=夹具），
 * 52px 原样呈现，零拷贝零缩放——像素不再占任何堆。 */
static void identity_pixels(const lv_image_dsc_t *image,const uint8_t *wire)
{
    assert(image&&image->header.w==52&&image->header.h==52&&image->header.stride==104);
    assert(image->data_size==PODCAST_COVER_PIXEL_BYTES&&image->data==wire+32);
    assert(((uintptr_t)image->data&3)==0);
    assert(!memcmp(image->data,wire+32,PODCAST_COVER_PIXEL_BYTES));
}
static void registry_adoption_and_eviction(void)
{
    assert(podcast_dynamic_covers_start());const char *ids[]={"new-show","another","third"};
    podcast_dynamic_covers_want(ids,3);
    uint8_t *a0=fixture(0),*a17=fixture(17),*b=fixture(1),*c=fixture(2);
    /* 收养即时生效（设备上由 worker 在锁内调用，同语义）。 */
    assert(adopt(ids[0],a0));
    identity_pixels(podcast_dynamic_cover_get(ids[0]),a0);
    assert(podcast_dynamic_covers_ui_poll());
    uint32_t stamp=podcast_dynamic_cover_stamp(ids[0]);assert(stamp);
    /* 同内容版本再收养：不闪屏（stamp/revision 均不动）。 */
    assert(adopt(ids[0],a0));
    assert(!podcast_dynamic_covers_ui_poll()&&podcast_dynamic_cover_stamp(ids[0])==stamp);
    /* 新版本：重绑槽位、换数据指针、要求重绘。 */
    assert(adopt(ids[0],a17));
    assert(podcast_dynamic_covers_ui_poll()&&podcast_dynamic_cover_stamp(ids[0])!=stamp);
    identity_pixels(podcast_dynamic_cover_get(ids[0]),a17);
    /* 坏 wire 拒收，已显示的有效版本保留。 */
    uint8_t *bad=fixture(0);bad[16]^=1;
    assert(!adopt(ids[0],bad));free(bad);
    identity_pixels(podcast_dynamic_cover_get(ids[0]),a17);
    /* 非当前想要的 id 不进注册表。 */
    uint8_t *x=fixture(3);assert(!adopt("fourth",x));free(x);
    assert(adopt(ids[1],b)&&adopt(ids[2],c)&&podcast_dynamic_covers_ui_poll());
    unsigned used=0;for(unsigned i=0;i<PODCAST_COVER_SLOTS;i++)used+=entries[i].wire!=NULL;
    assert(used==3);
    /* 起播不再清空可见封面：像素零 RAM 常驻，prepare_audio 只置让路窗口。 */
    podcast_dynamic_covers_prepare_audio();
    assert(podcast_dynamic_cover_get(ids[0])&&podcast_dynamic_cover_get(ids[2]));
    /* 翻页：离页槽位立即让位，下一次轮询无新变化。 */
    const char *only[]={"new-show"};podcast_dynamic_covers_want(only,1);
    assert(podcast_dynamic_covers_ui_poll());
    assert(!podcast_dynamic_cover_get("another")&&!podcast_dynamic_cover_get("third"));
    assert(!podcast_dynamic_covers_ui_poll());
    identity_pixels(podcast_dynamic_cover_get(ids[0]),a17);
    podcast_dynamic_covers_clear_ui();
    assert(!podcast_dynamic_cover_get(ids[0]));
    assert(podcast_dynamic_covers_stop());
    assert(dropped==7); /* 重绑×3 + 新槽×2 + 离页×2，clear_ui 前共 7 次。 */
    free(a0);free(a17);free(b);free(c);
}
int main(int argc,char **argv)
{
    if(argc==2){
        FILE *file=fopen(argv[1],"rb");assert(file);uint8_t bytes[PODCAST_COVER_WIRE_BYTES+1];
        size_t size=fread(bytes,1,sizeof(bytes),file);assert(feof(file));fclose(file);
        assert(podcast_cover_wire_valid(bytes,size));puts("Backend-produced PDC1 bytes validated by production device CRC/SHA/size parser PASS");return 0;
    }
    independently_verified_protocol();registry_adoption_and_eviction();
    /* ② 回归底线：像素零 RAM 常驻后，空闲/错误/完成/播放/暂停态都允许封面
     * 活动（列表页空闲也要显示封面——本轮回归目标）；唯一禁区是 BUFFERING：
     * 起播/跳转窗口的流与瞬时堆预算不允许低优先级传输插队。 */
    assert(podcast_dynamic_cover_activity_allowed(PODCAST_PLAYER_IDLE,0));
    assert(podcast_dynamic_cover_activity_allowed(PODCAST_PLAYER_ERROR,0));
    assert(podcast_dynamic_cover_activity_allowed(PODCAST_PLAYER_FINISHED,0));
    assert(podcast_dynamic_cover_activity_allowed(PODCAST_PLAYER_PLAYING,0));
    assert(podcast_dynamic_cover_activity_allowed(PODCAST_PLAYER_PAUSED,0));
    assert(!podcast_dynamic_cover_activity_allowed(PODCAST_PLAYER_BUFFERING,32768));
    assert(!podcast_dynamic_cover_activity_allowed(PODCAST_PLAYER_BUFFERING,0));
    puts("Cover activity gate: every state except BUFFERING may load artwork (zero-RAM pixels) PASS");
    puts("Dynamic covers: independent CRC/SHA vector, malformed/truncated/oversized rejection, 52px zero-copy flash-mapped descriptors, version refresh without flash, off-page eviction, audio start keeps covers visible PASS");
}
