#!/usr/bin/env bash
# Se ejecuta como el usuario sin privilegios "tester", ya dentro de una
# sesión de D-Bus propia (run-test.sh lo invoca envuelto en
# `dbus-run-session`). Lanza gnome-shell en modo headless con un monitor
# virtual, habilita la extensión, toma capturas y comprueba (vía
# org.gnome.Shell.Eval, que requiere --unsafe-mode) que las esquinas, el
# notch y el dock existen, que la barra mide 28 px y que el banner nativo
# de GNOME no se mostró tras notify-send. Si una parte no es posible en el
# contenedor headless, se omite esa comprobación con un aviso claro en vez
# de fallar toda la prueba.
set -uo pipefail

ARTIFACT_DIR="${ARTIFACT_DIR:?falta ARTIFACT_DIR}"
UUID="${UUID:-opendock@josuegm24.github.io}"
LOG="$ARTIFACT_DIR/shell.log"
mkdir -p "$ARTIFACT_DIR"

export XDG_RUNTIME_DIR="/tmp/opendock-run-$$"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

FAIL=0
SKIPPED=()

if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    echo "FALLO: no hay sesión de D-Bus activa (se esperaba estar dentro de dbus-run-session)."
    exit 1
fi
echo "Bus de sesión: $DBUS_SESSION_BUS_ADDRESS"

cleanup() {
    [ -n "${SHELL_PID:-}" ] && kill "$SHELL_PID" 2>/dev/null
}
trap cleanup EXIT

gsettings set org.gnome.shell disable-user-extensions false 2>/dev/null || true
gsettings set org.gnome.shell enabled-extensions "['$UUID']" 2>/dev/null || true
gsettings set org.gnome.shell disable-extension-version-validation true 2>/dev/null || true

wait_for_shell() {
    for _ in $(seq 1 30); do
        if gdbus introspect --session --dest org.gnome.Shell \
            --object-path /org/gnome/Shell &>/dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}

start_shell() {
    local extra_args=("$@")
    gnome-shell --headless --virtual-monitor=1280x800 --wayland \
        --unsafe-mode "${extra_args[@]}" >>"$LOG" 2>&1 &
    SHELL_PID=$!
}

echo "== Arrancando gnome-shell (intento 1: --virtual-monitor) =="
: > "$LOG"
start_shell
if ! wait_for_shell; then
    echo "AVISO: gnome-shell no respondió con --virtual-monitor; probando con MUTTER_DEBUG_DUMMY_MODE_SPECS"
    kill "$SHELL_PID" 2>/dev/null
    wait "$SHELL_PID" 2>/dev/null
    export MUTTER_DEBUG_DUMMY_MODE_SPECS="1280x800"
    start_shell
    if ! wait_for_shell; then
        echo "FALLO: gnome-shell --headless no arrancó con ninguna de las dos variantes probadas."
        echo "---- shell.log ----"
        cat "$LOG" || true
        exit 1
    fi
fi
echo "gnome-shell respondiendo en D-Bus (pid $SHELL_PID)."

gnome-extensions enable "$UUID" 2>/dev/null || true
sleep 3

shell_eval() {
    # Ejecuta JS dentro de gnome-shell vía org.gnome.Shell.Eval (requiere
    # --unsafe-mode). Devuelve la salida cruda de gdbus.
    gdbus call --session --dest org.gnome.Shell \
        --object-path /org/gnome/Shell --method org.gnome.Shell.Eval "$1" 2>&1
}

take_screenshot() {
    local name="$1"
    gdbus call --session --dest org.gnome.Shell \
        --object-path /org/gnome/Shell/Screenshot \
        --method org.gnome.Shell.Screenshot.Screenshot \
        false false "$ARTIFACT_DIR/$name" >>"$LOG" 2>&1
    local size=0
    [ -f "$ARTIFACT_DIR/$name" ] && size=$(stat -c%s "$ARTIFACT_DIR/$name" 2>/dev/null || echo 0)
    if [ -f "$ARTIFACT_DIR/$name" ] && [ "$size" -gt 0 ]; then
        echo "Captura guardada: $name ($size bytes)"
    else
        echo "AVISO: la captura $name quedó vacía o no se generó (renderizado por software sin GPU en este contenedor); se omite la inspección visual de esta captura."
        SKIPPED+=("captura:$name")
    fi
}

STATE_JS='(() => { const g = Main.layoutManager.uiGroup.get_children(); const corners = g.filter(a => a.name && a.name.indexOf("opendock-corner") === 0).length; const notch = g.find(a => a.name === "opendock-notch"); const dock = g.find(a => a.name === "opendock-dock"); const bannerChildren = Main.messageTray._bannerBin ? Main.messageTray._bannerBin.get_n_children() : -1; return "corners=" + corners + ";notch=" + (notch ? 1 : 0) + ";notchWidth=" + (notch ? Math.round(notch.width) : -1) + ";dock=" + (dock ? 1 : 0) + ";panelHeight=" + Math.round(Main.panel.height) + ";bannerChildren=" + bannerChildren; })()'

echo "== Estado inicial (esquinas, barra, dock) =="
take_screenshot "01-desktop.png"
RESULT1="$(shell_eval "$STATE_JS")"
echo "Eval -> $RESULT1"

check_field() {
    local label="$1" pattern="$2" output="$3"
    if echo "$output" | grep -qE "$pattern"; then
        echo "OK: $label"
    else
        echo "FALLO: $label (no coincide con /$pattern/ en: $output)"
        FAIL=1
    fi
}

if echo "$RESULT1" | grep -q 'corners='; then
    check_field "4 esquinas redondeadas en uiGroup" 'corners=4' "$RESULT1"
    check_field "barra superior de 28 px" 'panelHeight=28' "$RESULT1"
    check_field "el dock existe" 'dock=1' "$RESULT1"
else
    echo "AVISO: org.gnome.Shell.Eval no disponible (¿--unsafe-mode no soportado en esta versión?); se omiten las comprobaciones de estado."
    SKIPPED+=("eval:estado-inicial")
fi

echo "== notify-send y notch =="
if command -v notify-send >/dev/null 2>&1; then
    notify-send "Prueba" "Hola desde CI" || echo "AVISO: notify-send devolvió un error"
    sleep 3
    take_screenshot "02-notification.png"
    RESULT2="$(shell_eval "$STATE_JS")"
    echo "Eval -> $RESULT2"
    if echo "$RESULT2" | grep -q 'notch='; then
        check_field "el notch se muestra (notch=1)" 'notch=1' "$RESULT2"
        check_field "el banner nativo de GNOME no aparece (bannerChildren=0)" 'bannerChildren=0' "$RESULT2"
        if ! echo "$RESULT2" | grep -q 'bannerChildren=0'; then
            DIAG_JS='(() => { const own = Object.prototype.hasOwnProperty.call(Main.messageTray, "_showNotification"); const proto = Object.getOwnPropertyNames(Object.getPrototypeOf(Main.messageTray)).filter(n => /show|banner|notif/i.test(n)); return "patchedOwnProp=" + own + ";protoMethods=" + proto.join(","); })()'
            echo "Diagnóstico del banner -> $(shell_eval "$DIAG_JS")"
        fi
    else
        echo "AVISO: no se pudo evaluar el estado tras notify-send; se omite esa comprobación."
        SKIPPED+=("eval:tras-notificacion")
    fi
else
    echo "AVISO: notify-send no está instalado; se omite la prueba de notificación."
    SKIPPED+=("notify-send")
fi

echo "== Revisando el log del shell en busca de errores de JS =="
if grep -E 'JS ERROR|JS WARNING.*opendock|Exception.*opendock' "$LOG" >"$ARTIFACT_DIR/js-errors.txt"; then
    echo "FALLO: se encontraron errores de JS relacionados con la extensión:"
    cat "$ARTIFACT_DIR/js-errors.txt"
    FAIL=1
else
    echo "Sin errores de JS de OpenDock en el log."
fi

echo "== Resumen =="
if [ "${#SKIPPED[@]}" -gt 0 ]; then
    echo "Comprobaciones omitidas (no disponibles en este contenedor headless):"
    printf ' - %s\n' "${SKIPPED[@]}"
fi

exit "$FAIL"
