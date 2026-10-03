#!/usr/bin/env bash
# Dependencias para probar la extensión en un contenedor fedora (latest,
# o una versión fijada como fedora:41/fedora:42 para probar un GNOME
# concreto). mesa-dri-drivers/mesa-libEGL/mesa-libgbm dan el renderizador
# por software (llvmpipe) que necesita org.gnome.Shell.Screenshot para
# producir un PNG real en un contenedor sin GPU.
set -euo pipefail

dnf install -y --setopt=install_weak_deps=False \
    gnome-shell \
    mutter \
    gjs \
    dbus-daemon \
    dbus-tools \
    glib2 \
    glib2-devel \
    glibc-langpack-en \
    git \
    libnotify \
    zip \
    unzip \
    shadow-utils \
    util-linux \
    which \
    xorg-x11-server-Xvfb \
    mesa-dri-drivers \
    mesa-libEGL \
    mesa-libgbm \
    ImageMagick

dnf clean all
