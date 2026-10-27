#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "protocol.h"
#include "sd_spi.h"
#include <stdio.h>
extern "C" {
#include "ff.h"
#include "diskio.h"
}

namespace {
constexpr uint CS=13;
bool initialized=false, high_capacity=false;
uint8_t transfer(uint8_t v=0xff) { uint8_t r;spi_write_read_blocking(spi1,&v,&r,1);return r; }
void release() { gpio_put(CS,1);transfer(); }
bool ready(uint32_t ms=500) {
    auto end=make_timeout_time_ms(ms);
    do { if(transfer()==0xff) return true; } while(!time_reached(end));
    return false;
}
uint8_t command(uint8_t cmd,uint32_t arg) {
    if(cmd!=12 && !ready()) return 0xff;
    transfer(0x40|cmd);transfer(arg>>24);transfer(arg>>16);transfer(arg>>8);transfer(arg);
    transfer(cmd==0?0x95:cmd==8?0x87:0x01);
    if(cmd==12) transfer(); // CMD12 has a stuff byte before its response.
    for(int i=0;i<10;i++) {uint8_t r=transfer();if(!(r&0x80)) return r;}
    return 0xff;
}
bool block(uint8_t* out,size_t size) {
    auto end=make_timeout_time_ms(500);uint8_t token;
    do {token=transfer();if(token!=0xff) break;} while(!time_reached(end));
    if(token!=0xfe) return false;
    spi_read_blocking(spi1,0xff,out,size);
    uint16_t crc=uint16_t(transfer())<<8;crc|=transfer();
    return galaxy::sd_crc16(out,size)==crc;
}
}
extern "C" DSTATUS disk_initialize(BYTE drive) {
    if(drive) return STA_NOINIT;
    initialized=false;
    spi_init(spi1,400000);spi_set_format(spi1,8,SPI_CPOL_0,SPI_CPHA_0,SPI_MSB_FIRST);
    for(uint pin=10;pin<=12;pin++) gpio_set_function(pin,GPIO_FUNC_SPI);
    gpio_pull_up(12);gpio_init(CS);gpio_set_dir(CS,GPIO_OUT);gpio_put(CS,1);
    for(int i=0;i<10;i++) transfer();
    gpio_put(CS,0);
    if(command(0,0)!=1 || command(8,0x1aa)!=1) {release();return STA_NOINIT;}
    uint8_t r7[4];for(auto& b:r7) b=transfer();
    if(r7[2]!=1 || r7[3]!=0xaa) {release();return STA_NOINIT;}
    auto end=make_timeout_time_ms(2500);uint8_t r=1;
    do {command(55,0);r=command(41,1u<<30);if(!r) break;sleep_ms(10);} while(!time_reached(end));
    if(r || command(58,0)) {release();return STA_NOINIT;}
    uint8_t ocr[4];for(auto& b:ocr) b=transfer();high_capacity=(ocr[0]&0x40)!=0;
    if(!high_capacity && command(16,512)) {release();return STA_NOINIT;}
    release();spi_set_baudrate(spi1,12000000);initialized=true;
    printf("SD initialized: OCR=%02x %02x %02x %02x, %s addressing\n",
           ocr[0],ocr[1],ocr[2],ocr[3],high_capacity?"block":"byte");
    return 0;
}
extern "C" DSTATUS disk_status(BYTE drive) {return drive||!initialized?STA_NOINIT:0;}
extern "C" DRESULT disk_read(BYTE drive,BYTE* buff,LBA_t sector,UINT count) {
    if(drive||!count) return RES_PARERR;
    if(!initialized) return RES_NOTRDY;
    gpio_put(CS,0);bool ok=command(count==1?17:18,high_capacity?sector:sector*512u)==0;
    if(ok) for(UINT i=0;i<count;i++) if(!block(buff+i*512,512)) {ok=false;break;}
    if(count>1) {if(command(12,0)>1) ok=false;ready();}
    release();if(!ok) initialized=false;return ok?RES_OK:RES_ERROR;
}
extern "C" DRESULT disk_ioctl(BYTE drive,BYTE cmd,void* buff) {
    if(drive||!initialized) return RES_NOTRDY;
    if(cmd==CTRL_SYNC) return RES_OK;
    if(cmd==GET_SECTOR_SIZE) {*(WORD*)buff=512;return RES_OK;}
    if(cmd==GET_BLOCK_SIZE) {*(DWORD*)buff=1;return RES_OK;}
    if(cmd==GET_SECTOR_COUNT) {
        uint8_t csd[16];gpio_put(CS,0);bool ok=command(9,0)==0 && block(csd,16);release();
        if(!ok || (csd[0]>>6)!=1) return RES_ERROR;
        *(LBA_t*)buff=(LBA_t(((uint32_t(csd[7]&63)<<16)|(uint32_t(csd[8])<<8)|csd[9]))+1)*1024;
        return RES_OK;
    }
    return RES_PARERR;
}

void sd_print_layout() {
    uint8_t sector[512];
    if(disk_read(0,sector,0,1)!=RES_OK) {printf("SD layout: sector 0 read failed\n");return;}
    printf("SD sector 0: signature=%02x%02x, jump=%02x %02x %02x, OEM=%.8s\n",
           sector[511],sector[510],sector[0],sector[1],sector[2],sector+3);
    uint32_t starts[4]={};uint8_t types[4]={};
    for(int i=0;i<4;i++) {
        const uint8_t* entry=sector+446+i*16;
        starts[i]=galaxy::u32(entry+8);
        types[i]=entry[4];
        printf("SD partition %d: type=%02x, start=%lu, sectors=%lu\n",i+1,entry[4],
               (unsigned long)starts[i],(unsigned long)galaxy::u32(entry+12));
    }
    for(int i=0;i<4;i++) if(starts[i]) {
        if(disk_read(0,sector,starts[i],1)!=RES_OK) {printf("SD partition %d: boot read failed\n",i+1);continue;}
        printf("SD partition %d boot: signature=%02x%02x, jump=%02x %02x %02x, OEM=%.8s, type=%.8s\n",
               i+1,sector[511],sector[510],sector[0],sector[1],sector[2],sector+3,sector+82);
        printf("SD BPB: bytes/sector=%u, sectors/cluster=%u, reserved=%u, fats=%u, root_entries=%u, "
               "total16=%u, total32=%lu, fat16=%u, fat32=%lu, version=%u, root_cluster=%lu\n",
               galaxy::u16(sector+11),sector[13],galaxy::u16(sector+14),sector[16],
               galaxy::u16(sector+17),galaxy::u16(sector+19),(unsigned long)galaxy::u32(sector+32),
               galaxy::u16(sector+22),(unsigned long)galaxy::u32(sector+36),galaxy::u16(sector+42),
               (unsigned long)galaxy::u32(sector+44));
        if(types[i]==0xaf && disk_read(0,sector,starts[i]+2,1)==RES_OK) {
            printf("SD Apple volume header: signature=%02x%02x (482b=HFS+, 4858=HFSX), version=%u\n",
                   sector[0],sector[1],unsigned((sector[2]<<8)|sector[3]));
        }
    }
}
