#define _GNU_SOURCE

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include <sys/mman.h>

#include <dlfcn.h>
#include <err.h>
#include <stdint.h>
#include <string.h>

#include "keymap.h"

#define KEY_CODE_MAX 256
#define KEY_GROUP_MAX 2
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
    uint8_t groups;
    bool ready;
    uint8_t kind[KEY_CODE_MAX];
    uint8_t len[KEY_GROUP_MAX][KEY_LEVEL_MAX][KEY_CODE_MAX];
    char text[KEY_GROUP_MAX][KEY_LEVEL_MAX][KEY_CODE_MAX][KEY_UTF8_MAX];
};

typedef struct xkb_context* (*context_new_fn)(enum xkb_context_flags);
typedef void (*context_unref_fn)(struct xkb_context*);
typedef struct xkb_keymap* (*keymap_new_fn)(struct xkb_context*, const char*, size_t, enum xkb_keymap_format, enum xkb_keymap_compile_flags);
typedef void (*keymap_unref_fn)(struct xkb_keymap*);
typedef xkb_mod_index_t (*mod_index_fn)(struct xkb_keymap*, const char*);
typedef xkb_layout_index_t (*num_layouts_fn)(struct xkb_keymap*);
typedef struct xkb_state* (*state_new_fn)(struct xkb_keymap*);
typedef void (*state_unref_fn)(struct xkb_state*);
typedef enum xkb_state_component (*update_mask_fn)(struct xkb_state*, xkb_mod_mask_t, xkb_mod_mask_t, xkb_mod_mask_t, xkb_layout_index_t, xkb_layout_index_t, xkb_layout_index_t);
typedef int (*key_utf8_fn)(struct xkb_state*, xkb_keycode_t, char*, size_t);
typedef xkb_keysym_t (*key_sym_fn)(struct xkb_state*, xkb_keycode_t);

struct xkb {
    context_new_fn context_new;
    context_unref_fn context_unref;
    keymap_new_fn keymap_new;
    keymap_unref_fn keymap_unref;
    mod_index_fn mod_index;
    num_layouts_fn num_layouts;
    state_new_fn state_new;
    state_unref_fn state_unref;
    update_mask_fn update_mask;
    key_utf8_fn key_utf8;
    key_sym_fn key_sym;
};

static const xkb_mod_mask_t depressed_bit[LEVEL_BLOCK] = { 0, 1, 0, 1, 2, 3 };
static const xkb_mod_mask_t locked_bit[LEVEL_BLOCK] = { 0, 0, 1, 1, 0, 0 };

alignas(4096)
static struct key_table live;

static bool load_fn(void* handle, const char* name, void* dest) {
    void* sym = dlsym(handle, name);
    if (!sym) {
        warnx("dlsym %s", name);
        return false;
    }

    memcpy(dest, &sym, sizeof(sym));

    return true;
}

static uint32_t mask_mod(struct xkb* api, struct xkb_keymap* map, const char* name) {
    xkb_mod_index_t index = api->mod_index(map, name);
    if (index == XKB_MOD_INVALID || index >= 32) {
        return 0;
    }

    return 1u << index;
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

static struct key_table* map_scratch(void) {
    struct key_table* scratch = mmap(NULL, sizeof(struct key_table), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (scratch == MAP_FAILED) {
        return NULL;
    }

    return scratch;
}

static void drop_scratch(struct key_table* scratch) {
    if (!scratch) {
        return;
    }

    explicit_bzero(scratch, sizeof(*scratch));
    munmap(scratch, sizeof(*scratch));
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

static bool load_xkb(void* handle, struct xkb* api) {
    if (!load_fn(handle, "xkb_context_new", &api->context_new)) {
        return false;
    }

    if (!load_fn(handle, "xkb_context_unref", &api->context_unref)) {
        return false;
    }

    if (!load_fn(handle, "xkb_keymap_new_from_buffer", &api->keymap_new)) {
        return false;
    }

    if (!load_fn(handle, "xkb_keymap_unref", &api->keymap_unref)) {
        return false;
    }

    if (!load_fn(handle, "xkb_keymap_mod_get_index", &api->mod_index)) {
        return false;
    }

    if (!load_fn(handle, "xkb_keymap_num_layouts", &api->num_layouts)) {
        return false;
    }

    if (!load_fn(handle, "xkb_state_new", &api->state_new)) {
        return false;
    }

    if (!load_fn(handle, "xkb_state_unref", &api->state_unref)) {
        return false;
    }

    if (!load_fn(handle, "xkb_state_update_mask", &api->update_mask)) {
        return false;
    }

    if (!load_fn(handle, "xkb_state_key_get_utf8", &api->key_utf8)) {
        return false;
    }

    if (!load_fn(handle, "xkb_state_key_get_one_sym", &api->key_sym)) {
        return false;
    }

    return true;
}

int keys_load(const char* text, size_t size) {
    struct xkb api = {};
    struct xkb_context* ctx = NULL;
    struct xkb_keymap* map = NULL;
    struct xkb_state* st = NULL;
    struct key_table* scratch = NULL;

    int ret = -1;

    void* so = dlopen("libxkbcommon.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!so) {
        warnx("%s", dlerror());
        return -1;
    }

    if (!load_xkb(so, &api)) {
        goto end;
    }

    ctx = api.context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) {
        warnx("xkb_context_new");
        goto end;
    }

    map = api.keymap_new(ctx, text, size, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!map) {
        warnx("xkb_keymap_new_from_buffer");
        goto end;
    }

    st = api.state_new(map);
    if (!st) {
        warnx("xkb_state_new");
        goto end;
    }

    scratch = map_scratch();
    if (!scratch) {
        warn("mmap");
        goto end;
    }

    scratch->shift = mask_mod(&api, map, XKB_MOD_NAME_SHIFT);
    scratch->lock = mask_mod(&api, map, XKB_MOD_NAME_CAPS);
    scratch->control = mask_mod(&api, map, XKB_MOD_NAME_CTRL);
    scratch->level3 = mask_mod(&api, map, XKB_MOD_NAME_MOD5);
    scratch->num = mask_mod(&api, map, XKB_MOD_NAME_NUM);

    xkb_layout_index_t layouts = api.num_layouts(map);
    if (layouts == 0) {
        layouts = 1;
    }
    if (layouts > KEY_GROUP_MAX) {
        layouts = KEY_GROUP_MAX;
    }
    scratch->groups = (uint8_t) layouts;

    for (xkb_layout_index_t group = 0; group < layouts; group++) {
        for (uint32_t code = 0; code < KEY_CODE_MAX; code++) {
            for (int level = 0; level < LEVEL_BLOCK; level++) {
                xkb_mod_mask_t depressed = 0;
                xkb_mod_mask_t locked = 0;

                if (depressed_bit[level] & 1) {
                    depressed |= scratch->shift;
                }
                if (depressed_bit[level] & 2) {
                    depressed |= scratch->level3;
                }
                if (locked_bit[level] & 1) {
                    locked |= scratch->lock;
                }

                api.update_mask(st, depressed, 0, locked, 0, 0, group);

                char buf[8];
                int written = api.key_utf8(st, code, buf, sizeof(buf));
                if (written > 0 && written <= KEY_UTF8_MAX && (unsigned char) buf[0] >= 0x20) {
                    scratch->len[group][level][code] = (uint8_t) written;
                    memcpy(scratch->text[group][level][code], buf, (size_t) written);
                }

                api.update_mask(st, depressed, 0, locked | scratch->num, 0, 0, group);
                written = api.key_utf8(st, code, buf, sizeof(buf));
                if (written > 0 && written <= KEY_UTF8_MAX && (unsigned char) buf[0] >= 0x20) {
                    scratch->len[group][level + LEVEL_BLOCK][code] = (uint8_t) written;
                    memcpy(scratch->text[group][level + LEVEL_BLOCK][code], buf, (size_t) written);
                }
            }
        }
    }

    api.update_mask(st, 0, 0, 0, 0, 0, 0);
    for (uint32_t code = 0; code < KEY_CODE_MAX; code++) {
        uint8_t kind = classify_sym(api.key_sym(st, code));
        if (kind == KEY_KIND_NONE) {
            for (xkb_layout_index_t group = 0; group < layouts; group++) {
                for (int level = 0; level < KEY_LEVEL_MAX; level++) {
                    if (scratch->len[group][level][code] > 0) {
                        kind = KEY_KIND_TEXT;
                        break;
                    }
                }

                if (kind == KEY_KIND_TEXT) {
                    break;
                }
            }
        }

        scratch->kind[code] = kind;
    }

    scratch->ready = true;
    live = *scratch;
    ret = 0;

end:
    if (st) {
        api.state_unref(st);
    }
    if (map) {
        api.keymap_unref(map);
    }
    if (ctx) {
        api.context_unref(ctx);
    }
    dlclose(so);
    drop_scratch(scratch);
    return ret;
}

bool keys_ready(void) {
    return live.ready;
}

uint8_t keys_kind(uint32_t code) {
    if (!live.ready || code >= KEY_CODE_MAX) {
        return KEY_KIND_NONE;
    }

    return live.kind[code];
}

bool keys_is_u(uint32_t code) {
    if (!live.ready || code >= KEY_CODE_MAX) {
        return false;
    }

    if (live.len[0][LEVEL_BASE][code] != 1) {
        return false;
    }

    char ch = live.text[0][LEVEL_BASE][code][0];

    return ch == 'u' || ch == 'U';
}

bool keys_ctrl(uint32_t depressed, uint32_t latched, uint32_t locked) {
    uint32_t mods = depressed | latched | locked;

    return live.control && (mods & live.control);
}

int keys_utf8(uint32_t code, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group, char* out, size_t out_size) {
    if (!live.ready || !out || out_size == 0 || code >= KEY_CODE_MAX) {
        return 0;
    }

    if (live.groups == 0) {
        return 0;
    }

    if (group >= live.groups) {
        group = 0;
    }

    int level = choose_level(&live, depressed, latched, locked);
    uint8_t len = live.len[group][level][code];
    if (len == 0 || (size_t) len >= out_size) {
        return 0;
    }

    memcpy(out, live.text[group][level][code], len);
    out[len] = '\0';

    return len;
}
