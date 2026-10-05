#!/bin/sh
# Instala OpenDock en Linux con un solo comando:
#
#     curl -fsSL https://open-dock.netlify.app/install.sh | sh
#
# - En GNOME instala la extensión de GNOME Shell (sin sudo, solo para tu usuario).
# - En el resto (KDE Plasma, Hyprland, Sway, labwc, COSMIC, XFCE, MATE, Cinnamon, i3...)
#   instala el paquete nativo de tu distribución: .deb, .rpm o el de Arch (pide sudo).
#
# Opciones (con `| sh -s -- <opción>`):
#   --gnome        forzar la extensión de GNOME
#   --nativo       forzar el paquete nativo
#   --desinstalar  quitar OpenDock
#
# Todo se descarga de las versiones de GitHub (https://github.com/JosueGM24/OpenDock/releases)
# y se comprueba con su SHA-256 antes de instalar. Puedes leer este script antes de ejecutarlo:
#     curl -fsSL https://open-dock.netlify.app/install.sh | less
set -eu

REPO="JosueGM24/OpenDock"
BASE="https://github.com/$REPO/releases/latest/download"
UUID="opendock@josuegm24.github.io"
EXT_ZIP="$UUID.shell-extension.zip"

# Todo va dentro de main(): si la descarga del script se corta a medias, no se ejecuta nada.
main() {
    if [ -t 1 ]; then B=$(printf '\033[1m'); D=$(printf '\033[2m'); R=$(printf '\033[0m'); E=$(printf '\033[31m'); else B='' D='' R='' E=''; fi

    modo=auto
    for a in "$@"; do
        case "$a" in
            --gnome) modo=gnome ;;
            --nativo|--native) modo=nativo ;;
            --desinstalar|--uninstall) modo=desinstalar ;;
            -h|--help) printf '%s
' "Uso: curl -fsSL https://open-dock.netlify.app/install.sh | sh -s -- [--gnome|--nativo|--desinstalar]"; exit 0 ;;
            *) fallo "opción desconocida: $a" ;;
        esac
    done

    [ "$(uname -s)" = Linux ] || fallo "este instalador es para Linux. En Windows descarga OpenDock.exe desde https://open-dock.netlify.app/"

    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT INT TERM

    if [ "$modo" = auto ]; then
        if es_gnome; then modo=gnome; else modo=nativo; fi
    fi

    case "$modo" in
        gnome)       instalar_gnome ;;
        nativo)      instalar_nativo ;;
        desinstalar) desinstalar ;;
    esac
}

dice() { printf '%s\n' "${B}==>${R} $*"; }
nota() { printf '%s\n' "    ${D}$*${R}"; }
fallo() { printf '%s\n' "${E}${B}error:${R} $*" >&2; exit 1; }
hay() { command -v "$1" >/dev/null 2>&1; }

# GNOME Shell en marcha (y no Budgie/Cinnamon/Unity, que también dicen "GNOME").
es_gnome() {
    if hay pgrep && pgrep -x gnome-shell >/dev/null 2>&1; then return 0; fi
    case "$(printf '%s' "${XDG_CURRENT_DESKTOP:-}" | tr '[:upper:]' '[:lower:]')" in
        *budgie*|*cinnamon*|*unity*|*pantheon*) return 1 ;;
        *gnome*) return 0 ;;
    esac
    return 1
}

descargar() {   # descargar <archivo de la versión> <destino>
    if hay curl; then curl -fL --proto '=https' --tlsv1.2 --retry 3 -sS -o "$2" "$BASE/$1"
    elif hay wget; then wget -q --https-only -O "$2" "$BASE/$1"
    else fallo "hace falta curl o wget"
    fi || fallo "no se pudo descargar $1 (¿sin conexión, o la versión aún no lo incluye?)"
}

# Comprueba el archivo contra SHA256SUMS-linux.txt de la misma versión.
verificar() {   # verificar <archivo> <nombre en la versión>
    [ -f "$tmp/SHA256SUMS" ] || descargar SHA256SUMS-linux.txt "$tmp/SHA256SUMS"
    esperado=$(awk -v n="$2" '{ f = $2; sub(/^\*/, "", f) } f == n { print $1 }' "$tmp/SHA256SUMS")
    [ -n "$esperado" ] || fallo "$2 no aparece en SHA256SUMS-linux.txt"
    if hay sha256sum; then real=$(sha256sum "$1" | cut -d' ' -f1)
    else real=$(shasum -a 256 "$1" | cut -d' ' -f1)
    fi
    [ "$real" = "$esperado" ] || fallo "el SHA-256 de $2 no coincide: descarga dañada o manipulada"
    nota "SHA-256 correcto"
}

como_root() {
    if [ "$(id -u)" = 0 ]; then "$@"
    elif hay sudo; then sudo "$@"
    elif hay doas; then doas "$@"
    else fallo "hace falta sudo (o doas) para instalar el paquete"
    fi
}

# ───────────────────────── GNOME ─────────────────────────
instalar_gnome() {
    dice "GNOME Shell: instalo la extensión de OpenDock (solo para tu usuario, sin sudo)"
    hay gnome-extensions || fallo "falta el comando gnome-extensions (paquete gnome-shell)"
    ver=$(gnome-shell --version 2>/dev/null | sed -n 's/[^0-9]*\([0-9][0-9]*\).*/\1/p')
    if [ -n "$ver" ] && [ "$ver" -lt 45 ]; then
        fallo "OpenDock necesita GNOME 45 o más nuevo (tienes GNOME $ver)"
    fi

    # Si ya está en extensions.gnome.org, GNOME la instala y la activa al momento
    # (te pide confirmar en una ventana), sin cerrar sesión.
    if hay gdbus && en_ego "$ver"; then
        dice "Instalando desde extensions.gnome.org: confirma en la ventana que aparece"
        if gdbus call --session --dest org.gnome.Shell.Extensions --object-path /org/gnome/Shell/Extensions \
               --method org.gnome.Shell.Extensions.InstallRemoteExtension "$UUID" 2>/dev/null | grep -q successful; then
            dice "${B}Listo.${R} OpenDock ya está funcionando."
            nota "Ajustes: gnome-extensions prefs $UUID"
            return
        fi
        nota "No se completó desde extensions.gnome.org; sigo con la versión de GitHub"
    fi

    descargar "$EXT_ZIP" "$tmp/$EXT_ZIP"
    verificar "$tmp/$EXT_ZIP" "$EXT_ZIP"
    gnome-extensions install --force "$tmp/$EXT_ZIP"

    # Que quede activada: ahora mismo si el shell ya la ve, si no al volver a entrar.
    if hay gsettings; then
        gsettings set org.gnome.shell disable-user-extensions false 2>/dev/null || true
        act=$(gsettings get org.gnome.shell enabled-extensions 2>/dev/null || echo "@as []")
        case "$act" in
            *"'$UUID'"*) ;;
            *"[]"*) gsettings set org.gnome.shell enabled-extensions "['$UUID']" ;;
            *) gsettings set org.gnome.shell enabled-extensions "$(printf '%s' "$act" | sed "s/]\$/, '$UUID']/")" ;;
        esac
    fi
    if gnome-extensions enable "$UUID" 2>/dev/null && gnome-extensions info "$UUID" 2>/dev/null | grep -qi 'state: active\|estado: activ'; then
        dice "${B}Listo.${R} OpenDock ya está funcionando."
    else
        dice "${B}Instalado.${R} Cierra sesión y vuelve a entrar: OpenDock arrancará solo."
        nota "GNOME solo carga extensiones nuevas al iniciar sesión."
    fi
    nota "Ajustes: gnome-extensions prefs $UUID"
}

en_ego() {      # en_ego <versión de GNOME>
    url="https://extensions.gnome.org/extension-info/?uuid=$UUID&shell_version=${1:-45}"
    if hay curl; then curl -fsS -o /dev/null --max-time 8 "$url" 2>/dev/null
    elif hay wget; then wget -q -O /dev/null --timeout=8 "$url" 2>/dev/null
    else return 1
    fi
}

# ───────────────────────── Nativo ─────────────────────────
instalar_nativo() {
    [ "$(uname -m)" = x86_64 ] || fallo "por ahora solo hay paquetes para x86_64; compílalo: https://github.com/$REPO/tree/main/linux/opendock"
    [ -r /etc/os-release ] || fallo "no se reconoce la distribución (falta /etc/os-release)"
    # shellcheck disable=SC1091
    . /etc/os-release
    ids=" ${ID:-} ${ID_LIKE:-} "

    case "$ids" in
        *" arch "*|*" archlinux "*|*" manjaro "*|*" endeavouros "*|*" cachyos "*)
            f=opendock-linux-x86_64.pkg.tar.zst
            dice "Arch Linux: instalo el paquete de OpenDock"
            descargar "$f" "$tmp/$f"; verificar "$tmp/$f" "$f"
            como_root pacman -U --needed --noconfirm "$tmp/$f" ;;
        *" debian "*|*" ubuntu "*)
            if [ "${ID:-}" = ubuntu ] && ! version_minima "${VERSION_ID:-0}" 24.10; then
                fallo "Ubuntu ${VERSION_ID:-} no trae gtk4-layer-shell (hace falta 24.10 o más nuevo). En Ubuntu con GNOME usa: curl -fsSL https://open-dock.netlify.app/install.sh | sh -s -- --gnome"
            fi
            f=opendock-linux-amd64.deb
            dice "Debian/Ubuntu: instalo el paquete de OpenDock"
            descargar "$f" "$tmp/$f"; verificar "$tmp/$f" "$f"
            chmod 644 "$tmp/$f"; chmod 755 "$tmp"     # apt lo lee como usuario _apt
            como_root apt-get install -y "$tmp/$f" ;;
        *" fedora "*|*" rhel "*|*" centos "*)
            f=opendock-linux-x86_64.rpm
            dice "Fedora: instalo el paquete de OpenDock"
            descargar "$f" "$tmp/$f"; verificar "$tmp/$f" "$f"
            como_root dnf install -y "$tmp/$f" ;;
        *" opensuse"*|*" suse "*|*" sles "*)
            f=opendock-linux-x86_64.rpm
            dice "openSUSE: instalo el paquete de OpenDock"
            descargar "$f" "$tmp/$f"; verificar "$tmp/$f" "$f"
            como_root zypper --non-interactive install --allow-unsigned-rpm "$tmp/$f" ;;
        *)
            fallo "aún no hay paquete para ${PRETTY_NAME:-tu distribución}. Compílalo en 3 pasos: https://github.com/$REPO/tree/main/linux/opendock" ;;
    esac

    dice "${B}Listo.${R} OpenDock arrancará solo al iniciar sesión."
    if [ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ] && hay opendock && ! pgrep -x opendock >/dev/null 2>&1; then
        (setsid opendock >/dev/null 2>&1 &) 2>/dev/null || (nohup opendock >/dev/null 2>&1 &)
        nota "Lo he abierto ya. Si tenías otro servidor de notificaciones (mako, dunst...), ciérralo o usa: opendock --replace"
    fi
    nota "Ajustes: ~/.config/opendock/config.ini"
}

version_minima() {   # version_minima <tiene> <mínima>   (24.10 ≥ 24.04)
    [ "$(printf '%s\n%s\n' "$2" "$1" | sort -t. -k1,1n -k2,2n | head -n1)" = "$2" ]
}

# ───────────────────────── Desinstalar ─────────────────────────
desinstalar() {
    hecho=no
    if hay gnome-extensions && gnome-extensions info "$UUID" >/dev/null 2>&1; then
        gnome-extensions uninstall "$UUID" && hecho=si && dice "Extensión de GNOME quitada"
    fi
    if hay opendock || [ -x /usr/bin/opendock ]; then
        pkill -x opendock 2>/dev/null || true
        if hay pacman && pacman -Qq opendock-git >/dev/null 2>&1; then como_root pacman -R --noconfirm opendock-git
        elif hay pacman && pacman -Qq opendock >/dev/null 2>&1; then como_root pacman -R --noconfirm opendock
        elif hay dpkg && dpkg -s opendock >/dev/null 2>&1; then como_root apt-get remove -y opendock
        elif hay rpm && rpm -q opendock >/dev/null 2>&1; then
            if hay dnf; then como_root dnf remove -y opendock; else como_root zypper --non-interactive remove opendock; fi
        else fallo "opendock no se instaló con un paquete; quítalo con: sudo ninja -C build uninstall"
        fi
        hecho=si && dice "OpenDock nativo quitado"
    fi
    [ "$hecho" = si ] || dice "OpenDock no estaba instalado"
    nota "Tus ajustes siguen en ~/.config/opendock (bórralo si no vas a volver)."
}

main "$@"
