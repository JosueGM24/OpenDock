# OpenDock para Linux

Lo mismo que la versión de Windows, en los escritorios que admiten superficies de capa:
esquinas redondeadas, notch con notificaciones y centro de notificaciones, barra superior con
centro de control y bandeja, y dock. En GNOME Shell va la extensión
(`linux/gnome-extension`), porque Mutter no admite layer-shell.

| Escritorio | Cómo se dibuja | Ventanas abiertas en el dock |
|---|---|---|
| Hyprland, Sway, river, labwc, Wayfire, COSMIC | `zwlr_layer_shell_v1` | Sí (`zwlr_foreign_toplevel_manager_v1`) |
| KDE Plasma 6 | `zwlr_layer_shell_v1` | No: KWin no ofrece el protocolo, sólo se ven las apps ancladas |
| XFCE, MATE, Cinnamon, i3, Openbox | X11 con EWMH (`_NET_WM_WINDOW_TYPE_DOCK`, `_NET_WM_STRUT_PARTIAL`) | Sí (`_NET_CLIENT_LIST`) |
| GNOME | No arranca: usa la extensión | — |

## Instalar

### Arch / Manjaro

```sh
cd linux/opendock
makepkg -si          # paquete opendock-git
```

### Ubuntu / Debian

```sh
sudo apt install build-essential meson ninja-build pkg-config libgtk-4-dev libglib2.0-dev \
    libwayland-dev libx11-dev libpulse-dev libcanberra-dev
# gtk4-layer-shell: en Ubuntu 24.10+ y Debian 13 es libgtk4-layer-shell-dev;
# en Ubuntu 24.04 hay que compilarlo (https://github.com/wmww/gtk4-layer-shell).
meson setup build linux/opendock --prefix=/usr
meson compile -C build
sudo meson install -C build
```

### Fedora

```sh
sudo dnf install gcc meson ninja-build pkgconf-pkg-config gtk4-devel gtk4-layer-shell-devel \
    wayland-devel libX11-devel pulseaudio-libs-devel libcanberra-devel
meson setup build linux/opendock --prefix=/usr
meson compile -C build
sudo meson install -C build
```

Sin `gtk4-layer-shell` compila igual, pero sólo funciona en X11. Sin `libpulse` no hay volumen,
y sin `libcanberra` las notificaciones no suenan. Wi‑Fi, batería, Bluetooth y brillo se leen de
NetworkManager, UPower, BlueZ y logind por D-Bus, sin root, y se ocultan si el servicio no está.

Se instala un arranque automático en `/etc/xdg/autostart`, que GNOME omite. Para probarlo sin
instalar: `./build/opendock` (con `--replace` si ya hay otro servidor de notificaciones, como
mako o dunst).

## Configuración

`~/.config/opendock/config.ini` (se crea la primera vez; el botón "Ajustes ›" lo abre):

```ini
[general]
# 0 quita las esquinas
radio_esquinas=16
alto_barra=28
alto_dock=60
# dock negro puro
oled=false
no_molestar=false
# silencio, sistema, nota, eco, alerta, destello, cascada, aviso, ambiente,
# confirmar, logro, brindis, fanfarria, fiesta o completado
sonido=eco
volumen_sonido=70

[dock]
# nunca, mitad o completo
ocultar=nunca
apps=firefox.desktop;org.gnome.Nautilus.desktop;
```

## Atajos de teclado

OpenDock exporta acciones por D-Bus para asociarlas a una tecla en el escritorio:

```sh
gdbus call --session --dest io.github.josuegm24.OpenDock --object-path /io/github/josuegm24/OpenDock \
    --method org.gtk.Actions.Activate centro '[]' '{}'     # centro de notificaciones
#                                     bandeja                # bandeja del sistema
```

## Pruebas

En cada cambio, el CI compila en Arch, Ubuntu y Fedora, construye el paquete de Arch y prueba en
un sway sin pantalla (`tests/run-sway-test.sh`) y en Xvfb con Openbox (`tests/run-x11-test.sh`).
Las pruebas comprueban capturas, la zona reservada, el servidor de notificaciones, el centro, el
mini notch, la bandeja (con un icono falso) y el dock.
