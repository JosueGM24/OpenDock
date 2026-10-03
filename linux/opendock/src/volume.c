#include "volume.h"
#include "opendock-build-config.h"

#if HAVE_PULSE
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>

typedef struct {
    pa_glib_mainloop *bucle;
    pa_context *ctx;
    gboolean conectado;
    guint32 indice_sink_defecto;
    gchar *nombre_sink_defecto;
    pa_cvolume volumen;          /* con los canales reales del sink */
    int volumen_pct;
    gboolean silenciado;
    OdVolumenCambioFn cb;
    gpointer cb_datos;
} OdPulse;

static OdPulse g_pa;

static void notificar_cambio(void)
{
    if (g_pa.cb) g_pa.cb(g_pa.cb_datos);
}

static void al_info_sink(pa_context *c, const pa_sink_info *info, int eol, void *datos)
{
    (void)c; (void)datos;
    if (eol || !info) return;
    g_pa.volumen = info->volume;
    pa_volume_t vol = pa_cvolume_max(&info->volume);
    g_pa.volumen_pct = (int)((vol * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM);
    g_pa.silenciado = info->mute ? TRUE : FALSE;
    g_free(g_pa.nombre_sink_defecto);
    g_pa.nombre_sink_defecto = g_strdup(info->name);
    notificar_cambio();
}

static void pedir_info_sink_defecto(void)
{
    if (!g_pa.nombre_sink_defecto) return;
    pa_operation *op = pa_context_get_sink_info_by_name(g_pa.ctx,
        g_pa.nombre_sink_defecto, al_info_sink, NULL);
    if (op) pa_operation_unref(op);
}

static void al_info_servidor(pa_context *c, const pa_server_info *info, void *datos)
{
    (void)c; (void)datos;
    if (!info || !info->default_sink_name) return;
    g_free(g_pa.nombre_sink_defecto);
    g_pa.nombre_sink_defecto = g_strdup(info->default_sink_name);
    pedir_info_sink_defecto();
}

static void al_evento_subscripcion(pa_context *c, pa_subscription_event_type_t t,
    uint32_t idx, void *datos)
{
    (void)c; (void)idx; (void)datos;
    if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK ||
        (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SERVER) {
        pa_operation *op = pa_context_get_server_info(g_pa.ctx, al_info_servidor, NULL);
        if (op) pa_operation_unref(op);
    }
}

static void al_cambiar_estado(pa_context *c, void *datos)
{
    (void)datos;
    pa_context_state_t estado = pa_context_get_state(c);
    if (estado == PA_CONTEXT_READY) {
        g_pa.conectado = TRUE;
        pa_context_set_subscribe_callback(c, al_evento_subscripcion, NULL);
        pa_operation *op1 = pa_context_subscribe(c,
            PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL);
        if (op1) pa_operation_unref(op1);
        pa_operation *op2 = pa_context_get_server_info(c, al_info_servidor, NULL);
        if (op2) pa_operation_unref(op2);
    } else if (estado == PA_CONTEXT_FAILED || estado == PA_CONTEXT_TERMINATED) {
        g_pa.conectado = FALSE;
    }
}

void od_volumen_iniciar(void)
{
    g_pa.volumen_pct = -1;
    /* libpulse dentro del bucle de GLib: sin hilos ni temporizadores, sólo
     * se despierta cuando PulseAudio/PipeWire manda algo. */
    g_pa.bucle = pa_glib_mainloop_new(NULL);
    if (!g_pa.bucle) return;
    pa_mainloop_api *api = pa_glib_mainloop_get_api(g_pa.bucle);
    g_pa.ctx = pa_context_new(api, "OpenDock");
    if (!g_pa.ctx) return;
    pa_context_set_state_callback(g_pa.ctx, al_cambiar_estado, NULL);
    if (pa_context_connect(g_pa.ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        g_warning("opendock: no se pudo conectar con PulseAudio/PipeWire");
        return;
    }
}

int od_volumen_obtener(void)
{
    return g_pa.volumen_pct;
}

gboolean od_volumen_silenciado(void)
{
    return g_pa.silenciado;
}

void od_volumen_fijar(int porcentaje)
{
    if (!g_pa.conectado || !g_pa.nombre_sink_defecto || !pa_cvolume_valid(&g_pa.volumen)) return;
    porcentaje = CLAMP(porcentaje, 0, 150);
    /* Escala todos los canales manteniendo el balance del sink. */
    pa_cvolume cv = g_pa.volumen;
    pa_cvolume_scale(&cv, (pa_volume_t)((porcentaje * PA_VOLUME_NORM) / 100));
    pa_operation *op = pa_context_set_sink_volume_by_name(g_pa.ctx,
        g_pa.nombre_sink_defecto, &cv, NULL, NULL);
    if (op) pa_operation_unref(op);
    g_pa.volumen_pct = porcentaje;
}

void od_volumen_alternar_silencio(void)
{
    if (!g_pa.conectado || !g_pa.nombre_sink_defecto) return;
    g_pa.silenciado = !g_pa.silenciado;
    pa_operation *op = pa_context_set_sink_mute_by_name(g_pa.ctx,
        g_pa.nombre_sink_defecto, g_pa.silenciado, NULL, NULL);
    if (op) pa_operation_unref(op);
}

void od_volumen_conectar_cambio(OdVolumenCambioFn fn, gpointer datos)
{
    g_pa.cb = fn;
    g_pa.cb_datos = datos;
}

#else /* !HAVE_PULSE */

void od_volumen_iniciar(void) { }
int od_volumen_obtener(void) { return -1; }
gboolean od_volumen_silenciado(void) { return FALSE; }
void od_volumen_fijar(int porcentaje) { (void)porcentaje; }
void od_volumen_alternar_silencio(void) { }
void od_volumen_conectar_cambio(OdVolumenCambioFn fn, gpointer datos) { (void)fn; (void)datos; }

#endif
