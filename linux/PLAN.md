# OpenDock para Linux — plan

## Decisión: Wayland primero, X11 de respaldo, GNOME con extensión

| Escritorio | Servidor gráfico | Cómo se dibuja OpenDock | Componente |
|---|---|---|---|
| KDE Plasma 6 (Manjaro KDE) | Wayland | `zwlr_layer_shell_v1` (KWin lo soporta) | `opendock` (C + GTK4 + gtk4-layer-shell) |
| Hyprland, Sway, river, labwc, Wayfire, COSMIC | Wayland (wlroots y afines) | `zwlr_layer_shell_v1` | `opendock` |
| GNOME 45–48 (Manjaro GNOME, Ubuntu, Fedora) | Wayland | Mutter **no** tiene layer-shell: extensión de GNOME Shell (GJS) | `opendock@josuegm24.github.io` |
| XFCE, MATE, Cinnamon, i3 | X11 | Ventanas `_NET_WM_WINDOW_TYPE_DOCK` + `_NET_WM_STRUT_PARTIAL` (EWMH) | `opendock` con backend X11 |

Wayland es el presente de los escritorios principales (GNOME, Plasma 6, Hyprland), y layer-shell da justo lo que hace
OpenDock en Windows: superficies ancladas a los bordes, por encima de todo, que reservan espacio (zona exclusiva) igual que
una AppBar. X11 queda como respaldo con EWMH, que todos los gestores de X entienden. GNOME no permite clientes de capa,
así que ahí OpenDock es una extensión del propio shell (lo mismo que hacen Dash to Dock o Just Perfection).

## Piezas y sus fuentes en Linux

| Pieza | Fuente |
|---|---|
| Esquinas redondeadas | 4 superficies de capa `overlay` de r×r, sin entrada (región de entrada vacía), que siguen el radio y el monitor |
| Notch + centro de notificaciones | Servidor D-Bus `org.freedesktop.Notifications` (spec 1.2): Notify, CloseNotification, GetCapabilities, GetServerInformation, señales NotificationClosed / ActionInvoked. Si ya hay otro servidor (el del escritorio), se ofrece reemplazarlo (`--replace`) |
| Barra superior | Superficie `top` anclada arriba con zona exclusiva de 28 px |
| Reloj | GLib |
| Wi‑Fi | NetworkManager por D-Bus (`org.freedesktop.NetworkManager`) |
| Batería | UPower por D-Bus (`org.freedesktop.UPower`, DisplayDevice) |
| Volumen | PipeWire / PulseAudio vía `libpulse` (pipewire-pulse lo atiende) o `wpctl` |
| Brillo | `org.freedesktop.login1.Session.SetBrightness` (logind, sin root) + `/sys/class/backlight` |
| Bluetooth | BlueZ por D-Bus |
| Bandeja (Tailscale, etc.) | Anfitrión StatusNotifierItem (`org.kde.StatusNotifierWatcher`) |
| Dock: apps ancladas | Archivos `.desktop` (lista configurable, por defecto los favoritos del escritorio si se pueden leer) |
| Dock: ventanas abiertas | `zwlr_foreign_toplevel_manager_v1` (wlroots, Hyprland, labwc, KWin ≥ 6.1 la expone como `ext-foreign-toplevel-list` + KDE `org_kde_plasma_window_management`); en X11, `_NET_CLIENT_LIST` |
| Lanzar apps | `GDesktopAppInfo` (sin shell intermedio) |
| Sonidos | Los mismos 13 de Material Design (`sounds/`), con `libcanberra` o GStreamer |
| Ajustes | GSettings o un `config.ini` en `~/.config/opendock/` |

## Fases (cada una se compila y se prueba en CI antes de pasar a la siguiente)

1. **Esqueleto**: Meson, C11, GTK4, gtk4-layer-shell opcional, backend X11 opcional; `opendock --version`; CI en Arch y Ubuntu.
2. **Esquinas** en todos los monitores; prueba: sway sin cabeza + `grim`, se comprueban píxeles negros en las esquinas.
3. **Notch + servidor de notificaciones**: `notify-send` en la prueba → aparece el notch (captura); centro de notificaciones; mini notch.
4. **Barra superior** con reloj, Wi‑Fi, batería, volumen y centro de control; zona exclusiva (una ventana maximizada queda debajo).
5. **Dock** con anclados, abiertos (foreign-toplevel), lupa, rebote, ocultación a la mitad / del todo.
6. **Bandeja** (StatusNotifierItem) y sonidos.
7. **Extensión de GNOME**: esquinas, notch reemplazando los banners de GNOME (MessageTray), dock y estilo de la barra; prueba con `gnome-shell --headless --virtual-monitor` y `notify-send`.
8. **Paquetes**: `PKGBUILD` (AUR: `opendock`, `gnome-shell-extension-opendock`), instrucciones para Ubuntu/Fedora.

## Reglas comunes (las mismas que la versión de Windows)

- Sin red, sin telemetría, sin root. Todo en `~/.config/opendock/`.
- Nada de shell intermedio para abrir cosas; todo lo externo validado y acotado.
- Muelles con las mismas k y ζ que Windows (ver `DESIGN.md`) y respetar "reducir animaciones" (`gtk-enable-animations`).
- Commits en español, en la rama de su componente; CI en verde antes de dar algo por hecho.
