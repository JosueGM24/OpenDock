/*
 * bar.c — barra superior (ver DESIGN.md: alto 28, zona exclusiva 28).
 *
 * Una barra en cada monitor (como en Windows): cada una con su ventana, su
 * reloj, sus iconos y su centro de control, que se abre bajo la barra en la
 * que se pulsó. El estado del sistema (Wi-Fi, batería, volumen, app activa)
 * se lee una sola vez y se pinta en todas. La bandeja y el mini notch viven
 * en la del primer monitor.
 *
 * Izquierda: icono + nombre de la app activa (lo rellena dock.c vía
 * od_bar_set_app_activa, con foreign-toplevel en Wayland o
 * _NET_ACTIVE_WINDOW en X11). Derecha: Wi-Fi, batería, volumen, reloj y
 * el botón del centro de control (popover con los mismos ajustes que
 * DESIGN.md, simplificados).
 */
#include "bar.h"
#include "opendock-build-config.h"
#include "volume.h"
#include "power.h"
#include "centro.h"
#include "mini.h"
#include "bandeja.h"
#include <gtk/gtk.h>
#include <math.h>

#if HAVE_LAYER_SHELL
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#endif
#if HAVE_X11
#include "x11.h"
#include <X11/Xatom.h>
#endif

/* Lo de cada monitor. */
typedef struct {
    GdkMonitor *monitor;
    GtkWidget *ventana;
    GtkWidget *caja_der;
    GtkWidget *lbl_app;
    GtkWidget *img_app;
    GtkWidget *lbl_reloj;
    GtkWidget *img_wifi;
    GtkWidget *img_bateria;
    GtkWidget *lbl_bateria;
    GtkWidget *img_volumen;
    GtkWidget *interruptor_wifi;
    GtkWidget *interruptor_bt;
    GtkWidget *control_brillo;
    GtkWidget *control_volumen;
    GtkWidget *lbl_bateria_cc;   /* tarjeta de batería del centro de control */
    GtkWidget *popover;
    GtkWidget *interruptor_no_molestar;
    gboolean principal;          /* la del primer monitor: bandeja y mini notch */
    gboolean pantalla_completa;  /* hay una ventana a pantalla completa encima */
} OdVistaBarra;

static struct {
    OdConfig *cfg;
    int alto;
    OdBackendTipo backend;
    gboolean iniciada;
    GPtrArray *vistas;           /* OdVistaBarra* */
    GtkWidget *chevron;          /* la bandeja: una sola, en la barra principal */
    gchar *app_nombre, *app_icono;
} g_barra;

#define PARA_CADA_VISTA(v) \
    for (guint i_ = 0; g_barra.vistas && i_ < g_barra.vistas->len; i_++) \
        for (OdVistaBarra *v = g_ptr_array_index(g_barra.vistas, i_); v; v = NULL)

static gboolean al_tic_reloj(gpointer datos)
{
    (void)datos;
    GDateTime *ahora = g_date_time_new_now_local();
    /* "Jue 2 oct  14:05" (DESIGN.md); el idioma depende del locale del
     * sistema, en C/POSIX sale en inglés y sin acentuar -- no afecta a
     * la prueba automática, que sólo comprueba que la barra existe. */
    char *txt = g_date_time_format(ahora, "%a %e %b  %H:%M");
    PARA_CADA_VISTA(v) gtk_label_set_text(GTK_LABEL(v->lbl_reloj), txt);
    g_free(txt);
    g_date_time_unref(ahora);
    return G_SOURCE_CONTINUE;
}

static void refrescar_wifi(void)
{
    int on = od_wifi_activado();
    /* Con cable conectado, el icono de la barra es el de Ethernet (como en Windows). */
    const char *icono = od_red_por_cable() == 1 ? "network-wired-symbolic"
        : on == 1 ? "network-wireless-symbolic" : "network-wireless-disabled-symbolic";
    PARA_CADA_VISTA(v) {
        /* Sin NetworkManager no sabemos nada del Wi-Fi: mejor no enseñarlo. */
        gtk_widget_set_visible(v->img_wifi, on >= 0);
        gtk_image_set_from_icon_name(GTK_IMAGE(v->img_wifi), icono);
        if (v->interruptor_wifi) {
            gtk_widget_set_sensitive(v->interruptor_wifi, on >= 0);
            gtk_switch_set_state(GTK_SWITCH(v->interruptor_wifi), on == 1);
        }
    }
}

static void refrescar_bateria(void)
{
    int pct; gboolean cargando;
    od_bateria_leer(&pct, &cargando);
    const char *icono = cargando ? "battery-good-charging-symbolic"
        : pct < 20 ? "battery-caution-symbolic"
        : pct < 60 ? "battery-low-symbolic" : "battery-full-symbolic";
    char *txt = g_strdup_printf("%d%%", pct);
    char *txt_cc = g_strdup_printf("%d %%%s", pct, cargando ? " · cargando" : "");
    PARA_CADA_VISTA(v) {
        if (v->lbl_bateria_cc) {
            /* La tarjeta entera se oculta en equipos sin batería. */
            gtk_widget_set_visible(gtk_widget_get_parent(v->lbl_bateria_cc), pct >= 0);
            if (pct >= 0) gtk_label_set_text(GTK_LABEL(v->lbl_bateria_cc), txt_cc);
        }
        gtk_widget_set_visible(v->img_bateria, pct >= 0);
        gtk_widget_set_visible(v->lbl_bateria, pct >= 0);
        if (pct < 0) continue;
        gtk_image_set_from_icon_name(GTK_IMAGE(v->img_bateria), icono);
        gtk_label_set_text(GTK_LABEL(v->lbl_bateria), txt);
    }
    g_free(txt);
    g_free(txt_cc);
}

static void al_mover_volumen(GtkRange *r, gpointer datos);

static void refrescar_volumen(void)
{
    int vol = od_volumen_obtener();
    gboolean mudo = od_volumen_silenciado();
    const char *icono = mudo ? "audio-volume-muted-symbolic"
        : vol < 34 ? "audio-volume-low-symbolic"
        : vol < 67 ? "audio-volume-medium-symbolic" : "audio-volume-high-symbolic";
    PARA_CADA_VISTA(v) {
        gtk_widget_set_visible(v->img_volumen, vol >= 0);
        if (vol < 0) continue;
        gtk_image_set_from_icon_name(GTK_IMAGE(v->img_volumen), icono);
        if (v->control_volumen) {
            /* Sin volver a mandar el valor a PulseAudio (evita el eco al arrastrar). */
            g_signal_handlers_block_by_func(v->control_volumen, al_mover_volumen, NULL);
            gtk_range_set_value(GTK_RANGE(v->control_volumen), vol);
            g_signal_handlers_unblock_by_func(v->control_volumen, al_mover_volumen, NULL);
        }
    }
}

static void al_cambiar_volumen(gpointer datos)
{
    (void)datos;
    refrescar_volumen();
}

static gboolean al_tic_estado(gpointer datos)
{
    (void)datos;
    refrescar_wifi();
    refrescar_bateria();
    return G_SOURCE_CONTINUE;
}

static gboolean al_mover_wifi(GtkSwitch *sw, gboolean estado, gpointer datos)
{
    (void)sw; (void)datos;
    od_wifi_fijar_activado(estado);
    PARA_CADA_VISTA(v) if (v->interruptor_wifi) gtk_switch_set_state(GTK_SWITCH(v->interruptor_wifi), estado);
    return TRUE; /* no dejar que el manejador por defecto también fije el estado */
}

static gboolean al_mover_bt(GtkSwitch *sw, gboolean estado, gpointer datos)
{
    (void)sw; (void)datos;
    od_bluetooth_fijar_activado(estado);
    PARA_CADA_VISTA(v) if (v->interruptor_bt) gtk_switch_set_state(GTK_SWITCH(v->interruptor_bt), estado);
    return TRUE;
}

static void al_mover_brillo(GtkRange *r, gpointer datos)
{
    (void)datos;
    od_brillo_fijar(gtk_range_get_value(r) / 100.0);
}

static void al_mover_volumen(GtkRange *r, gpointer datos)
{
    (void)datos;
    od_volumen_fijar((int)gtk_range_get_value(r));
}

static gboolean al_mover_no_molestar(GtkSwitch *sw, gboolean estado, gpointer datos)
{
    (void)sw; (void)datos;
    od_bar_fijar_no_molestar(estado);
    return TRUE;
}

/* Un solo sitio que cambia No molestar: lo guarda y pone al día el
 * interruptor de cada barra, la campana del centro y la vista rápida. */
void od_bar_fijar_no_molestar(gboolean activo)
{
    if (!g_barra.cfg) return;
    g_barra.cfg->no_molestar = activo;
    od_config_guardar_bool(g_barra.cfg, "general", "no_molestar", activo);
    PARA_CADA_VISTA(v)
        if (v->interruptor_no_molestar)
            gtk_switch_set_state(GTK_SWITCH(v->interruptor_no_molestar), activo);
    od_centro_refrescar_no_molestar();
    od_mini_refrescar();
}

/* "Ajustes ›": abre config.ini con el editor predeterminado (sin shell). */
static void al_pulsar_ajustes(GtkButton *b, gpointer datos)
{
    (void)b;
    OdVistaBarra *v = datos;
    gtk_popover_popdown(GTK_POPOVER(v->popover));
    od_config_abrir(g_barra.cfg);
}

/* Cursor sobre la barra: la zona central despierta el mini notch (sólo en la
 * principal, que es donde está el notch). */
static void al_mover_en_barra(GtkEventControllerMotion *c, double x, double y, gpointer datos)
{
    (void)y;
    OdVistaBarra *v = datos;
    if (!v->principal) return;
    double dx = x - gtk_widget_get_width(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(c))) / 2.0;
    od_mini_cursor(fabs(dx) < OD_MINI_ZONA / 2.0, dx);
}

static void al_salir_de_barra(GtkEventControllerMotion *c, gpointer datos)
{
    (void)c;
    OdVistaBarra *v = datos;
    if (v->principal) od_mini_cursor(FALSE, 0);
}

static void al_pulsar_reloj(GtkGestureClick *g, int n, double x, double y, gpointer datos)
{
    (void)g; (void)n; (void)x; (void)y; (void)datos;
    od_centro_alternar();
}

static GtkWidget *crear_tarjeta_interruptor(const char *icono, const char *texto,
    GCallback al_mover, GtkWidget **interruptor)
{
    GtkWidget *tarjeta = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(tarjeta, "opendock-tarjeta");
    gtk_widget_set_hexpand(tarjeta, TRUE);
    gtk_box_append(GTK_BOX(tarjeta), gtk_image_new_from_icon_name(icono));
    gtk_box_append(GTK_BOX(tarjeta), gtk_label_new(texto));
    GtkWidget *sw = gtk_switch_new();
    gtk_widget_set_hexpand(sw, TRUE);
    gtk_widget_set_halign(sw, GTK_ALIGN_END);
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    g_signal_connect(sw, "state-set", al_mover, NULL);
    gtk_box_append(GTK_BOX(tarjeta), sw);
    *interruptor = sw;
    return tarjeta;
}

/* Centro de control (DESIGN.md, simplificado): Wi-Fi y Bluetooth, No
 * molestar, batería, brillo, volumen y "Ajustes ›". Uno por barra. */
static GtkWidget *crear_centro_control(OdVistaBarra *v)
{
    GtkWidget *caja = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(caja, 14);
    gtk_widget_set_margin_end(caja, 14);
    gtk_widget_set_margin_top(caja, 14);
    gtk_widget_set_margin_bottom(caja, 14);
    gtk_widget_set_size_request(caja, 312, -1);

    GtkWidget *fila_redes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    GtkWidget *tarjeta_wifi = crear_tarjeta_interruptor("network-wireless-symbolic", "Wi-Fi",
        G_CALLBACK(al_mover_wifi), &v->interruptor_wifi);
    GtkWidget *tarjeta_bt = crear_tarjeta_interruptor("bluetooth-symbolic", "Bluetooth",
        G_CALLBACK(al_mover_bt), &v->interruptor_bt);
    int bt = od_bluetooth_activado();
    if (bt < 0) gtk_widget_set_sensitive(v->interruptor_bt, FALSE);
    else gtk_switch_set_state(GTK_SWITCH(v->interruptor_bt), bt == 1);

    gtk_box_append(GTK_BOX(fila_redes), tarjeta_wifi);
    gtk_box_append(GTK_BOX(fila_redes), tarjeta_bt);
    gtk_box_append(GTK_BOX(caja), fila_redes);

    GtkWidget *tarjeta_nm = crear_tarjeta_interruptor("notifications-disabled-symbolic",
        "No molestar", G_CALLBACK(al_mover_no_molestar), &v->interruptor_no_molestar);
    gtk_switch_set_state(GTK_SWITCH(v->interruptor_no_molestar), g_barra.cfg->no_molestar);
    gtk_box_append(GTK_BOX(caja), tarjeta_nm);

    GtkWidget *tarjeta_bateria = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(tarjeta_bateria, "opendock-tarjeta");
    gtk_box_append(GTK_BOX(tarjeta_bateria), gtk_image_new_from_icon_name("battery-full-symbolic"));
    gtk_box_append(GTK_BOX(tarjeta_bateria), gtk_label_new("Batería"));
    GtkWidget *lbl_pct = gtk_label_new("");
    gtk_widget_set_hexpand(lbl_pct, TRUE);
    gtk_widget_set_halign(lbl_pct, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(tarjeta_bateria), lbl_pct);
    gtk_widget_set_visible(tarjeta_bateria, FALSE);
    gtk_box_append(GTK_BOX(caja), tarjeta_bateria);
    v->lbl_bateria_cc = lbl_pct;

    GtkWidget *fila_brillo = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(fila_brillo), gtk_image_new_from_icon_name("display-brightness-symbolic"));
    GtkWidget *r_brillo = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_widget_set_hexpand(r_brillo, TRUE);
    double brillo; od_brillo_leer(&brillo);
    if (brillo < 0) {
        gtk_widget_set_sensitive(r_brillo, FALSE);
    } else {
        gtk_range_set_value(GTK_RANGE(r_brillo), brillo * 100);
    }
    g_signal_connect(r_brillo, "value-changed", G_CALLBACK(al_mover_brillo), NULL);
    gtk_box_append(GTK_BOX(fila_brillo), r_brillo);
    gtk_box_append(GTK_BOX(caja), fila_brillo);
    v->control_brillo = r_brillo;

    GtkWidget *fila_volumen = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(fila_volumen), gtk_image_new_from_icon_name("audio-volume-high-symbolic"));
    GtkWidget *r_vol = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_widget_set_hexpand(r_vol, TRUE);
    int vol = od_volumen_obtener();
    if (vol < 0) gtk_widget_set_sensitive(r_vol, FALSE);
    else gtk_range_set_value(GTK_RANGE(r_vol), vol);
    g_signal_connect(r_vol, "value-changed", G_CALLBACK(al_mover_volumen), NULL);
    gtk_box_append(GTK_BOX(fila_volumen), r_vol);
    gtk_box_append(GTK_BOX(caja), fila_volumen);
    v->control_volumen = r_vol;

    GtkWidget *boton_ajustes = gtk_button_new_with_label("Ajustes ›");
    gtk_widget_add_css_class(boton_ajustes, "flat");
    gtk_widget_set_halign(boton_ajustes, GTK_ALIGN_END);
    g_signal_connect(boton_ajustes, "clicked", G_CALLBACK(al_pulsar_ajustes), v);
    gtk_box_append(GTK_BOX(caja), boton_ajustes);

    return caja;
}

#if HAVE_X11
static GdkRectangle geometria_de(OdVistaBarra *v)
{
    GdkRectangle geo = {0, 0, 1280, 800};
    if (v->monitor) gdk_monitor_get_geometry(v->monitor, &geo);
    return geo;
}

/* Antes de mapear: el gestor de ventanas lee el tipo y el strut al mapear
 * la ventana (si se ponen después, openbox/xfwm4 la tratan como normal). */
static void al_realizar_x11(GtkWidget *win, gpointer datos)
{
    OdVistaBarra *v = datos;
    GdkSurface *surface = gtk_native_get_surface(gtk_widget_get_native(win));
    if (!GDK_IS_X11_SURFACE(surface)) return;
    Display *xdisplay = od_x11_display(surface);
    Window xid = od_x11_ventana(surface);
    GdkRectangle geo = geometria_de(v);

    Atom tipo = XInternAtom(xdisplay, "_NET_WM_WINDOW_TYPE", False);
    Atom dock = XInternAtom(xdisplay, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(xdisplay, xid, tipo, XA_ATOM, 32, PropModeReplace, (unsigned char *)&dock, 1);

    /* _NET_WM_STRUT_PARTIAL: left,right,top,bottom, y los rangos y1/y2,x1/x2
     * de cada borde reservado. Sólo reservamos arriba, en el ancho de su
     * monitor. El strut se mide desde el borde de la pantalla X entera:
     * en un monitor que no empieza arriba (y > 0) hay que sumarle su y. */
    long strut[12] = { 0 };
    strut[2] = geo.y + g_barra.alto;     /* top */
    strut[8] = geo.x;                    /* top_start_x */
    strut[9] = geo.x + geo.width - 1;    /* top_end_x (inclusivo) */
    Atom strut_partial = XInternAtom(xdisplay, "_NET_WM_STRUT_PARTIAL", False);
    XChangeProperty(xdisplay, xid, strut_partial, XA_CARDINAL, 32, PropModeReplace,
        (unsigned char *)strut, 12);
    Atom strut_simple = XInternAtom(xdisplay, "_NET_WM_STRUT", False);
    XChangeProperty(xdisplay, xid, strut_simple, XA_CARDINAL, 32, PropModeReplace,
        (unsigned char *)strut, 4);
}

/* Después de mapear: GTK4 no deja colocar ventanas, así que vamos a Xlib. */
static void colocar_x11(OdVistaBarra *v)
{
    GdkSurface *surface = gtk_native_get_surface(gtk_widget_get_native(v->ventana));
    if (!GDK_IS_X11_SURFACE(surface)) return;
    GdkRectangle geo = geometria_de(v);
    XMoveResizeWindow(od_x11_display(surface), od_x11_ventana(surface),
        geo.x, geo.y, geo.width, g_barra.alto);
}
#endif

static void cargar_css(GdkDisplay *display)
{
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window.opendock-barra { background-color: #1C1C1E; }"
        ".opendock-app-activa { color: #FFFFFF; font-weight: 600; font-size: 13px; }"
        ".opendock-reloj { color: #FFFFFF; font-weight: 600; font-size: 13px; }"
        ".opendock-tarjeta { background-color: rgba(255,255,255,0.08); border-radius: 12px; padding: 8px; }"
        /* El botón del engranaje con el estilo del tema mide 34 px y estira la
         * barra: aquí es un icono más, sin fondo ni relleno. */
        /* Sólo los hijos directos de las zonas: el popover del centro de
         * control también cuelga de la barra y conserva el tema. */
        ".opendock-zona > menubutton > button { min-height: 0; min-width: 0; padding: 2px 4px;"
        "  background: none; border: none; box-shadow: none; color: #FFFFFF; }"
        ".opendock-zona > image { color: #FFFFFF; -gtk-icon-size: 14px; }"
        ".opendock-zona > label { color: #FFFFFF; font-size: 13px; }"
        /* Popovers de la barra (centro de control, bandeja): oscuros como ella. */
        "popover.opendock-popover > contents, popover.opendock-popover > arrow {"
        "  background-color: #1C1C1E; color: #FFFFFF; border: none; }"
        "popover.opendock-popover > contents { border-radius: 16px; }"
        "popover.opendock-popover label, popover.opendock-popover image { color: #FFFFFF; }"
        "popover.opendock-popover button.flat:hover { background: rgba(255,255,255,0.10); }"
        "popover.opendock-popover menubutton > button { background: none; border: none; box-shadow: none; }"
        "popover.opendock-popover menubutton > button:hover { background: rgba(255,255,255,0.10); }"
        /* DESIGN.md: pasar el cursor 1,16, pulsar 0,88. GTK no tiene muelles
         * en CSS; una curva con rebote se le parece (k 520 ≈ 0,18 s). */
        ".opendock-icono { transition: transform 180ms cubic-bezier(0.34, 1.56, 0.64, 1); }"
        ".opendock-icono:hover { transform: scale(1.16); }"
        ".opendock-icono:active { transform: scale(0.88); }"
        );
    gtk_style_context_add_provider_for_display(display,
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static void mostrar_vista(OdVistaBarra *v)
{
    gboolean ver = !v->pantalla_completa;
    if (gtk_widget_get_visible(v->ventana) == ver) return;
    gtk_widget_set_visible(v->ventana, ver);
#if HAVE_X11
    if (ver && g_barra.backend == OD_BACKEND_X11) colocar_x11(v);
#endif
}

static OdVistaBarra *crear_vista(GdkMonitor *mon)
{
    OdVistaBarra *v = g_new0(OdVistaBarra, 1);
    v->monitor = g_object_ref(mon);
    OdConfig *cfg = g_barra.cfg;

    GtkWidget *win = gtk_window_new();
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    /* El título no se ve (ni en layer-shell ni sin decoración en X11); nos
     * sirve sólo para encontrar la ventana desde fuera (pruebas con xprop). */
    gtk_window_set_title(GTK_WINDOW(win), "OpenDock-Barra");
    gtk_widget_add_css_class(win, "opendock-barra");

    GtkWidget *centro = gtk_center_box_new();
    gtk_widget_set_size_request(centro, -1, cfg->alto_barra);

    GtkWidget *caja_izq = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(caja_izq, "opendock-zona");
    gtk_widget_set_margin_start(caja_izq, 16);
    v->img_app = gtk_image_new_from_icon_name(
        g_barra.app_icono ? g_barra.app_icono : "application-x-executable-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(v->img_app), 13);
    v->lbl_app = gtk_label_new(g_barra.app_nombre ? g_barra.app_nombre : "Escritorio");
    gtk_widget_add_css_class(v->lbl_app, "opendock-app-activa");
    gtk_box_append(GTK_BOX(caja_izq), v->img_app);
    gtk_box_append(GTK_BOX(caja_izq), v->lbl_app);
    gtk_center_box_set_start_widget(GTK_CENTER_BOX(centro), caja_izq);

    GtkWidget *caja_der = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(caja_der, "opendock-zona");
    gtk_widget_set_margin_end(caja_der, 16);
    gtk_widget_set_halign(caja_der, GTK_ALIGN_END);
    v->caja_der = caja_der;

    v->img_volumen = gtk_image_new_from_icon_name("audio-volume-high-symbolic");
    v->img_wifi = gtk_image_new_from_icon_name("network-wireless-symbolic");
    v->img_bateria = gtk_image_new_from_icon_name("battery-full-symbolic");
    v->lbl_bateria = gtk_label_new("");
    v->lbl_reloj = gtk_label_new("");
    gtk_widget_add_css_class(v->lbl_reloj, "opendock-reloj");

    GtkWidget *boton_ajustes = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(boton_ajustes), "emblem-system-symbolic");
    v->popover = gtk_popover_new();
    gtk_widget_add_css_class(v->popover, "opendock-popover");
    gtk_popover_set_child(GTK_POPOVER(v->popover), crear_centro_control(v));
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(boton_ajustes), v->popover);

    gtk_widget_add_css_class(v->img_volumen, "opendock-icono");
    gtk_widget_add_css_class(v->img_wifi, "opendock-icono");
    gtk_widget_add_css_class(v->img_bateria, "opendock-icono");
    gtk_widget_add_css_class(boton_ajustes, "opendock-icono");
    /* DESIGN.md: chevrón de la bandeja, sonido, Wi-Fi, batería, hora, engranaje.
     * El chevrón se pone luego, sólo en la barra principal. */
    gtk_box_append(GTK_BOX(caja_der), v->img_volumen);
    gtk_box_append(GTK_BOX(caja_der), v->img_wifi);
    gtk_box_append(GTK_BOX(caja_der), v->img_bateria);
    gtk_box_append(GTK_BOX(caja_der), v->lbl_bateria);
    gtk_box_append(GTK_BOX(caja_der), v->lbl_reloj);
    gtk_box_append(GTK_BOX(caja_der), boton_ajustes);
    gtk_center_box_set_end_widget(GTK_CENTER_BOX(centro), caja_der);

    gtk_window_set_child(GTK_WINDOW(win), centro);

    GtkEventController *mov = gtk_event_controller_motion_new();
    g_signal_connect(mov, "motion", G_CALLBACK(al_mover_en_barra), v);
    g_signal_connect(mov, "enter", G_CALLBACK(al_mover_en_barra), v);
    g_signal_connect(mov, "leave", G_CALLBACK(al_salir_de_barra), v);
    gtk_widget_add_controller(win, mov);

    /* Clic en el reloj: abre o cierra el centro de notificaciones. */
    GtkGesture *clic_reloj = gtk_gesture_click_new();
    g_signal_connect(clic_reloj, "released", G_CALLBACK(al_pulsar_reloj), NULL);
    gtk_widget_add_controller(v->lbl_reloj, GTK_EVENT_CONTROLLER(clic_reloj));
    gtk_widget_set_cursor_from_name(v->lbl_reloj, "pointer");

    v->ventana = win;

#if HAVE_LAYER_SHELL
    if (g_barra.backend == OD_BACKEND_WAYLAND && gtk_layer_is_supported()) {
        gtk_layer_init_for_window(GTK_WINDOW(win));
        gtk_layer_set_monitor(GTK_WINDOW(win), mon);
        gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_TOP);
        gtk_layer_set_namespace(GTK_WINDOW(win), "opendock-bar");
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_exclusive_zone(GTK_WINDOW(win), cfg->alto_barra);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    }
#endif
#if HAVE_X11
    if (g_barra.backend == OD_BACKEND_X11) {
        GdkRectangle geo = geometria_de(v);
        /* En X11 fijamos el tamaño; en Wayland lo da el compositor (ancla
         * izquierda + derecha), por eso la ventana no puede ser fija allí. */
        gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
        gtk_widget_set_size_request(win, geo.width, cfg->alto_barra);
        g_signal_connect(win, "realize", G_CALLBACK(al_realizar_x11), v);
    }
#endif

    gtk_widget_set_visible(win, TRUE);
#if HAVE_X11
    if (g_barra.backend == OD_BACKEND_X11) colocar_x11(v);
#endif
    return v;
}

static void liberar_vista(gpointer p)
{
    OdVistaBarra *v = p;
    if (g_barra.chevron && gtk_widget_get_parent(g_barra.chevron) == v->caja_der)
        gtk_box_remove(GTK_BOX(v->caja_der), g_barra.chevron);   /* la bandeja sigue viva (ref propia) */
    if (v->ventana) gtk_window_destroy(GTK_WINDOW(v->ventana));
    g_clear_object(&v->monitor);
    g_free(v);
}

/* Monitores conectados o quitados: una barra por monitor, la principal en el
 * primero de la lista (con la bandeja y el mini notch). */
static void sincronizar_vistas(void)
{
    GListModel *mons = gdk_display_get_monitors(gdk_display_get_default());
    guint n = g_list_model_get_n_items(mons);

    /* fuera las de monitores que ya no están */
    for (guint i = g_barra.vistas->len; i > 0; i--) {
        OdVistaBarra *v = g_ptr_array_index(g_barra.vistas, i - 1);
        gboolean sigue = FALSE;
        for (guint k = 0; k < n && !sigue; k++) {
            GdkMonitor *m = g_list_model_get_item(mons, k);
            sigue = m == v->monitor && gdk_monitor_is_valid(m);
            g_object_unref(m);
        }
        if (!sigue) g_ptr_array_remove_index(g_barra.vistas, i - 1);
    }
    /* y una nueva por cada monitor sin barra, en el orden de la lista */
    GPtrArray *orden = g_ptr_array_new();
    for (guint k = 0; k < n; k++) {
        GdkMonitor *m = g_list_model_get_item(mons, k);
        OdVistaBarra *v = NULL;
        for (guint i = 0; i < g_barra.vistas->len && !v; i++) {
            OdVistaBarra *c = g_ptr_array_index(g_barra.vistas, i);
            if (c->monitor == m) v = c;
        }
        if (!v) v = crear_vista(m);
        g_ptr_array_add(orden, v);
        g_object_unref(m);
    }
    g_ptr_array_set_free_func(g_barra.vistas, NULL);
    g_ptr_array_unref(g_barra.vistas);
    g_barra.vistas = g_ptr_array_new_with_free_func(liberar_vista);
    for (guint i = 0; i < orden->len; i++) g_ptr_array_add(g_barra.vistas, g_ptr_array_index(orden, i));
    g_ptr_array_free(orden, TRUE);

    /* la principal: la bandeja se muda a ella si hace falta */
    PARA_CADA_VISTA(v) {
        v->principal = i_ == 0;
        if (v->principal && g_barra.chevron && gtk_widget_get_parent(g_barra.chevron) != v->caja_der) {
            GtkWidget *antes = gtk_widget_get_parent(g_barra.chevron);
            if (antes) gtk_box_remove(GTK_BOX(antes), g_barra.chevron);
            gtk_box_prepend(GTK_BOX(v->caja_der), g_barra.chevron);
        }
    }
    al_tic_reloj(NULL);
    refrescar_wifi();
    refrescar_bateria();
    refrescar_volumen();
}

static void al_cambiar_monitores(GListModel *l, guint pos, guint quitados, guint puestos, gpointer d)
{
    (void)l; (void)pos; (void)quitados; (void)puestos; (void)d;
    sincronizar_vistas();
}

void od_bar_iniciar(OdConfig *cfg, OdBackendTipo backend)
{
    if (g_barra.iniciada) return;
    g_barra.iniciada = TRUE;
    g_barra.alto = cfg->alto_barra;
    g_barra.cfg = cfg;
    g_barra.backend = backend;
    g_barra.vistas = g_ptr_array_new_with_free_func(liberar_vista);

    GdkDisplay *display = gdk_display_get_default();
    cargar_css(display);
    g_barra.chevron = g_object_ref_sink(od_bandeja_crear_boton());

    od_volumen_iniciar();
    od_volumen_conectar_cambio(al_cambiar_volumen, NULL);

    sincronizar_vistas();
    g_signal_connect(gdk_display_get_monitors(display), "items-changed",
        G_CALLBACK(al_cambiar_monitores), NULL);

    g_timeout_add_seconds(1, al_tic_reloj, NULL);
    g_timeout_add_seconds(15, al_tic_estado, NULL);
    od_red_vigilar(refrescar_wifi);     /* cable / Wi-Fi al momento, sin esperar al tic */
}

void od_bar_set_app_activa(const char *nombre, const char *icono_nombre)
{
    if (!g_barra.iniciada) return;
    g_free(g_barra.app_nombre);
    g_free(g_barra.app_icono);
    g_barra.app_nombre = g_strdup(nombre && *nombre ? nombre : "Escritorio");
    g_barra.app_icono = g_strdup((icono_nombre && *icono_nombre) ? icono_nombre : "application-x-executable-symbolic");
    PARA_CADA_VISTA(v) {
        gtk_label_set_text(GTK_LABEL(v->lbl_app), g_barra.app_nombre);
        gtk_image_set_from_icon_name(GTK_IMAGE(v->img_app), g_barra.app_icono);
    }
}

void od_bar_pantalla_completa(OdPantallaCompletaFn en_monitor)
{
    if (!g_barra.iniciada) return;
    PARA_CADA_VISTA(v) {
        v->pantalla_completa = en_monitor && en_monitor(v->monitor);
        mostrar_vista(v);
    }
}
