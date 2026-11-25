#define _GNU_SOURCE

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#define LINE_MAX_LEN 256
#define FAIL_MS_MAX 60000

static int parse_color(const char* text, uint32_t* out) {
    if (text[0] == '#') {
        text++;
    } else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }

    if (strlen(text) != 6) {
        return -1;
    }

    char* end_ptr;

    errno = 0;

    unsigned long value = strtoul(text, &end_ptr, 16);
    if (end_ptr == text || *end_ptr || errno != 0 || value > 0xfffffful) {
        return -1;
    }

    *out = (uint32_t) value;
    return 0;
}

static int parse_service(const char* text, char* out) {
    size_t length = strlen(text);
    static const char allowed[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.";

    if (length == 0 || length >= SERVICE_MAX || strspn(text, allowed) != length) {
        return -1;
    }

    memcpy(out, text, length + 1);
    return 0;
}

static void apply_line(struct config* cfg, const char* path, int line, const char* key, const char* value) {
    if (strcmp(key, "idle") == 0 || strcmp(key, "input") == 0 || strcmp(key, "fail") == 0) {
        uint32_t color = 0;
        if (parse_color(value, &color) < 0) {
            warnx("%s:%d: bad color '%s'", path, line, value);
            return;
        }

        if (strcmp(key, "idle") == 0) {
            cfg->idle = color;
        } else if (strcmp(key, "input") == 0) {
            cfg->input = color;
        } else {
            cfg->fail = color;
        }

        return;
    }

    if (strcmp(key, "fail_ms") == 0) {
        char* end_ptr;

        errno = 0;

        long ms = strtol(value, &end_ptr, 10);
        if (end_ptr == value || *end_ptr || errno != 0 || ms < 1 || ms > FAIL_MS_MAX) {
            warnx("%s:%d: bad fail_ms '%s'", path, line, value);
            return;
        }

        cfg->fail_ms = (int) ms;
        return;
    }

    if (strcmp(key, "clock") == 0) {
        if (strcmp(value, "on") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "1") == 0) {
            cfg->clock = true;
        } else if (strcmp(value, "off") == 0 || strcmp(value, "no") == 0 || strcmp(value, "0") == 0) {
            cfg->clock = false;
        } else {
            warnx("%s:%d: bad clock '%s'", path, line, value);
        }
        return;
    }

    if (strcmp(key, "hours") == 0) {
        if (strcmp(value, "12") == 0 || strcmp(value, "ampm") == 0 || strcmp(value, "12h") == 0) {
            cfg->hours12 = true;
        } else if (strcmp(value, "24") == 0 || strcmp(value, "24h") == 0) {
            cfg->hours12 = false;
        } else {
            warnx("%s:%d: bad hours '%s'", path, line, value);
        }
        return;
    }

    if (strcmp(key, "service") == 0) {
        if (parse_service(value, cfg->service) < 0) {
            warnx("%s:%d: bad service '%s'", path, line, value);
        }
        return;
    }

    warnx("%s:%d: unknown key '%s'", path, line, key);
}

static int read_line(FILE* file, char* line, size_t size) {
    if (!fgets(line, (int) size, file)) {
        return 0;
    }

    if (strchr(line, '\n') != NULL || feof(file)) {
        return 1;
    }

    int ch = 0;
    while ((ch = fgetc(file)) != EOF && ch != '\n') { }

    line[0] = '\0';
    errno = EMSGSIZE;
    return -1;
}

void config_defaults(struct config* cfg) {
    cfg->idle = 0x000000u;
    cfg->input = 0x005577u;
    cfg->fail = 0xCC3333u;
    cfg->fail_ms = 1000;
    cfg->clock = true;
    cfg->hours12 = false;
    memcpy(cfg->service, "simple-lock", sizeof("simple-lock"));
}

int config_read(struct config* cfg, const char* path) {
    FILE* fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    char line[LINE_MAX_LEN];
    int number = 0;

    for (;;) {
        int got = read_line(fp, line, sizeof(line));
        if (got == 0) {
            break;
        }

        number++;
        if (got < 0) {
            warnx("%s:%d: line is too long", path, number);
            continue;
        }

        char* end = strpbrk(line, "\r\n");
        if (end) {
            *end = '\0';
        }

        char* text = line;
        while (*text == ' ' || *text == '\t') {
            text++;
        }

        if (*text == '\0' || *text == '#') {
            continue;
        }

        char key[32];
        char value[SERVICE_MAX];
        if (sscanf(text, "%31s %63s", key, value) != 2) {
            warnx("%s:%d: expected a key and a value", path, number);
            continue;
        }

        apply_line(cfg, path, number, key, value);
    }

    fclose(fp);
    return 0;
}

static int user_path(char* path, size_t size) {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");

    int wrote = 0;

    if (xdg && xdg[0] != '\0') {
        wrote = snprintf(path, size, "%s/simple-lock/config", xdg);
    } else if (home && home[0] != '\0') {
        wrote = snprintf(path, size, "%s/.config/simple-lock/config", home);
    } else {
        return -1;
    }

    if (wrote < 0 || (size_t) wrote >= size) {
        return -1;
    }

    return 0;
}

static int read_optional(struct config* cfg, const char* path) {
    if (config_read(cfg, path) == 0) {
        return 0;
    }

    if (errno != ENOENT) {
        warn("cannot open '%s'", path);
    }

    return -1;
}

int config_load(struct config* cfg, const char* path) {
    config_defaults(cfg);

    if (path) {
        if (config_read(cfg, path) < 0) {
            warn("cannot open '%s'", path);
            return -1;
        }

        return 0;
    }

    char user[PATH_MAX];
    if (user_path(user, sizeof(user)) == 0 && read_optional(cfg, user) == 0) {
        return 0;
    }

    return 0;
}
