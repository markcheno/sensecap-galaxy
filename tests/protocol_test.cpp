#include "../firmware/shared/protocol.h"
#include <assert.h>
#include <stdio.h>
using namespace galaxy;
int main() {
    assert(crc32((const uint8_t*)"123456789",9)==0xcbf43926);
    assert(sd_crc16((const uint8_t*)"123456789",9)==0x31c3);
    Packet p={},out={};uint8_t wire[WIRE_MAX],scratch[RAW_MAX];
    for(size_t size=0;size<=BLOCK;size++) {
        p.op=READ;p.seq=65535;p.status=0;p.length=size;
        for(size_t i=0;i<size;i++) p.payload[i]=uint8_t(i*37+size);
        size_t n=encode(p,wire,scratch);assert(n<=WIRE_MAX && wire[n-1]==0);
        assert(decode(wire,n-1,out,scratch));
        assert(out.seq==65535 && out.op==READ && out.length==size);
        assert(!memcmp(p.payload,out.payload,size));
        if(n>3) {wire[n/2]^=0x80;assert(!decode(wire,n-1,out,scratch));}
    }
    uint8_t bad[]={4,1};assert(!decode(bad,sizeof(bad),out,scratch));
    assert(path_valid("/galaxy/pandora/manifest.json"));
    assert(path_valid("/galaxy/rubin/b0001000.dat"));
    assert(!path_valid("/galaxy/../manifest.json"));
    assert(!path_valid("/galaxy/x/../../secret.dat"));
    assert(!path_valid("/galaxy/x/b0001000.dat/"));
    assert(!path_valid("/galaxy/x/b00010000.dat"));
    puts("Protocol: all 7,681 payload lengths, CRC, corruption and path checks passed");
}
