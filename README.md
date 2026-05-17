# frostedglass

A minimal wlroots-based Wayland compositor purpose-built for YetiOS.
Wine **is** the desktop — frostedglass is invisible plumbing.

Part of the YetiOS snow suite:
```
snowcone (boot splash) → snowfall (login) → frostedglass (desktop)
```

## What this replaces

The old WSLK stack:
```
snowfall → labwc → swaybg + shell scripts → Wine explorer.exe
```

The new stack:
```
snowfall → frostedglass → Wine explorer.exe
```

No labwc, no swaybg, no rc.xml, no sed hacks, no autostart scripts.
Frostedglass launches Wine directly and gets out of the way.

## Architecture

Frostedglass does exactly five things:

1. **Acquires DRM master** from the kernel (this causes snowcone to
   detect the handoff and exit).
2. **Serves Wayland protocols**: `wl_compositor`, `xdg_shell`, `wl_seat`,
   `wl_output`. Wine's native Wayland driver connects as a client.
3. **Routes input**: libinput → `wl_keyboard` / `wl_pointer` events
   delivered to whatever surface has focus.
4. **Composites** (or direct-scanout when possible): wlroots scene graph
   renders surfaces to the display.
5. **Launches Wine**: `explorer.exe /desktop=shell` with native Wayland
   driver forced. Wine draws the taskbar, start menu, and manages all
   windows through its own Win32 window manager.

What frostedglass does NOT do: decorations, animations, blur, rounded
corners, wallpaper, panels, notifications, tiling, workspaces. Wine
handles the desktop; frostedglass handles the display.

## Latency path

```
User input (keyboard/mouse)
  → libinput (kernel evdev)
  → wl_seat (frostedglass routes to focused surface)
  → Wine Wayland driver (translates to Win32 messages)
  → Win32 app processes input
  → App renders to Wayland buffer (GDI/DirectX/Vulkan)
  → wlr_scene composites (or direct scanout for fullscreen)
  → DRM/KMS scanout to display
```

With direct scanout (fullscreen Wine desktop window), the compositor
doesn't even touch the pixels — zero-copy to display.

## Building

### Host dependencies (Debian/Ubuntu)

```bash
apt install build-essential meson ninja-build pkg-config \
    libwlroots-dev libwayland-dev wayland-protocols \
    libxkbcommon-dev libpixman-1-dev libdrm-dev libinput-dev
```

### Build

```bash
meson setup build
ninja -C build
```

### Test (nested in an existing Wayland session)

```bash
WAYLAND_DISPLAY=wayland-0 ./build/frostedglass --res 1280x720
```

### Install into YetiOS build

Place the `frostedglass/` directory next to `run.py`, add
`stage_09b_frostedglass` to the build stages, and it handles the rest.

## Runtime

### Launched by snowfall

Snowfall reads `/usr/share/wayland-sessions/frostedglass.desktop` and
offers it as a session. When the user picks it, snowfall execs
`frostedglass`, which acquires DRM master (causing snowcone to exit),
then launches Wine.

### Command-line options

```
frostedglass [--res WxH]
```

- `--res 1920x1080` — explicit Wine desktop resolution
- omit `--res` — Wine auto-detects from the Wayland output

### Compositor keybindings

Only one, intentionally:

- **Ctrl+Alt+Backspace** — emergency kill compositor

Everything else (Alt+Tab, Alt+F4, window drag/resize, minimize,
maximize) is handled by Wine's Win32 window manager.

### Wine environment

Frostedglass sets these before launching Wine:

| Variable | Value | Why |
|----------|-------|-----|
| `WAYLAND_DISPLAY` | (socket) | Connect to frostedglass |
| `DISPLAY` | (unset) | Force native Wayland, skip XWayland |
| `WINEWAYLAND` | `1` | Explicit Wayland driver opt-in |

The registry file (`~/.frostedglass_prefs.reg`) forces:
- `Graphics=wayland` (no X11 fallback)
- `DPI=96` (1:1 pixel mapping)
- `Decorations=N` (Wine draws its own)
- `renderer=vulkan` (DXVK when available)

## Files

```
frostedglass/
├── meson.build                  # Build system
├── frostedglass.c               # The compositor (~700 lines)
├── frostedglass.desktop         # Wayland session file for snowfall
├── frostedglass_prefs.reg       # Wine registry preferences
├── stage_09b_frostedglass.py    # YetiOS build integration
└── README.md                    # This file
```

## Integration with yeti-build

To wire frostedglass into the build pipeline:

1. Add `"09b_frostedglass"` to `STAGES` in `src/common.py`
2. Import `stage_09b_frostedglass` in `src/__init__.py`
3. Add the stage dispatch in `run.py`
4. Add `app-emulation/wine-vanilla` to `YETI_PACKAGE_LIST` in `src/templates.py`
5. Remove labwc, swaybg, and sway from the package list (no longer needed)