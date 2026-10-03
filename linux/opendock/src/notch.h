/*
 * Notch: la "isla" bajo la barra superior donde aparecen los avisos.
 * Ver DESIGN.md: aviso 360×54, esquinas de abajo radio 16, se queda 4,5 s,
 * entra bajando desde el borde con un muelle.
 */
#ifndef OPENDOCK_NOTCH_H
#define OPENDOCK_NOTCH_H

#include "config.h"
#include "session.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>

void od_notch_iniciar(OdConfig *cfg, OdBackendTipo backend);

/* Muestra (o reemplaza) el aviso actual. 'icono' (imagen decodificada de la
 * notificación) e 'icono_nombre' (nombre de icono simbólico) pueden ser NULL;
 * si ambos lo son se usa un icono genérico. */
void od_notch_mostrar_aviso(const char *app_name, const char *resumen,
    const char *cuerpo, GdkPixbuf *icono, const char *icono_nombre);

/* Oculta el aviso actual con la animación de cierre (si lo hay). */
void od_notch_ocultar(void);

#endif
