/*
 * Muelle físico compartido por notch, barra, dock y centro de control.
 * Misma fórmula que la versión de Windows (ver DESIGN.md):
 *   a = (objetivo - x)*k - v*2*sqrt(k)*zeta
 * Integración de Euler semiimplícito con 2 subpasos de dt/2 (dt <= 0.05 s).
 */
#ifndef OPENDOCK_SPRING_H
#define OPENDOCK_SPRING_H

#include <glib.h>

typedef struct {
    double valor;
    double velocidad;
    double objetivo;
    double k;
    double zeta;
} OdMuelle;

/* Amortiguaciones estándar usadas en todo el proyecto (ver DESIGN.md). */
#define OD_ZETA_SUAVE   0.85
#define OD_ZETA_NORMAL  0.62
#define OD_ZETA_BOUNCY  0.40

void od_muelle_iniciar(OdMuelle *m, double valor_inicial, double k, double zeta);
void od_muelle_fijar_objetivo(OdMuelle *m, double objetivo);
/* Avanza el muelle 'dt' segundos partiéndolo en sub-pasos de como mucho 0.05 s.
 * Devuelve TRUE si todavía se está moviendo (hay que seguir llamando). */
gboolean od_muelle_actualizar(OdMuelle *m, double dt);

#endif
