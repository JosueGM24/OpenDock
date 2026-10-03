#!/usr/bin/env bash
# Se ejecuta como root dentro del contenedor: prepara un usuario sin
# privilegios (gnome-shell se niega a correr como root), instala la
# extensión en su HOME y delega la prueba real a inner-test.sh.
set -euo pipefail

CI_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$CI_DIR/../../.." && pwd)"
UUID="opendock@josuegm24.github.io"
ARTIFACT_DIR="${1:-$REPO_ROOT/ci-artifacts}"

mkdir -p "$ARTIFACT_DIR"

if ! id tester &>/dev/null; then
    useradd -m -s /bin/bash tester
fi
USER_HOME=$(getent passwd tester | cut -d: -f6)

EXT_TARGET="$USER_HOME/.local/share/gnome-shell/extensions/$UUID"
mkdir -p "$EXT_TARGET"
cp -r "$REPO_ROOT/linux/gnome-extension/$UUID/." "$EXT_TARGET/"

mkdir -p "$EXT_TARGET/sounds"
cp "$REPO_ROOT"/sounds/*.wav "$EXT_TARGET/sounds/" 2>/dev/null || true

echo "== glib-compile-schemas --strict =="
glib-compile-schemas --strict "$EXT_TARGET/schemas"

mkdir -p "$USER_HOME/.config"
chown -R tester:tester "$USER_HOME" "$ARTIFACT_DIR" 2>/dev/null || chown -R tester "$USER_HOME" "$ARTIFACT_DIR"

chmod +x "$CI_DIR/inner-test.sh"

# gnome-shell elige LoginManagerSystemd en vez de LoginManagerDummy con solo
# comprobar si existe /run/systemd/system (ver loginManager.js, haveSystemd);
# algunas imágenes base dejan ese directorio aunque no haya systemd ni logind
# corriendo de verdad, y entonces gnome-shell aborta (excepción sin capturar)
# al intentar hablar con org.freedesktop.login1. Se borra para forzar el
# backend dummy, que no necesita logind.
rm -rf /run/systemd/system 2>/dev/null || true

# Por si algo más mira el bus de sistema (UPower, NetworkManager...), se deja
# uno mínimo disponible; no es necesario para el login manager una vez
# forzado el backend dummy, pero no hace daño tenerlo.
command -v dbus-uuidgen >/dev/null 2>&1 && dbus-uuidgen --ensure || true
mkdir -p /run/dbus
if [ ! -S /run/dbus/system_bus_socket ]; then
    dbus-daemon --system --fork || echo "AVISO: no se pudo levantar el bus de sistema; se continúa de todos modos"
fi

# Se ejecuta la prueba real como "tester", dentro de su propia sesión de
# D-Bus (dbus-run-session se encarga de crearla y de limpiarla al salir).
runuser -u tester -- dbus-run-session -- env \
    HOME="$USER_HOME" \
    ARTIFACT_DIR="$ARTIFACT_DIR" \
    UUID="$UUID" \
    bash "$CI_DIR/inner-test.sh"
