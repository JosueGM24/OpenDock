/*
 * ventanas.c — lista de ventanas abiertas (ver ventanas.h).
 *
 * Todo ocurre en el hilo principal: en Wayland los eventos llegan por la
 * cola por defecto de wl_display, que ya despacha GDK; en X11 escuchamos
 * PropertyNotify en la raíz y en cada ventana (sin sondeos).
 */
#include "ventanas.h"
#include "opendock-build-config.h"
#include <gtk/gtk.h>
#include <string.h>

#if HAVE_FOREIGN_TOPLEVEL
#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#endif
#if HAVE_X11
#include "x11.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#endif

#define OD_MAX_TITULO 256

struct OdVentana {
    gchar *app_id;
    gchar *titulo;
    gboolean activa;
    gboolean minimizada;
#if HAVE_FOREIGN_TOPLEVEL
    struct zwlr_foreign_toplevel_handle_v1 *handle;
#endif
#if HAVE_X11
    unsigned long xid;
#endif
};

static struct {
    OdBackendTipo backend;
    OdVentanasCambioFn cb;
    gpointer cb_datos;
    GPtrArray *lista;
    guint aviso_pendiente;
    gboolean disponible;
#if HAVE_FOREIGN_TOPLEVEL
    struct zwlr_foreign_toplevel_manager_v1 *gestor;
#endif
} g_v;

static void liberar_ventana(gpointer p)
{
    OdVentana *v = p;
    g_free(v->app_id);
    g_free(v->titulo);
#if HAVE_FOREIGN_TOPLEVEL
    if (v->handle) zwlr_foreign_toplevel_handle_v1_destroy(v->handle);
#endif
    g_free(v);
}

static gboolean emitir_aviso(gpointer datos)
{
    (void)datos;
    g_v.aviso_pendiente = 0;
    if (g_v.cb) g_v.cb(g_v.cb_datos);
    return G_SOURCE_REMOVE;
}

/* Varios cambios seguidos se avisan una sola vez. */
static void avisar(void)
{
    if (!g_v.aviso_pendiente) g_v.aviso_pendiente = g_idle_add(emitir_aviso, NULL);
}

static gchar *acotar(const char *s)
{
    if (!s) return g_strdup("");
    if (!g_utf8_validate(s, -1, NULL)) return g_utf8_make_valid(s, -1);
    if (g_utf8_strlen(s, -1) <= OD_MAX_TITULO) return g_strdup(s);
    return g_utf8_substring(s, 0, OD_MAX_TITULO);
}

/* ---- Wayland: zwlr_foreign_toplevel ------------------------------------- */
#if HAVE_FOREIGN_TOPLEVEL

static void ft_titulo(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *t)
{
    (void)h;
    OdVentana *v = d;
    g_free(v->titulo);
    v->titulo = acotar(t);
}

static void ft_app_id(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *id)
{
    (void)h;
    OdVentana *v = d;
    g_free(v->app_id);
    v->app_id = acotar(id);
}

static void ft_salida_entra(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o)
{
    (void)d; (void)h; (void)o;
}

static void ft_salida_sale(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o)
{
    (void)d; (void)h; (void)o;
}

static void ft_estado(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *estado)
{
    (void)h;
    OdVentana *v = d;
    v->activa = FALSE;
    v->minimizada = FALSE;
    uint32_t *e;
    wl_array_for_each(e, estado) {
        if (*e == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) v->activa = TRUE;
        if (*e == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) v->minimizada = TRUE;
    }
}

static void ft_hecho(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)d; (void)h;
    avisar();
}

static void ft_cerrada(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)h;
    OdVentana *v = d;
    g_ptr_array_remove(g_v.lista, v);   /* liberar_ventana destruye el handle */
    avisar();
}

static void ft_padre(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
    struct zwlr_foreign_toplevel_handle_v1 *padre)
{
    (void)d; (void)h; (void)padre;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener oyente_ventana = {
    .title = ft_titulo,
    .app_id = ft_app_id,
    .output_enter = ft_salida_entra,
    .output_leave = ft_salida_sale,
    .state = ft_estado,
    .done = ft_hecho,
    .closed = ft_cerrada,
    .parent = ft_padre,
};

static void gestor_nueva(void *d, struct zwlr_foreign_toplevel_manager_v1 *g,
    struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)d; (void)g;
    OdVentana *v = g_new0(OdVentana, 1);
    v->app_id = g_strdup("");
    v->titulo = g_strdup("");
    v->handle = h;
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &oyente_ventana, v);
    g_ptr_array_add(g_v.lista, v);
}

static void gestor_terminado(void *d, struct zwlr_foreign_toplevel_manager_v1 *g)
{
    (void)d;
    zwlr_foreign_toplevel_manager_v1_destroy(g);
    g_v.gestor = NULL;
    g_v.disponible = FALSE;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener oyente_gestor = {
    .toplevel = gestor_nueva,
    .finished = gestor_terminado,
};

static void registro_global(void *d, struct wl_registry *r, uint32_t nombre,
    const char *interfaz, uint32_t version)
{
    (void)d;
    if (strcmp(interfaz, zwlr_foreign_toplevel_manager_v1_interface.name) == 0 && !g_v.gestor) {
        g_v.gestor = wl_registry_bind(r, nombre, &zwlr_foreign_toplevel_manager_v1_interface,
            MIN(version, 3u));
        zwlr_foreign_toplevel_manager_v1_add_listener(g_v.gestor, &oyente_gestor, NULL);
        g_v.disponible = TRUE;
    }
}

static void registro_quitado(void *d, struct wl_registry *r, uint32_t nombre)
{
    (void)d; (void)r; (void)nombre;
}

static const struct wl_registry_listener oyente_registro = {
    .global = registro_global,
    .global_remove = registro_quitado,
};

static void iniciar_wayland(void)
{
    GdkDisplay *gd = gdk_display_get_default();
    if (!GDK_IS_WAYLAND_DISPLAY(gd)) return;
    struct wl_display *wd = gdk_wayland_display_get_wl_display(gd);
    struct wl_registry *reg = wl_display_get_registry(wd);
    wl_registry_add_listener(reg, &oyente_registro, NULL);
    /* Una ida y vuelta para saber ya si el compositor tiene el protocolo. */
    wl_display_roundtrip(wd);
    if (!g_v.disponible)
        g_message("opendock: el compositor no ofrece zwlr_foreign_toplevel_manager_v1; "
            "el dock sólo mostrará las apps ancladas");
}

static struct wl_seat *asiento_wayland(void)
{
    GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());
    return seat ? gdk_wayland_seat_get_wl_seat(seat) : NULL;
}
#endif

/* ---- X11: EWMH ----------------------------------------------------------- */
#if HAVE_X11

static Atom atomo(Display *d, const char *n)
{
    return XInternAtom(d, n, False);
}

/* Lee una propiedad de 32 bits (lista de ventanas o átomos). */
static unsigned long *leer_lista(Display *d, Window w, Atom prop, Atom tipo, unsigned long *n)
{
    Atom real; int formato; unsigned long resto; unsigned char *datos = NULL;
    *n = 0;
    if (XGetWindowProperty(d, w, prop, 0, 4096, False, tipo, &real, &formato, n, &resto,
            &datos) != Success || !datos || formato != 32) {
        if (datos) XFree(datos);
        *n = 0;
        return NULL;
    }
    return (unsigned long *)datos;
}

static gchar *leer_titulo(Display *d, Window w)
{
    Atom real; int formato; unsigned long n, resto; unsigned char *datos = NULL;
    gchar *t = NULL;
    if (XGetWindowProperty(d, w, atomo(d, "_NET_WM_NAME"), 0, 1024, False,
            atomo(d, "UTF8_STRING"), &real, &formato, &n, &resto, &datos) == Success && datos) {
        t = acotar((const char *)datos);
        XFree(datos);
    }
    if (!t) {
        char *nombre = NULL;
        if (XFetchName(d, w, &nombre) && nombre) {
            t = acotar(nombre);
            XFree(nombre);
        }
    }
    return t ? t : g_strdup("");
}

static gboolean tiene_atomo(Display *d, Window w, const char *prop, const char *valor)
{
    unsigned long n;
    unsigned long *l = leer_lista(d, w, atomo(d, prop), XA_ATOM, &n);
    gboolean si = FALSE;
    Atom buscado = atomo(d, valor);
    for (unsigned long i = 0; l && i < n; i++) if (l[i] == buscado) si = TRUE;
    if (l) XFree(l);
    return si;
}

static Display *display_x11(void)
{
    return od_x11_display_de(gdk_display_get_default());
}

static void releer_x11(void)
{
    Display *d = display_x11();
    Window raiz = DefaultRootWindow(d);
    unsigned long n = 0, na = 0;
    unsigned long *clientes = leer_lista(d, raiz, atomo(d, "_NET_CLIENT_LIST"), XA_WINDOW, &n);
    unsigned long *activa = leer_lista(d, raiz, atomo(d, "_NET_ACTIVE_WINDOW"), XA_WINDOW, &na);
    Window w_activa = (activa && na) ? activa[0] : 0;

    g_ptr_array_set_size(g_v.lista, 0);
    for (unsigned long i = 0; clientes && i < n; i++) {
        Window w = clientes[i];
        if (tiene_atomo(d, w, "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK") ||
            tiene_atomo(d, w, "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DESKTOP") ||
            tiene_atomo(d, w, "_NET_WM_STATE", "_NET_WM_STATE_SKIP_TASKBAR"))
            continue;
        OdVentana *v = g_new0(OdVentana, 1);
        v->xid = w;
        XClassHint clase = { 0 };
        if (XGetClassHint(d, w, &clase)) {
            v->app_id = acotar(clase.res_class ? clase.res_class : clase.res_name);
            if (clase.res_name) XFree(clase.res_name);
            if (clase.res_class) XFree(clase.res_class);
        } else {
            v->app_id = g_strdup("");
        }
        v->titulo = leer_titulo(d, w);
        v->activa = (w == w_activa);
        v->minimizada = tiene_atomo(d, w, "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN");
        g_ptr_array_add(g_v.lista, v);
        /* Para enterarnos de cambios de título/estado de esta ventana. */
        XSelectInput(d, w, PropertyChangeMask);
    }
    if (clientes) XFree(clientes);
    if (activa) XFree(activa);
    avisar();
}

static gboolean al_evento_x(GdkDisplay *display, const XEvent *ev, gpointer datos)
{
    (void)display; (void)datos;
    if (ev->type != PropertyNotify) return FALSE;
    Display *d = ev->xproperty.display;
    Atom a = ev->xproperty.atom;
    if (a == atomo(d, "_NET_CLIENT_LIST") || a == atomo(d, "_NET_ACTIVE_WINDOW") ||
        a == atomo(d, "_NET_WM_NAME") || a == atomo(d, "_NET_WM_STATE") || a == XA_WM_NAME)
        releer_x11();
    return FALSE;   /* que GDK lo siga procesando */
}

static void iniciar_x11(void)
{
    GdkDisplay *gd = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gd)) return;
    Display *d = display_x11();
    Window raiz = DefaultRootWindow(d);
    XWindowAttributes at;
    XGetWindowAttributes(d, raiz, &at);
    XSelectInput(d, raiz, at.your_event_mask | PropertyChangeMask);
    g_signal_connect(gd, "xevent", G_CALLBACK(al_evento_x), NULL);
    g_v.disponible = TRUE;
    releer_x11();
}

static void mensaje_cliente(Window w, const char *tipo, long l0, long l1)
{
    Display *d = display_x11();
    XEvent ev = { 0 };
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = atomo(d, tipo);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = l0;
    ev.xclient.data.l[1] = l1;
    XSendEvent(d, DefaultRootWindow(d), False,
        SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(d);
}
#endif

/* ---- API ----------------------------------------------------------------- */

void od_ventanas_iniciar(OdBackendTipo backend, OdVentanasCambioFn cb, gpointer datos)
{
    if (g_v.lista) return;
    g_v.backend = backend;
    g_v.cb = cb;
    g_v.cb_datos = datos;
    g_v.lista = g_ptr_array_new_with_free_func(liberar_ventana);
#if HAVE_FOREIGN_TOPLEVEL
    if (backend == OD_BACKEND_WAYLAND) iniciar_wayland();
#endif
#if HAVE_X11
    if (backend == OD_BACKEND_X11) iniciar_x11();
#endif
}

gboolean od_ventanas_disponible(void)
{
    return g_v.disponible;
}

GPtrArray *od_ventanas_lista(void)
{
    return g_v.lista;
}

const char *od_ventana_app_id(const OdVentana *v) { return v->app_id; }
const char *od_ventana_titulo(const OdVentana *v) { return v->titulo; }
gboolean od_ventana_activa(const OdVentana *v) { return v->activa; }
gboolean od_ventana_minimizada(const OdVentana *v) { return v->minimizada; }

void od_ventana_activar(OdVentana *v)
{
#if HAVE_FOREIGN_TOPLEVEL
    if (v->handle) {
        struct wl_seat *seat = asiento_wayland();
        if (seat) zwlr_foreign_toplevel_handle_v1_activate(v->handle, seat);
        return;
    }
#endif
#if HAVE_X11
    if (v->xid) mensaje_cliente(v->xid, "_NET_ACTIVE_WINDOW", 2 /* fuente: pager */, CurrentTime);
#endif
    (void)v;
}

void od_ventana_minimizar(OdVentana *v)
{
#if HAVE_FOREIGN_TOPLEVEL
    if (v->handle) {
        zwlr_foreign_toplevel_handle_v1_set_minimized(v->handle);
        return;
    }
#endif
#if HAVE_X11
    if (v->xid) {
        Display *d = display_x11();
        XIconifyWindow(d, v->xid, DefaultScreen(d));
        XFlush(d);
    }
#endif
    (void)v;
}

void od_ventana_cerrar(OdVentana *v)
{
#if HAVE_FOREIGN_TOPLEVEL
    if (v->handle) {
        zwlr_foreign_toplevel_handle_v1_close(v->handle);
        return;
    }
#endif
#if HAVE_X11
    if (v->xid) mensaje_cliente(v->xid, "_NET_CLOSE_WINDOW", CurrentTime, 2);
#endif
    (void)v;
}
