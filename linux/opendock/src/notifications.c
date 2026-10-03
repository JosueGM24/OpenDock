/*
 * Servidor org.freedesktop.Notifications (spec 1.2). Recibe Notify() por
 * D-Bus, valida/acota lo que llega de fuera (nunca hay que fiarse de un
 * cliente ajeno) y pasa el resultado al notch para que lo muestre.
 */
#include "notifications.h"
#include "notch.h"
#include "opendock-build-config.h"
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>

#define OD_MAX_CADENA      4096      /* límite para app_name/summary/body/etc. */
#define OD_MAX_IMG_LADO    512       /* ancho/alto máximo de image-data */
#define OD_MAX_IMG_BYTES   (4 * 1024 * 1024)

static const gchar introspeccion_xml[] =
    "<node>"
    "  <interface name='org.freedesktop.Notifications'>"
    "    <method name='Notify'>"
    "      <arg type='s' name='app_name' direction='in'/>"
    "      <arg type='u' name='replaces_id' direction='in'/>"
    "      <arg type='s' name='app_icon' direction='in'/>"
    "      <arg type='s' name='summary' direction='in'/>"
    "      <arg type='s' name='body' direction='in'/>"
    "      <arg type='as' name='actions' direction='in'/>"
    "      <arg type='a{sv}' name='hints' direction='in'/>"
    "      <arg type='i' name='expire_timeout' direction='in'/>"
    "      <arg type='u' name='id' direction='out'/>"
    "    </method>"
    "    <method name='CloseNotification'>"
    "      <arg type='u' name='id' direction='in'/>"
    "    </method>"
    "    <method name='GetCapabilities'>"
    "      <arg type='as' name='capabilities' direction='out'/>"
    "    </method>"
    "    <method name='GetServerInformation'>"
    "      <arg type='s' name='name' direction='out'/>"
    "      <arg type='s' name='vendor' direction='out'/>"
    "      <arg type='s' name='version' direction='out'/>"
    "      <arg type='s' name='spec_version' direction='out'/>"
    "    </method>"
    "    <signal name='NotificationClosed'>"
    "      <arg type='u' name='id'/>"
    "      <arg type='u' name='reason'/>"
    "    </signal>"
    "    <signal name='ActionInvoked'>"
    "      <arg type='u' name='id'/>"
    "      <arg type='s' name='action_key'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

typedef struct {
    OdConfig *cfg;
    GDBusConnection *bus;
    guint id_registro;
    guint id_propietario;
    guint32 siguiente_id;
    guint32 id_actual;
} OdServidorNotif;

static OdServidorNotif g_srv;

static gchar *limitar_cadena(const gchar *s)
{
    if (!s) return g_strdup("");
    gsize len = strlen(s);
    if (len <= OD_MAX_CADENA) return g_strdup(s);
    const gchar *fin = s + OD_MAX_CADENA;
    while (fin > s && (*fin & 0xC0) == 0x80) fin--;
    return g_strndup(s, (gsize)(fin - s));
}

/* Hint "image-data"/"image_data"/"icon_data": (iiibiiay) ancho, alto,
 * stride, alfa, bits por muestra, canales, datos. Lo acotamos todo porque
 * viene de un proceso cualquiera del sistema. */
static GdkPixbuf *decodificar_image_data(GVariant *v)
{
    if (!v || !g_variant_is_of_type(v, G_VARIANT_TYPE("(iiibiiay)")))
        return NULL;

    gint32 ancho, alto, stride, bps, canales;
    gboolean alfa;
    GVariant *datos_v = NULL;
    g_variant_get(v, "(iiibii@ay)", &ancho, &alto, &stride, &alfa, &bps, &canales, &datos_v);

    GdkPixbuf *resultado = NULL;
    if (ancho > 0 && alto > 0 && ancho <= OD_MAX_IMG_LADO && alto <= OD_MAX_IMG_LADO &&
        bps == 8 && (canales == 3 || canales == 4) &&
        stride >= ancho * canales && (gint64)stride * alto <= OD_MAX_IMG_BYTES) {
        gsize n = 0;
        const guint8 *datos = g_variant_get_fixed_array(datos_v, &n, sizeof(guint8));
        if (datos && (gint64)n >= (gint64)stride * alto) {
            GBytes *bytes = g_bytes_new(datos, (gsize)stride * alto);
            resultado = gdk_pixbuf_new_from_bytes(bytes, GDK_COLORSPACE_RGB,
                alfa, bps, ancho, alto, stride);
            g_bytes_unref(bytes);
        }
    }
    if (datos_v) g_variant_unref(datos_v);
    return resultado;
}

static void manejar_notify(GVariant *parametros, GDBusMethodInvocation *invocacion)
{
    const gchar *app_name_in = NULL, *app_icon_in = NULL, *summary_in = NULL, *body_in = NULL;
    guint32 replaces_id = 0;
    gint32 expire_timeout = -1;
    GVariantIter *iter_acciones = NULL;
    GVariant *hints = NULL;

    g_message("opendock: Notify: desempaquetando parámetros (tipo %s)",
        g_variant_get_type_string(parametros));
    g_variant_get(parametros, "(&su&s&s&sasa{sv}i)",
        &app_name_in, &replaces_id, &app_icon_in, &summary_in, &body_in,
        &iter_acciones, &hints, &expire_timeout);
    g_message("opendock: Notify: parámetros desempaquetados app=%s resumen=%s",
        app_name_in ? app_name_in : "(null)", summary_in ? summary_in : "(null)");
    if (iter_acciones) g_variant_iter_free(iter_acciones);
    (void)expire_timeout; /* la duración del peek la fija el notch (DESIGN.md: 4,5 s) */

    gchar *app_name = limitar_cadena(app_name_in);
    gchar *app_icon = limitar_cadena(app_icon_in);
    gchar *summary = limitar_cadena(summary_in);
    gchar *body = limitar_cadena(body_in);

    GdkPixbuf *pixbuf = NULL;
    if (hints) {
        GVariant *img = g_variant_lookup_value(hints, "image-data", NULL);
        if (!img) img = g_variant_lookup_value(hints, "icon_data", NULL);
        if (img) {
            pixbuf = decodificar_image_data(img);
            g_variant_unref(img);
        }
    }

    guint32 id = replaces_id != 0 ? replaces_id : g_srv.siguiente_id++;
    if (g_srv.siguiente_id == 0) g_srv.siguiente_id = 1; /* por si desborda */
    g_srv.id_actual = id;

    g_message("opendock: Notify: llamando al notch (id=%u)", id);
    od_notch_mostrar_aviso(app_name, summary, body, pixbuf,
        (app_icon && *app_icon) ? app_icon : NULL);
    g_message("opendock: Notify: notch actualizado, devolviendo id=%u", id);

    if (pixbuf) g_object_unref(pixbuf);
    g_free(app_name);
    g_free(app_icon);
    g_free(summary);
    g_free(body);
    if (hints) g_variant_unref(hints);

    g_dbus_method_invocation_return_value(invocacion, g_variant_new("(u)", id));
}

static void emitir_cerrado(guint32 id, guint32 razon)
{
    if (!g_srv.bus) return;
    g_dbus_connection_emit_signal(g_srv.bus, NULL,
        "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
        "NotificationClosed", g_variant_new("(uu)", id, razon), NULL);
}

static void manejar_close(GVariant *parametros, GDBusMethodInvocation *invocacion)
{
    guint32 id = 0;
    g_variant_get(parametros, "(u)", &id);
    if (id != 0 && id == g_srv.id_actual) {
        od_notch_ocultar();
        emitir_cerrado(id, 3); /* 3 = cerrada por CloseNotification */
    }
    g_dbus_method_invocation_return_value(invocacion, NULL);
}

static void manejar_capacidades(GDBusMethodInvocation *invocacion)
{
    g_message("opendock: GetCapabilities: construyendo respuesta");
    const gchar *caps[] = { "body", "actions", "icon-static", "persistence", NULL };
    GVariant *respuesta = g_variant_new("(^as)", (gchar **)caps);
    g_message("opendock: GetCapabilities: respuesta lista, tipo %s",
        g_variant_get_type_string(respuesta));
    g_dbus_method_invocation_return_value(invocacion, respuesta);
}

static void manejar_info_servidor(GDBusMethodInvocation *invocacion)
{
    g_dbus_method_invocation_return_value(invocacion,
        g_variant_new("(ssss)", "OpenDock", "OpenDock", OPENDOCK_VERSION, "1.2"));
}

static void al_llamar_metodo(GDBusConnection *conexion, const gchar *remitente,
    const gchar *ruta, const gchar *interfaz, const gchar *metodo,
    GVariant *parametros, GDBusMethodInvocation *invocacion, gpointer datos)
{
    (void)conexion; (void)remitente; (void)ruta; (void)interfaz; (void)datos;
    g_message("opendock: D-Bus Notifications.%s llamado por %s", metodo, remitente);
    if (g_strcmp0(metodo, "Notify") == 0) {
        manejar_notify(parametros, invocacion);
    } else if (g_strcmp0(metodo, "CloseNotification") == 0) {
        manejar_close(parametros, invocacion);
    } else if (g_strcmp0(metodo, "GetCapabilities") == 0) {
        manejar_capacidades(invocacion);
    } else if (g_strcmp0(metodo, "GetServerInformation") == 0) {
        manejar_info_servidor(invocacion);
    } else {
        g_dbus_method_invocation_return_error(invocacion, G_DBUS_ERROR,
            G_DBUS_ERROR_UNKNOWN_METHOD, "Método desconocido: %s", metodo);
    }
}

static const GDBusInterfaceVTable vtabla = {
    .method_call = al_llamar_metodo,
    .get_property = NULL,
    .set_property = NULL,
};

static void al_adquirir_bus(GDBusConnection *conexion, const gchar *nombre, gpointer datos)
{
    (void)nombre; (void)datos;
    g_srv.bus = conexion;
    GError *error = NULL;
    GDBusNodeInfo *nodo = g_dbus_node_info_new_for_xml(introspeccion_xml, &error);
    if (!nodo) {
        g_warning("opendock: no se pudo analizar la introspección D-Bus: %s", error->message);
        g_clear_error(&error);
        return;
    }
    g_srv.id_registro = g_dbus_connection_register_object(conexion,
        "/org/freedesktop/Notifications", nodo->interfaces[0], &vtabla, NULL, NULL, &error);
    if (!g_srv.id_registro) {
        g_warning("opendock: no se pudo registrar org.freedesktop.Notifications: %s", error->message);
        g_clear_error(&error);
    } else {
        g_message("opendock: servidor de notificaciones activo en %s", "org.freedesktop.Notifications");
    }
    g_dbus_node_info_unref(nodo);
}

static void al_perder_nombre(GDBusConnection *conexion, const gchar *nombre, gpointer datos)
{
    (void)conexion; (void)datos;
    /* Alguien más (o el propio escritorio) ya tenía el nombre: seguimos
     * funcionando con el resto de módulos, sin el notch alimentado por
     * D-Bus. Con --replace esto no debería pasar salvo error de permisos. */
    g_message("opendock: %s ya lo posee otro servidor de notificaciones; "
        "el resto de opendock sigue funcionando (usa --replace para tomarlo)",
        nombre);
}

void od_notificaciones_iniciar(OdConfig *cfg)
{
    g_srv.cfg = cfg;
    g_srv.siguiente_id = 1;

    GBusNameOwnerFlags flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
    if (cfg->reemplazar_notificaciones)
        flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

    g_srv.id_propietario = g_bus_own_name(G_BUS_TYPE_SESSION,
        "org.freedesktop.Notifications", flags,
        al_adquirir_bus, NULL, al_perder_nombre, NULL, NULL);
}
