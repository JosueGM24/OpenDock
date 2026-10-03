project('opendock', 'c',
  version: '0.1.0',
  license: 'MIT',
  default_options: ['c_std=c11', 'warning_level=3'])

fs = import('fs')
cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments(['-Wno-unused-parameter']), language: 'c')

gtk4_dep = dependency('gtk4', version: '>=4.10')
glib_dep = dependency('glib-2.0')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
gobject_dep = dependency('gobject-2.0')

deps = [gtk4_dep, glib_dep, gio_dep, gio_unix_dep, gobject_dep]

# gtk4-layer-shell: necesario para Wayland (KDE, Hyprland, Sway, labwc, river, COSMIC).
# Si no está disponible seguimos compilando: en tal caso sólo funcionará el respaldo X11.
layer_shell_dep = dependency('gtk4-layer-shell-0', required: false)
have_layer_shell = layer_shell_dep.found()
if have_layer_shell
  deps += layer_shell_dep
endif

# Backend X11 de GDK: ventanas EWMH dock + _NET_WM_STRUT_PARTIAL.
gdkx11_dep = dependency('gdk-x11-4.0', required: false)
have_x11 = gdkx11_dep.found()
if have_x11
  deps += gdkx11_dep
endif

# libpulse: volumen. Opcional (PLAN.md lo marca como dependencia opcional).
pulse_dep = dependency('libpulse', required: false)
have_pulse = pulse_dep.found()
if have_pulse
  deps += pulse_dep
endif

# libcanberra: sonidos de notificación. Opcional.
canberra_dep = dependency('libcanberra', required: false)
have_canberra = canberra_dep.found()
if have_canberra
  deps += canberra_dep
endif

conf = configuration_data()
conf.set('HAVE_LAYER_SHELL', have_layer_shell ? 1 : 0)
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
]

# Protocolo wlr-foreign-toplevel-management: se añade en la fase 5 (dock), generado
# con wayland-scanner a partir del XML vendido en protocols/.
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
  sources += [wl_proto_code]
  sources += [wl_proto_header]
  have_foreign_toplevel = true
  wayland_client_dep = dependency('wayland-client', required: false)
  if wayland_client_dep.found()
    deps += wayland_client_dep
  endif
endif

executable('opendock', sources,
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
