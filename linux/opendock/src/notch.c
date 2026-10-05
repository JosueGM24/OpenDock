/*
 * notch.c — la "isla" bajo la barra superior (ver DESIGN.md).
 *
 * En Wayland es una superficie de capa anclada sólo arriba (sin ancla
 * horizontal: el compositor la centra) que entra y sale deslizando su
 * margen superior con un muelle (persiana). En X11 es una ventana normal,
 * siempre encima, recolocada con Xlib; se muestra/oculta sin animación.
 */
#include "notch.h"
#include "centro.h"
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

#define OD_AVISO_ANCHO   360
#define OD_AVISO_ALTO    54
#define OD_AVISO_HOLD_S  4.5   /* DESIGN.md: "se queda 4,5 s" */
#define OD_AVISO_DEJAR_S 1.2   /* al salir el cursor, se va 1,2 s después */
#define OD_EXPANDIR_MS   260   /* cursor quieto encima: crece hasta enseñar todo el texto */
#define OD_AVISO_LINEAS  7     /* líneas del cuerpo como mucho al expandirlo */
#define OD_DISCRETO_ANCHO 112
#define OD_DISCRETO_ALTO  34
#define OD_DISCRETO_HOLD_S 3.5

typedef struct {
    GtkWidget *ventana;
    GtkWidget *icono;
    GtkWidget *lbl_titulo;
    GtkWidget *lbl_cuerpo;
    GtkWidget *lbl_hora;
    GtkWidget *desliz;         /* recorta título + cuerpo al alto animado */
    GtkWidget *textos;         /* caja vertical con título y cuerpo */
    GtkWidget *pila;           /* "aviso" o "discreto" */
    GtkWidget *lbl_no_leidas;
    guint no_leidas;

    OdMuelle  muelle_margen; /* px de margen superior: 0 = visible, negativo = oculto arriba */
    gboolean  objetivo_visible;
    gint64    ultimo_us;
    guint     temporizador_autocierre;
    int       alto_barra;
    OdBackendTipo backend;
    gboolean  iniciado;

    /* Expansión al pasar el cursor (como las tarjetas del centro). */
    OdMuelle  muelle_alto;     /* alto de los textos: alto_base cerrado, más al expandir */
    double    alto_base;
    gboolean  expandido;
    gboolean  cursor_dentro;
    guint     temporizador_expandir;
    guint     tic_alto;
    gint64    ultimo_alto_us;
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
    Display *xdisplay = od_x11_display(surface);
    Window xid = od_x11_ventana(surface);
    int ancho = g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(g_notch.pila)), "discreto") == 0
        ? OD_DISCRETO_ANCHO : OD_AVISO_ANCHO;
    int x = geo.x + (geo.width - ancho) / 2;
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

/* Margen que deja la isla justo por encima del borde: depende del alto actual. */
static int margen_oculto(void)
{
    int alto = g_notch.ventana ? gtk_widget_get_height(g_notch.ventana) : 0;
    return -(MAX(alto, OD_AVISO_ALTO) + 8);
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

static void programar_cierre(double segundos)
{
    if (g_notch.temporizador_autocierre) g_source_remove(g_notch.temporizador_autocierre);
    g_notch.temporizador_autocierre = g_timeout_add((guint)(segundos * 1000), al_expirar, NULL);
}

/* ── Expansión del aviso ── */
static void cuerpo_en_varias_lineas(gboolean si)
{
    gtk_label_set_wrap(GTK_LABEL(g_notch.lbl_cuerpo), si);
    gtk_label_set_lines(GTK_LABEL(g_notch.lbl_cuerpo), si ? OD_AVISO_LINEAS : 1);
}

static void fijar_alto_textos(double alto)
{
    int px = (int)lround(MAX(alto, 1.0));
    /* el máximo primero: GTK exige min <= max en cada llamada */
    if (px > gtk_scrolled_window_get_max_content_height(GTK_SCROLLED_WINDOW(g_notch.desliz))) {
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(g_notch.desliz), px);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(g_notch.desliz), px);
    } else {
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(g_notch.desliz), px);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(g_notch.desliz), px);
    }
}

/* Con un temporizador y no con el reloj de fotogramas, como el resto de la isla:
 * hay compositores que no mandan fotogramas con regularidad (sway sin cabeza). */
static gboolean fotograma_alto(gpointer datos)
{
    (void)datos;
    gint64 ahora = g_get_monotonic_time();
    double dt = g_notch.ultimo_alto_us ? (ahora - g_notch.ultimo_alto_us) / 1000000.0 : (1.0 / 60.0);
    g_notch.ultimo_alto_us = ahora;
    if (dt > 0.05) dt = 0.05;
    gboolean sigue = od_muelle_actualizar(&g_notch.muelle_alto, dt);
    if (g_notch.muelle_alto.valor < g_notch.alto_base) g_notch.muelle_alto.valor = g_notch.alto_base;
    fijar_alto_textos(g_notch.muelle_alto.valor);
    if (sigue) return G_SOURCE_CONTINUE;
    g_notch.tic_alto = 0;
    g_notch.ultimo_alto_us = 0;
    /* ya cerrado: el cuerpo vuelve a una línea con puntos suspensivos */
    if (!g_notch.expandido) cuerpo_en_varias_lineas(FALSE);
    return G_SOURCE_REMOVE;
}

static void animar_alto(double objetivo)
{
    od_muelle_fijar_objetivo(&g_notch.muelle_alto, objetivo);
    if (!g_notch.tic_alto) g_notch.tic_alto = g_timeout_add(16, fotograma_alto, NULL);
}

/* Cierra la expansión de golpe (aviso nuevo) o con muelle (el cursor se fue). */
static void plegar(gboolean al_momento)
{
    if (g_notch.temporizador_expandir) {
        g_source_remove(g_notch.temporizador_expandir);
        g_notch.temporizador_expandir = 0;
    }
    g_notch.expandido = FALSE;
    if (!al_momento) {
        animar_alto(g_notch.alto_base);
        return;
    }
    if (g_notch.tic_alto) {
        g_source_remove(g_notch.tic_alto);
        g_notch.tic_alto = 0;
    }
    g_notch.ultimo_alto_us = 0;
    cuerpo_en_varias_lineas(FALSE);
    od_muelle_iniciar(&g_notch.muelle_alto, g_notch.alto_base, 340.0, MAX(0.7, OD_ZETA_NORMAL));
    fijar_alto_textos(g_notch.alto_base);
}

static gboolean al_vencer_expandir(gpointer datos)
{
    (void)datos;
    g_notch.temporizador_expandir = 0;
    if (!g_notch.cursor_dentro || !g_notch.objetivo_visible) return G_SOURCE_REMOVE;
    if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(g_notch.pila)), "aviso") != 0)
        return G_SOURCE_REMOVE;
    /* En X11 la isla no cambia de tamaño (se coloca con Xlib y GTK deja de repintar
     * mientras el gestor de ventanas confirma cada cambio): ahí sólo no se oculta. */
    if (g_notch.backend == OD_BACKEND_X11) return G_SOURCE_REMOVE;
    /* alto que necesitan título y cuerpo con el cuerpo partido en líneas (hasta 7) */
    cuerpo_en_varias_lineas(TRUE);
    int ancho = gtk_widget_get_width(g_notch.desliz);
    int min = 0, nat = 0;
    gtk_widget_measure(g_notch.textos, GTK_ORIENTATION_VERTICAL, ancho > 0 ? ancho : -1, &min, &nat, NULL, NULL);
    if (nat <= g_notch.alto_base + 1) {         /* cabía en una línea: nada que enseñar */
        if (!g_notch.tic_alto) cuerpo_en_varias_lineas(FALSE);
        return G_SOURCE_REMOVE;
    }
    g_notch.expandido = TRUE;
    animar_alto(nat);
    return G_SOURCE_REMOVE;
}

static void al_entrar_aviso(GtkEventControllerMotion *c, double x, double y, gpointer datos)
{
    (void)c; (void)x; (void)y; (void)datos;
    g_notch.cursor_dentro = TRUE;
    /* con el cursor encima no se va: se lee con calma */
    if (g_notch.temporizador_autocierre) {
        g_source_remove(g_notch.temporizador_autocierre);
        g_notch.temporizador_autocierre = 0;
    }
    if (g_notch.temporizador_expandir) g_source_remove(g_notch.temporizador_expandir);
    g_notch.temporizador_expandir = g_timeout_add(OD_EXPANDIR_MS, al_vencer_expandir, NULL);
}

static void al_salir_aviso(GtkEventControllerMotion *c, gpointer datos)
{
    (void)c; (void)datos;
    g_notch.cursor_dentro = FALSE;
    plegar(FALSE);
    if (g_notch.objetivo_visible) programar_cierre(OD_AVISO_DEJAR_S);
}

static void mostrar(double segundos)
{
    if (g_notch.temporizador_autocierre) {
        g_source_remove(g_notch.temporizador_autocierre);
        g_notch.temporizador_autocierre = 0;
    }
    plegar(TRUE);                   /* el aviso nuevo nace cerrado */
    /* Con el centro abierto ya se ven las notificaciones: no hace falta aviso. */
    if (od_centro_abierto()) return;

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

    /* con el cursor ya encima espera a que salga (al_salir_aviso programa el cierre) */
    if (!g_notch.cursor_dentro) programar_cierre(segundos);
    else g_notch.temporizador_expandir = g_timeout_add(OD_EXPANDIR_MS, al_vencer_expandir, NULL);
}

static void al_pulsar_notch(GtkGestureClick *g, int n, double x, double y, gpointer datos)
{
    (void)g; (void)n; (void)x; (void)y; (void)datos;
    od_centro_abrir();
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
    gtk_widget_add_css_class(win, "opendock-notch");

    GtkWidget *tarjeta = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(tarjeta, "opendock-aviso");
    gtk_widget_set_size_request(tarjeta, OD_AVISO_ANCHO, OD_AVISO_ALTO);

    GtkWidget *caja_icono = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(caja_icono, 30, 30);
    gtk_widget_set_halign(caja_icono, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(caja_icono, GTK_ALIGN_START);
    gtk_widget_set_margin_top(caja_icono, (OD_AVISO_ALTO - 30) / 2);
    gtk_widget_set_margin_start(caja_icono, 12);
    GtkWidget *icono = gtk_image_new_from_icon_name("dialog-information-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icono), 18);
    gtk_box_append(GTK_BOX(caja_icono), icono);
    gtk_box_append(GTK_BOX(tarjeta), caja_icono);

    GtkWidget *vcaja = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(vcaja, GTK_ALIGN_START);

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

    /* Los textos van en un recorte de alto animado: al expandir, el cuerpo completo
     * se va descubriendo según crece la isla. */
    GtkWidget *desliz = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(desliz), GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(desliz), vcaja);
    gtk_widget_set_valign(desliz, GTK_ALIGN_START);
    gtk_widget_set_hexpand(desliz, TRUE);
    gtk_widget_set_margin_start(desliz, 10);
    gtk_widget_set_margin_top(desliz, 7);
    gtk_widget_set_margin_bottom(desliz, 7);
    gtk_widget_set_can_target(desliz, FALSE);   /* el clic y el cursor son de la isla */
    gtk_box_append(GTK_BOX(tarjeta), desliz);

    GtkWidget *hora = gtk_label_new("");
    gtk_widget_add_css_class(hora, "opendock-aviso-hora");
    gtk_widget_set_margin_end(hora, 16);
    gtk_widget_set_margin_start(hora, 8);
    gtk_widget_set_valign(hora, GTK_ALIGN_START);
    gtk_widget_set_margin_top(hora, 9);
    gtk_box_append(GTK_BOX(tarjeta), hora);

    /* Campana discreta (No molestar): campana de 14 px + contador en
     * píldora de acento. */
    GtkWidget *discreto = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(discreto, "opendock-aviso");
    gtk_widget_set_size_request(discreto, OD_DISCRETO_ANCHO, OD_DISCRETO_ALTO);
    gtk_widget_set_halign(discreto, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(discreto, GTK_ALIGN_START);
    GtkWidget *campana = gtk_image_new_from_icon_name("preferences-system-notifications-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(campana), 14);
    gtk_widget_add_css_class(campana, "opendock-campana");
    gtk_widget_set_margin_start(campana, 23);
    gtk_widget_set_hexpand(campana, TRUE);
    gtk_widget_set_halign(campana, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(discreto), campana);
    GtkWidget *no_leidas = gtk_label_new("1");
    gtk_widget_add_css_class(no_leidas, "opendock-no-leidas");
    gtk_widget_set_valign(no_leidas, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(no_leidas, 14);
    gtk_box_append(GTK_BOX(discreto), no_leidas);

    GtkWidget *pila = gtk_stack_new();
    gtk_stack_set_hhomogeneous(GTK_STACK(pila), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(pila), FALSE);
    gtk_stack_add_named(GTK_STACK(pila), tarjeta, "aviso");
    gtk_stack_add_named(GTK_STACK(pila), discreto, "discreto");
    gtk_window_set_child(GTK_WINDOW(win), pila);

    /* Clic en el aviso o la campana: abre el centro de notificaciones. */
    GtkGesture *clic = gtk_gesture_click_new();
    g_signal_connect(clic, "released", G_CALLBACK(al_pulsar_notch), NULL);
    gtk_widget_add_controller(win, GTK_EVENT_CONTROLLER(clic));

    /* Cursor encima: no se oculta y, tras un instante, enseña el texto completo. */
    GtkEventController *cursor = gtk_event_controller_motion_new();
    g_signal_connect(cursor, "enter", G_CALLBACK(al_entrar_aviso), NULL);
    g_signal_connect(cursor, "leave", G_CALLBACK(al_salir_aviso), NULL);
    gtk_widget_add_controller(win, cursor);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window.opendock-notch { background: transparent; }"
        ".opendock-aviso {"
        "  background-color: rgba(28,28,30,0.97);"
        "  border-radius: 16px;"
        "}"
        ".opendock-aviso-titulo {"
        "  color: #FFFFFF; font-weight: 700; font-size: 14px;"
        "}"
        ".opendock-aviso-cuerpo {"
        "  color: #AEAEB2; font-weight: 600; font-size: 13px;"
        "}"
        ".opendock-aviso-hora {"
        "  color: #6E6E73; font-size: 12px;"
        "}"
        ".opendock-campana, .opendock-aviso image { color: #FFFFFF; }"
        ".opendock-no-leidas { min-width: 24px; min-height: 20px; padding: 0 7px;"
        "  border-radius: 10px; background: #0A84FF; color: #FFFFFF;"
        "  font-size: 12px; font-weight: 600; }");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(win),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    g_notch.ventana = win;
    g_notch.icono = icono;
    g_notch.lbl_titulo = titulo;
    g_notch.lbl_cuerpo = cuerpo;
    g_notch.lbl_hora = hora;
    g_notch.desliz = desliz;
    g_notch.textos = vcaja;
    g_notch.pila = pila;
    g_notch.lbl_no_leidas = no_leidas;

#if HAVE_LAYER_SHELL
    if (backend == OD_BACKEND_WAYLAND && gtk_layer_is_supported()) {
        gtk_layer_init_for_window(GTK_WINDOW(win));
        gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_namespace(GTK_WINDOW(win), "opendock-notch");
        /* -1: se coloca desde el borde de la pantalla, sin apartarse de la
         * zona exclusiva de la barra (el margen ya la salta). */
        gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        /* Sin ancla horizontal: el compositor centra la superficie. */
        gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP,
            -(OD_AVISO_ALTO + 8));
    }
#endif

    od_muelle_iniciar(&g_notch.muelle_margen, -(OD_AVISO_ALTO + 8), 420.0, OD_ZETA_NORMAL + 0.1);
    /* alto de título + cuerpo en una línea: lo que cabe en los 54 px del aviso */
    g_notch.alto_base = OD_AVISO_ALTO - 14;
    od_muelle_iniciar(&g_notch.muelle_alto, g_notch.alto_base, 340.0, MAX(0.7, OD_ZETA_NORMAL));
    fijar_alto_textos(g_notch.alto_base);
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

    gtk_stack_set_visible_child_name(GTK_STACK(g_notch.pila), "aviso");
    mostrar(OD_AVISO_HOLD_S);
}

void od_notch_mostrar_discreto(void)
{
    if (!g_notch.iniciado) return;
    g_notch.no_leidas++;
    char txt[16];
    g_snprintf(txt, sizeof txt, "%u", g_notch.no_leidas);
    gtk_label_set_text(GTK_LABEL(g_notch.lbl_no_leidas), txt);
    gtk_stack_set_visible_child_name(GTK_STACK(g_notch.pila), "discreto");
    mostrar(OD_DISCRETO_HOLD_S);
}

gboolean od_notch_visible(void)
{
    return g_notch.iniciado && g_notch.objetivo_visible;
}

void od_notch_marcar_leidas(void)
{
    g_notch.no_leidas = 0;
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
    od_muelle_fijar_objetivo(&g_notch.muelle_margen, margen_oculto());
    iniciar_animacion();
}
