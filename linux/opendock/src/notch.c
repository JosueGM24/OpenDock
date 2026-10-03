/*
 * notch.c — la "isla" bajo la barra superior (ver DESIGN.md).
 *
 * En Wayland es una superficie de capa anclada sólo arriba (sin ancla
 * horizontal: el compositor la centra) que entra y sale deslizando su
 * margen superior con un muelle (persiana). En X11 es una ventana normal,
 * siempre encima, recolocada con Xlib; se muestra/oculta sin animación.
 */
#include "notch.h"
#include "opendock-build-config.h"
#include "spring.h"
#include <gtk/gtk.h>
#include <math.h>

#if HAVE_LAYER_SHELL
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#endif
#if HAVE_X11
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#endif

#define OD_AVISO_ANCHO   360
#define OD_AVISO_ALTO    54
#define OD_AVISO_HOLD_S  4.5   /* DESIGN.md: "se queda 4,5 s" */

typedef struct {
    GtkWidget *ventana;
    GtkWidget *icono;
    GtkWidget *lbl_titulo;
    GtkWidget *lbl_cuerpo;
    GtkWidget *lbl_hora;

    OdMuelle  muelle_margen; /* px de margen superior: 0 = visible, negativo = oculto arriba */
    gboolean  objetivo_visible;
    gint64    ultimo_us;
    guint     temporizador_autocierre;
    int       alto_barra;
    OdBackendTipo backend;
    gboolean  iniciado;
} OdEstadoNotch;

static OdEstadoNotch g_notch;

static char *hora_actual(void)
{
    GDateTime *ahora = g_date_time_new_now_local();
    /* Formato corto tipo "14:05" (la cabecera completa va en la barra). */
    char *s = g_date_time_format(ahora, "%H:%M");
    g_date_time_unref(ahora);
    return s;
}

#if HAVE_X11
static void colocar_x11(void)
{
    GdkSurface *surface = gtk_native_get_surface(gtk_widget_get_native(g_notch.ventana));
    if (!GDK_IS_X11_SURFACE(surface)) return;
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *mon = NULL;
    GListModel *monitores = gdk_display_get_monitors(display);
    if (g_list_model_get_n_items(monitores) > 0)
        mon = g_list_model_get_item(monitores, 0);
    GdkRectangle geo = {0, 0, 1280, 800};
    if (mon) {
        gdk_monitor_get_geometry(mon, &geo);
        g_object_unref(mon);
    }
    Display *xdisplay = GDK_SURFACE_XDISPLAY(surface);
    Window xid = GDK_SURFACE_XID(surface);
    int x = geo.x + (geo.width - OD_AVISO_ANCHO) / 2;
    int y = geo.y + g_notch.alto_barra;
    XMoveWindow(xdisplay, xid, x, y);
}
#endif

static void aplicar_margen(int margen_px)
{
#if HAVE_LAYER_SHELL
    if (g_notch.backend == OD_BACKEND_WAYLAND) {
        gtk_layer_set_margin(GTK_WINDOW(g_notch.ventana), GTK_LAYER_SHELL_EDGE_TOP, margen_px);
        return;
    }
#endif
    (void)margen_px;
}

static gboolean fotograma_muelle(GtkWidget *widget, GdkFrameClock *clock, gpointer datos)
{
    (void)widget; (void)datos;
    gint64 ahora = gdk_frame_clock_get_frame_time(clock);
    double dt = g_notch.ultimo_us ? (ahora - g_notch.ultimo_us) / 1000000.0 : (1.0 / 60.0);
    g_notch.ultimo_us = ahora;
    if (dt > 0.1) dt = 0.1;

    gboolean en_movimiento = od_muelle_actualizar(&g_notch.muelle_margen, dt);
    aplicar_margen((int)lround(g_notch.muelle_margen.valor));

    if (!en_movimiento) {
        g_notch.ultimo_us = 0;
        if (!g_notch.objetivo_visible) {
            gtk_widget_set_visible(g_notch.ventana, FALSE);
        }
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void iniciar_animacion(void)
{
    g_notch.ultimo_us = 0;
    /* Adelantamos el muelle "a mano" un buen trozo antes de depender del
     * frame clock: en compositores sin vsync real (p. ej. sway con salida
     * "headless" en integración continua) el primer tic puede tardar o no
     * llegar nunca, y no queremos que el notch se quede invisible por eso.
     * Con k=420 esto ya deja el valor pegado al objetivo; los tics reales
     * que lleguen después sólo afinan el último tramo. */
    od_muelle_actualizar(&g_notch.muelle_margen, 0.3);
    aplicar_margen((int)lround(g_notch.muelle_margen.valor));
    gtk_widget_add_tick_callback(g_notch.ventana, fotograma_muelle, NULL, NULL);
}

static gboolean al_expirar(gpointer datos)
{
    (void)datos;
    g_notch.temporizador_autocierre = 0;
    od_notch_ocultar();
    return G_SOURCE_REMOVE;
}

void od_notch_iniciar(OdConfig *cfg, OdBackendTipo backend)
{
    if (g_notch.iniciado) return;
    g_notch.iniciado = TRUE;
    g_notch.backend = backend;
    g_notch.alto_barra = cfg->alto_barra;

    GtkWidget *win = gtk_window_new();
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_widget_set_can_target(win, FALSE);
    gtk_widget_set_size_request(win, OD_AVISO_ANCHO, OD_AVISO_ALTO);
    gtk_window_set_default_size(GTK_WINDOW(win), OD_AVISO_ANCHO, OD_AVISO_ALTO);
    gtk_widget_add_css_class(win, "opendock-notch");

    GtkWidget *tarjeta = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(tarjeta, "opendock-aviso");
    gtk_widget_set_size_request(tarjeta, OD_AVISO_ANCHO, OD_AVISO_ALTO);

    GtkWidget *caja_icono = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(caja_icono, 30, 30);
    gtk_widget_set_halign(caja_icono, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(caja_icono, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(caja_icono, 12);
    GtkWidget *icono = gtk_image_new_from_icon_name("dialog-information-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icono), 18);
    gtk_box_append(GTK_BOX(caja_icono), icono);
    gtk_box_append(GTK_BOX(tarjeta), caja_icono);

    GtkWidget *vcaja = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(vcaja, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(vcaja, TRUE);
    gtk_widget_set_margin_start(vcaja, 10);

    GtkWidget *titulo = gtk_label_new("");
    gtk_widget_add_css_class(titulo, "opendock-aviso-titulo");
    gtk_label_set_xalign(GTK_LABEL(titulo), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(titulo), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(vcaja), titulo);

    GtkWidget *cuerpo = gtk_label_new("");
    gtk_widget_add_css_class(cuerpo, "opendock-aviso-cuerpo");
    gtk_label_set_xalign(GTK_LABEL(cuerpo), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(cuerpo), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(vcaja), cuerpo);

    gtk_box_append(GTK_BOX(tarjeta), vcaja);

    GtkWidget *hora = gtk_label_new("");
    gtk_widget_add_css_class(hora, "opendock-aviso-hora");
    gtk_widget_set_margin_end(hora, 16);
    gtk_widget_set_valign(hora, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(tarjeta), hora);

    gtk_window_set_child(GTK_WINDOW(win), tarjeta);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window.opendock-notch { background: transparent; }"
        ".opendock-aviso {"
        "  background-color: rgba(28,28,30,0.97);"
        "  border-radius: 16px;"
        "}"
        ".opendock-aviso-titulo {"
        "  color: #FFFFFF; font-weight: 600; font-size: 14px;"
        "}"
        ".opendock-aviso-cuerpo {"
        "  color: #AEAEB2; font-size: 12px;"
        "}"
        ".opendock-aviso-hora {"
        "  color: #6E6E73; font-size: 12px;"
        "}");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(win),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    g_notch.ventana = win;
    g_notch.icono = icono;
    g_notch.lbl_titulo = titulo;
    g_notch.lbl_cuerpo = cuerpo;
    g_notch.lbl_hora = hora;

#if HAVE_LAYER_SHELL
    if (backend == OD_BACKEND_WAYLAND && gtk_layer_is_supported()) {
        gtk_layer_init_for_window(GTK_WINDOW(win));
        gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_namespace(GTK_WINDOW(win), "opendock-notch");
        gtk_layer_set_exclusive_zone(GTK_WINDOW(win), 0);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        /* Sin ancla horizontal: el compositor centra la superficie. */
        gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP,
            -(OD_AVISO_ALTO + 8));
    }
#endif

    od_muelle_iniciar(&g_notch.muelle_margen, -(OD_AVISO_ALTO + 8), 420.0, OD_ZETA_NORMAL + 0.1);
    g_notch.objetivo_visible = FALSE;
    gtk_widget_set_visible(win, FALSE);
}

void od_notch_mostrar_aviso(const char *app_name, const char *resumen,
    const char *cuerpo, GdkPixbuf *icono, const char *icono_nombre)
{
    if (!g_notch.iniciado) return;

    gtk_label_set_text(GTK_LABEL(g_notch.lbl_titulo), resumen ? resumen : (app_name ? app_name : ""));
    gtk_label_set_text(GTK_LABEL(g_notch.lbl_cuerpo), cuerpo ? cuerpo : "");
    char *hora = hora_actual();
    gtk_label_set_text(GTK_LABEL(g_notch.lbl_hora), hora);
    g_free(hora);

    if (icono) {
        /* gdk_texture_new_for_pixbuf está marcada obsoleta en GTK recientes
         * (recomiendan cargar texturas directamente), pero sigue siendo la
         * forma correcta de convertir un GdkPixbuf ya decodificado a mano
         * (desde image-data de la notificación) en algo que pintar. */
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        GdkTexture *textura = gdk_texture_new_for_pixbuf(icono);
        G_GNUC_END_IGNORE_DEPRECATIONS
        gtk_image_set_from_paintable(GTK_IMAGE(g_notch.icono), GDK_PAINTABLE(textura));
        g_object_unref(textura);
    } else if (icono_nombre && *icono_nombre) {
        gtk_image_set_from_icon_name(GTK_IMAGE(g_notch.icono), icono_nombre);
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(g_notch.icono), "dialog-information-symbolic");
    }

    if (g_notch.temporizador_autocierre) {
        g_source_remove(g_notch.temporizador_autocierre);
        g_notch.temporizador_autocierre = 0;
    }

    gtk_widget_set_visible(g_notch.ventana, TRUE);
    g_notch.objetivo_visible = TRUE;

#if HAVE_X11
    if (g_notch.backend == OD_BACKEND_X11) {
        colocar_x11();
    } else
#endif
    {
        od_muelle_fijar_objetivo(&g_notch.muelle_margen, g_notch.alto_barra);
        iniciar_animacion();
    }

    g_message("opendock: notch mostrado, visible=%d margen=%.1f",
        gtk_widget_get_visible(g_notch.ventana), g_notch.muelle_margen.valor);

    g_notch.temporizador_autocierre = g_timeout_add(
        (guint)(OD_AVISO_HOLD_S * 1000), al_expirar, NULL);
}

void od_notch_ocultar(void)
{
    if (!g_notch.iniciado) return;
    if (g_notch.temporizador_autocierre) {
        g_source_remove(g_notch.temporizador_autocierre);
        g_notch.temporizador_autocierre = 0;
    }
    g_notch.objetivo_visible = FALSE;

#if HAVE_X11
    if (g_notch.backend == OD_BACKEND_X11) {
        gtk_widget_set_visible(g_notch.ventana, FALSE);
        return;
    }
#endif
    od_muelle_fijar_objetivo(&g_notch.muelle_margen, -(OD_AVISO_ALTO + 8));
    iniciar_animacion();
}
