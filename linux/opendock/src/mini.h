/*
 * Mini notch (DESIGN.md): al tocar la zona central de la barra asoma una
 * pastilla de 110×9 bajo ella que sigue al cursor (k 240) y se imanta al
 * centro a menos de 64 px. Con el cursor quieto 450 ms se abre la vista
 * rápida de 272×44: "Notificaciones", silenciar y el contador.
 */
#ifndef OPENDOCK_MINI_H
#define OPENDOCK_MINI_H

#include "config.h"
#include "session.h"
#include <glib.h>

/* Ancho de la zona central de la barra que despierta el mini notch. */
#define OD_MINI_ZONA 480

void od_mini_iniciar(OdConfig *cfg, OdBackendTipo backend);

/* La barra informa del cursor: 'dentro' si está en la zona central, y
 * 'dx' su distancia horizontal al centro del monitor. */
void od_mini_cursor(gboolean dentro, double dx);

/* Cambió el número de notificaciones o el estado de No molestar. */
void od_mini_refrescar(void);

#endif
