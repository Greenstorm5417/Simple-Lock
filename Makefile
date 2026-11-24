CC ?= cc
PKG_CONFIG ?= pkg-config

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin

WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)

CFLAGS = -Os -std=c23 -Wall -Wextra -Wpedantic \
	-Wnested-externs -Wold-style-definition -Wredundant-decls \
	-Wshadow -Wstrict-prototypes -Wvla \
	-ffunction-sections -fdata-sections \
	$(shell $(PKG_CONFIG) --cflags wayland-client xkbcommon)
LDFLAGS = -Wl,--gc-sections
LDLIBS = $(shell $(PKG_CONFIG) --libs wayland-client) -lpam -ldl

PROTO_DIR = build/protocol
LOCK_XML = $(WAYLAND_PROTOCOLS)/staging/ext-session-lock/ext-session-lock-v1.xml
VIEW_XML = $(WAYLAND_PROTOCOLS)/stable/viewporter/viewporter.xml
PIXEL_XML = $(WAYLAND_PROTOCOLS)/staging/single-pixel-buffer/single-pixel-buffer-v1.xml

LOCK_H = $(PROTO_DIR)/ext-session-lock-v1-client-protocol.h
LOCK_C = $(PROTO_DIR)/ext-session-lock-v1-protocol.c
VIEW_H = $(PROTO_DIR)/viewporter-client-protocol.h
VIEW_C = $(PROTO_DIR)/viewporter-protocol.c
PIXEL_H = $(PROTO_DIR)/single-pixel-buffer-v1-client-protocol.h
PIXEL_C = $(PROTO_DIR)/single-pixel-buffer-v1-protocol.c

GEN_H = $(LOCK_H) $(VIEW_H) $(PIXEL_H)
GEN_C = $(LOCK_C) $(VIEW_C) $(PIXEL_C)

.PHONY: all check install uninstall clean

all: build/simple-lock

build/simple-lock: src/main.c src/auth.c src/keymap.c src/config.c src/clock.c src/auth.h src/keymap.h src/config.h src/clock.h Makefile $(GEN_H) $(GEN_C)
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(PROTO_DIR) -o $@ src/main.c src/auth.c src/keymap.c src/config.c src/clock.c $(GEN_C) $(LDFLAGS) $(LDLIBS)

$(LOCK_H): $(LOCK_XML)
	mkdir -p $(PROTO_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(LOCK_C): $(LOCK_XML)
	mkdir -p $(PROTO_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(VIEW_H): $(VIEW_XML)
	mkdir -p $(PROTO_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(VIEW_C): $(VIEW_XML)
	mkdir -p $(PROTO_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(PIXEL_H): $(PIXEL_XML)
	mkdir -p $(PROTO_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(PIXEL_C): $(PIXEL_XML)
	mkdir -p $(PROTO_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

check: build/simple-lock
	! ./build/simple-lock --help
	! ./build/simple-lock extra
	! ./build/simple-lock -c /no/such/simple-lock.conf
	! env -u WAYLAND_SOCKET -u XDG_CONFIG_HOME -u HOME WAYLAND_DISPLAY=simple-lock-missing ./build/simple-lock
	! env -u WAYLAND_SOCKET WAYLAND_DISPLAY=simple-lock-missing ./build/simple-lock -c simple-lock.conf

install: build/simple-lock
	install -Dm755 build/simple-lock "$(DESTDIR)$(BINDIR)/simple-lock"
	install -Dm644 pam.d/simple-lock "$(DESTDIR)/etc/pam.d/simple-lock"
	install -Dm644 simple-lock.conf "$(DESTDIR)$(PREFIX)/share/simple-lock/simple-lock.conf"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/simple-lock"
	rm -f "$(DESTDIR)/etc/pam.d/simple-lock"
	rm -f "$(DESTDIR)$(PREFIX)/share/simple-lock/simple-lock.conf"

clean:
	rm -rf build
