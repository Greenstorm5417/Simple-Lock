#define _GNU_SOURCE

#include <string.h>
#include <time.h>

#include "clock.h"

#define CLOCK_CELLS 5

static const uint8_t digits[10][CLOCK_H] = {
    { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e },
    { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f },
    { 0x0e, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0e },
    { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 },
    { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e },
    { 0x0e, 0x10, 0x1e, 0x11, 0x11, 0x11, 0x0e },
    { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e },
    { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e },
};

static const int column[CLOCK_CELLS] = { 0, 6, 12, 14, 20 };

static void fill_block(uint32_t* pixels, int stride, int x, int y, int scale) {
    for (int row = 0; row < scale; row++) {
        uint32_t* dest = pixels + (y + row) * stride + x;

        for (int col = 0; col < scale; col++) {
            dest[col] = 0xffffffffu;
        }
    }
}

static void draw_digit(uint32_t* pixels, int stride, int x, int digit, int scale) {
    for (int row = 0; row < CLOCK_H; row++) {
        uint8_t bits = digits[digit][row];

        for (int col = 0; col < 5; col++) {
            if ((bits & (1u << (4 - col))) == 0) {
                continue;
            }

            fill_block(pixels, stride, x + col * scale, row * scale, scale);
        }
    }
}

int clock_delay_ms(void) {
    struct timespec now = {};
    struct tm tm = {};

    if (clock_gettime(CLOCK_REALTIME, &now) < 0 || !localtime_r(&now.tv_sec, &tm)) {
        return 60000;
    }

    int ms = (60 - tm.tm_sec) * 1000 - (int) (now.tv_nsec / 1000000L);
    if (ms < 1) {
        ms = 1;
    }

    return ms;
}

void clock_draw(uint32_t* pixels, int stride, int scale, const char text[CLOCK_TEXT]) {
    if (scale < 1) {
        return;
    }

    memset(pixels, 0, (size_t) stride * (size_t) (CLOCK_H * scale) * sizeof(uint32_t));

    for (int i = 0; i < CLOCK_CELLS; i++) {
        int x = column[i] * scale;
        if (text[i] == ':') {
            fill_block(pixels, stride, x, 2 * scale, scale);
            fill_block(pixels, stride, x, 4 * scale, scale);
            continue;
        }

        if (text[i] < '0' || text[i] > '9') {
            continue;
        }

        draw_digit(pixels, stride, x, text[i] - '0', scale);
    }
}

void clock_stamp(char text[CLOCK_TEXT]) {
    struct timespec now = {};
    struct tm tm = {};

    if (clock_gettime(CLOCK_REALTIME, &now) < 0 || !localtime_r(&now.tv_sec, &tm)) {
        memcpy(text, "--:--", sizeof("--:--"));
        return;
    }

    text[0] = (char) ('0' + tm.tm_hour / 10);
    text[1] = (char) ('0' + tm.tm_hour % 10);
    text[2] = ':';
    text[3] = (char) ('0' + tm.tm_min / 10);
    text[4] = (char) ('0' + tm.tm_min % 10);
    text[5] = '\0';
}
