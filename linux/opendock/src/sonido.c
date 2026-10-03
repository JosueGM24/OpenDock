/*
 * sonido.c — sonido de notificación (ver sonido.h).
 *
 * Los .wav se buscan, por orden, en $OPENDOCK_SONIDOS, en el directorio
 * de datos instalado (OPENDOCK_DIR_SONIDOS) y junto al código fuente
 * (para probar desde build/ sin instalar).
 */
#include "sonido.h"
#include "opendock-build-config.h"
#include <gio/gio.h>
#include <math.h>

#if HAVE_CANBERRA
#include <canberra.h>
#endif

static const struct { const char *clave; const char *archivo; } SONIDOS[] = {
    { "nota",       "simple1.wav" },
    { "eco",        "simple2.wav" },   /* "Eco suave", el de por defecto */
    { "alerta",     "alert.wav" },
    { "destello",   "deco1.wav" },
    { "cascada",    "deco2.wav" },
    { "aviso",      "intense.wav" },
    { "ambiente",   "ambient.wav" },
    { "confirmar",  "confirm.wav" },
    { "logro",      "celeb1.wav" },
    { "brindis",    "celeb2.wav" },
    { "fanfarria",  "celeb3.wav" },
    { "fiesta",     "party.wav" },
    { "completado", "complete.wav" },
};

static struct {
    OdConfig *cfg;
    gchar *ruta;           /* .wav elegido, o NULL (silencio / sistema / no hallado) */
    gboolean sistema;
#if HAVE_CANBERRA
    ca_context *ctx;
#endif
} g_s;

static gchar *buscar_archivo(const char *archivo)
{
    const char *env = g_getenv("OPENDOCK_SONIDOS");
    gchar *candidatos[4] = { NULL };
    int n = 0;
    if (env && *env) candidatos[n++] = g_build_filename(env, archivo, NULL);
    candidatos[n++] = g_build_filename(OPENDOCK_DIR_SONIDOS, archivo, NULL);
    /* build/opendock -> ../../../sounds (raíz del repositorio). */
    gchar *exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe) {
        gchar *dir = g_path_get_dirname(exe);
        candidatos[n++] = g_build_filename(dir, "..", "..", "..", "sounds", archivo, NULL);
        g_free(dir);
        g_free(exe);
    }
    gchar *hallado = NULL;
    for (int i = 0; i < n; i++) {
        if (!hallado && g_file_test(candidatos[i], G_FILE_TEST_IS_REGULAR))
            hallado = g_canonicalize_filename(candidatos[i], NULL);
        g_free(candidatos[i]);
    }
    return hallado;
}

void od_sonido_iniciar(OdConfig *cfg)
{
    g_s.cfg = cfg;
    const char *elegido = cfg->sonido ? cfg->sonido : "eco";
    if (g_strcmp0(elegido, "silencio") == 0) return;
    if (g_strcmp0(elegido, "sistema") == 0) {
        g_s.sistema = TRUE;
    } else {
        for (gsize i = 0; i < G_N_ELEMENTS(SONIDOS); i++)
            if (g_strcmp0(elegido, SONIDOS[i].clave) == 0) g_s.ruta = buscar_archivo(SONIDOS[i].archivo);
        if (!g_s.ruta) {
            g_message("opendock: no se encontró el sonido \"%s\"; las notificaciones irán sin sonido",
                elegido);
            return;
        }
    }
#if HAVE_CANBERRA
    if (ca_context_create(&g_s.ctx) != CA_SUCCESS) g_s.ctx = NULL;
    else ca_context_change_props(g_s.ctx, CA_PROP_APPLICATION_NAME, "OpenDock",
        CA_PROP_APPLICATION_ID, "io.github.josuegm24.OpenDock", NULL);
#else
    g_message("opendock: compilado sin libcanberra; las notificaciones irán sin sonido");
#endif
}

void od_sonido_notificacion(void)
{
#if HAVE_CANBERRA
    if (!g_s.ctx || (!g_s.ruta && !g_s.sistema)) return;
    int vol = CLAMP(g_s.cfg->volumen_sonido, 0, 100);
    if (vol == 0) return;
    /* canberra.volume va en dB: 100 % = 0 dB. */
    char db[16];
    g_ascii_formatd(db, sizeof db, "%.1f", 20.0 * log10(vol / 100.0));
    if (g_s.sistema)
        ca_context_play(g_s.ctx, 0, CA_PROP_EVENT_ID, "message-new-instant",
            CA_PROP_CANBERRA_VOLUME, db, NULL);
    else
        ca_context_play(g_s.ctx, 0, CA_PROP_MEDIA_FILENAME, g_s.ruta,
            CA_PROP_CANBERRA_VOLUME, db, CA_PROP_EVENT_DESCRIPTION, "Notificación", NULL);
#endif
}
