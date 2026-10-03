project('opendock', 'c',
  version: '0.1.0',
  license: 'MIT',
  default_options: ['c_std=c11', 'warning_level=2'])

fs = import('fs')
cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments(['-Wno-unused-parameter']), language: 'c')

gtk4_dep = dependency('gtk4', version: '>=4.10')
glib_dep = dependency('glib-2.0')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
gobject_dep = dependency('gobject-2.0')

m_dep = cc.find_library('m', required: false)

deps = [gtk4_dep, glib_dep, gio_dep, gio_unix_dep, gobject_dep, m_dep]

# gtk4-layer-shell: necesario para Wayland (KDE, Hyprland, Sway, labwc, river, COSMIC).
# Si no está disponible seguimos compilando: en tal caso sólo funcionará el respaldo X11.
layer_shell_dep = dependency('gtk4-layer-shell-0', required: false)
have_layer_shell = layer_shell_dep.found()
if have_layer_shell
  deps += layer_shell_dep
endif

# Backend X11 de GDK: ventanas EWMH dock + _NET_WM_STRUT_PARTIAL.
# (GTK4 lo publica como gtk4-x11; no existe un gdk-x11-4.0.)
gdkx11_dep = dependency('gtk4-x11', required: false)
x11_raw_dep = dependency('x11', required: false)
have_x11 = gdkx11_dep.found() and x11_raw_dep.found()
if have_x11
  deps += [gdkx11_dep, x11_raw_dep]
endif

# libpulse: volumen. Opcional (PLAN.md lo marca como dependencia opcional).
pulse_dep = dependency('libpulse', required: false)
pulse_glib_dep = dependency('libpulse-mainloop-glib', required: false)
have_pulse = pulse_dep.found() and pulse_glib_dep.found()
if have_pulse
  deps += [pulse_dep, pulse_glib_dep]
endif

# libcanberra: sonidos de notificación. Opcional.
canberra_dep = dependency('libcanberra', required: false)
have_canberra = canberra_dep.found()
if have_canberra
  deps += canberra_dep
endif

conf = configuration_data()
conf.set('HAVE_LAYER_SHELL', have_layer_shell ? 1 : 0)
proto_sources = []
# Protocolo wlr-foreign-toplevel-management (ventanas abiertas para el dock),
# generado con wayland-scanner a partir del XML vendido en protocols/.
have_foreign_toplevel = false
proto_xml = meson.current_source_dir() / 'protocols/wlr-foreign-toplevel-management-unstable-v1.xml'
wayland_scanner = find_program('wayland-scanner', required: false)
if have_layer_shell and wayland_scanner.found() and fs.exists(proto_xml)
  wl_proto_header = custom_target('wlr-foreign-toplevel-client-header',
    input: proto_xml,
    output: 'wlr-foreign-toplevel-management-unstable-v1-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  wl_proto_code = custom_target('wlr-foreign-toplevel-code',
    input: proto_xml,
    output: 'wlr-foreign-toplevel-management-unstable-v1-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  gtk_wayland_dep = dependency('gtk4-wayland', required: false)
  wayland_client_dep = dependency('wayland-client', required: false)
  if gtk_wayland_dep.found() and wayland_client_dep.found()
    proto_sources = [wl_proto_code, wl_proto_header]
    deps += [gtk_wayland_dep, wayland_client_dep]
    have_foreign_toplevel = true
  endif
endif

conf.set('HAVE_FOREIGN_TOPLEVEL', have_foreign_toplevel ? 1 : 0)
conf.set('HAVE_X11', have_x11 ? 1 : 0)
conf.set('HAVE_PULSE', have_pulse ? 1 : 0)
conf.set('HAVE_CANBERRA', have_canberra ? 1 : 0)
conf.set_quoted('OPENDOCK_VERSION', meson.project_version())
configure_file(output: 'opendock-build-config.h', configuration: conf)

inc = include_directories('.', 'src')

sources = [
  'src/main.c',
  'src/config.c',
  'src/spring.c',
  'src/session.c',
  'src/corners.c',
  'src/notch.c',
  'src/notifications.c',
  'src/bar.c',
  'src/volume.c',
  'src/power.c',
  'src/centro.c',
  'src/mini.c',
  'src/ventanas.c',
  'src/dock.c',
]

executable('opendock', sources + proto_sources,
  include_directories: inc,
  dependencies: deps,
  install: true)

install_data('opendock.desktop', install_dir: get_option('sysconfdir') / 'xdg/autostart')

summary({
  'gtk4-layer-shell (Wayland)': have_layer_shell,
  'gdk-x11 (X11)': have_x11,
  'libpulse (volumen)': have_pulse,
  'libcanberra (sonidos)': have_canberra,
  'wlr-foreign-toplevel (dock: ventanas)': have_foreign_toplevel,
})
