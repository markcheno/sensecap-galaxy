#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "storage.h"
#include "protocol.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

namespace {
constexpr int CAPACITY=1024;
constexpr int64_t SOURCE_NAME_US=5000000;
constexpr float BRIGHTNESS[]={0.1f,0.2f,0.75f,1.0f};
struct Request {const Dataset* dataset;size_t band;uint32_t first,epoch;uint16_t count;};
struct Result {uint32_t first,epoch;uint16_t count;bool ok;int64_t elapsed;uint8_t pixels[galaxy::BLOCK];};
QueueHandle_t requests,results;
SemaphoreHandle_t storage_ready;
Dataset dataset;
std::vector<std::string> sources;
uint16_t* ring;
uint32_t position=0,loaded=0,epoch=0;
size_t band=0;
bool in_flight=false,menu=false,playing=false,failed=false;
bool announce_source=false;
std::string pending,remembered;
uint8_t brightness_idx=0,speed_idx=0;
float speeds[]={10,30,60};int speed_count=3;
float fraction=0;
int page=0;
int64_t pass_time=0,play_start=0,hold_until=0,last_ui=0,last_progress=0;
int64_t source_name_until=0;
uint64_t sample_us=0,sample_cols=0;
nvs_handle_t settings;

void save() {
    nvs_set_u8(settings,"brightness",brightness_idx);nvs_set_u8(settings,"speed",speed_idx);
    if(!dataset.name.empty()) nvs_set_str(settings,"source",dataset.name.c_str());
    nvs_commit(settings);
}
void reader(void*) {
    // UART interrupts are allocated on the core that installs the driver.
    // Keep them with the reader on CPU1, away from CPU0's RGB/GDMA ISRs.
    storage::init();
    xSemaphoreGive(storage_ready);
    Request req;static Result result;
    for(;;) {
        xQueueReceive(requests,&req,portMAX_DELAY);
        result.first=req.first;result.count=req.count;result.epoch=req.epoch;
        int64_t start=esp_timer_get_time();
        result.ok=storage::columns(*req.dataset,req.band,req.first,req.count,result.pixels);
        result.elapsed=esp_timer_get_time()-start;
        xQueueSend(results,&result,portMAX_DELAY);
    }
}
void reset_band() {
    ++epoch;position=loaded=0;fraction=0;playing=false;failed=false;
    pass_time=play_start=0;board::backlight(0);
}
void receive() {
    static Result r;
    if(xQueueReceive(results,&r,0)!=pdTRUE) return;
    in_flight=false;
    if(r.epoch!=epoch) return;
    if(!r.ok) {
        failed=true;
        ESP_LOGE("galaxy","Read failed: source=%s band=%u column=%lu count=%u: %s",
                 dataset.name.c_str(),unsigned(band+1),(unsigned long)r.first,unsigned(r.count),storage::error());
        return;
    }
    for(unsigned x=0;x<r.count;x++) for(unsigned y=0;y<480;y++) {
        size_t offset=(x*480+y)*2;
        ring[((r.first+x)%CAPACITY)*480+y]=(uint16_t(r.pixels[offset])<<8)|r.pixels[offset+1];
    }
    loaded=r.first+r.count;
    if(sample_cols<480) {
        sample_cols+=r.count;sample_us+=r.elapsed;
        if(sample_cols>=480 && sample_us) {
            float rate=sample_cols*1000000.0f/sample_us;
            float cap=std::min(120.0f,floorf(rate*0.6f));
            speed_count=1;speeds[0]=10;
            if(cap>=20) speeds[speed_count++]=std::min(30.0f,cap/2);
            if(cap>speeds[speed_count-1]+1) speeds[speed_count++]=cap;
            speed_idx=std::min<unsigned>(speed_idx,speed_count-1);
            ESP_LOGI("galaxy","Measured %.1f columns/s; maximum preset %.1f px/s (60%% budget)",rate,speeds[speed_count-1]);
        }
    }
}
void prefetch() {
    if(in_flight||failed||!pending.empty()||dataset.bands.empty()) return;
    uint32_t width=dataset.bands[band].width;
    uint32_t room=CAPACITY-(loaded-position);
    uint16_t n=std::min({uint32_t(8),room,width-loaded});
    if(!n) return;
    Request req{&dataset,band,loaded,epoch,n};
    if(xQueueSend(requests,&req,0)==pdTRUE) in_flight=true;
}
void render_image(int64_t now) {
    uint16_t* frame=board::backbuffer();
    // Ring is column-major; RGB DMA consumes row-major native RGB565.
    for(int x=0;x<480;x++) {
        const uint16_t* column=ring+((position+x)%CAPACITY)*480;
        for(int y=0;y<480;y++) frame[y*480+x]=column[y];
    }
    if(now<source_name_until) {
        int scale=dataset.name.size()<=24?3:2;
        board::rect(0,0,board::W,42,0);
        board::text(12,(42-7*scale)/2,dataset.name.c_str(),0xffff,scale);
    }
}
void draw_menu() {
    board::clear(0x0841);board::text(20,20,"SELECT SOURCE",0xffff,3);
    board::text(20,50,"TAP HEADER TO CLOSE",0x7bef,2);
    for(int row=0;row<7;row++) {
        size_t index=page*7+row;if(index>=sources.size()) break;
        int y=80+row*48;
        board::rect(12,y,456,43,sources[index]==dataset.name?0x02a0:0x18c3);
        board::text(20,y+13,sources[index].c_str(),0xffff,2);
    }
    char status[40];snprintf(status,sizeof(status),"PREV   %d OF %d   NEXT",page+1,int((sources.size()+6)/7));
    board::text(20,444,status,0xffff,2);
}
void touch() {
    static bool was=false,long_press=false;
    static int tx=0,ty=0;static int64_t began=0;
    int x=0,y=0;bool pressed=board::touch(x,y);int64_t now=esp_timer_get_time();
    if(pressed&&!was) {tx=x;ty=y;began=now;long_press=false;}
    if(pressed && !long_press && now-began>=700000) {
        long_press=true;menu=!menu;last_ui=0;
        ESP_LOGI("galaxy","Source menu %s",menu?"opened":"closed");
    }
    if(!pressed && was && !long_press && now-began>=30000) {
        if(menu) {
            if(ty<75) {
                menu=false;
                ESP_LOGI("galaxy","Source menu closed via header (tap x=%d y=%d)",tx,ty);
            }
            else if(ty>=430) {
                int pages=std::max(1,int((sources.size()+6)/7));
                page=(page+(tx<240?pages-1:1))%pages;
            } else {
                size_t index=page*7+(ty-80)/48;
                if(ty>=80 && ty<416 && index<sources.size()) {
                    menu=false;
                    if(sources[index]!=dataset.name || failed) {
                        pending=sources[index];board::backlight(0);
                        ESP_LOGI("galaxy","Source selected: %s -> %s",dataset.name.c_str(),pending.c_str());
                    } else ESP_LOGI("galaxy","Current source selected; resuming %s",dataset.name.c_str());
                }
            }
        } else if(tx<240) {
            brightness_idx=(brightness_idx+1)%4;save();
            ESP_LOGI("galaxy","Brightness set to %.0f%% (tap x=%d)",BRIGHTNESS[brightness_idx]*100,tx);
        } else {
            speed_idx=(speed_idx+1)%speed_count;save();
            ESP_LOGI("galaxy","Speed set to %.1f px/s (tap x=%d)",speeds[speed_idx],tx);
        }
        last_ui=0;
    }
    was=pressed;
}
void message(const char* first,const char* second,const char* third=nullptr) {
    board::clear();board::text(20,160,first,0xffff,3);board::text(20,220,second,0x7bef,2);
    if(third) board::text(20,260,third,0x7bef,2);
    ESP_ERROR_CHECK(board::present());board::backlight(0.2f);
}
bool discover() {
    if(!storage::hello()||!storage::list(sources)) return false;
    if(sources.empty()) return false;
    auto it=std::find(sources.begin(),sources.end(),remembered);
    pending=it!=sources.end()?remembered:sources.front();
    return true;
}
}

extern "C" void app_main() {
    esp_err_t err=nvs_flash_init();
    if(err==ESP_ERR_NVS_NO_FREE_PAGES||err==ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());err=nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);ESP_ERROR_CHECK(nvs_open("galaxy",NVS_READWRITE,&settings));
    nvs_get_u8(settings,"brightness",&brightness_idx);brightness_idx%=4;
    nvs_get_u8(settings,"speed",&speed_idx);speed_idx%=3;
    char name[32];size_t len=sizeof(name);
    if(nvs_get_str(settings,"source",name,&len)==ESP_OK) remembered=name;
    ESP_ERROR_CHECK(board::init());
    ring=(uint16_t*)heap_caps_malloc(CAPACITY*480*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(ring?ESP_OK:ESP_ERR_NO_MEM);
    requests=xQueueCreate(1,sizeof(Request));results=xQueueCreate(1,sizeof(Result));
    storage_ready=xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(requests&&results&&storage_ready?ESP_OK:ESP_ERR_NO_MEM);
    BaseType_t created=xTaskCreatePinnedToCore(reader,"sd-reader",6144,nullptr,4,nullptr,1);
    ESP_ERROR_CHECK(created==pdPASS?ESP_OK:ESP_ERR_NO_MEM);
    // Do not make metadata requests until CPU1 has installed its UART ISR.
    ESP_ERROR_CHECK(xSemaphoreTake(storage_ready,pdMS_TO_TICKS(5000))==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT);
    ESP_LOGI("galaxy","Indicator Galaxy ESP32 1.0.0, protocol %u",galaxy::VERSION);
    message("INDICATOR GALAXY","WAITING FOR SD SERVICE");
    int64_t retry=0,previous=esp_timer_get_time();bool discovered=false;
    for(;;) {
        int64_t now=esp_timer_get_time();float dt=std::min(0.1f,float(now-previous)/1000000);previous=now;
        touch();receive();
        if(!discovered) {
            if(now>=retry) {
                discovered=discover();retry=esp_timer_get_time()+2000000;
                if(!discovered) message("WAITING FOR CARD",storage::error());
            }
            vTaskDelay(pdMS_TO_TICKS(10));continue;
        }
        if(!pending.empty() && !in_flight) {
            message("LOADING SOURCE",pending.c_str());
            Dataset candidate;
            if(storage::load(pending,candidate)) {
                dataset=std::move(candidate);band=0;sample_cols=sample_us=0;reset_band();save();
                announce_source=true;source_name_until=0;
                ESP_LOGI("galaxy","%s: %u bands. Credit: %s",dataset.title.c_str(),unsigned(dataset.bands.size()),dataset.credit.c_str());
            } else {failed=true;}
            pending.clear();previous=esp_timer_get_time();last_ui=0;
        }
        if(failed) {
            if(now-last_ui>1000000) {message("DATA READ FAILED","LONG PRESS FOR SOURCES");last_ui=now;}
            if(menu) {draw_menu();ESP_ERROR_CHECK(board::present());board::backlight(0.2f);}
            // Reconnect and reload in a bounded way, preserving UI between attempts.
            if(!menu && now>=retry && !in_flight) {
                ESP_LOGW("galaxy","Storage recovery: reloading %s",dataset.name.c_str());
                discovered=false;remembered=dataset.name;retry=now+2000000;
            }
            vTaskDelay(pdMS_TO_TICKS(10));continue;
        }
        prefetch();
        if(dataset.bands.empty()) {vTaskDelay(pdMS_TO_TICKS(10));continue;}
        if(menu) {
            if(!last_ui||now-last_ui>100000) {draw_menu();ESP_ERROR_CHECK(board::present());board::backlight(0.2f);last_ui=now;}
            vTaskDelay(pdMS_TO_TICKS(5));continue;
        }
        if(hold_until>now) {board::backlight(0);vTaskDelay(pdMS_TO_TICKS(5));continue;}
        if(!playing && loaded>=480) {
            playing=true;play_start=now;pass_time=0;fraction=0;last_ui=0;
            if(announce_source) {
                source_name_until=now+SOURCE_NAME_US;announce_source=false;
            }
            last_progress=now;
            ESP_LOGI("galaxy","Playback started: source=%s band=%u/%u width=%lu",
                     dataset.name.c_str(),unsigned(band+1),unsigned(dataset.bands.size()),
                     (unsigned long)dataset.bands[band].width);
        }
        if(!playing) {
            if(!last_ui||now-last_ui>250000) {
                char progress[32];snprintf(progress,sizeof(progress),"%lu OF 480 COLUMNS",(unsigned long)loaded);
                message("BUFFERING",dataset.name.c_str(),progress);last_ui=now;
            }
            vTaskDelay(pdMS_TO_TICKS(5));continue;
        }
        uint32_t max_pos=dataset.bands[band].width-480;
        float speed=speeds[speed_idx];
        // Pause elapsed playback time on starvation and discard excess progress.
        fraction+=dt*speed;uint32_t delta=uint32_t(fraction);
        uint32_t available=loaded-position-480;
        uint32_t step=std::min({delta,available,max_pos-position});
        position+=step;fraction-=step;
        if(step<delta) fraction=0;
        if(now-last_progress>=30000000) {
            ESP_LOGI("galaxy","Playback progress: source=%s band=%u column=%lu buffered=%lu speed=%.1f heap=%u",
                     dataset.name.c_str(),unsigned(band+1),(unsigned long)position,
                     (unsigned long)(loaded-position),speed,unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
            last_progress=now;
        }
        if(available>0 || position==max_pos) pass_time+=int64_t(dt*1000000);
        float fade_in=std::min(1.0f,pass_time/500000.0f);
        float remaining=(max_pos-position)/speed;
        // Static (exactly 480-wide) bands fade in, hold, and fade out too.
        float fade_out=max_pos?std::min(1.0f,remaining/0.5f):std::min(1.0f,std::max(0.0f,(2.0f-pass_time/1000000.0f)/0.5f));
        float k=std::min(fade_in,fade_out);
        if(!last_ui||now-last_ui>=33333) {
            render_image(now);ESP_ERROR_CHECK(board::present());last_ui=now;
        }
        board::backlight(BRIGHTNESS[brightness_idx]*k*k);
        if((max_pos && position==max_pos)||(!max_pos&&pass_time>=2000000)) {
            board::backlight(0);bool last=band+1==dataset.bands.size();
            band=last?0:band+1;reset_band();hold_until=now+(last?1000000:250000);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
