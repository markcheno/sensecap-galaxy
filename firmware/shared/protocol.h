#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// All wire integers are little endian; pixel words remain big endian.
namespace galaxy {
constexpr uint8_t VERSION = 1;
constexpr uint32_t BAUD = 1000000;
constexpr size_t BLOCK = 7680; // Eight 480-pixel columns.
constexpr size_t MAX_PAYLOAD = BLOCK;
constexpr size_t RAW_MAX = MAX_PAYLOAD + 12;
constexpr size_t WIRE_MAX = RAW_MAX + RAW_MAX / 254 + 2;
enum Op : uint8_t { HELLO = 1, LIST = 2, OPEN = 3, READ = 4, CLOSE = 5 };
enum Status : uint16_t { OK = 0, BAD_REQUEST = 1, NO_CARD = 2, IO_ERROR = 3,
                         NOT_FOUND = 4, BAD_VERSION = 5, TOO_LARGE = 6,
                         BAD_FILESYSTEM = 7 };
inline uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline uint32_t u32(const uint8_t* p) { return uint32_t(u16(p)) | uint32_t(u16(p+2)) << 16; }
inline void put16(uint8_t* p, uint16_t v) { p[0]=v; p[1]=v>>8; }
inline void put32(uint8_t* p, uint32_t v) { put16(p,v); put16(p+2,v>>16); }
inline uint32_t crc32(const uint8_t* data, size_t n) {
    uint32_t c=0xffffffff;
    while(n--) { c ^= *data++; for(int b=0;b<8;b++) c=(c>>1)^(0xedb88320u & (0u-(c&1))); }
    return ~c;
}
inline uint16_t sd_crc16(const uint8_t* data,size_t n) {
    uint16_t c=0;
    while(n--) {c^=uint16_t(*data++)<<8;for(int b=0;b<8;b++) c=(c<<1)^((c&0x8000)?0x1021:0);}
    return c;
}
inline size_t cobs_encode(const uint8_t* in, size_t n, uint8_t* out) {
    size_t pos=1, code_at=0; uint8_t code=1;
    for(size_t i=0;i<n;i++) {
        if(in[i]==0) { out[code_at]=code; code_at=pos++; code=1; }
        else { out[pos++]=in[i]; if(++code==255) { out[code_at]=code; code_at=pos++; code=1; } }
    }
    out[code_at]=code; return pos;
}
inline size_t cobs_decode(const uint8_t* in, size_t n, uint8_t* out, size_t cap) {
    size_t i=0,o=0;
    while(i<n) {
        uint8_t code=in[i++];
        if(!code || size_t(code-1)>n-i || size_t(code-1)>cap-o) return 0;
        for(unsigned j=1;j<code;j++) { if(in[i]==0) return 0; out[o++]=in[i++]; }
        if(code!=255 && i<n) { if(o==cap) return 0; out[o++]=0; }
    }
    return o;
}
struct Packet { uint8_t op; uint16_t seq,status,length; uint8_t payload[MAX_PAYLOAD]; };
inline size_t encode(const Packet& p, uint8_t* wire, uint8_t* scratch) {
    if(p.length>MAX_PAYLOAD) return 0;
    scratch[0]=VERSION; scratch[1]=p.op; put16(scratch+2,p.seq);
    put16(scratch+4,p.status); put16(scratch+6,p.length);
    memcpy(scratch+8,p.payload,p.length);
    put32(scratch+8+p.length,crc32(scratch,8+p.length));
    size_t n=cobs_encode(scratch,12+p.length,wire); wire[n++]=0; return n;
}
inline bool decode(const uint8_t* wire,size_t n,Packet& p,uint8_t* scratch) {
    size_t bytes=cobs_decode(wire,n,scratch,RAW_MAX);
    if(bytes<12 || scratch[0]!=VERSION) return false;
    uint16_t len=u16(scratch+6);
    if(len>MAX_PAYLOAD || bytes!=size_t(12+len) ||
       u32(scratch+8+len)!=crc32(scratch,8+len)) return false;
    p.op=scratch[1];p.seq=u16(scratch+2);p.status=u16(scratch+4);p.length=len;
    memcpy(p.payload,scratch+8,len);return true;
}
// Names cannot escape /galaxy or require JSON quoting.
inline bool name_valid(const char* name, size_t max=31) {
    size_t n=0;
    for(;name[n];n++) {
        unsigned char c=name[n];
        if(n>=max || !((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
           (c>='0'&&c<='9')||c=='_'||c=='-')) return false;
    }
    return n>0;
}
inline bool path_valid(const char* path) {
    if(strncmp(path,"/galaxy/",8)) return false;
    const char* slash=strchr(path+8,'/');
    if(!slash || slash-(path+8)>31) return false;
    char name[32]={};memcpy(name,path+8,slash-(path+8));
    if(!name_valid(name)) return false;
    const char* file=slash+1;
    if(!strcmp(file,"manifest.json")) return true;
    if(strlen(file)!=12 || file[0]!='b' || strcmp(file+8,".dat")) return false;
    for(int i=1;i<8;i++) if(file[i]<'0'||file[i]>'9') return false;
    return true;
}
} // namespace galaxy
