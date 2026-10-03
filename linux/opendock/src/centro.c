/*
 * centro.c — centro de notificaciones (ver DESIGN.md, "Notch" → "Centro").
 *
 * En Wayland es una superficie de capa anclada sólo arriba (el compositor
 * la centra) que baja como una persiana deslizando su margen con un
 * muelle, igual que el notch. En X11 es una ventana recolocada con Xlib.
 * Se cierra con Esc, al perder el foco (clic fuera) o al volver a pulsar
 * el reloj / el notch.
 */
#include "centro.h"
#include "bar.h"
#include "notch.h"
#include "opendock-build-config.h"
#include "spring.h"
#include <math.h>

#if HAVE_LAYER_SHELL
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#endif
#if HAVE_X11
#include "x11.h"
#endif

#define OD_CENTRO_ANCHO     384
#define OD_TARJETA_ANCHO    348
#define OD_TARJETA_ALTO     70
#define OD_TARJETA_SEP      12
#define OD_TARJETAS_VISIBLES 6
#define OD_EXPANDIR_MS      260
#define OD_BORRAR_MS        260
#define OD_OCULTO_PX        (-720)   /* margen con el que queda fuera de la pantalla */
#define OD_MAX_ACCIONES     3

typedef struct {
    OdConfig *cfg;
    OdBackendTipo backend;
    OdCentroRetrollamadas rr;
    GtkWidget *ventana;
    GtkWidget *lista;
    GtkWidget *vacio;
    GtkWidget *btn_campana;
    GList *notifs;              /* OdNotif*, la más nueva primero */
    gboolean abierto;
    gboolean estuvo_activo;
    OdMuelle muelle;
    gint64 ultimo_us;
    gboolean iniciado;
} OdEstadoCentro;

static OdEstadoCentro g_c;

void od_notif_liberar(OdNotif *n)
{
    if (!n) return;
    g_free(n->app);
    g_free(n->titulo);
    g_free(n->cuerpo);
    g_free(n->icono_nombre);
    if (n->icono) g_object_unref(n->icono);
    g_strfreev(n->acciones);
    g_free(n);
}

static OdNotif *buscar(guint32 id)
{
    for (GList *l = g_c.notifs; l; l = l->next) {
        OdNotif *n = l->data;
        if (n->id == id) return n;
    }
    return NULL;
}

static GtkWidget *tarjeta_de(guint32 id)
{
    for (GtkWidget *t = gtk_widget_get_first_child(g_c.lista); t; t = gtk_widget_get_next_sibling(t)) {
        if (GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(t), "od-id")) == id) return t;
    }
    return NULL;
}

static void actualizar_vacio(void)
{
    gtk_widget_set_visible(g_c.vacio, g_c.notifs == NULL);
}

static void actualizar_campana(void)
{
    gtk_button_set_icon_name(GTK_BUTTON(g_c.btn_campana), g_c.cfg->no_molestar
        ? "notifications-disabled-symbolic" : "preferences-system-notifications-symbolic");
    gtk_widget_set_tooltip_text(g_c.btn_campana, g_c.cfg->no_molestar
        ? "Desactivar No molestar" : "Activar No molestar");
}

/* ---- persiana ------------------------------------------------------------ */

static void aplicar_margen(int margen)
{
#if HAVE_LAYER_SHELL
    if (g_c.backend == OD_BACKEND_WAYLAND && gtk_layer_is_layer_window(GTK_WINDOW(g_c.ventana)))
        gtk_layer_set_margin(GTK_WINDOW(g_c.ventana), GTK_LAYER_SHELL_EDGE_TOP, margen);
#endif
    (void)margen;
}

static gboolean fotograma(GtkWidget *w, GdkFrameClock *reloj, gpointer datos)
{
    (void)w; (void)datos;
    gint64 ahora = gdk_frame_clock_get_frame_time(reloj);
    double dt = g_c.ultimo_us ? (ahora - g_c.ultimo_us) / 1e6 : 1.0 / 60.0;
    g_c.ultimo_us = ahora;
    if (dt > 0.1) dt = 0.1;
    gboolean sigue = od_muelle_actualizar(&g_c.muelle, dt);
    aplicar_margen((int)lround(g_c.muelle.valor));
    if (!sigue) {
        g_c.ultimo_us = 0;
        if (!g_c.abierto) gtk_widget_set_visible(g_c.ventana, FALSE);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void animar(void)
{
    g_c.ultimo_us = 0;
    /* Como en el notch: un primer tramo a mano por si el compositor no
     * manda fotogramas (sway sin cabeza en CI). */
    od_muelle_actualizar(&g_c.muelle, 0.3);
    aplicar_margen((int)lround(g_c.muelle.valor));
    gtk_widget_add_tick_callback(g_c.ventana, fotograma, NULL, NULL);
}

#if HAVE_X11
static void colocar_x11(void)
{
    GdkSurface *s = gtk_native_get_surface(gtk_widget_get_native(g_c.ventana));
    if (!GDK_IS_X11_SURFACE(s)) return;
    GdkRectangle geo = {0, 0, 1280, 800};
    GListModel *mons = gdk_display_get_monitors(gdk_display_get_default());
    if (g_list_model_get_n_items(mons) > 0) {
        GdkMonitor *m = g_list_model_get_item(mons, 0);
        gdk_monitor_get_geometry(m, &geo);
        g_object_unref(m);
    }
    XMoveWindow(od_x11_display(s), od_x11_ventana(s),
        geo.x + (geo.width - OD_CENTRO_ANCHO) / 2, geo.y + g_c.cfg->alto_barra);
}
#endif

/* ---- tarjetas ------------------------------------------------------------ */

static void quitar_tarjeta_y_notif(guint32 id)
{
    GtkWidget *t = tarjeta_de(id);
    if (t) gtk_box_remove(GTK_BOX(g_c.lista), t);
    OdNotif *n = buscar(id);
    if (n) {
        g_c.notifs = g_list_remove(g_c.notifs, n);
        od_notif_liberar(n);
    }
    actualizar_vacio();
}

static gboolean al_terminar_borrado(gpointer datos)
{
    guint32 id = GPOINTER_TO_UINT(datos);
    quitar_tarjeta_y_notif(id);
    if (g_c.rr.descartada) g_c.rr.descartada(id);
    return G_SOURCE_REMOVE;
}

/* Desliza la tarjeta a la derecha y la desvanece (0,26 s), luego la quita. */
static void borrar_con_animacion(GtkWidget *tarjeta)
{
    if (g_object_get_data(G_OBJECT(tarjeta), "od-borrando")) return;
    g_object_set_data(G_OBJECT(tarjeta), "od-borrando", GINT_TO_POINTER(1));
    gtk_widget_add_css_class(tarjeta, "od-borrando");
    guint32 id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(tarjeta), "od-id"));
    g_timeout_add(OD_BORRAR_MS, al_terminar_borrado, GUINT_TO_POINTER(id));
}

static void expandir(GtkWidget *tarjeta, gboolean si)
{
    GtkWidget *cuerpo = g_object_get_data(G_OBJECT(tarjeta), "od-cuerpo");
    GtkWidget *acciones = g_object_get_data(G_OBJECT(tarjeta), "od-acciones");
    gtk_label_set_wrap(GTK_LABEL(cuerpo), si);
    gtk_label_set_lines(GTK_LABEL(cuerpo), si ? 4 : 1);
    gtk_label_set_ellipsize(GTK_LABEL(cuerpo), PANGO_ELLIPSIZE_END);
    if (acciones) gtk_revealer_set_reveal_child(GTK_REVEALER(acciones), si);
    if (si) gtk_widget_add_css_class(tarjeta, "od-expandida");
    else gtk_widget_remove_css_class(tarjeta, "od-expandida");
}

static gboolean al_vencer_expansion(gpointer datos)
{
    GtkWidget *tarjeta = datos;
    g_object_set_data(G_OBJECT(tarjeta), "od-temporizador", NULL);
    expandir(tarjeta, TRUE);
    return G_SOURCE_REMOVE;
}

static void cancelar_expansion(GtkWidget *tarjeta)
{
    guint id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(tarjeta), "od-temporizador"));
    if (id) g_source_remove(id);
    g_object_set_data(G_OBJECT(tarjeta), "od-temporizador", NULL);
}

static void al_entrar_tarjeta(GtkEventControllerMotion *c, double x, double y, gpointer datos)
{
    (void)c; (void)x; (void)y; (void)datos;
    GtkWidget *tarjeta = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(c));
    gtk_widget_add_css_class(g_c.lista, "od-hay-hover");
    gtk_widget_add_css_class(tarjeta, "od-hover");
    cancelar_expansion(tarjeta);
    guint id = g_timeout_add(OD_EXPANDIR_MS, al_vencer_expansion, tarjeta);
    g_object_set_data(G_OBJECT(tarjeta), "od-temporizador", GUINT_TO_POINTER(id));
}

static void al_mover_en_tarjeta(GtkEventControllerMotion *c, double x, double y, gpointer datos)
{
    (void)y; (void)datos;
    GtkWidget *tarjeta = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(c));
    /* La papelera asoma al acercarse al borde derecho. */
    if (x > gtk_widget_get_width(tarjeta) - 80)
        gtk_widget_add_css_class(tarjeta, "od-cerca-borde");
    else
        gtk_widget_remove_css_class(tarjeta, "od-cerca-borde");
}

static void al_salir_tarjeta(GtkEventControllerMotion *c, gpointer datos)
{
    (void)datos;
    GtkWidget *tarjeta = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(c));
    gtk_widget_remove_css_class(g_c.lista, "od-hay-hover");
    gtk_widget_remove_css_class(tarjeta, "od-hover");
    gtk_widget_remove_css_class(tarjeta, "od-cerca-borde");
    cancelar_expansion(tarjeta);
    expandir(tarjeta, FALSE);
}

static void al_destruir_tarjeta(GtkWidget *tarjeta, gpointer datos)
{
    (void)datos;
    cancelar_expansion(tarjeta);
}

static void invocar(GtkWidget *tarjeta, const char *clave)
{
    guint32 id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(tarjeta), "od-id"));
    if (g_c.rr.accion) g_c.rr.accion(id, clave);
    quitar_tarjeta_y_notif(id);
}

static void al_pulsar_tarjeta(GtkGestureClick *g, int n, double x, double y, gpointer datos)
{
    (void)n; (void)x; (void)y; (void)datos;
    GtkWidget *tarjeta = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    guint32 id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(tarjeta), "od-id"));
    OdNotif *notif = buscar(id);
    gboolean tiene_default = FALSE;
    for (int i = 0; notif && notif->acciones && notif->acciones[i] && notif->acciones[i + 1]; i += 2)
        if (g_strcmp0(notif->acciones[i], "default") == 0) tiene_default = TRUE;
    if (tiene_default) {
        invocar(tarjeta, "default");
        od_centro_cerrar();
    }
}

static void al_pulsar_accion(GtkButton *b, gpointer datos)
{
    GtkWidget *tarjeta = datos;
    invocar(tarjeta, g_object_get_data(G_OBJECT(b), "od-clave"));
}

static void al_pulsar_papelera(GtkButton *b, gpointer datos)
{
    (void)b;
    borrar_con_animacion(datos);
}

static char *hora_corta(gint64 us)
{
    GDateTime *t = g_date_time_new_from_unix_local(us / G_USEC_PER_SEC);
    if (!t) return g_strdup("");
    char *s = g_date_time_format(t, "%H:%M");
    g_date_time_unref(t);
    return s;
}

static GtkWidget *crear_tarjeta(OdNotif *n)
{
    GtkWidget *tarjeta = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(tarjeta, "od-tarjeta");
    gtk_widget_set_size_request(tarjeta, OD_TARJETA_ANCHO, OD_TARJETA_ALTO);
    gtk_widget_set_halign(tarjeta, GTK_ALIGN_CENTER);
    gtk_widget_set_overflow(tarjeta, GTK_OVERFLOW_HIDDEN);
    g_object_set_data(G_OBJECT(tarjeta), "od-id", GUINT_TO_POINTER(n->id));

    GtkWidget *contenido = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(contenido, TRUE);
    gtk_widget_set_valign(contenido, GTK_ALIGN_CENTER);

    GtkWidget *fila = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(fila, 12);
    gtk_widget_set_margin_end(fila, 12);

    GtkWidget *icono = n->icono ? gtk_image_new_from_paintable(GDK_PAINTABLE(n->icono))
        : gtk_image_new_from_icon_name(n->icono_nombre ? n->icono_nombre : "dialog-information-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icono), 30);
    gtk_widget_set_valign(icono, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(fila), icono);

    GtkWidget *textos = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(textos, TRUE);
    GtkWidget *cabecera = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *titulo = gtk_label_new(n->titulo && *n->titulo ? n->titulo : n->app);
    gtk_widget_add_css_class(titulo, "od-tarjeta-titulo");
    gtk_label_set_xalign(GTK_LABEL(titulo), 0);
    gtk_label_set_ellipsize(GTK_LABEL(titulo), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(titulo, TRUE);
    gtk_box_append(GTK_BOX(cabecera), titulo);
    char *hora = hora_corta(n->hora_us);
    GtkWidget *lbl_hora = gtk_label_new(hora);
    g_free(hora);
    gtk_widget_add_css_class(lbl_hora, "od-tarjeta-hora");
    gtk_box_append(GTK_BOX(cabecera), lbl_hora);
    gtk_box_append(GTK_BOX(textos), cabecera);

    GtkWidget *cuerpo = gtk_label_new(n->cuerpo);
    gtk_widget_add_css_class(cuerpo, "od-tarjeta-cuerpo");
    gtk_label_set_xalign(GTK_LABEL(cuerpo), 0);
    gtk_label_set_ellipsize(GTK_LABEL(cuerpo), PANGO_ELLIPSIZE_END);
    gtk_label_set_wrap_mode(GTK_LABEL(cuerpo), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(cuerpo), 40);
    gtk_box_append(GTK_BOX(textos), cuerpo);
    gtk_box_append(GTK_BOX(fila), textos);
    gtk_box_append(GTK_BOX(contenido), fila);
    g_object_set_data(G_OBJECT(tarjeta), "od-cuerpo", cuerpo);

    /* Botones de acción (píldoras de 26 px), ocultos hasta expandir. */
    GtkWidget *botones = NULL;
    int n_botones = 0;
    for (int i = 0; n->acciones && n->acciones[i] && n->acciones[i + 1]; i += 2) {
        if (g_strcmp0(n->acciones[i], "default") == 0 || n_botones >= OD_MAX_ACCIONES) continue;
        if (!botones) {
            botones = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_margin_start(botones, 52);
            gtk_widget_set_margin_end(botones, 12);
        }
        GtkWidget *b = gtk_button_new_with_label(n->acciones[i + 1]);
        gtk_widget_add_css_class(b, "od-accion");
        g_object_set_data_full(G_OBJECT(b), "od-clave", g_strdup(n->acciones[i]), g_free);
        g_signal_connect(b, "clicked", G_CALLBACK(al_pulsar_accion), tarjeta);
        gtk_box_append(GTK_BOX(botones), b);
        n_botones++;
    }
    if (botones) {
        GtkWidget *revelador = gtk_revealer_new();
        gtk_revealer_set_transition_type(GTK_REVEALER(revelador), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
        gtk_revealer_set_transition_duration(GTK_REVEALER(revelador), 150);
        gtk_revealer_set_child(GTK_REVEALER(revelador), botones);
        gtk_box_append(GTK_BOX(contenido), revelador);
        g_object_set_data(G_OBJECT(tarjeta), "od-acciones", revelador);
    }
    gtk_box_append(GTK_BOX(tarjeta), contenido);

    GtkWidget *papelera = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_add_css_class(papelera, "od-papelera");
    gtk_widget_set_tooltip_text(papelera, "Borrar");
    g_signal_connect(papelera, "clicked", G_CALLBACK(al_pulsar_papelera), tarjeta);
    gtk_box_append(GTK_BOX(tarjeta), papelera);

    GtkEventController *mov = gtk_event_controller_motion_new();
    g_signal_connect(mov, "enter", G_CALLBACK(al_entrar_tarjeta), NULL);
    g_signal_connect(mov, "motion", G_CALLBACK(al_mover_en_tarjeta), NULL);
    g_signal_connect(mov, "leave", G_CALLBACK(al_salir_tarjeta), NULL);
    gtk_widget_add_controller(tarjeta, mov);

    GtkGesture *clic = gtk_gesture_click_new();
    g_signal_connect(clic, "released", G_CALLBACK(al_pulsar_tarjeta), NULL);
    gtk_widget_add_controller(tarjeta, GTK_EVENT_CONTROLLER(clic));

    g_signal_connect(tarjeta, "destroy", G_CALLBACK(al_destruir_tarjeta), NULL);
    return tarjeta;
}

/* ---- cabecera ------------------------------------------------------------ */

static void al_borrar_todo(GtkButton *b, gpointer datos)
{
    (void)b; (void)datos;
    for (GtkWidget *t = gtk_widget_get_first_child(g_c.lista); t; t = gtk_widget_get_next_sibling(t))
        borrar_con_animacion(t);
}

static void al_pulsar_campana(GtkButton *b, gpointer datos)
{
    (void)b; (void)datos;
    od_bar_fijar_no_molestar(!g_c.cfg->no_molestar);
}

static void al_pulsar_ajustes(GtkButton *b, gpointer datos)
{
    (void)b; (void)datos;
    od_centro_cerrar();
    od_config_abrir(g_c.cfg);
}

static GtkWidget *boton_cabecera(const char *icono, const char *ayuda, GCallback cb)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icono);
    gtk_widget_add_css_class(b, "od-boton-cabecera");
    gtk_widget_set_size_request(b, 32, 32);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(b, ayuda);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

/* ---- ventana ------------------------------------------------------------- */

static gboolean al_tecla(GtkEventControllerKey *c, guint tecla, guint codigo,
    GdkModifierType mods, gpointer datos)
{
    (void)c; (void)codigo; (void)mods; (void)datos;
    if (tecla == GDK_KEY_Escape) {
        od_centro_cerrar();
        return TRUE;
    }
    return FALSE;
}

/* Clic fuera = el compositor nos quita el foco. */
static void al_cambiar_activo(GObject *obj, GParamSpec *p, gpointer datos)
{
    (void)p; (void)datos;
    gboolean activo = gtk_window_is_active(GTK_WINDOW(obj));
    if (activo) g_c.estuvo_activo = TRUE;
    else if (g_c.estuvo_activo && g_c.abierto) od_centro_cerrar();
}

void od_centro_iniciar(OdConfig *cfg, OdBackendTipo backend, const OdCentroRetrollamadas *rr)
{
    if (g_c.iniciado) return;
    g_c.iniciado = TRUE;
    g_c.cfg = cfg;
    g_c.backend = backend;
    if (rr) g_c.rr = *rr;

    GtkWidget *win = gtk_window_new();
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_title(GTK_WINDOW(win), "OpenDock-Centro");
    gtk_widget_add_css_class(win, "od-centro-ventana");

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(panel, "od-centro");
    gtk_widget_set_size_request(panel, OD_CENTRO_ANCHO, -1);

    GtkWidget *cabecera = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_size_request(cabecera, -1, 56);
    gtk_widget_set_margin_start(cabecera, 18);
    gtk_widget_set_margin_end(cabecera, 12);
    GtkWidget *titulo = gtk_label_new("Notificaciones");
    gtk_widget_add_css_class(titulo, "od-centro-titulo");
    gtk_label_set_xalign(GTK_LABEL(titulo), 0);
    gtk_widget_set_hexpand(titulo, TRUE);
    gtk_box_append(GTK_BOX(cabecera), titulo);
    gtk_box_append(GTK_BOX(cabecera), boton_cabecera("edit-clear-all-symbolic", "Borrar todo",
        G_CALLBACK(al_borrar_todo)));
    g_c.btn_campana = boton_cabecera("preferences-system-notifications-symbolic", "",
        G_CALLBACK(al_pulsar_campana));
    gtk_box_append(GTK_BOX(cabecera), g_c.btn_campana);
    gtk_box_append(GTK_BOX(cabecera), boton_cabecera("emblem-system-symbolic", "Ajustes",
        G_CALLBACK(al_pulsar_ajustes)));
    gtk_box_append(GTK_BOX(panel), cabecera);

    GtkWidget *lista = gtk_box_new(GTK_ORIENTATION_VERTICAL, OD_TARJETA_SEP);
    gtk_widget_add_css_class(lista, "od-lista");
    gtk_widget_set_margin_bottom(lista, 18);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll),
        OD_TARJETAS_VISIBLES * OD_TARJETA_ALTO + (OD_TARJETAS_VISIBLES - 1) * OD_TARJETA_SEP + 18);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), lista);
    gtk_box_append(GTK_BOX(panel), scroll);

    GtkWidget *vacio = gtk_label_new("Sin notificaciones");
    gtk_widget_add_css_class(vacio, "od-centro-vacio");
    gtk_widget_set_margin_bottom(vacio, 22);
    gtk_box_append(GTK_BOX(panel), vacio);

    gtk_window_set_child(GTK_WINDOW(win), panel);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window.od-centro-ventana { background: transparent; }"
        ".od-centro { background-color: #1C1C1E; border-radius: 0 0 26px 26px; }"
        ".od-centro-titulo { color: #FFFFFF; font-size: 17px; font-weight: 600; }"
        ".od-centro-vacio { color: #6E6E73; font-size: 13px; }"
        ".od-boton-cabecera { min-width: 32px; min-height: 32px; padding: 0; border-radius: 16px;"
        "  background: rgba(255,255,255,0.08); color: #FFFFFF; border: none; box-shadow: none; }"
        ".od-boton-cabecera:hover { background: rgba(255,255,255,0.16); }"
        /* Tarjetas: 1,035 la del cursor y 0,965 las demás (k 420 ≈ 0,22 s con rebote). */
        ".od-tarjeta { background-color: #2C2C2E; border-radius: 16px; min-height: 70px;"
        "  transition: transform 220ms cubic-bezier(0.34, 1.4, 0.64, 1), opacity 260ms ease-in; }"
        ".od-lista.od-hay-hover .od-tarjeta { transform: scale(0.965); }"
        ".od-lista .od-tarjeta.od-hover { transform: scale(1.035); background-color: #3A3A3C; }"
        ".od-tarjeta.od-borrando { transform: translateX(348px); opacity: 0; }"
        ".od-tarjeta-titulo { color: #FFFFFF; font-size: 14px; font-weight: 600; }"
        ".od-tarjeta-cuerpo { color: #AEAEB2; font-size: 12px; }"
        ".od-tarjeta-hora { color: #6E6E73; font-size: 12px; }"
        ".od-accion { min-height: 26px; padding: 0 12px; border-radius: 13px; border: none;"
        "  box-shadow: none; background: #0A84FF; color: #FFFFFF; font-size: 12px; margin-bottom: 8px; }"
        /* Papelera: escondida (0 px) hasta acercarse al borde; 40 → 58 al pasar por ella. */
        ".od-papelera { background: #E5443C; color: #FFFFFF; border: none; box-shadow: none;"
        "  border-radius: 0; min-width: 0; padding: 0; opacity: 0;"
        "  transition: min-width 180ms cubic-bezier(0.34, 1.4, 0.64, 1), opacity 120ms; }"
        ".od-tarjeta.od-cerca-borde .od-papelera { min-width: 40px; opacity: 1; }"
        ".od-tarjeta.od-cerca-borde .od-papelera:hover { min-width: 58px; }");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(win),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkEventController *teclas = gtk_event_controller_key_new();
    g_signal_connect(teclas, "key-pressed", G_CALLBACK(al_tecla), NULL);
    gtk_widget_add_controller(win, teclas);
    g_signal_connect(win, "notify::is-active", G_CALLBACK(al_cambiar_activo), NULL);

    g_c.ventana = win;
    g_c.lista = lista;
    g_c.vacio = vacio;

#if HAVE_LAYER_SHELL
    if (backend == OD_BACKEND_WAYLAND && gtk_layer_is_supported()) {
        gtk_layer_init_for_window(GTK_WINDOW(win));
        gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_namespace(GTK_WINDOW(win), "opendock-centro");
        /* -1: se coloca desde el borde de la pantalla, sin apartarse de la
         * zona exclusiva de la barra (el margen ya la salta). */
        gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, OD_OCULTO_PX);
    }
#endif

    /* DESIGN.md: abrir con k 420; cerrar k 300 ζ 1 (se cambia al cerrar). */
    od_muelle_iniciar(&g_c.muelle, OD_OCULTO_PX, 420.0, OD_ZETA_NORMAL + 0.1);
    actualizar_campana();
    actualizar_vacio();
    gtk_widget_set_visible(win, FALSE);
}

void od_centro_agregar(OdNotif *n)
{
    if (!g_c.iniciado || !n) {
        od_notif_liberar(n);
        return;
    }
    if (buscar(n->id)) quitar_tarjeta_y_notif(n->id);
    g_c.notifs = g_list_prepend(g_c.notifs, n);
    gtk_box_prepend(GTK_BOX(g_c.lista), crear_tarjeta(n));
    actualizar_vacio();
}

void od_centro_quitar(guint32 id)
{
    if (!g_c.iniciado) return;
    quitar_tarjeta_y_notif(id);
}

guint od_centro_cantidad(void)
{
    return g_list_length(g_c.notifs);
}

gboolean od_centro_abierto(void)
{
    return g_c.abierto;
}

void od_centro_abrir(void)
{
    if (!g_c.iniciado || g_c.abierto) return;
    g_c.abierto = TRUE;
    g_c.estuvo_activo = FALSE;
    od_notch_ocultar();
    od_notch_marcar_leidas();
    actualizar_campana();
    g_c.muelle.k = 420.0;
    g_c.muelle.zeta = OD_ZETA_NORMAL + 0.1;
    gtk_window_present(GTK_WINDOW(g_c.ventana));
#if HAVE_X11
    if (g_c.backend == OD_BACKEND_X11) {
        colocar_x11();
        return;
    }
#endif
    od_muelle_fijar_objetivo(&g_c.muelle, g_c.cfg->alto_barra);
    animar();
}

void od_centro_cerrar(void)
{
    if (!g_c.iniciado || !g_c.abierto) return;
    g_c.abierto = FALSE;
#if HAVE_X11
    if (g_c.backend == OD_BACKEND_X11) {
        gtk_widget_set_visible(g_c.ventana, FALSE);
        return;
    }
#endif
    g_c.muelle.k = 300.0;
    g_c.muelle.zeta = 1.0;
    od_muelle_fijar_objetivo(&g_c.muelle, OD_OCULTO_PX);
    animar();
}

void od_centro_alternar(void)
{
    if (g_c.abierto) od_centro_cerrar();
    else od_centro_abrir();
}

void od_centro_refrescar_no_molestar(void)
{
    if (g_c.iniciado) actualizar_campana();
}
