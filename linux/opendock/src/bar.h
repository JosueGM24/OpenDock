/*
 * Barra superior: 28 px, zona exclusiva (layer-shell en Wayland, strut en
 * X11). App activa a la izquierda; reloj, Wi-Fi, batería, volumen y el
 * centro de control a la derecha. Ver DESIGN.md.
 */
#ifndef OPENDOCK_BAR_H
#define OPENDOCK_BAR_H

#include "config.h"
#include "session.h"

void od_bar_iniciar(OdConfig *cfg, OdBackendTipo backend);

/* Usado por dock.c / backend-wayland.c cuando se sabe el nombre de la
 * app con foco (foreign-toplevel en Wayland, _NET_ACTIVE_WINDOW en X11). */
void od_bar_set_app_activa(const char *nombre, const char *icono_nombre);

#endif
