#!/usr/bin/env bash
# Prueba funcional en un sway sin cabeza (wlroots, backend "headless").
# Arranca sway + un bus de sesión D-Bus propio, lanza opendock, toma
# capturas con grim y las valida con check_screenshot.py.
#
# Variable de entorno PHASES: lista separada por comas de lo que hay que
# comprobar en esta fase del desarrollo. Valores posibles: corners,notch,bar,dock
set -euo pipefail

export PHASES="${PHASES:-corners}"
export ANCHO=1280
export ALTO=800
export RADIO=16
export BAR_ALTURA=28
export ARTEFACTOS="${1:-/tmp/opendock-ci-screens}"
mkdir -p "$ARTEFACTOS"

export REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export OPENDOCK_BIN="$REPO_ROOT/linux/opendock/build/opendock"
export CHECK_PY="$REPO_ROOT/linux/opendock/tests/check_screenshot.py"

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/xdg-runtime-opendock}"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
export XDG_CONFIG_HOME="$ARTEFACTOS/config"
mkdir -p "$XDG_CONFIG_HOME"
# Sin apps ancladas: el runner trae Firefox (anclado por defecto) y la
# prueba del dock necesita empezar vacío para ver entrar y salir una ventana.
mkdir -p "$XDG_CONFIG_HOME/opendock"
printf '[dock]
apps=
' > "$XDG_CONFIG_HOME/opendock/config.ini"

export SWAY_CFG="$ARTEFACTOS/sway-config"
cat > "$SWAY_CFG" <<EOF
output * resolution ${ANCHO}x${ALTO} position 0,0 bg #ffffff solid_color
seat seat0 hide_cursor 1
exec_always 'echo sway-listo'
EOF

export WLR_BACKENDS=headless
export WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER=pixman
export GDK_BACKEND=wayland
# Los runners de CI no tienen GPU: forzamos render por software tanto en
# sway/wlroots (pixman) como en GTK4 (GSK con el renderizador "cairo").
export LIBGL_ALWAYS_SOFTWARE=1
export GSK_RENDERER=cairo

fallo_general=0

ejecutar_dentro_de_sway() {
    # Todo lo que necesita WAYLAND_DISPLAY/D-Bus corre aquí dentro.
    sway -c "$SWAY_CFG" >"$ARTEFACTOS/sway.log" 2>&1 &
    SWAY_PID=$!

    # Espera a que sway publique un WAYLAND_DISPLAY (crea el socket en XDG_RUNTIME_DIR).
    for _ in $(seq 1 50); do
        WAYLAND_SOCK=$(ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -n1 || true)
        [ -n "$WAYLAND_SOCK" ] && break
        sleep 0.2
    done
    if [ -z "${WAYLAND_SOCK:-}" ]; then
        echo "FALLO: sway no arrancó (no hay socket wayland-N en $XDG_RUNTIME_DIR)"
        cat "$ARTEFACTOS/sway.log" || true
        kill "$SWAY_PID" 2>/dev/null || true
        return 1
    fi
    export WAYLAND_DISPLAY="$WAYLAND_SOCK"
    echo "sway listo: WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
    sleep 1

    # Mensajes de depuración de opendock (sin dominio) en el log de la prueba.
    G_MESSAGES_DEBUG=all "$OPENDOCK_BIN" >"$ARTEFACTOS/opendock.log" 2>&1 &
    OPENDOCK_PID=$!
    sleep 2

    local ok=0
    # Con la barra, las esquinas de arriba empiezan debajo de ella.
    local desplazamiento=0
    [[ "$PHASES" == *bar* ]] && desplazamiento=$BAR_ALTURA

    grim "$ARTEFACTOS/01-esquinas.png"
    if ! python3 "$CHECK_PY" "$ARTEFACTOS/01-esquinas.png" --width "$ANCHO" --height "$ALTO" \
            --radius "$RADIO" --bg 255 255 255 --top-offset "$desplazamiento" --check corners; then
        ok=1
    fi

    if [[ "$PHASES" == *bar* ]]; then
        grim "$ARTEFACTOS/02-barra.png"
        if ! python3 "$CHECK_PY" "$ARTEFACTOS/02-barra.png" --width "$ANCHO" \
                --bar-height "$BAR_ALTURA" --check bar; then
            ok=1
        fi
        # Zona exclusiva: una ventana (sway la pone a pantalla completa del
        # espacio de trabajo) debe empezar justo debajo de la barra.
        if command -v foot >/dev/null && command -v swaymsg >/dev/null; then
            foot &
            local foot_pid=$!
            sleep 1.5
            local y_ventana
            # swaymsg busca a sway por SWAYSOCK, que sólo tienen sus hijos.
            export SWAYSOCK
            SWAYSOCK=$(ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock 2>/dev/null | head -n1 || true)
            y_ventana=$(swaymsg -t get_tree | python3 -c '
import json, sys
def buscar(n):
    if n.get("app_id") == "foot":
        return n["rect"]["y"]
    for h in n.get("nodes", []) + n.get("floating_nodes", []):
        r = buscar(h)
        if r is not None:
            return r
print(buscar(json.load(sys.stdin)))')
            echo "ventana foot en y=$y_ventana"
            if ! [[ "$y_ventana" =~ ^[0-9]+$ ]] || [ "$y_ventana" -lt "$BAR_ALTURA" ]; then
                echo "FALLO: la ventana no respeta la zona exclusiva de la barra"
                ok=1
            else
                echo "ok: la ventana empieza debajo de la barra"
            fi
            kill "$foot_pid" 2>/dev/null || true
            sleep 0.5
        else
            echo "AVISO: sin foot/swaymsg, se omite la comprobación de la zona exclusiva"
        fi
    fi

    if [[ "$PHASES" == *notch* ]]; then
        if command -v notify-send >/dev/null; then
            notify-send "Prueba" "Hola desde CI"
            sleep 2
            grim "$ARTEFACTOS/03-notch.png"
            if ! python3 "$CHECK_PY" "$ARTEFACTOS/03-notch.png" --width "$ANCHO" --check notch; then
                ok=1
            fi
            if command -v gdbus >/dev/null; then
                NOMBRE_SERVIDOR=$(gdbus call --session \
                    --dest org.freedesktop.Notifications \
                    --object-path /org/freedesktop/Notifications \
                    --method org.freedesktop.Notifications.GetServerInformation 2>&1 || true)
                echo "GetServerInformation -> $NOMBRE_SERVIDOR"
                case "$NOMBRE_SERVIDOR" in
                    *OpenDock*) echo "ok: opendock es el servidor de notificaciones" ;;
                    *) echo "FALLO: opendock no respondió como servidor de notificaciones"; ok=1 ;;
                esac
            fi
        else
            echo "AVISO: notify-send no disponible, se omite la comprobación del notch"
        fi
    fi

    if [[ "$PHASES" == *centro* ]]; then
        centro() {
            gdbus call --session --dest io.github.josuegm24.OpenDock \
                --object-path /io/github/josuegm24/OpenDock \
                --method org.gtk.Actions.Activate centro '[]' '{}' >/dev/null
        }
        captura() {  # captura <archivo> <check>
            grim "$ARTEFACTOS/$1"
            python3 "$CHECK_PY" "$ARTEFACTOS/$1" --width "$ANCHO" --height "$ALTO" \
                --bar-height "$BAR_ALTURA" --check "$2"
        }
        # Una notificación con acciones (gdbus: notify-send -A se queda esperando).
        gdbus call --session --dest org.freedesktop.Notifications \
            --object-path /org/freedesktop/Notifications \
            --method org.freedesktop.Notifications.Notify \
            "CI" 0 "" "Con acciones" "Cuerpo largo de una notificación de prueba" \
            '["default", "Abrir", "ok", "Vale"]' '{}' 5000 >/dev/null
        sleep 5   # que se vaya el aviso del notch

        centro; sleep 1
        captura 05-centro.png center || ok=1
        centro; sleep 1
        captura 06-centro-cerrado.png closed || ok=1

        # CloseNotification de todas (los ids empiezan en 1): el centro queda vacío.
        for id in $(seq 1 10); do
            gdbus call --session --dest org.freedesktop.Notifications \
                --object-path /org/freedesktop/Notifications \
                --method org.freedesktop.Notifications.CloseNotification "$id" >/dev/null 2>&1 || true
        done
        centro; sleep 1
        captura 07-centro-vacio.png center-empty || ok=1
        centro; sleep 1

        export SWAYSOCK
        SWAYSOCK=$(ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock 2>/dev/null | head -n1 || true)
        # Mini notch: cursor al centro de la barra. sway sin cabeza puede no
        # tener puntero, así que esto sólo avisa.
        # Mini notch: sway sin cabeza no tiene puntero, así que se le pasa la
        # posición del cursor con la acción "cursor-barra" (dx desde el centro).
        cursor_barra() {
            gdbus call --session --dest io.github.josuegm24.OpenDock                 --object-path /io/github/josuegm24/OpenDock                 --method org.gtk.Actions.Activate cursor-barra "[<$1>]" '{}' >/dev/null
        }
        captura 08-sin-mini.png closed || ok=1
        cursor_barra 20.0     # a 20 px del centro: el imán la deja centrada
        sleep 0.2
        captura 09-mini.png mini || ok=1
        sleep 0.6             # quieto más de 450 ms
        captura 10-rapida.png quick || ok=1
        cursor_barra 9999.0   # fuera de la zona: se esconde
        sleep 0.5
        captura 11-mini-fuera.png closed || ok=1
    fi

    if [[ "$PHASES" == *bandeja* ]]; then
        vigilante() {
            gdbus call --session --dest org.kde.StatusNotifierWatcher \
                --object-path /StatusNotifierWatcher \
                --method org.freedesktop.DBus.Properties.Get \
                org.kde.StatusNotifierWatcher RegisteredStatusNotifierItems 2>&1
        }
        if gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
                --method org.freedesktop.DBus.GetNameOwner org.kde.StatusNotifierWatcher >/dev/null 2>&1; then
            echo "ok: opendock ofrece org.kde.StatusNotifierWatcher"
        else
            echo "FALLO: nadie ofrece org.kde.StatusNotifierWatcher"
            ok=1
        fi
        SNI_LOG="$ARTEFACTOS/sni.log"
        : > "$SNI_LOG"
        python3 "$REPO_ROOT/linux/opendock/tests/sni_falso.py" "$SNI_LOG" 60 &
        SNI_PID=$!
        sleep 2
        echo "Iconos registrados -> $(vigilante)"
        if vigilante | grep -q "/StatusNotifierItem"; then
            echo "ok: el icono falso queda registrado"
        else
            echo "FALLO: el icono falso no aparece en RegisteredStatusNotifierItems"
            ok=1
        fi
        if grep -q "propiedad IconName" "$SNI_LOG"; then
            echo "ok: la bandeja lee las propiedades del icono"
        else
            echo "FALLO: la bandeja no leyó las propiedades del icono"
            cat "$SNI_LOG"
            ok=1
        fi
        # Desplegar la bandeja para la captura (la comprueba una persona).
        gdbus call --session --dest io.github.josuegm24.OpenDock \
            --object-path /io/github/josuegm24/OpenDock \
            --method org.gtk.Actions.Activate bandeja '[]' '{}' >/dev/null
        sleep 1
        grim "$ARTEFACTOS/15-bandeja.png"
        gdbus call --session --dest io.github.josuegm24.OpenDock \
            --object-path /io/github/josuegm24/OpenDock \
            --method org.gtk.Actions.Activate bandeja '[]' '{}' >/dev/null
        kill "$SNI_PID" 2>/dev/null || true
        sleep 1
        if vigilante | grep -q "/StatusNotifierItem"; then
            echo "FALLO: el icono sigue registrado después de cerrarse su proceso"
            ok=1
        else
            echo "ok: al cerrarse su proceso el icono se da de baja"
        fi
    fi

    if [[ "$PHASES" == *dock* ]]; then
        # Sin ventanas y sin apps ancladas (config.ini de arriba), el dock no
        # dibuja nada.
        grim "$ARTEFACTOS/12-sin-dock.png"
        if python3 "$CHECK_PY" "$ARTEFACTOS/12-sin-dock.png" --width "$ANCHO" --height "$ALTO" --check dock >/dev/null; then
            echo "FALLO: el dock se dibuja sin ninguna app"
            ok=1
        else
            echo "ok: sin apps, el dock no se dibuja"
        fi
        if command -v foot >/dev/null; then
            foot &
            FOOT_PID=$!
            sleep 1.5
            # La ventana de foot sale en el dock (zwlr_foreign_toplevel)...
            grim "$ARTEFACTOS/13-dock.png"
            if ! python3 "$CHECK_PY" "$ARTEFACTOS/13-dock.png" --width "$ANCHO" --height "$ALTO" --check dock; then
                ok=1
            fi
            # ...y termina por encima de la media altura que reserva el dock.
            export SWAYSOCK
            SWAYSOCK=$(ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock 2>/dev/null | head -n1 || true)
            fondo_ventana=$(swaymsg -t get_tree | python3 -c '
import json, sys
def buscar(n):
    if n.get("app_id") == "foot":
        return n["rect"]["y"] + n["rect"]["height"]
    for h in n.get("nodes", []) + n.get("floating_nodes", []):
        r = buscar(h)
        if r is not None:
            return r
print(buscar(json.load(sys.stdin)))')
            echo "ventana foot termina en y=$fondo_ventana"
            if [[ "$fondo_ventana" =~ ^[0-9]+$ ]] && [ "$fondo_ventana" -le $((ALTO - 42)) ]; then
                echo "ok: la ventana deja libres los 42 px del dock"
            else
                echo "FALLO: la ventana no respeta la zona del dock"
                ok=1
            fi
            kill "$FOOT_PID" 2>/dev/null || true
            sleep 1
            # Al cerrarla, el icono (no anclado) desaparece.
            grim "$ARTEFACTOS/14-dock-vacio.png"
            if python3 "$CHECK_PY" "$ARTEFACTOS/14-dock-vacio.png" --width "$ANCHO" --height "$ALTO" --check dock >/dev/null; then
                echo "FALLO: el icono de foot sigue en el dock tras cerrarla"
                ok=1
            else
                echo "ok: al cerrar foot su icono sale del dock"
            fi
        else
            echo "AVISO: no hay 'foot' disponible, se omite la comprobación de ventana abierta en el dock"
        fi
    fi

    kill "$OPENDOCK_PID" 2>/dev/null || true
    wait "$OPENDOCK_PID" 2>/dev/null || true
    kill "$SWAY_PID" 2>/dev/null || true
    wait "$SWAY_PID" 2>/dev/null || true

    return $ok
}

if ! dbus-run-session -- bash -c "$(declare -f ejecutar_dentro_de_sway); ejecutar_dentro_de_sway"; then
    fallo_general=1
fi

echo "--- logs de opendock ---"
cat "$ARTEFACTOS/opendock.log" 2>/dev/null || true
echo "--- logs de sway ---"
cat "$ARTEFACTOS/sway.log" 2>/dev/null || true

exit $fallo_general
