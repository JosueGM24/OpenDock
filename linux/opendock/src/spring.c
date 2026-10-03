#include "spring.h"
#include <math.h>

void od_muelle_iniciar(OdMuelle *m, double valor_inicial, double k, double zeta)
{
    m->valor = valor_inicial;
    m->velocidad = 0.0;
    m->objetivo = valor_inicial;
    m->k = k;
    m->zeta = zeta;
}

void od_muelle_fijar_objetivo(OdMuelle *m, double objetivo)
{
    m->objetivo = objetivo;
}

static void od_muelle_subpaso(OdMuelle *m, double dt)
{
    double a = (m->objetivo - m->valor) * m->k - m->velocidad * 2.0 * sqrt(m->k) * m->zeta;
    m->velocidad += a * dt;
    m->valor += m->velocidad * dt;
}

gboolean od_muelle_actualizar(OdMuelle *m, double dt)
{
    while (dt > 0.0) {
        double paso = dt > 0.05 ? 0.05 : dt;
        od_muelle_subpaso(m, paso * 0.5);
        od_muelle_subpaso(m, paso * 0.5);
        dt -= paso;
    }
    gboolean en_movimiento = fabs(m->objetivo - m->valor) > 0.001 || fabs(m->velocidad) > 0.001;
    if (!en_movimiento) {
        m->valor = m->objetivo;
        m->velocidad = 0.0;
    }
    return en_movimiento;
}
