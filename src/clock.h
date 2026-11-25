#pragma once

#define CLOCK_H 7
#define CLOCK_TEXT 9

int clock_cols(bool twelve);
int clock_delay_ms(void);
void clock_draw(uint32_t* pixels, int stride, int scale, const char text[CLOCK_TEXT]);
void clock_stamp(char text[CLOCK_TEXT], bool twelve);
