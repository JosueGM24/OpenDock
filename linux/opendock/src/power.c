#include "power.h"
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

static GDBusConnection *bus_sistema(void)
{
    static GDBusConnection *conexion = NULL;
    static gboolean intentado = FALSE;
    if (!intentado) {
        intentado = TRUE;
        GError *error = NULL;
        conexion = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
        if (!conexion) {
            g_message("opendock: sin bus de sistema (%s)", error ? error->message : "?");
            g_clear_error(&error);
        }
    }
    return conexion;
}

/* Properties.Get envuelto en 'v'; desenvuelve y devuelve el GVariant interior
 * (floating, listo para g_variant_get). NULL si falla. */
static GVariant *propiedad_get(const char *bus_name, const char *ruta,
    const char *interfaz, const char *propiedad)
{
    GDBusConnection *con = bus_sistema();
    if (!con) return NULL;
    GError *error = NULL;
    GVariant *resp = g_dbus_connection_call_sync(con, bus_name, ruta,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", interfaz, propiedad),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &error);
    if (!resp) {
        g_clear_error(&error);
        return NULL;
    }
    GVariant *v = NULL;
    g_variant_get(resp, "(v)", &v);
    g_variant_unref(resp);
    return v; /* el llamador debe hacer g_variant_unref */
}

static gboolean propiedad_set(const char *bus_name, const char *ruta,
    const char *interfaz, const char *propiedad, GVariant *valor)
{
    GDBusConnection *con = bus_sistema();
    if (!con) return FALSE;
    GError *error = NULL;
    GVariant *resp = g_dbus_connection_call_sync(con, bus_name, ruta,
        "org.freedesktop.DBus.Properties", "Set",
        g_variant_new("(ssv)", interfaz, propiedad, valor),
        NULL, G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &error);
    if (!resp) {
        g_message("opendock: no se pudo cambiar %s.%s: %s", interfaz, propiedad,
            error ? error->message : "?");
        g_clear_error(&error);
        return FALSE;
    }
    g_variant_unref(resp);
    return TRUE;
}

/* ---- NetworkManager (Wi-Fi) ---- */

int od_wifi_activado(void)
{
    GVariant *v = propiedad_get("org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager",
        "WirelessEnabled");
    if (!v) return -1;
    gboolean on = g_variant_get_boolean(v);
    g_variant_unref(v);
    return on ? 1 : 0;
}

void od_wifi_fijar_activado(gboolean on)
{
    propiedad_set("org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager",
        "WirelessEnabled", g_variant_new_boolean(on));
}

/* ---- UPower (batería) ---- */

void od_bateria_leer(int *pct, gboolean *cargando)
{
    *pct = -1;
    if (cargando) *cargando = FALSE;
    GVariant *v = propiedad_get("org.freedesktop.UPower",
        "/org/freedesktop/UPower/devices/DisplayDevice",
        "org.freedesktop.UPower.Device", "Percentage");
    if (!v) return;
    double p = g_variant_get_double(v);
    g_variant_unref(v);
    *pct = (int)(p + 0.5);

    GVariant *estado = propiedad_get("org.freedesktop.UPower",
        "/org/freedesktop/UPower/devices/DisplayDevice",
        "org.freedesktop.UPower.Device", "State");
    if (estado) {
        guint32 s = g_variant_get_uint32(estado);
        g_variant_unref(estado);
        if (cargando) *cargando = (s == 1); /* 1 = cargando */
    }
}

/* ---- BlueZ ---- */

static const gchar *adaptador_bluez(void)
{
    static gchar *ruta = NULL;
    static gboolean buscado = FALSE;
    if (buscado) return ruta;
    buscado = TRUE;

    GDBusConnection *con = bus_sistema();
    if (!con) return NULL;
    GError *error = NULL;
    GVariant *resp = g_dbus_connection_call_sync(con, "org.bluez", "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &error);
    if (!resp) {
        g_clear_error(&error);
        return NULL;
    }
    GVariantIter *objetos = NULL;
    g_variant_get(resp, "(a{oa{sa{sv}}})", &objetos);
    const gchar *obj_path;
    GVariantIter *interfaces;
    while (g_variant_iter_loop(objetos, "{o a{sa{sv}}}", &obj_path, &interfaces)) {
        const gchar *nombre_iface;
        GVariantIter *props;
        while (g_variant_iter_loop(interfaces, "{sa{sv}}", &nombre_iface, &props)) {
            if (g_strcmp0(nombre_iface, "org.bluez.Adapter1") == 0 && !ruta) {
                ruta = g_strdup(obj_path);
            }
        }
    }
    g_variant_iter_free(objetos);
    g_variant_unref(resp);
    return ruta;
}

int od_bluetooth_activado(void)
{
    const gchar *ruta = adaptador_bluez();
    if (!ruta) return -1;
    GVariant *v = propiedad_get("org.bluez", ruta, "org.bluez.Adapter1", "Powered");
    if (!v) return -1;
    gboolean on = g_variant_get_boolean(v);
    g_variant_unref(v);
    return on ? 1 : 0;
}

void od_bluetooth_fijar_activado(gboolean on)
{
    const gchar *ruta = adaptador_bluez();
    if (!ruta) return;
    propiedad_set("org.bluez", ruta, "org.bluez.Adapter1", "Powered",
        g_variant_new_boolean(on));
}

/* ---- logind (brillo, sin root) ---- */

static gboolean primer_backlight(gchar **nombre, int *actual, int *maximo)
{
    *nombre = NULL;
    *actual = *maximo = 0;
    const char *base = "/sys/class/backlight";
    DIR *d = opendir(base);
    if (!d) return FALSE;
    struct dirent *ent;
    gboolean ok = FALSE;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        gchar *ruta_max = g_strdup_printf("%s/%s/max_brightness", base, ent->d_name);
        gchar *ruta_act = g_strdup_printf("%s/%s/brightness", base, ent->d_name);
        gchar *c_max = NULL, *c_act = NULL;
        if (g_file_get_contents(ruta_max, &c_max, NULL, NULL) &&
            g_file_get_contents(ruta_act, &c_act, NULL, NULL)) {
            *maximo = atoi(c_max);
            *actual = atoi(c_act);
            *nombre = g_strdup(ent->d_name);
            ok = (*maximo > 0);
        }
        g_free(c_max); g_free(c_act);
        g_free(ruta_max); g_free(ruta_act);
        if (ok) break;
    }
    closedir(d);
    return ok;
}

void od_brillo_leer(double *valor)
{
    *valor = -1;
    gchar *nombre = NULL;
    int actual, maximo;
    if (primer_backlight(&nombre, &actual, &maximo) && maximo > 0) {
        *valor = (double)actual / (double)maximo;
    }
    g_free(nombre);
}

static void al_fijar_brillo(GObject *origen, GAsyncResult *res, gpointer datos)
{
    (void)datos;
    GError *error = NULL;
    GVariant *resp = g_dbus_connection_call_finish(G_DBUS_CONNECTION(origen), res, &error);
    if (!resp) {
        g_message("opendock: SetBrightness falló (sin permiso vía logind): %s",
            error ? error->message : "?");
        g_clear_error(&error);
        return;
    }
    g_variant_unref(resp);
}

void od_brillo_fijar(double valor)
{
    gchar *nombre = NULL;
    int actual, maximo;
    if (!primer_backlight(&nombre, &actual, &maximo) || maximo <= 0) {
        g_free(nombre);
        return;
    }
    valor = CLAMP(valor, 0.0, 1.0);
    guint32 objetivo = (guint32)(valor * maximo + 0.5);

    GDBusConnection *con = bus_sistema();
    if (!con) { g_free(nombre); return; }

    /* "auto" es la sesión del proceso que llama (o la de su usuario si el
     * proceso no está dentro de una). Asíncrono: el deslizador manda
     * muchos valores seguidos y no debe congelar la barra. */
    g_dbus_connection_call(con, "org.freedesktop.login1",
        "/org/freedesktop/login1/session/auto", "org.freedesktop.login1.Session", "SetBrightness",
        g_variant_new("(ssu)", "backlight", nombre, objetivo),
        NULL, G_DBUS_CALL_FLAGS_NONE, 1500, NULL, al_fijar_brillo, NULL);
    g_free(nombre);
}
