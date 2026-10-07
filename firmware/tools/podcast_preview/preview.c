#include "lvgl.h"
#include "src/misc/lv_text_private.h"
#include "src/draw/lv_draw_buf_private.h"
#include "podcast_ui.h"
#include "podcast_covers.h"
#include "podcast_cover_dynamic.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "podcast_fonts.h"
/* Match the single 40-line buffer used by bsp_display_lvgl.c on the device. */
static uint16_t pixels[240*320], buffer[240*40];
static lv_obj_t *idle_screen;
static lv_draw_buf_malloc_cb_t font_malloc;
static lv_draw_buf_width_to_stride_cb_t font_stride;
static uint32_t requested_font_width;
static size_t max_font_request;
static bool sample_stress;
static unsigned stress_min_free,stress_min_largest,stress_frames;
static uint32_t measured_font_stride(uint32_t width,lv_color_format_t format) {
    requested_font_width=width;
    return font_stride?font_stride(width,format):LV_DRAW_BUF_STRIDE(width,format);
}
static void *measured_font_malloc(size_t size,lv_color_format_t format) {
    if(size>max_font_request)max_font_request=size;
    void *out=font_malloc(size,format);
    if(!out) {
        lv_mem_monitor_t mem;lv_mem_monitor(&mem);
        fprintf(stderr,"Font buffer allocation failed: width=%u request=%zu format=%u free=%u largest=%u peak=%u\n",requested_font_width,size,(unsigned)format,(unsigned)mem.free_size,(unsigned)mem.free_biggest_size,(unsigned)mem.max_used);
    }
    return out;
}
static unsigned check_decoded_text(const lv_font_t *font,const char *text) {
    lv_draw_buf_t *buffer=lv_draw_buf_create(32,32,LV_COLOR_FORMAT_A8,LV_STRIDE_AUTO);assert(buffer);
    uint32_t offset=0;unsigned checked=0;
    while(text[offset]) {
        uint32_t cp=lv_text_encoded_next(text,&offset);assert(cp);
        lv_font_glyph_dsc_t glyph;assert(lv_font_get_glyph_dsc(font,&glyph,cp,0)&&!glyph.is_placeholder);
        if(!glyph.box_w||!glyph.box_h)continue;
        assert(glyph.format==LV_FONT_GLYPH_FORMAT_A4&&glyph.box_w<=32&&glyph.box_h<=32);
        assert(lv_draw_buf_reshape(buffer,LV_COLOR_FORMAT_A8,glyph.box_w,glyph.box_h,LV_STRIDE_AUTO));
        glyph.req_raw_bitmap=1;const uint8_t *raw=font->get_glyph_bitmap(&glyph,NULL);assert(raw);
        glyph.req_raw_bitmap=0;const lv_draw_buf_t *out=lv_font_get_glyph_bitmap(&glyph,buffer);assert(out);
        unsigned ink=0;
        for(unsigned y=0;y<glyph.box_h;y++)for(unsigned x=0;x<glyph.box_w;x++) {
            unsigned p=y*glyph.box_w+x;uint8_t expected=((raw[p/2]>>((p&1)?0:4))&15)*17;
            uint8_t actual=out->data[y*out->header.stride+x];assert(actual==expected);ink+=actual;
        }
        assert(ink);checked++;
    }
    lv_draw_buf_destroy(buffer);return checked;
}
static void check_labels(lv_obj_t *obj) {
    if(lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if(lv_obj_check_type(obj, &lv_label_class)) {
        const lv_font_t *font=lv_obj_get_style_text_font(obj, LV_PART_MAIN);
        const char *p=lv_label_get_text(obj);
        while(*p) {
            uint32_t cp=lv_text_encoded_next(p,NULL), n=lv_text_encoded_size(p);
            lv_font_glyph_dsc_t glyph={0};
            if(cp>=0x20 && (!lv_font_get_glyph_dsc(font,&glyph,cp,0)||glyph.is_placeholder)) {
                fprintf(stderr,"Actual widget missing U+%04x in %s\n",cp,lv_label_get_text(obj));
                exit(4);
            }
            p+=n;
        }
    }
    for(uint32_t i=0;i<lv_obj_get_child_count(obj);i++)check_labels(lv_obj_get_child(obj,i));
}
static void check_screen_bounds(lv_obj_t *obj) {
    if(lv_obj_has_flag(obj,LV_OBJ_FLAG_HIDDEN))return;
    lv_area_t box;lv_obj_get_coords(obj,&box);
    assert(box.x1>=0&&box.y1>=0&&box.x2<240&&box.y2<320);
    for(uint32_t i=0;i<lv_obj_get_child_count(obj);i++)check_screen_bounds(lv_obj_get_child(obj,i));
}
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *data) {
    uint16_t *in=(uint16_t *)data;
    for(int y=a->y1;y<=a->y2;y++) for(int x=a->x1;x<=a->x2;x++) pixels[y*240+x]=*in++;
    lv_display_flush_ready(d);
}
/* Roles are attached to production widgets; tests do not depend on their
 * child ordering or on a mirror implementation of the layout. */
static lv_obj_t *find_role(lv_obj_t *obj,const char *role) {
    const char *actual=lv_obj_get_user_data(obj);
    if(actual&&!strcmp(actual,role))return obj;
    for(uint32_t i=0;i<lv_obj_get_child_count(obj);++i) {
        lv_obj_t *found=find_role(lv_obj_get_child(obj,i),role);if(found)return found;
    }
    return NULL;
}
static lv_obj_t *widget(const char *role) {
    lv_obj_t *obj=find_role(lv_screen_active(),role);assert(obj);return obj;
}
static void check_cover_pixel(const char *id,bool thumbnail,int x,int y) {
    const lv_image_dsc_t *image=podcast_cover_get(id,thumbnail);assert(image);
    assert(image->header.w==(thumbnail?48:112)&&image->header.h==(thumbnail?48:112));
    assert(image->header.cf==LV_COLOR_FORMAT_RGB565);
    assert(image->header.stride==image->header.w*2);
    assert((uintptr_t)image->data%4==0);
    assert(!(image->header.flags&(LV_IMAGE_FLAGS_ALLOCATED|LV_IMAGE_FLAGS_MODIFIABLE)));
    unsigned checked=0;
    for(unsigned py=0;py<image->header.h;py+=7)for(unsigned px=0;px<image->header.w;px+=7) {
        uint16_t expected;memcpy(&expected,image->data+py*image->header.stride+px*2,2);
        assert(pixels[(y+(int)py)*240+x+(int)px]==expected);checked++;
    }
    assert(checked>32);
}
static void check_player_cover(const char *id) {
    const lv_image_dsc_t *im=podcast_cover_get(id,false);assert(im&&im->header.w==112&&im->header.h==112);
    unsigned checked=0;
    for(unsigned y=4;y<48;y+=6)for(unsigned x=4;x<48;x+=6){
        unsigned sx=x*112/52,sy=y*112/52;uint16_t expected;memcpy(&expected,im->data+sy*im->header.stride+sx*2,2);
        bool uniform=true;
        for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++){uint16_t p;memcpy(&p,im->data+(sy+dy)*im->header.stride+(sx+dx)*2,2);if(p!=expected)uniform=false;}
        if(uniform){assert(pixels[(14+(int)y)*240+10+(int)x]==expected);checked++;}
    }
    assert(checked>=2);
}
static void check_public_cover(const char *id,int x,int y) {
    const lv_image_dsc_t *im=podcast_dynamic_cover_get(id);assert(im);
    assert(im->header.w==52&&im->header.h==52);
    for(unsigned py=0;py<52;py+=7)for(unsigned px=0;px<52;px+=7) {
        uint16_t expected;memcpy(&expected,im->data+py*im->header.stride+px*2,2);
        assert(pixels[(y+(int)py)*240+x+(int)px]==expected);
    }
}
static void render_view(podcast_view_t *v,bool trace) {
    podcast_ui_render(v);lv_obj_update_layout(lv_screen_active());check_labels(lv_screen_active());
    const char *keys=lv_label_get_text(widget("back_hint"));
    if(trace)printf("Context hint: %s (%s)\n",keys,lv_obj_has_flag(widget("back_hint"),LV_OBJ_FLAG_HIDDEN)?"hidden":"visible");
    if(!lv_obj_has_flag(widget("back_hint"),LV_OBJ_FLAG_HIDDEN)) {
        assert(*keys);lv_point_t text_size;
        lv_text_get_size(&text_size,keys,lv_obj_get_style_text_font(widget("back_hint"),0),0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
        assert(text_size.x<=lv_obj_get_width(widget("back_hint"))&&text_size.y<=lv_obj_get_height(widget("back_hint")));
    }
    podcast_ui_text_report_t report;assert(podcast_ui_inspect_text(&report));assert(report.chinese>0);
    check_screen_bounds(lv_screen_active());
    lv_mem_monitor_t draw_mem;lv_mem_monitor(&draw_mem);
    if(trace)printf("Pool before draw: free=%u largest=%u peak=%u\n",(unsigned)draw_mem.free_size,(unsigned)draw_mem.free_biggest_size,(unsigned)draw_mem.max_used);
    lv_refr_now(NULL);
    if(sample_stress){
        lv_mem_monitor_t m;lv_mem_monitor(&m);
        if(m.free_size<stress_min_free)stress_min_free=m.free_size;
        if(m.free_biggest_size<stress_min_largest)stress_min_largest=m.free_biggest_size;
        assert(m.free_size>=2048&&m.free_biggest_size>=1024);stress_frames++;
    }
}
static void save(const char *name, podcast_view_t *v) {
    printf("Rendering scenario: %s\n",name);
    render_view(v,true);
    char path[256];snprintf(path,sizeof(path),"%s.ppm",name);
    FILE *f=fopen(path,"wb");fprintf(f,"P6\n240 320\n255\n");
    for(int i=0;i<240*320;i++){uint16_t c=pixels[i];unsigned char p[]={((c>>11)&31)*255/31,((c>>5)&63)*255/63,(c&31)*255/31};fwrite(p,1,3,f);}fclose(f);
}
static unsigned ink(int x,int y,int w,int h) {
    unsigned n=0;for(int py=y;py<y+h;++py)for(int px=x;px<x+w;++px)n+=pixels[py*240+px]!=pixels[0];return n;
}
static uint32_t patch_hash(int x,int y,int w,int h) {
    uint32_t hash=2166136261u;
    for(int py=y;py<y+h;++py)for(int px=x;px<x+w;++px){hash^=pixels[py*240+px];hash*=16777619u;}return hash;
}
static void assert_heading_fits(const char *text) {
    assert(!strcmp(lv_label_get_text(widget("heading")),text));lv_point_t size;
    lv_text_get_size(&size,text,lv_obj_get_style_text_font(widget("heading"),0),0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    assert(size.x<=lv_obj_get_width(widget("heading")));
}
static unsigned timer_count(void) {
    unsigned count=0;
    for(lv_timer_t *t=lv_timer_get_next(NULL);t;t=lv_timer_get_next(t))count++;
    return count;
}
static void advance_clock(unsigned milliseconds) {
    lv_tick_inc(milliseconds);lv_timer_handler();
}
static bool help_visible(void) {
    return !lv_obj_has_flag(widget("back_hint"),LV_OBJ_FLAG_HIDDEN);
}
/* Drive the production widgets and actual LVGL timer list with virtual ticks.
 * No timer/help-state implementation is copied into this harness. */
static void check_help_lifecycle(podcast_view_t original) {
    podcast_view_t v=original;
    unsigned baseline=timer_count();
    assert(help_visible()&&!strcmp(lv_label_get_text(widget("back_hint")),"长按侧键调进度"));
    advance_clock(4000);render_view(&v,false);
    assert(timer_count()==baseline&&help_visible());

    v.page=PODCAST_ACTIONS;v.count=3;strcpy(v.heading,"播放操作");
    strcpy(v.rows[0].title,"节目库");strcpy(v.rows[0].detail,"按最近更新排序");
    strcpy(v.rows[1].title,"调整进度");strcpy(v.rows[1].detail,"先选位置，再确定跳转");
    strcpy(v.rows[2].title,"睡眠定时");strcpy(v.rows[2].detail,"到时间暂停，进度会保存");
    save("hints-menu-enter",&v);assert(help_visible()&&timer_count()==baseline+1);
    assert(strchr(lv_label_get_text(widget("back_hint")),'\n'));
    advance_clock(1000);v.selected=1;v.battery_percent=80;render_view(&v,false);
    advance_clock(1999);render_view(&v,false);assert(help_visible());
    advance_clock(1);assert(help_visible()&&timer_count()==baseline&&!strcmp(lv_label_get_text(widget("back_hint")),"长按确定回到播放"));
    save("hints-menu-after-three-seconds",&v);
    v.selected=2;render_view(&v,false);assert(help_visible()&&timer_count()==baseline&&!strchr(lv_label_get_text(widget("back_hint")),'\n'));
    puts("Help 3000ms expiry / same-page selection-battery redraw does not renew PASS");

    v.page=PODCAST_SEEK;v.seek_target=1936;save("hints-seek-enter",&v);
    assert(help_visible()&&timer_count()==baseline+1);
    advance_clock(3000);save("hints-seek-after-three-seconds",&v);
    assert(!strcmp(lv_label_get_text(widget("back_hint")),"确定应用 · 长按确定取消"));
    v.seek_target+=15;render_view(&v,false);assert(timer_count()==baseline);

    v.page=PODCAST_ACTIONS;render_view(&v,false);assert(timer_count()==baseline+1);
    advance_clock(1000);v.page=PODCAST_SLEEP;strcpy(v.heading,"睡眠定时");
    strcpy(v.rows[0].title,"关闭定时");strcpy(v.rows[1].title,"15分钟后暂停");strcpy(v.rows[2].title,"30分钟后暂停");
    save("hints-sleep-enter",&v);
    advance_clock(2000);assert(help_visible()&&timer_count()==baseline+1);
    advance_clock(1000);save("hints-sleep-after-three-seconds",&v);
    assert(help_visible()&&timer_count()==baseline&&!strcmp(lv_label_get_text(widget("back_hint")),"长按确定回到播放"));
    puts("Help per-entry reset / leaving cancels previous deadline PASS");

    v.page=PODCAST_EPISODES;v.count=0;strcpy(v.status,"正在读取节目");
    save("hints-episodes-loading",&v);assert(!help_visible()&&timer_count()==baseline);
    advance_clock(4000);render_view(&v,false);assert(!help_visible()&&timer_count()==baseline);
    v.count=3;strcpy(v.status,"最新更新在前");
    strcpy(v.rows[0].title,"一杯茶里的中国与世界");strcpy(v.rows[1].title,"这些看似平常的生活里藏着什么");strcpy(v.rows[2].title,"一个普通家庭的二十年");
    save("hints-episodes-first-ready",&v);assert(help_visible()&&timer_count()==baseline+1);
    advance_clock(3000);render_view(&v,false);assert(!help_visible()&&timer_count()==baseline);
    puts("Loading list waits for first usable content / later redraw does not reopen PASS");

    v.page=PODCAST_SHOWS;v.count=0;v.has_recent=false;
    render_view(&v,false);advance_clock(4000);assert(!help_visible()&&timer_count()==baseline);
    v.has_recent=true;strcpy(v.recent_show_id,"huzuoyou");strcpy(v.recent_name,"忽左忽右");
    strcpy(v.recent_title,"一杯茶里的中国与世界");v.recent_elapsed=1936;v.recent_duration=6215;
    save("hints-recent-only-first-ready",&v);assert(help_visible()&&timer_count()==baseline+1);
    assert(!strchr(lv_label_get_text(widget("back_hint")),'\n'));
    v.count=2;
    for(unsigned i=0;i<2;i++){strcpy(v.rows[i].show_id,"zhangxiaojun");strcpy(v.rows[i].title,"张小珺Jùn｜商业访谈录");strcpy(v.rows[i].detail,"听过 2集 · 听完 1集");}
    save("hints-recent-two-line-shows",&v);
    advance_clock(3000);save("hints-recent-after-three-seconds",&v);
    assert(!help_visible()&&timer_count()==baseline);

    v.page=PODCAST_EPISODES;v.count=0;v.has_recent=false;
    render_view(&v,false);advance_clock(1000);
    v=original;render_view(&v,false);advance_clock(4000);
    assert(help_visible()&&timer_count()==baseline&&!strcmp(lv_label_get_text(widget("back_hint")),"长按侧键调进度"));
    puts("Leaving an unready list cancels deferred help PASS");

    v.page=PODCAST_VOLUME;strcpy(v.heading,"调整音量");save("hints-volume-enter",&v);
    advance_clock(3000);save("hints-volume-after-three-seconds",&v);
    assert(timer_count()==baseline&&!strcmp(lv_label_get_text(widget("back_hint")),"确定返回"));

    v.page=PODCAST_ACTIONS;render_view(&v,false);advance_clock(1000);
    /* The app host owns the next active screen. LVGL 9.5's refresh timer
     * updates layout before its NULL-active-screen guard, so keep that real
     * platform responsibility in this lifecycle test. */
    lv_screen_load(idle_screen);podcast_ui_delete();assert(timer_count()==baseline);
    advance_clock(1000);podcast_ui_create();render_view(&v,false);
    assert(timer_count()==baseline+1&&help_visible());
    advance_clock(1000);assert(help_visible()); /* Former deleted deadline. */
    advance_clock(2000);assert(help_visible()&&timer_count()==baseline&&!strcmp(lv_label_get_text(widget("back_hint")),"长按确定回到播放"));
    lv_screen_load(idle_screen);podcast_ui_delete();podcast_ui_delete();advance_clock(4000);assert(timer_count()==baseline);
    podcast_ui_create();render_view(&original,false);
    assert(timer_count()==baseline&&help_visible());
    save("hints-playback-final",&original);
    puts("Delete active timer / recreate rejects old deadline / double delete / no late callbacks PASS");
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);lv_init();
    lv_draw_buf_handlers_t *fh=lv_draw_buf_get_font_handlers(),*dh=lv_draw_buf_get_handlers();
    font_malloc=fh->buf_malloc_cb;font_stride=dh->width_to_stride_cb;
    assert(font_malloc);fh->buf_malloc_cb=measured_font_malloc;dh->width_to_stride_cb=measured_font_stride;
    uint32_t hash=0;assert(podcast_fonts_probe(&hash));printf("Font bitmap probe checksum: %08x\n",hash);
    lv_display_t *d=lv_display_create(240,320);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
    podcast_font_render_report_t drawn;assert(podcast_fonts_render_probe(&drawn));
    printf("A8/RGB565 glyph drawing PASS checksum=%08x\n",drawn.rendered_checksum);idle_screen=lv_screen_active();podcast_ui_create();
    /* Publication exports use the production UI and deliberately fictional copy.
     * They are host-rendered screen images, not photographs or device captures. */
    if(argc>1&&!strcmp(argv[1],"--publication")) {
        const char *public_ids[]={"gushifen","huzuoyou","buheshiyi"};
        static uint8_t sample_wires[3][PODCAST_COVER_WIRE_BYTES];
        podcast_dynamic_covers_want(public_ids,3);
        for(unsigned i=0;i<3;i++) {
            char path[256];snprintf(path,sizeof(path),"%s/neutral-%s-52.pdc1",
                argc>2?argv[2]:"neutral-covers",public_ids[i]);
            FILE *f=fopen(path,"rb");assert(f);
            size_t n=fread(sample_wires[i],1,sizeof(sample_wires[i]),f);
            assert(n==sizeof(sample_wires[i])&&!ferror(f)&&fgetc(f)==EOF);fclose(f);
            assert(podcast_dynamic_covers_host_adopt(public_ids[i],sample_wires[i],n));
        }
        (void)podcast_dynamic_covers_ui_poll();
        podcast_view_t p={.page=PODCAST_NOW,.battery_percent=85,.elapsed=1936,
            .duration=3800,.volume=55,.playing=true,.auto_next=true,.has_next=true,
            .next_position=2,.next_total=3};
        strcpy(p.show_id,"gushifen");strcpy(p.show,"日常电台");
        strcpy(p.title,"一段声音，\n陪你慢慢走");strcpy(p.next_title,"去听城市里的声音");
        strcpy(p.status,"正在播放");strcpy(p.back_hint,"确定暂停 长按菜单");
        save("player-native",&p);check_public_cover(p.show_id,10,14);
        p=(podcast_view_t){.page=PODCAST_SHOWS,.count=3,.selected=0,.battery_percent=85,.total=3};
        const char *public_names[]={"日常电台","慢慢聊","声音笔记"};
        const char *public_dates[]={"2026-10-07","2026-10-06","2026-10-05"};
        strcpy(p.status,"最新更新在前");strcpy(p.hint,"上下选择 确定进入");
        strcpy(p.back_hint,"长确定回到播放");
        for(unsigned i=0;i<3;i++) {
            strcpy(p.rows[i].show_id,public_ids[i]);strcpy(p.rows[i].title,public_names[i]);
            strcpy(p.rows[i].latest_date,public_dates[i]);
        }
        save("library-native",&p);check_public_cover(public_ids[0],12,68);
        puts("Publication production-UI exports / fictional sample copy PASS");
        return 0;
    }
    podcast_view_t v={.page=PODCAST_SHOWS,.count=3,.selected=0,.battery_percent=85,.total=11};
    v.page=PODCAST_NOW;v.elapsed=1936;v.duration=6215;v.volume=55;v.playing=true;
    strcpy(v.show_id,"huzuoyou");strcpy(v.show,"忽左忽右");strcpy(v.title,"一杯茶里的\n中国与世界");
    strcpy(v.status,"正在播放");strcpy(v.back_hint,"确定暂停 长按菜单");
    save("selected-same-content",&v);
    assert(lv_obj_get_style_text_font(widget("elapsed"),0)==&lv_font_montserrat_48);
    assert(pixels[180*240+79]==lv_color_to_u16(lv_color_hex(0x0063F2)));
    if(argc>1&&!strcmp(argv[1],"--keyframes")){
        strcpy(v.title,"153. 和曾鸣聊产业史观：残酷的真相、会消亡的公司、优秀≠卓越");save("key-long-title",&v);
        v.elapsed=3600;v.duration=36000;save("key-one-hour-ten-hours",&v);
        v.elapsed=36001;v.duration=10000000;save("key-very-long-ruler",&v);
        v.elapsed=UINT32_MAX;v.duration=UINT32_MAX;save("key-uint32-duration",&v);assert(!strcmp(lv_label_get_text(widget("value")),"1193046:28:15"));
        v.elapsed=754;v.duration=0;save("key-unknown-duration",&v);
        v.elapsed=22;v.duration=45;save("key-short-clip",&v);
        v.elapsed=1936;v.duration=6215;v.playing=false;strcpy(v.status,"已暂停，确定接着听");strcpy(v.back_hint,"确定续播 长按菜单");save("key-paused",&v);
        strcpy(v.status,"正在缓冲");strcpy(v.back_hint,"确定暂停 长按菜单");save("key-buffering",&v);
        strcpy(v.status,"网络音频中断");strcpy(v.back_hint,"确定重试 长按菜单");save("key-error",&v);
        return 0;
    }
    if(argc>1&&!strcmp(argv[1],"--selected-only")){
        lv_mem_monitor_t m;lv_mem_monitor(&m);
        printf("Selected frame 24KiB pool: peak=%u free=%u largest=%u\n",(unsigned)m.max_used,(unsigned)m.free_size,(unsigned)m.free_biggest_size);
        return 0;
    }
    check_help_lifecycle(v);
    if(argc>1&&!strcmp(argv[1],"--hints-only")){
        lv_mem_monitor_t m;lv_mem_monitor(&m);
        printf("Hint lifecycle 24KiB pool: peak=%u free=%u largest=%u\n",(unsigned)m.max_used,(unsigned)m.free_size,(unsigned)m.free_biggest_size);
        return 0;
    }
    v=(podcast_view_t){.page=PODCAST_SHOWS,.count=3,.selected=0,.battery_percent=85,.total=11};
    const char *ids[]={"gushifen","huzuoyou","buheshiyi","guixian101","yeelok","tianzhen","lidan","zhangxiaojun","luoyonghao","wechat-talk","thatisbiz"};
    const char *names[]={"故事FM","忽左忽右","不合时宜","硅谷101","怡楽播客","天真不天真","李诞","张小珺Jùn｜商业访谈录","罗永浩的十字路口","微信公开TALK","商业就是这样"};
    strcpy(v.status,"最新更新在前");strcpy(v.hint,"上下选择 确定进入");strcpy(v.back_hint,"长确定回到播放");
    strcpy(v.show,"故事FM");
    for(unsigned i=0;i<3;i++){strcpy(v.rows[i].show_id,ids[i]);strcpy(v.rows[i].title,names[i]);strcpy(v.rows[i].latest_date,i==0?"2026-10-03":i==1?"2026-10-02":"2026-10-01");}
    save("shows",&v);check_cover_pixel(ids[0],true,12,68);
    for(unsigned i=0;i<3;i++){strcpy(v.rows[i].show_id,ids[i+7]);strcpy(v.rows[i].title,names[i+7]);}
    v.absolute_selected=7;save("shows-new",&v);
    v.has_recent=true;v.count=2;strcpy(v.recent_show_id,"huzuoyou");strcpy(v.recent_name,"忽左忽右");strcpy(v.recent_title,"505. 茶海轶闻：中国与世界");v.recent_elapsed=1936;v.recent_duration=6215;save("shows-recent",&v);check_cover_pixel("huzuoyou",true,12,65);assert(!lv_obj_has_flag(widget("show"),LV_OBJ_FLAG_HIDDEN));assert(strstr(lv_label_get_text(widget("back_hint")),"续听"));
    v.selected=-1;save("shows-recent-selected",&v);
    assert(pixels[60*240+8]==lv_color_to_u16(lv_color_hex(0x0063F2)));
    assert(lv_color_to_u16(lv_obj_get_style_text_color(widget("show"),0))==lv_color_to_u16(lv_color_hex(0x0063F2)));
    assert(lv_color_to_u16(lv_obj_get_style_text_color(widget("row_title_0"),0))==lv_color_to_u16(lv_color_hex(0x111111)));
    v.selected=0;save("shows-recent-first-show-selected",&v);
    assert(pixels[60*240+8]==lv_color_to_u16(lv_color_hex(0xEDF4FF)));
    assert(lv_color_to_u16(lv_obj_get_style_text_color(widget("show"),0))==lv_color_to_u16(lv_color_hex(0x111111)));
    assert(lv_color_to_u16(lv_obj_get_style_text_color(widget("row_title_0"),0))==lv_color_to_u16(lv_color_hex(0x0063F2)));
    puts("Recent card/first-show focus swaps its actual blue marker and label colors PASS");
    v.has_recent=false;v.count=3;
    v.page=PODCAST_EPISODES;v.total=993;v.absolute_selected=1;v.selected=1;
    strcpy(v.show_id,"gushifen");strcpy(v.show,"故事FM");strcpy(v.hint,"上下选择 确定播放");
    strcpy(v.rows[0].title,"993. 我和朋友们在遥远的地方重逢");strcpy(v.rows[0].detail,"2026-10-03 58分 未播");
    strcpy(v.rows[1].title,"992. 这些看似平常的生活里藏着什么");strcpy(v.rows[1].detail,"2026-10-01 72分 听过");
    strcpy(v.rows[2].title,"991. 一个普通家庭的二十年");strcpy(v.rows[2].detail,"2026-09-29 34分 听完");
    save("episodes",&v);check_cover_pixel(v.show_id,true,12,36);
    v.oldest_first=true;v.absolute_selected=991;save("episodes-oldest",&v);assert(strstr(lv_label_get_text(widget("status")),"旧到新"));
    v.page=PODCAST_NOW;v.elapsed=754;v.duration=4380;v.volume=55;v.oldest_first=false;v.auto_next=true;v.has_next=true;v.next_position=3;v.next_total=993;
    strcpy(v.title,"992. 这些看似平常的生活里藏着什么：普通人的二十年");strcpy(v.next_title,"991. 一个普通家庭的二十年");
    strcpy(v.status,"正在播放");strcpy(v.back_hint,"确定暂停 长按菜单");v.playing=true;save("playing",&v);
    check_player_cover(v.show_id);assert(!strcmp(lv_label_get_text(widget("volume")),"55"));
    assert(!strcmp(lv_label_get_text(widget("elapsed")),"12:34"));assert(lv_obj_has_flag(widget("status"),LV_OBJ_FLAG_HIDDEN));
    assert(ink(46,244,66,36)>60);assert(ink(10,104,218,53)>100);
    podcast_view_t before_late=v;
    strcpy(v.show_id,"src_5f90a6b1eacd7a9c");strcpy(v.show,"晚点聊 LateTalk");
    strcpy(v.title,"183. 与Henry的AI季报：个人助理与机器人");
    save("late-talk-playing",&v);check_player_cover(v.show_id);
    assert(podcast_cover_get(v.show_id,false)!=podcast_cover_get("unknown-source",false));
    v=before_late;save("playing-restored",&v);
    uint32_t paused_action=patch_hash(10,85,21,25);
    v.playing=false;strcpy(v.status,"已暂停，确定接着听");strcpy(v.back_hint,"确定续播 长按菜单");save("paused",&v);
    assert_heading_fits("已暂停，确定续播");
    assert(paused_action!=patch_hash(10,85,21,25));
    v.resuming=true;strcpy(v.status,"正在同步续播，再按取消");save("central-resume-wait",&v);
    assert(strstr(lv_label_get_text(widget("heading")),"再按取消")&&paused_action==patch_hash(10,85,21,25));
    strcpy(v.status,"离线续播，进度待同步");save("offline-resume",&v);assert(strstr(lv_label_get_text(widget("heading")),"待同步"));
    v.resuming=false;strcpy(v.status,"进度已更新，请再按确定续播");save("central-resume-stale",&v);assert(strstr(lv_label_get_text(widget("heading")),"进度更新"));
    strcpy(v.status,"正在缓冲");strcpy(v.back_hint,"确定暂停 长按菜单");save("buffering",&v);
    assert(paused_action==patch_hash(10,85,21,25));
    v.playing=true;v.duration=9296;v.sleep_minutes=30;v.elapsed=3754;
    strcpy(v.show_id,"zhangxiaojun");strcpy(v.show,names[7]);strcpy(v.status,"正在播放");
    strcpy(v.title,"153. 和曾鸣聊产业史观：残酷的真相、会消亡的公司、优秀≠卓越");
    strcpy(v.next_title,"152. 从平台到原生时代：公司怎样找到下一条路");save("long-title-sleep",&v);
    assert(strstr(lv_label_get_text(widget("heading")),"30分后停"));assert(!strcmp(lv_label_get_text(widget("elapsed")),"1:02:34"));
    v.elapsed=37554;v.duration=92960;save("long-clock",&v);assert(!strcmp(lv_label_get_text(widget("elapsed")),"10:25:54"));
    v.elapsed=3754;v.duration=9296;
    v.oldest_first=true;strcpy(v.next_title,"154. 新技术出现以后，我们如何重新理解商业");save("next-oldest",&v);
    v.auto_next=false;save("auto-next-off",&v);assert(lv_obj_has_flag(widget("status"),LV_OBJ_FLAG_HIDDEN));
    v.has_next=false;v.next_position=994;save("last-episode",&v);
    v.next_position=3;save("next-awaiting",&v);
    v.has_next=true;v.auto_next=true;
    for(unsigned level=0;level<=100;level+=50){v.volume=level;char f[32];snprintf(f,sizeof(f),"volume-%u",level);save(f,&v);}
    v.volume=55;v.page=PODCAST_SEEK;v.seek_target=3784;strcpy(v.back_hint,"长确定取消");save("seek",&v);
    assert(!strcmp(lv_label_get_text(widget("elapsed")),"1:03:04"));assert(strstr(lv_label_get_text(widget("back_hint")),"取消"));
    v.seek_target=0;save("seek-start",&v);v.seek_target=v.duration;save("seek-end",&v);
    v.page=PODCAST_VOLUME;strcpy(v.heading,"调整音量");strcpy(v.title,"侧键直接调整音量");save("volume",&v);
    v.page=PODCAST_ACTIONS;v.total=8;v.absolute_selected=0;v.count=3;v.selected=0;strcpy(v.heading,"播放操作 1/8");
    strcpy(v.rows[0].title,"节目库");strcpy(v.rows[0].detail,"按最近更新排序");strcpy(v.rows[1].title,"找单集");strcpy(v.rows[1].detail,"定位正在播放的单集");strcpy(v.rows[2].title,"下一集");strcpy(v.rows[2].detail,"按当前时间方向接着听");
    strcpy(v.hint,"上下选择 确定执行");strcpy(v.back_hint,"长确定回到播放");save("actions",&v);
    strcpy(v.heading,"播放操作 4/8");strcpy(v.rows[0].title,"上一集");strcpy(v.rows[0].detail,"按相反时间方向选择");strcpy(v.rows[1].title,"调整进度");strcpy(v.rows[1].detail,"先选位置，再确定跳转");strcpy(v.rows[2].title,"连续播放：开启");strcpy(v.rows[2].detail,"已开启，到列表末尾停止");save("actions-more",&v);
    v.page=PODCAST_SLEEP;strcpy(v.heading,"睡眠定时 2/4");v.selected=1;strcpy(v.status,"到时间暂停，进度会保存");
    strcpy(v.rows[0].title,"关闭定时");strcpy(v.rows[0].detail,"");strcpy(v.rows[1].title,"15分钟后暂停");strcpy(v.rows[1].detail,"");strcpy(v.rows[2].title,"30分钟后暂停");strcpy(v.rows[2].detail,"");
    strcpy(v.hint,"上下选择 确定设置");save("sleep",&v);
    v.page=PODCAST_NOW;strcpy(v.title,"153. 和曾鸣聊产业史观：残酷的真相、会消亡的公司、优秀≠卓越");v.busy=true;v.playing=false;v.elapsed=0;strcpy(v.status,"正在准备音频");strcpy(v.back_hint,"长确定菜单");save("preparing",&v);
    v.busy=false;strcpy(v.status,"网络音频中断");strcpy(v.back_hint,"确定重试 长按菜单");save("error",&v);
    assert_heading_fits("播放中断，确定重试");
    strcpy(v.status,"本集已播完");strcpy(v.back_hint,"确定重播 长按菜单");v.elapsed=v.duration;save("finished",&v);
    assert_heading_fits("本集播完，确定重播");
    strcpy(v.status,"正在播放");strcpy(v.back_hint,"确定暂停 长按菜单");v.playing=true;v.elapsed=754;v.sleep_minutes=0;
    for(unsigned i=0;i<11;i++) {
        strcpy(v.show_id,ids[i]);strcpy(v.show,names[i]);char f[64];snprintf(f,sizeof(f),"cover-%s",ids[i]);save(f,&v);check_player_cover(ids[i]);
    }
    puts("11 stable-ID native covers displayed PASS");
    strcpy(v.show_id,"unknown-id");strcpy(v.show,"尚未加入的新节目");save("cover-unknown",&v);check_player_cover(v.show_id);
    v.show_id[0]=0;save("cover-empty-id",&v);check_player_cover(v.show_id);
    v.battery_percent=-1;save("unknown-battery",&v);assert(!strcmp(lv_label_get_text(widget("battery")),"--"));v.battery_percent=85;
    if(argc>2){FILE *f=fopen(argv[2],"rb");assert(f);size_t n=fread(v.title,1,240,f);assert(n&&!ferror(f)&&fgetc(f)==EOF);fclose(f);v.title[n]=0;save("longest-real-title",&v);}
    v.duration=0;v.elapsed=754;save("unknown-duration",&v);assert(!strcmp(lv_label_get_text(widget("value")),"--:--"));
    v.duration=45;v.elapsed=22;save("short-duration",&v);
    v.duration=36000;v.elapsed=3600;save("one-hour-ten-hours",&v);assert(lv_obj_get_style_text_font(widget("elapsed"),0)==&lv_font_montserrat_48);
    v.duration=UINT32_MAX;v.elapsed=UINT32_MAX;save("uint32-duration",&v);assert(!strcmp(lv_label_get_text(widget("value")),"1193046:28:15"));
    v.duration=6215;v.elapsed=1936;
    const char *words="一个普通家庭的二十年我们如何面对生活中的改变与选择";
    for(unsigned i=0;i<80;i++)memcpy(v.title+i*3,words+(i%(strlen(words)/3))*3,3);v.title[240]=0;save("maximum-title-boundary",&v);
    stress_min_free=UINT32_MAX;stress_min_largest=UINT32_MAX;stress_frames=0;sample_stress=true;
    for(unsigned round=0;round<32;round++) {
        v.page=PODCAST_NOW;v.volume=round%3==0?0:round%3==1?55:100;render_view(&v,false);
        v.page=PODCAST_SEEK;v.seek_target=round*240;render_view(&v,false);
        v.page=PODCAST_EPISODES;v.selected=round%3;render_view(&v,false);
        v.page=PODCAST_VOLUME;render_view(&v,false);
    }
    sample_stress=false;assert(stress_frames==128);
    printf("24KiB/32-round player-seek-list-volume stress PASS frames=%u min_free=%u min_largest=%u\n",stress_frames,stress_min_free,stress_min_largest);
    v.page=PODCAST_NOW;
    if(argc>1) {
        FILE *f=fopen(argv[1],"rb");assert(f);char corpus[16384];size_t n=fread(corpus,1,sizeof(corpus)-1,f);assert(!ferror(f)&&feof(f));fclose(f);corpus[n]=0;
        uint32_t offset=0;unsigned checked=0,decoded=0;
        while(corpus[offset]){uint32_t before=offset,cp=lv_text_encoded_next(corpus,&offset);assert(cp&&offset>before);if(cp<0x20)continue;
            memset(v.title,0,sizeof(v.title));memcpy(v.title,corpus+before,offset-before);podcast_ui_render(&v);
            podcast_ui_text_report_t r;assert(podcast_ui_inspect_text(&r));decoded+=check_decoded_text(&app_cjk_18,lv_label_get_text(widget("title")));checked++;
        }
        printf("Actual title corpus PASS chars=%u pixel-decoded=%u\n",checked,decoded);
    }
    strcpy(v.title,"ǎ ι 〇 の ん ︱ ｜ ≠ ⚠️ ～ ✖️ ￼");save("real-title-symbols",&v);
    assert(!strcmp(lv_label_get_text(widget("title")),"ǎ ι 〇 の ん ︱ ｜ ≠ ⚠ ～ × [图]"));
    podcast_ui_text_report_t r;
    strcpy(v.title,"\xf0\x9f\xa6\x84");podcast_ui_render(&v);assert(!podcast_ui_inspect_text(&r)&&r.missing==1);
    strcpy(v.title,"\xe4");podcast_ui_render(&v);assert(!podcast_ui_inspect_text(&r)&&r.invalid_utf8==1);
    strcpy(v.title,"一个普通家庭的二十年");podcast_ui_render(&v);lv_obj_set_style_text_font(widget("heading"),&lv_font_montserrat_20,0);
    assert(!podcast_ui_inspect_text(&r)&&r.wrong_font==1&&r.missing>0);lv_obj_set_style_text_font(widget("heading"),&app_cjk_18,0);
    lv_obj_set_style_text_opa(widget("heading"),LV_OPA_TRANSP,0);assert(!podcast_ui_inspect_text(&r)&&r.invisible==1);lv_obj_set_style_text_opa(widget("heading"),LV_OPA_COVER,0);
    assert(podcast_ui_inspect_text(&r));puts("Missing/invalid/wrong-font/invisible negative probes PASS");
    printf("Font draw largest individual allocation=%zu bytes; display RGB565 40-row buffer=%zu bytes outside LVGL pool; full host framebuffer=%zu bytes is preview-only\n",max_font_request,sizeof(buffer),sizeof(pixels));
    lv_mem_monitor_t m;lv_mem_monitor(&m);printf("Final pool used=%u peak=%u free=%u largest=%u\n",(unsigned)(m.total_size-m.free_size),(unsigned)m.max_used,(unsigned)m.free_size,(unsigned)m.free_biggest_size);
    return 0;
}
