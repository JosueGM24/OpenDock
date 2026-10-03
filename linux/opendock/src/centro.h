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
} OdNotif;

void od_notif_liberar(OdNotif *n);

typedef struct {
    /* El usuario pulsó una acción ("default" = la tarjeta entera). */
    void (*accion)(guint32 id, const char *clave);
    /* El usuario la borró (papelera o "Borrar todo"). */
    void (*descartada)(guint32 id);
} OdCentroRetrollamadas;

void od_centro_iniciar(OdConfig *cfg, OdBackendTipo backend, const OdCentroRetrollamadas *rr);

/* Añade (o reemplaza, si ya hay una con el mismo id). Se queda con 'n'. */
void od_centro_agregar(OdNotif *n);
/* La quita sin avisar a nadie (CloseNotification del propio cliente). */
void od_centro_quitar(guint32 id);
guint od_centro_cantidad(void);

void od_centro_abrir(void);
void od_centro_cerrar(void);
void od_centro_alternar(void);
gboolean od_centro_abierto(void);

/* El estado de No molestar cambió desde otro sitio (barra, vista rápida). */
void od_centro_refrescar_no_molestar(void);

#endif
