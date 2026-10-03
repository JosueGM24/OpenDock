#!/usr/bin/env bash
# Dependencias para probar la extensión en un contenedor ubuntu:24.04
# (GNOME 46 en los repos de Ubuntu Noble).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends \
    gnome-shell \
    mutter \
    gjs \
    dbus-daemon \
    dbus-user-session \
    dbus-x11 \
    libglib2.0-bin \
    git \
    libnotify-bin \
    zip \
    unzip \
    passwd \
    util-linux \
    systemd \
    mesa-utils \
    libgl1-mesa-dri \
    libegl-mesa0 \
    libgbm1 \
    imagemagick \
    x11-apps \
    xvfb \
    ca-certificates

rm -rf /var/lib/apt/lists/*
