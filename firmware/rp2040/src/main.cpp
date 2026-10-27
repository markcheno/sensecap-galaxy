#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "protocol.h"
#include "sd_spi.h"
extern "C" {
#include "ff.h"
#include "diskio.h"
}
#include "card_mount.h"
using namespace galaxy;
static FATFS fs;
static FIL file;
static bool mounted=false,opened=false;
static Status mount_error=NO_CARD;
static Packet packet;
static uint8_t wire[WIRE_MAX],scratch[RAW_MAX];

static void unmount() {
    if(opened) f_close(&file);
    opened=false;mounted=false;f_mount(nullptr,"",0);
}
static bool mount() {
    if(mounted && (disk_status(0)&STA_NOINIT)) unmount();
    if(mounted) return true;
    FRESULT r=f_mount(&fs,"",1);
    mount_error=card_mount_status(r,fs.fs_type);
    if(mount_error!=OK) {
        printf("FAT32 mount failed (FatFs=%u, type=%u, status=%u); card is never auto-formatted\n",
               unsigned(r),unsigned(fs.fs_type),unsigned(mount_error));
        if(r==FR_NO_FILESYSTEM || r==FR_OK) sd_print_layout();
        unmount();return false;
    }
    mounted=true;printf("FAT32 mounted: %lu clusters\n",(unsigned long)fs.n_fatent);return true;
}
static void handle() {
    uint8_t op=packet.op;uint16_t length=packet.length;
    packet.status=OK;packet.length=0;
    if(op==HELLO) {
        if(length!=0) {packet.status=BAD_REQUEST;return;}
        if(!mount()) {packet.status=mount_error;return;}
        put16(packet.payload,VERSION);put32(packet.payload+2,BAUD);packet.length=6;return;
    }
    if(!mounted) {packet.status=NO_CARD;return;}
    if(op==LIST) {
        DIR dir;FILINFO info;
        FRESULT opened_dir=f_opendir(&dir,"/galaxy");
        if(opened_dir!=FR_OK) {
            packet.status=opened_dir==FR_NO_PATH?NOT_FOUND:IO_ERROR;
            if(opened_dir==FR_DISK_ERR||opened_dir==FR_NOT_READY) unmount();
            return;
        }
        size_t n=0;unsigned count=0;FRESULT r;
        while((r=f_readdir(&dir,&info))==FR_OK && info.fname[0]) {
            if(!(info.fattrib&AM_DIR)||!name_valid(info.fname)) continue;
            char path[96];snprintf(path,sizeof(path),"/galaxy/%.31s/manifest.json",info.fname);
            FILINFO manifest;
            if(f_stat(path,&manifest)!=FR_OK || manifest.fsize>128*1024) continue;
            size_t len=strlen(info.fname);
            if(++count>64 || n+len+1>MAX_PAYLOAD) {packet.status=TOO_LARGE;break;}
            memcpy(packet.payload+n,info.fname,len);n+=len;packet.payload[n++]='\n';
        }
        f_closedir(&dir);if(r!=FR_OK) packet.status=IO_ERROR;
        if(packet.status==OK) packet.length=n;
    } else if(op==OPEN) {
        if(opened) f_close(&file);
        opened=false;
        char path[96];
        if(length==0||length>=sizeof(path)||memchr(packet.payload,0,length)) {packet.status=BAD_REQUEST;return;}
        memcpy(path,packet.payload,length);path[length]=0;
        if(!path_valid(path)) {packet.status=BAD_REQUEST;return;}
        FRESULT r=f_open(&file,path,FA_READ);
        if(r!=FR_OK) {packet.status=r==FR_NO_FILE?NOT_FOUND:IO_ERROR;return;}
        opened=true;put32(packet.payload,f_size(&file));packet.length=4;
    } else if(op==READ) {
        if(length!=6||!opened) {packet.status=BAD_REQUEST;return;}
        uint32_t offset=u32(packet.payload);uint16_t count=u16(packet.payload+4);
        if(count>BLOCK || offset>f_size(&file) || count>f_size(&file)-offset) {packet.status=BAD_REQUEST;return;}
        UINT got=0;
        if(f_lseek(&file,offset)!=FR_OK || f_read(&file,packet.payload,count,&got)!=FR_OK || got!=count) {
            packet.status=IO_ERROR;unmount();return;
        }
        packet.length=got;
    } else if(op==CLOSE) {
        if(opened) f_close(&file);
        opened=false;
    } else packet.status=BAD_REQUEST;
}
int main() {
    stdio_init_all();
    gpio_init(18);gpio_set_dir(18,GPIO_OUT);gpio_put(18,1);
    uart_init(uart0,BAUD);gpio_set_function(16,GPIO_FUNC_UART);gpio_set_function(17,GPIO_FUNC_UART);
    uart_set_fifo_enabled(uart0,true);
    printf("Indicator Galaxy RP2040 1.0.0 protocol %u, UART %lu\n",VERSION,(unsigned long)BAUD);
    size_t n=0;bool dropping=false;uint64_t last=0;
    for(;;) {
        if(!uart_is_readable(uart0)) {tight_loop_contents();continue;}
        uint8_t b=uart_getc(uart0);uint64_t now=time_us_64();
        if(now-last>100000) {n=0;dropping=false;}last=now;
        if(b) {
            if(n>=sizeof(wire)-1) dropping=true;
            if(!dropping) wire[n++]=b;
            continue;
        }
        if(!dropping && n && decode(wire,n,packet,scratch)) {
            handle();size_t len=encode(packet,wire,scratch);
            uart_write_blocking(uart0,wire,len);
        }
        n=0;dropping=false;
    }
}
