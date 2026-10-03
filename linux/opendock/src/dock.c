/*
 * dock.c — el dock (ver dock.h y DESIGN.md).
 *
 * Una superficie transparente pegada abajo y a todo lo ancho; el panel y
 * los iconos se dibujan con GtkSnapshot en un widget propio (OdLienzo)
 * para poder escalarlos y moverlos en cada fotograma. La región de entrada
 * se limita al panel (y a lo que asoma de los iconos con lupa), así el
 * resto de la franja no tapa nada.
 *
 * Apps ancladas: cfg->apps_ancladas (ids .desktop). Ventanas abiertas:
 * ventanas.c (zwlr_foreign_toplevel en Wayland, EWMH en X11). Las apps se
 * lanzan con GAppInfo, nunca con un shell.
 */
#include "dock.h"
#include "bar.h"
#include "opendock-build-config.h"
#include "spring.h"
#include "ventanas.h"
#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>
#include <math.h>
#include <string.h>

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

typedef struct {
    OdConfig *cfg;
    OdBackendTipo backend;
    GtkWidget *ventana;
    GtkWidget *lienzo;
    GtkWidget *menu;
    GPtrArray *items;          /* OdItem* */
    double cursor_x, cursor_y;
    gboolean cursor_dentro;
    OdMuelle oculto;           /* px que baja el panel (0 = visible) */
    guint temporizador_ocultar;
    guint tic;
    gint64 ultimo_us;
    cairo_rectangle_int_t region;   /* última región de entrada aplicada */
    GHashTable *por_clase;     /* StartupWMClass / último trozo del id -> id .desktop */
    double icono, sep, panel;  /* medidas efectivas */
    gboolean iniciado;
} OdEstadoDock;

static OdEstadoDock g_d;

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

static GdkPaintable *cargar_icono(GDesktopAppInfo *info, const char *app_id)
{
    GtkIconTheme *tema = gtk_icon_theme_get_for_display(gdk_display_get_default());
    int tam = (int)(g_d.icono * (1 + OD_LUPA_MAX)) + 1;
    int escala = 1;
    GListModel *mons = gdk_display_get_monitors(gdk_display_get_default());
    if (g_list_model_get_n_items(mons) > 0) {
        GdkMonitor *m = g_list_model_get_item(mons, 0);
        escala = gdk_monitor_get_scale_factor(m);
        g_object_unref(m);
    }
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

/* ---- animación ----------------------------------------------------------- */

static gboolean fotograma(GtkWidget *w, GdkFrameClock *reloj, gpointer datos);

static void animar(void)
{
    if (g_d.tic) return;
    g_d.ultimo_us = 0;
    g_d.tic = gtk_widget_add_tick_callback(g_d.lienzo, fotograma, NULL, NULL);
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
    return alto - OD_MARGEN_INF + g_d.oculto.valor;
}

/* Calcula x_centro y ancho de cada icono (con la lupa actual) y devuelve
 * la caja del panel. */
static void disponer(int ancho_widget, double *panel_x, double *panel_ancho)
{
    double total = 0;
    for (guint i = 0; i < g_d.items->len; i++) {
        OdItem *it = g_ptr_array_index(g_d.items, i);
        it->ancho = g_d.icono * it->escala.valor;
        total += it->ancho;
    }
    if (g_d.items->len > 1) total += g_d.sep * (g_d.items->len - 1);
    double x = ancho_widget / 2.0 - total / 2.0;
    for (guint i = 0; i < g_d.items->len; i++) {
        OdItem *it = g_ptr_array_index(g_d.items, i);
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
    int n = g_d.items->len;
    double paso = g_d.icono + g_d.sep;
    double inicio = gtk_widget_get_width(g_d.lienzo) / 2.0 - (n * g_d.icono + (n - 1) * g_d.sep) / 2.0;
    double f = (g_d.cursor_x - inicio - g_d.icono / 2.0) / paso;
    for (int i = 0; i < n; i++) {
        OdItem *it = g_ptr_array_index(g_d.items, i);
        double objetivo = 1.0;
        if (g_d.cursor_dentro) {
            double z = (i - f) / OD_LUPA_SIGMA;
            objetivo = 1.0 + OD_LUPA_MAX * exp(-z * z);
        }
        od_muelle_fijar_objetivo(&it->escala, objetivo);
    }
    animar();
}

static void actualizar_region(void)
{
    GdkSurface *s = gtk_native_get_surface(GTK_NATIVE(g_d.ventana));
    if (!s) return;
    int alto = gtk_widget_get_height(g_d.lienzo);
    double px, pa;
    disponer(gtk_widget_get_width(g_d.lienzo), &px, &pa);
    double fondo = fondo_panel(alto);
    cairo_rectangle_int_t r;
    r.x = (int)floor(px);
    r.width = (int)ceil(pa);
    /* Panel y lo que asoma de los iconos con lupa; si está oculto del todo,
     * una franja de 2 px en el borde para volver a sacarlo. */
    double arriba = fondo - g_d.panel - (g_d.cursor_dentro ? g_d.icono * OD_LUPA_MAX : 0);
    r.y = (int)floor(MAX(arriba, 0));
    r.height = (int)ceil(MIN(fondo, alto) - r.y);
    if (r.height < 2) { r.y = alto - 2; r.height = 2; }
    if (memcmp(&r, &g_d.region, sizeof r) == 0) return;
    g_d.region = r;
    cairo_region_t *reg = cairo_region_create_rectangle(&r);
    gdk_surface_set_input_region(s, reg);
    cairo_region_destroy(reg);
}

static gboolean fotograma(GtkWidget *w, GdkFrameClock *reloj, gpointer datos)
{
    (void)w; (void)datos;
    gint64 ahora = gdk_frame_clock_get_frame_time(reloj);
    double dt = g_d.ultimo_us ? (ahora - g_d.ultimo_us) / 1e6 : 1.0 / 60.0;
    g_d.ultimo_us = ahora;
    if (dt > 0.05) dt = 0.05;

    gboolean sigue = avanzar(&g_d.oculto, dt);
    for (guint i = 0; i < g_d.items->len; i++) {
        OdItem *it = g_ptr_array_index(g_d.items, i);
        sigue |= avanzar(&it->escala, dt);
        sigue |= avanzar(&it->ind_ancho, dt);
        sigue |= avanzar(&it->ind_opacidad, dt);
        if (it->rebote_t >= 0) {
            it->rebote_t += dt;
            if (it->rebote_t >= it->rebote_saltos * OD_SALTO_S) it->rebote_t = -1;
            else sigue = TRUE;
        }
    }
    gtk_widget_queue_draw(g_d.lienzo);
    actualizar_region();
    if (!sigue) {
        g_d.tic = 0;
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
    int ancho = gtk_widget_get_width(w), alto = gtk_widget_get_height(w);
    if (!g_d.items || g_d.items->len == 0) return;
    double px, pa;
    disponer(ancho, &px, &pa);
    double fondo = fondo_panel(alto);
    double arriba = fondo - g_d.panel;

    GdkRGBA panel = g_d.cfg->oled ? (GdkRGBA){ 0, 0, 0, 0.75f }
        : (GdkRGBA){ 0x1C / 255.0f, 0x1C / 255.0f, 0x1E / 255.0f, 0.75f };
    rect_redondeado(s, px, arriba, pa, g_d.panel, 0.30 * g_d.panel, &panel);

    double base = fondo - OD_RELLENO_Y * g_d.panel / 60.0;
    for (guint i = 0; i < g_d.items->len; i++) {
        OdItem *it = g_ptr_array_index(g_d.items, i);
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

/* El menú contextual cuelga del lienzo: GTK exige colocarlo aquí. */
static void od_lienzo_size_allocate(GtkWidget *w, int ancho, int alto, int linea_base)
{
    (void)w; (void)ancho; (void)alto; (void)linea_base;
    if (g_d.menu) gtk_popover_present(GTK_POPOVER(g_d.menu));
}

static void od_lienzo_dispose(GObject *o)
{
    if (g_d.menu && gtk_widget_get_parent(g_d.menu) == GTK_WIDGET(o)) {
        gtk_widget_unparent(g_d.menu);
        g_d.menu = NULL;
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

/* ---- ocultar --------------------------------------------------------------- */

static double distancia_oculto(void)
{
    if (g_d.cfg->ocultar_dock == OD_OCULTAR_MITAD) return OD_MARGEN_INF + g_d.panel / 2;
    if (g_d.cfg->ocultar_dock == OD_OCULTAR_COMPLETO) return OD_MARGEN_INF + g_d.panel + 4;
    return 0;
}

static gboolean al_vencer_ocultar(gpointer datos)
{
    (void)datos;
    g_d.temporizador_ocultar = 0;
    g_d.oculto.k = 140.0;
    g_d.oculto.zeta = 0.95;
    od_muelle_fijar_objetivo(&g_d.oculto, distancia_oculto());
    animar();
    return G_SOURCE_REMOVE;
}

static void mostrar_panel(void)
{
    if (g_d.temporizador_ocultar) g_source_remove(g_d.temporizador_ocultar);
    g_d.temporizador_ocultar = 0;
    g_d.oculto.k = 260.0;
    g_d.oculto.zeta = OD_ZETA_SUAVE;
    od_muelle_fijar_objetivo(&g_d.oculto, 0);
    animar();
}

static void programar_ocultar(void)
{
    if (g_d.cfg->ocultar_dock == OD_OCULTAR_NUNCA || g_d.temporizador_ocultar) return;
    g_d.temporizador_ocultar = g_timeout_add(OD_OCULTAR_MS, al_vencer_ocultar, NULL);
}

/* ---- entrada ------------------------------------------------------------- */

static OdItem *item_en(double x)
{
    for (guint i = 0; i < g_d.items->len; i++) {
        OdItem *it = g_ptr_array_index(g_d.items, i);
        if (fabs(x - it->x_centro) <= (it->ancho + g_d.sep) / 2) return it;
    }
    return NULL;
}

static void al_mover(GtkEventControllerMotion *c, double x, double y, gpointer datos)
{
    (void)c; (void)datos;
    g_d.cursor_x = x;
    g_d.cursor_y = y;
    g_d.cursor_dentro = TRUE;
    mostrar_panel();
    fijar_lupa();
}

static void al_salir(GtkEventControllerMotion *c, gpointer datos)
{
    (void)c; (void)datos;
    g_d.cursor_dentro = FALSE;
    fijar_lupa();
    programar_ocultar();
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

/* Clic: sin ventanas abre la app; con una ventana al frente la minimiza;
 * con varias va pasando de una a otra; si no, trae la primera. */
static void activar(OdItem *it)
{
    GPtrArray *vs = ventanas_de(it);
    if (vs->len == 0) {
        lanzar(it);
    } else {
        int activa = -1;
        for (guint i = 0; i < vs->len; i++)
            if (od_ventana_activa(g_ptr_array_index(vs, i))) activa = (int)i;
        if (activa >= 0 && vs->len == 1) {
            od_ventana_minimizar(g_ptr_array_index(vs, 0));
        } else {
            guint sig = activa >= 0 ? (guint)(activa + 1) % vs->len : 0;
            od_ventana_activar(g_ptr_array_index(vs, sig));
            rebotar(it, 1, 0.38 * g_d.icono);   /* enfocar: 1 salto */
        }
    }
    g_ptr_array_free(vs, TRUE);
}

/* --- menú contextual --- */

static OdItem *g_item_menu;

static void reconstruir(void);

static void menu_nueva_ventana(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gtk_popover_popdown(GTK_POPOVER(g_d.menu));
    if (g_item_menu) lanzar(g_item_menu);
}

static void menu_anclar(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gtk_popover_popdown(GTK_POPOVER(g_d.menu));
    OdItem *it = g_item_menu;
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
    it->anclada = !it->anclada;
    reconstruir();
}

static void menu_cerrar(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gtk_popover_popdown(GTK_POPOVER(g_d.menu));
    if (!g_item_menu) return;
    GPtrArray *vs = ventanas_de(g_item_menu);
    for (guint i = 0; i < vs->len; i++) od_ventana_cerrar(g_ptr_array_index(vs, i));
    g_ptr_array_free(vs, TRUE);
}

static GtkWidget *boton_menu(const char *texto, GCallback cb)
{
    GtkWidget *b = gtk_button_new_with_label(texto);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(b)), GTK_ALIGN_START);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static void abrir_menu(OdItem *it)
{
    g_item_menu = it;
    GPtrArray *vs = ventanas_de(it);
    GtkWidget *caja = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *titulo = gtk_label_new(nombre_item(it));
    gtk_widget_add_css_class(titulo, "heading");
    gtk_widget_set_margin_bottom(titulo, 4);
    gtk_box_append(GTK_BOX(caja), titulo);
    if (it->info) {
        gtk_box_append(GTK_BOX(caja), boton_menu(vs->len ? "Nueva ventana" : "Abrir",
            G_CALLBACK(menu_nueva_ventana)));
        gtk_box_append(GTK_BOX(caja), boton_menu(it->anclada ? "Quitar del dock" : "Anclar al dock",
            G_CALLBACK(menu_anclar)));
    }
    if (vs->len)
        gtk_box_append(GTK_BOX(caja), boton_menu(vs->len > 1 ? "Cerrar todas" : "Cerrar",
            G_CALLBACK(menu_cerrar)));
    g_ptr_array_free(vs, TRUE);

    gtk_popover_set_child(GTK_POPOVER(g_d.menu), caja);
    int alto = gtk_widget_get_height(g_d.lienzo);
    GdkRectangle r = { (int)(it->x_centro - it->ancho / 2),
        (int)(fondo_panel(alto) - g_d.panel), (int)it->ancho, 4 };
    gtk_popover_set_pointing_to(GTK_POPOVER(g_d.menu), &r);
    gtk_popover_popup(GTK_POPOVER(g_d.menu));
}

static void al_soltar(GtkGestureClick *g, int n, double x, double y, gpointer datos)
{
    (void)n; (void)y; (void)datos;
    OdItem *it = item_en(x);
    if (!it) return;
    guint boton = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    if (boton == GDK_BUTTON_SECONDARY) abrir_menu(it);
    else if (boton == GDK_BUTTON_MIDDLE) lanzar(it);
    else activar(it);
}

static gboolean al_pedir_ayuda(GtkWidget *w, int x, int y, gboolean teclado, GtkTooltip *t, gpointer d)
{
    (void)w; (void)y; (void)teclado; (void)d;
    OdItem *it = item_en(x);
    if (!it) return FALSE;
    gtk_tooltip_set_text(t, nombre_item(it));
    return TRUE;
}

/* ---- lista de iconos ----------------------------------------------------- */

static void actualizar_estado_items(void)
{
    const char *nombre_activa = NULL, *icono_activa = NULL;
    gchar *icono_txt = NULL;
    for (guint i = 0; i < g_d.items->len; i++) {
        OdItem *it = g_ptr_array_index(g_d.items, i);
        GPtrArray *vs = ventanas_de(it);
        it->n_ventanas = vs->len;
        it->activa = FALSE;
        for (guint j = 0; j < vs->len; j++)
            if (od_ventana_activa(g_ptr_array_index(vs, j))) it->activa = TRUE;
        g_ptr_array_free(vs, TRUE);
        /* Indicador: 16 de ancho al frente (0,95), 6 si está abierta (0,55). */
        od_muelle_fijar_objetivo(&it->ind_ancho, it->activa ? 16 : it->n_ventanas ? 6 : 0);
        od_muelle_fijar_objetivo(&it->ind_opacidad, it->activa ? 0.95 : it->n_ventanas ? 0.55 : 0);
        if (it->activa) {
            nombre_activa = nombre_item(it);
            GIcon *gi = it->info ? g_app_info_get_icon(G_APP_INFO(it->info)) : NULL;
            if (gi && G_IS_THEMED_ICON(gi)) {
                const gchar * const *ns = g_themed_icon_get_names(G_THEMED_ICON(gi));
                if (ns && ns[0]) icono_txt = g_strdup(ns[0]);
            }
            icono_activa = icono_txt;
        }
    }
    od_bar_set_app_activa(nombre_activa, icono_activa);
    g_free(icono_txt);
    animar();
}

/* Ancladas (en su orden) y luego las abiertas sin anclar, en el orden en
 * que fueron apareciendo. Los items que siguen conservan sus muelles. */
static void reconstruir(void)
{
    g_debug("dock: reconstruir (%u items antes)", g_d.items->len);
    GPtrArray *nuevos = g_ptr_array_new_with_free_func(item_liberar);
    GPtrArray *viejos = g_d.items;
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
    g_d.items = nuevos;

    /* Fuera las no ancladas que ya no tienen ventanas. */
    for (guint i = g_d.items->len; i > 0; i--) {
        OdItem *it = g_ptr_array_index(g_d.items, i - 1);
        if (it->anclada) continue;
        GPtrArray *vs = ventanas_de(it);
        gboolean vacia = vs->len == 0;
        g_ptr_array_free(vs, TRUE);
        if (vacia) g_ptr_array_remove_index(g_d.items, i - 1);
    }
    actualizar_estado_items();
    if (g_d.cursor_dentro) fijar_lupa();
    for (guint i = 0; i < g_d.items->len; i++) {
        OdItem *it = g_ptr_array_index(g_d.items, i);
        g_debug("dock: %s anclada=%d ventanas=%d activa=%d", it->id, it->anclada,
            it->n_ventanas, it->activa);
    }
}

static void al_cambiar_ventanas(gpointer datos)
{
    (void)datos;
    reconstruir();
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
    GListModel *mons = gdk_display_get_monitors(gdk_display_get_default());
    if (g_list_model_get_n_items(mons) > 0) {
        GdkMonitor *m = g_list_model_get_item(mons, 0);
        gdk_monitor_get_geometry(m, &geo);
        g_object_unref(m);
    }
    return geo;
}

static void al_realizar_x11(GtkWidget *win, gpointer datos)
{
    (void)datos;
    GdkSurface *s = gtk_native_get_surface(gtk_widget_get_native(win));
    if (!GDK_IS_X11_SURFACE(s)) return;
    Display *d = od_x11_display(s);
    Window xid = od_x11_ventana(s);
    GdkRectangle geo = geometria_monitor();
    Atom tipo = XInternAtom(d, "_NET_WM_WINDOW_TYPE", False);
    Atom dock = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(d, xid, tipo, XA_ATOM, 32, PropModeReplace, (unsigned char *)&dock, 1);
    long strut[12] = { 0 };
    strut[3] = zona_reservada();                 /* bottom */
    strut[10] = geo.x;                           /* bottom_start_x */
    strut[11] = geo.x + geo.width - 1;           /* bottom_end_x */
    XChangeProperty(d, xid, XInternAtom(d, "_NET_WM_STRUT_PARTIAL", False), XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)strut, 12);
    XChangeProperty(d, xid, XInternAtom(d, "_NET_WM_STRUT", False), XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)strut, 4);
}

static void colocar_x11(void)
{
    GdkSurface *s = gtk_native_get_surface(gtk_widget_get_native(g_d.ventana));
    if (!GDK_IS_X11_SURFACE(s)) return;
    GdkRectangle geo = geometria_monitor();
    int alto = alto_superficie();
    XMoveResizeWindow(od_x11_display(s), od_x11_ventana(s),
        geo.x, geo.y + geo.height - alto, geo.width, alto);
}
#endif

static void al_mapear(GtkWidget *w, gpointer datos)
{
    (void)w; (void)datos;
    memset(&g_d.region, 0, sizeof g_d.region);
    actualizar_region();
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
    g_d.items = g_ptr_array_new_with_free_func(item_liberar);
    od_muelle_iniciar(&g_d.oculto, 0, 260.0, OD_ZETA_SUAVE);

    construir_indice();
    g_signal_connect(g_app_info_monitor_get(), "changed", G_CALLBACK(al_cambiar_apps), NULL);

    GtkWidget *win = gtk_window_new();
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_title(GTK_WINDOW(win), "OpenDock-Dock");
    gtk_widget_add_css_class(win, "od-dock-ventana");
    gtk_widget_set_size_request(win, -1, alto_superficie());

    GtkWidget *lienzo = g_object_new(OD_TIPO_LIENZO, NULL);
    gtk_widget_set_hexpand(lienzo, TRUE);
    gtk_widget_set_vexpand(lienzo, TRUE);
    gtk_widget_set_has_tooltip(lienzo, TRUE);
    g_signal_connect(lienzo, "query-tooltip", G_CALLBACK(al_pedir_ayuda), NULL);
    gtk_window_set_child(GTK_WINDOW(win), lienzo);

    GtkEventController *mov = gtk_event_controller_motion_new();
    g_signal_connect(mov, "enter", G_CALLBACK(al_mover), NULL);
    g_signal_connect(mov, "motion", G_CALLBACK(al_mover), NULL);
    g_signal_connect(mov, "leave", G_CALLBACK(al_salir), NULL);
    gtk_widget_add_controller(lienzo, mov);

    GtkGesture *clic = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(clic), 0);
    g_signal_connect(clic, "released", G_CALLBACK(al_soltar), NULL);
    gtk_widget_add_controller(lienzo, GTK_EVENT_CONTROLLER(clic));

    g_d.menu = gtk_popover_new();
    gtk_popover_set_position(GTK_POPOVER(g_d.menu), GTK_POS_TOP);
    gtk_popover_set_has_arrow(GTK_POPOVER(g_d.menu), FALSE);
    gtk_widget_set_parent(g_d.menu, lienzo);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, "window.od-dock-ventana { background: transparent; }");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(win),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    g_signal_connect(win, "map", G_CALLBACK(al_mapear), NULL);
    g_d.ventana = win;
    g_d.lienzo = lienzo;

#if HAVE_LAYER_SHELL
    if (backend == OD_BACKEND_WAYLAND && gtk_layer_is_supported()) {
        gtk_layer_init_for_window(GTK_WINDOW(win));
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
    if (backend == OD_BACKEND_X11) {
        GdkRectangle geo = geometria_monitor();
        gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
        gtk_widget_set_size_request(win, geo.width, alto_superficie());
        g_signal_connect(win, "realize", G_CALLBACK(al_realizar_x11), NULL);
    }
#endif

    od_ventanas_iniciar(backend, al_cambiar_ventanas, NULL);
    reconstruir();

    gtk_widget_set_visible(win, TRUE);
#if HAVE_X11
    if (backend == OD_BACKEND_X11) colocar_x11();
#endif
    /* Si empieza oculto, que se esconda tras el primer vistazo. */
    programar_ocultar();
}
