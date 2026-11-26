#pragma once

#include <stdint.h>

struct config {
    uint32_t idle;
    uint32_t input;
    uint32_t fail;
    int fail_ms;
    bool clock;
    bool hours12;
};

void config_defaults(struct config* config);
int config_load(struct config* config, const char* path);
