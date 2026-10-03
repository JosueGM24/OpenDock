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
    gboolean no_molestar;        /* sin avisos en el notch (salvo críticos) */
    gboolean reemplazar_notificaciones; /* --replace */
    gchar **apps_ancladas;       /* lista de ids .desktop, terminada en NULL */
    int ocultar_dock;            /* OD_OCULTAR_NUNCA / _MITAD / _COMPLETO */
    gchar *config_path;
} OdConfig;

#define OD_OCULTAR_NUNCA    0   /* siempre visible, reserva media altura */
#define OD_OCULTAR_MITAD    1   /* al salir el cursor baja hasta dejar media */
#define OD_OCULTAR_COMPLETO 2   /* al salir el cursor desaparece del todo */

/* Carga (o crea con valores por defecto) ~/.config/opendock/config.ini */
OdConfig *od_config_cargar(void);
void od_config_liberar(OdConfig *cfg);

/* Abre config.ini con la app predeterminada (sin shell intermedio). */
void od_config_abrir(OdConfig *cfg);

/* Cambia un valor booleano y lo guarda en config.ini sin tocar el resto. */
void od_config_guardar_bool(OdConfig *cfg, const char *grupo, const char *clave,
    gboolean valor);

/* Guarda cfg->apps_ancladas en [dock] apps. */
void od_config_guardar_apps(OdConfig *cfg);

#endif
