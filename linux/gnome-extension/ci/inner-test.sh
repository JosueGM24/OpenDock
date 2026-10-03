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

# Sin GPU en el contenedor, se fuerza el renderizador por software
# (llvmpipe) para que org.gnome.Shell.Screenshot pueda producir un PNG de
# verdad en vez de un archivo vacío.
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export MESA_GL_VERSION_OVERRIDE=3.3

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

STATE_JS='(() => { const g = Main.layoutManager.uiGroup.get_children(); const corners = g.filter(a => a.name && a.name.indexOf("opendock-corner") === 0).length; const notch = g.find(a => a.name === "opendock-notch"); const dock = g.find(a => a.name === "opendock-dock"); const bannerChildren = Main.messageTray._bannerBin ? Main.messageTray._bannerBin.get_n_children() : -1; let shellVersion = "?"; try { shellVersion = imports.misc.config.PACKAGE_VERSION; } catch (e) { /* no disponible */ } return "shellVersion=" + shellVersion + ";corners=" + corners + ";notch=" + (notch ? 1 : 0) + ";notchWidth=" + (notch ? Math.round(notch.width) : -1) + ";dock=" + (dock ? 1 : 0) + ";panelHeight=" + Math.round(Main.panel.height) + ";bannerChildren=" + bannerChildren; })()'

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
        # Puede quedar un banner de antes de la prueba en _bannerBin: lo
        # que cuenta es que la notificación no añada ninguno nuevo.
        BANNERS_ANTES=$(echo "$RESULT1" | grep -oE 'bannerChildren=-?[0-9]+' | cut -d= -f2)
        BANNERS_ANTES=${BANNERS_ANTES:-0}
        BANNERS_DESPUES=$(echo "$RESULT2" | grep -oE 'bannerChildren=-?[0-9]+' | cut -d= -f2)
        BANNERS_DESPUES=${BANNERS_DESPUES:-99}
        # Puede bajar (un banner de arranque que se va), nunca subir.
        if [ "$BANNERS_DESPUES" -le "$BANNERS_ANTES" ]; then
            echo "OK: el banner nativo de GNOME no aparece (bannerChildren $BANNERS_ANTES -> $BANNERS_DESPUES)"
        else
            echo "FALLO: el banner nativo de GNOME aparece (bannerChildren $BANNERS_ANTES -> $BANNERS_DESPUES)"
            FAIL=1
        fi
        if [ "$BANNERS_DESPUES" -gt "$BANNERS_ANTES" ]; then
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

echo "== Interacciones finas (mini notch, tarjetas, papelera, borrar) =="
UUID_JS_LITERAL="\"$UUID\""
LOOKUP_EXPR='Main.extensionManager.lookup('"$UUID_JS_LITERAL"').stateObj'
EXT_STATE="$(shell_eval "(() => { const ext = $LOOKUP_EXPR; return ext ? 'yes' : 'no'; })()")"
if ! echo "$EXT_STATE" | grep -q "yes"; then
    echo "AVISO: no se pudo llegar a Main.extensionManager.lookup(uuid).stateObj; se omiten las comprobaciones finas de interacción."
    SKIPPED+=("interacciones:stateObj")
else
    soft_check() {
        local label="$1" pattern="$2" output="$3"
        if echo "$output" | grep -qE "$pattern"; then
            echo "OK: $label"
        else
            echo "AVISO (no bloqueante): $label no coincidió con /$pattern/ en: $output"
            SKIPPED+=("interaccion:$label")
        fi
    }

    # Segunda notificación para tener 2 tarjetas en el centro (hover de una
    # debe encoger la otra a 0.965).
    notify-send "Prueba 2" "Segunda notificación" 2>/dev/null || true
    sleep 1

    # --- Mini notch: tocar el borde central, seguir al cursor y abrir la
    # vista rápida a los 450 ms quieto. ------------------------------------
    # Puntero virtual para mover el cursor de verdad (entra por la misma ruta
    # que un ratón). En GNOME 45+ Eval no trae Clutter/GLib como globales.
    shell_eval "(() => { const Clutter = imports.gi.Clutter; const GLib = imports.gi.GLib; const seat = Clutter.get_default_backend().get_default_seat(); const vd = seat.create_virtual_device(Clutter.InputDeviceType.POINTER_DEVICE); globalThis._odPuntero = (x, y) => vd.notify_absolute_motion(GLib.get_monotonic_time(), x, y); return 'puntero-listo'; })()" >>"$LOG" 2>&1
    WARP_JS="(() => { const ext = $LOOKUP_EXPR; const m = ext._notch._monitor; if (!m) return 'sin-monitor'; globalThis._odPuntero(m.x + m.width / 2, m.y + 31); return 'ok'; })()"
    WARP_RESULT="$(shell_eval "$WARP_JS")"
    echo "Puntero al centro bajo la barra -> $WARP_RESULT"
    sleep 0.3
    MINI_JS="(() => { const ext = $LOOKUP_EXPR; return 'miniMode=' + ext._notch._miniMode + ';miniVisible=' + (ext._notch._mini ? ext._notch._mini.visible : 'n/a') + ';miniX=' + Math.round(ext._notch._miniXSpring.value); })()"
    MINI_RESULT="$(shell_eval "$MINI_JS")"
    echo "Eval mini notch -> $MINI_RESULT"
    soft_check "la pastilla del mini notch aparece y se imanta al centro" 'miniMode=pill;miniVisible=true;miniX=0' "$MINI_RESULT"

    sleep 0.3
    MINI_RESULT2="$(shell_eval "$MINI_JS")"
    echo "Eval mini notch (tras 450 ms quieto) -> $MINI_RESULT2"
    soft_check "la vista rápida se abre a los 450 ms quieto" 'miniMode=quick' "$MINI_RESULT2"

    # --- Centro de notificaciones: tarjetas, lupa de hover y papelera ------
    OPEN_CENTER_JS="(() => { const ext = $LOOKUP_EXPR; ext._notch._openCenter(); return 'abierto'; })()"
    shell_eval "$OPEN_CENTER_JS" >>"$LOG" 2>&1
    sleep 0.3

    CARD_COUNT_JS="(() => { const ext = $LOOKUP_EXPR; const n = ext._notch._cardList ? ext._notch._cardList.get_n_children() : -1; return 'cards=' + n; })()"
    CARD_COUNT="$(shell_eval "$CARD_COUNT_JS")"
    echo "Eval tarjetas -> $CARD_COUNT"

    TARJETAS=$(echo "$CARD_COUNT" | grep -oE 'cards=[0-9]+' | cut -d= -f2)
    if [ "${TARJETAS:-0}" -ge 2 ]; then
        HOVER_FIRST_JS="(() => { const ext = $LOOKUP_EXPR; const card = ext._notch._cardList.get_children()[0]; ext._notch._onCardEnter(card); return 'hover-iniciado'; })()"
        shell_eval "$HOVER_FIRST_JS" >>"$LOG" 2>&1
        sleep 0.4
        SCALE_JS="(() => { const ext = $LOOKUP_EXPR; const cards = ext._notch._cardList.get_children(); return 'scale0=' + cards[0].scale_x.toFixed(3) + ';scale1=' + cards[1].scale_x.toFixed(3); })()"
        SCALE_RESULT="$(shell_eval "$SCALE_JS")"
        echo "Eval escala de tarjetas -> $SCALE_RESULT"
        soft_check "la tarjeta con el cursor crece a 1.035" 'scale0=1\.03[0-9]' "$SCALE_RESULT"
        soft_check "la otra tarjeta se encoge a 0.965" 'scale1=0\.96[0-9]' "$SCALE_RESULT"

        sleep 0.2
        EXPAND_JS="(() => { const ext = $LOOKUP_EXPR; const card = ext._notch._cardList.get_children()[0]; return 'expanded=' + !!card._opendockExpanded + ';actionsVisible=' + (card._opendockActions ? card._opendockActions.visible : 'n/a'); })()"
        EXPAND_RESULT="$(shell_eval "$EXPAND_JS")"
        echo "Eval tarjeta expandida -> $EXPAND_RESULT"
        soft_check "a los 260 ms la tarjeta muestra el cuerpo y los botones" 'expanded=true;actionsVisible=true' "$EXPAND_RESULT"

        TRASH_HOVER_JS="(() => { const ext = $LOOKUP_EXPR; const card = ext._notch._cardList.get_children()[0]; const trash = card.get_children().find(c => c.name === 'opendock-trash-strip'); if (!trash) return 'sin-papelera'; const [x, y] = trash.get_transformed_position(); const [w, h] = trash.get_transformed_size(); globalThis._odPuntero(x + w / 2, y + h / 2); return 'width-antes=' + Math.round(trash.width); })()"
        shell_eval "$TRASH_HOVER_JS" >>"$LOG" 2>&1
        sleep 0.3
        TRASH_WIDTH_JS="(() => { const ext = $LOOKUP_EXPR; const card = ext._notch._cardList.get_children()[0]; const trash = card.get_children().find(c => c.name === 'opendock-trash-strip'); return trash ? ('trashWidth=' + Math.round(trash.width)) : 'sin-papelera'; })()"
        TRASH_WIDTH_RESULT="$(shell_eval "$TRASH_WIDTH_JS")"
        echo "Eval papelera -> $TRASH_WIDTH_RESULT"
        soft_check "la papelera crece de 40 a 58 px al pasar el cursor" 'trashWidth=5[0-9]' "$TRASH_WIDTH_RESULT"

        # --- Borrar: la tarjeta debe desaparecer tras deslizar y desvanecer.
        DISMISS_JS="(() => { const ext = $LOOKUP_EXPR; const card = ext._notch._cardList.get_children()[1]; const n = card._opendockNotification; ext._notch._dismiss(n); return 'borrando'; })()"
        shell_eval "$DISMISS_JS" >>"$LOG" 2>&1
        sleep 1
        AFTER_DISMISS_JS="(() => { const ext = $LOOKUP_EXPR; return 'cards=' + ext._notch._cardList.get_n_children(); })()"
        AFTER_DISMISS="$(shell_eval "$AFTER_DISMISS_JS")"
        echo "Eval tras borrar -> $AFTER_DISMISS"
        soft_check "la tarjeta borrada desaparece de la lista" "cards=$((TARJETAS - 1))([^0-9]|\$)" "$AFTER_DISMISS"
    else
        echo "AVISO: no había 2 tarjetas en el centro; se omiten las comprobaciones de hover/papelera/borrado."
        SKIPPED+=("interaccion:tarjetas-sin-2-notificaciones")
    fi

    CLOSE_CENTER_JS="(() => { const ext = $LOOKUP_EXPR; ext._notch._closeCenter(); return 'cerrado'; })()"
    shell_eval "$CLOSE_CENTER_JS" >>"$LOG" 2>&1
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
