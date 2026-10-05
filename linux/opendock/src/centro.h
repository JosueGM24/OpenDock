/*
 * Centro de notificaciones: baja bajo la barra con las notificaciones que
 * siguen vivas (ver DESIGN.md, "Centro"). Tarjetas de 348×70, lupa al
 * pasar el cursor, se expanden a los 260 ms con el cuerpo y las acciones,
 * y papelera roja al acercarse al borde derecho.
 */
#ifndef OPENDOCK_CENTRO_H
#define OPENDOCK_CENTRO_H

#include "config.h"
#include "session.h"
#include <gtk/gtk.h>

typedef struct {
    guint32 id;
    gchar *app;
    gchar *titulo;
    gchar *cuerpo;
    gchar *icono_nombre;   /* nombre de icono, o NULL */
    GdkTexture *icono;     /* imagen de la notificación, o NULL */
    gchar **acciones;      /* pares clave, etiqueta; terminado en NULL */
    gint64 hora_us;        /* g_get_real_time() al llegar */
    guint32 cadena;        /* historial: id del aviso vivo que la reemplazó (0 = viva) */
} OdNotif;

void od_notif_liberar(OdNotif *n);

typedef struct {
    /* El usuario pulsó una acción ("default" = la tarjeta entera). */
    void (*accion)(guint32 id, const char *clave);
    /* El usuario la borró (papelera o "Borrar todo"). */
    void (*descartada)(guint32 id);
} OdCentroRetrollamadas;

void od_centro_iniciar(OdConfig *cfg, OdBackendTipo backend, const OdCentroRetrollamadas *rr);

/* Añade (o reemplaza, si ya hay una con el mismo id). Se queda con 'n'.
 * Con 'historial' (navegadores: WhatsApp Web, Telegram... usan el mismo id para todo
 * un chat), la que reemplaza se queda en el centro como historial si su texto era otro.
 * Cada aviso (app + título + texto) sale una sola vez: el más nuevo. */
void od_centro_agregar(OdNotif *n, gboolean historial);
/* La quita sin avisar a nadie (CloseNotification del propio cliente), con su historial. */
void od_centro_quitar(guint32 id);
/* Ya hay en el centro un aviso con la misma app, título y texto (publicado otra vez). */
gboolean od_centro_ya_visto(const char *app, const char *titulo, const char *cuerpo);
guint od_centro_cantidad(void);

void od_centro_abrir(void);
void od_centro_cerrar(void);
void od_centro_alternar(void);
gboolean od_centro_abierto(void);

/* El estado de No molestar cambió desde otro sitio (barra, vista rápida). */
void od_centro_refrescar_no_molestar(void);

#endif
