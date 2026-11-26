#define _GNU_SOURCE

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include <err.h>
#include <stdlib.h>
#include <string.h>

#include "keymap.h"

#define KEY_LEVEL_MAX 12
#define KEY_UTF8_MAX 4

#define LEVEL_BASE          0
#define LEVEL_SHIFT         1
#define LEVEL_CAPS          2
#define LEVEL_CAPS_SHIFT    3
#define LEVEL_ALT           4
#define LEVEL_ALT_SHIFT     5
#define LEVEL_BLOCK         6

struct key_table {
    uint32_t shift;
    uint32_t lock;
    uint32_t control;
    uint32_t level3;
    uint32_t num;

    xkb_keycode_t min_keycode;
    xkb_keycode_t max_keycode;

    size_t key_count;
    xkb_layout_index_t groups;

    uint8_t* kind;
    uint8_t* len;
    char* text;

    bool ready;
};

static const xkb_mod_mask_t depressed_bit[LEVEL_BLOCK] = { 0, 1, 0, 1, 2, 3 };
static const xkb_mod_mask_t locked_bit[LEVEL_BLOCK] = { 0, 0, 1, 1, 0, 0 };

static struct key_table live;

static int choose_level(const struct key_table* table, uint32_t depressed, uint32_t latched, uint32_t locked);
static uint8_t classify_sym(xkb_keysym_t sym);
static void build_table(struct key_table* table, struct xkb_state* state);
static bool create_table(struct key_table* table, xkb_keycode_t min, xkb_keycode_t max, xkb_layout_index_t groups);
static void destroy_table(struct key_table* table);
static void store_utf8(struct key_table* table, xkb_layout_index_t group, int level, size_t key, struct xkb_state* state, xkb_keycode_t code);

static inline size_t key_index(const struct key_table* table, xkb_keycode_t code) {
    return (size_t) (code - table->min_keycode);
}

static inline size_t entry_index(const struct key_table* table, xkb_layout_index_t group, int level, size_t key) {
    return ((size_t) group * KEY_LEVEL_MAX + (size_t) level) * table->key_count + key;
}

static inline uint32_t mask_mod(struct xkb_keymap* map, const char* name) {
    xkb_mod_index_t index = xkb_keymap_mod_get_index(map, name);
    if (index == XKB_MOD_INVALID || index >= 32) {
        return 0;
    }

    return UINT32_C(1) << index;
}

static int choose_level(const struct key_table* table, uint32_t depressed, uint32_t latched, uint32_t locked) {
    uint32_t mods = depressed | latched | locked;

    bool shift = table->shift && (mods & table->shift);
    bool caps = table->lock && (mods & table->lock);
    bool alt = table->level3 && (mods & table->level3);
    bool num = table->num && (mods & table->num);

    int level = LEVEL_BASE;

    if (alt) {
        level = shift ? LEVEL_ALT_SHIFT : LEVEL_ALT;
    } else if (shift && caps) {
        level = LEVEL_CAPS_SHIFT;
    } else if (shift) {
        level = LEVEL_SHIFT;
    } else if (caps) {
        level = LEVEL_CAPS;
    }

    if (num) {
        level += LEVEL_BLOCK;
    }

    return level;
}

static uint8_t classify_sym(xkb_keysym_t sym) {
    switch (sym) {
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
        case XKB_KEY_ISO_Enter:
            return KEY_KIND_RETURN;
        case XKB_KEY_BackSpace:
            return KEY_KIND_BACKSPACE;
        case XKB_KEY_Escape:
            return KEY_KIND_ESCAPE;
        default:
            return KEY_KIND_NONE;
    }
}

static void build_table(struct key_table* table, struct xkb_state* state) {
    for (xkb_layout_index_t group = 0; group < table->groups; group++) {
        for (size_t key = 0; key < table->key_count; key++) {
            xkb_keycode_t code = table->min_keycode + (xkb_keycode_t) key;

            for (int level = 0; level < LEVEL_BLOCK; level++) {
                xkb_mod_mask_t depressed = 0;
                xkb_mod_mask_t locked = 0;

                if (depressed_bit[level] & 1) {
                    depressed |= table->shift;
                }

                if (depressed_bit[level] & 2) {
                    depressed |= table->level3;
                }

                if (locked_bit[level] & 1) {
                    locked |= table->lock;
                }

                xkb_state_update_mask(state, depressed, 0, locked, 0, 0, group);
                store_utf8(table, group, level, key, state, code);

                xkb_state_update_mask(state, depressed, 0, locked | table->num, 0, 0, group);
                store_utf8(table, group, level + LEVEL_BLOCK, key, state, code);
            }
        }
    }

    xkb_state_update_mask(state, 0, 0, 0, 0, 0, 0);

    for (size_t key = 0; key < table->key_count; key++) {
        xkb_keycode_t code = table->min_keycode + (xkb_keycode_t) key;

        uint8_t kind = classify_sym(xkb_state_key_get_one_sym(state, code));
        if (kind == KEY_KIND_NONE) {
            for (xkb_layout_index_t group = 0; group < table->groups && kind == KEY_KIND_NONE; group++) {
                for (int level = 0; level < KEY_LEVEL_MAX; level++) {
                    size_t index = entry_index(table, group, level, key);
                    if (table->len[index]) {
                        kind = KEY_KIND_TEXT;
                        break;
                    }
                }
            }
        }

        table->kind[key] = kind;
    }
}

static bool create_table(struct key_table* table, xkb_keycode_t min, xkb_keycode_t max, xkb_layout_index_t groups) {
    uint64_t range = (uint64_t) max - min + 1;
    if (range > SIZE_MAX) {
        return false;
    }

    size_t key_count = (size_t) range;

    size_t entries;
    size_t text_size;

    if (__builtin_mul_overflow(key_count, (size_t) groups, &entries) ||
            __builtin_mul_overflow(entries, (size_t) KEY_LEVEL_MAX, &entries) ||
            __builtin_mul_overflow(entries, (size_t) KEY_UTF8_MAX, &text_size)) {
        return false;
    }

    table->kind = calloc(key_count, sizeof(*table->kind));
    table->len = calloc(entries, sizeof(*table->len));
    table->text = calloc(text_size, sizeof(*table->text));

    if (!table->kind || !table->len || !table->text) {
        destroy_table(table);
        return false;
    }

    table->min_keycode = min;
    table->max_keycode = max;
    table->key_count = key_count;
    table->groups = groups;

    return true;
}

static void destroy_table(struct key_table* table) {
    free(table->kind);
    free(table->len);
    free(table->text);

    memset(table, 0, sizeof(struct key_table));
}

static void store_utf8(struct key_table* table, xkb_layout_index_t group, int level, size_t key, struct xkb_state* state, xkb_keycode_t code) {
    char buf[KEY_UTF8_MAX + 1];

    int written = xkb_state_key_get_utf8(state, code, buf, sizeof(buf));
    if (written <= 0 || written > KEY_UTF8_MAX || (unsigned char) buf[0] < 0x20) {
        return;
    }

    size_t index = entry_index(table, group, level, key);

    table->len[index] = (uint8_t) written;

    memcpy(table->text + index * KEY_UTF8_MAX, buf, (size_t) written);
}

bool keys_ctrl(uint32_t depressed, uint32_t latched, uint32_t locked) {
    uint32_t mods = depressed | latched | locked;
    return live.ready && live.control && (mods & live.control);
}

bool keys_is_u(uint32_t code) {
    if (!live.ready || code < live.min_keycode || code > live.max_keycode) {
        return false;
    }

    size_t key = key_index(&live, code);
    size_t index = entry_index(&live, 0, LEVEL_BASE, key);

    if (live.len[index] != 1) {
        return false;
    }

    char ch = live.text[index * KEY_UTF8_MAX];
    return ch == 'u' || ch == 'U';
}

uint8_t keys_kind(uint32_t code) {
    if (!live.ready || code < live.min_keycode || code > live.max_keycode) {
        return KEY_KIND_NONE;
    }

    return live.kind[key_index(&live, code)];
}

int keys_load(const char* text, size_t size) {
    if (!text || size == 0) {
        return -1;
    }

    struct xkb_context* ctx = NULL;
    struct xkb_keymap* map = NULL;
    struct xkb_state* state = NULL;

    struct key_table next = {};

    int ret = -1;

    ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) {
        warnx("xkb_context_new");
        goto end;
    }

    map = xkb_keymap_new_from_buffer(ctx, text, size, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!map) {
        warnx("xkb_keymap_new_from_buffer");
        goto end;
    }

    state = xkb_state_new(map);
    if (!state) {
        warnx("xkb_state_new");
        goto end;
    }

    next.shift = mask_mod(map, XKB_MOD_NAME_SHIFT);
    next.lock = mask_mod(map, XKB_MOD_NAME_CAPS);
    next.control = mask_mod(map, XKB_MOD_NAME_CTRL);
    next.level3 = mask_mod(map, XKB_MOD_NAME_MOD5);
    next.num = mask_mod(map, XKB_MOD_NAME_NUM);

    xkb_layout_index_t groups = xkb_keymap_num_layouts(map);
    if (groups == 0) {
        groups = 1;
    }

    xkb_keycode_t min = xkb_keymap_min_keycode(map);
    xkb_keycode_t max = xkb_keymap_max_keycode(map);

    if (min > max || !create_table(&next, min, max, groups)) {
        warnx("keymap allocation failed");
        goto end;
    }

    build_table(&next, state);

    next.ready = true;

    destroy_table(&live);
    live = next;
    memset(&next, 0, sizeof(next));

    ret = 0;

end:
    if (state) {
        xkb_state_unref(state);
    }
    if (map) {
        xkb_keymap_unref(map);
    }
    if (ctx) {
        xkb_context_unref(ctx);
    }

    destroy_table(&next);
    return ret;
}

bool keys_ready(void) {
    return live.ready;
}

int keys_utf8(uint32_t code, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group, char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return 0;
    }

    out[0] = '\0';

    if (!live.ready || code < live.min_keycode || code > live.max_keycode) {
        return 0;
    }

    if (group >= live.groups) {
        group = 0;
    }

    size_t key = key_index(&live, code);
    int level = choose_level(&live, depressed, latched, locked);

    size_t index = entry_index(&live, group, level, key);
    uint8_t len = live.len[index];

    if (len == 0 || (size_t) len >= out_size) {
        return 0;
    }

    memcpy(out, live.text + index * KEY_UTF8_MAX, len);
    out[len] = '\0';

    return len;
}
