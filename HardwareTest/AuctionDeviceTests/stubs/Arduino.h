#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
using String = std::string;
#define LOW 0
#define INPUT_PULLUP 2
extern unsigned long now;
extern int pins[40];
inline unsigned long millis(){ return now; }
inline int digitalRead(int p){ return pins[p]; }
inline void pinMode(int,int){}
inline void delay(int){}
struct SerialType { void begin(int){} int available(){return 0;} char read(){return 0;} template<class T> void print(const T&){} template<class T> void println(const T&){} };
extern SerialType Serial;
