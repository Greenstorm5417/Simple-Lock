#pragma once

#include <stdint.h>

enum {
    SERVICE_MAX = 64,
};

struct config {
    uint32_t idle;
    uint32_t input;
    uint32_t fail;
    int fail_ms;
    bool clock;
    bool hours12;
    char service[SERVICE_MAX];
};

void config_defaults(struct config* cfg);
int config_read(struct config* cfg, const char* path);
int config_load(struct config* cfg, const char* path);
