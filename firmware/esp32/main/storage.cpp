#include "storage.h"
#include "protocol.h"
#include <algorithm>
#include <math.h>
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
using namespace galaxy;
namespace {
SemaphoreHandle_t mutex;
QueueHandle_t uart_events;
Packet request,reply;
uint8_t wire[WIRE_MAX],scratch[RAW_MAX];
uint16_t sequence=0;
std::string opened;
uint32_t opened_size=0;
const char* last_error="Waiting for RP2040";
void report_uart_errors() {
    uart_event_t event;
    while(xQueueReceive(uart_events,&event,0)==pdTRUE) {
        if(event.type==UART_FIFO_OVF || event.type==UART_BUFFER_FULL ||
           event.type==UART_FRAME_ERR || event.type==UART_PARITY_ERR)
            ESP_LOGW("storage","UART receive error: %s",
                     event.type==UART_FIFO_OVF?"hardware FIFO overflow":
                     event.type==UART_BUFFER_FULL?"software buffer full":
                     event.type==UART_FRAME_ERR?"framing":"parity");
    }
}
bool rpc(uint8_t op,const uint8_t* payload,uint16_t length) {
    request.op=op;request.seq=++sequence;request.status=0;request.length=length;
    if(length) memcpy(request.payload,payload,length);
    for(int attempt=0;attempt<3;attempt++) {
        report_uart_errors();
        uart_flush_input(UART_NUM_2);size_t n=encode(request,wire,scratch);
        if(uart_write_bytes(UART_NUM_2,wire,n)!=int(n)) continue;
        int64_t deadline=esp_timer_get_time()+1200000;
        size_t pos=0,received=0,rejected=0;bool dropping=false;
        uint8_t input[1024];
        while(esp_timer_get_time()<deadline) {
            report_uart_errors();
            size_t available=0;uart_get_buffered_data_len(UART_NUM_2,&available);
            int got=uart_read_bytes(UART_NUM_2,input,std::min(sizeof(input),std::max(size_t(1),available)),pdMS_TO_TICKS(5));
            if(got>0) received+=got;
            for(int i=0;i<got;i++) {
                uint8_t b=input[i];
                if(b) {if(pos>=WIRE_MAX-1) dropping=true;if(!dropping) wire[pos++]=b;continue;}
                if(!dropping && pos && decode(wire,pos,reply,scratch) && reply.seq==request.seq && reply.op==op) {
                    if(reply.status!=OK) {
                        const char* messages[]={"OK","Bad storage request","No FAT32 card","SD read error",
                                                "Dataset or file missing","Protocol mismatch","Too many sources",
                                                "No FAT32 filesystem"};
                        last_error=reply.status<8?messages[reply.status]:"Storage error";
                        opened.clear();return false;
                    }
                    return true;
                }
                if(pos || dropping) ++rejected;
                pos=0;dropping=false;
            }
        }
        report_uart_errors();
        ESP_LOGW("storage","RPC op %u seq %u attempt %d timed out: %u bytes, %u rejected frames",
                 unsigned(op),unsigned(request.seq),attempt+1,unsigned(received),unsigned(rejected));
    }
    opened.clear();last_error="RP2040 timeout";return false;
}
bool read_file(const std::string& path,uint32_t offset,size_t count,uint8_t* out) {
    if(opened!=path) {
        if(!path_valid(path.c_str())||!rpc(OPEN,(const uint8_t*)path.data(),path.size())||reply.length!=4) return false;
        opened_size=u32(reply.payload);opened=path;
    }
    if(offset>opened_size||count>opened_size-offset) {last_error="Invalid file size";return false;}
    while(count) {
        uint16_t n=std::min(count,BLOCK);uint8_t args[6];put32(args,offset);put16(args+4,n);
        if(!rpc(READ,args,6)||reply.length!=n) {opened.clear();return false;}
        memcpy(out,reply.payload,n);count-=n;offset+=n;out+=n;
    }
    return true;
}

}
namespace storage {
void init() {
    init_manifest_allocator();
    mutex=xSemaphoreCreateMutex();ESP_ERROR_CHECK(mutex?ESP_OK:ESP_ERR_NO_MEM);
    uart_config_t cfg={};cfg.baud_rate=BAUD;cfg.data_bits=UART_DATA_8_BITS;
    cfg.parity=UART_PARITY_DISABLE;cfg.stop_bits=UART_STOP_BITS_1;cfg.flow_ctrl=UART_HW_FLOWCTRL_DISABLE;cfg.source_clk=UART_SCLK_DEFAULT;
    // At 1 Mbaud the default 120-byte RX threshold leaves only 80 us before
    // the 128-byte hardware FIFO overflows. RGB bounce-buffer interrupts can
    // exceed that window. Service RX earlier and let its IRAM ISR preempt the
    // level-1 LCD/GDMA interrupts, preserving the existing baud rate/protocol.
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_2,WIRE_MAX*2,WIRE_MAX*2,32,&uart_events,
                                       ESP_INTR_FLAG_IRAM|ESP_INTR_FLAG_LEVEL2));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_2,&cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2,19,20,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_rx_full_threshold(UART_NUM_2,32));
    ESP_LOGI("storage","UART %lu baud, RX threshold 32, level-2 IRAM ISR on CPU%d",
             (unsigned long)BAUD,xPortGetCoreID());
}
bool hello() {
    xSemaphoreTake(mutex,portMAX_DELAY);bool ok=rpc(HELLO,nullptr,0)&&reply.length==6&&u16(reply.payload)==VERSION&&u32(reply.payload+2)==BAUD;
    opened.clear();xSemaphoreGive(mutex);return ok;
}
bool list(std::vector<std::string>& names) {
    xSemaphoreTake(mutex,portMAX_DELAY);bool ok=rpc(LIST,nullptr,0);
    if(ok) {
        names.clear();size_t start=0;
        for(size_t i=0;i<reply.length;i++) if(reply.payload[i]=='\n') {
            std::string name((char*)reply.payload+start,i-start);start=i+1;
            if(!name_valid(name.c_str())||names.size()>=64) {ok=false;break;}
            names.push_back(std::move(name));
        }
        if(start!=reply.length) ok=false;
        std::sort(names.begin(),names.end());
    }
    xSemaphoreGive(mutex);return ok;
}
bool load(const std::string& name,Dataset& dataset) {
    if(!name_valid(name.c_str())) return false;
    xSemaphoreTake(mutex,portMAX_DELAY);
    std::string path="/galaxy/"+name+"/manifest.json";
    bool ok=rpc(OPEN,(const uint8_t*)path.data(),path.size())&&reply.length==4;
    uint32_t size=ok?u32(reply.payload):0;
    opened.clear();
    if(size<1||size>128*1024) ok=false;
    std::vector<uint8_t> body;
    if(ok) {body.resize(size+1);ok=read_file(path,0,size,body.data());body[size]=0;}
    xSemaphoreGive(mutex);
    Dataset candidate;candidate.name=name;
    if(ok) ok=parse_dataset((char*)body.data(),candidate);
    if(ok) dataset=std::move(candidate);
    else last_error="Invalid dataset or read error";
    return ok;
}
bool columns(const Dataset& dataset,size_t band,uint32_t start,uint16_t count,uint8_t* out) {
    if(band>=dataset.bands.size()) return false;
    const Band& b=dataset.bands[band];
    if(start>b.width||count>b.width-start) return false;
    xSemaphoreTake(mutex,portMAX_DELAY);bool ok=true;
    for(const auto& c:b.chunks) {
        if(!count) break;
        if(start<c.first||start-c.first>=c.columns) continue;
        uint32_t local=start-c.first;uint16_t n=std::min(uint32_t(count),c.columns-local);
        std::string path="/galaxy/"+dataset.name+"/"+c.file;
        if(!read_file(path,local*960,n*960,out)||opened_size!=c.bytes) {ok=false;break;}
        start+=n;count-=n;out+=n*960;
    }
    xSemaphoreGive(mutex);return ok && !count;
}
const char* error() {return last_error;}
}
