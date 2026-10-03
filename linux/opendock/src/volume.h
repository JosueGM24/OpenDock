/*
 * Volumen vía PipeWire/PulseAudio (libpulse; pipewire-pulse lo atiende
 * igual). Dependencia opcional: si no está disponible (HAVE_PULSE=0),
 * las funciones no hacen nada y el volumen no aparece en la barra.
 */
#ifndef OPENDOCK_VOLUME_H
#define OPENDOCK_VOLUME_H

#include <glib.h>

void od_volumen_iniciar(void);
/* 0..100, o -1 si todavía no se sabe / no hay backend. */
int od_volumen_obtener(void);
gboolean od_volumen_silenciado(void);
void od_volumen_fijar(int porcentaje);
void od_volumen_alternar_silencio(void);

typedef void (*OdVolumenCambioFn)(gpointer datos);
/* Se llama cuando cambia el volumen o el silencio (para refrescar la UI). */
void od_volumen_conectar_cambio(OdVolumenCambioFn fn, gpointer datos);

#endif
