// Indicator wiring and GX initialization adapted from Seeed's Apache-2.0 SDK.
// See third_party/NOTICES.md for the exact source and modifications.
#include "board.h"
#include <algorithm>
#include <ctype.h>
#include <string.h>
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_psram.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

namespace {
esp_lcd_panel_handle_t panel;
SemaphoreHandle_t vsync;
uint16_t* fb[2];int back=1;
constexpr size_t BOUNCE_PIXELS=board::W*10;
static_assert((board::W*board::H)%(2*BOUNCE_PIXELS)==0,
              "RGB frames must contain an even number of bounce buffers");
uint8_t expander=0,touch_addr=0;
uint16_t output=0xffff,direction=0xffff;
void sleep_ms(unsigned ms) {vTaskDelay(pdMS_TO_TICKS(ms));}
esp_err_t probe(uint8_t addr) {
    auto cmd=i2c_cmd_link_create();i2c_master_start(cmd);
    i2c_master_write_byte(cmd,addr<<1,true);i2c_master_stop(cmd);
    auto r=i2c_master_cmd_begin(I2C_NUM_0,cmd,pdMS_TO_TICKS(30));i2c_cmd_link_delete(cmd);return r;
}
esp_err_t write16(uint8_t reg,uint16_t v) {
    uint8_t b[]={reg,uint8_t(v),uint8_t(v>>8)};
    return i2c_master_write_to_device(I2C_NUM_0,expander,b,3,pdMS_TO_TICKS(50));
}
void exp_level(int pin,bool value) {
    uint16_t mask=1u<<pin;output=(output&~mask)|(value?mask:0);
    ESP_ERROR_CHECK(write16(2,output));
}
void exp_output(int pin) {direction&=~(1u<<pin);ESP_ERROR_CHECK(write16(6,direction));}
void lcd_word(uint16_t value) {
    exp_level(4,false);gpio_set_level(GPIO_NUM_41,0);
    for(int bit=8;bit>=0;--bit) {
        gpio_set_level(GPIO_NUM_48,(value>>bit)&1);esp_rom_delay_us(2);
        gpio_set_level(GPIO_NUM_41,1);esp_rom_delay_us(2);gpio_set_level(GPIO_NUM_41,0);
    }
    exp_level(4,true);
}
void lcd_command(unsigned v) {lcd_word(v);}
void lcd_data(unsigned v) {lcd_word(0x100|v);}
#include "st7701_init.inc"
bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void*) {
    BaseType_t awoken=pdFALSE;xSemaphoreGiveFromISR(vsync,&awoken);return awoken==pdTRUE;
}
// Five columns per glyph, low bit at the top; independently specified here.
const uint8_t letters[26][5]={
 {126,9,9,9,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
 {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
 {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
 {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
 {62,65,81,33,94},{127,9,25,41,70},{38,73,73,73,50},{1,1,127,1,1},
 {63,64,64,64,63},{31,32,64,32,31},{127,32,24,32,127},{99,20,8,20,99},
 {3,4,120,4,3},{97,81,73,69,67}};
const uint8_t digits[10][5]={
 {62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},{33,65,69,75,49},
 {24,20,18,127,16},{39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},
 {54,73,73,73,54},{6,73,73,41,30}};
}
namespace board {
esp_err_t init() {
    if(!esp_psram_is_initialized()) return ESP_ERR_NO_MEM;
    i2c_config_t i2c={};i2c.mode=I2C_MODE_MASTER;i2c.sda_io_num=GPIO_NUM_39;i2c.scl_io_num=GPIO_NUM_40;
    i2c.sda_pullup_en=GPIO_PULLUP_ENABLE;i2c.scl_pullup_en=GPIO_PULLUP_ENABLE;i2c.master.clk_speed=400000;
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0,&i2c));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0,I2C_MODE_MASTER,0,0,0));
    if(probe(0x20)==ESP_OK) expander=0x20;
    else if(probe(0x39)==ESP_OK) expander=0x39;
    else return ESP_ERR_NOT_FOUND;
    // Preserve unrelated expander outputs and directions, including the RP reset.
    uint8_t reg=2,b[2];
    ESP_ERROR_CHECK(i2c_master_write_read_device(I2C_NUM_0,expander,&reg,1,b,2,pdMS_TO_TICKS(50)));
    output=b[0]|uint16_t(b[1])<<8;
    reg=6;ESP_ERROR_CHECK(i2c_master_write_read_device(I2C_NUM_0,expander,&reg,1,b,2,pdMS_TO_TICKS(50)));
    direction=b[0]|uint16_t(b[1])<<8;
    exp_level(8,true);exp_output(8); // Release RP2040 reset.
    exp_level(7,false);exp_output(7);sleep_ms(5);exp_level(7,true);sleep_ms(100);
    for(uint8_t addr: {uint8_t(0x48),uint8_t(0x38)}) if(probe(addr)==ESP_OK) {touch_addr=addr;break;}
    ESP_LOGI("board","Expander 0x%02x, touch 0x%02x, PSRAM %u bytes",expander,touch_addr,unsigned(esp_psram_get_size()));
    ledc_timer_config_t timer={};timer.speed_mode=LEDC_LOW_SPEED_MODE;timer.timer_num=LEDC_TIMER_0;
    timer.duty_resolution=LEDC_TIMER_12_BIT;timer.freq_hz=10000;timer.clk_cfg=LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t bl={};bl.gpio_num=45;bl.speed_mode=LEDC_LOW_SPEED_MODE;
    bl.channel=LEDC_CHANNEL_0;bl.timer_sel=LEDC_TIMER_0;
    ESP_ERROR_CHECK(ledc_channel_config(&bl));
    exp_level(4,true);exp_level(5,true);exp_output(4);exp_output(5);
    exp_level(5,false);sleep_ms(10);exp_level(5,true);sleep_ms(120);
    gpio_config_t serial={};serial.pin_bit_mask=(1ULL<<41)|(1ULL<<48);serial.mode=GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&serial));
#if !CONFIG_GALAXY_LCD_DX
    st7701_init();
#endif
    esp_lcd_rgb_panel_config_t cfg={};cfg.clk_src=LCD_CLK_SRC_PLL160M;
    cfg.data_width=16;cfg.bits_per_pixel=16;cfg.num_fbs=2;cfg.psram_trans_align=64;
    // Read PSRAM through the CPU cache into internal SRAM. Direct PSRAM DMA
    // can underrun during competing memory/flash traffic and leave a wrapped
    // image until scanout is restarted. Keep both full framebuffers in PSRAM.
    cfg.bounce_buffer_size_px=BOUNCE_PIXELS;cfg.sram_trans_align=64;
    cfg.disp_gpio_num=-1;cfg.pclk_gpio_num=21;cfg.vsync_gpio_num=17;cfg.hsync_gpio_num=16;cfg.de_gpio_num=18;
    for(int i=0;i<16;i++) cfg.data_gpio_nums[i]=15-i;
    cfg.timings.pclk_hz=CONFIG_GALAXY_LCD_PIXEL_CLOCK;cfg.timings.h_res=W;cfg.timings.v_res=H;
    cfg.timings.hsync_back_porch=50;cfg.timings.hsync_front_porch=10;cfg.timings.hsync_pulse_width=8;
    cfg.timings.vsync_back_porch=20;cfg.timings.vsync_front_porch=10;cfg.timings.vsync_pulse_width=8;
    cfg.flags.fb_in_psram=1;
    vsync=xSemaphoreCreateCounting(4,0);if(!vsync) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg,&panel));
    esp_lcd_rgb_panel_event_callbacks_t callbacks={};callbacks.on_vsync=on_vsync;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel,&callbacks,nullptr));
    void* first=nullptr;void* second=nullptr;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel,2,&first,&second));
    fb[0]=static_cast<uint16_t*>(first);fb[1]=static_cast<uint16_t*>(second);
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_LOGI("board","RGB %dx%d, %u Hz pixel clock, two %u-pixel SRAM bounce buffers on CPU%d",
             W,H,unsigned(CONFIG_GALAXY_LCD_PIXEL_CLOCK),unsigned(BOUNCE_PIXELS),xPortGetCoreID());
    return ESP_OK;
}
uint16_t* backbuffer() {return fb[back];}
esp_err_t present() {
    // The bounce-buffer ISR latches the next PSRAM frame while prefetching.
    // Wait two boundaries so neither scanout nor its lookahead reads the old
    // framebuffer before we return it to the renderer.
    while(xSemaphoreTake(vsync,0)==pdTRUE) {}
    auto r=esp_lcd_panel_draw_bitmap(panel,0,0,W,H,fb[back]);if(r!=ESP_OK) return r;
    for(int i=0;i<2;i++) if(xSemaphoreTake(vsync,pdMS_TO_TICKS(250))!=pdTRUE) return ESP_ERR_TIMEOUT;
    back^=1;return ESP_OK;
}
void backlight(float value) {
    unsigned duty=unsigned(std::max(0.0f,std::min(1.0f,value))*4095);
    ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,duty);ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0);
}
bool touch(int& x,int& y) {
    if(!touch_addr) return false;
    uint8_t reg=2,b[5];
    if(i2c_master_write_read_device(I2C_NUM_0,touch_addr,&reg,1,b,5,pdMS_TO_TICKS(10))!=ESP_OK || !(b[0]&15)) return false;
    x=((b[1]&15)<<8)|b[2];y=((b[3]&15)<<8)|b[4];
#if CONFIG_GALAXY_TOUCH_SWAP_XY
    std::swap(x,y);
#endif
#if CONFIG_GALAXY_TOUCH_MIRROR_X
    x=W-1-x;
#endif
#if CONFIG_GALAXY_TOUCH_MIRROR_Y
    y=H-1-y;
#endif
    return x>=0&&x<W&&y>=0&&y<H;
}
void clear(uint16_t color) {std::fill(fb[back],fb[back]+W*H,color);}
void rect(int x,int y,int w,int h,uint16_t color) {
    for(int yy=std::max(0,y);yy<std::min(H,y+h);yy++)
        for(int xx=std::max(0,x);xx<std::min(W,x+w);xx++) fb[back][yy*W+xx]=color;
}
void text(int x,int y,const char* value,uint16_t color,int scale) {
    for(;*value;value++,x+=6*scale) {
        int c=toupper(static_cast<unsigned char>(*value));const uint8_t* glyph=nullptr;
        if(c>='A'&&c<='Z') glyph=letters[c-'A'];
        if(c>='0'&&c<='9') glyph=digits[c-'0'];
        if(glyph) for(int xx=0;xx<5;xx++) for(int yy=0;yy<7;yy++)
            if(glyph[xx]&(1<<yy)) rect(x+xx*scale,y+yy*scale,scale,scale,color);
        if(c=='-'||c=='_') rect(x,y+(c=='-'?3:6)*scale,5*scale,scale,color);
        if(c=='.'||c==':') {rect(x+2*scale,y+6*scale,scale,scale,color);if(c==':') rect(x+2*scale,y+2*scale,scale,scale,color);}
    }
}
}
