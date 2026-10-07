#include "podcast_font_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    lv_init();
    static const uint16_t counts[]={0,0,4},first[]={0,0,0};
    static const uint8_t symbols[]={0x00,0xff,0xa5,0x5a};
    podcast_font_codec_t codec={.glyph_count=2,.bitmap_bytes=2,.symbol_count=4,.max_code_bits=2,
        .counts=counts,.first_codes=first,.first_symbols=first,.symbols=symbols};
    const uint8_t stream[]={0x1b,0x00},expected[]={0x00,0xff,0xa5,0x5a,0x00};
    uint8_t guard[24];memset(guard,0xcc,sizeof(guard));
    assert(podcast_font_decode_packed(&codec,stream,sizeof(stream),true,guard+8,5));
    assert(!memcmp(guard+8,expected,5));
    for(unsigned i=0;i<8;i++)assert(guard[i]==0xcc);
    for(unsigned i=13;i<sizeof(guard);i++)assert(guard[i]==0xcc);
    assert(!podcast_font_decode_packed(&codec,stream,0,true,guard+8,5));
    assert(!podcast_font_decode_packed(&codec,stream,1,true,guard+8,5));
    assert(!podcast_font_decode_packed(NULL,stream,sizeof(stream),true,guard+8,5));
    assert(!podcast_font_decode_packed(&codec,stream,sizeof(stream),true,NULL,5));
    assert(!podcast_font_decode_packed(&codec,stream,sizeof(stream),true,guard+8,PODCAST_FONT_RAW_LIMIT+1));
    assert(!podcast_font_decode_packed(NULL,expected,4,false,guard+8,5));
    assert(podcast_font_decode_packed(NULL,expected,5,false,guard+8,5));
    assert(podcast_font_decode_packed(NULL,NULL,0,false,NULL,0));
    codec.max_code_bits=17;
    assert(!podcast_font_decode_packed(&codec,stream,2,true,guard+8,5));
    codec.max_code_bits=1;
    const uint16_t incomplete_counts[]={0,1},bad_offset[]={0,4};
    codec.counts=incomplete_counts;
    const uint8_t invalid=0x80;
    assert(!podcast_font_decode_packed(&codec,&invalid,1,true,guard+8,1));
    codec.first_symbols=bad_offset;
    const uint8_t zero=0;
    assert(!podcast_font_decode_packed(&codec,&zero,1,true,guard+8,1));
    codec.counts=counts;codec.first_symbols=first;codec.max_code_bits=2;

    lv_font_fmt_txt_glyph_dsc_t descriptors[2]={{0},{.bitmap_index=PODCAST_FONT_HUFFMAN_FLAG,.box_w=3,.box_h=3}};
    lv_font_fmt_txt_dsc_t data={.glyph_bitmap=stream,.glyph_dsc=descriptors,.bpp=4};
    lv_font_t font={.get_glyph_bitmap=podcast_font_get_bitmap_lossless,.dsc=&data,.user_data=&codec};
    lv_font_glyph_dsc_t glyph={.resolved_font=&font,.gid.index=1,.req_raw_bitmap=1};
    const uint8_t *raw=podcast_font_get_bitmap_lossless(&glyph,NULL);assert(raw&&!memcmp(raw,expected,5));
    lv_draw_buf_t *buffer=lv_draw_buf_create(3,3,LV_COLOR_FORMAT_A8,LV_STRIDE_AUTO);assert(buffer);
    memset(buffer->data,0xcc,buffer->data_size);
    glyph.req_raw_bitmap=0;
    assert(podcast_font_get_bitmap_lossless(&glyph,buffer)==buffer);
    for(unsigned i=0;i<9;i++){
        unsigned nibble=(expected[i/2]>>(i&1?0:4))&15;
        assert(buffer->data[i/3*buffer->header.stride+i%3]==nibble*17);
    }
    for(unsigned y=0;y<3;y++)for(unsigned x=3;x<buffer->header.stride;x++)assert(buffer->data[y*buffer->header.stride+x]==0xcc);
    assert(!memcmp(raw,expected,5)); /* Normal draw does not overwrite raw workspace. */
    glyph.gid.index=2;assert(!podcast_font_get_bitmap_lossless(&glyph,buffer));
    glyph.gid.index=1;descriptors[1].bitmap_index=PODCAST_FONT_HUFFMAN_FLAG|3;
    assert(!podcast_font_get_bitmap_lossless(&glyph,buffer));
    descriptors[1].bitmap_index=PODCAST_FONT_HUFFMAN_FLAG;
    data.bpp=3;assert(!podcast_font_get_bitmap_lossless(&glyph,buffer));data.bpp=4;
    descriptors[1].box_w=1025;descriptors[1].box_h=1;
    assert(!podcast_font_get_bitmap_lossless(&glyph,buffer));
    descriptors[1].box_w=3;descriptors[1].box_h=3;
    assert(!podcast_font_get_bitmap_lossless(&glyph,NULL));
    lv_draw_buf_destroy(buffer);
    puts("Codec bounds/canaries/truncation/invalid-code/odd-width/A8-padding/raw-lifetime PASS");
    return 0;
}
