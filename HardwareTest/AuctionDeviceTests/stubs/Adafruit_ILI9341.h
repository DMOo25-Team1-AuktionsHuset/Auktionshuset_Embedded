#pragma once
#include "SPI.h"
#define ILI9341_BLACK 0
#define ILI9341_GREEN 1
#define ILI9341_YELLOW 2
#define ILI9341_WHITE 3
struct Adafruit_ILI9341 { Adafruit_ILI9341(SPIType*,int,int,int){} void begin(){} void setRotation(int){} void fillScreen(int){} void setTextSize(int){} void setTextColor(int){} void setCursor(int,int){} template<class T> void print(const T&){} };
