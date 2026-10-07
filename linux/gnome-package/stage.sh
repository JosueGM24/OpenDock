#!/bin/sh
# Arma en DEST el contenido del paquete opendock-gnome (el .deb y el .rpm comparten esto):
# la extensión de GNOME para todo el sistema, con sus sonidos y sus esquemas compilados,
# y el activador que la enciende la primera vez que cada usuario entra en GNOME.
#     sh linux/gnome-package/stage.sh <DEST>
set -eu
DEST=$1
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
UUID=opendock@josuegm24.github.io
EXT="$DEST/usr/share/gnome-shell/extensions/$UUID"

mkdir -p "$EXT/sounds" "$DEST/usr/libexec" "$DEST/etc/xdg/autostart" "$DEST/usr/share/doc/opendock-gnome"
cp -r "$ROOT/linux/gnome-extension/$UUID"/. "$EXT/"
cp "$ROOT"/sounds/*.wav "$EXT/sounds/"
rm -f "$EXT/schemas/gschemas.compiled"
glib-compile-schemas --strict "$EXT/schemas"           # para todo el sistema: ya compilados
find "$EXT" -type d -exec chmod 755 {} +
find "$EXT" -type f -exec chmod 644 {} +

install -m 755 "$HERE/opendock-gnome-enable" "$DEST/usr/libexec/opendock-gnome-enable"
install -m 644 "$HERE/opendock-gnome-enable.desktop" "$DEST/etc/xdg/autostart/opendock-gnome-enable.desktop"
install -m 644 "$ROOT/LICENSE" "$DEST/usr/share/doc/opendock-gnome/copyright"
install -m 644 "$ROOT/sounds/CREDITS.txt" "$DEST/usr/share/doc/opendock-gnome/SOUNDS-CREDITS.txt"
echo "listo: $DEST"
