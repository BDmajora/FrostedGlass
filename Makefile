# frostedglass — Makefile
#
# Build: make
# Install: sudo make install
# Clean: make clean

CC       ?= gcc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin

# 1. Added -DWLR_USE_UNSTABLE and -I. to silence wlroots errors and find local headers
CFLAGS   ?= -O2 -pipe -Wall -Wextra -Wno-unused-parameter
CFLAGS   += -std=c11 -DWLR_USE_UNSTABLE -I.

# Try wlroots-0.18 first, fall back to unversioned
WLROOTS_PKG := $(shell $(PKG_CONFIG) --exists wlroots-0.18 2>/dev/null && echo wlroots-0.18 || echo wlroots)

PKGS := $(WLROOTS_PKG) wayland-server wayland-protocols xkbcommon pixman-1

# Optional — append only if present
PKGS += $(shell $(PKG_CONFIG) --exists libdrm 2>/dev/null && echo libdrm)
PKGS += $(shell $(PKG_CONFIG) --exists libinput 2>/dev/null && echo libinput)

CFLAGS  += $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDFLAGS += $(shell $(PKG_CONFIG) --libs $(PKGS))

# 2. Locate the wayland-protocols XML files
WAYLAND_PROTOCOLS_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML := $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml

BIN := frostedglass

.PHONY: all clean install uninstall

all: $(BIN)

# 3. Rules to generate the xdg-shell Wayland protocols via wayland-scanner
xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header $(XDG_SHELL_XML) $@

xdg-shell-protocol.c: xdg-shell-protocol.h
	$(WAYLAND_SCANNER) private-code $(XDG_SHELL_XML) $@

# Ensure headers are generated before compiling the objects
frostedglass.o: frostedglass.c xdg-shell-protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

xdg-shell-protocol.o: xdg-shell-protocol.c xdg-shell-protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

# 4. Link the generated protocol objects alongside the main compositor
$(BIN): frostedglass.o xdg-shell-protocol.o
	$(CC) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(BIN) *.o xdg-shell-protocol.h xdg-shell-protocol.c

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -Dm644 frostedglass.desktop $(DESTDIR)/usr/share/wayland-sessions/frostedglass.desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)/usr/share/wayland-sessions/frostedglass.desktop