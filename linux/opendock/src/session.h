/*
 * Detección de sesión: qué compositor/gestor hay debajo y qué backend usar.
 * GNOME no admite layer-shell: ahí OpenDock no dibuja nada y remite a la
 * extensión de GNOME Shell (componente aparte).
 */
#ifndef OPENDOCK_SESSION_H
#define OPENDOCK_SESSION_H

#include <glib.h>

typedef enum {
    OD_BACKEND_DESCONOCIDO,
    OD_BACKEND_WAYLAND,
    OD_BACKEND_X11,
} OdBackendTipo;

/* TRUE si la sesión actual es GNOME Shell (Mutter), donde OpenDock debe
 * ceder el paso a la extensión gnome-shell-extension-opendock. */
gboolean od_sesion_es_gnome(void);

/* Backend gráfico disponible según las variables de entorno de la sesión. */
OdBackendTipo od_sesion_backend(void);

const char *od_sesion_backend_nombre(OdBackendTipo b);

#endif
