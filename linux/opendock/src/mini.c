/*
 * mini.c — mini notch y vista rápida (ver mini.h y DESIGN.md).
 *
 * Una superficie transparente de OD_MINI_ZONA×44 pegada bajo la barra. La
 * pastilla se pinta con cairo y no recibe clics (región de entrada vacía,
 * así no tapa nada); la vista rápida sí es clicable y sólo ella ocupa la
 * región de entrada.
 */
#include "mini.h"
#include "bar.h"
#include "centro.h"
#include "notch.h"
#include "opendock-build-config.h"
#include "spring.h"
#include <gtk/gtk.h>
#include <math.h>

#if HAVE_LAYER_SHELL
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#endif
#if HAVE_X11
#include "x11.h"
#endif

#define OD_MINI_ALTO        44
#define OD_PASTILLA_ANCHO   110
#define OD_PASTILLA_ALTO    9
#define OD_RAPIDA_ANCHO     272
#define OD_RAPIDA_ALTO      44
#define OD_IMAN_PX          64
#define OD_QUIETO_MS        450
#define OD_QUIETO_PX        4
#define OD_SALIDA_MS        180

typedef enum { OD_MINI_OCULTO, OD_MINI_PASTILLA, OD_MINI_RAPIDA } OdModoMini;

typedef struct {
    OdConfig *cfg;
    OdBackendTipo backend;
    GtkWidget *ventana;
    GtkWidget *lienzo;
    GtkWidget *rapida;
    GtkWidget *lbl_contador;
    GtkWidget *btn_silencio;
    OdModoMini modo;
    OdMuelle muelle_x;          /* desplazamiento de la pastilla respecto al centro */
    gint64 ultimo_us;
    guint tic;
    double quieto_dx;
    guint temporizador_quieto;
    guint temporizador_salida;
    gboolean cursor_en_rapida;
    gboolean iniciado;
} OdEstadoMini;

static OdEstadoMini g_m;

static void fijar_region_entrada(void)
{
    GdkSurface *s = gtk_native_get_surface(GTK_NATIVE(g_m.ventana));
    if (!s) return;
    cairo_region_t *r;
    if (g_m.modo == OD_MINI_RAPIDA) {
        cairo_rectangle_int_t rect = { (OD_MINI_ZONA - OD_RAPIDA_ANCHO) / 2, 0,
            OD_RAPIDA_ANCHO, OD_RAPIDA_ALTO };
        r = cairo_region_create_rectangle(&rect);
    } else {
        r = cairo_region_create();
    }
    gdk_surface_set_input_region(s, r);
    cairo_region_destroy(r);
}

static void al_realizar(GtkWidget *w, gpointer datos)
{
    (void)w; (void)datos;
    fijar_region_entrada();
}

/* Pastilla con las esquinas de abajo redondeadas, del color de la barra:
 * parece que la barra "gotea" donde está el cursor. */
static void dibujar(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer datos)
{
    (void)a; (void)h; (void)datos;
    if (g_m.modo != OD_MINI_PASTILLA) return;
    double x = w / 2.0 + g_m.muelle_x.valor - OD_PASTILLA_ANCHO / 2.0;
    double r = OD_PASTILLA_ALTO / 2.0;
    double y0 = 0, y1 = OD_PASTILLA_ALTO, x1 = x + OD_PASTILLA_ANCHO;
    cairo_set_source_rgb(cr, 0x1C / 255.0, 0x1C / 255.0, 0x1E / 255.0);
    cairo_move_to(cr, x, y0);
    cairo_line_to(cr, x1, y0);
    cairo_line_to(cr, x1, y1 - r);
    cairo_arc(cr, x1 - r, y1 - r, r, 0, G_PI / 2);
    cairo_line_to(cr, x + r, y1);
    cairo_arc(cr, x + r, y1 - r, r, G_PI / 2, G_PI);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static gboolean fotograma(GtkWidget *w, GdkFrameClock *reloj, gpointer datos)
{
    (void)w; (void)datos;
    gint64 ahora = gdk_frame_clock_get_frame_time(reloj);
    double dt = g_m.ultimo_us ? (ahora - g_m.ultimo_us) / 1e6 : 1.0 / 60.0;
    g_m.ultimo_us = ahora;
    if (dt > 0.1) dt = 0.1;
    gboolean sigue = od_muelle_actualizar(&g_m.muelle_x, dt);
    gtk_widget_queue_draw(g_m.lienzo);
    if (!sigue) {
        g_m.tic = 0;
        g_m.ultimo_us = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void animar(void)
{
    if (g_m.tic) return;
    g_m.ultimo_us = 0;
    g_m.tic = gtk_widget_add_tick_callback(g_m.lienzo, fotograma, NULL, NULL);
}

#if HAVE_X11
static void colocar_x11(void)
{
    GdkSurface *s = gtk_native_get_surface(GTK_NATIVE(g_m.ventana));
    if (!GDK_IS_X11_SURFACE(s)) return;
    GdkRectangle geo = {0, 0, 1280, 800};
    GListModel *mons = gdk_display_get_monitors(gdk_display_get_default());
    if (g_list_model_get_n_items(mons) > 0) {
        GdkMonitor *m = g_list_model_get_item(mons, 0);
        gdk_monitor_get_geometry(m, &geo);
        g_object_unref(m);
    }
    XMoveWindow(od_x11_display(s), od_x11_ventana(s),
        geo.x + (geo.width - OD_MINI_ZONA) / 2, geo.y + g_m.cfg->alto_barra);
}
#endif

static void cancelar(guint *id)
{
    if (*id) g_source_remove(*id);
    *id = 0;
}

static void entrar_modo(OdModoMini modo)
{
    g_debug("mini: modo %d -> %d", g_m.modo, modo);
    g_m.modo = modo;
    gtk_widget_set_visible(g_m.rapida, modo == OD_MINI_RAPIDA);
    if (modo == OD_MINI_OCULTO) {
        cancelar(&g_m.temporizador_quieto);
        cancelar(&g_m.temporizador_salida);
        g_m.cursor_en_rapida = FALSE;
        gtk_widget_set_visible(g_m.ventana, FALSE);
        return;
    }
    if (modo == OD_MINI_RAPIDA) od_mini_refrescar();
    if (!gtk_widget_get_visible(g_m.ventana)) {
        gtk_widget_set_visible(g_m.ventana, TRUE);
#if HAVE_X11
        if (g_m.backend == OD_BACKEND_X11) colocar_x11();
#endif
    }
    fijar_region_entrada();
    gtk_widget_queue_draw(g_m.lienzo);
}

static gboolean al_quedar_quieto(gpointer datos)
{
    (void)datos;
    g_m.temporizador_quieto = 0;
    g_debug("mini: quieto 450 ms (modo %d)", g_m.modo);
    if (g_m.modo == OD_MINI_PASTILLA) entrar_modo(OD_MINI_RAPIDA);
    return G_SOURCE_REMOVE;
}

static gboolean al_vencer_salida(gpointer datos)
{
    (void)datos;
    g_m.temporizador_salida = 0;
    if (!g_m.cursor_en_rapida) entrar_modo(OD_MINI_OCULTO);
    return G_SOURCE_REMOVE;
}

void od_mini_cursor(gboolean dentro, double dx)
{
    if (!g_m.iniciado) return;
    g_debug("mini: cursor dentro=%d dx=%.0f modo=%d", dentro, dx, g_m.modo);
    /* Con el aviso o el centro a la vista, el mini notch sobra. */
    if (od_notch_visible() || od_centro_abierto()) {
        if (g_m.modo != OD_MINI_OCULTO) entrar_modo(OD_MINI_OCULTO);
        return;
    }
    if (!dentro) {
        if (g_m.modo != OD_MINI_OCULTO && !g_m.temporizador_salida)
            g_m.temporizador_salida = g_timeout_add(OD_SALIDA_MS, al_vencer_salida, NULL);
        return;
    }
    cancelar(&g_m.temporizador_salida);

    double rango = OD_MINI_ZONA / 2.0 - OD_PASTILLA_ANCHO / 2.0;
    double objetivo = fabs(dx) < OD_IMAN_PX ? 0 : CLAMP(dx, -rango, rango);
    if (g_m.modo == OD_MINI_OCULTO) {
        /* Aparece ya donde está el cursor, sin venir deslizándose. */
        od_muelle_iniciar(&g_m.muelle_x, objetivo, 240.0, OD_ZETA_NORMAL);
        entrar_modo(OD_MINI_PASTILLA);
        g_m.quieto_dx = dx;
        g_m.temporizador_quieto = g_timeout_add(OD_QUIETO_MS, al_quedar_quieto, NULL);
        return;
    }
    if (g_m.modo == OD_MINI_PASTILLA) {
        od_muelle_fijar_objetivo(&g_m.muelle_x, objetivo);
        animar();
        if (fabs(dx - g_m.quieto_dx) > OD_QUIETO_PX) {
            g_m.quieto_dx = dx;
            cancelar(&g_m.temporizador_quieto);
            g_m.temporizador_quieto = g_timeout_add(OD_QUIETO_MS, al_quedar_quieto, NULL);
        }
    }
}

void od_mini_refrescar(void)
{
    if (!g_m.iniciado) return;
    guint n = od_centro_cantidad();
    char txt[16];
    g_snprintf(txt, sizeof txt, "%u", n);
    gtk_label_set_text(GTK_LABEL(g_m.lbl_contador), txt);
    if (n) gtk_widget_add_css_class(g_m.lbl_contador, "od-contador-activo");
    else gtk_widget_remove_css_class(g_m.lbl_contador, "od-contador-activo");
    gtk_button_set_icon_name(GTK_BUTTON(g_m.btn_silencio), g_m.cfg->no_molestar
        ? "notifications-disabled-symbolic" : "preferences-system-notifications-symbolic");
}

static void al_abrir_centro(GtkGestureClick *g, int n, double x, double y, gpointer datos)
{
    (void)g; (void)n; (void)x; (void)y; (void)datos;
    entrar_modo(OD_MINI_OCULTO);
    od_centro_abrir();
}

static void al_pulsar_silencio(GtkButton *b, gpointer datos)
{
    (void)b; (void)datos;
    od_bar_fijar_no_molestar(!g_m.cfg->no_molestar);
}

static void al_entrar_rapida(GtkEventControllerMotion *c, double x, double y, gpointer datos)
{
    (void)c; (void)x; (void)y; (void)datos;
    g_m.cursor_en_rapida = TRUE;
    cancelar(&g_m.temporizador_salida);
}

static void al_salir_rapida(GtkEventControllerMotion *c, gpointer datos)
{
    (void)c; (void)datos;
    g_m.cursor_en_rapida = FALSE;
    if (g_m.modo == OD_MINI_RAPIDA && !g_m.temporizador_salida)
        g_m.temporizador_salida = g_timeout_add(OD_SALIDA_MS, al_vencer_salida, NULL);
}

void od_mini_iniciar(OdConfig *cfg, OdBackendTipo backend)
{
    if (g_m.iniciado) return;
    g_m.iniciado = TRUE;
    g_m.cfg = cfg;
    g_m.backend = backend;

    GtkWidget *win = gtk_window_new();
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_title(GTK_WINDOW(win), "OpenDock-Mini");
    gtk_widget_set_size_request(win, OD_MINI_ZONA, OD_MINI_ALTO);
    gtk_widget_add_css_class(win, "od-mini-ventana");

    GtkWidget *capas = gtk_overlay_new();
    GtkWidget *lienzo = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(lienzo), OD_MINI_ZONA);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(lienzo), OD_MINI_ALTO);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(lienzo), dibujar, NULL, NULL);
    gtk_widget_set_can_target(lienzo, FALSE);
    gtk_overlay_set_child(GTK_OVERLAY(capas), lienzo);

    /* Vista rápida: "Notificaciones" · silenciar · contador. */
    GtkWidget *rapida = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(rapida, "od-rapida");
    gtk_widget_set_size_request(rapida, OD_RAPIDA_ANCHO, OD_RAPIDA_ALTO);
    gtk_widget_set_halign(rapida, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(rapida, GTK_ALIGN_START);
    GtkWidget *titulo = gtk_label_new("Notificaciones");
    gtk_widget_add_css_class(titulo, "od-rapida-titulo");
    gtk_label_set_xalign(GTK_LABEL(titulo), 0);
    gtk_widget_set_hexpand(titulo, TRUE);
    gtk_widget_set_margin_start(titulo, 18);
    gtk_box_append(GTK_BOX(rapida), titulo);
    GtkWidget *silencio = gtk_button_new_from_icon_name("preferences-system-notifications-symbolic");
    gtk_widget_add_css_class(silencio, "od-rapida-silencio");
    gtk_widget_set_valign(silencio, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(silencio, "No molestar");
    g_signal_connect(silencio, "clicked", G_CALLBACK(al_pulsar_silencio), NULL);
    gtk_box_append(GTK_BOX(rapida), silencio);
    GtkWidget *contador = gtk_label_new("0");
    gtk_widget_add_css_class(contador, "od-contador");
    gtk_widget_set_valign(contador, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(contador, 14);
    gtk_box_append(GTK_BOX(rapida), contador);
    gtk_widget_set_visible(rapida, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(capas), rapida);

    /* Clic en el título o en el contador: abre el centro. */
    GtkGesture *clic = gtk_gesture_click_new();
    g_signal_connect(clic, "released", G_CALLBACK(al_abrir_centro), NULL);
    gtk_widget_add_controller(titulo, GTK_EVENT_CONTROLLER(clic));
    clic = gtk_gesture_click_new();
    g_signal_connect(clic, "released", G_CALLBACK(al_abrir_centro), NULL);
    gtk_widget_add_controller(contador, GTK_EVENT_CONTROLLER(clic));

    GtkEventController *mov = gtk_event_controller_motion_new();
    g_signal_connect(mov, "enter", G_CALLBACK(al_entrar_rapida), NULL);
    g_signal_connect(mov, "leave", G_CALLBACK(al_salir_rapida), NULL);
    gtk_widget_add_controller(rapida, mov);

    gtk_window_set_child(GTK_WINDOW(win), capas);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window.od-mini-ventana { background: transparent; }"
        ".od-rapida { background-color: #1C1C1E; border-radius: 0 0 16px 16px; }"
        ".od-rapida-titulo { color: #FFFFFF; font-size: 13px; font-weight: 600; }"
        ".od-rapida-silencio { min-width: 28px; min-height: 28px; padding: 0; border-radius: 14px;"
        "  border: none; box-shadow: none; background: rgba(255,255,255,0.08); color: #FFFFFF; }"
        ".od-contador { min-width: 24px; min-height: 22px; padding: 0 7px; border-radius: 11px;"
        "  background: #2C2C2E; color: #AEAEB2; font-size: 12px; font-weight: 600; }"
        ".od-contador.od-contador-activo { background: #0A84FF; color: #FFFFFF; }");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(win),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    g_signal_connect(win, "realize", G_CALLBACK(al_realizar), NULL);

    g_m.ventana = win;
    g_m.lienzo = lienzo;
    g_m.rapida = rapida;
    g_m.lbl_contador = contador;
    g_m.btn_silencio = silencio;

#if HAVE_LAYER_SHELL
    if (backend == OD_BACKEND_WAYLAND && gtk_layer_is_supported()) {
        gtk_layer_init_for_window(GTK_WINDOW(win));
        gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_namespace(GTK_WINDOW(win), "opendock-mini");
        /* -1: se coloca desde el borde de la pantalla, sin apartarse de la
         * zona exclusiva de la barra (el margen ya la salta). */
        gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, cfg->alto_barra);
    }
#endif

    od_muelle_iniciar(&g_m.muelle_x, 0, 240.0, OD_ZETA_NORMAL);
    g_m.modo = OD_MINI_OCULTO;
    gtk_widget_set_visible(win, FALSE);
}
