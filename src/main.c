#define _GNU_SOURCE

#include <wayland-client.h>

#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/timerfd.h>

#include <err.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth.h"
#include "clock.h"
#include "config.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "keymap.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

#define OUTPUT_MAX 16
#define SEAT_MAX 4
#define PASSWORD_MAX 256
#define KEYMAP_MAX (1024 * 1024)
#define CLOCK_STORE_MAX 4

enum shade {
    SHADE_IDLE,
    SHADE_INPUT,
    SHADE_FAIL,
};

struct output {
    struct wl_output* output;
    struct wl_surface* surface;
    struct ext_session_lock_surface_v1* lock_surface;
    struct wp_viewport* viewport;
    struct wl_buffer* buffer;
    uint32_t name;
    uint32_t serial;
    int32_t width;
    int32_t height;
    bool configured;
    bool ack;
    struct wl_surface* clock_surface;
    struct wl_subsurface* clock_sub;
    struct wp_viewport* clock_viewport;
    struct wl_buffer* clock_buffer;
    uint32_t* clock_pixels;
    int32_t clock_dest_w;
    int32_t clock_dest_h;
    int clock_scale;
};

struct clock_store {
    int scale;
    int width;
    int height;
    size_t size;
    uint32_t* pixels;
    struct wl_shm_pool* pool;
    int users;
};

struct seat {
    struct wl_seat* seat;
    struct wl_keyboard* keyboard;
    uint32_t name;
    uint32_t version;
    int32_t repeat_rate;
    int32_t repeat_delay;
    uint32_t depressed;
    uint32_t latched;
    uint32_t locked_mods;
    uint32_t group;
};

static void handle_configure(void* data, struct ext_session_lock_surface_v1* surface, uint32_t serial, uint32_t width, uint32_t height);
static void handle_locked(void* data, struct ext_session_lock_v1* lock);
static void handle_finished(void* data, struct ext_session_lock_v1* lock);
static void handle_output_geometry(void* data, struct wl_output* wl_output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char* make, const char* model, int32_t transform);
static void handle_output_mode(void* data, struct wl_output* wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh);
static void handle_keymap(void* data, struct wl_keyboard* keyboard, uint32_t format, int32_t fd, uint32_t size);
static void handle_keyboard_enter(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface, struct wl_array* keys);
static void handle_keyboard_leave(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface);
static void handle_keyboard_key(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
static void handle_modifiers(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked_mods, uint32_t group);
static void handle_repeat_info(void* data, struct wl_keyboard* keyboard, int32_t rate, int32_t delay);
static void handle_seat_capabilities(void* data, struct wl_seat* wl_seat, uint32_t capabilities);
static void handle_seat_name(void* data, struct wl_seat* wl_seat, const char* name);
static void handle_shm_format(void* data, struct wl_shm* wl_shm, uint32_t format);
static void handle_registry(void* data, struct wl_registry* source, uint32_t name, const char* interface, uint32_t version);
static void handle_registry_remove(void* data, struct wl_registry* source, uint32_t name);

static struct wl_display* display;
static struct wl_registry* registry;
static struct wl_compositor* compositor;
static struct ext_session_lock_manager_v1* lock_manager;
static struct ext_session_lock_v1* session_lock;
static struct wp_viewporter* viewporter;
static struct wp_single_pixel_buffer_manager_v1* single_pixel;
static struct wl_shm* shm;
static struct wl_subcompositor* subcompositor;
static bool shm_argb;

static struct output outputs[OUTPUT_MAX];
static struct seat seats[SEAT_MAX];

static struct config cfg;
static char password[PASSWORD_MAX];
static size_t password_len;
static enum shade shade = SHADE_IDLE;
static bool session_locked;
static bool stop;
static bool submit;
static bool want_unlock;
static int exit_status = EXIT_SUCCESS;

static int repeat_fd = -1;
static int fail_fd = -1;
static int clock_fd = -1;
static bool clock_broken;
static struct clock_store clock_stores[CLOCK_STORE_MAX];
static struct seat* repeat_seat;
static uint32_t repeat_key;
static bool repeat_down;

static const struct ext_session_lock_surface_v1_listener lock_surface_listener = {
    .configure = handle_configure,
};

static const struct ext_session_lock_v1_listener lock_listener = {
    .locked = handle_locked,
    .finished = handle_finished,
};

static const struct wl_output_listener output_listener = {
    .geometry = handle_output_geometry,
    .mode = handle_output_mode,
};

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = handle_keymap,
    .enter = handle_keyboard_enter,
    .leave = handle_keyboard_leave,
    .key = handle_keyboard_key,
    .modifiers = handle_modifiers,
    .repeat_info = handle_repeat_info,
};

static const struct wl_seat_listener seat_listener = {
    .capabilities = handle_seat_capabilities,
    .name = handle_seat_name,
};

static const struct wl_shm_listener shm_listener = {
    .format = handle_shm_format,
};

static const struct wl_registry_listener registry_listener = {
    .global = handle_registry,
    .global_remove = handle_registry_remove,
};

static void usage(void) {
    fprintf(stderr, "usage: kisslock [-c file]\n");
    exit(EXIT_FAILURE);
}

static void wipe_password(void) {
    password_len = 0;
    explicit_bzero(password, sizeof(password));
}

static void arm_timer(int fd, int delay_ms, int interval_ms) {
    struct itimerspec spec = {};

    if (delay_ms > 0) {
        spec.it_value.tv_sec = delay_ms / 1000;
        spec.it_value.tv_nsec = (delay_ms % 1000) * 1000000L;
    }
    if (interval_ms > 0) {
        spec.it_interval.tv_sec = interval_ms / 1000;
        spec.it_interval.tv_nsec = (interval_ms % 1000) * 1000000L;
    }

    if (timerfd_settime(fd, 0, &spec, NULL) < 0) {
        err(EXIT_FAILURE, "timerfd_settime");
    }
}

static void drain_timer(int fd) {
    uint64_t expirations;
    ssize_t nread = read(fd, &expirations, sizeof(expirations));
    if (nread < 0 && errno != EAGAIN) {
        err(EXIT_FAILURE, "read");
    }
}

static void ignore_signals(void) {
    struct sigaction sa = { .sa_handler = SIG_IGN, .sa_flags = 0 };

    sigemptyset(&sa.sa_mask);

    int ignored[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU, SIGPIPE };

    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++) {
        if (sigaction(ignored[i], &sa, NULL) < 0) {
            err(EXIT_FAILURE, "sigaction");
        }
    }
}

static void pop_password(void) {
    if (password_len == 0) {
        return;
    }

    size_t end = password_len;

    do {
        password_len--;
    } while (password_len > 0 && ((unsigned char) password[password_len] & 0xc0) == 0x80);

    explicit_bzero(password + password_len, end - password_len);
}

static void push_password(const char* text) {
    size_t length = strlen(text);
    if (length == 0 || (unsigned char) text[0] < 0x20 || password_len + length >= PASSWORD_MAX) {
        return;
    }

    memcpy(password + password_len, text, length);
    password_len += length;
    password[password_len] = '\0';
}

static void release_clock_store(int scale);

static void drop_clock(struct output* out) {
    if (out->clock_sub) {
        wl_subsurface_destroy(out->clock_sub);
        out->clock_sub = NULL;
    }
    if (out->clock_viewport) {
        wp_viewport_destroy(out->clock_viewport);
        out->clock_viewport = NULL;
    }
    if (out->clock_buffer) {
        wl_buffer_destroy(out->clock_buffer);
        out->clock_buffer = NULL;
        release_clock_store(out->clock_scale);
        out->clock_scale = 0;
        out->clock_pixels = NULL;
    }
    if (out->clock_surface) {
        wl_surface_destroy(out->clock_surface);
        out->clock_surface = NULL;
    }
}

static void release_clock_store(int scale) {
    for (int i = 0; i < CLOCK_STORE_MAX; i++) {
        struct clock_store* store = &clock_stores[i];
        if (store->scale != scale || !store->pool) {
            continue;
        }

        if (store->users > 0) {
            store->users--;
        }
        if (store->users == 0) {
            wl_shm_pool_destroy(store->pool);
            munmap(store->pixels, store->size);
            *store = (struct clock_store) {0};
        }
        return;
    }
}

static int clock_scale_for(const struct output* out) {
    int scale = out->height / (CLOCK_H * 12);
    int width_limit = out->width > 32 ? (out->width - 32) / clock_cols(cfg.hours12) : 1;
    int height_limit = out->height / CLOCK_H;

    if (scale < 6) {
        scale = 6;
    }
    if (scale > 32) {
        scale = 32;
    }
    if (width_limit < 1) {
        width_limit = 1;
    }
    if (height_limit < 1) {
        height_limit = 1;
    }
    if (scale > width_limit) {
        scale = width_limit;
    }
    if (scale > height_limit) {
        scale = height_limit;
    }
    return scale;
}

static bool ensure_clock(struct output* out) {
    if (out->clock_surface) {
        return true;
    }
    if (clock_broken || !compositor || !shm || !subcompositor || !viewporter) {
        return false;
    }

    struct wl_surface* surface = wl_compositor_create_surface(compositor);
    if (!surface) {
        warnx("wl_compositor_create_surface");
        clock_broken = true;
        return false;
    }

    struct wl_subsurface* sub = wl_subcompositor_get_subsurface(subcompositor, surface, out->surface);
    if (!sub) {
        warnx("wl_subcompositor_get_subsurface");
        wl_surface_destroy(surface);
        clock_broken = true;
        return false;
    }

    struct wp_viewport* viewport = wp_viewporter_get_viewport(viewporter, surface);
    if (!viewport) {
        warnx("wp_viewporter_get_viewport");
        wl_subsurface_destroy(sub);
        wl_surface_destroy(surface);
        clock_broken = true;
        return false;
    }

    wl_subsurface_set_desync(sub);
    out->clock_surface = surface;
    out->clock_sub = sub;
    out->clock_viewport = viewport;
    return true;
}

static struct clock_store* take_clock_store(int scale) {
    struct clock_store* slot = NULL;

    for (int i = 0; i < CLOCK_STORE_MAX; i++) {
        if (clock_stores[i].pool && clock_stores[i].scale == scale) {
            return &clock_stores[i];
        }
        if (!clock_stores[i].pool && !slot) {
            slot = &clock_stores[i];
        }
    }
    if (!slot) {
        warnx("clock scale limit");
        return NULL;
    }

    int width = clock_cols(cfg.hours12) * scale;
    int height = CLOCK_H * scale;
    size_t size = (size_t) width * (size_t) height * sizeof(uint32_t);
    if (size > (size_t) INT32_MAX) {
        warnx("clock buffer");
        return NULL;
    }

    int fd = memfd_create("kisslock-clock", MFD_CLOEXEC);
    if (fd < 0) {
        warn("memfd_create");
        return NULL;
    }
    if (ftruncate(fd, (off_t) size) < 0) {
        warn("ftruncate");
        close(fd);
        return NULL;
    }

    uint32_t* pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        warn("mmap");
        close(fd);
        return NULL;
    }

    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, (int32_t) size);
    close(fd);
    if (!pool) {
        warnx("wl_shm_create_pool");
        munmap(pixels, size);
        return NULL;
    }

    slot->scale = scale;
    slot->width = width;
    slot->height = height;
    slot->size = size;
    slot->pixels = pixels;
    slot->pool = pool;
    slot->users = 0;
    return slot;
}

static bool ensure_clock_buffer(struct output* out, int scale) {
    if (out->clock_buffer && out->clock_pixels && out->clock_scale == scale) {
        return true;
    }
    if (clock_broken) {
        return false;
    }

    if (out->clock_buffer) {
        wl_buffer_destroy(out->clock_buffer);
        out->clock_buffer = NULL;
        release_clock_store(out->clock_scale);
        out->clock_scale = 0;
        out->clock_pixels = NULL;
    }

    struct clock_store* store = take_clock_store(scale);
    if (!store) {
        clock_broken = true;
        return false;
    }

    struct wl_buffer* buffer = wl_shm_pool_create_buffer(store->pool, 0, store->width, store->height, store->width * (int) sizeof(uint32_t), WL_SHM_FORMAT_ARGB8888);
    if (!buffer) {
        warnx("wl_shm_pool_create_buffer");
        if (store->users == 0) {
            release_clock_store(scale);
        }
        clock_broken = true;
        return false;
    }

    store->users++;
    out->clock_buffer = buffer;
    out->clock_pixels = store->pixels;
    out->clock_scale = scale;
    return true;
}

static void place_clock(struct output* out) {
    if (!cfg.clock || !shm_argb || !ensure_clock(out)) {
        return;
    }

    int scale = clock_scale_for(out);
    if (!ensure_clock_buffer(out, scale)) {
        return;
    }

    char text[CLOCK_TEXT];
    clock_stamp(text, cfg.hours12);
    int dest_w = clock_cols(cfg.hours12) * scale;
    int dest_h = CLOCK_H * scale;
    clock_draw(out->clock_pixels, dest_w, scale, text);

    int x = (out->width - dest_w) / 2;
    int y = out->height / 12;
    if (x < 0) {
        x = 0;
    }
    if (y + dest_h > out->height) {
        y = 0;
    }

    out->clock_dest_w = dest_w;
    out->clock_dest_h = dest_h;
    wl_subsurface_set_position(out->clock_sub, x, y);
    wp_viewport_set_source(out->clock_viewport, 0, 0, wl_fixed_from_int(dest_w), wl_fixed_from_int(dest_h));
    wp_viewport_set_destination(out->clock_viewport, dest_w, dest_h);
    wl_surface_attach(out->clock_surface, out->clock_buffer, 0, 0);
    wl_surface_damage(out->clock_surface, 0, 0, dest_w, dest_h);
    wl_surface_commit(out->clock_surface);
}

static void refresh_clocks(void) {
    char text[CLOCK_TEXT];
    clock_stamp(text, cfg.hours12);

    for (int i = 0; i < CLOCK_STORE_MAX; i++) {
        struct clock_store* store = &clock_stores[i];
        if (!store->pixels || store->users == 0) {
            continue;
        }

        clock_draw(store->pixels, store->width, store->scale, text);
    }

    for (int i = 0; i < OUTPUT_MAX; i++) {
        struct output* out = &outputs[i];
        if (!out->clock_surface || !out->clock_buffer) {
            continue;
        }

        wl_surface_attach(out->clock_surface, out->clock_buffer, 0, 0);
        wl_surface_damage(out->clock_surface, 0, 0, out->clock_dest_w, out->clock_dest_h);
        wl_surface_commit(out->clock_surface);
    }

    if (clock_fd >= 0) {
        arm_timer(clock_fd, clock_delay_ms(), 0);
    }
}

static void present(struct output* out) {
    if (!out->configured || !out->lock_surface || !single_pixel) {
        return;
    }

    if (out->ack) {
        ext_session_lock_surface_v1_ack_configure(out->lock_surface, out->serial);
        out->ack = false;
    }

    wp_viewport_set_destination(out->viewport, out->width, out->height);

    uint32_t rgb = cfg.idle;

    if (shade == SHADE_INPUT) {
        rgb = cfg.input;
    } else if (shade == SHADE_FAIL) {
        rgb = cfg.fail;
    }

    uint32_t red = ((rgb >> 16) & 0xffu) * 0x01010101u;
    uint32_t green = ((rgb >> 8) & 0xffu) * 0x01010101u;
    uint32_t blue = (rgb & 0xffu) * 0x01010101u;
    struct wl_buffer* buffer = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(single_pixel, red, green, blue, 0xffffffffu);
    if (!buffer) {
        errx(EXIT_FAILURE, "wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer");
    }

    wl_surface_attach(out->surface, buffer, 0, 0);
    wl_surface_damage(out->surface, 0, 0, out->width, out->height);
    place_clock(out);
    wl_surface_commit(out->surface);

    if (out->buffer) {
        wl_buffer_destroy(out->buffer);
    }
    out->buffer = buffer;
}

static void present_all(void) {
    for (int i = 0; i < OUTPUT_MAX; i++) {
        if (outputs[i].lock_surface) {
            present(&outputs[i]);
        }
    }
}

static void show_input(void) {
    if (shade == SHADE_FAIL && fail_fd >= 0) {
        arm_timer(fail_fd, 0, 0);
    }

    enum shade next = password_len > 0 ? SHADE_INPUT : SHADE_IDLE;
    if (next == shade) {
        return;
    }

    shade = next;
    present_all();
}

static void show_fail(void) {
    wipe_password();
    shade = SHADE_FAIL;
    present_all();

    if (fail_fd >= 0) {
        arm_timer(fail_fd, cfg.fail_ms, 0);
    }
}

static void end_fail(void) {
    if (shade != SHADE_FAIL) {
        return;
    }

    shade = SHADE_IDLE;
    present_all();
}

static void drop_keyboard(struct seat* seat) {
    if (repeat_seat == seat) {
        repeat_seat = NULL;
        repeat_down = false;
        if (repeat_fd >= 0) {
            arm_timer(repeat_fd, 0, 0);
        }
    }

    if (seat->keyboard) {
        if (seat->version >= 3) {
            wl_keyboard_release(seat->keyboard);
        } else {
            wl_keyboard_destroy(seat->keyboard);
        }
        seat->keyboard = NULL;
    }
}

static void destroy_output(struct output* out) {
    drop_clock(out);
    if (out->lock_surface) {
        ext_session_lock_surface_v1_destroy(out->lock_surface);
    }
    if (out->viewport) {
        wp_viewport_destroy(out->viewport);
    }
    if (out->buffer) {
        wl_buffer_destroy(out->buffer);
    }
    if (out->surface) {
        wl_surface_destroy(out->surface);
    }
    if (out->output) {
        wl_output_destroy(out->output);
    }
    *out = (struct output) {0};
}

static void destroy_seat(struct seat* seat) {
    drop_keyboard(seat);

    if (seat->seat) {
        wl_seat_destroy(seat->seat);
    }
    *seat = (struct seat) {0};
}

static bool should_repeat(struct seat* seat, uint32_t key) {
    if (seat->repeat_rate <= 0 || seat->repeat_delay <= 0 || !keys_ready()) {
        return false;
    }

    uint32_t code = key + 8;
    uint8_t kind = keys_kind(code);
    if (kind == KEY_KIND_BACKSPACE) {
        return true;
    }
    if (kind != KEY_KIND_TEXT) {
        return false;
    }

    char text[8];

    int written = keys_utf8(code, seat->depressed, seat->latched, seat->locked_mods, seat->group, text, sizeof(text));
    return written > 0 && (unsigned char) text[0] >= 0x20;
}

static void input_key(struct seat* seat, uint32_t key, bool repeat) {
    if (!session_locked || !keys_ready()) {
        return;
    }

    uint32_t code = key + 8;
    uint8_t kind = keys_kind(code);
    bool ctrl = keys_ctrl(seat->depressed, seat->latched, seat->locked_mods);

    if (kind == KEY_KIND_RETURN) {
        if (!repeat && password_len > 0) {
            submit = true;
        }
        return;
    }

    if (kind == KEY_KIND_ESCAPE || (ctrl && (keys_is_u(code) || kind == KEY_KIND_BACKSPACE))) {
        if (!repeat) {
            wipe_password();
            show_input();
        }
        return;
    }

    if (kind == KEY_KIND_BACKSPACE) {
        pop_password();
        show_input();
        return;
    }

    if (ctrl || kind != KEY_KIND_TEXT) {
        return;
    }

    char text[8];
    int written = keys_utf8(code, seat->depressed, seat->latched, seat->locked_mods, seat->group, text, sizeof(text));
    if (written <= 0) {
        return;
    }

    push_password(text);
    show_input();
}

static void handle_configure(void* data, struct ext_session_lock_surface_v1* surface, uint32_t serial, uint32_t width, uint32_t height) {
    (void) surface;

    struct output* out = data;

    if (width == 0 || height == 0 || width > (uint32_t) INT32_MAX || height > (uint32_t) INT32_MAX) {
        return;
    }

    out->serial = serial;
    out->width = (int32_t) width;
    out->height = (int32_t) height;
    out->configured = true;
    out->ack = true;
    present(out);
}

static void handle_locked(void* data, struct ext_session_lock_v1* lock) {
    (void) data;
    (void) lock;

    session_locked = true;
}

static void handle_finished(void* data, struct ext_session_lock_v1* lock) {
    (void) data;
    (void) lock;

    if (!session_lock) {
        return;
    }

    if (session_locked) {
        ext_session_lock_v1_unlock_and_destroy(session_lock);
    } else {
        ext_session_lock_v1_destroy(session_lock);
        warnx("compositor rejected the lock");
    }
    session_lock = NULL;
    session_locked = false;
    exit_status = EXIT_FAILURE;
    stop = true;
}

static void handle_output_geometry(void* data, struct wl_output* wl_output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char* make, const char* model, int32_t transform) {
    (void) data;
    (void) wl_output;
    (void) x;
    (void) y;
    (void) physical_width;
    (void) physical_height;
    (void) subpixel;
    (void) make;
    (void) model;
    (void) transform;
}

static void handle_output_mode(void* data, struct wl_output* wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    (void) data;
    (void) wl_output;
    (void) flags;
    (void) width;
    (void) height;
    (void) refresh;
}

static void handle_keymap(void* data, struct wl_keyboard* keyboard, uint32_t format, int32_t fd, uint32_t size) {
    (void) data;
    (void) keyboard;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        warnx("unsupported keymap format %u", format);
        return;
    }

    if (size == 0 || size > KEYMAP_MAX) {
        close(fd);
        warnx("keymap size %u is unsupported", size);
        return;
    }

    char* map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        warn("mmap");
        return;
    }

    keys_load(map, size);
    munmap(map, size);
}

static void handle_keyboard_enter(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface, struct wl_array* keys) {
    (void) data;
    (void) keyboard;
    (void) serial;
    (void) surface;
    (void) keys;
}

static void handle_keyboard_leave(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface) {
    (void) keyboard;
    (void) serial;
    (void) surface;

    struct seat* seat = data;
    if (repeat_seat == seat) {
        repeat_seat = NULL;
        repeat_down = false;
        if (repeat_fd >= 0) {
            arm_timer(repeat_fd, 0, 0);
        }
    }
}

static void handle_keyboard_key(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void) keyboard;
    (void) serial;
    (void) time;

    struct seat* seat = data;
    bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    if (!pressed) {
        if (repeat_down && repeat_seat == seat && repeat_key == key) {
            repeat_seat = NULL;
            repeat_down = false;
            if (repeat_fd >= 0) {
                arm_timer(repeat_fd, 0, 0);
            }
        }
        return;
    }

    input_key(seat, key, false);

    if (should_repeat(seat, key)) {
        repeat_seat = seat;
        repeat_key = key;
        repeat_down = true;

        int interval = 1000 / seat->repeat_rate;
        if (interval < 1) {
            interval = 1;
        }
        arm_timer(repeat_fd, seat->repeat_delay, interval);
        return;
    }

    repeat_seat = NULL;
    repeat_down = false;
    if (repeat_fd >= 0) {
        arm_timer(repeat_fd, 0, 0);
    }
}

static void handle_modifiers(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked_mods, uint32_t group) {
    (void) keyboard;
    (void) serial;

    struct seat* seat = data;

    seat->depressed = depressed;
    seat->latched = latched;
    seat->locked_mods = locked_mods;
    seat->group = group;
}

static void handle_repeat_info(void* data, struct wl_keyboard* keyboard, int32_t rate, int32_t delay) {
    (void) keyboard;

    struct seat* seat = data;

    seat->repeat_rate = rate > 0 ? rate : 0;
    seat->repeat_delay = delay > 0 ? delay : 0;
}

static void handle_seat_capabilities(void* data, struct wl_seat* wl_seat, uint32_t capabilities) {
    (void) wl_seat;

    struct seat* seat = data;

    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        if (!seat->keyboard) {
            seat->keyboard = wl_seat_get_keyboard(seat->seat);
            if (seat->keyboard) {
                wl_keyboard_add_listener(seat->keyboard, &keyboard_listener, seat);
            }
        }
    } else {
        drop_keyboard(seat);
    }
}

static void handle_seat_name(void* data, struct wl_seat* wl_seat, const char* name) {
    (void) data;
    (void) wl_seat;
    (void) name;
}

static void map_output(struct output* out) {
    if (out->surface || !session_lock || !compositor || !viewporter) {
        return;
    }

    out->surface = wl_compositor_create_surface(compositor);
    if (!out->surface) {
        warnx("wl_compositor_create_surface");
        return;
    }

    out->viewport = wp_viewporter_get_viewport(viewporter, out->surface);
    if (!out->viewport) {
        warnx("wp_viewporter_get_viewport");
        goto fail;
    }

    out->lock_surface = ext_session_lock_v1_get_lock_surface(session_lock, out->surface, out->output);
    if (!out->lock_surface) {
        warnx("ext_session_lock_v1_get_lock_surface");
        goto fail;
    }

    ext_session_lock_surface_v1_add_listener(out->lock_surface, &lock_surface_listener, out);
    return;

fail:
    if (out->viewport) {
        wp_viewport_destroy(out->viewport);
        out->viewport = NULL;
    }
    if (out->surface) {
        wl_surface_destroy(out->surface);
        out->surface = NULL;
    }
}

static void add_output(struct wl_registry* source, uint32_t name, uint32_t version) {
    (void) version;

    struct output* out = NULL;

    for (int i = 0; i < OUTPUT_MAX; i++) {
        if (!outputs[i].output) {
            out = &outputs[i];
            break;
        }
    }
    if (!out) {
        warnx("ignoring output above the limit of %d", OUTPUT_MAX);
        return;
    }

    out->output = wl_registry_bind(source, name, &wl_output_interface, 1);
    if (!out->output) {
        warnx("wl_registry_bind wl_output");
        *out = (struct output) {0};
        return;
    }

    out->name = name;
    wl_output_add_listener(out->output, &output_listener, out);
    if (session_lock) {
        map_output(out);
    }
}

static void add_seat(struct wl_registry* source, uint32_t name, uint32_t version) {
    struct seat* seat = NULL;

    for (int i = 0; i < SEAT_MAX; i++) {
        if (!seats[i].seat) {
            seat = &seats[i];
            break;
        }
    }
    if (!seat) {
        warnx("ignoring seat above the limit of %d", SEAT_MAX);
        return;
    }

    uint32_t bound = version < 4 ? version : 4;

    seat->seat = wl_registry_bind(source, name, &wl_seat_interface, bound);
    if (!seat->seat) {
        warnx("wl_registry_bind wl_seat");
        *seat = (struct seat) {0};
        return;
    }

    seat->name = name;
    seat->version = bound;
    wl_seat_add_listener(seat->seat, &seat_listener, seat);
}

static void handle_shm_format(void* data, struct wl_shm* wl_shm, uint32_t format) {
    (void) data;
    (void) wl_shm;

    if (format == WL_SHM_FORMAT_ARGB8888) {
        shm_argb = true;
    }
}

static void handle_registry(void* data, struct wl_registry* source, uint32_t name, const char* interface, uint32_t version) {
    (void) data;

    if (version < 1) {
        return;
    }

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        if (!compositor) {
            compositor = wl_registry_bind(source, name, &wl_compositor_interface, 1);
        }
    } else if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0) {
        if (!lock_manager) {
            lock_manager = wl_registry_bind(source, name, &ext_session_lock_manager_v1_interface, 1);
        }
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        if (!viewporter) {
            viewporter = wl_registry_bind(source, name, &wp_viewporter_interface, 1);
        }
    } else if (strcmp(interface, wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
        if (!single_pixel) {
            single_pixel = wl_registry_bind(source, name, &wp_single_pixel_buffer_manager_v1_interface, 1);
        }
    } else if (cfg.clock && strcmp(interface, wl_shm_interface.name) == 0) {
        if (!shm) {
            shm = wl_registry_bind(source, name, &wl_shm_interface, 1);
            if (shm) {
                wl_shm_add_listener(shm, &shm_listener, NULL);
            }
        }
    } else if (cfg.clock && strcmp(interface, wl_subcompositor_interface.name) == 0) {
        if (!subcompositor) {
            subcompositor = wl_registry_bind(source, name, &wl_subcompositor_interface, 1);
        }
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        add_output(source, name, version);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        add_seat(source, name, version);
    }
}

static void handle_registry_remove(void* data, struct wl_registry* source, uint32_t name) {
    (void) data;
    (void) source;

    for (int i = 0; i < OUTPUT_MAX; i++) {
        if (outputs[i].output && outputs[i].name == name) {
            destroy_output(&outputs[i]);
            return;
        }
    }

    for (int i = 0; i < SEAT_MAX; i++) {
        if (seats[i].seat && seats[i].name == name) {
            destroy_seat(&seats[i]);
            return;
        }
    }
}

static int flush_display(void) {
    for (;;) {
        if (wl_display_flush(display) >= 0) {
            return 0;
        }
        if (errno != EAGAIN) {
            return -1;
        }

        struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLOUT };

        if (poll(&pfd, 1, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
    }
}

static void take_input(void) {
    if (submit) {
        submit = false;
        repeat_seat = NULL;
        repeat_down = false;

        if (repeat_fd >= 0) {
            arm_timer(repeat_fd, 0, 0);
        }

        if (session_locked && authenticate(password) == 0) {
            want_unlock = true;
        } else if (session_locked) {
            show_fail();
        }

        wipe_password();
    }

    if (want_unlock && session_lock && session_locked) {
        ext_session_lock_v1_unlock_and_destroy(session_lock);
        session_lock = NULL;
        session_locked = false;
        want_unlock = false;
        if (wl_display_roundtrip(display) < 0) {
            warn("wl_display_roundtrip");
            exit_status = EXIT_FAILURE;
        }
        stop = true;
    }
}

static void cleanup(void) {
    wipe_password();

    for (int i = 0; i < OUTPUT_MAX; i++) {
        if (outputs[i].output) {
            destroy_output(&outputs[i]);
        }
    }
    for (int i = 0; i < SEAT_MAX; i++) {
        if (seats[i].seat) {
            destroy_seat(&seats[i]);
        }
    }

    if (session_lock && !session_locked) {
        ext_session_lock_v1_destroy(session_lock);
        session_lock = NULL;
    }
    if (lock_manager) {
        ext_session_lock_manager_v1_destroy(lock_manager);
        lock_manager = NULL;
    }
    if (viewporter) {
        wp_viewporter_destroy(viewporter);
        viewporter = NULL;
    }
    if (single_pixel) {
        wp_single_pixel_buffer_manager_v1_destroy(single_pixel);
        single_pixel = NULL;
    }
    if (subcompositor) {
        wl_subcompositor_destroy(subcompositor);
        subcompositor = NULL;
    }
    if (shm) {
        wl_shm_destroy(shm);
        shm = NULL;
    }
    if (compositor) {
        wl_compositor_destroy(compositor);
        compositor = NULL;
    }
    if (registry) {
        wl_registry_destroy(registry);
        registry = NULL;
    }
    if (display) {
        wl_display_disconnect(display);
        display = NULL;
    }
    if (repeat_fd >= 0) {
        close(repeat_fd);
        repeat_fd = -1;
    }
    if (fail_fd >= 0) {
        close(fail_fd);
        fail_fd = -1;
    }
    if (clock_fd >= 0) {
        close(clock_fd);
        clock_fd = -1;
    }
}

int main(int argc, char* argv[]) {
    const char* config_path = NULL;
    int opt = 0;

    while ((opt = getopt(argc, argv, "c:")) != -1) {
        switch (opt) {
            case 'c':
                config_path = optarg;
                break;
            default:
                usage();
        }
    }

    argc -= optind;
    argv += optind;

    if (argc > 0) {
        warnx("extra operand '%s'", argv[0]);
        usage();
    }

    if (config_load(&cfg, config_path) < 0) {
        return EXIT_FAILURE;
    }

    if (atexit(wipe_password) != 0) {
        errx(EXIT_FAILURE, "atexit");
    }

    // A core file would keep the password. Keep going if the kernel refuses.
    if (prctl(PR_SET_DUMPABLE, 0) < 0) {
        warn("prctl");
    }

    // Unprivileged sessions often cannot lock memory.
    (void) mlock(password, sizeof(password));

    // Dying while locked leaves the session locked, so these are ignored.
    ignore_signals();

    repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (repeat_fd < 0) {
        warn("timerfd_create");
        exit_status = EXIT_FAILURE;
        goto end;
    }

    fail_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fail_fd < 0) {
        warn("timerfd_create");
        exit_status = EXIT_FAILURE;
        goto end;
    }

    if (cfg.clock) {
        clock_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (clock_fd < 0) {
            warn("timerfd_create");
        } else {
            arm_timer(clock_fd, clock_delay_ms(), 0);
        }
    }

    display = wl_display_connect(NULL);
    if (!display) {
        warn("wl_display_connect");
        exit_status = EXIT_FAILURE;
        goto end;
    }

    registry = wl_display_get_registry(display);
    if (!registry) {
        warnx("wl_display_get_registry");
        exit_status = EXIT_FAILURE;
        goto end;
    }
    wl_registry_add_listener(registry, &registry_listener, NULL);

    if (wl_display_roundtrip(display) < 0) {
        warn("wl_display_roundtrip");
        exit_status = EXIT_FAILURE;
        goto end;
    }

    if (!compositor) {
        warnx("compositor missing wl_compositor");
        exit_status = EXIT_FAILURE;
        goto end;
    }
    if (!lock_manager) {
        warnx("compositor missing ext_session_lock_manager_v1");
        exit_status = EXIT_FAILURE;
        goto end;
    }
    if (!viewporter) {
        warnx("compositor missing wp_viewporter");
        exit_status = EXIT_FAILURE;
        goto end;
    }
    if (!single_pixel) {
        warnx("compositor missing wp_single_pixel_buffer_manager_v1");
        exit_status = EXIT_FAILURE;
        goto end;
    }

    session_lock = ext_session_lock_manager_v1_lock(lock_manager);
    if (!session_lock) {
        warnx("ext_session_lock_manager_v1_lock");
        exit_status = EXIT_FAILURE;
        goto end;
    }
    ext_session_lock_v1_add_listener(session_lock, &lock_listener, NULL);

    for (int i = 0; i < OUTPUT_MAX; i++) {
        if (outputs[i].output) {
            map_output(&outputs[i]);
        }
    }

    for (;;) {
        if (flush_display() < 0) {
            warn("wl_display_flush");
            exit_status = EXIT_FAILURE;
            break;
        }

        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) {
                warn("wl_display_dispatch_pending");
                exit_status = EXIT_FAILURE;
                stop = true;
                break;
            }
            if (stop) {
                break;
            }
        }
        if (stop) {
            break;
        }

        if (submit) {
            wl_display_cancel_read(display);
            take_input();
            if (stop) {
                break;
            }
            continue;
        }

        struct pollfd pfds[4] = {
            { .fd = wl_display_get_fd(display), .events = POLLIN },
            { .fd = repeat_fd, .events = POLLIN },
            { .fd = fail_fd, .events = POLLIN },
        };
        nfds_t nfds = 3;
        if (clock_fd >= 0) {
            pfds[3].fd = clock_fd;
            pfds[3].events = POLLIN;
            nfds = 4;
        }
        int ready = poll(pfds, nfds, -1);
        if (ready < 0) {
            wl_display_cancel_read(display);
            if (errno == EINTR) {
                continue;
            }
            warn("poll");
            exit_status = EXIT_FAILURE;
            break;
        }

        if (pfds[0].revents & POLLIN) {
            if (wl_display_read_events(display) < 0) {
                warn("wl_display_read_events");
                exit_status = EXIT_FAILURE;
                stop = true;
            }
        } else {
            wl_display_cancel_read(display);
        }

        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            warnx("wayland display disconnected");
            exit_status = EXIT_FAILURE;
            stop = true;
        }

        if (!stop && wl_display_dispatch_pending(display) < 0) {
            warn("wl_display_dispatch_pending");
            exit_status = EXIT_FAILURE;
            stop = true;
        }

        if (pfds[1].revents & POLLIN) {
            drain_timer(repeat_fd);
            if (repeat_down && repeat_seat) {
                input_key(repeat_seat, repeat_key, true);
            }
        }

        if (pfds[2].revents & POLLIN) {
            drain_timer(fail_fd);
            end_fail();
        }

        if (nfds == 4 && (pfds[3].revents & POLLIN)) {
            drain_timer(clock_fd);
            refresh_clocks();
        }

        take_input();
        if (stop) {
            break;
        }
    }

end:
    cleanup();
    return exit_status;
}
