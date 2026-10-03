#include "config.h"
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include <string.h>

static const char *APPS_POR_DEFECTO[] = {
    "firefox.desktop",
    "org.gnome.Nautilus.desktop",
    "org.kde.dolphin.desktop",
    "org.gnome.Console.desktop",
    NULL
};

static gchar *ruta_config_ini(void)
{
    const gchar *base = g_get_user_config_dir();
    gchar *dir = g_build_filename(base, "opendock", NULL);
    g_mkdir_with_parents(dir, 0700);
    gchar *ruta = g_build_filename(dir, "config.ini", NULL);
    g_free(dir);
    return ruta;
}

/* De la lista de apps por defecto nos quedamos sólo con las que existen
 * de verdad en este sistema (GDesktopAppInfo), para no anclar iconos rotos. */
static gchar **filtrar_apps_existentes(const char **candidatas)
{
    GPtrArray *encontradas = g_ptr_array_new();
    for (int i = 0; candidatas[i]; i++) {
        GDesktopAppInfo *info = g_desktop_app_info_new(candidatas[i]);
        if (info) {
            g_ptr_array_add(encontradas, g_strdup(candidatas[i]));
            g_object_unref(info);
        }
    }
    g_ptr_array_add(encontradas, NULL);
    return (gchar **)g_ptr_array_free(encontradas, FALSE);
}

OdConfig *od_config_cargar(void)
{
    OdConfig *cfg = g_new0(OdConfig, 1);
    cfg->config_path = ruta_config_ini();
    cfg->radio_esquinas = 16;
    cfg->alto_barra = 28;
    cfg->alto_dock = 60;
    cfg->oled = FALSE;
    cfg->reemplazar_notificaciones = FALSE;

    GKeyFile *kf = g_key_file_new();
    GError *error = NULL;
    gboolean existe = g_key_file_load_from_file(kf, cfg->config_path, G_KEY_FILE_NONE, &error);
    if (!existe) {
        g_clear_error(&error);
    }

    if (existe && g_key_file_has_key(kf, "general", "radio_esquinas", NULL))
        cfg->radio_esquinas = g_key_file_get_integer(kf, "general", "radio_esquinas", NULL);
    if (existe && g_key_file_has_key(kf, "general", "alto_barra", NULL))
        cfg->alto_barra = g_key_file_get_integer(kf, "general", "alto_barra", NULL);
    if (existe && g_key_file_has_key(kf, "general", "alto_dock", NULL))
        cfg->alto_dock = g_key_file_get_integer(kf, "general", "alto_dock", NULL);
    if (existe && g_key_file_has_key(kf, "general", "oled", NULL))
        cfg->oled = g_key_file_get_boolean(kf, "general", "oled", NULL);
    if (existe && g_key_file_has_key(kf, "general", "no_molestar", NULL))
        cfg->no_molestar = g_key_file_get_boolean(kf, "general", "no_molestar", NULL);

    if (existe && g_key_file_has_key(kf, "dock", "apps", NULL)) {
        gsize n = 0;
        gchar **lista = g_key_file_get_string_list(kf, "dock", "apps", &n, NULL);
        cfg->apps_ancladas = lista;
    } else {
        cfg->apps_ancladas = filtrar_apps_existentes(APPS_POR_DEFECTO);
    }
    cfg->ocultar_dock = OD_OCULTAR_NUNCA;
    if (existe && g_key_file_has_key(kf, "dock", "ocultar", NULL)) {
        gchar *modo = g_key_file_get_string(kf, "dock", "ocultar", NULL);
        if (g_strcmp0(modo, "mitad") == 0) cfg->ocultar_dock = OD_OCULTAR_MITAD;
        else if (g_strcmp0(modo, "completo") == 0) cfg->ocultar_dock = OD_OCULTAR_COMPLETO;
        g_free(modo);
    }

    /* Límites sanos: nunca confiar ciegamente en un config.ini a mano. */
    cfg->radio_esquinas = CLAMP(cfg->radio_esquinas, 0, 64);
    cfg->alto_barra = CLAMP(cfg->alto_barra, 16, 64);
    cfg->alto_dock = CLAMP(cfg->alto_dock, 32, 96);

    if (!existe) {
        /* Primera vez: dejamos un config.ini legible con los valores por defecto. */
        g_key_file_set_integer(kf, "general", "radio_esquinas", cfg->radio_esquinas);
        g_key_file_set_integer(kf, "general", "alto_barra", cfg->alto_barra);
        g_key_file_set_integer(kf, "general", "alto_dock", cfg->alto_dock);
        g_key_file_set_boolean(kf, "general", "oled", cfg->oled);
        g_key_file_set_string(kf, "dock", "ocultar", "nunca");
        g_key_file_set_string_list(kf, "dock", "apps",
            (const gchar * const *)cfg->apps_ancladas,
            g_strv_length(cfg->apps_ancladas));
        g_key_file_save_to_file(kf, cfg->config_path, NULL);
    }

    g_key_file_free(kf);
    return cfg;
}

void od_config_abrir(OdConfig *cfg)
{
    gchar *uri = g_filename_to_uri(cfg->config_path, NULL, NULL);
    GError *error = NULL;
    if (!uri || !g_app_info_launch_default_for_uri(uri, NULL, &error)) {
        g_message("opendock: no se pudo abrir %s: %s", cfg->config_path,
            error ? error->message : "?");
        g_clear_error(&error);
    }
    g_free(uri);
}

void od_config_guardar_bool(OdConfig *cfg, const char *grupo, const char *clave,
    gboolean valor)
{
    GKeyFile *kf = g_key_file_new();
    /* Se relee para conservar lo que el usuario haya editado a mano. */
    g_key_file_load_from_file(kf, cfg->config_path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_boolean(kf, grupo, clave, valor);
    GError *error = NULL;
    if (!g_key_file_save_to_file(kf, cfg->config_path, &error)) {
        g_message("opendock: no se pudo guardar %s: %s", cfg->config_path,
            error ? error->message : "?");
        g_clear_error(&error);
    }
    g_key_file_free(kf);
}

void od_config_guardar_apps(OdConfig *cfg)
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, cfg->config_path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_string_list(kf, "dock", "apps", (const gchar * const *)cfg->apps_ancladas,
        g_strv_length(cfg->apps_ancladas));
    GError *error = NULL;
    if (!g_key_file_save_to_file(kf, cfg->config_path, &error)) {
        g_message("opendock: no se pudo guardar %s: %s", cfg->config_path,
            error ? error->message : "?");
        g_clear_error(&error);
    }
    g_key_file_free(kf);
}

void od_config_liberar(OdConfig *cfg)
{
    if (!cfg) return;
    g_free(cfg->config_path);
    g_strfreev(cfg->apps_ancladas);
    g_free(cfg);
}
