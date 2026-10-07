#include "podcast_font_codec.h"
#include <string.h>

static uint8_t raw_workspace[PODCAST_FONT_RAW_LIMIT];

typedef struct {
    const uint8_t *next,*end;
    uint8_t bits,remaining;
} bit_reader_t;
typedef struct {
    uint8_t *raw,*a8;
    uint32_t width,stride,pixels,at;
} output_t;

static bool read_symbol(bit_reader_t *reader,const podcast_font_codec_t *codec,uint8_t *symbol)
{
    uint32_t code=0;
    for(unsigned n=1;n<=codec->max_code_bits;n++){
        if(!reader->remaining){
            if(reader->next==reader->end)return false;
            reader->bits=*reader->next++;reader->remaining=8;
        }
        code=(code<<1)|(reader->bits>>7);
        reader->bits<<=1;reader->remaining--;
        uint32_t delta=code-codec->first_codes[n];
        if(delta<codec->counts[n]){
            uint32_t at=codec->first_symbols[n]+delta;
            if(at>=codec->symbol_count)return false;
            *symbol=codec->symbols[at];return true;
        }
    }
    return false;
}
static void write_byte(output_t *output,uint8_t byte)
{
    if(output->raw){output->raw[output->at++]=byte;return;}
    for(unsigned shift=0;shift<2;shift++){
        uint32_t at=output->at++;
        if(at<output->pixels)
            output->a8[at/output->width*output->stride+at%output->width]=
                (uint8_t)(((byte>>(shift?0:4))&15)*17);
    }
}
static bool decode(const podcast_font_codec_t *codec,const uint8_t *input,
                   size_t input_bytes,bool compressed,output_t *output,size_t bytes)
{
    if(bytes>PODCAST_FONT_RAW_LIMIT)return false;
    if(!bytes)return true;
    if(!input||(!output->raw&&!output->a8))return false;
    if(!compressed){
        if(input_bytes<bytes)return false;
        for(size_t i=0;i<bytes;i++)write_byte(output,input[i]);
        return true;
    }
    if(!codec||!codec->counts||!codec->first_codes||!codec->first_symbols||!codec->symbols||
       !codec->max_code_bits||codec->max_code_bits>16||!codec->symbol_count||codec->symbol_count>256)
        return false;
    bit_reader_t reader={.next=input,.end=input+input_bytes};
    for(size_t i=0;i<bytes;i++){
        uint8_t symbol;
        if(!read_symbol(&reader,codec,&symbol))return false;
        write_byte(output,symbol);
    }
    return true;
}
bool podcast_font_decode_packed(const podcast_font_codec_t *codec,const uint8_t *input,
    size_t input_bytes,bool compressed,uint8_t *output,size_t output_bytes)
{
    output_t target={.raw=output};
    return decode(codec,input,input_bytes,compressed,&target,output_bytes);
}
const void *podcast_font_get_bitmap_lossless(lv_font_glyph_dsc_t *glyph,lv_draw_buf_t *draw_buf)
{
    if(!glyph||!glyph->resolved_font)return NULL;
    const lv_font_t *font=glyph->resolved_font;
    const lv_font_fmt_txt_dsc_t *dsc=font->dsc;
    const podcast_font_codec_t *codec=font->user_data;
    uint32_t gid=glyph->gid.index;
    if(!codec||!dsc||!gid||gid>=codec->glyph_count||dsc->bpp!=4||dsc->stride)return NULL;
    const lv_font_fmt_txt_glyph_dsc_t *g=&dsc->glyph_dsc[gid];
    uint32_t pixels=(uint32_t)g->box_w*g->box_h;
    size_t bytes=(pixels+1)/2;
    if(bytes>PODCAST_FONT_RAW_LIMIT)return NULL;
    uint32_t at=g->bitmap_index&PODCAST_FONT_OFFSET_MASK;
    uint32_t end=gid+1<codec->glyph_count?
        dsc->glyph_dsc[gid+1].bitmap_index&PODCAST_FONT_OFFSET_MASK:codec->bitmap_bytes;
    if(at>end||end>codec->bitmap_bytes)return NULL;
    bool compressed=(g->bitmap_index&PODCAST_FONT_HUFFMAN_FLAG)!=0;
    const uint8_t *input=dsc->glyph_bitmap+at;
    if(glyph->req_raw_bitmap){
        if(!podcast_font_decode_packed(codec,input,end-at,compressed,raw_workspace,bytes))return NULL;
        return raw_workspace;
    }
    if(!pixels||!draw_buf||draw_buf->header.cf!=LV_COLOR_FORMAT_A8||
       draw_buf->header.w<g->box_w||draw_buf->header.h<g->box_h||draw_buf->header.stride<g->box_w||
       !draw_buf->data||(uint64_t)draw_buf->header.stride*g->box_h>draw_buf->data_size)return NULL;
    output_t target={.a8=draw_buf->data,.width=g->box_w,.stride=draw_buf->header.stride,.pixels=pixels};
    if(!decode(codec,input,end-at,compressed,&target,bytes))return NULL;
    lv_draw_buf_flush_cache(draw_buf,NULL);return draw_buf;
}
