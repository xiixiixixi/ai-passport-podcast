#pragma once
#include <stdbool.h>
#include <stdint.h>

/* A bounded physical ruler: every tick and pointer uses the actual duration.
 * Unit changes keep both short clips and multi-hour episodes readable. */
typedef enum { PODCAST_RULER_SECONDS, PODCAST_RULER_MINUTES, PODCAST_RULER_HOURS } podcast_ruler_unit_t;
typedef struct {
    bool known;
    uint32_t duration, major_seconds, minor_seconds, unit_seconds;
    podcast_ruler_unit_t unit;
    unsigned left, right, pointer;
} podcast_ruler_t;
static inline uint32_t podcast_ruler_nice(uint32_t minimum)
{
    const unsigned choices[]={1,2,3,5,6,10};
    uint32_t scale=1;
    while((uint64_t)scale*10<minimum)scale*=10;
    for(unsigned i=0;i<sizeof(choices)/sizeof(choices[0]);i++){
        uint64_t value=(uint64_t)scale*choices[i];
        if(value>=minimum)return (uint32_t)value;
    }
    return scale;
}
static inline unsigned podcast_ruler_position(uint32_t seconds,uint32_t duration,unsigned left,unsigned right)
{
    if(!duration||right<left)return left;
    if(seconds>duration)seconds=duration;
    return left+(unsigned)(((uint64_t)seconds*(right-left)+duration/2)/duration);
}
static inline podcast_ruler_t podcast_ruler_make(uint32_t elapsed,uint32_t duration,unsigned left,unsigned right)
{
    podcast_ruler_t r={.known=duration>0,.duration=duration,.left=left,.right=right,.pointer=left};
    if(!duration)return r;
    r.unit=duration<60?PODCAST_RULER_SECONDS:duration>=28800?PODCAST_RULER_HOURS:PODCAST_RULER_MINUTES;
    r.unit_seconds=r.unit==PODCAST_RULER_SECONDS?1:r.unit==PODCAST_RULER_HOURS?3600:60;
    uint32_t major=(uint32_t)(((uint64_t)duration+r.unit_seconds*4-1)/(r.unit_seconds*4));
    uint32_t minor=(uint32_t)(((uint64_t)duration+r.unit_seconds*54-1)/(r.unit_seconds*54));
    r.major_seconds=podcast_ruler_nice(major)*r.unit_seconds;
    r.minor_seconds=podcast_ruler_nice(minor)*r.unit_seconds;
    r.pointer=podcast_ruler_position(elapsed,duration,left,right);
    return r;
}
