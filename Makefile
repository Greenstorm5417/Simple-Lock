CC = cc
PKG_CONFIG = pkg-config

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man
DESTDIR =

_VERSION = 0.1-dev
VERSION  = $(shell git describe --tags --dirty 2>/dev/null || echo $(_VERSION))

WAYLAND_PROTOCOLS = $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER = $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)

CPPFLAGS += $(shell $(PKG_CONFIG) --cflags wayland-client pam xkbcommon)
CPPFLAGS += -DKISSLOCK_VERSION=\"$(VERSION)\"

CFLAGS += -O2 -std=c23 -Wall -Wextra -pedantic \
		  -Wnested-externs -Wold-style-definition -Wredundant-decls \
		  -Wshadow -Wstrict-prototypes -Wvla \
		  -ffunction-sections -fdata-sections

LDFLAGS += -Wl,--gc-sections
LDLIBS += $(shell $(PKG_CONFIG) --libs wayland-client pam xkbcommon)

BUILD_DIR = build
PROTO_DIR = $(BUILD_DIR)/protocol

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

SRC = \
	  src/auth.c \
	  src/clock.c \
	  src/config.c \
	  src/keymap.c \
	  src/main.c

OBJ = $(patsubst src/%.c,$(BUILD_DIR)/src/%.o,$(SRC)) $(patsubst $(PROTO_DIR)/%.c,$(PROTO_DIR)/%.o,$(GEN_C))
DEP = $(OBJ:.o=.d)

.PHONY: all clean install uninstall

all: build/kisslock

build/kisslock: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

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

$(BUILD_DIR)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(PROTO_DIR) -MMD -MP -c $< -o $@

$(PROTO_DIR)/%.o: $(PROTO_DIR)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(PROTO_DIR) -MMD -MP -c $< -o $@

$(BUILD_DIR)/src/main.o: $(GEN_H)
$(BUILD_DIR)/src/auth.o: src/auth.h
$(BUILD_DIR)/src/keymap.o: src/keymap.h
$(BUILD_DIR)/src/config.o: src/config.h
$(BUILD_DIR)/src/clock.o: src/clock.h

-include $(DEP)

install: build/kisslock
	install -Dm755 build/kisslock $(DESTDIR)$(BINDIR)/kisslock
	install -Dm644 pam/kisslock $(DESTDIR)/etc/pam.d/kisslock
	install -Dm644 kisslock.1 $(DESTDIR)$(MANDIR)/man1/kisslock.1
	gzip -f -9 $(DESTDIR)$(MANDIR)/man1/kisslock.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kisslock
	rm -f $(DESTDIR)/etc/pam.d/kisslock
	rm -f $(DESTDIR)$(MANDIR)/man1/kisslock.1.gz

clean:
	rm -rf build
