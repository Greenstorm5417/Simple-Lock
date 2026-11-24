#define _GNU_SOURCE

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "clock.h"

enum {
    GLYPH_W = 5,
    GLYPH_GAP = 1,
    SPACE_W = 3,
    COLON_W = 1,
};

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

static const uint8_t letter_a[CLOCK_H] = { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 };
static const uint8_t letter_m[CLOCK_H] = { 0x11, 0x1b, 0x15, 0x11, 0x11, 0x11, 0x11 };
static const uint8_t letter_p[CLOCK_H] = { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 };

static int measure_text(const char* text) {
    int pen = 0;

    for (int i = 0; text[i] != '\0'; i++) {
        if (text[i] == ' ') {
            pen += SPACE_W;
            continue;
        }
        if (i > 0 && text[i - 1] != ' ') {
            pen += GLYPH_GAP;
        }
        pen += text[i] == ':' ? COLON_W : GLYPH_W;
    }

    return pen;
}

static void fill_block(uint32_t* pixels, int stride, int x, int y, int scale) {
    for (int row = 0; row < scale; row++) {
        uint32_t* dest = pixels + (y + row) * stride + x;
        for (int col = 0; col < scale; col++) {
            dest[col] = 0xffffffffu;
        }
    }
}

static void draw_bits(uint32_t* pixels, int stride, int x, int scale, const uint8_t bits[CLOCK_H]) {
    for (int row = 0; row < CLOCK_H; row++) {
        for (int col = 0; col < GLYPH_W; col++) {
            if ((bits[row] & (1u << (4 - col))) == 0) {
                continue;
            }
            fill_block(pixels, stride, x + col * scale, row * scale, scale);
        }
    }
}

static const uint8_t* find_letter(char ch) {
    if (ch == 'A') {
        return letter_a;
    }
    if (ch == 'M') {
        return letter_m;
    }
    if (ch == 'P') {
        return letter_p;
    }
    return NULL;
}

int clock_cols(bool twelve) {
    return measure_text(twelve ? "00:00 AM" : "00:00");
}

void clock_stamp(char text[CLOCK_TEXT], bool twelve) {
    struct timespec now = {0};
    struct tm tm = {0};
    int hour = 0;
    bool pm = false;

    memset(text, 0, CLOCK_TEXT);
    if (clock_gettime(CLOCK_REALTIME, &now) < 0 || localtime_r(&now.tv_sec, &tm) == NULL) {
        if (twelve) {
            memcpy(text, "--:-- --", sizeof("--:-- --"));
        } else {
            memcpy(text, "--:--", sizeof("--:--"));
        }
        return;
    }

    hour = tm.tm_hour;
    if (twelve) {
        pm = hour >= 12;
        hour %= 12;
        if (hour == 0) {
            hour = 12;
        }
    }

    text[0] = (char) ('0' + hour / 10);
    text[1] = (char) ('0' + hour % 10);
    text[2] = ':';
    text[3] = (char) ('0' + tm.tm_min / 10);
    text[4] = (char) ('0' + tm.tm_min % 10);
    if (twelve) {
        text[5] = ' ';
        text[6] = pm ? 'P' : 'A';
        text[7] = 'M';
    }
}

int clock_delay_ms(void) {
    struct timespec now = {0};
    struct tm tm = {0};

    if (clock_gettime(CLOCK_REALTIME, &now) < 0 || localtime_r(&now.tv_sec, &tm) == NULL) {
        return 60000;
    }

    int ms = (60 - tm.tm_sec) * 1000 - (int) (now.tv_nsec / 1000000L);
    if (ms < 1) {
        ms = 1;
    }
    return ms;
}

void clock_draw(uint32_t* pixels, int stride, int scale, const char text[CLOCK_TEXT]) {
    int pen = 0;

    if (scale < 1) {
        return;
    }

    memset(pixels, 0, (size_t) stride * (size_t) (CLOCK_H * scale) * sizeof(uint32_t));

    for (int i = 0; text[i] != '\0'; i++) {
        char ch = text[i];

        if (ch == ' ') {
            pen += SPACE_W * scale;
            continue;
        }
        if (i > 0 && text[i - 1] != ' ') {
            pen += GLYPH_GAP * scale;
        }
        if (ch == ':') {
            fill_block(pixels, stride, pen, 2 * scale, scale);
            fill_block(pixels, stride, pen, 4 * scale, scale);
            pen += COLON_W * scale;
            continue;
        }
        if (ch >= '0' && ch <= '9') {
            draw_bits(pixels, stride, pen, scale, digits[ch - '0']);
            pen += GLYPH_W * scale;
            continue;
        }

        const uint8_t* bits = find_letter(ch);
        if (!bits) {
            pen += GLYPH_W * scale;
            continue;
        }
        draw_bits(pixels, stride, pen, scale, bits);
        pen += GLYPH_W * scale;
    }
}
