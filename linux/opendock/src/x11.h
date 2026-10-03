/*
 * Acceso a Xlib desde superficies de GDK para el respaldo X11.
 * GTK 4.18 marca obsoleto el backend X11 (desaparece en GTK 5); mientras
 * exista lo usamos, y el aviso se silencia sólo aquí.
 */
#ifndef OPENDOCK_X11_H
#define OPENDOCK_X11_H

#include "opendock-build-config.h"

#if HAVE_X11
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>

static inline Display *od_x11_display(GdkSurface *surface)
{
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    Display *d = GDK_SURFACE_XDISPLAY(surface);
    G_GNUC_END_IGNORE_DEPRECATIONS
    return d;
}

static inline Display *od_x11_display_de(GdkDisplay *display)
{
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    Display *d = gdk_x11_display_get_xdisplay(display);
    G_GNUC_END_IGNORE_DEPRECATIONS
    return d;
}

static inline Window od_x11_ventana(GdkSurface *surface)
{
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    Window w = GDK_SURFACE_XID(surface);
    G_GNUC_END_IGNORE_DEPRECATIONS
    return w;
}
#endif

#endif
