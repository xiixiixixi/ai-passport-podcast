#include "podcast_ruler.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

int main(void)
{
    podcast_ruler_t r=podcast_ruler_make(1936,6215,12,228);
    assert(r.known&&r.unit==PODCAST_RULER_MINUTES&&r.major_seconds==1800&&r.minor_seconds==120);
    assert(r.pointer==79); /* 32:16 in 1:43:35, just past the true 30-minute mark. */
    assert(podcast_ruler_position(1800,6215,12,228)==75);
    assert(podcast_ruler_position(3600,6215,12,228)==137);
    assert(podcast_ruler_position(5400,6215,12,228)==200);
    assert(podcast_ruler_position(6215,6215,12,228)==228);
    r=podcast_ruler_make(22,45,12,228);assert(r.unit==PODCAST_RULER_SECONDS&&r.pointer==118&&r.major_seconds==20);
    r=podcast_ruler_make(3600,86400,12,228);assert(r.unit==PODCAST_RULER_HOURS&&r.major_seconds==21600&&r.pointer==21);
    assert(!podcast_ruler_make(100,0,12,228).known);
    const uint32_t durations[]={1,5,30,59,60,61,300,4380,6215,10800,28799,28800,92960,10000000,UINT32_MAX};
    for(unsigned i=0;i<sizeof(durations)/sizeof(durations[0]);i++){
        uint32_t total=durations[i];r=podcast_ruler_make(total,total,12,228);
        assert(r.known&&r.pointer==228&&r.major_seconds&&r.minor_seconds);
        assert((uint64_t)total/r.major_seconds<=4&&((uint64_t)total/r.minor_seconds)<=54);
        assert(r.major_seconds%r.unit_seconds==0&&r.minor_seconds%r.unit_seconds==0);
        unsigned previous=12;
        for(unsigned part=0;part<=100;part++){
            uint32_t at=(uint32_t)((uint64_t)total*part/100);
            unsigned p=podcast_ruler_position(at,total,12,228);assert(p>=previous&&p>=12&&p<=228);previous=p;
        }
        assert(podcast_ruler_position(UINT32_MAX,total,12,228)==228);
    }
    puts("Ruler: exact 32:16/1:43:35 positions, honest seconds/minutes/hours units, unknown duration, bounded truthful ticks, monotonic mapping and uint32 extremes PASS");
    return 0;
}
