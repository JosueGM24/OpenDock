/*
 * bandeja.c — anfitrión (y, si hace falta, vigilante) StatusNotifierItem.
 * Ver bandeja.h.
 *
 * Todo lo que llega de los iconos viene de otros procesos: se acota el
 * tamaño de las imágenes, el número de iconos y de entradas de menú, y la
 * profundidad de los submenús.
 */
#include "bandeja.h"
#include <gio/gio.h>
#include <string.h>
#include <unistd.h>

#define OD_SNW_NOMBRE   "org.kde.StatusNotifierWatcher"
#define OD_SNW_RUTA     "/StatusNotifierWatcher"
#define OD_SNI_IFAZ     "org.kde.StatusNotifierItem"
#define OD_MENU_IFAZ    "com.canonical.dbusmenu"
#define OD_MAX_ITEMS    64
#define OD_MAX_LADO     256
#define OD_MAX_ENTRADAS 200
#define OD_MAX_NIVEL    4
#define OD_TAM_ICONO    22

static const gchar xml_vigilante[] =
    "<node><interface name='org.kde.StatusNotifierWatcher'>"
    "  <method name='RegisterStatusNotifierItem'><arg type='s' name='service' direction='in'/></method>"
    "  <method name='RegisterStatusNotifierHost'><arg type='s' name='service' direction='in'/></method>"
    "  <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
    "  <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    "  <property name='ProtocolVersion' type='i' access='read'/>"
    "  <signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>"
    "  <signal name='StatusNotifierItemUnregistered'><arg type='s'/></signal>"
    "  <signal name='StatusNotifierHostRegistered'/>"
    "  <signal name='StatusNotifierHostUnregistered'/>"
    "</interface></node>";

typedef struct {
    gchar *clave;          /* "bus/ruta" */
    gchar *bus;
    gchar *ruta;
    gchar *titulo;
    gchar *estado;         /* Passive / Active / NeedsAttention */
    gchar *icono_nombre;
    gchar *ruta_tema;
    gchar *menu;           /* ruta del objeto dbusmenu, o NULL */
    gboolean es_menu;
    GdkTexture *pixmap;
    guint senales;
    guint vigilancia;      /* sólo como vigilante: si el dueño desaparece */
    GtkWidget *boton;
} OdIcono;

static struct {
    GDBusConnection *bus;
    gboolean vigilante;
    gboolean anfitrion_registrado;
    guint id_objeto;
    guint senal_alta, senal_baja;
    guint vigilancia_vigilante;
    GPtrArray *iconos;     /* OdIcono* */
    GtkWidget *chevron;
    GtkWidget *caja;
} g_b;

/* ---- utilidades ---------------------------------------------------------- */

static OdIcono *buscar(const char *clave)
{
    for (guint i = 0; g_b.iconos && i < g_b.iconos->len; i++) {
        OdIcono *ic = g_ptr_array_index(g_b.iconos, i);
        if (g_strcmp0(ic->clave, clave) == 0) return ic;
    }
    return NULL;
}

/* "bus/ruta", "bus" (ruta por defecto) o "/ruta" (bus = quien llama). */
static gboolean partir_servicio(const char *servicio, const char *remitente, gchar **bus, gchar **ruta)
{
    if (!servicio || !*servicio) return FALSE;
    if (servicio[0] == '/') {
        if (!remitente || !g_variant_is_object_path(servicio)) return FALSE;
        *bus = g_strdup(remitente);
        *ruta = g_strdup(servicio);
        return TRUE;
    }
    const char *barra = strchr(servicio, '/');
    *bus = barra ? g_strndup(servicio, barra - servicio) : g_strdup(servicio);
    *ruta = g_strdup(barra ? barra : "/StatusNotifierItem");
    if (!g_dbus_is_name(*bus) || !g_variant_is_object_path(*ruta)) {
        g_free(*bus); g_free(*ruta);
        return FALSE;
    }
    return TRUE;
}

static void actualizar_chevron(void)
{
    gboolean alguno = FALSE;
    for (guint i = 0; i < g_b.iconos->len; i++) {
        OdIcono *ic = g_ptr_array_index(g_b.iconos, i);
        if (ic->boton && gtk_widget_get_visible(ic->boton)) alguno = TRUE;
    }
    gtk_widget_set_visible(g_b.chevron, alguno);
}

/* ---- imagen del icono ---------------------------------------------------- */

/* IconPixmap: a(iiay) en ARGB32 big-endian. Nos quedamos con el más grande
 * que no pase de OD_MAX_LADO y lo pasamos a RGBA. */
static GdkTexture *leer_pixmap(GVariant *v)
{
    if (!v || !g_variant_is_of_type(v, G_VARIANT_TYPE("a(iiay)"))) return NULL;
    GVariantIter it;
    g_variant_iter_init(&it, v);
    gint32 w, h, mejor_w = 0, mejor_h = 0;
    GVariant *datos, *mejor = NULL;
    while (g_variant_iter_next(&it, "(ii@ay)", &w, &h, &datos)) {
        gsize n = g_variant_get_size(datos);
        if (w > 0 && h > 0 && w <= OD_MAX_LADO && h <= OD_MAX_LADO &&
            (gsize)w * h * 4 == n && w * h > mejor_w * mejor_h) {
            if (mejor) g_variant_unref(mejor);
            mejor = datos; mejor_w = w; mejor_h = h;
        } else {
            g_variant_unref(datos);
        }
    }
    if (!mejor) return NULL;
    gsize n;
    const guint8 *src = g_variant_get_fixed_array(mejor, &n, 1);
    guint8 *rgba = g_malloc(n);
    for (gsize i = 0; i + 3 < n; i += 4) {
        rgba[i] = src[i + 1];
        rgba[i + 1] = src[i + 2];
        rgba[i + 2] = src[i + 3];
        rgba[i + 3] = src[i];
    }
    GBytes *bytes = g_bytes_new_take(rgba, n);
    GdkTexture *t = gdk_memory_texture_new(mejor_w, mejor_h, GDK_MEMORY_R8G8B8A8, bytes,
        (gsize)mejor_w * 4);
    g_bytes_unref(bytes);
    g_variant_unref(mejor);
    return t;
}

static void poner_imagen(OdIcono *ic, GtkWidget *img)
{
    if (ic->icono_nombre && *ic->icono_nombre) {
        /* Algunas apps traen sus iconos en IconThemePath (plano o como tema). */
        if (ic->ruta_tema && *ic->ruta_tema && !strchr(ic->icono_nombre, '/')) {
            static const char *ext[] = { ".png", ".svg" };
            for (gsize i = 0; i < G_N_ELEMENTS(ext); i++) {
                gchar *f = g_strconcat(ic->ruta_tema, "/", ic->icono_nombre, ext[i], NULL);
                gboolean hay = g_file_test(f, G_FILE_TEST_IS_REGULAR);
                if (hay) gtk_image_set_from_file(GTK_IMAGE(img), f);
                g_free(f);
                if (hay) return;
            }
            GtkIconTheme *tema = gtk_icon_theme_new();
            const char *rutas[] = { ic->ruta_tema, NULL };
            gtk_icon_theme_set_search_path(tema, rutas);
            if (gtk_icon_theme_has_icon(tema, ic->icono_nombre)) {
                GtkIconPaintable *p = gtk_icon_theme_lookup_icon(tema, ic->icono_nombre, NULL,
                    OD_TAM_ICONO, gtk_widget_get_scale_factor(img), GTK_TEXT_DIR_NONE, 0);
                gtk_image_set_from_paintable(GTK_IMAGE(img), GDK_PAINTABLE(p));
                g_object_unref(p);
                g_object_unref(tema);
                return;
            }
            g_object_unref(tema);
        }
        if (g_path_is_absolute(ic->icono_nombre) && g_file_test(ic->icono_nombre, G_FILE_TEST_IS_REGULAR)) {
            gtk_image_set_from_file(GTK_IMAGE(img), ic->icono_nombre);
            return;
        }
        GtkIconTheme *tema = gtk_icon_theme_get_for_display(gtk_widget_get_display(img));
        if (gtk_icon_theme_has_icon(tema, ic->icono_nombre)) {
            gtk_image_set_from_icon_name(GTK_IMAGE(img), ic->icono_nombre);
            return;
        }
    }
    if (ic->pixmap) gtk_image_set_from_paintable(GTK_IMAGE(img), GDK_PAINTABLE(ic->pixmap));
    else gtk_image_set_from_icon_name(GTK_IMAGE(img), "application-x-executable-symbolic");
}

/* ---- menú (com.canonical.dbusmenu) --------------------------------------- */

/* "_Abrir" -> "Abrir", "a__b" -> "a_b" (mnemónicos de GTK2/Qt). */
static gchar *sin_mnemonico(const char *s)
{
    GString *r = g_string_new(NULL);
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '_') {
            if (p[1] == '_') { g_string_append_c(r, '_'); p++; }
            continue;
        }
        g_string_append_c(r, *p);
    }
    return g_string_free(r, FALSE);
}

static void llenar_menu(GMenu *menu, GVariant *hijos, int nivel, int *cuenta)
{
    GMenu *seccion = g_menu_new();
    gsize n = g_variant_n_children(hijos);
    for (gsize i = 0; i < n && *cuenta < OD_MAX_ENTRADAS; i++) {
        GVariant *envuelto = g_variant_get_child_value(hijos, i);
        GVariant *e = g_variant_get_variant(envuelto);
        g_variant_unref(envuelto);
        if (!g_variant_is_of_type(e, G_VARIANT_TYPE("(ia{sv}av)"))) { g_variant_unref(e); continue; }
        gint32 id;
        GVariant *props, *sub;
        g_variant_get(e, "(i@a{sv}@av)", &id, &props, &sub);
        gboolean visible = TRUE, activo = TRUE;
        const char *tipo = NULL, *etiqueta = NULL, *conmutador = NULL, *hijos_disp = NULL;
        gint32 estado = -1;
        g_variant_lookup(props, "visible", "b", &visible);
        g_variant_lookup(props, "enabled", "b", &activo);
        g_variant_lookup(props, "type", "&s", &tipo);
        g_variant_lookup(props, "label", "&s", &etiqueta);
        g_variant_lookup(props, "toggle-type", "&s", &conmutador);
        g_variant_lookup(props, "toggle-state", "i", &estado);
        g_variant_lookup(props, "children-display", "&s", &hijos_disp);
        (*cuenta)++;
        if (!visible) {
            /* nada */
        } else if (g_strcmp0(tipo, "separator") == 0) {
            if (g_menu_model_get_n_items(G_MENU_MODEL(seccion)) > 0) {
                g_menu_append_section(menu, NULL, G_MENU_MODEL(seccion));
                g_object_unref(seccion);
                seccion = g_menu_new();
            }
        } else {
            gchar *limpia = sin_mnemonico(etiqueta);
            gchar *texto = limpia;
            if (conmutador && estado == 1)
                texto = g_strconcat(g_strcmp0(conmutador, "radio") == 0 ? "● " : "✓ ", limpia, NULL);
            if ((g_strcmp0(hijos_disp, "submenu") == 0 || g_variant_n_children(sub) > 0) &&
                nivel < OD_MAX_NIVEL) {
                GMenu *submenu = g_menu_new();
                llenar_menu(submenu, sub, nivel + 1, cuenta);
                g_menu_append_submenu(seccion, texto, G_MENU_MODEL(submenu));
                g_object_unref(submenu);
            } else {
                GMenuItem *mi = g_menu_item_new(texto, NULL);
                /* Sin acción el elemento sale desactivado. */
                if (activo) g_menu_item_set_action_and_target_value(mi, "bandeja.evento",
                    g_variant_new_int32(id));
                g_menu_append_item(seccion, mi);
                g_object_unref(mi);
            }
            if (texto != limpia) g_free(texto);
            g_free(limpia);
        }
        g_variant_unref(props);
        g_variant_unref(sub);
        g_variant_unref(e);
    }
    if (g_menu_model_get_n_items(G_MENU_MODEL(seccion)) > 0)
        g_menu_append_section(menu, NULL, G_MENU_MODEL(seccion));
    g_object_unref(seccion);
}

static void al_recibir_menu(GObject *origen, GAsyncResult *res, gpointer datos)
{
    gchar *clave = datos;
    GError *error = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(origen), res, &error);
    OdIcono *ic = buscar(clave);
    g_free(clave);
    if (!r) {
        g_message("opendock: menú de la bandeja no disponible: %s", error ? error->message : "?");
        g_clear_error(&error);
        return;
    }
    if (ic && ic->boton) {
        GVariant *disposicion = g_variant_get_child_value(r, 1);
        GVariant *hijos = g_variant_get_child_value(disposicion, 2);
        GMenu *menu = g_menu_new();
        int cuenta = 0;
        llenar_menu(menu, hijos, 0, &cuenta);
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(ic->boton), G_MENU_MODEL(menu));
        gtk_menu_button_popup(GTK_MENU_BUTTON(ic->boton));
        g_object_unref(menu);
        g_variant_unref(hijos);
        g_variant_unref(disposicion);
    }
    g_variant_unref(r);
}

static void abrir_menu(OdIcono *ic)
{
    if (!ic->menu) {
        g_dbus_connection_call(g_b.bus, ic->bus, ic->ruta, OD_SNI_IFAZ, "ContextMenu",
            g_variant_new("(ii)", 0, 0), NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL, NULL);
        return;
    }
    g_dbus_connection_call(g_b.bus, ic->bus, ic->menu, OD_MENU_IFAZ, "AboutToShow",
        g_variant_new("(i)", 0), NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL, NULL);
    const char *ninguna[] = { NULL };
    g_dbus_connection_call(g_b.bus, ic->bus, ic->menu, OD_MENU_IFAZ, "GetLayout",
        g_variant_new("(ii^as)", 0, -1, ninguna), G_VARIANT_TYPE("(u(ia{sv}av))"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, al_recibir_menu, g_strdup(ic->clave));
}

static void al_evento_menu(GSimpleAction *a, GVariant *p, gpointer datos)
{
    (void)a;
    OdIcono *ic = buscar(datos);
    if (!ic || !ic->menu) return;
    g_dbus_connection_call(g_b.bus, ic->bus, ic->menu, OD_MENU_IFAZ, "Event",
        g_variant_new("(isvu)", g_variant_get_int32(p), "clicked", g_variant_new_int32(0),
            (guint32)(g_get_real_time() / 1000)),
        NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL, NULL);
    gtk_menu_button_popdown(GTK_MENU_BUTTON(g_b.chevron));
}

/* ---- botones ------------------------------------------------------------- */

static void al_pulsar_icono(GtkGestureClick *g, int n, double x, double y, gpointer datos)
{
    (void)n; (void)x; (void)y;
    OdIcono *ic = buscar(datos);
    if (!ic) return;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    guint boton = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    if (boton == GDK_BUTTON_SECONDARY || (boton == GDK_BUTTON_PRIMARY && ic->es_menu)) {
        abrir_menu(ic);
    } else {
        g_dbus_connection_call(g_b.bus, ic->bus, ic->ruta, OD_SNI_IFAZ,
            boton == GDK_BUTTON_MIDDLE ? "SecondaryActivate" : "Activate",
            g_variant_new("(ii)", 0, 0), NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL, NULL);
        gtk_menu_button_popdown(GTK_MENU_BUTTON(g_b.chevron));
    }
}

static void crear_boton(OdIcono *ic)
{
    GtkWidget *b = gtk_menu_button_new();
    gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(b), FALSE);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_name(b, "opendock-bandeja-icono");
    GtkWidget *img = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(img), OD_TAM_ICONO);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(b), img);

    GSimpleActionGroup *grupo = g_simple_action_group_new();
    GSimpleAction *evento = g_simple_action_new("evento", G_VARIANT_TYPE_INT32);
    gchar *clave = g_strdup(ic->clave);
    g_signal_connect_data(evento, "activate", G_CALLBACK(al_evento_menu), clave,
        (GClosureNotify)(void (*)(void))g_free, 0);
    g_action_map_add_action(G_ACTION_MAP(grupo), G_ACTION(evento));
    g_object_unref(evento);
    gtk_widget_insert_action_group(b, "bandeja", G_ACTION_GROUP(grupo));
    g_object_unref(grupo);

    GtkGesture *clic = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(clic), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(clic), GTK_PHASE_CAPTURE);
    g_signal_connect_data(clic, "pressed", G_CALLBACK(al_pulsar_icono), g_strdup(ic->clave),
        (GClosureNotify)(void (*)(void))g_free, 0);
    gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(clic));

    gtk_widget_set_visible(b, FALSE);   /* hasta saber su estado */
    gtk_box_append(GTK_BOX(g_b.caja), b);
    ic->boton = b;
}

static void pintar(OdIcono *ic)
{
    if (!ic->boton) return;
    poner_imagen(ic, gtk_menu_button_get_child(GTK_MENU_BUTTON(ic->boton)));
    gtk_widget_set_tooltip_text(ic->boton, ic->titulo && *ic->titulo ? ic->titulo : NULL);
    gtk_widget_set_visible(ic->boton, g_strcmp0(ic->estado, "Passive") != 0);
    actualizar_chevron();
}

/* ---- propiedades del icono ----------------------------------------------- */

static gchar *cadena(GVariant *d, const char *k)
{
    const char *s = NULL;
    if (!g_variant_lookup(d, k, "&s", &s) && !g_variant_lookup(d, k, "&o", &s)) return NULL;
    return g_utf8_validate(s, -1, NULL) ? g_strndup(s, 1024) : NULL;
}

static void al_recibir_propiedades(GObject *origen, GAsyncResult *res, gpointer datos)
{
    gchar *clave = datos;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(origen), res, NULL);
    OdIcono *ic = buscar(clave);
    g_free(clave);
    if (!r) return;
    if (ic) {
        GVariant *d = g_variant_get_child_value(r, 0);
        g_free(ic->titulo); ic->titulo = cadena(d, "Title");
        g_free(ic->estado); ic->estado = cadena(d, "Status");
        g_free(ic->icono_nombre);
        ic->icono_nombre = g_strcmp0(ic->estado, "NeedsAttention") == 0 ? cadena(d, "AttentionIconName") : NULL;
        if (!ic->icono_nombre || !*ic->icono_nombre) { g_free(ic->icono_nombre); ic->icono_nombre = cadena(d, "IconName"); }
        g_free(ic->ruta_tema); ic->ruta_tema = cadena(d, "IconThemePath");
        g_free(ic->menu); ic->menu = cadena(d, "Menu");
        if (ic->menu && (!g_variant_is_object_path(ic->menu) || g_strcmp0(ic->menu, "/") == 0)) {
            g_free(ic->menu);
            ic->menu = NULL;
        }
        ic->es_menu = FALSE;
        g_variant_lookup(d, "ItemIsMenu", "b", &ic->es_menu);
        GVariant *px = g_variant_lookup_value(d, "IconPixmap", G_VARIANT_TYPE("a(iiay)"));
        g_clear_object(&ic->pixmap);
        ic->pixmap = leer_pixmap(px);
        if (px) g_variant_unref(px);
        /* ToolTip (sa(iiay)ss): su título, si no hay Title. */
        GVariant *tt = g_variant_lookup_value(d, "ToolTip", G_VARIANT_TYPE("(sa(iiay)ss)"));
        if (tt) {
            const char *t = NULL;
            g_variant_get_child(tt, 2, "&s", &t);
            if (t && *t && (!ic->titulo || !*ic->titulo) && g_utf8_validate(t, -1, NULL)) {
                g_free(ic->titulo);
                ic->titulo = g_strndup(t, 1024);
            }
            g_variant_unref(tt);
        }
        g_variant_unref(d);
        pintar(ic);
    }
    g_variant_unref(r);
}

static void refrescar(OdIcono *ic)
{
    g_dbus_connection_call(g_b.bus, ic->bus, ic->ruta, "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new("(s)", OD_SNI_IFAZ), G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE,
        2000, NULL, al_recibir_propiedades, g_strdup(ic->clave));
}

static void al_senal_icono(GDBusConnection *c, const gchar *rem, const gchar *ruta,
    const gchar *ifaz, const gchar *senal, GVariant *p, gpointer datos)
{
    (void)c; (void)rem; (void)ruta; (void)ifaz; (void)senal; (void)p;
    OdIcono *ic = buscar(datos);
    if (ic) refrescar(ic);   /* NewIcon, NewStatus, NewTitle, NewToolTip... */
}

/* ---- alta y baja --------------------------------------------------------- */

static void emitir(const char *senal, GVariant *p)
{
    if (!g_b.vigilante) {
        if (p) g_variant_unref(g_variant_ref_sink(p));
        return;
    }
    g_dbus_connection_emit_signal(g_b.bus, NULL, OD_SNW_RUTA, OD_SNW_NOMBRE, senal, p, NULL);
}

static void liberar_icono(gpointer p)
{
    OdIcono *ic = p;
    if (ic->senales) g_dbus_connection_signal_unsubscribe(g_b.bus, ic->senales);
    if (ic->vigilancia) g_bus_unwatch_name(ic->vigilancia);
    if (ic->boton) gtk_box_remove(GTK_BOX(g_b.caja), ic->boton);
    g_clear_object(&ic->pixmap);
    g_free(ic->clave); g_free(ic->bus); g_free(ic->ruta);
    g_free(ic->titulo); g_free(ic->estado); g_free(ic->icono_nombre);
    g_free(ic->ruta_tema); g_free(ic->menu);
    g_free(ic);
}

static void quitar(const char *clave)
{
    OdIcono *ic = buscar(clave);
    if (!ic) return;
    gchar *copia = g_strdup(clave);
    g_ptr_array_remove(g_b.iconos, ic);
    emitir("StatusNotifierItemUnregistered", g_variant_new("(s)", copia));
    g_free(copia);
    actualizar_chevron();
}

static void al_desaparecer_dueno(GDBusConnection *c, const gchar *nombre, gpointer datos)
{
    (void)c; (void)nombre;
    quitar(datos);
}

static void agregar(const char *bus, const char *ruta)
{
    gchar *clave = g_strconcat(bus, ruta, NULL);
    if (buscar(clave) || g_b.iconos->len >= OD_MAX_ITEMS) {
        g_free(clave);
        return;
    }
    OdIcono *ic = g_new0(OdIcono, 1);
    ic->clave = clave;
    ic->bus = g_strdup(bus);
    ic->ruta = g_strdup(ruta);
    g_ptr_array_add(g_b.iconos, ic);
    ic->senales = g_dbus_connection_signal_subscribe(g_b.bus, bus, OD_SNI_IFAZ, NULL, ruta, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, al_senal_icono, g_strdup(clave), g_free);
    if (g_b.vigilante)
        ic->vigilancia = g_bus_watch_name_on_connection(g_b.bus, bus, G_BUS_NAME_WATCHER_FLAGS_NONE,
            NULL, al_desaparecer_dueno, g_strdup(clave), g_free);
    crear_boton(ic);
    refrescar(ic);
    emitir("StatusNotifierItemRegistered", g_variant_new("(s)", clave));
}

/* ---- como vigilante ------------------------------------------------------ */

static void al_llamar_vigilante(GDBusConnection *c, const gchar *remitente, const gchar *ruta,
    const gchar *ifaz, const gchar *metodo, GVariant *p, GDBusMethodInvocation *inv, gpointer datos)
{
    (void)c; (void)ruta; (void)ifaz; (void)datos;
    if (g_strcmp0(metodo, "RegisterStatusNotifierItem") == 0) {
        const char *servicio;
        g_variant_get(p, "(&s)", &servicio);
        gchar *bus = NULL, *ruta_item = NULL;
        if (!partir_servicio(servicio, remitente, &bus, &ruta_item)) {
            g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                "Servicio no válido: %s", servicio);
            return;
        }
        agregar(bus, ruta_item);
        g_free(bus);
        g_free(ruta_item);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (g_strcmp0(metodo, "RegisterStatusNotifierHost") == 0) {
        g_dbus_method_invocation_return_value(inv, NULL);
        emitir("StatusNotifierHostRegistered", NULL);
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
            "Método desconocido: %s", metodo);
    }
}

static GVariant *al_leer_vigilante(GDBusConnection *c, const gchar *rem, const gchar *ruta,
    const gchar *ifaz, const gchar *prop, GError **error, gpointer datos)
{
    (void)c; (void)rem; (void)ruta; (void)ifaz; (void)error; (void)datos;
    if (g_strcmp0(prop, "RegisteredStatusNotifierItems") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        for (guint i = 0; i < g_b.iconos->len; i++)
            g_variant_builder_add(&b, "s", ((OdIcono *)g_ptr_array_index(g_b.iconos, i))->clave);
        return g_variant_builder_end(&b);
    }
    if (g_strcmp0(prop, "IsStatusNotifierHostRegistered") == 0) return g_variant_new_boolean(TRUE);
    if (g_strcmp0(prop, "ProtocolVersion") == 0) return g_variant_new_int32(0);
    return NULL;
}

static const GDBusInterfaceVTable vtabla_vigilante = {
    .method_call = al_llamar_vigilante,
    .get_property = al_leer_vigilante,
};

/* ---- como anfitrión de otro vigilante ------------------------------------ */

static void al_alta_ajena(GDBusConnection *c, const gchar *rem, const gchar *ruta,
    const gchar *ifaz, const gchar *senal, GVariant *p, gpointer datos)
{
    (void)c; (void)rem; (void)ruta; (void)ifaz; (void)datos;
    const char *servicio;
    g_variant_get(p, "(&s)", &servicio);
    gchar *bus = NULL, *ruta_item = NULL;
    if (g_strcmp0(senal, "StatusNotifierItemRegistered") == 0) {
        if (partir_servicio(servicio, NULL, &bus, &ruta_item)) agregar(bus, ruta_item);
    } else if (partir_servicio(servicio, NULL, &bus, &ruta_item)) {
        gchar *clave = g_strconcat(bus, ruta_item, NULL);
        quitar(clave);
        g_free(clave);
    }
    g_free(bus);
    g_free(ruta_item);
}

static void al_recibir_lista(GObject *origen, GAsyncResult *res, gpointer datos)
{
    (void)datos;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(origen), res, NULL);
    if (!r) return;
    GVariant *v = g_variant_get_child_value(r, 0);
    GVariant *lista = g_variant_get_variant(v);
    if (g_variant_is_of_type(lista, G_VARIANT_TYPE("as"))) {
        GVariantIter it;
        const char *servicio;
        g_variant_iter_init(&it, lista);
        while (g_variant_iter_next(&it, "&s", &servicio)) {
            gchar *bus = NULL, *ruta_item = NULL;
            if (partir_servicio(servicio, NULL, &bus, &ruta_item)) agregar(bus, ruta_item);
            g_free(bus);
            g_free(ruta_item);
        }
    }
    g_variant_unref(lista);
    g_variant_unref(v);
    g_variant_unref(r);
}

static void hacerse_anfitrion(void)
{
    if (g_b.anfitrion_registrado) return;
    g_b.anfitrion_registrado = TRUE;
    gchar *nombre = g_strdup_printf("org.kde.StatusNotifierHost-%d", (int)getpid());
    g_bus_own_name_on_connection(g_b.bus, nombre, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
    g_b.senal_alta = g_dbus_connection_signal_subscribe(g_b.bus, OD_SNW_NOMBRE, OD_SNW_NOMBRE,
        "StatusNotifierItemRegistered", OD_SNW_RUTA, NULL, G_DBUS_SIGNAL_FLAGS_NONE, al_alta_ajena, NULL, NULL);
    g_b.senal_baja = g_dbus_connection_signal_subscribe(g_b.bus, OD_SNW_NOMBRE, OD_SNW_NOMBRE,
        "StatusNotifierItemUnregistered", OD_SNW_RUTA, NULL, G_DBUS_SIGNAL_FLAGS_NONE, al_alta_ajena, NULL, NULL);
    g_dbus_connection_call(g_b.bus, OD_SNW_NOMBRE, OD_SNW_RUTA, OD_SNW_NOMBRE,
        "RegisterStatusNotifierHost", g_variant_new("(s)", nombre), NULL, G_DBUS_CALL_FLAGS_NONE,
        2000, NULL, NULL, NULL);
    g_dbus_connection_call(g_b.bus, OD_SNW_NOMBRE, OD_SNW_RUTA, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", OD_SNW_NOMBRE, "RegisteredStatusNotifierItems"), G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, al_recibir_lista, NULL);
    g_message("opendock: ya hay un %s; la bandeja se registra en él como anfitrión", OD_SNW_NOMBRE);
    g_free(nombre);
}

static void al_adquirir_vigilante(GDBusConnection *c, const gchar *nombre, gpointer datos)
{
    (void)nombre; (void)datos;
    GDBusNodeInfo *nodo = g_dbus_node_info_new_for_xml(xml_vigilante, NULL);
    g_b.id_objeto = g_dbus_connection_register_object(c, OD_SNW_RUTA, nodo->interfaces[0],
        &vtabla_vigilante, NULL, NULL, NULL);
    g_dbus_node_info_unref(nodo);
    g_b.vigilante = TRUE;
    g_message("opendock: bandeja activa (%s)", OD_SNW_NOMBRE);
}

static void al_perder_vigilante(GDBusConnection *c, const gchar *nombre, gpointer datos)
{
    (void)c; (void)nombre; (void)datos;
    if (!g_b.vigilante) hacerse_anfitrion();
}

static void al_obtener_bus(GObject *o, GAsyncResult *res, gpointer datos)
{
    (void)o; (void)datos;
    g_b.bus = g_bus_get_finish(res, NULL);
    if (!g_b.bus) return;
    g_bus_own_name_on_connection(g_b.bus, OD_SNW_NOMBRE, G_BUS_NAME_OWNER_FLAGS_NONE,
        al_adquirir_vigilante, al_perder_vigilante, NULL, NULL);
}

void od_bandeja_alternar(void)
{
    if (!g_b.chevron || !gtk_widget_get_visible(g_b.chevron)) return;
    GtkPopover *p = gtk_menu_button_get_popover(GTK_MENU_BUTTON(g_b.chevron));
    if (p && gtk_widget_get_visible(GTK_WIDGET(p))) gtk_menu_button_popdown(GTK_MENU_BUTTON(g_b.chevron));
    else gtk_menu_button_popup(GTK_MENU_BUTTON(g_b.chevron));
}

GtkWidget *od_bandeja_crear_boton(void)
{
    g_b.iconos = g_ptr_array_new_with_free_func(liberar_icono);

    g_b.chevron = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(g_b.chevron), "pan-down-symbolic");
    gtk_widget_set_tooltip_text(g_b.chevron, "Bandeja");
    gtk_widget_add_css_class(g_b.chevron, "opendock-icono");
    GtkWidget *popover = gtk_popover_new();
    g_b.caja = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_popover_set_child(GTK_POPOVER(popover), g_b.caja);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(g_b.chevron), popover);
    gtk_widget_set_visible(g_b.chevron, FALSE);

    g_bus_get(G_BUS_TYPE_SESSION, NULL, al_obtener_bus, NULL);
    return g_b.chevron;
}
