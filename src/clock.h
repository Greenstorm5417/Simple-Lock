#pragma once

#include <stdint.h>

#define CLOCK_W     25
#define CLOCK_H     7
#define CLOCK_TEXT  6

int clock_delay_ms(void);
void clock_draw(uint32_t* pixels, int stride, int scale, const char text[CLOCK_TEXT]);
void clock_stamp(char text[CLOCK_TEXT]);
