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

/* No molestar: en vez del aviso, una campana de 112×34 con el número de
 * notificaciones sin leer (se suma una cada vez). */
void od_notch_mostrar_discreto(void);

/* Oculta el aviso actual con la animación de cierre (si lo hay). */
void od_notch_ocultar(void);

/* TRUE mientras el aviso (o la campana) está a la vista. */
gboolean od_notch_visible(void);

/* Al abrir el centro: las notificaciones ya se han visto. */
void od_notch_marcar_leidas(void);

#endif
