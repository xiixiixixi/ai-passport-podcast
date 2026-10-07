#include "podcast_cover_wire.h"
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint32_t crc32(const uint8_t *p,size_t n)
{
    uint32_t c=UINT32_MAX;
    while(n--){c^=*p++;for(unsigned bit=0;bit<8;bit++)c=(c>>1)^(UINT32_C(0xedb88320)&(0U-(c&1U)));}
    return ~c;
}
static uint32_t rotate(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
/* Fixed-size SHA256 verification keeps this protocol usable in host tests as
 * well as on ESP-IDF, without decoder libraries or a dynamically sized image. */
static void sha256(const uint8_t *data,size_t n,uint8_t out[32])
{
    static const uint32_t k[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t padded=(n+9+63)/64*64;
    for(size_t offset=0;offset<padded;offset+=64){
        uint32_t w[64];
        for(unsigned i=0;i<16;i++){
            uint32_t v=0;
            for(unsigned b=0;b<4;b++){
                size_t at=offset+i*4+b;uint8_t c=0;
                if(at<n)c=data[at];else if(at==n)c=0x80;
                else if(at>=padded-8)c=(uint8_t)(((uint64_t)n*8)>>(8*(padded-1-at)));
                v=(v<<8)|c;
            }
            w[i]=v;
        }
        for(unsigned i=16;i<64;i++){
            uint32_t a=w[i-15],b=w[i-2];
            w[i]=w[i-16]+(rotate(a,7)^rotate(a,18)^(a>>3))+w[i-7]+(rotate(b,17)^rotate(b,19)^(b>>10));
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],t=h[7];
        for(unsigned i=0;i<64;i++){
            uint32_t one=t+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            uint32_t two=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
            t=g;g=f;f=e;e=d+one;d=c;c=b;b=a;a=one+two;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=t;
    }
    for(unsigned i=0;i<8;i++)for(unsigned b=0;b<4;b++)out[i*4+b]=(uint8_t)(h[i]>>(24-8*b));
}
bool podcast_cover_id_valid(const char *id)
{
    if(!id)return false;size_t n=strlen(id);if(!n||n>=24)return false;
    for(size_t i=0;i<n;i++)if(!((id[i]>='a'&&id[i]<='z')||(id[i]>='A'&&id[i]<='Z')||
        (id[i]>='0'&&id[i]<='9')||id[i]=='-'||id[i]=='_'))return false;
    return true;
}
bool podcast_cover_wire_valid(const uint8_t *wire,size_t size)
{
    if(!wire||size!=PODCAST_COVER_WIRE_BYTES||memcmp(wire,"PDC1",4)||wire[4]!=52||wire[5]||wire[6]!=52||wire[7]||
        le32(wire+8)!=PODCAST_COVER_PIXEL_BYTES||le32(wire+12)!=crc32(wire+32,PODCAST_COVER_PIXEL_BYTES))return false;
    uint8_t hash[32];sha256(wire+32,PODCAST_COVER_PIXEL_BYTES,hash);
    return !memcmp(hash,wire+16,16);
}
void podcast_cover_native_pixels(uint8_t *wire)
{
    uint8_t *pixels=wire+PODCAST_COVER_HEADER_BYTES;
    for(unsigned y=0;y<PODCAST_COVER_NATIVE_SIDE;y++)for(unsigned x=0;x<PODCAST_COVER_NATIVE_SIDE;x++){
        unsigned sx=(2*x+1)*PODCAST_COVER_SIDE/(2*PODCAST_COVER_NATIVE_SIDE);
        unsigned sy=(2*y+1)*PODCAST_COVER_SIDE/(2*PODCAST_COVER_NATIVE_SIDE);
        memcpy(pixels+2*(y*PODCAST_COVER_NATIVE_SIDE+x),pixels+2*(sy*PODCAST_COVER_SIDE+sx),2);
    }
}
