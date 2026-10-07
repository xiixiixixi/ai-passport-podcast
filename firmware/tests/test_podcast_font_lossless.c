#include "podcast_font_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(app_cjk_18);
LV_FONT_DECLARE(app_cjk_18_original);

static uint32_t hash_byte(uint32_t value,uint8_t byte){return (value^byte)*UINT32_C(16777619);}
int main(void)
{
    lv_init();
    const lv_font_fmt_txt_dsc_t *before=app_cjk_18_original.dsc,*after=app_cjk_18.dsc;
    const podcast_font_codec_t *codec=app_cjk_18.user_data;assert(codec&&codec->glyph_count==27782);
    assert(before->bpp==4&&after->bpp==4&&before->cmap_num==after->cmap_num);
    assert(app_cjk_18.line_height==app_cjk_18_original.line_height&&app_cjk_18.base_line==app_cjk_18_original.base_line);
    lv_draw_buf_t *a8=lv_draw_buf_create(32,32,LV_COLOR_FORMAT_A8,LV_STRIDE_AUTO);assert(a8);
    unsigned glyphs=0,nonempty=0,max_raw=0;uint64_t pixels=0,bytes=0;
    uint32_t raw_hash=2166136261u,pixel_hash=2166136261u;clock_t start=clock();
    for(uint32_t gid=0;gid<codec->glyph_count;gid++){
        const lv_font_fmt_txt_glyph_dsc_t *old=&before->glyph_dsc[gid],*now=&after->glyph_dsc[gid];
        assert(old->adv_w==now->adv_w&&old->box_w==now->box_w&&old->box_h==now->box_h&&old->ofs_x==now->ofs_x&&old->ofs_y==now->ofs_y);
        unsigned n=((unsigned)old->box_w*old->box_h+1)/2;
        if(n>max_raw)max_raw=n;
        glyphs++;
        if(!n)continue;
        const uint8_t *expected=before->glyph_bitmap+old->bitmap_index;
        lv_font_glyph_dsc_t g={.resolved_font=&app_cjk_18,.gid.index=gid,.req_raw_bitmap=1};
        const uint8_t *raw=app_cjk_18.get_glyph_bitmap(&g,NULL);assert(raw&&!memcmp(raw,expected,n));
        uint8_t retained[PODCAST_FONT_RAW_LIMIT];memcpy(retained,raw,n);
        for(unsigned i=0;i<n;i++)raw_hash=hash_byte(raw_hash,raw[i]);
        assert(old->box_w<=32&&old->box_h<=32);
        assert(lv_draw_buf_reshape(a8,LV_COLOR_FORMAT_A8,old->box_w,old->box_h,LV_STRIDE_AUTO));
        memset(a8->data,0xa7,a8->data_size);g.req_raw_bitmap=0;
        assert(lv_font_get_glyph_bitmap(&g,a8)==a8);
        assert(!memcmp(raw,retained,n));
        for(unsigned y=0;y<old->box_h;y++)for(unsigned x=0;x<old->box_w;x++){
            unsigned p=y*old->box_w+x;uint8_t pixel=(uint8_t)(((expected[p/2]>>(p&1?0:4))&15)*17);
            assert(a8->data[y*a8->header.stride+x]==pixel);pixel_hash=hash_byte(pixel_hash,pixel);pixels++;
        }
        for(unsigned y=0;y<old->box_h;y++)for(unsigned x=old->box_w;x<a8->header.stride;x++)assert(a8->data[y*a8->header.stride+x]==0xa7);
        nonempty++;bytes+=n;
    }
    unsigned coverage=0;
    for(uint32_t cp=0;cp<=0xffff;cp++){
        lv_font_glyph_dsc_t old={0},now={0};
        bool was=app_cjk_18_original.get_glyph_dsc(&app_cjk_18_original,&old,cp,0);
        bool is=app_cjk_18.get_glyph_dsc(&app_cjk_18,&now,cp,0);
        assert(was==is);
        if(was){
            assert(old.gid.index==now.gid.index&&old.adv_w==now.adv_w&&old.box_w==now.box_w&&old.box_h==now.box_h&&old.ofs_x==now.ofs_x&&old.ofs_y==now.ofs_y&&old.format==now.format&&old.stride==now.stride);
            coverage++;
        }
    }
    lv_font_glyph_dsc_t unknown={0};assert(!app_cjk_18.get_glyph_dsc(&app_cjk_18,&unknown,0x1f984,0));
    assert(bytes==4206308&&glyphs==27782&&max_raw==171);
    lv_draw_buf_destroy(a8);
    printf("Entire original font vs production codec PASS descriptors=%u nonempty=%u unicode_coverage=%u raw_bytes=%llu A8_pixels=%llu max_raw=%u raw_fnv=%08x pixel_fnv=%08x host_seconds=%.3f\n",
        glyphs,nonempty,coverage,(unsigned long long)bytes,(unsigned long long)pixels,max_raw,raw_hash,pixel_hash,(double)(clock()-start)/CLOCKS_PER_SEC);
    return 0;
}
