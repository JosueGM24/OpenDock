/*
 * dock.c — el dock (ver dock.h y DESIGN.md).
 *
 * Un dock en cada monitor (como en Windows): todos con las mismas apps, cada
 * uno con su lupa, sus rebotes y su ocultación. Cada uno es una superficie
 * transparente pegada abajo y a todo lo ancho de su monitor; el panel y los
 * iconos se dibujan con GtkSnapshot en un widget propio (OdLienzo) para poder
 * escalarlos y moverlos en cada fotograma. La región de entrada se limita al
 * panel (y a lo que asoma de los iconos con lupa), así el resto de la franja
 * no tapa nada.
 *
 * Apps ancladas: cfg->apps_ancladas (ids .desktop). Ventanas abiertas:
 * ventanas.c (zwlr_foreign_toplevel en Wayland, EWMH en X11). Las apps se
 * lanzan con GAppInfo, nunca con un shell.
 *
 * Vista previa: con el cursor un momento sobre una app abierta (o con un clic
 * si tiene varias ventanas) sale encima una tarjeta por ventana, que crece
 * desde el icono con el muelle de OpenDock. En X11 lleva la miniatura de la
 * ventana (XComposite); en Wayland no hay forma estándar de leerla y la tarjeta
 * lleva el icono y el título.
 */
#include "dock.h"
#include "bar.h"
#include "opendock-build-config.h"
#include "spring.h"
#include "ventanas.h"
#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if HAVE_LAYER_SHELL
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#endif
#if HAVE_X11
#include "x11.h"
#include <X11/Xatom.h>
#endif

/* Medidas de DESIGN.md con el panel de 60 (se escalan con cfg->alto_dock). */
#define OD_MARGEN_INF     12.0
#define OD_SEP_BASE       12.0
#define OD_RELLENO_X      16.0
#define OD_RELLENO_Y      11.0
#define OD_LUPA_MAX       0.5
#define OD_LUPA_SIGMA     1.55
#define OD_HOLGURA        56     /* espacio sobre el panel para lupa y rebote */
#define OD_OCULTAR_MS     450
#define OD_SALTO_S        0.4
#define OD_PREVIA_ABRIR   400    /* ms con el cursor encima antes de la vista previa */
#define OD_PREVIA_CERRAR  300    /* ms tras salir del icono y de la vista previa */
#define OD_PREVIA_ANCHO   220    /* miniatura como mucho */
#define OD_PREVIA_ALTO    132
#define OD_PREVIA_MAX     8      /* tarjetas */

typedef struct {
    gchar *id;                 /* id .desktop, o app_id si no hay .desktop */
    GDesktopAppInfo *info;     /* puede ser NULL */
    GdkPaintable *icono;
    gboolean anclada;
    int n_ventanas;
    gboolean activa;
    OdMuelle escala;           /* lupa */
    OdMuelle ind_ancho;        /* indicador: 16 al frente, 6 abierta, 0 */
    OdMuelle ind_opacidad;
    double rebote_t;           /* segundos desde que empezó (<0 = no rebota) */
    int rebote_saltos;
    double rebote_amp;
    double x_centro;           /* calculado al dibujar */
    double ancho;
} OdItem;

/* Vista previa de las ventanas de una app (una por dock). */
typedef struct {
    GtkWidget *popover;
    GtkWidget *escala;         /* OdEscala: hace crecer el contenido desde el icono */
    GtkWidget *fila;           /* las tarjetas */
    gchar *id;                 /* app que enseña (NULL = cerrada) */
    gchar *pendiente;          /* app sobre la que espera el temporizador de abrir */
    gchar *suprimida;          /* tras un clic: no se abre hasta salir de ese icono */
    guint abrir, cerrar, refresco;
    gboolean cursor_encima;
    OdMuelle crecer, opacidad;
    guint tic;
    gint64 ultimo_us;
} OdPrevia;

/* Lo de cada monitor. */
typedef struct {
    GdkMonitor *monitor;
    GtkWidget *ventana;
    GtkWidget *lienzo;
    GtkWidget *menu;
    OdItem *item_menu;
    GPtrArray *items;          /* OdItem* */
    double cursor_x, cursor_y;
    gboolean cursor_dentro;
    OdMuelle oculto;           /* px que baja el panel (0 = visible) */
    guint temporizador_ocultar;
    guint tic;
    gint64 ultimo_us;
    cairo_rectangle_int_t region;   /* última región de entrada aplicada */
    gboolean pantalla_completa;
    OdPrevia previa;
} OdVistaDock;

static struct {
    OdConfig *cfg;
    OdBackendTipo backend;
    GHashTable *por_clase;     /* StartupWMClass / último trozo del id -> id .desktop */
    double icono, sep, panel;  /* medidas efectivas */
    GPtrArray *vistas;         /* OdVistaDock* */
    gboolean iniciado;
} g_d;

/* El dock con el que se está trabajando: lo fija cada entrada (señales, ticks) a
 * partir de sus datos; GTK corre en un solo hilo. */
static OdVistaDock *V;

#define PARA_CADA_DOCK(v) \
    for (guint i_ = 0; g_d.vistas && i_ < g_d.vistas->len; i_++) \
        for (OdVistaDock *v = g_ptr_array_index(g_d.vistas, i_); v; v = NULL)

/* ---- utilidades ---------------------------------------------------------- */

static gboolean animaciones_activas(void)
{
    gboolean si = TRUE;
    g_object_get(gtk_settings_get_default(), "gtk-enable-animations", &si, NULL);
    return si;
}

static gboolean avanzar(OdMuelle *m, double dt)
{
    if (!animaciones_activas()) {
        m->valor = m->objetivo;
        m->velocidad = 0;
        return FALSE;
    }
    return od_muelle_actualizar(m, dt);
}

static gchar *normalizar(const char *s)
{
    if (!s) return g_strdup("");
    gchar *sin = g_str_has_suffix(s, ".desktop") ? g_strndup(s, strlen(s) - 8) : g_strdup(s);
    gchar *min = g_ascii_strdown(sin, -1);
    g_free(sin);
    return min;
}

static const char *ultimo_trozo(const char *id)
{
    const char *p = strrchr(id, '.');
    return p ? p + 1 : id;
}

/* Índice "clase de ventana" -> id .desktop, para casar ventanas cuyo
 * app_id/WM_CLASS no es el nombre del .desktop (p. ej. "Gnome-terminal"). */
static void construir_indice(void)
{
    if (g_d.por_clase) g_hash_table_remove_all(g_d.por_clase);
    else g_d.por_clase = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GList *todas = g_app_info_get_all();
    for (GList *l = todas; l; l = l->next) {
        if (!G_IS_DESKTOP_APP_INFO(l->data)) continue;
        GDesktopAppInfo *d = l->data;
        const char *id = g_app_info_get_id(G_APP_INFO(d));
        if (!id) continue;
        const char *clase = g_desktop_app_info_get_startup_wm_class(d);
        if (clase) g_hash_table_insert(g_d.por_clase, g_ascii_strdown(clase, -1), g_strdup(id));
        gchar *n = normalizar(id);
        g_hash_table_insert(g_d.por_clase, g_strdup(ultimo_trozo(n)), g_strdup(id));
        g_hash_table_insert(g_d.por_clase, n, g_strdup(id));
    }
    g_list_free_full(todas, g_object_unref);
}

static void al_cambiar_apps(GAppInfoMonitor *m, gpointer datos)
{
    (void)m; (void)datos;
    construir_indice();
}

/* app_id / WM_CLASS -> id .desktop (o NULL si no hay ninguno). */
static const char *id_para_app(const char *app_id)
{
    if (!app_id || !*app_id) return NULL;
    gchar *n = normalizar(app_id);
    const char *id = g_hash_table_lookup(g_d.por_clase, n);
    if (!id) id = g_hash_table_lookup(g_d.por_clase, ultimo_trozo(n));
    g_free(n);
    return id;
}

static gboolean ventana_es_de(OdVentana *v, OdItem *it)
{
    const char *id = id_para_app(od_ventana_app_id(v));
    if (id) return g_strcmp0(id, it->id) == 0;
    return g_ascii_strcasecmp(od_ventana_app_id(v), it->id) == 0;
}

/* Icono a la escala del monitor de este dock. */
static GdkPaintable *cargar_icono(GDesktopAppInfo *info, const char *app_id)
{
    GtkIconTheme *tema = gtk_icon_theme_get_for_display(gdk_display_get_default());
    int tam = (int)(g_d.icono * (1 + OD_LUPA_MAX)) + 1;
    int escala = V && V->monitor ? gdk_monitor_get_scale_factor(V->monitor) : 1;
    GIcon *gicon = info ? g_app_info_get_icon(G_APP_INFO(info)) : NULL;
    GtkIconPaintable *p;
    if (gicon) {
        p = gtk_icon_theme_lookup_by_gicon(tema, gicon, tam, escala, GTK_TEXT_DIR_NONE, 0);
    } else {
        const char *nombres[] = { app_id, "application-x-executable", NULL };
        p = gtk_icon_theme_lookup_icon(tema, app_id && *app_id ? app_id : "application-x-executable",
            nombres + 1, tam, escala, GTK_TEXT_DIR_NONE, 0);
    }
    return GDK_PAINTABLE(p);
}

static OdItem *item_nuevo(const char *id, GDesktopAppInfo *info, gboolean anclada)
{
    OdItem *it = g_new0(OdItem, 1);
    it->id = g_strdup(id);
    it->info = info;
    it->icono = cargar_icono(info, id);
    it->anclada = anclada;
    it->rebote_t = -1;
    od_muelle_iniciar(&it->escala, 1.0, 520.0, 0.66);
    od_muelle_iniciar(&it->ind_ancho, 0, 300.0, OD_ZETA_SUAVE);
    od_muelle_iniciar(&it->ind_opacidad, 0, 300.0, OD_ZETA_SUAVE);
    return it;
}

static void item_liberar(gpointer p)
{
    OdItem *it = p;
    g_free(it->id);
    if (it->info) g_object_unref(it->info);
    if (it->icono) g_object_unref(it->icono);
    g_free(it);
}

static const char *nombre_item(OdItem *it)
{
    return it->info ? g_app_info_get_display_name(G_APP_INFO(it->info)) : it->id;
}

static OdItem *item_por_id(const char *id)
{
    for (guint i = 0; id && i < V->items->len; i++) {
        OdItem *it = g_ptr_array_index(V->items, i);
        if (g_strcmp0(it->id, id) == 0) return it;
    }
    return NULL;
}

/* ---- animación ----------------------------------------------------------- */

static gboolean fotograma(GtkWidget *w, GdkFrameClock *reloj, gpointer datos);

static void animar(void)
{
    if (V->tic) return;
    V->ultimo_us = 0;
    V->tic = gtk_widget_add_tick_callback(V->lienzo, fotograma, V, NULL);
}

static void rebotar(OdItem *it, int saltos, double amplitud)
{
    if (!animaciones_activas()) return;
    it->rebote_t = 0;
    it->rebote_saltos = saltos;
    it->rebote_amp = amplitud;
    animar();
}

/* Altura del rebote: saltos de 0,4 s, cada uno ×0,62 del anterior. */
static double altura_rebote(OdItem *it)
{
    if (it->rebote_t < 0) return 0;
    int k = (int)(it->rebote_t / OD_SALTO_S);
    if (k >= it->rebote_saltos) return 0;
    double fase = (it->rebote_t - k * OD_SALTO_S) / OD_SALTO_S;
    return it->rebote_amp * pow(0.62, k) * sin(G_PI * fase);
}

/* ---- disposición ----------------------------------------------------------- */

static double fondo_panel(int alto)
{
    return alto - OD_MARGEN_INF + V->oculto.valor;
}

/* Calcula x_centro y ancho de cada icono (con la lupa actual) y devuelve
 * la caja del panel. */
static void disponer(int ancho_widget, double *panel_x, double *panel_ancho)
{
    double total = 0;
    for (guint i = 0; i < V->items->len; i++) {
        OdItem *it = g_ptr_array_index(V->items, i);
        it->ancho = g_d.icono * it->escala.valor;
        total += it->ancho;
    }
    if (V->items->len > 1) total += g_d.sep * (V->items->len - 1);
    double x = ancho_widget / 2.0 - total / 2.0;
    for (guint i = 0; i < V->items->len; i++) {
        OdItem *it = g_ptr_array_index(V->items, i);
        it->x_centro = x + it->ancho / 2;
        x += it->ancho + g_d.sep;
    }
    *panel_ancho = total + 2 * OD_RELLENO_X;
    *panel_x = ancho_widget / 2.0 - *panel_ancho / 2.0;
}

/* Lupa: 1 + 0,5·exp(−((i − f)/1,55)²), con f calculado sobre la
 * disposición sin lupa para que no se retroalimente. */
static void fijar_lupa(void)
{
    int n = V->items->len;
    double paso = g_d.icono + g_d.sep;
    double inicio = gtk_widget_get_width(V->lienzo) / 2.0 - (n * g_d.icono + (n - 1) * g_d.sep) / 2.0;
    double f = (V->cursor_x - inicio - g_d.icono / 2.0) / paso;
    for (int i = 0; i < n; i++) {
        OdItem *it = g_ptr_array_index(V->items, i);
        double objetivo = 1.0;
        if (V->cursor_dentro) {
            double z = (i - f) / OD_LUPA_SIGMA;
            objetivo = 1.0 + OD_LUPA_MAX * exp(-z * z);
        }
        od_muelle_fijar_objetivo(&it->escala, objetivo);
    }
    animar();
}

static void actualizar_region(void)
{
    GdkSurface *s = gtk_native_get_surface(GTK_NATIVE(V->ventana));
    if (!s) return;
    int alto = gtk_widget_get_height(V->lienzo);
    double px, pa;
    disponer(gtk_widget_get_width(V->lienzo), &px, &pa);
    double fondo = fondo_panel(alto);
    cairo_rectangle_int_t r;
    r.x = (int)floor(px);
    r.width = (int)ceil(pa);
    /* Panel y lo que asoma de los iconos con lupa; si está oculto del todo,
     * una franja de 2 px en el borde para volver a sacarlo. */
    double arriba = fondo - g_d.panel - (V->cursor_dentro ? g_d.icono * OD_LUPA_MAX : 0);
    r.y = (int)floor(MAX(arriba, 0));
    r.height = (int)ceil(MIN(fondo, alto) - r.y);
    if (r.height < 2) { r.y = alto - 2; r.height = 2; }
    if (memcmp(&r, &V->region, sizeof r) == 0) return;
    V->region = r;
    cairo_region_t *reg = cairo_region_create_rectangle(&r);
    gdk_surface_set_input_region(s, reg);
    cairo_region_destroy(reg);
}

static gboolean fotograma(GtkWidget *w, GdkFrameClock *reloj, gpointer datos)
{
    (void)w;
    V = datos;
    gint64 ahora = gdk_frame_clock_get_frame_time(reloj);
    double dt = V->ultimo_us ? (ahora - V->ultimo_us) / 1e6 : 1.0 / 60.0;
    V->ultimo_us = ahora;
    if (dt > 0.05) dt = 0.05;

    gboolean sigue = avanzar(&V->oculto, dt);
    for (guint i = 0; i < V->items->len; i++) {
        OdItem *it = g_ptr_array_index(V->items, i);
        sigue |= avanzar(&it->escala, dt);
        sigue |= avanzar(&it->ind_ancho, dt);
        sigue |= avanzar(&it->ind_opacidad, dt);
        if (it->rebote_t >= 0) {
            it->rebote_t += dt;
            if (it->rebote_t >= it->rebote_saltos * OD_SALTO_S) it->rebote_t = -1;
            else sigue = TRUE;
        }
    }
    gtk_widget_queue_draw(V->lienzo);
    actualizar_region();
    if (!sigue) {
        V->tic = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* ---- lienzo (GtkWidget propio con snapshot) ------------------------------ */

#define OD_TIPO_LIENZO (od_lienzo_get_type())
G_DECLARE_FINAL_TYPE(OdLienzo, od_lienzo, OD, LIENZO, GtkWidget)

struct _OdLienzo {
    GtkWidget parent;
};

G_DEFINE_FINAL_TYPE(OdLienzo, od_lienzo, GTK_TYPE_WIDGET)

static OdVistaDock *vista_de(GtkWidget *w)
{
    return g_object_get_data(G_OBJECT(w), "od-vista");
}

static void rect_redondeado(GtkSnapshot *s, double x, double y, double w, double h, double r,
    const GdkRGBA *color)
{
    GskRoundedRect rr;
    graphene_rect_t caja = GRAPHENE_RECT_INIT((float)x, (float)y, (float)w, (float)h);
    gsk_rounded_rect_init_from_rect(&rr, &caja, (float)r);
    gtk_snapshot_push_rounded_clip(s, &rr);
    gtk_snapshot_append_color(s, color, &caja);
    gtk_snapshot_pop(s);
}

static void od_lienzo_snapshot(GtkWidget *w, GtkSnapshot *s)
{
    OdVistaDock *v = vista_de(w);
    if (!v) return;
    V = v;
    int ancho = gtk_widget_get_width(w), alto = gtk_widget_get_height(w);
    if (!V->items || V->items->len == 0) return;
    double px, pa;
    disponer(ancho, &px, &pa);
    double fondo = fondo_panel(alto);
    double arriba = fondo - g_d.panel;

    GdkRGBA panel = g_d.cfg->oled ? (GdkRGBA){ 0, 0, 0, 0.75f }
        : (GdkRGBA){ 0x1C / 255.0f, 0x1C / 255.0f, 0x1E / 255.0f, 0.75f };
    rect_redondeado(s, px, arriba, pa, g_d.panel, 0.30 * g_d.panel, &panel);

    double base = fondo - OD_RELLENO_Y * g_d.panel / 60.0;
    for (guint i = 0; i < V->items->len; i++) {
        OdItem *it = g_ptr_array_index(V->items, i);
        double tam = it->ancho;
        double y = base - tam - altura_rebote(it);
        if (it->icono) {
            gtk_snapshot_save(s);
            gtk_snapshot_translate(s, &GRAPHENE_POINT_INIT((float)(it->x_centro - tam / 2), (float)y));
            gdk_paintable_snapshot(it->icono, s, tam, tam);
            gtk_snapshot_restore(s);
        }
        /* Indicador: 3 px de alto a 5,5 px del borde del panel. */
        double iw = it->ind_ancho.valor;
        if (iw > 0.5 && it->ind_opacidad.valor > 0.01) {
            GdkRGBA blanco = { 1, 1, 1, (float)CLAMP(it->ind_opacidad.valor, 0, 1) };
            rect_redondeado(s, it->x_centro - iw / 2, fondo - 5.5 - 3, iw, 3, 1.5, &blanco);
        }
    }
}

/* El menú contextual y la vista previa cuelgan del lienzo: GTK exige colocarlos aquí. */
static void od_lienzo_size_allocate(GtkWidget *w, int ancho, int alto, int linea_base)
{
    (void)ancho; (void)alto; (void)linea_base;
    OdVistaDock *v = vista_de(w);
    if (!v) return;
    if (v->menu) gtk_popover_present(GTK_POPOVER(v->menu));
    if (v->previa.popover) gtk_popover_present(GTK_POPOVER(v->previa.popover));
}

static void od_lienzo_dispose(GObject *o)
{
    OdVistaDock *v = vista_de(GTK_WIDGET(o));
    if (v && v->menu && gtk_widget_get_parent(v->menu) == GTK_WIDGET(o)) {
        gtk_widget_unparent(v->menu);
        v->menu = NULL;
    }
    if (v && v->previa.popover && gtk_widget_get_parent(v->previa.popover) == GTK_WIDGET(o)) {
        gtk_widget_unparent(v->previa.popover);
        v->previa.popover = NULL;
    }
    G_OBJECT_CLASS(od_lienzo_parent_class)->dispose(o);
}

static void od_lienzo_class_init(OdLienzoClass *k)
{
    GTK_WIDGET_CLASS(k)->snapshot = od_lienzo_snapshot;
    GTK_WIDGET_CLASS(k)->size_allocate = od_lienzo_size_allocate;
    G_OBJECT_CLASS(k)->dispose = od_lienzo_dispose;
}

static void od_lienzo_init(OdLienzo *l)
{
    (void)l;
}

/* ---- OdEscala: un hijo que crece desde abajo al centro --------------------- */

#define OD_TIPO_ESCALA (od_escala_get_type())
G_DECLARE_FINAL_TYPE(OdEscala, od_escala, OD, ESCALA, GtkWidget)

struct _OdEscala {
    GtkWidget parent;
    GtkWidget *hijo;
    double k, alfa;
};

G_DEFINE_FINAL_TYPE(OdEscala, od_escala, GTK_TYPE_WIDGET)

static void od_escala_medir(GtkWidget *w, GtkOrientation o, int para, int *min, int *nat, int *bmin, int *bnat)
{
    OdEscala *e = OD_ESCALA(w);
    *min = *nat = 0;
    *bmin = *bnat = -1;
    if (e->hijo) gtk_widget_measure(e->hijo, o, para, min, nat, NULL, NULL);
}

static void od_escala_colocar(GtkWidget *w, int ancho, int alto, int linea_base)
{
    OdEscala *e = OD_ESCALA(w);
    if (e->hijo) gtk_widget_size_allocate(e->hijo, &(GtkAllocation){ 0, 0, ancho, alto }, linea_base);
}

static void od_escala_snapshot(GtkWidget *w, GtkSnapshot *s)
{
    OdEscala *e = OD_ESCALA(w);
    if (!e->hijo || e->alfa <= 0.001) return;
    const float cx = gtk_widget_get_width(w) / 2.0f, base = (float)gtk_widget_get_height(w);
    gtk_snapshot_save(s);
    gtk_snapshot_push_opacity(s, CLAMP(e->alfa, 0, 1));
    gtk_snapshot_translate(s, &GRAPHENE_POINT_INIT(cx, base));      /* el pie, junto al icono */
    gtk_snapshot_scale(s, (float)e->k, (float)e->k);
    gtk_snapshot_translate(s, &GRAPHENE_POINT_INIT(-cx, -base));
    gtk_widget_snapshot_child(w, e->hijo, s);
    gtk_snapshot_pop(s);
    gtk_snapshot_restore(s);
}

static void od_escala_dispose(GObject *o)
{
    OdEscala *e = OD_ESCALA(o);
    g_clear_pointer(&e->hijo, gtk_widget_unparent);
    G_OBJECT_CLASS(od_escala_parent_class)->dispose(o);
}

static void od_escala_class_init(OdEscalaClass *k)
{
    GTK_WIDGET_CLASS(k)->measure = od_escala_medir;
    GTK_WIDGET_CLASS(k)->size_allocate = od_escala_colocar;
    GTK_WIDGET_CLASS(k)->snapshot = od_escala_snapshot;
    G_OBJECT_CLASS(k)->dispose = od_escala_dispose;
}

static void od_escala_init(OdEscala *e)
{
    e->k = 1;
    e->alfa = 1;
}

static void od_escala_fijar(GtkWidget *w, double k, double alfa)
{
    OdEscala *e = OD_ESCALA(w);
    e->k = k;
    e->alfa = alfa;
    gtk_widget_queue_draw(w);
}

/* ---- ocultar --------------------------------------------------------------- */

static void previa_cerrar(void);

static double distancia_oculto(void)
{
    if (g_d.cfg->ocultar_dock == OD_OCULTAR_MITAD) return OD_MARGEN_INF + g_d.panel / 2;
    if (g_d.cfg->ocultar_dock == OD_OCULTAR_COMPLETO) return OD_MARGEN_INF + g_d.panel + 4;
    return 0;
}

static gboolean al_vencer_ocultar(gpointer datos)
{
    V = datos;
    V->temporizador_ocultar = 0;
    if (V->previa.id) return G_SOURCE_REMOVE;    /* con la vista previa abierta no baja */
    V->oculto.k = 140.0;
    V->oculto.zeta = 0.95;
    od_muelle_fijar_objetivo(&V->oculto, distancia_oculto());
    animar();
    return G_SOURCE_REMOVE;
}

static void mostrar_panel(void)
{
    if (V->temporizador_ocultar) g_source_remove(V->temporizador_ocultar);
    V->temporizador_ocultar = 0;
    V->oculto.k = 260.0;
    V->oculto.zeta = OD_ZETA_SUAVE;
    od_muelle_fijar_objetivo(&V->oculto, 0);
    animar();
}

static void programar_ocultar(void)
{
    if (g_d.cfg->ocultar_dock == OD_OCULTAR_NUNCA || V->temporizador_ocultar) return;
    V->temporizador_ocultar = g_timeout_add(OD_OCULTAR_MS, al_vencer_ocultar, V);
}

/* ---- ventanas de una app ------------------------------------------------- */

static GPtrArray *ventanas_de(OdItem *it)
{
    GPtrArray *r = g_ptr_array_new();
    GPtrArray *todas = od_ventanas_lista();
    for (guint i = 0; todas && i < todas->len; i++) {
        OdVentana *v = g_ptr_array_index(todas, i);
        if (ventana_es_de(v, it)) g_ptr_array_add(r, v);
    }
    return r;
}

static gboolean ventana_sigue(OdVentana *v)
{
    GPtrArray *todas = od_ventanas_lista();
    for (guint i = 0; v && todas && i < todas->len; i++)
        if (g_ptr_array_index(todas, i) == v) return TRUE;
    return FALSE;
}

/* ---- vista previa ---------------------------------------------------------- */

static gboolean previa_fotograma(GtkWidget *w, GdkFrameClock *reloj, gpointer datos)
{
    (void)w;
    OdVistaDock *v = datos;
    OdPrevia *p = &v->previa;
    gint64 ahora = gdk_frame_clock_get_frame_time(reloj);
    double dt = p->ultimo_us ? (ahora - p->ultimo_us) / 1e6 : 1.0 / 60.0;
    p->ultimo_us = ahora;
    if (dt > 0.05) dt = 0.05;
    gboolean sigue = avanzar(&p->crecer, dt);
    sigue |= avanzar(&p->opacidad, dt);
    od_escala_fijar(p->escala, p->crecer.valor, p->opacidad.valor);
    if (!sigue) { p->tic = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

static void previa_animar(void)
{
    OdPrevia *p = &V->previa;
    if (p->tic) return;
    p->ultimo_us = 0;
    p->tic = gtk_widget_add_tick_callback(p->escala, previa_fotograma, V, NULL);
}

static void previa_quitar_temporizadores(OdPrevia *p)
{
    if (p->abrir) g_source_remove(p->abrir);
    if (p->cerrar) g_source_remove(p->cerrar);
    if (p->refresco) g_source_remove(p->refresco);
    p->abrir = p->cerrar = p->refresco = 0;
}

static void previa_cerrar(void)
{
    OdPrevia *p = &V->previa;
    previa_quitar_temporizadores(p);
    g_clear_pointer(&p->pendiente, g_free);
    if (!p->id) return;
    g_clear_pointer(&p->id, g_free);
    p->cursor_encima = FALSE;
    gtk_popover_popdown(GTK_POPOVER(p->popover));
    od_ventanas_soltar_miniaturas();
    if (!V->cursor_dentro) programar_ocultar();
}

static void tarjeta_activar(GtkGestureClick *g, int n, double x, double y, gpointer datos)
{
    (void)n; (void)x; (void)y;
    V = datos;
    OdVentana *ven = g_object_get_data(G_OBJECT(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g))), "od-ventana");
    guint boton = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    if (!ventana_sigue(ven)) return;
    if (boton == GDK_BUTTON_MIDDLE) { od_ventana_cerrar(ven); return; }   /* como en Windows */
    od_ventana_activar(ven);
    OdItem *it = item_por_id(V->previa.id);
    if (it) rebotar(it, 1, 0.38 * g_d.icono);
    previa_cerrar();
}

static void tarjeta_cerrar(GtkButton *b, gpointer datos)
{
    V = datos;
    OdVentana *ven = g_object_get_data(G_OBJECT(b), "od-ventana");
    if (ventana_sigue(ven)) od_ventana_cerrar(ven);    /* la tarjeta se va al cerrarse de verdad */
}

static GtkWidget *crear_tarjeta(OdItem *it, OdVentana *ven)
{
    GtkWidget *t = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(t, "od-previa-tarjeta");
    if (od_ventana_activa(ven)) gtk_widget_add_css_class(t, "od-previa-activa");
    g_object_set_data(G_OBJECT(t), "od-ventana", ven);

    GtkWidget *cab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *ico = it->icono ? gtk_image_new_from_paintable(it->icono) : gtk_image_new_from_icon_name("application-x-executable");
    gtk_image_set_pixel_size(GTK_IMAGE(ico), 16);
    gtk_box_append(GTK_BOX(cab), ico);
    const char *titulo = od_ventana_titulo(ven);
    GtkWidget *lbl = gtk_label_new(titulo && *titulo ? titulo : nombre_item(it));
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 22);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_add_css_class(lbl, "od-previa-titulo");
    gtk_box_append(GTK_BOX(cab), lbl);
    GtkWidget *x = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(x, "flat");
    gtk_widget_add_css_class(x, "od-previa-cerrar");
    gtk_widget_set_tooltip_text(x, "Cerrar ventana");
    g_object_set_data(G_OBJECT(x), "od-ventana", ven);
    g_signal_connect(x, "clicked", G_CALLBACK(tarjeta_cerrar), V);
    gtk_box_append(GTK_BOX(cab), x);
    gtk_box_append(GTK_BOX(t), cab);

    /* cuerpo: la miniatura (X11) o, si no hay, el icono en grande */
    GtkWidget *cuerpo = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(cuerpo, "od-previa-cuerpo");
    gtk_widget_set_size_request(cuerpo, OD_PREVIA_ANCHO, OD_PREVIA_ALTO);
    GdkTexture *mini = od_ventana_miniatura(ven, OD_PREVIA_ANCHO, OD_PREVIA_ALTO);
    GtkWidget *pic;
    if (mini) {
        pic = gtk_picture_new_for_paintable(GDK_PAINTABLE(mini));
        gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_CONTAIN);
        g_object_unref(mini);
    } else {
        pic = it->icono ? gtk_image_new_from_paintable(it->icono) : gtk_image_new_from_icon_name("application-x-executable");
        gtk_image_set_pixel_size(GTK_IMAGE(pic), 56);
    }
    gtk_widget_set_vexpand(pic, TRUE);
    gtk_widget_set_hexpand(pic, TRUE);
    g_object_set_data(G_OBJECT(t), "od-cuerpo", cuerpo);
    gtk_box_append(GTK_BOX(cuerpo), pic);
    gtk_box_append(GTK_BOX(t), cuerpo);

    GtkGesture *clic = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(clic), 0);
    g_signal_connect(clic, "released", G_CALLBACK(tarjeta_activar), V);
    gtk_widget_add_controller(t, GTK_EVENT_CONTROLLER(clic));
    gtk_widget_set_cursor_from_name(t, "pointer");
    return t;
}

/* Pone las tarjetas de la app que enseña; FALSE si ya no tiene ventanas. */
static gboolean previa_rellenar(void)
{
    OdPrevia *p = &V->previa;
    OdItem *it = item_por_id(p->id);
    if (!it) return FALSE;
    GPtrArray *vs = ventanas_de(it);
    if (vs->len == 0) { g_ptr_array_free(vs, TRUE); return FALSE; }
    GtkWidget *hijo;
    while ((hijo = gtk_widget_get_first_child(p->fila))) gtk_box_remove(GTK_BOX(p->fila), hijo);
    for (guint i = 0; i < vs->len && i < OD_PREVIA_MAX; i++)
        gtk_box_append(GTK_BOX(p->fila), crear_tarjeta(it, g_ptr_array_index(vs, i)));
    g_ptr_array_free(vs, TRUE);

    int alto = gtk_widget_get_height(V->lienzo);
    GdkRectangle r = { (int)(it->x_centro - it->ancho / 2),
        (int)(fondo_panel(alto) - g_d.panel - g_d.icono * OD_LUPA_MAX - 8), (int)it->ancho, 4 };
    gtk_popover_set_pointing_to(GTK_POPOVER(p->popover), &r);
    return TRUE;
}

/* Las miniaturas se van poniendo al día mientras está abierta (1 vez por segundo). */
static gboolean previa_refrescar(gpointer datos)
{
    V = datos;
    OdPrevia *p = &V->previa;
    if (!p->id) { p->refresco = 0; return G_SOURCE_REMOVE; }
    for (GtkWidget *t = gtk_widget_get_first_child(p->fila); t; t = gtk_widget_get_next_sibling(t)) {
        OdVentana *ven = g_object_get_data(G_OBJECT(t), "od-ventana");
        GtkWidget *cuerpo = g_object_get_data(G_OBJECT(t), "od-cuerpo");
        if (!ventana_sigue(ven) || !cuerpo) continue;
        GdkTexture *mini = od_ventana_miniatura(ven, OD_PREVIA_ANCHO, OD_PREVIA_ALTO);
        if (!mini) continue;
        GtkWidget *pic = gtk_widget_get_first_child(cuerpo);
        if (GTK_IS_PICTURE(pic)) {
            gtk_picture_set_paintable(GTK_PICTURE(pic), GDK_PAINTABLE(mini));
        } else {
            if (pic) gtk_box_remove(GTK_BOX(cuerpo), pic);
            pic = gtk_picture_new_for_paintable(GDK_PAINTABLE(mini));
            gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_CONTAIN);
            gtk_widget_set_vexpand(pic, TRUE);
            gtk_widget_set_hexpand(pic, TRUE);
            gtk_box_append(GTK_BOX(cuerpo), pic);
        }
        g_object_unref(mini);
    }
    return G_SOURCE_CONTINUE;
}

/* Abre (o cambia a) la vista previa de una app; crece desde el icono. */
static void previa_abrir(OdItem *it)
{
    OdPrevia *p = &V->previa;
    if (p->cerrar) { g_source_remove(p->cerrar); p->cerrar = 0; }
    if (p->abrir) { g_source_remove(p->abrir); p->abrir = 0; }
    g_clear_pointer(&p->pendiente, g_free);
    const gboolean ya = p->id != NULL, otra = ya && g_strcmp0(p->id, it->id) != 0;
    if (ya && !otra) return;
    g_free(p->id);
    p->id = g_strdup(it->id);
    if (!previa_rellenar()) { g_clear_pointer(&p->id, g_free); return; }
    if (V->menu) gtk_popover_popdown(GTK_POPOVER(V->menu));
    /* con la del muelle de OpenDock: nace pequeña y transparente y crece con rebote;
     * al pasar a otra app, solo un pequeño saltito */
    od_muelle_iniciar(&p->crecer, otra ? 0.94 : 0.55, 300.0, OD_ZETA_NORMAL);
    od_muelle_iniciar(&p->opacidad, otra ? 0.6 : 0.0, 420.0, OD_ZETA_SUAVE);
    od_muelle_fijar_objetivo(&p->crecer, 1.0);
    od_muelle_fijar_objetivo(&p->opacidad, 1.0);
    if (!animaciones_activas()) { p->crecer.valor = 1; p->opacidad.valor = 1; }
    od_escala_fijar(p->escala, p->crecer.valor, p->opacidad.valor);
    gtk_popover_popup(GTK_POPOVER(p->popover));
    previa_animar();
    if (!p->refresco) p->refresco = g_timeout_add_seconds(1, previa_refrescar, V);
    mostrar_panel();
}

static gboolean al_vencer_abrir(gpointer datos)
{
    V = datos;
    OdPrevia *p = &V->previa;
    p->abrir = 0;
    OdItem *it = item_por_id(p->pendiente);
    g_clear_pointer(&p->pendiente, g_free);
    if (it && V->cursor_dentro) previa_abrir(it);
    return G_SOURCE_REMOVE;
}

static gboolean al_vencer_cerrar(gpointer datos)
{
    V = datos;
    V->previa.cerrar = 0;
    if (!V->previa.cursor_encima) previa_cerrar();
    return G_SOURCE_REMOVE;
}

static void previa_programar_cierre(void)
{
    OdPrevia *p = &V->previa;
    if (!p->id || p->cerrar || p->cursor_encima) return;
    p->cerrar = g_timeout_add(OD_PREVIA_CERRAR, al_vencer_cerrar, V);
}

/* El cursor se mueve por el dock: decide si abrir, cambiar o cerrar la vista previa. */
static void previa_seguir(OdItem *it)
{
    OdPrevia *p = &V->previa;
    const char *id = it ? it->id : NULL;
    if (p->suprimida && g_strcmp0(p->suprimida, id) != 0) g_clear_pointer(&p->suprimida, g_free);
    if (!it || it->n_ventanas == 0 || p->suprimida) {
        if (p->abrir) { g_source_remove(p->abrir); p->abrir = 0; g_clear_pointer(&p->pendiente, g_free); }
        previa_programar_cierre();
        return;
    }
    if (p->cerrar) { g_source_remove(p->cerrar); p->cerrar = 0; }
    if (p->id) {                    /* ya abierta: cambia al momento */
        if (g_strcmp0(p->id, id) != 0) previa_abrir(it);
        return;
    }
    if (g_strcmp0(p->pendiente, id) == 0) return;
    if (p->abrir) g_source_remove(p->abrir);
    g_free(p->pendiente);
    p->pendiente = g_strdup(id);
    p->abrir = g_timeout_add(OD_PREVIA_ABRIR, al_vencer_abrir, V);
}

static void al_entrar_previa(GtkEventControllerMotion *c, double x, double y, gpointer datos)
{
    (void)c; (void)x; (void)y;
    V = datos;
    V->previa.cursor_encima = TRUE;
    if (V->previa.cerrar) { g_source_remove(V->previa.cerrar); V->previa.cerrar = 0; }
}

static void al_salir_previa(GtkEventControllerMotion *c, gpointer datos)
{
    (void)c;
    V = datos;
    V->previa.cursor_encima = FALSE;
    previa_programar_cierre();
}

static void crear_previa(void)
{
    OdPrevia *p = &V->previa;
    p->popover = gtk_popover_new();
    gtk_popover_set_position(GTK_POPOVER(p->popover), GTK_POS_TOP);
    gtk_popover_set_has_arrow(GTK_POPOVER(p->popover), FALSE);
    gtk_popover_set_autohide(GTK_POPOVER(p->popover), FALSE);    /* sin agarrar el ratón: el dock sigue vivo */
    gtk_widget_add_css_class(p->popover, "od-previa");
    p->fila = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(p->fila, "od-previa-fila");
    /* aire alrededor: el rebote al crecer y la tarjeta señalada pasan un poco de 1 */
    gtk_widget_set_margin_start(p->fila, 6);
    gtk_widget_set_margin_end(p->fila, 6);
    gtk_widget_set_margin_top(p->fila, 6);
    gtk_widget_set_margin_bottom(p->fila, 4);
    p->escala = g_object_new(OD_TIPO_ESCALA, NULL);
    OD_ESCALA(p->escala)->hijo = p->fila;
    gtk_widget_set_parent(p->fila, p->escala);
    gtk_popover_set_child(GTK_POPOVER(p->popover), p->escala);
    gtk_widget_set_parent(p->popover, V->lienzo);

    GtkEventController *mov = gtk_event_controller_motion_new();
    g_signal_connect(mov, "enter", G_CALLBACK(al_entrar_previa), V);
    g_signal_connect(mov, "leave", G_CALLBACK(al_salir_previa), V);
    gtk_widget_add_controller(p->popover, mov);
    od_muelle_iniciar(&p->crecer, 1, 300.0, OD_ZETA_NORMAL);
    od_muelle_iniciar(&p->opacidad, 1, 420.0, OD_ZETA_SUAVE);
}

/* ---- entrada ------------------------------------------------------------- */

static OdItem *item_en(double x)
{
    for (guint i = 0; i < V->items->len; i++) {
        OdItem *it = g_ptr_array_index(V->items, i);
        if (fabs(x - it->x_centro) <= (it->ancho + g_d.sep) / 2) return it;
    }
    return NULL;
}

static void al_mover(GtkEventControllerMotion *c, double x, double y, gpointer datos)
{
    (void)c;
    V = datos;
    V->cursor_x = x;
    V->cursor_y = y;
    V->cursor_dentro = TRUE;
    mostrar_panel();
    fijar_lupa();
    previa_seguir(item_en(x));
}

static void al_salir(GtkEventControllerMotion *c, gpointer datos)
{
    (void)c;
    V = datos;
    V->cursor_dentro = FALSE;
    fijar_lupa();
    previa_seguir(NULL);
    g_clear_pointer(&V->previa.suprimida, g_free);
    if (!V->previa.id) programar_ocultar();
}

static void lanzar(OdItem *it)
{
    if (!it->info) return;
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
    GError *error = NULL;
    if (!g_app_info_launch(G_APP_INFO(it->info), NULL, G_APP_LAUNCH_CONTEXT(ctx), &error)) {
        g_message("opendock: no se pudo abrir %s: %s", it->id, error ? error->message : "?");
        g_clear_error(&error);
    } else {
        /* Abrir: 3 saltos de 0,75·icono. */
        rebotar(it, 3, 0.75 * g_d.icono);
    }
    g_object_unref(ctx);
}

/* Clic: sin ventanas abre la app; con una ventana al frente la minimiza, si no la
 * trae; con varias abre (o cierra) la vista previa para elegir, como en Windows. */
static void activar(OdItem *it)
{
    GPtrArray *vs = ventanas_de(it);
    OdPrevia *p = &V->previa;
    if (vs->len >= 2) {
        if (p->id && g_strcmp0(p->id, it->id) == 0) previa_cerrar();
        else previa_abrir(it);
    } else {
        previa_cerrar();
        g_free(p->suprimida);
        p->suprimida = g_strdup(it->id);     /* que no salte encima tras el clic */
        if (vs->len == 0) {
            lanzar(it);
        } else if (od_ventana_activa(g_ptr_array_index(vs, 0))) {
            od_ventana_minimizar(g_ptr_array_index(vs, 0));
        } else {
            od_ventana_activar(g_ptr_array_index(vs, 0));
            rebotar(it, 1, 0.38 * g_d.icono);   /* enfocar: 1 salto */
        }
    }
    g_ptr_array_free(vs, TRUE);
}

/* --- menú contextual --- */

static void reconstruir_todos(void);
#if HAVE_X11
static void colocar_x11(void);
#endif

static void menu_nueva_ventana(GtkButton *b, gpointer d)
{
    (void)b;
    V = d;
    gtk_popover_popdown(GTK_POPOVER(V->menu));
    if (V->item_menu) lanzar(V->item_menu);
}

/* Una ventana de la lista: la trae al frente (si sigue abierta). */
static void menu_ventana(GtkButton *b, gpointer d)
{
    V = d;
    gtk_popover_popdown(GTK_POPOVER(V->menu));
    OdVentana *v = g_object_get_data(G_OBJECT(b), "od-ventana");
    if (ventana_sigue(v)) od_ventana_activar(v);
}

/* Acción del .desktop ("Nueva ventana privada", "Redactar"...), como las listas
 * de la barra de tareas de Windows. */
static void menu_accion(GtkButton *b, gpointer d)
{
    V = d;
    gtk_popover_popdown(GTK_POPOVER(V->menu));
    const char *accion = g_object_get_data(G_OBJECT(b), "od-accion");
    if (!V->item_menu || !V->item_menu->info || !accion) return;
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
    g_desktop_app_info_launch_action(V->item_menu->info, accion, G_APP_LAUNCH_CONTEXT(ctx));
    g_object_unref(ctx);
    rebotar(V->item_menu, 3, 0.75 * g_d.icono);
}

static void menu_ajustes(GtkButton *b, gpointer d)
{
    (void)b;
    V = d;
    gtk_popover_popdown(GTK_POPOVER(V->menu));
    od_config_abrir(g_d.cfg);
}

/* --- Finalizar tarea: SIGTERM y, si a los 2 s sigue vivo, SIGKILL --- */

/* Momento de arranque del proceso (campo 22 de /proc/PID/stat): así no se mata a
 * otro proceso que haya heredado el número entretanto. */
static unsigned long long arranque_proceso(int pid)
{
    char ruta[64], linea[1024];
    g_snprintf(ruta, sizeof ruta, "/proc/%d/stat", pid);
    FILE *f = fopen(ruta, "r");
    if (!f) return 0;
    size_t n = fread(linea, 1, sizeof linea - 1, f);
    fclose(f);
    linea[n] = 0;
    const char *p = strrchr(linea, ')');       /* el nombre puede llevar espacios */
    if (!p) return 0;
    unsigned long long inicio = 0;
    int campo = 2;
    for (const char *q = p + 1; *q && campo < 22; q++) {
        if (*q == ' ') {
            campo++;
            if (campo == 22) inicio = g_ascii_strtoull(q + 1, NULL, 10);
        }
    }
    return inicio;
}

typedef struct { int pid; unsigned long long inicio; } OdTarea;

static gboolean rematar(gpointer datos)
{
    OdTarea *t = datos;
    if (kill(t->pid, 0) == 0 && arranque_proceso(t->pid) == t->inicio) kill(t->pid, SIGKILL);
    return G_SOURCE_REMOVE;
}

static gboolean pid_finalizable(int pid)
{
    return pid > 1 && pid != getpid();
}

static void menu_finalizar(GtkButton *b, gpointer d)
{
    (void)b;
    V = d;
    gtk_popover_popdown(GTK_POPOVER(V->menu));
    if (!V->item_menu) return;
    GPtrArray *vs = ventanas_de(V->item_menu);
    GArray *hechos = g_array_new(FALSE, FALSE, sizeof(int));
    for (guint i = 0; i < vs->len; i++) {
        int pid = od_ventana_pid(g_ptr_array_index(vs, i));
        gboolean repetido = FALSE;
        for (guint k = 0; k < hechos->len; k++) repetido |= g_array_index(hechos, int, k) == pid;
        if (!pid_finalizable(pid) || repetido) continue;
        g_array_append_val(hechos, pid);
        OdTarea *t = g_new(OdTarea, 1);
        t->pid = pid;
        t->inicio = arranque_proceso(pid);
        if (kill(pid, SIGTERM) == 0) g_timeout_add_full(G_PRIORITY_DEFAULT, 2000, rematar, t, g_free);
        else g_free(t);
    }
    g_array_free(hechos, TRUE);
    g_ptr_array_free(vs, TRUE);
}

static void menu_anclar(GtkButton *b, gpointer d)
{
    (void)b;
    V = d;
    gtk_popover_popdown(GTK_POPOVER(V->menu));
    OdItem *it = V->item_menu;
    if (!it || !it->info) return;
    GPtrArray *nueva = g_ptr_array_new_with_free_func(g_free);
    for (int i = 0; g_d.cfg->apps_ancladas && g_d.cfg->apps_ancladas[i]; i++)
        if (g_strcmp0(g_d.cfg->apps_ancladas[i], it->id) != 0)
            g_ptr_array_add(nueva, g_strdup(g_d.cfg->apps_ancladas[i]));
    if (!it->anclada) g_ptr_array_add(nueva, g_strdup(it->id));
    g_ptr_array_add(nueva, NULL);
    g_strfreev(g_d.cfg->apps_ancladas);
    g_d.cfg->apps_ancladas = (gchar **)g_ptr_array_free(nueva, FALSE);
    od_config_guardar_apps(g_d.cfg);
    V->item_menu = NULL;
    reconstruir_todos();       /* en todos los docks */
}

static void menu_cerrar(GtkButton *b, gpointer d)
{
    (void)b;
    V = d;
    gtk_popover_popdown(GTK_POPOVER(V->menu));
    if (!V->item_menu) return;
    GPtrArray *vs = ventanas_de(V->item_menu);
    for (guint i = 0; i < vs->len; i++) od_ventana_cerrar(g_ptr_array_index(vs, i));
    g_ptr_array_free(vs, TRUE);
}

/* Fila del menú: icono de 16 px (crece al pasar el cursor) y texto. */
static GtkWidget *fila_menu(const char *icono, GdkPaintable *pintable, const char *texto,
    GCallback cb, const char *clase)
{
    GtkWidget *b = gtk_button_new();
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_add_css_class(b, "od-menu-fila");
    if (clase) gtk_widget_add_css_class(b, clase);
    GtkWidget *caja = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *img = pintable ? gtk_image_new_from_paintable(pintable) : gtk_image_new_from_icon_name(icono);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
    gtk_box_append(GTK_BOX(caja), img);
    GtkWidget *lbl = gtk_label_new(texto);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 34);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_box_append(GTK_BOX(caja), lbl);
    gtk_button_set_child(GTK_BUTTON(b), caja);
    g_signal_connect(b, "clicked", cb, V);
    return b;
}

static void separador_menu(GtkWidget *caja)
{
    GtkWidget *s = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(s, "od-menu-sep");
    gtk_box_append(GTK_BOX(caja), s);
}

/* Como el de la barra de tareas de Windows: ventanas abiertas, acciones de la app,
 * nueva ventana, anclar, cerrar, finalizar tarea y ajustes del dock. */
static void abrir_menu(OdItem *it)
{
    previa_cerrar();
    V->item_menu = it;
    GPtrArray *vs = ventanas_de(it);
    GtkWidget *caja = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);

    GtkWidget *titulo = gtk_label_new(nombre_item(it));
    gtk_widget_add_css_class(titulo, "od-menu-cabecera");
    gtk_label_set_xalign(GTK_LABEL(titulo), 0);
    gtk_label_set_ellipsize(GTK_LABEL(titulo), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(caja), titulo);

    /* ventanas abiertas: la del frente, resaltada */
    for (guint i = 0; i < vs->len && i < 8; i++) {
        OdVentana *v = g_ptr_array_index(vs, i);
        const char *t = od_ventana_titulo(v);
        GtkWidget *f = fila_menu("view-restore-symbolic", it->icono, t && *t ? t : nombre_item(it),
            G_CALLBACK(menu_ventana), od_ventana_activa(v) ? "od-menu-activa" : NULL);
        g_object_set_data(G_OBJECT(f), "od-ventana", v);
        gtk_box_append(GTK_BOX(caja), f);
    }
    if (vs->len) separador_menu(caja);

    /* acciones del .desktop */
    const gchar * const *acciones = it->info ? g_desktop_app_info_list_actions(it->info) : NULL;
    int n_acciones = 0;
    for (int i = 0; acciones && acciones[i] && n_acciones < 6; i++) {
        gchar *nombre = g_desktop_app_info_get_action_name(it->info, acciones[i]);
        if (!nombre) continue;
        GtkWidget *f = fila_menu("media-playlist-consecutive-symbolic", NULL, nombre, G_CALLBACK(menu_accion), NULL);
        g_object_set_data_full(G_OBJECT(f), "od-accion", g_strdup(acciones[i]), g_free);
        gtk_box_append(GTK_BOX(caja), f);
        g_free(nombre);
        n_acciones++;
    }
    if (n_acciones) separador_menu(caja);

    if (it->info) {
        gtk_box_append(GTK_BOX(caja), vs->len
            ? fila_menu("window-new-symbolic", NULL, "Nueva ventana", G_CALLBACK(menu_nueva_ventana), NULL)
            : fila_menu(NULL, it->icono, nombre_item(it), G_CALLBACK(menu_nueva_ventana), NULL));
        gtk_box_append(GTK_BOX(caja), fila_menu(it->anclada ? "list-remove-symbolic" : "view-pin-symbolic",
            NULL, it->anclada ? "Quitar del dock" : "Anclar al dock", G_CALLBACK(menu_anclar), NULL));
    }
    if (vs->len) {
        separador_menu(caja);
        gtk_box_append(GTK_BOX(caja), fila_menu("window-close-symbolic", NULL,
            vs->len > 1 ? "Cerrar todas las ventanas" : "Cerrar ventana", G_CALLBACK(menu_cerrar), NULL));
        /* sólo si se sabe el proceso (X11); en Wayland el compositor no lo dice */
        gboolean hay_pid = FALSE;
        for (guint i = 0; i < vs->len && !hay_pid; i++)
            hay_pid = pid_finalizable(od_ventana_pid(g_ptr_array_index(vs, i)));
        if (hay_pid)
            gtk_box_append(GTK_BOX(caja), fila_menu("process-stop-symbolic", NULL, "Finalizar tarea",
                G_CALLBACK(menu_finalizar), "od-menu-peligro"));
    }
    separador_menu(caja);
    gtk_box_append(GTK_BOX(caja), fila_menu("emblem-system-symbolic", NULL, "Ajustes del dock…",
        G_CALLBACK(menu_ajustes), NULL));
    g_ptr_array_free(vs, TRUE);

    gtk_popover_set_child(GTK_POPOVER(V->menu), caja);
    int alto = gtk_widget_get_height(V->lienzo);
    GdkRectangle r = { (int)(it->x_centro - it->ancho / 2),
        (int)(fondo_panel(alto) - g_d.panel), (int)it->ancho, 4 };
    gtk_popover_set_pointing_to(GTK_POPOVER(V->menu), &r);
    gtk_popover_popup(GTK_POPOVER(V->menu));
}

static void al_soltar(GtkGestureClick *g, int n, double x, double y, gpointer datos)
{
    (void)n; (void)y;
    V = datos;
    OdItem *it = item_en(x);
    if (!it) return;
    guint boton = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    if (boton == GDK_BUTTON_SECONDARY) abrir_menu(it);
    else if (boton == GDK_BUTTON_MIDDLE) { previa_cerrar(); lanzar(it); }
    else activar(it);
}

static gboolean al_pedir_ayuda(GtkWidget *w, int x, int y, gboolean teclado, GtkTooltip *t, gpointer d)
{
    (void)w; (void)y; (void)teclado;
    V = d;
    OdItem *it = item_en(x);
    if (!it || V->previa.id) return FALSE;     /* con la vista previa, el título ya se ve */
    gtk_tooltip_set_text(t, nombre_item(it));
    return TRUE;
}

/* ---- lista de iconos ----------------------------------------------------- */

static void actualizar_estado_items(void)
{
    for (guint i = 0; i < V->items->len; i++) {
        OdItem *it = g_ptr_array_index(V->items, i);
        GPtrArray *vs = ventanas_de(it);
        it->n_ventanas = vs->len;
        it->activa = FALSE;
        for (guint j = 0; j < vs->len; j++)
            if (od_ventana_activa(g_ptr_array_index(vs, j))) it->activa = TRUE;
        g_ptr_array_free(vs, TRUE);
        /* Indicador: 16 de ancho al frente (0,95), 6 si está abierta (0,55). */
        od_muelle_fijar_objetivo(&it->ind_ancho, it->activa ? 16 : it->n_ventanas ? 6 : 0);
        od_muelle_fijar_objetivo(&it->ind_opacidad, it->activa ? 0.95 : it->n_ventanas ? 0.55 : 0);
    }
    animar();
}

/* La app al frente, para la barra (la de cualquier dock vale: todos tienen las mismas). */
static void avisar_app_activa(void)
{
    const char *nombre = NULL;
    gchar *icono = NULL;
    for (guint i = 0; i < V->items->len && !nombre; i++) {
        OdItem *it = g_ptr_array_index(V->items, i);
        if (!it->activa) continue;
        nombre = nombre_item(it);
        GIcon *gi = it->info ? g_app_info_get_icon(G_APP_INFO(it->info)) : NULL;
        if (gi && G_IS_THEMED_ICON(gi)) {
            const gchar * const *ns = g_themed_icon_get_names(G_THEMED_ICON(gi));
            if (ns && ns[0]) icono = g_strdup(ns[0]);
        }
    }
    od_bar_set_app_activa(nombre, icono);
    g_free(icono);
}

static void aplicar_visibilidad(void)
{
    /* Sin iconos no hay dock: un lienzo vacío no manda cuadro nuevo y el
     * compositor seguiría enseñando el último. Con pantalla completa encima, fuera. */
    gboolean ver = V->items->len > 0 && !V->pantalla_completa;
    if (!ver) previa_cerrar();
    if (!V->ventana || gtk_widget_get_visible(V->ventana) == ver) return;
    gtk_widget_set_visible(V->ventana, ver);
#if HAVE_X11
    if (ver && g_d.backend == OD_BACKEND_X11) colocar_x11();
#endif
}

/* Ancladas (en su orden) y luego las abiertas sin anclar, en el orden en
 * que fueron apareciendo. Los items que siguen conservan sus muelles. */
static void reconstruir(void)
{
    g_debug("dock: reconstruir (%u items antes)", V->items->len);
    GPtrArray *nuevos = g_ptr_array_new_with_free_func(item_liberar);
    GPtrArray *viejos = V->items;
    g_ptr_array_set_free_func(viejos, NULL);

    for (int i = 0; g_d.cfg->apps_ancladas && g_d.cfg->apps_ancladas[i]; i++) {
        const char *id = g_d.cfg->apps_ancladas[i];
        OdItem *it = NULL;
        for (guint j = 0; j < viejos->len; j++) {
            OdItem *v = g_ptr_array_index(viejos, j);
            if (v && g_strcmp0(v->id, id) == 0) { it = v; g_ptr_array_index(viejos, j) = NULL; break; }
        }
        if (!it) {
            GDesktopAppInfo *info = g_desktop_app_info_new(id);
            if (!info) continue;   /* app desinstalada: no dejamos un icono roto */
            it = item_nuevo(id, info, TRUE);
        }
        it->anclada = TRUE;
        g_ptr_array_add(nuevos, it);
    }

    /* Abiertas sin anclar: primero las que ya estaban (mismo orden). */
    GPtrArray *ventanas = od_ventanas_lista();
    for (guint j = 0; j < viejos->len; j++) {
        OdItem *v = g_ptr_array_index(viejos, j);
        if (!v) continue;
        v->anclada = FALSE;
        g_ptr_array_add(nuevos, v);
        g_ptr_array_index(viejos, j) = NULL;
    }
    for (guint i = 0; ventanas && i < ventanas->len; i++) {
        OdVentana *w = g_ptr_array_index(ventanas, i);
        const char *app = od_ventana_app_id(w);
        if (!app || !*app) continue;
        const char *id = id_para_app(app);
        const char *clave = id ? id : app;
        gboolean ya = FALSE;
        for (guint k = 0; k < nuevos->len && !ya; k++)
            ya = g_strcmp0(((OdItem *)g_ptr_array_index(nuevos, k))->id, clave) == 0;
        if (ya) continue;
        GDesktopAppInfo *info = id ? g_desktop_app_info_new(id) : NULL;
        g_ptr_array_add(nuevos, item_nuevo(clave, info, FALSE));
    }
    g_ptr_array_unref(viejos);
    V->items = nuevos;
    V->item_menu = NULL;       /* el menú abierto se rehace al volver a abrirlo */

    /* Fuera las no ancladas que ya no tienen ventanas. */
    for (guint i = V->items->len; i > 0; i--) {
        OdItem *it = g_ptr_array_index(V->items, i - 1);
        if (it->anclada) continue;
        GPtrArray *vs = ventanas_de(it);
        gboolean vacia = vs->len == 0;
        g_ptr_array_free(vs, TRUE);
        if (vacia) g_ptr_array_remove_index(V->items, i - 1);
    }
    actualizar_estado_items();
    if (V->cursor_dentro) fijar_lupa();
    /* la vista previa abierta sigue a sus ventanas (o se cierra si ya no quedan) */
    if (V->previa.id && !previa_rellenar()) previa_cerrar();
    aplicar_visibilidad();
}

static void reconstruir_todos(void)
{
    OdVistaDock *antes = V;
    PARA_CADA_DOCK(v) { V = v; reconstruir(); }
    if (g_d.vistas->len) { V = g_ptr_array_index(g_d.vistas, 0); avisar_app_activa(); }
    V = antes;
}

/* Ventanas abiertas, foco u orden de apilado cambiados: los docks se rehacen y
 * cada monitor mira si lo tapa una ventana a pantalla completa (su barra también). */
static void al_cambiar_ventanas(gpointer datos)
{
    (void)datos;
    PARA_CADA_DOCK(v) v->pantalla_completa = od_ventanas_pantalla_completa_en(v->monitor);
    reconstruir_todos();
    od_bar_pantalla_completa(od_ventanas_pantalla_completa_en);
}

/* ---- ventana ------------------------------------------------------------- */

static int alto_superficie(void)
{
    return (int)(OD_MARGEN_INF + g_d.panel + OD_HOLGURA);
}

static int zona_reservada(void)
{
    /* Media altura del panel: las ventanas maximizadas acaban a 42 px del borde. */
    return g_d.cfg->ocultar_dock == OD_OCULTAR_NUNCA ? (int)(OD_MARGEN_INF + g_d.panel / 2) : 0;
}

#if HAVE_X11
static GdkRectangle geometria_monitor(void)
{
    GdkRectangle geo = {0, 0, 1280, 800};
    if (V->monitor) gdk_monitor_get_geometry(V->monitor, &geo);
    return geo;
}

static void al_realizar_x11(GtkWidget *win, gpointer datos)
{
    V = datos;
    GdkSurface *s = gtk_native_get_surface(gtk_widget_get_native(win));
    if (!GDK_IS_X11_SURFACE(s)) return;
    Display *d = od_x11_display(s);
    Window xid = od_x11_ventana(s);
    GdkRectangle geo = geometria_monitor();
    Atom tipo = XInternAtom(d, "_NET_WM_WINDOW_TYPE", False);
    Atom dock = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(d, xid, tipo, XA_ATOM, 32, PropModeReplace, (unsigned char *)&dock, 1);
    /* El strut de abajo se mide desde el borde inferior de la pantalla X entera:
     * en un monitor que no llega hasta abajo, hay que sumar lo que queda debajo. */
    long strut[12] = { 0 };
    const int reserva = zona_reservada();
    const int debajo = DisplayHeight(d, DefaultScreen(d)) - (geo.y + geo.height);
    strut[3] = reserva ? MAX(0, debajo) + reserva : 0;   /* bottom */
    strut[10] = geo.x;                           /* bottom_start_x */
    strut[11] = geo.x + geo.width - 1;           /* bottom_end_x */
    XChangeProperty(d, xid, XInternAtom(d, "_NET_WM_STRUT_PARTIAL", False), XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)strut, 12);
    XChangeProperty(d, xid, XInternAtom(d, "_NET_WM_STRUT", False), XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)strut, 4);
}

static void colocar_x11(void)
{
    GdkSurface *s = gtk_native_get_surface(gtk_widget_get_native(V->ventana));
    if (!GDK_IS_X11_SURFACE(s)) return;
    GdkRectangle geo = geometria_monitor();
    int alto = alto_superficie();
    XMoveResizeWindow(od_x11_display(s), od_x11_ventana(s),
        geo.x, geo.y + geo.height - alto, geo.width, alto);
}
#endif

static void al_mapear(GtkWidget *w, gpointer datos)
{
    (void)w;
    V = datos;
    memset(&V->region, 0, sizeof V->region);
    actualizar_region();
}

static void cargar_css(GdkDisplay *display)
{
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window.od-dock-ventana { background: transparent; }"
        /* Menú del clic derecho con el material del dock, como en Windows. */
        "popover.od-menu-dock > contents { background-color: rgba(28,28,30,0.97); color: #FFFFFF;"
        "  border-radius: 14px; padding: 6px; border: 1px solid rgba(255,255,255,0.08);"
        "  box-shadow: 0 8px 24px rgba(0,0,0,0.45); }"
        ".od-menu-cabecera { color: #8E8E93; font-size: 12px; font-weight: 600; padding: 4px 10px 6px; }"
        ".od-menu-fila { min-height: 30px; padding: 0 10px; border-radius: 8px; color: #FFFFFF;"
        "  font-size: 13px; }"
        ".od-menu-fila:hover { background-color: rgba(255,255,255,0.10); }"
        ".od-menu-fila image { transition: transform 160ms cubic-bezier(0.34, 1.4, 0.64, 1); }"
        ".od-menu-fila:hover image { transform: scale(1.18); }"
        ".od-menu-activa { font-weight: 700; }"
        ".od-menu-peligro, .od-menu-peligro image { color: #FF453A; }"
        ".od-menu-sep { background-color: rgba(255,255,255,0.10); min-height: 1px; margin: 4px 6px; }"
        /* Vista previa: el mismo material; la tarjeta señalada crece 1,035 con rebote. */
        "popover.od-previa > contents { background-color: rgba(28,28,30,0.97); color: #FFFFFF;"
        "  border-radius: 18px; padding: 10px; border: 1px solid rgba(255,255,255,0.08); box-shadow: none; }"
        /* sin sombra ni márgenes: en X11 sin compositor lo transparente sale negro y taparía el dock */
        "popover.od-previa { margin: 0; padding: 0; box-shadow: none; }"
        ".od-previa-tarjeta { padding: 8px; border-radius: 12px; background-color: rgba(255,255,255,0.05);"
        "  transition: transform 220ms cubic-bezier(0.34, 1.56, 0.64, 1), background-color 160ms; }"
        ".od-previa-tarjeta:hover { transform: scale(1.035); background-color: rgba(255,255,255,0.12); }"
        ".od-previa-activa { box-shadow: inset 0 0 0 1px rgba(255,255,255,0.22); }"
        ".od-previa-titulo { font-size: 12px; font-weight: 600; color: #FFFFFF; }"
        ".od-previa-cuerpo { border-radius: 8px; background-color: rgba(0,0,0,0.25); }"
        ".od-previa-cerrar { min-width: 20px; min-height: 20px; padding: 0; border-radius: 10px;"
        "  color: #C7C7CC; opacity: 0; transition: opacity 120ms; }"
        ".od-previa-tarjeta:hover .od-previa-cerrar { opacity: 1; }"
        ".od-previa-cerrar:hover { background-color: #FF453A; color: #FFFFFF; }");
    gtk_style_context_add_provider_for_display(display,
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static OdVistaDock *crear_vista(GdkMonitor *mon)
{
    OdVistaDock *v = g_new0(OdVistaDock, 1);
    v->monitor = g_object_ref(mon);
    v->items = g_ptr_array_new_with_free_func(item_liberar);
    od_muelle_iniciar(&v->oculto, 0, 260.0, OD_ZETA_SUAVE);
    V = v;

    GtkWidget *win = gtk_window_new();
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_title(GTK_WINDOW(win), "OpenDock-Dock");
    gtk_widget_add_css_class(win, "od-dock-ventana");
    gtk_widget_set_size_request(win, -1, alto_superficie());

    GtkWidget *lienzo = g_object_new(OD_TIPO_LIENZO, NULL);
    g_object_set_data(G_OBJECT(lienzo), "od-vista", v);
    gtk_widget_set_hexpand(lienzo, TRUE);
    gtk_widget_set_size_request(lienzo, 1, -1);   /* ancho natural 0 = aviso de GDK */
    gtk_widget_set_vexpand(lienzo, TRUE);
    gtk_widget_set_has_tooltip(lienzo, TRUE);
    g_signal_connect(lienzo, "query-tooltip", G_CALLBACK(al_pedir_ayuda), v);
    gtk_window_set_child(GTK_WINDOW(win), lienzo);

    GtkEventController *mov = gtk_event_controller_motion_new();
    g_signal_connect(mov, "enter", G_CALLBACK(al_mover), v);
    g_signal_connect(mov, "motion", G_CALLBACK(al_mover), v);
    g_signal_connect(mov, "leave", G_CALLBACK(al_salir), v);
    gtk_widget_add_controller(lienzo, mov);

    GtkGesture *clic = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(clic), 0);
    g_signal_connect(clic, "released", G_CALLBACK(al_soltar), v);
    gtk_widget_add_controller(lienzo, GTK_EVENT_CONTROLLER(clic));

    v->menu = gtk_popover_new();
    gtk_popover_set_position(GTK_POPOVER(v->menu), GTK_POS_TOP);
    gtk_popover_set_has_arrow(GTK_POPOVER(v->menu), FALSE);
    gtk_widget_add_css_class(v->menu, "od-menu-dock");
    gtk_widget_set_parent(v->menu, lienzo);

    g_signal_connect(win, "map", G_CALLBACK(al_mapear), v);
    v->ventana = win;
    v->lienzo = lienzo;
    crear_previa();

#if HAVE_LAYER_SHELL
    if (g_d.backend == OD_BACKEND_WAYLAND && gtk_layer_is_supported()) {
        gtk_layer_init_for_window(GTK_WINDOW(win));
        gtk_layer_set_monitor(GTK_WINDOW(win), mon);
        gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_TOP);
        gtk_layer_set_namespace(GTK_WINDOW(win), "opendock-dock");
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_exclusive_zone(GTK_WINDOW(win), zona_reservada());
        gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    }
#endif
#if HAVE_X11
    if (g_d.backend == OD_BACKEND_X11) {
        GdkRectangle geo = geometria_monitor();
        gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
        gtk_widget_set_size_request(win, geo.width, alto_superficie());
        g_signal_connect(win, "realize", G_CALLBACK(al_realizar_x11), v);
    }
#endif
    gtk_widget_set_visible(win, FALSE);
    return v;
}

static void liberar_vista(gpointer p)
{
    OdVistaDock *v = p;
    OdVistaDock *antes = V;
    V = v;
    previa_cerrar();
    previa_quitar_temporizadores(&v->previa);
    g_free(v->previa.suprimida);
    if (v->temporizador_ocultar) g_source_remove(v->temporizador_ocultar);
    if (v->ventana) gtk_window_destroy(GTK_WINDOW(v->ventana));    /* el lienzo suelta menú y previa */
    g_ptr_array_unref(v->items);
    g_clear_object(&v->monitor);
    g_free(v);
    V = antes == v ? NULL : antes;
}

/* Monitores conectados o quitados: un dock por monitor. */
static void sincronizar_vistas(void)
{
    GListModel *mons = gdk_display_get_monitors(gdk_display_get_default());
    guint n = g_list_model_get_n_items(mons);
    for (guint i = g_d.vistas->len; i > 0; i--) {
        OdVistaDock *v = g_ptr_array_index(g_d.vistas, i - 1);
        gboolean sigue = FALSE;
        for (guint k = 0; k < n && !sigue; k++) {
            GdkMonitor *m = g_list_model_get_item(mons, k);
            sigue = m == v->monitor && gdk_monitor_is_valid(m);
            g_object_unref(m);
        }
        if (!sigue) g_ptr_array_remove_index(g_d.vistas, i - 1);
    }
    GPtrArray *orden = g_ptr_array_new();
    for (guint k = 0; k < n; k++) {
        GdkMonitor *m = g_list_model_get_item(mons, k);
        OdVistaDock *v = NULL;
        for (guint i = 0; i < g_d.vistas->len && !v; i++) {
            OdVistaDock *c = g_ptr_array_index(g_d.vistas, i);
            if (c->monitor == m) v = c;
        }
        if (!v) {
            v = crear_vista(m);
            reconstruir();             /* la muestra si hay algún icono */
            programar_ocultar();       /* si empieza oculto, que se esconda tras el primer vistazo */
        }
        g_ptr_array_add(orden, v);
        g_object_unref(m);
    }
    g_ptr_array_set_free_func(g_d.vistas, NULL);
    g_ptr_array_unref(g_d.vistas);
    g_d.vistas = g_ptr_array_new_with_free_func(liberar_vista);
    for (guint i = 0; i < orden->len; i++) g_ptr_array_add(g_d.vistas, g_ptr_array_index(orden, i));
    g_ptr_array_free(orden, TRUE);
}

static void al_cambiar_monitores(GListModel *l, guint pos, guint quitados, guint puestos, gpointer d)
{
    (void)l; (void)pos; (void)quitados; (void)puestos; (void)d;
    sincronizar_vistas();
}

void od_dock_iniciar(OdConfig *cfg, OdBackendTipo backend)
{
    if (g_d.iniciado) return;
    g_d.iniciado = TRUE;
    g_d.cfg = cfg;
    g_d.backend = backend;
    g_d.panel = cfg->alto_dock;
    g_d.icono = cfg->alto_dock - 2 * OD_RELLENO_Y * cfg->alto_dock / 60.0;  /* 60 -> 38 */
    g_d.sep = OD_SEP_BASE * cfg->alto_dock / 60.0;
    g_d.vistas = g_ptr_array_new_with_free_func(liberar_vista);

    construir_indice();
    g_signal_connect(g_app_info_monitor_get(), "changed", G_CALLBACK(al_cambiar_apps), NULL);
    GdkDisplay *display = gdk_display_get_default();
    cargar_css(display);

    od_ventanas_iniciar(backend, al_cambiar_ventanas, NULL);
    sincronizar_vistas();
    g_signal_connect(gdk_display_get_monitors(display), "items-changed",
        G_CALLBACK(al_cambiar_monitores), NULL);
    if (g_d.vistas->len) { V = g_ptr_array_index(g_d.vistas, 0); avisar_app_activa(); }
}
