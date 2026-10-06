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
#if HAVE_XCOMPOSITE
#include <X11/extensions/Xcomposite.h>
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
#if HAVE_X11
    GHashTable *redirigidas;     /* xid de las ventanas redirigidas para las miniaturas */
#endif
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
    g_debug("ventanas: aviso (%u ventanas)", g_v.lista->len);
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
    (void)h;
    OdVentana *v = d;
    g_debug("ventanas: hecho %s activa=%d", v->app_id, v->activa);
    avisar();
}

static void ft_cerrada(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)h;
    OdVentana *v = d;
    g_debug("ventanas: cerrada %s (%s)", v->app_id, v->titulo);
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
        a == atomo(d, "_NET_CLIENT_LIST_STACKING") ||
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

int od_ventana_pid(const OdVentana *v)
{
#if HAVE_X11
    if (v->xid) {
        Display *d = display_x11();
        unsigned long n = 0;
        unsigned long *pid = leer_lista(d, v->xid, atomo(d, "_NET_WM_PID"), XA_CARDINAL, &n);
        int r = (pid && n) ? (int)pid[0] : 0;
        if (pid) XFree(pid);
        return r;
    }
#endif
    (void)v;
    return 0;
}

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

/* ---- pantalla completa y miniaturas ------------------------------------- */

#if HAVE_X11
static void trampa_x11(gboolean poner)
{
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GdkDisplay *gd = gdk_display_get_default();
    if (poner) gdk_x11_display_error_trap_push(gd);
    else gdk_x11_display_error_trap_pop_ignored(gd);
    G_GNUC_END_IGNORE_DEPRECATIONS
}
#endif

/* Mira el orden de apilado de arriba abajo: la primera ventana visible que pisa el
 * monitor decide (si está a pantalla completa, el monitor está tapado). Así, con
 * dos monitores, un vídeo a pantalla completa en uno lo sigue tapando aunque el
 * foco esté en el otro. En Wayland el compositor ya pone la pantalla completa por
 * encima de la capa TOP de layer-shell (sway, Hyprland, KWin...). */
gboolean od_ventanas_pantalla_completa_en(GdkMonitor *monitor)
{
#if HAVE_X11
    if (g_v.backend != OD_BACKEND_X11 || !g_v.disponible || !monitor) return FALSE;
    GdkRectangle geo;
    gdk_monitor_get_geometry(monitor, &geo);
    const int f = gdk_monitor_get_scale_factor(monitor);   /* GDK mide en píxeles de app */
    geo.x *= f; geo.y *= f; geo.width *= f; geo.height *= f;
    Display *d = display_x11();
    Window raiz = DefaultRootWindow(d);
    unsigned long n = 0;
    unsigned long *pila = leer_lista(d, raiz, atomo(d, "_NET_CLIENT_LIST_STACKING"), XA_WINDOW, &n);
    gboolean tapado = FALSE;
    trampa_x11(TRUE);
    for (long i = (long)n - 1; pila && i >= 0; i--) {      /* de arriba abajo */
        Window w = pila[i];
        if (tiene_atomo(d, w, "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK") ||
            tiene_atomo(d, w, "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DESKTOP") ||
            tiene_atomo(d, w, "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN"))
            continue;
        XWindowAttributes at;
        if (!XGetWindowAttributes(d, w, &at) || at.map_state != IsViewable) continue;
        int x = 0, y = 0;
        Window hijo;
        if (!XTranslateCoordinates(d, w, raiz, 0, 0, &x, &y, &hijo)) continue;
        if (x >= geo.x + geo.width || y >= geo.y + geo.height ||
            x + at.width <= geo.x || y + at.height <= geo.y)
            continue;                                       /* no pisa este monitor */
        tapado = tiene_atomo(d, w, "_NET_WM_STATE", "_NET_WM_STATE_FULLSCREEN");
        break;
    }
    trampa_x11(FALSE);
    if (pila) XFree(pila);
    return tapado;
#else
    (void)monitor;
    return FALSE;
#endif
}

/* Miniatura de la ventana (X11 con XComposite): la ventana se redirige fuera de
 * pantalla en modo automático (el servidor la sigue pintando igual) mientras la
 * vista previa está abierta, y se lee su pixmap reducido. NULL si no se puede
 * (Wayland, minimizada, sin XComposite o aún sin contenido). */
GdkTexture *od_ventana_miniatura(OdVentana *v, int max_ancho, int max_alto)
{
#if HAVE_X11 && HAVE_XCOMPOSITE
    if (!v || !v->xid || v->minimizada || g_v.backend != OD_BACKEND_X11) return NULL;
    Display *d = display_x11();
    int ev = 0, er = 0;
    if (!XCompositeQueryExtension(d, &ev, &er)) return NULL;
    trampa_x11(TRUE);
    XWindowAttributes at;
    XImage *img = NULL;
    if (XGetWindowAttributes(d, v->xid, &at) && at.map_state == IsViewable && at.width > 1 && at.height > 1) {
        if (!g_v.redirigidas) g_v.redirigidas = g_hash_table_new(NULL, NULL);
        if (!g_hash_table_contains(g_v.redirigidas, GSIZE_TO_POINTER(v->xid))) {
            XCompositeRedirectWindow(d, v->xid, CompositeRedirectAutomatic);
            g_hash_table_add(g_v.redirigidas, GSIZE_TO_POINTER(v->xid));
        }
        Pixmap px = XCompositeNameWindowPixmap(d, v->xid);
        XSync(d, False);
        if (px) {
            img = XGetImage(d, px, 0, 0, at.width, at.height, AllPlanes, ZPixmap);
            XFreePixmap(d, px);
        }
    }
    XSync(d, False);
    trampa_x11(FALSE);
    if (!img) return NULL;
    if (img->bits_per_pixel != 32) { XDestroyImage(img); return NULL; }

    const double k = MIN(1.0, MIN((double)max_ancho / at.width, (double)max_alto / at.height));
    const int tw = MAX(1, (int)(at.width * k)), th = MAX(1, (int)(at.height * k));
    guchar *buf = g_malloc((gsize)tw * th * 4);
    const gboolean con_alfa = at.depth == 32;
    guint64 suma = 0;
    /* reducción por promedio de cada bloque (nítida, sin el dentado del vecino más cercano) */
    for (int y = 0; y < th; y++) {
        const int y0 = (int)((double)y * at.height / th), y1 = MAX(y0 + 1, (int)((double)(y + 1) * at.height / th));
        for (int x = 0; x < tw; x++) {
            const int x0 = (int)((double)x * at.width / tw), x1 = MAX(x0 + 1, (int)((double)(x + 1) * at.width / tw));
            guint b = 0, g = 0, r = 0, a = 0, cuenta = 0;
            for (int yy = y0; yy < y1; yy += 1 + (y1 - y0) / 4)
                for (int xx = x0; xx < x1; xx += 1 + (x1 - x0) / 4) {
                    const guint32 p = *(guint32 *)(void *)(img->data + (gsize)yy * img->bytes_per_line + (gsize)xx * 4);
                    b += p & 255; g += (p >> 8) & 255; r += (p >> 16) & 255; a += con_alfa ? p >> 24 : 255;
                    cuenta++;
                }
            guchar *o = buf + ((gsize)y * tw + x) * 4;
            o[0] = b / cuenta; o[1] = g / cuenta; o[2] = r / cuenta; o[3] = a / cuenta;
            suma += o[0] + o[1] + o[2];
        }
    }
    XDestroyImage(img);
    if (suma == 0) { g_free(buf); return NULL; }     /* aún sin pintar fuera de pantalla */
    GBytes *bytes = g_bytes_new_take(buf, (gsize)tw * th * 4);
    GdkTexture *t = gdk_memory_texture_new(tw, th, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes, (gsize)tw * 4);
    g_bytes_unref(bytes);
    return t;
#else
    (void)v; (void)max_ancho; (void)max_alto;
    return NULL;
#endif
}

/* Se cerró la vista previa: las ventanas vuelven a pintarse como antes. */
void od_ventanas_soltar_miniaturas(void)
{
#if HAVE_X11 && HAVE_XCOMPOSITE
    if (!g_v.redirigidas || !g_hash_table_size(g_v.redirigidas)) return;
    Display *d = display_x11();
    GHashTableIter it;
    gpointer clave;
    trampa_x11(TRUE);
    g_hash_table_iter_init(&it, g_v.redirigidas);
    while (g_hash_table_iter_next(&it, &clave, NULL))
        XCompositeUnredirectWindow(d, (Window)GPOINTER_TO_SIZE(clave), CompositeRedirectAutomatic);
    XSync(d, False);
    trampa_x11(FALSE);
    g_hash_table_remove_all(g_v.redirigidas);
#endif
}
