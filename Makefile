# frostedglass — Makefile
# [cite: 3]
# Build: make
# Install: sudo make install
# Clean: make clean

CC       ?= gcc
PKG_CONFIG ?= pkg-config
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin

CFLAGS   ?= -O2 -pipe -Wall -Wextra -Wno-unused-parameter
CFLAGS   += -std=c11

# Try wlroots-0.18 first, fall back to unversioned
WLROOTS_PKG := $(shell $(PKG_CONFIG) --exists wlroots-0.18 2>/dev/null && echo wlroots-0.18 || echo wlroots)

PKGS := $(WLROOTS_PKG) wayland-server wayland-protocols xkbcommon pixman-1

# Optional — append only if present
PKGS += $(shell $(PKG_CONFIG) --exists libdrm 2>/dev/null && echo libdrm)
PKGS += $(shell $(PKG_CONFIG) --exists libinput 2>/dev/null && echo libinput)

CFLAGS  += $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDFLAGS += $(shell $(PKG_CONFIG) --libs $(PKGS))

SRC := frostedglass.c
BIN := frostedglass

.PHONY: all clean install uninstall

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BIN)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -Dm644 frostedglass.desktop $(DESTDIR)/usr/share/wayland-sessions/frostedglass.desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)/usr/share/wayland-sessions/frostedglass.desktop