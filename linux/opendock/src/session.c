#include "session.h"
#include <string.h>
#include <stdlib.h>

gboolean od_sesion_es_gnome(void)
{
    const char *candidatos[] = {
        g_getenv("XDG_CURRENT_DESKTOP"),
        g_getenv("XDG_SESSION_DESKTOP"),
        g_getenv("DESKTOP_SESSION"),
        NULL
    };
    for (int i = 0; candidatos[i]; i++) {
        if (candidatos[i] && *candidatos[i]) {
            gchar *minus = g_ascii_strdown(candidatos[i], -1);
            gboolean es_gnome = strstr(minus, "gnome") != NULL;
            g_free(minus);
            if (es_gnome) return TRUE;
        }
    }
    return FALSE;
}

OdBackendTipo od_sesion_backend(void)
{
    /* GDK_BACKEND permite forzar el backend (lo usamos en CI). */
    const char *forzado = g_getenv("GDK_BACKEND");
    if (forzado) {
        if (strstr(forzado, "wayland")) return OD_BACKEND_WAYLAND;
        if (strstr(forzado, "x11")) return OD_BACKEND_X11;
    }
    if (g_getenv("WAYLAND_DISPLAY")) return OD_BACKEND_WAYLAND;
    if (g_getenv("DISPLAY")) return OD_BACKEND_X11;
    return OD_BACKEND_DESCONOCIDO;
}

const char *od_sesion_backend_nombre(OdBackendTipo b)
{
    switch (b) {
        case OD_BACKEND_WAYLAND: return "wayland";
        case OD_BACKEND_X11: return "x11";
        default: return "desconocido";
    }
}
