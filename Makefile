project(
  'frostedglass',
  'c',
  version: '0.1.0',
  license: 'MIT',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'werror=false',
  ],
)

# wlroots 0.18+ (Gentoo: gui-libs/wlroots)
wlroots = dependency('wlroots-0.18', version: '>=0.18.0', required: false)
if not wlroots.found()
  wlroots = dependency('wlroots', version: '>=0.18.0')
endif

wayland_server  = dependency('wayland-server')
wayland_protos  = dependency('wayland-protocols')
xkbcommon       = dependency('xkbcommon')
pixman          = dependency('pixman-1')

libdrm   = dependency('libdrm', required: false)
libinput = dependency('libinput', required: false)

executable(
  'frostedglass',
  'frostedglass.c',
  dependencies: [
    wlroots,
    wayland_server,
    xkbcommon,
    pixman,
    libdrm,
    libinput,
  ],
  install: true,
  install_dir: get_option('bindir'),
)
