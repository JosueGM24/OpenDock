/*
 * Servidor D-Bus org.freedesktop.Notifications (spec 1.2) que alimenta el
 * notch. Si ya hay otro servidor de notificaciones en la sesión (el del
 * propio escritorio), no se reemplaza salvo que se pase --replace.
 */
#ifndef OPENDOCK_NOTIFICATIONS_H
#define OPENDOCK_NOTIFICATIONS_H

#include "config.h"
#include <glib.h>

void od_notificaciones_iniciar(OdConfig *cfg);

#endif
