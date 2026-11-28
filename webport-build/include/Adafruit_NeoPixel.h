#pragma once
#include <Arduino.h>
#define NEO_GRB 1
#define NEO_KHZ800 2
class Adafruit_NeoPixel { public: Adafruit_NeoPixel(int,int,int){} void begin(){} void show(){} void setPixelColor(int,uint32_t){} void setPixelColor(int,uint8_t,uint8_t,uint8_t){} void clear(){} static uint32_t Color(uint8_t r,uint8_t g,uint8_t b){return ((uint32_t)r<<16)|((uint32_t)g<<8)|b;} static uint32_t ColorHSV(uint16_t h,uint8_t,uint8_t v){uint8_t r=0,g=0,b=0,region=(h*6)>>8,rem=(h*6)&255;switch(region){case 0:r=v;g=rem*v/255;break;case 1:r=(255-rem)*v/255;g=v;break;case 2:g=v;b=rem*v/255;break;case 3:g=(255-rem)*v/255;b=v;break;case 4:r=rem*v/255;b=v;break;default:r=v;b=(255-rem)*v/255;}return Color(r,g,b);} };
