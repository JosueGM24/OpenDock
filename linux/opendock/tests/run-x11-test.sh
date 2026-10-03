#!/usr/bin/env bash
# Prueba de humo en X11: Xvfb + openbox, lanza opendock con el respaldo
# X11 y comprueba que la barra está arriba (EWMH dock) y que
# _NET_WM_STRUT_PARTIAL reserva el espacio (ventanas maximizadas quedan
# debajo), tal y como describe PLAN.md para XFCE/MATE/Cinnamon/i3.
set -euo pipefail

ARTEFACTOS="${1:-/tmp/opendock-ci-x11}"
mkdir -p "$ARTEFACTOS"

export DISPLAY=:99
export GDK_BACKEND=x11
# Xvfb no tiene GPU: render por software, como en la prueba de sway.
export LIBGL_ALWAYS_SOFTWARE=1
export GSK_RENDERER=cairo
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/xdg-runtime-opendock-x11}"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
export XDG_CONFIG_HOME="$ARTEFACTOS/config"
mkdir -p "$XDG_CONFIG_HOME"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OPENDOCK_BIN="$REPO_ROOT/linux/opendock/build/opendock"

Xvfb "$DISPLAY" -screen 0 1280x800x24 >"$ARTEFACTOS/xvfb.log" 2>&1 &
XVFB_PID=$!
sleep 1

openbox >"$ARTEFACTOS/openbox.log" 2>&1 &
OPENBOX_PID=$!
sleep 1

dbus-run-session -- "$OPENDOCK_BIN" >"$ARTEFACTOS/opendock.log" 2>&1 &
OPENDOCK_PID=$!

fallo=0

# Espera a que aparezca la barra (en un runner cargado tarda más de 2 s).
VENTANA=""
for _ in $(seq 1 75); do
    VENTANA=$(xwininfo -root -tree -display "$DISPLAY" | grep -F '"OpenDock-Barra"' | awk '{print $1}' | head -n1 || true)
    [ -n "$VENTANA" ] && break
    if ! kill -0 "$OPENDOCK_PID" 2>/dev/null; then
        echo "FALLO: opendock terminó antes de mostrar la barra"
        break
    fi
    sleep 0.2
done
sleep 1   # deja que pinte el primer cuadro

import -display "$DISPLAY" -window root "$ARTEFACTOS/x11-barra.png" || fallo=1
if [ -z "$VENTANA" ]; then
    echo "FALLO: no se encontró la ventana OpenDock-Barra"
    fallo=1
else
    echo "ventana de la barra: $VENTANA"
    PROPS=$(xprop -display "$DISPLAY" -id "$VENTANA" _NET_WM_STRUT_PARTIAL _NET_WM_WINDOW_TYPE 2>&1 || true)
    echo "$PROPS"
    if echo "$PROPS" | grep -q "_NET_WM_STRUT_PARTIAL"; then
        echo "ok: _NET_WM_STRUT_PARTIAL está presente"
    else
        echo "FALLO: _NET_WM_STRUT_PARTIAL no está presente"
        fallo=1
    fi
    GEOM=$(xwininfo -display "$DISPLAY" -id "$VENTANA" | grep -E "Absolute upper-left Y|Height")
    echo "$GEOM"
    if echo "$GEOM" | grep -q "Absolute upper-left Y: *0$"; then
        echo "ok: la barra está pegada arriba (y=0)"
    else
        echo "FALLO: la barra no está en y=0"
        fallo=1
    fi
fi

python3 "$REPO_ROOT/linux/opendock/tests/check_screenshot.py" "$ARTEFACTOS/x11-barra.png" \
    --width 1280 --bar-height 28 --check bar || fallo=1

kill "$OPENDOCK_PID" 2>/dev/null || true
wait "$OPENDOCK_PID" 2>/dev/null || true
kill "$OPENBOX_PID" 2>/dev/null || true
kill "$XVFB_PID" 2>/dev/null || true

echo "--- logs de opendock ---"
cat "$ARTEFACTOS/opendock.log" 2>/dev/null || true

exit $fallo
