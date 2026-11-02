// E!nkster 397 bench: the basics every file of the sketch expects, on a computer
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <cctype>
#include <string>
#include <map>
#include <vector>
#include <functional>
#include <algorithm>
#include <initializer_list>
#include <sys/time.h>
#include <unistd.h>
#include "WString.h"
using std::min; using std::max; using std::abs; using std::isnan;
#define PI 3.1415926535897932384626433832795
#define HIGH 1
#define LOW 0
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#define RTC_DATA_ATTR
#define ESP_OK 0
typedef int esp_err_t;
#define MALLOC_CAP_SPIRAM 0
static inline void *heap_caps_malloc(size_t n, int) { return malloc(n); }
static inline void *heap_caps_realloc(void *p, size_t n, int) { return realloc(p, n); }
extern uint64_t hostUs;                 // the world's clock, microseconds since 1970
extern uint64_t hostWakeUs;             // when this wake began
static inline uint32_t millis() { hostUs += 50; return (uint32_t)((hostUs - hostWakeUs) / 1000); }
void delay(uint32_t ms);
static inline void delayMicroseconds(uint32_t us) { hostUs += us; }
static inline uint32_t esp_random() { return (uint32_t)rand(); }
