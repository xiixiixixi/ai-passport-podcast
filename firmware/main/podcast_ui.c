#include "podcast_ui.h"
#include "podcast_fonts.h"
#include "podcast_cover_dynamic.h"
#include "podcast_ruler.h"
#include "lvgl.h"
#include "src/misc/lv_text_private.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

/* The selected radio face: small cover, two-line title, large elapsed time,
 * truthful minute ruler and numeric loudness. Physical controls stay explicit.
 * Static draw primitives avoid extra objects, layers or decorative animation. */
enum { BG=0xFFFFFF, TEXT=0x111111, MUTED=0x5D6065,
       ACCENT=0x0063F2, TRACK=0xC5C7CA, RECENT=0xEDF4FF };
static lv_obj_t *screen,*heading,*battery,*status,*hint,*back_hint,*list;
static lv_obj_t *row[3],*row_title[3],*row_detail[3];
static lv_obj_t *show,*title,*state,*times,*value,*volume;
static podcast_page_t page;
static char cover_id[24],row_cover_id[3][24];
static unsigned drawn_elapsed,drawn_duration,drawn_volume,drawn_target;
static bool row_selected[3],drawn_recent,drawn_recent_current,drawn_recent_selected;
static int drawn_battery;
/* One bounded native-size image, separate from the 24KiB LVGL pool. LVGL's
 * scaled-image temporary buffers fragment that pool after list navigation. */
_Alignas(4) static uint16_t player_cover_pixels[52*52];
static char player_cover_cached_id[24];
static bool player_cover_ready;
static uint32_t player_cover_cached_stamp;
static const lv_image_dsc_t player_cover_image={
    .header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RGB565,.flags=0,.w=52,.h=52,.stride=104},
    .data_size=sizeof(player_cover_pixels),.data=(const uint8_t *)player_cover_pixels
};
static char recent_cover_id[24];
static const char *key_middle;
/* All entry, render and timer work runs in LVGL's locked UI context. A visit
 * begins on a page change, but loading lists defer help until usable content. */
static lv_timer_t *help_timer;
static bool help_visit,help_pending,help_showing;
static bool text_reported;
static podcast_ui_text_report_t last_text_report;

static void set_text(lv_obj_t *obj,const char *text)
{
    char plain[PODCAST_TITLE_BYTES];size_t used=0;uint32_t offset=0;
    while(text[offset]) {
        uint32_t before=offset,cp=lv_text_encoded_next(text,&offset);
        if(!cp||offset<=before){lv_label_set_text(obj,text);return;}
        if(cp==0xfe0f)continue;
        const char *piece=text+before;size_t bytes=offset-before;
        if(cp==0x2716){piece="×";bytes=sizeof("×")-1;}
        else if(cp==0xfffc){piece="[图]";bytes=sizeof("[图]")-1;}
        if(used+bytes>=sizeof(plain))break;
        memcpy(plain+used,piece,bytes);used+=bytes;
    }
    plain[used]=0;lv_label_set_text(obj,plain);
}
static void visible(lv_obj_t *obj,bool yes)
{
    if(yes)lv_obj_remove_flag(obj,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj,LV_OBJ_FLAG_HIDDEN);
}
static void cancel_help(void)
{
    if(help_timer){lv_timer_t *timer=help_timer;help_timer=NULL;lv_timer_delete(timer);}
    help_pending=false;help_showing=false;
}
static void collapsed_help(void)
{
    const char *text=page==PODCAST_NOW?"长按侧键调进度":
        page==PODCAST_SEEK?"确定应用 · 长按确定取消":page==PODCAST_VOLUME?"确定返回":
        page==PODCAST_ACTIONS||page==PODCAST_SLEEP?"长按确定回到播放":"";
    lv_obj_set_pos(back_hint,12,294);lv_obj_set_size(back_hint,216,20);
    lv_label_set_text_static(back_hint,text);visible(back_hint,*text!=0);
}
static void hide_help(lv_timer_t *timer)
{
    if(timer!=help_timer)return;
    help_timer=NULL;lv_timer_delete(timer);help_showing=false;
    if(screen){collapsed_help();lv_obj_invalidate(screen);}
}
static const char *entry_help(const podcast_view_t *v)
{
    switch(page){
    case PODCAST_SHOWS:return v->has_recent?"确定进入 · 长确定续听":
        "上下选择，确定进入\n长按侧键到首末";
    case PODCAST_EPISODES:return "确定播放，长按确定返回\n长上换序，长下首集";
    case PODCAST_NOW:return "长按侧键调进度";
    case PODCAST_ACTIONS:return "上下选择，确定执行\n长按确定回到播放";
    case PODCAST_VOLUME:return "侧键调整音量\n确定回到播放";
    case PODCAST_SLEEP:return "上下选择，确定设置\n长按确定回到播放";
    case PODCAST_SEEK:return "侧键调15秒，长按60秒\n确定应用，长按确定取消";
    }
    return "";
}
static void render_help(const podcast_view_t *v)
{
    bool ready=page==PODCAST_SHOWS?v->count>0||v->has_recent:
        page==PODCAST_EPISODES?v->count>0:true;
    if(page==PODCAST_NOW){help_pending=false;collapsed_help();return;}
    if(help_pending&&ready){
        help_pending=false;help_timer=lv_timer_create(hide_help,3000,NULL);
        help_showing=help_timer!=NULL;
    }
    if(help_showing){
        bool compact=page==PODCAST_SHOWS&&v->has_recent;
        lv_obj_set_pos(back_hint,12,compact?294:276);lv_obj_set_size(back_hint,216,compact?20:40);
        lv_label_set_text_static(back_hint,entry_help(v));visible(back_hint,true);
    } else collapsed_help();
}
static void place(lv_obj_t *obj,int x,int y,int w,int h)
{
    lv_obj_set_pos(obj,x,y);lv_obj_set_size(obj,w,h);
}
static lv_obj_t *area(lv_obj_t *parent,int x,int y,int w,int h)
{
    lv_obj_t *obj=lv_obj_create(parent);lv_obj_remove_style_all(obj);
    place(obj,x,y,w,h);lv_obj_remove_flag(obj,LV_OBJ_FLAG_SCROLLABLE);return obj;
}
static lv_obj_t *label(lv_obj_t *parent,const char *role,const lv_font_t *font,int color)
{
    lv_obj_t *obj=lv_label_create(parent);lv_obj_remove_style_all(obj);
    lv_obj_set_user_data(obj,(void *)role);
    lv_obj_set_style_text_font(obj,font,0);
    lv_obj_set_style_text_color(obj,lv_color_hex(color),0);
    lv_label_set_long_mode(obj,LV_LABEL_LONG_DOT);lv_label_set_text(obj,"");return obj;
}
static void clock_text(char *out,size_t size,unsigned seconds)
{
    if(seconds>=3600)snprintf(out,size,"%u:%02u:%02u",seconds/3600,seconds/60%60,seconds%60);
    else snprintf(out,size,"%u:%02u",seconds/60,seconds%60);
}
static void large_clock(lv_obj_t *obj,const char *text,int width)
{
    lv_point_t size;lv_text_get_size(&size,text,&lv_font_montserrat_32,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    lv_obj_set_style_text_font(obj,size.x>width?&lv_font_montserrat_20:&lv_font_montserrat_32,0);
    lv_label_set_text(obj,text);
}
static void rectangle(lv_layer_t *layer,int x,int y,int w,int h,int color)
{
    if(w<=0||h<=0)return;
    lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.bg_color=lv_color_hex(color);
    d.bg_opa=LV_OPA_COVER;d.radius=0;
    lv_area_t box={x,y,x+w-1,y+h-1};lv_draw_rect(layer,&d,&box);
}
static void draw_text(lv_layer_t *layer,const char *text,const lv_font_t *font,
                      int x,int y,int w,int h,int color,lv_text_align_t align)
{
    if(!text||!*text)return;
    lv_draw_label_dsc_t d;lv_draw_label_dsc_init(&d);
    d.text=text;d.font=font;d.color=lv_color_hex(color);d.align=align;
    lv_area_t box={x,y,x+w-1,y+h-1};lv_draw_label(layer,&d,&box);
}
static void draw_artwork(lv_layer_t *layer,const char *id,int x,int y,bool thumbnail)
{
    const lv_image_dsc_t *image=podcast_dynamic_cover_get(id);if(!image)return;
    lv_draw_image_dsc_t d;lv_draw_image_dsc_init(&d);d.src=image;
    /* Thumbnails draw at source size (52px, rows are 68px tall): scaling in
     * the draw path would allocate an intermediate image in the LVGL pool. */
    int side=thumbnail?(int)image->header.w:112;
    lv_area_t box={x,y,x+side-1,y+side-1};lv_draw_image(layer,&d,&box);
}
static void prepare_player_cover(void)
{
    uint32_t stamp=podcast_dynamic_cover_stamp(cover_id);
    if(player_cover_ready&&!strcmp(player_cover_cached_id,cover_id)&&
        (player_cover_cached_stamp==stamp||(!stamp&&player_cover_cached_stamp)))return;
    const lv_image_dsc_t *image=podcast_dynamic_cover_get(cover_id);
    if(!image)return;
    for(unsigned y=0;y<52;y++)for(unsigned x=0;x<52;x++){
        unsigned sx=(2*x+1)*image->header.w/104,sy=(2*y+1)*image->header.h/104;
        memcpy(&player_cover_pixels[y*52+x],image->data+sy*image->header.stride+sx*2,2);
    }
    lv_image_cache_drop(&player_cover_image);
    snprintf(player_cover_cached_id,sizeof(player_cover_cached_id),"%s",cover_id);player_cover_ready=true;player_cover_cached_stamp=stamp;
}
static void draw_player_cover(lv_layer_t *layer)
{
    lv_draw_image_dsc_t d;lv_draw_image_dsc_init(&d);d.src=&player_cover_image;
    lv_area_t box={10,14,61,65};lv_draw_image(layer,&d,&box);
}
static void radio_ruler(lv_layer_t *layer)
{
    podcast_ruler_t r=podcast_ruler_make(drawn_elapsed,drawn_duration,12,228);
    if(!r.known){
        draw_text(layer,"时长待获取",&app_cjk_18,12,184,216,23,MUTED,LV_TEXT_ALIGN_LEFT);
        rectangle(layer,12,212,216,1,TRACK);return;
    }
    const char *unit=r.unit==PODCAST_RULER_SECONDS?"秒":r.unit==PODCAST_RULER_HOURS?"小时":"分钟";
    draw_text(layer,unit,&app_cjk_18,170,157,58,23,MUTED,LV_TEXT_ALIGN_RIGHT);
    for(uint64_t sec=0;sec<=r.duration;sec+=r.minor_seconds){
        unsigned x=podcast_ruler_position((uint32_t)sec,r.duration,r.left,r.right);
        bool middle=(sec/r.minor_seconds)%5==0;
        rectangle(layer,(int)x,middle?181:187,1,middle?22:13,TRACK);
    }
    for(uint64_t sec=0;sec<=r.duration;sec+=r.major_seconds){
        unsigned x=podcast_ruler_position((uint32_t)sec,r.duration,r.left,r.right);
        rectangle(layer,(int)x,177,1,29,TEXT);
        char text[16];snprintf(text,sizeof(text),"%u",(unsigned)(sec/r.unit_seconds));
        lv_point_t size;lv_text_get_size(&size,text,&lv_font_montserrat_14,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
        int at=(int)x-size.x/2;if(at<12)at=12;if(at+size.x>228)at=228-size.x;
        draw_text(layer,text,&lv_font_montserrat_14,at,209,size.x+1,19,TEXT,LV_TEXT_ALIGN_LEFT);
    }
    rectangle(layer,228,177,1,37,TEXT);
    lv_draw_triangle_dsc_t t;lv_draw_triangle_dsc_init(&t);t.color=lv_color_hex(ACCENT);t.opa=LV_OPA_COVER;
    int x=(int)r.pointer;t.p[0]=(lv_point_precise_t){x-5,170};t.p[1]=(lv_point_precise_t){x+5,170};t.p[2]=(lv_point_precise_t){x,177};lv_draw_triangle(layer,&t);
    rectangle(layer,x,180,2,27,ACCENT);
}
static void volume_segments(lv_layer_t *layer)
{
    unsigned value=drawn_volume>100?100:drawn_volume;
    for(unsigned i=0;i<10;i++){
        int x=114+(int)i*11;rectangle(layer,x,253,8,21,TRACK);
        unsigned fraction=value>i*10?value-i*10:0;if(fraction>10)fraction=10;
        int fill=(int)((fraction*8+5)/10);rectangle(layer,x,253,fill,21,ACCENT);
    }
}
static void battery_shape(lv_layer_t *layer)
{
    int color=drawn_battery>=0?TEXT:TRACK;
    rectangle(layer,215,11,18,1,color);rectangle(layer,215,22,18,1,color);
    rectangle(layer,215,11,1,12,color);rectangle(layer,232,11,1,12,color);rectangle(layer,234,14,2,6,color);
    unsigned value=drawn_battery>100?100:drawn_battery<0?0:(unsigned)drawn_battery;
    rectangle(layer,218,14,(int)((uint64_t)value*12/100),6,color);
}
static void draw_cover(lv_layer_t *layer,const char *id,int x,int y)
{
    draw_artwork(layer,id,x,y,true);
}
static void track(lv_layer_t *layer,int y,unsigned at,unsigned total)
{
    rectangle(layer,12,y,216,4,TRACK);
    unsigned width=total?(unsigned)((uint64_t)at*216/total):0;
    rectangle(layer,12,y,width>216?216:(int)width,4,ACCENT);
}
static void loudness(lv_layer_t *layer,int x,int y,int w)
{
    unsigned level=drawn_volume>100?100:drawn_volume;
    for(unsigned i=0;i<20;++i)
        rectangle(layer,x+(int)i*w/20,y,w/20-2,5,i*5<level?ACCENT:TRACK);
}
static const char *transport_symbol(void)
{
    if(!strcmp(key_middle,"暂停"))return LV_SYMBOL_PAUSE;
    if(!strcmp(key_middle,"续播")||!strcmp(key_middle,"重播"))return LV_SYMBOL_PLAY;
    if(!strcmp(key_middle,"重试"))return LV_SYMBOL_REFRESH;
    return NULL;
}
static void draw_page(lv_event_t *event)
{
    lv_layer_t *layer=lv_event_get_layer(event);
    if(page==PODCAST_NOW) {
        draw_player_cover(layer);battery_shape(layer);rectangle(layer,10,74,220,1,TEXT);
        const char *symbol=transport_symbol();
        draw_text(layer,symbol?symbol:"...",&lv_font_montserrat_20,10,85,21,25,ACCENT,LV_TEXT_ALIGN_LEFT);
        radio_ruler(layer);
        draw_text(layer,"音量",&app_cjk_18,10,254,36,23,TEXT,LV_TEXT_ALIGN_LEFT);volume_segments(layer);
    } else if(page==PODCAST_SHOWS&&drawn_recent) {
        draw_text(layer,drawn_recent_current?"正在收听":"最近收听",&app_ui_16,12,35,216,20,TEXT,LV_TEXT_ALIGN_LEFT);
        rectangle(layer,8,58,224,68,RECENT);
        if(drawn_recent_selected){
            rectangle(layer,8,58,3,68,ACCENT);
            draw_text(layer,LV_SYMBOL_RIGHT,&lv_font_montserrat_14,216,84,12,20,ACCENT,LV_TEXT_ALIGN_RIGHT);
        }
        draw_cover(layer,recent_cover_id,12,65);
        draw_text(layer,"最近更新",&app_ui_16,12,129,216,20,TEXT,LV_TEXT_ALIGN_LEFT);
    } else if(page==PODCAST_EPISODES)draw_cover(layer,cover_id,12,36);
    else if(page==PODCAST_SEEK)track(layer,162,drawn_target,drawn_duration);
    else if(page==PODCAST_VOLUME)loudness(layer,12,205,216);
}
static void draw_row(lv_event_t *event)
{
    unsigned i=(unsigned)(uintptr_t)lv_event_get_user_data(event);if(i>=3)return;
    lv_obj_t *obj=lv_event_get_target(event);lv_area_t box;lv_obj_get_coords(obj,&box);
    lv_layer_t *layer=lv_event_get_layer(event);
    if(page==PODCAST_SHOWS) {
        draw_cover(layer,row_cover_id[i],12,box.y1+6);
        rectangle(layer,12,box.y2,216,1,TRACK);
    }
    if(row_selected[i])draw_text(layer,LV_SYMBOL_RIGHT,&lv_font_montserrat_14,
        216,box.y1+10,12,20,ACCENT,LV_TEXT_ALIGN_RIGHT);
}
void podcast_ui_create(void)
{
    text_reported=false;page=PODCAST_SHOWS;key_middle="";help_visit=false;
    screen=area(NULL,0,0,240,320);
    lv_obj_set_style_bg_color(screen,lv_color_hex(BG),0);
    lv_obj_set_style_bg_opa(screen,LV_OPA_COVER,0);
    lv_obj_add_event_cb(screen,draw_page,LV_EVENT_DRAW_MAIN,NULL);
    heading=label(screen,"heading",&app_cjk_18,TEXT);place(heading,12,8,164,23);
    battery=label(screen,"battery",&lv_font_montserrat_14,MUTED);place(battery,180,12,48,20);
    lv_obj_set_style_text_align(battery,LV_TEXT_ALIGN_RIGHT,0);
    status=label(screen,"status",&app_ui_16,MUTED);
    hint=label(screen,"hint",&app_ui_16,TEXT);
    back_hint=label(screen,"back_hint",&app_ui_16,MUTED);
    lv_obj_set_style_text_align(back_hint,LV_TEXT_ALIGN_CENTER,0);
    list=area(screen,0,62,240,204);
    for(unsigned i=0;i<3;++i) {
        row[i]=area(list,0,i*68,240,68);
        lv_obj_add_event_cb(row[i],draw_row,LV_EVENT_DRAW_MAIN,(void *)(uintptr_t)i);
        static const char *titles[]={"row_title_0","row_title_1","row_title_2"};
        static const char *details[]={"row_detail_0","row_detail_1","row_detail_2"};
        row_title[i]=label(row[i],titles[i],&app_cjk_18,TEXT);
        row_detail[i]=label(row[i],details[i],&app_ui_16,MUTED);
    }
    show=label(screen,"show",&app_cjk_18,TEXT);
    title=label(screen,"title",&app_cjk_18,TEXT);
    state=label(screen,"state",&app_ui_16,ACCENT);
    times=label(screen,"elapsed",&lv_font_montserrat_32,TEXT);
    value=label(screen,"value",&lv_font_montserrat_14,MUTED);
    volume=label(screen,"volume",&lv_font_montserrat_32,ACCENT);
    lv_screen_load(screen);
}
static const char *player_status(const podcast_view_t *v)
{
    if(strstr(v->status,"无法存进度"))return "进度未保存";
    if(strstr(v->status,"离线续播"))return "离线续播，待同步";
    if(strstr(v->status,"进度待同步"))return v->playing?"正在播放，待同步":"暂停待同步，确定续播";
    if(strstr(v->status,"进度已更新"))return "进度更新，确定续播";
    if(strstr(v->status,"记录待同步"))return "离线记录待同步";
    if(v->resuming)return "同步续播，再按取消";
    if(v->busy)return "准备音频";
    if(strstr(v->status,"中断"))return "播放中断，确定重试";
    if(strstr(v->status,"失败"))return "播放失败，确定重试";
    if(strstr(v->back_hint,"确定重试"))return "确定重试播放";
    if(strstr(v->back_hint,"确定续播"))return "已暂停，确定续播";
    if(strstr(v->status,"播完"))return "本集播完，确定重播";
    return v->status;
}
static const char *action(const podcast_view_t *v)
{
    if(v->resuming)return "暂停";
    if(v->busy)return "等待";
    if(strstr(v->back_hint,"重试"))return "重试";
    if(strstr(v->back_hint,"重播"))return "重播";
    return v->playing||strstr(v->back_hint,"暂停")?"暂停":"续播";
}
void podcast_ui_render(const podcast_view_t *v)
{
    if(!screen)return;
    if(!help_visit||page!=v->page){cancel_help();help_pending=true;help_visit=true;}
    page=v->page;
    drawn_recent=v->page==PODCAST_SHOWS&&v->has_recent; drawn_recent_current=v->recent_is_current;
    drawn_recent_selected=drawn_recent&&v->selected<0;
    snprintf(recent_cover_id,sizeof(recent_cover_id),"%s",v->recent_show_id);
    drawn_battery=v->battery_percent;
    drawn_elapsed=v->elapsed;drawn_duration=v->duration;drawn_volume=v->volume;drawn_target=v->seek_target;
    snprintf(cover_id,sizeof(cover_id),"%s",v->show_id);
    for(unsigned i=0;i<3;++i)snprintf(row_cover_id[i],sizeof(row_cover_id[i]),"%s",v->rows[i].show_id);
    bool library=page==PODCAST_SHOWS,episodes=page==PODCAST_EPISODES;
    bool player=page==PODCAST_NOW,seek=page==PODCAST_SEEK,vol=page==PODCAST_VOLUME;
    bool is_list=library||episodes||page==PODCAST_ACTIONS||page==PODCAST_SLEEP;
    lv_obj_t *optional[]={status,hint,back_hint,list,show,title,state,times,value,volume};
    for(unsigned i=0;i<sizeof(optional)/sizeof(optional[0]);++i)visible(optional[i],false);
    /* Off-page texts are rebound from the immutable view on return. Avoid
     * retaining their large heap buffers while drawing the 48px clock. */
    if(!is_list){
        for(unsigned i=0;i<3;i++){lv_label_set_text_static(row_title[i],"");lv_label_set_text_static(row_detail[i],"");}
        lv_label_set_text_static(hint,"");
    }
    if(player){lv_label_set_text_static(show,"");lv_label_set_text_static(status,"");}
    lv_obj_set_style_text_font(value,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_align(value,LV_TEXT_ALIGN_LEFT,0);
    lv_obj_set_style_text_color(show,lv_color_hex(TEXT),0);
    lv_obj_set_style_text_color(title,lv_color_hex(TEXT),0);
    place(heading,12,8,164,23);place(battery,180,12,48,20);
    lv_obj_set_style_text_color(battery,lv_color_hex(MUTED),0);
    set_text(heading,library?"节目库":episodes?"单集":player?"随身听":seek?"调整进度":v->heading);
    if(v->battery_percent>=0)lv_label_set_text_fmt(battery,"%d%%",v->battery_percent);else lv_label_set_text(battery,"");
    place(status,12,35,216,20);place(hint,12,272,216,20);place(back_hint,12,296,216,20);
    key_middle="";
    if(player)prepare_player_cover();
    if(is_list) {
        visible(status,true);visible(list,true);
        lv_label_set_text_static(hint,"");set_text(status,v->status);
        place(list,0,episodes?90:drawn_recent?151:62,240,episodes?180:drawn_recent?136:204);
        if(drawn_recent) {
            int text_width=drawn_recent_selected?136:156;
            visible(show,true);place(show,72,61,text_width,23);set_text(show,v->recent_name);
            visible(title,true);place(title,72,85,text_width,23);set_text(title,v->recent_title);
            lv_obj_set_style_text_color(show,lv_color_hex(drawn_recent_selected?ACCENT:TEXT),0);
            lv_obj_set_style_text_color(title,lv_color_hex(drawn_recent_selected?ACCENT:TEXT),0);
            visible(times,true);place(times,72,110,156,18);
            lv_obj_set_style_text_font(times,&lv_font_montserrat_14,0);
            char a[16],b[16];clock_text(a,sizeof(a),v->recent_elapsed);clock_text(b,sizeof(b),v->recent_duration);
            if(v->recent_duration)lv_label_set_text_fmt(times,"%s / %s",a,b);
            else lv_label_set_text(times,a);
            visible(status,false);visible(hint,false);
            place(back_hint,12,293,216,20);
        }
        if(library||episodes) {
            if(episodes){visible(show,true);place(show,72,36,156,23);set_text(show,v->show[0]?v->show:v->heading);place(status,72,62,156,20);}
            if(v->count) {
                if(episodes)lv_label_set_text_fmt(status,"%s %d/%d",v->oldest_first?"旧到新":"新到旧",v->absolute_selected+1,v->total);
                else lv_label_set_text_fmt(status,"最新更新 %d/%d",v->absolute_selected+1,v->total);
            }
        }
        int y=0;
        for(unsigned i=0;i<3;++i) {
            visible(row[i],i<(unsigned)v->count);if(i>=(unsigned)v->count)continue;
            bool chosen=(int)i==v->selected;row_selected[i]=chosen;
            int h=episodes?(chosen?76:52):68;place(row[i],0,y,240,h);y+=h;
            lv_obj_set_style_text_color(row_title[i],lv_color_hex(chosen?ACCENT:TEXT),0);
            place(row_title[i],library?72:12,1,library?136:chosen?196:216,46);
            set_text(row_title[i],v->rows[i].title);
            if(library) {
                lv_point_t size;lv_text_get_size(&size,lv_label_get_text(row_title[i]),&app_cjk_18,0,0,136,LV_TEXT_FLAG_NONE);
                bool one=size.y<=app_cjk_18.line_height;
                place(row_title[i],72,one?6:0,136,one?23:46);
                place(row_detail[i],72,one?32:47,156,20);
                set_text(row_detail[i],v->rows[i].detail[0]?v->rows[i].detail:v->rows[i].latest_date);
            } else {
                bool two=episodes&&chosen;
                place(row_title[i],12,episodes?1:11,chosen?196:216,two?46:23);
                place(row_detail[i],12,two?51:episodes?29:39,216,20);set_text(row_detail[i],v->rows[i].detail);
            }
            lv_obj_invalidate(row[i]);
        }
    } else if(player) {
        char head[112];
        snprintf(head,sizeof(head),"%s",player_status(v));
        if(v->sleep_minutes){
            snprintf(head,sizeof(head),"%s %u分后停",player_status(v),v->sleep_minutes);
            lv_point_t width;lv_text_get_size(&width,head,&app_cjk_18,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
            if(width.x>193)snprintf(head,sizeof(head),"%s",player_status(v));
        }
        place(heading,35,84,193,25);set_text(heading,head);
        place(battery,173,8,39,17);lv_obj_set_style_text_color(battery,lv_color_hex(TEXT),0);
        if(v->battery_percent<0)lv_label_set_text(battery,"--");
        visible(title,true);place(title,72,26,156,46);set_text(title,v->title);
        visible(status,false);visible(show,false);
        char a[16];clock_text(a,sizeof(a),v->elapsed);
        const lv_font_t *font=&lv_font_montserrat_48;lv_point_t size;
        lv_text_get_size(&size,a,font,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
        if(size.x>218){font=&lv_font_montserrat_32;lv_text_get_size(&size,a,font,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);}
        if(size.x>218){font=&lv_font_montserrat_20;lv_text_get_size(&size,a,font,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);}
        visible(times,true);place(times,10,104,218,53);lv_obj_set_style_text_font(times,font,0);lv_label_set_text(times,a);
        visible(state,true);lv_obj_set_style_text_color(state,lv_color_hex(MUTED),0);set_text(state,"已播放");
        if(size.x+60<=218)place(state,20+size.x,104+font->line_height-font->base_line-15,58,20);
        else place(state,10,150,78,20);
        visible(value,true);place(value,12,228,216,19);
        char b[16];clock_text(b,sizeof(b),v->duration);lv_label_set_text(value,v->duration?b:"--:--");
        lv_obj_set_style_text_align(value,LV_TEXT_ALIGN_RIGHT,0);
        visible(volume,true);place(volume,46,244,66,36);
        lv_obj_set_style_text_font(volume,&lv_font_montserrat_32,0);lv_obj_set_style_text_color(volume,lv_color_hex(TEXT),0);
        lv_obj_set_style_text_align(volume,LV_TEXT_ALIGN_LEFT,0);lv_label_set_text_fmt(volume,"%u",v->volume>100?100:v->volume);
        key_middle=action(v);
    } else if(seek||vol) {
        visible(show,true);place(show,12,38,216,23);set_text(show,v->show);
        visible(title,true);place(title,12,65,216,46);set_text(title,v->title);
        visible(times,true);place(times,12,113,216,40);
        visible(status,true);place(status,12,184,216,20);
        if(seek) {
            char target[16],total[16],original[48],old[16];clock_text(target,sizeof(target),v->seek_target);
            clock_text(total,sizeof(total),v->duration);large_clock(times,target,216);clock_text(old,sizeof(old),v->elapsed);
            snprintf(original,sizeof(original),"原位置 %s",old);set_text(status,original);
            visible(value,true);place(value,12,214,216,20);lv_label_set_text_fmt(value,"/ %s",total);
        } else {
            lv_obj_set_style_text_font(times,&lv_font_montserrat_32,0);
            lv_label_set_text_fmt(times,"%u",v->volume>100?100:v->volume);set_text(status,"音量会自动记住");
        }
    }
    render_help(v);lv_obj_invalidate(screen);
    podcast_ui_text_report_t report;bool ok=podcast_ui_inspect_text(&report);
    if(!text_reported||memcmp(&last_text_report,&report,sizeof(report))) {
#ifdef ESP_PLATFORM
        ESP_LOGI("podcast_ui","Visible text: %s labels=%u chars=%u cjk=%u missing=%u first=U+%04lx utf8=%u invisible=%u binding=%u",
          ok?"PASS":"FAIL",report.labels,report.characters,report.chinese,report.missing,
          (unsigned long)report.first_missing,report.invalid_utf8,report.invisible,report.wrong_font);
#else
        (void)ok;
#endif
        last_text_report=report;text_reported=true;
    }
}
static void inspect_text(const char *text,const lv_font_t *font,podcast_ui_text_report_t *r)
{
    uint32_t offset=0;
    while(text[offset]) {
        uint32_t before=offset,cp=lv_text_encoded_next(text,&offset);
        if(!cp||offset<=before){++r->invalid_utf8;if(offset<=before)break;continue;}
        if(cp<0x20)continue;
        ++r->characters;
        if(cp>=0x3400&&cp<=0x9fff)++r->chinese;
        lv_font_glyph_dsc_t glyph;
        if(!font||!lv_font_get_glyph_dsc(font,&glyph,cp,0)||glyph.is_placeholder) {
            if(!r->missing)r->first_missing=cp;
            ++r->missing;
        }
    }
}
static void inspect_tree(lv_obj_t *obj,podcast_ui_text_report_t *r)
{
    if(lv_obj_has_flag(obj,LV_OBJ_FLAG_HIDDEN))return;
    if(lv_obj_check_type(obj,&lv_label_class)) {
        const char *text=lv_label_get_text(obj);
        if(*text){++r->labels;if(!lv_obj_get_style_text_opa(obj,0))++r->invisible;inspect_text(text,lv_obj_get_style_text_font(obj,0),r);}
    }
    for(unsigned i=0;i<lv_obj_get_child_count(obj);++i)inspect_tree(lv_obj_get_child(obj,i),r);
}
bool podcast_ui_inspect_text(podcast_ui_text_report_t *r)
{
    if(!r||!screen)return false;
    memset(r,0,sizeof(*r));inspect_tree(screen,r);
    if(page==PODCAST_NOW) {
        const char *symbol=transport_symbol();
        inspect_text(symbol?symbol:key_middle,symbol?&lv_font_montserrat_20:&app_ui_16,r);
    }
    if(page==PODCAST_NOW)inspect_text("音量分钟小时秒时长待获取",&app_cjk_18,r);
    if(drawn_recent){inspect_text("最近收听最近更新",&app_ui_16,r);}
    lv_obj_t *full[]={heading,show,title,row_title[0],row_title[1],row_title[2]};
    for(unsigned i=0;i<sizeof(full)/sizeof(full[0]);++i)
        if(lv_obj_get_style_text_font(full[i],0)!=&app_cjk_18)++r->wrong_font;
    return !r->missing&&!r->invalid_utf8&&!r->invisible&&!r->wrong_font;
}
void podcast_ui_delete(void)
{
    podcast_dynamic_covers_clear_ui();
    cancel_help();help_visit=false;
    if(screen)lv_obj_delete(screen);
    screen=NULL;text_reported=false;key_middle="";
}
