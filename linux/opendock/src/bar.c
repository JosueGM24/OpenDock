/*
 * bar.c — barra superior (ver DESIGN.md: alto 28, zona exclusiva 28).
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
#include <gtk/gtk.h>

#if HAVE_LAYER_SHELL
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#endif
#if HAVE_X11
#include "x11.h"
#include <X11/Xatom.h>
#endif

typedef struct {
    GtkWidget *ventana;
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
    int alto;
    OdBackendTipo backend;
    gboolean iniciada;
} OdEstadoBarra;

static OdEstadoBarra g_barra;

static gboolean al_tic_reloj(gpointer datos)
{
    (void)datos;
    GDateTime *ahora = g_date_time_new_now_local();
    /* "Jue 2 oct  14:05" (DESIGN.md); el idioma depende del locale del
     * sistema, en C/POSIX sale en inglés y sin acentuar -- no afecta a
     * la prueba automática, que sólo comprueba que la barra existe. */
    char *txt = g_date_time_format(ahora, "%a %e %b  %H:%M");
    gtk_label_set_text(GTK_LABEL(g_barra.lbl_reloj), txt);
    g_free(txt);
    g_date_time_unref(ahora);
    return G_SOURCE_CONTINUE;
}

static void refrescar_wifi(void)
{
    int on = od_wifi_activado();
    const char *icono = (on == 1) ? "network-wireless-symbolic"
        : (on == 0) ? "network-wireless-disabled-symbolic"
        : "network-wireless-offline-symbolic";
    gtk_image_set_from_icon_name(GTK_IMAGE(g_barra.img_wifi), icono);
    if (g_barra.interruptor_wifi)
        gtk_switch_set_state(GTK_SWITCH(g_barra.interruptor_wifi), on == 1);
}

static void refrescar_bateria(void)
{
    int pct; gboolean cargando;
    od_bateria_leer(&pct, &cargando);
    if (pct < 0) {
        gtk_widget_set_visible(g_barra.img_bateria, FALSE);
        gtk_widget_set_visible(g_barra.lbl_bateria, FALSE);
        return;
    }
    gtk_widget_set_visible(g_barra.img_bateria, TRUE);
    gtk_widget_set_visible(g_barra.lbl_bateria, TRUE);
    const char *icono = cargando ? "battery-good-charging-symbolic"
        : pct < 20 ? "battery-caution-symbolic"
        : pct < 60 ? "battery-low-symbolic" : "battery-full-symbolic";
    gtk_image_set_from_icon_name(GTK_IMAGE(g_barra.img_bateria), icono);
    char *txt = g_strdup_printf("%d%%", pct);
    gtk_label_set_text(GTK_LABEL(g_barra.lbl_bateria), txt);
    g_free(txt);
}

static void al_mover_volumen(GtkRange *r, gpointer datos);

static void refrescar_volumen(void)
{
    int vol = od_volumen_obtener();
    if (vol < 0) {
        gtk_widget_set_visible(g_barra.img_volumen, FALSE);
        return;
    }
    gtk_widget_set_visible(g_barra.img_volumen, TRUE);
    gboolean mudo = od_volumen_silenciado();
    const char *icono = mudo ? "audio-volume-muted-symbolic"
        : vol < 34 ? "audio-volume-low-symbolic"
        : vol < 67 ? "audio-volume-medium-symbolic" : "audio-volume-high-symbolic";
    gtk_image_set_from_icon_name(GTK_IMAGE(g_barra.img_volumen), icono);
    if (g_barra.control_volumen) {
        /* Sin volver a mandar el valor a PulseAudio (evita el eco al arrastrar). */
        g_signal_handlers_block_by_func(g_barra.control_volumen, al_mover_volumen, NULL);
        gtk_range_set_value(GTK_RANGE(g_barra.control_volumen), vol);
        g_signal_handlers_unblock_by_func(g_barra.control_volumen, al_mover_volumen, NULL);
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
    (void)datos;
    od_wifi_fijar_activado(estado);
    gtk_switch_set_state(sw, estado);
    return TRUE; /* no dejar que el manejador por defecto también fije el estado */
}

static gboolean al_mover_bt(GtkSwitch *sw, gboolean estado, gpointer datos)
{
    (void)datos;
    od_bluetooth_fijar_activado(estado);
    gtk_switch_set_state(sw, estado);
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

/* Centro de control simplificado (DESIGN.md): Wi-Fi/Bluetooth, brillo y
 * volumen. No incluye todavía No molestar / notificaciones / "Ajustes"
 * (quedan para una iteración posterior del propio centro de control). */
static GtkWidget *crear_centro_control(void)
{
    GtkWidget *caja = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(caja, 14);
    gtk_widget_set_margin_end(caja, 14);
    gtk_widget_set_margin_top(caja, 14);
    gtk_widget_set_margin_bottom(caja, 14);
    gtk_widget_set_size_request(caja, 312, -1);

    GtkWidget *fila_redes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    GtkWidget *tarjeta_wifi = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(tarjeta_wifi, "opendock-tarjeta");
    gtk_box_append(GTK_BOX(tarjeta_wifi), gtk_image_new_from_icon_name("network-wireless-symbolic"));
    gtk_box_append(GTK_BOX(tarjeta_wifi), gtk_label_new("Wi-Fi"));
    GtkWidget *sw_wifi = gtk_switch_new();
    gtk_widget_set_hexpand(sw_wifi, TRUE);
    gtk_widget_set_halign(sw_wifi, GTK_ALIGN_END);
    g_signal_connect(sw_wifi, "state-set", G_CALLBACK(al_mover_wifi), NULL);
    gtk_box_append(GTK_BOX(tarjeta_wifi), sw_wifi);
    g_barra.interruptor_wifi = sw_wifi;

    GtkWidget *tarjeta_bt = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(tarjeta_bt, "opendock-tarjeta");
    gtk_box_append(GTK_BOX(tarjeta_bt), gtk_image_new_from_icon_name("bluetooth-symbolic"));
    gtk_box_append(GTK_BOX(tarjeta_bt), gtk_label_new("Bluetooth"));
    GtkWidget *sw_bt = gtk_switch_new();
    gtk_widget_set_hexpand(sw_bt, TRUE);
    gtk_widget_set_halign(sw_bt, GTK_ALIGN_END);
    g_signal_connect(sw_bt, "state-set", G_CALLBACK(al_mover_bt), NULL);
    gtk_box_append(GTK_BOX(tarjeta_bt), sw_bt);
    g_barra.interruptor_bt = sw_bt;
    int bt = od_bluetooth_activado();
    if (bt < 0) gtk_widget_set_sensitive(sw_bt, FALSE);
    else gtk_switch_set_state(GTK_SWITCH(sw_bt), bt == 1);

    gtk_box_append(GTK_BOX(fila_redes), tarjeta_wifi);
    gtk_box_append(GTK_BOX(fila_redes), tarjeta_bt);
    gtk_box_append(GTK_BOX(caja), fila_redes);

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
    g_barra.control_brillo = r_brillo;

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
    g_barra.control_volumen = r_vol;

    return caja;
}

#if HAVE_X11
static GdkRectangle geometria_monitor_principal(void)
{
    GdkRectangle geo = {0, 0, 1280, 800};
    GListModel *monitores = gdk_display_get_monitors(gdk_display_get_default());
    if (g_list_model_get_n_items(monitores) > 0) {
        GdkMonitor *mon = g_list_model_get_item(monitores, 0);
        gdk_monitor_get_geometry(mon, &geo);
        g_object_unref(mon);
    }
    return geo;
}

/* Antes de mapear: el gestor de ventanas lee el tipo y el strut al mapear
 * la ventana (si se ponen después, openbox/xfwm4 la tratan como normal). */
static void al_realizar_x11(GtkWidget *win, gpointer datos)
{
    int alto = GPOINTER_TO_INT(datos);
    GdkSurface *surface = gtk_native_get_surface(gtk_widget_get_native(win));
    if (!GDK_IS_X11_SURFACE(surface)) return;
    Display *xdisplay = od_x11_display(surface);
    Window xid = od_x11_ventana(surface);
    GdkRectangle geo = geometria_monitor_principal();

    Atom tipo = XInternAtom(xdisplay, "_NET_WM_WINDOW_TYPE", False);
    Atom dock = XInternAtom(xdisplay, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(xdisplay, xid, tipo, XA_ATOM, 32, PropModeReplace, (unsigned char *)&dock, 1);

    /* _NET_WM_STRUT_PARTIAL: left,right,top,bottom, y los rangos y1/y2,x1/x2
     * de cada borde reservado. Sólo reservamos arriba. */
    long strut[12] = { 0 };
    strut[2] = alto;                     /* top */
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
static void colocar_x11(GtkWidget *win, int alto)
{
    GdkSurface *surface = gtk_native_get_surface(gtk_widget_get_native(win));
    if (!GDK_IS_X11_SURFACE(surface)) return;
    GdkRectangle geo = geometria_monitor_principal();
    XMoveResizeWindow(od_x11_display(surface), od_x11_ventana(surface),
        geo.x, geo.y, geo.width, alto);
}
#endif

void od_bar_iniciar(OdConfig *cfg, OdBackendTipo backend)
{
    if (g_barra.iniciada) return;
    g_barra.iniciada = TRUE;
    g_barra.alto = cfg->alto_barra;
    g_barra.backend = backend;

    GtkWidget *win = gtk_window_new();
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    /* El título no se ve (ni en layer-shell ni sin decoración en X11); nos
     * sirve sólo para encontrar la ventana desde fuera (pruebas con xprop). */
    gtk_window_set_title(GTK_WINDOW(win), "OpenDock-Barra");
    gtk_widget_add_css_class(win, "opendock-barra");

    GtkWidget *centro = gtk_center_box_new();
    gtk_widget_set_size_request(centro, -1, cfg->alto_barra);

    GtkWidget *caja_izq = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(caja_izq, 16);
    GtkWidget *img_app = gtk_image_new_from_icon_name("application-x-executable-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(img_app), 13);
    GtkWidget *lbl_app = gtk_label_new("Escritorio");
    gtk_widget_add_css_class(lbl_app, "opendock-app-activa");
    gtk_box_append(GTK_BOX(caja_izq), img_app);
    gtk_box_append(GTK_BOX(caja_izq), lbl_app);
    gtk_center_box_set_start_widget(GTK_CENTER_BOX(centro), caja_izq);

    GtkWidget *caja_der = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_end(caja_der, 16);
    gtk_widget_set_halign(caja_der, GTK_ALIGN_END);

    GtkWidget *img_volumen = gtk_image_new_from_icon_name("audio-volume-high-symbolic");
    GtkWidget *img_wifi = gtk_image_new_from_icon_name("network-wireless-symbolic");
    GtkWidget *img_bateria = gtk_image_new_from_icon_name("battery-full-symbolic");
    GtkWidget *lbl_bateria = gtk_label_new("");
    GtkWidget *lbl_reloj = gtk_label_new("");
    gtk_widget_add_css_class(lbl_reloj, "opendock-reloj");

    GtkWidget *boton_ajustes = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(boton_ajustes), "emblem-system-symbolic");
    GtkWidget *popover = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(popover), crear_centro_control());
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(boton_ajustes), popover);

    gtk_box_append(GTK_BOX(caja_der), img_volumen);
    gtk_box_append(GTK_BOX(caja_der), img_wifi);
    gtk_box_append(GTK_BOX(caja_der), img_bateria);
    gtk_box_append(GTK_BOX(caja_der), lbl_bateria);
    gtk_box_append(GTK_BOX(caja_der), lbl_reloj);
    gtk_box_append(GTK_BOX(caja_der), boton_ajustes);
    gtk_center_box_set_end_widget(GTK_CENTER_BOX(centro), caja_der);

    gtk_window_set_child(GTK_WINDOW(win), centro);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window.opendock-barra { background-color: #1C1C1E; }"
        ".opendock-app-activa { color: #FFFFFF; font-weight: 600; font-size: 13px; }"
        ".opendock-reloj { color: #FFFFFF; font-weight: 600; font-size: 13px; }"
        ".opendock-tarjeta { background-color: rgba(255,255,255,0.08); border-radius: 12px; padding: 8px; }"
        );
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(win),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    g_barra.ventana = win;
    g_barra.lbl_app = lbl_app;
    g_barra.img_app = img_app;
    g_barra.lbl_reloj = lbl_reloj;
    g_barra.img_wifi = img_wifi;
    g_barra.img_bateria = img_bateria;
    g_barra.lbl_bateria = lbl_bateria;
    g_barra.img_volumen = img_volumen;

#if HAVE_LAYER_SHELL
    if (backend == OD_BACKEND_WAYLAND && gtk_layer_is_supported()) {
        gtk_layer_init_for_window(GTK_WINDOW(win));
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
    if (backend == OD_BACKEND_X11) {
        GdkRectangle geo = geometria_monitor_principal();
        /* En X11 fijamos el tamaño; en Wayland lo da el compositor (ancla
         * izquierda + derecha), por eso la ventana no puede ser fija allí. */
        gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
        gtk_widget_set_size_request(win, geo.width, cfg->alto_barra);
        g_signal_connect(win, "realize", G_CALLBACK(al_realizar_x11),
            GINT_TO_POINTER(cfg->alto_barra));
    }
#endif

    gtk_widget_set_visible(win, TRUE);

#if HAVE_X11
    if (backend == OD_BACKEND_X11) colocar_x11(win, cfg->alto_barra);
#endif

    od_volumen_iniciar();
    od_volumen_conectar_cambio(al_cambiar_volumen, NULL);

    al_tic_reloj(NULL);
    g_timeout_add_seconds(1, al_tic_reloj, NULL);
    al_tic_estado(NULL);
    g_timeout_add_seconds(15, al_tic_estado, NULL);
    refrescar_volumen();
}

void od_bar_set_app_activa(const char *nombre, const char *icono_nombre)
{
    if (!g_barra.iniciada) return;
    gtk_label_set_text(GTK_LABEL(g_barra.lbl_app), nombre && *nombre ? nombre : "Escritorio");
    gtk_image_set_from_icon_name(GTK_IMAGE(g_barra.img_app),
        (icono_nombre && *icono_nombre) ? icono_nombre : "application-x-executable-symbolic");
}
