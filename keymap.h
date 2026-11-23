#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    KEY_KIND_NONE,
    KEY_KIND_TEXT,
    KEY_KIND_RETURN,
    KEY_KIND_BACKSPACE,
    KEY_KIND_ESCAPE,
};

int keys_load(const char* text, size_t size);
bool keys_ready(void);
uint8_t keys_kind(uint32_t code);
bool keys_is_u(uint32_t code);
bool keys_ctrl(uint32_t depressed, uint32_t latched, uint32_t locked);
int keys_utf8(uint32_t code, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group, char* out, size_t out_size);
