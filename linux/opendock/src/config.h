/*
 * Configuración de usuario: ~/.config/opendock/config.ini (GKeyFile).
 * Nada de red ni root: todo vive bajo $XDG_CONFIG_HOME.
 */
#ifndef OPENDOCK_CONFIG_H
#define OPENDOCK_CONFIG_H

#include <glib.h>

typedef struct {
    int radio_esquinas;        /* px, por defecto 16 */
    int alto_barra;             /* px, por defecto 28 */
    int alto_dock;               /* px, por defecto 60 */
    gboolean oled;               /* fondo negro puro en vez de vidrio */
    gboolean reemplazar_notificaciones; /* --replace */
    gchar **apps_ancladas;       /* lista de ids .desktop, terminada en NULL */
    gchar *config_path;
} OdConfig;

/* Carga (o crea con valores por defecto) ~/.config/opendock/config.ini */
OdConfig *od_config_cargar(void);
void od_config_liberar(OdConfig *cfg);

#endif
