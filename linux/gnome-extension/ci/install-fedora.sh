#!/usr/bin/env bash
# Dependencias para probar la extensión en un contenedor fedora:latest.
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
    xorg-x11-server-Xvfb

dnf clean all
