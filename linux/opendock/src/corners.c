#include "corners.h"
#include "opendock-build-config.h"
#include <gtk/gtk.h>
#include <math.h>

#if HAVE_LAYER_SHELL
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#endif

#if HAVE_X11
#include "x11.h"
#include <X11/Xatom.h>
#endif

typedef enum {
    OD_ESQUINA_SUP_IZQ,
    OD_ESQUINA_SUP_DER,
    OD_ESQUINA_INF_IZQ,
    OD_ESQUINA_INF_DER,
    OD_ESQUINA_N
} OdEsquina;

/* Dibuja la máscara: negro fuera del cuarto de círculo, transparente dentro.
 * El centro de curvatura se sitúa siempre a 'r' px de la esquina física de
 * la pantalla, hacia el interior. Las de arriba empiezan bajo la barra y
 * llevan su color (DESIGN.md), así parecen la continuación de la barra. */
static void dibujar_esquina(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer datos)
{
    OdEsquina esquina = GPOINTER_TO_INT(datos);
    double r = (double)(w < h ? w : h);

    double cx, cy;      /* centro del cuarto de círculo */
    double ang_ini, ang_fin;

    switch (esquina) {
        case OD_ESQUINA_SUP_IZQ:
            cx = r; cy = r; ang_ini = G_PI; ang_fin = 1.5 * G_PI; break;
        case OD_ESQUINA_SUP_DER:
            cx = 0; cy = r; ang_ini = 1.5 * G_PI; ang_fin = 2.0 * G_PI; break;
        case OD_ESQUINA_INF_IZQ:
            cx = r; cy = 0; ang_ini = 0.5 * G_PI; ang_fin = G_PI; break;
        case OD_ESQUINA_INF_DER:
        default:
            cx = 0; cy = 0; ang_ini = 0.0; ang_fin = 0.5 * G_PI; break;
    }

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    /* Rectángulo completo (negro, o el color de la barra arriba)... */
    if (esquina == OD_ESQUINA_SUP_IZQ || esquina == OD_ESQUINA_SUP_DER)
        cairo_set_source_rgba(cr, 0x1C / 255.0, 0x1C / 255.0, 0x1E / 255.0, 1);
    else
        cairo_set_source_rgba(cr, 0, 0, 0, 1);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);

    /* ...al que le recortamos el cuarto de círculo (transparente = se ve
     * el escritorio / el contenido detrás). */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_move_to(cr, cx, cy);
    cairo_arc(cr, cx, cy, r, ang_ini, ang_fin);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_restore(cr);
}

static GtkWidget *crear_contenido(OdEsquina esquina, int radio)
{
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, radio, radio);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area), radio);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), radio);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), dibujar_esquina,
        GINT_TO_POINTER(esquina), NULL);
    return area;
}

static void al_realizar_region_vacia(GtkWidget *widget, gpointer datos)
{
    (void)datos;
    GdkSurface *surface = gtk_native_get_surface(gtk_widget_get_native(widget));
    if (!surface) return;
    cairo_region_t *vacia = cairo_region_create();
    gdk_surface_set_input_region(surface, vacia);
    cairo_region_destroy(vacia);
}

/* Sin uso si no hay ni layer-shell ni X11 (se compila igual, sin esquinas). */
G_GNUC_UNUSED static GtkWidget *crear_ventana_esquina(GtkApplication *app, OdEsquina esquina, int radio)
{
    GtkWidget *win = gtk_window_new();
    if (app) gtk_window_set_application(GTK_WINDOW(win), app);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_widget_set_can_target(win, FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), radio, radio);

    GtkWidget *area = crear_contenido(esquina, radio);
    gtk_window_set_child(GTK_WINDOW(win), area);

    /* CSS: sin fondo propio del tema (si no, taparía la transparencia). */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, "window { background: transparent; }");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(win),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    g_signal_connect(win, "realize", G_CALLBACK(al_realizar_region_vacia), NULL);

    return win;
}

#if HAVE_LAYER_SHELL
static void colocar_wayland(GtkWidget *win, GdkMonitor *monitor, OdEsquina esquina, int alto_barra)
{
    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_monitor(GTK_WINDOW(win), monitor);
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(win), "opendock-corner");
    gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    gboolean arriba = (esquina == OD_ESQUINA_SUP_IZQ || esquina == OD_ESQUINA_SUP_DER);
    gboolean izquierda = (esquina == OD_ESQUINA_SUP_IZQ || esquina == OD_ESQUINA_INF_IZQ);

    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, arriba);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, !arriba);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, izquierda);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, !izquierda);
    if (arriba) gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, alto_barra);
}

static void iniciar_wayland(GtkApplication *app, OdConfig *cfg)
{
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return;
    GListModel *monitores = gdk_display_get_monitors(display);
    guint n = g_list_model_get_n_items(monitores);
    for (guint i = 0; i < n; i++) {
        GdkMonitor *mon = g_list_model_get_item(monitores, i);
        for (OdEsquina e = 0; e < OD_ESQUINA_N; e++) {
            GtkWidget *win = crear_ventana_esquina(app, e, cfg->radio_esquinas);
            colocar_wayland(win, mon, e, cfg->alto_barra);
            gtk_window_present(GTK_WINDOW(win));
        }
        g_object_unref(mon);
    }
}
#endif

#if HAVE_X11
/* En X11 no hay zona de capa: creamos ventanas normales, sin decorar, que
 * marcamos _NET_WM_WINDOW_TYPE_DOCK + override-redirect a bajo nivel y
 * movemos con Xlib a la esquina física del monitor (GTK4 no deja mover
 * ventanas top-level directamente, por eso el acceso a Xlib). */
static void colocar_x11(GtkWidget *win, GdkMonitor *monitor, OdEsquina esquina, int radio,
    int alto_barra)
{
    GdkRectangle geo;
    gdk_monitor_get_geometry(monitor, &geo);

    int x = (esquina == OD_ESQUINA_SUP_IZQ || esquina == OD_ESQUINA_INF_IZQ)
        ? geo.x : geo.x + geo.width - radio;
    int y = (esquina == OD_ESQUINA_SUP_IZQ || esquina == OD_ESQUINA_SUP_DER)
        ? geo.y + alto_barra : geo.y + geo.height - radio;

    GdkSurface *surface = gtk_native_get_surface(gtk_widget_get_native(win));
    if (!GDK_IS_X11_SURFACE(surface)) return;

    Display *xdisplay = od_x11_display(surface);
    Window xid = od_x11_ventana(surface);

    XSetWindowAttributes attrs = { 0 };
    attrs.override_redirect = True;
    XChangeWindowAttributes(xdisplay, xid, CWOverrideRedirect, &attrs);

    Atom tipo = XInternAtom(xdisplay, "_NET_WM_WINDOW_TYPE", False);
    Atom dock = XInternAtom(xdisplay, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(xdisplay, xid, tipo, XA_ATOM, 32, PropModeReplace,
        (unsigned char *)&dock, 1);

    XMoveWindow(xdisplay, xid, x, y);
}

static void iniciar_x11(GtkApplication *app, OdConfig *cfg)
{
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return;
    GListModel *monitores = gdk_display_get_monitors(display);
    guint n = g_list_model_get_n_items(monitores);
    for (guint i = 0; i < n; i++) {
        GdkMonitor *mon = g_list_model_get_item(monitores, i);
        for (OdEsquina e = 0; e < OD_ESQUINA_N; e++) {
            GtkWidget *win = crear_ventana_esquina(app, e, cfg->radio_esquinas);
            gtk_window_present(GTK_WINDOW(win));
            colocar_x11(win, mon, e, cfg->radio_esquinas, cfg->alto_barra);
        }
        g_object_unref(mon);
    }
}
#endif

void od_esquinas_iniciar(OdConfig *cfg, OdBackendTipo backend)
{
    if (cfg->radio_esquinas <= 0) return;

#if HAVE_LAYER_SHELL
    if (backend == OD_BACKEND_WAYLAND && gtk_layer_is_supported()) {
        iniciar_wayland(NULL, cfg); /* no necesitan GtkApplication */
        return;
    }
#endif
#if HAVE_X11
    if (backend == OD_BACKEND_X11) {
        iniciar_x11(NULL, cfg);
        return;
    }
#endif
    g_message("opendock: esquinas redondeadas no disponibles en este backend (%s)",
        od_sesion_backend_nombre(backend));
}
