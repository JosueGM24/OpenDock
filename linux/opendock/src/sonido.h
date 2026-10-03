/*
 * Sonido al llegar una notificación: los mismos 13 de Material Design
 * Sound Resources (Google, CC-BY 4.0) que la versión de Windows, tocados
 * con libcanberra. En config.ini: [general] sonido = silencio | sistema |
 * nota | eco | alerta | ... (por defecto "eco", el "Eco suave" de Windows)
 * y volumen_sonido = 0..100.
 */
#ifndef OPENDOCK_SONIDO_H
#define OPENDOCK_SONIDO_H

#include "config.h"

void od_sonido_iniciar(OdConfig *cfg);
void od_sonido_notificacion(void);

#endif
