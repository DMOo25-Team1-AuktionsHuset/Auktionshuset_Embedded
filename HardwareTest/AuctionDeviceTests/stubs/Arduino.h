#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <sstream>
using String = std::string;
#define LOW 0
#define INPUT_PULLUP 2
extern unsigned long now;
extern int pins[40];
inline unsigned long millis(){ return now; }
inline int digitalRead(int p){ return pins[p]; }
inline void pinMode(int,int){}
inline void delay(int milliseconds){ now += milliseconds; }
struct SerialType {
    std::string output, input;
    size_t position = 0;
    size_t setRxBufferSize(size_t size){ return size; }
    void begin(int){}
    int available(){ return static_cast<int>(input.size() - position); }
    char read(){ return input[position++]; }
    template<class T> void print(const T& value){ std::ostringstream stream; stream << value; output += stream.str(); }
    template<class T> void println(const T& value){ print(value); output += '\n'; }
    void println(){ output += '\n'; }
};
extern SerialType Serial;
