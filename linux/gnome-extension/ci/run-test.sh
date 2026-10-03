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

# Se ejecuta la prueba real como "tester"; el estado (ok/fallo) se
# propaga mediante el código de salida de inner-test.sh.
runuser -u tester -- env \
    HOME="$USER_HOME" \
    ARTIFACT_DIR="$ARTIFACT_DIR" \
    UUID="$UUID" \
    "$CI_DIR/inner-test.sh"
