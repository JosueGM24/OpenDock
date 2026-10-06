/*
 * Ventanas abiertas del escritorio, para el dock y el nombre de la app
 * activa en la barra.
 *   Wayland: zwlr_foreign_toplevel_manager_v1 (sway, Hyprland, labwc,
 *            river, Wayfire...). KWin no lo ofrece: ahí el dock sólo
 *            muestra las apps ancladas.
 *   X11:     _NET_CLIENT_LIST, _NET_ACTIVE_WINDOW y WM_CLASS (EWMH).
 */
#ifndef OPENDOCK_VENTANAS_H
#define OPENDOCK_VENTANAS_H

#include "session.h"
#include <gtk/gtk.h>

typedef struct OdVentana OdVentana;

/* Se llama (en el hilo principal) cuando cambia la lista o alguna ventana. */
typedef void (*OdVentanasCambioFn)(gpointer datos);

void od_ventanas_iniciar(OdBackendTipo backend, OdVentanasCambioFn cb, gpointer datos);

/* TRUE si hay una fuente de ventanas (protocolo o EWMH) funcionando. */
gboolean od_ventanas_disponible(void);

/* Lista actual (OdVentana*); no hay que liberarla ni guardarla. */
GPtrArray *od_ventanas_lista(void);

/* Id de la app (app_id de Wayland o WM_CLASS en X11), puede ser "". */
const char *od_ventana_app_id(const OdVentana *v);
const char *od_ventana_titulo(const OdVentana *v);
gboolean od_ventana_activa(const OdVentana *v);
gboolean od_ventana_minimizada(const OdVentana *v);
/* Proceso dueño de la ventana (_NET_WM_PID en X11); 0 si no se sabe (Wayland). */
int od_ventana_pid(const OdVentana *v);

void od_ventana_activar(OdVentana *v);
void od_ventana_minimizar(OdVentana *v);
void od_ventana_cerrar(OdVentana *v);

/* ¿La ventana de arriba del todo en ese monitor está a pantalla completa? (X11) */
gboolean od_ventanas_pantalla_completa_en(GdkMonitor *monitor);

/* Miniatura de la ventana como mucho de max_ancho × max_alto (X11 con
 * XComposite); NULL si no hay (Wayland, minimizada...). Hay que liberarla. */
GdkTexture *od_ventana_miniatura(OdVentana *v, int max_ancho, int max_alto);

/* Deja de redirigir las ventanas usadas para las miniaturas. */
void od_ventanas_soltar_miniaturas(void);

#endif
