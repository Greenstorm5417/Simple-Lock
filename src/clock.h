#pragma once

#include <stdint.h>

enum {
    CLOCK_W = 25,
    CLOCK_H = 7,
    CLOCK_TEXT = 6,
};

void clock_stamp(char text[CLOCK_TEXT]);
int clock_delay_ms(void);
void clock_draw(uint32_t* pixels, int stride, int scale, const char text[CLOCK_TEXT]);
