/*
 * Esquinas redondeadas: 4 superficies pequeñas (r×r) por monitor, una por
 * esquina física de la pantalla. Pintan en negro la zona "L" que queda
 * fuera del cuarto de círculo de radio r y dejan transparente (e insensible
 * al ratón) el resto, simulando un panel con esquinas redondeadas de verdad.
 * Mismo truco que corner_radius.c en Windows, adaptado a Wayland
 * (zwlr_layer_shell_v1, capa "overlay", zona exclusiva -1) y a X11 (EWMH).
 */
#ifndef OPENDOCK_CORNERS_H
#define OPENDOCK_CORNERS_H

#include "config.h"
#include "session.h"

/* Crea las 4 esquinas en todos los monitores conectados (backend elegido
 * según la sesión). No hace nada (y no falla) si no hay ningún backend
 * disponible. */
void od_esquinas_iniciar(OdConfig *cfg, OdBackendTipo backend);

#endif
