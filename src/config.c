#define _GNU_SOURCE

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#define FAIL_MS_MAX 60000

static void apply_line(struct config* config, const char* path, int line, const char* key, const char* value);
static int parse_color(const char* text, uint32_t* out);
static int read_config(struct config* config, const char* path);
static int read_optional(struct config* config, const char* path);
static int user_path(char* path, size_t size);

static void apply_line(struct config* config, const char* path, int line, const char* key, const char* value) {
    if (strcmp(key, "idle") == 0 || strcmp(key, "input") == 0 || strcmp(key, "fail") == 0) {
        uint32_t color = 0;
        if (parse_color(value, &color) < 0) {
            warnx("%s:%d: bad color '%s'", path, line, value);
            return;
        }

        if (strcmp(key, "idle") == 0) {
            config->idle = color;
        } else if (strcmp(key, "input") == 0) {
            config->input = color;
        } else {
            config->fail = color;
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

        config->fail_ms = (int) ms;
        return;
    }

    if (strcmp(key, "clock") == 0) {
        if (strcmp(value, "on") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "1") == 0) {
            config->clock = true;
        } else if (strcmp(value, "off") == 0 || strcmp(value, "no") == 0 || strcmp(value, "0") == 0) {
            config->clock = false;
        } else {
            warnx("%s:%d: bad clock '%s'", path, line, value);
        }
        return;
    }

    if (strcmp(key, "hours") == 0) {
        if (strcmp(value, "12") == 0 || strcmp(value, "ampm") == 0 || strcmp(value, "12h") == 0) {
            config->hours12 = true;
        } else if (strcmp(value, "24") == 0 || strcmp(value, "24h") == 0) {
            config->hours12 = false;
        } else {
            warnx("%s:%d: bad hours '%s'", path, line, value);
        }
        return;
    }

    warnx("%s:%d: unknown key '%s'", path, line, key);
}

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

    if (end_ptr == text || *end_ptr || errno != 0 || value > UINT32_MAX) {
        return -1;
    }

    *out = (uint32_t) value;
    return 0;
}

static int read_config(struct config* config, const char* path) {
    FILE* fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    char* line = NULL;
    size_t capacity = 0;
    ssize_t length;
    int number = 0;

    while ((length = getline(&line, &capacity, fp)) != -1) {
        number++;

        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = '\0';
        }

        char* text = line;

        while (*text == ' ' || *text == '\t') {
            text++;
        }

        if (*text == '\0' || *text == '#') {
            continue;
        }

        char key[32];
        char value[32];

        if (sscanf(text, "%31s %31s", key, value) != 2) {
            warnx("%s:%d: expected a key and a value", path, number);
            continue;
        }

        apply_line(config, path, number, key, value);
    }

    int saved_errno = errno;

    free(line);

    if (ferror(fp)) {
        fclose(fp);
        errno = saved_errno ? saved_errno : EIO;
        return -1;
    }

    fclose(fp);
    return 0;
}

static int read_optional(struct config* config, const char* path) {
    if (read_config(config, path) == 0) {
        return 0;
    }

    if (errno != ENOENT) {
        warn("cannot read '%s'", path);
    }

    return -1;
}

static int user_path(char* path, size_t size) {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");

    int wrote = 0;

    if (xdg && xdg[0] != '\0') {
        wrote = snprintf(path, size, "%s/kisslock/config", xdg);
    } else if (home && home[0] != '\0') {
        wrote = snprintf(path, size, "%s/.config/kisslock/config", home);
    } else {
        return -1;
    }

    if (wrote < 0 || (size_t) wrote >= size) {
        return -1;
    }

    return 0;
}

void config_defaults(struct config* config) {
    config->idle = 0x000000;
    config->input = 0x005577;
    config->fail = 0xcc3333;
    config->fail_ms = 1000;
    config->clock = true;
    config->hours12 = false;
}

int config_load(struct config* config, const char* path) {
    config_defaults(config);

    if (path) {
        if (read_config(config, path) < 0) {
            warn("cannot read '%s'", path);
            return -1;
        }

        return 0;
    }

    char user[PATH_MAX];

    if (user_path(user, sizeof(user)) == 0) {
        read_optional(config, user);
    }

    return 0;
}
