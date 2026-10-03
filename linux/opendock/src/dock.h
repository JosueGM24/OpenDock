/*
 * Dock (ver DESIGN.md, "Dock"): apps ancladas y abiertas sobre un panel de
 * 60 px a 12 px del borde inferior, con lupa al pasar el cursor, rebote al
 * abrir y enfocar, indicadores de ventana y ocultación a la mitad o del
 * todo. Reserva media altura del panel (42 px) para las ventanas.
 */
#ifndef OPENDOCK_DOCK_H
#define OPENDOCK_DOCK_H

#include "config.h"
#include "session.h"

void od_dock_iniciar(OdConfig *cfg, OdBackendTipo backend);

#endif
