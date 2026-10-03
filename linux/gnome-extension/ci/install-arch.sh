#!/usr/bin/env bash
# Dependencias para probar la extensión en un contenedor archlinux:latest.
set -euo pipefail

pacman -Sy --noconfirm --needed \
    gnome-shell \
    mutter \
    gjs \
    dbus \
    glib2 \
    git \
    libnotify \
    zip \
    unzip \
    shadow \
    util-linux \
    which \
    xorg-server-xvfb
