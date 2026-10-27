#pragma once
#include <stdint.h>
#include "esp_err.h"
namespace board {
constexpr int W=480,H=480;
esp_err_t init();
uint16_t* backbuffer();
esp_err_t present();
void backlight(float value);
bool touch(int& x,int& y);
void clear(uint16_t color=0);
void text(int x,int y,const char* value,uint16_t color=0xffff,int scale=3);
void rect(int x,int y,int width,int height,uint16_t color);
}
