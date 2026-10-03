#!/usr/bin/env bash
# Empaqueta la extensión de GNOME Shell de OpenDock: copia los sonidos desde
# sounds/, compila el gschema en modo estricto y genera el .zip instalable
# con `gnome-extensions pack` (o un zip manual si esa herramienta no existe).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
UUID="opendock@josuegm24.github.io"
EXT_DIR="$SCRIPT_DIR/$UUID"
OUT_DIR="${1:-$SCRIPT_DIR/dist}"

echo "== Copiando sonidos =="
mkdir -p "$EXT_DIR/sounds"
cp "$REPO_ROOT"/sounds/*.wav "$EXT_DIR/sounds/"

echo "== Compilando schemas (--strict) =="
glib-compile-schemas --strict "$EXT_DIR/schemas"

echo "== Empaquetando =="
mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR/$UUID.shell-extension.zip"

if command -v gnome-extensions >/dev/null 2>&1; then
    (cd "$EXT_DIR" && gnome-extensions pack \
        --force \
        --out-dir="$OUT_DIR" \
        --extra-source=lib \
        --extra-source=sounds \
        .)
else
    echo "gnome-extensions no está disponible; empaquetando con zip manual."
    (cd "$EXT_DIR" && zip -r -X "$OUT_DIR/$UUID.shell-extension.zip" \
        metadata.json extension.js prefs.js stylesheet.css lib schemas sounds \
        -x '*.compiled')
fi

echo "== Listo: $OUT_DIR/$UUID.shell-extension.zip =="
