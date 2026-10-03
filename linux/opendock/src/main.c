/*
 * OpenDock para Linux — punto de entrada.
 *
 * Añade al escritorio lo que ya tiene la versión de Windows: esquinas
 * redondeadas, notch con notificaciones, barra superior y dock. En Wayland
 * se dibuja con zwlr_layer_shell_v1 (gtk4-layer-shell); en X11 se usa el
 * respaldo EWMH (_NET_WM_WINDOW_TYPE_DOCK + _NET_WM_STRUT_PARTIAL).
 * GNOME no admite clientes de capa: ahí cedemos el paso a la extensión
 * gnome-shell-extension-opendock y no dibujamos nada.
 *
 * CLI:
 *   opendock --version     imprime la versión y sale
 *   opendock --replace     reemplaza cualquier servidor de notificaciones
 *                          (org.freedesktop.Notifications) que ya exista
 *   opendock --help
 *
 * Acción por D-Bus (para un atajo de teclado del escritorio):
 *   gdbus call --session --dest io.github.josuegm24.OpenDock  *     --object-path /io/github/josuegm24/OpenDock  *     --method org.gtk.Actions.Activate centro [] {}
 */
#include "config.h"
#include "opendock-build-config.h"
#include "session.h"
#include "corners.h"
#include "notch.h"
#include "notifications.h"
#include "bar.h"
#include "centro.h"
#include "mini.h"
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <locale.h>
#include <stdio.h>

typedef struct {
    OdConfig *cfg;
    gboolean reemplazar;
    GApplication *app;
} OdEstado;

static gboolean manejar_senal_salida(gpointer datos)
{
    OdEstado *e = datos;
    g_message("opendock: señal de salida recibida, cerrando de forma ordenada");
    g_application_quit(e->app);
    return G_SOURCE_REMOVE;
}

static void al_accion_centro(GSimpleAction *a, GVariant *p, gpointer datos)
{
    (void)a; (void)p; (void)datos;
    od_centro_alternar();
}

static void al_activar(GApplication *app, gpointer datos)
{
    OdEstado *e = datos;

    /* GTK normalmente cierra la aplicación si no hay ninguna ventana activa;
     * como esto es un servicio de fondo (shell add-on), la mantenemos viva
     * explícitamente y cada módulo crea sus propias superficies cuando le
     * corresponda (fases siguientes). */
    g_application_hold(app);

    if (od_sesion_es_gnome()) {
        g_printerr(
            "opendock: esta sesión es GNOME Shell (Mutter), que no admite "
            "superficies de capa (layer-shell).\n"
            "Instala y activa la extensión «OpenDock» para GNOME Shell en su lugar:\n"
            "  https://github.com/JosueGM24/OpenDock (linux/gnome-shell-extension)\n"
            "opendock seguirá en ejecución sin dibujar nada en esta sesión.\n");
        return;
    }

    OdBackendTipo backend = od_sesion_backend();
    g_message("opendock %s: backend=%s radio_esquinas=%d alto_barra=%d config=%s",
        OPENDOCK_VERSION,
        od_sesion_backend_nombre(backend),
        e->cfg->radio_esquinas,
        e->cfg->alto_barra,
        e->cfg->config_path);

    if (backend == OD_BACKEND_DESCONOCIDO) {
        g_printerr("opendock: no se detecta ni WAYLAND_DISPLAY ni DISPLAY; nada que hacer.\n");
        return;
    }

    od_esquinas_iniciar(e->cfg, backend);
    od_notch_iniciar(e->cfg, backend);
    e->cfg->reemplazar_notificaciones = e->reemplazar;
    od_notificaciones_iniciar(e->cfg);
    od_bar_iniciar(e->cfg, backend);
    od_mini_iniciar(e->cfg, backend);

    GSimpleAction *centro = g_simple_action_new("centro", NULL);
    g_signal_connect(centro, "activate", G_CALLBACK(al_accion_centro), NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(centro));
    g_object_unref(centro);

    /* La fase siguiente añade aquí: dock. */
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");

    gboolean mostrar_version = FALSE;
    gboolean reemplazar = FALSE;
    GOptionEntry opciones[] = {
        { "version", 0, 0, G_OPTION_ARG_NONE, &mostrar_version, "Muestra la versión y sale", NULL },
        { "replace", 0, 0, G_OPTION_ARG_NONE, &reemplazar, "Reemplaza el servidor de notificaciones existente", NULL },
        { NULL }
    };

    GError *error = NULL;
    GOptionContext *ctx = g_option_context_new("— notch, barra y dock para Linux");
    g_option_context_add_main_entries(ctx, opciones, NULL);
    g_option_context_set_help_enabled(ctx, TRUE);
    if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
        g_printerr("opendock: %s\n", error->message);
        g_error_free(error);
        g_option_context_free(ctx);
        return 1;
    }
    g_option_context_free(ctx);

    if (mostrar_version) {
        printf("opendock %s\n", OPENDOCK_VERSION);
        return 0;
    }

    gtk_init();

    OdEstado estado = { 0 };
    estado.cfg = od_config_cargar();
    estado.reemplazar = reemplazar;

#if GLIB_CHECK_VERSION(2, 74, 0)
    GApplication *app = g_application_new("io.github.josuegm24.OpenDock", G_APPLICATION_DEFAULT_FLAGS);
#else
    GApplication *app = g_application_new("io.github.josuegm24.OpenDock", G_APPLICATION_FLAGS_NONE);
#endif
    estado.app = app;
    g_signal_connect(app, "activate", G_CALLBACK(al_activar), &estado);

    g_unix_signal_add(SIGINT, manejar_senal_salida, &estado);
    g_unix_signal_add(SIGTERM, manejar_senal_salida, &estado);

    int salida = g_application_run(app, 0, NULL);

    g_object_unref(app);
    od_config_liberar(estado.cfg);
    return salida;
}
