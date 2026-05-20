# frostedglass — Makefile
#
# Build: make
# Install: sudo make install
# Clean: make clean

CC           ?= gcc
PKG_CONFIG   ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
PREFIX       ?= /usr/local
BINDIR       ?= $(PREFIX)/bin

SRCDIR := src
INCDIR := include

CFLAGS   ?= -O2 -pipe -Wall -Wextra -Wno-unused-parameter
CFLAGS   += -std=c11 -DWLR_USE_UNSTABLE -I$(INCDIR) -I.

# Try versioned wlroots first (newest → oldest), fall back to unversioned
WLROOTS_PKG := $(shell \
  if $(PKG_CONFIG) --exists wlroots-0.20 2>/dev/null; then echo wlroots-0.20; \
  elif $(PKG_CONFIG) --exists wlroots-0.18 2>/dev/null; then echo wlroots-0.18; \
  else echo wlroots; fi)

PKGS := $(WLROOTS_PKG) wayland-server wayland-protocols xkbcommon pixman-1

# Optional — append only if present
PKGS += $(shell $(PKG_CONFIG) --exists libdrm 2>/dev/null && echo libdrm)
PKGS += $(shell $(PKG_CONFIG) --exists libinput 2>/dev/null && echo libinput)

CFLAGS  += $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDFLAGS += $(shell $(PKG_CONFIG) --libs $(PKGS))

# Wayland protocol code generation
WAYLAND_PROTOCOLS_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML := $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml

BIN := frostedglass

# main.c lives at repo root; modules live in src/
MODULE_SRCS := $(SRCDIR)/fg_output.c   \
               $(SRCDIR)/fg_toplevel.c \
               $(SRCDIR)/fg_taskbar.c  \
               $(SRCDIR)/fg_input.c    \
               $(SRCDIR)/fg_wine.c

MODULE_OBJS := $(MODULE_SRCS:.c=.o)

OBJS := main.o $(MODULE_OBJS) xdg-shell-protocol.o

HEADERS := $(wildcard $(INCDIR)/*.h)

.PHONY: all clean install uninstall

all: $(BIN)

# --- Wayland protocol generation ---

xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header $(XDG_SHELL_XML) $@

xdg-shell-protocol.c: xdg-shell-protocol.h
	$(WAYLAND_SCANNER) private-code $(XDG_SHELL_XML) $@

xdg-shell-protocol.o: xdg-shell-protocol.c xdg-shell-protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

# --- Compile main.c (repo root) ---

main.o: main.c $(HEADERS) xdg-shell-protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

# --- Compile src/*.c modules ---

$(SRCDIR)/%.o: $(SRCDIR)/%.c $(HEADERS) xdg-shell-protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

# --- Link ---

$(BIN): $(OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

# --- Housekeeping ---

clean:
	rm -f $(BIN) *.o $(SRCDIR)/*.o xdg-shell-protocol.h xdg-shell-protocol.c

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -Dm644 frostedglass.desktop $(DESTDIR)/usr/share/wayland-sessions/frostedglass.desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)/usr/share/wayland-sessions/frostedglass.desktop