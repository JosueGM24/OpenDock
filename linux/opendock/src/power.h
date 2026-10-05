/*
 * Pequeñas consultas/acciones de sistema por D-Bus para la barra y el
 * centro de control: NetworkManager (Wi-Fi), UPower (batería), BlueZ
 * (Bluetooth) y logind (brillo, sin root). Todo de sólo lectura salvo
 * los toggles/deslizadores explícitos.
 */
#ifndef OPENDOCK_POWER_H
#define OPENDOCK_POWER_H

#include <glib.h>

/* NetworkManager: -1 si no se pudo consultar (sin NM en el sistema). */
int od_wifi_activado(void);          /* 0/1 */
void od_wifi_fijar_activado(gboolean on);
/* 1 si la conexión principal es por cable (Ethernet), 0 si no, -1 sin NM. */
int od_red_por_cable(void);
/* Llama a 'fn' cuando NetworkManager cambia (conexión principal, Wi-Fi...). */
void od_red_vigilar(void (*fn)(void));

/* UPower (DisplayDevice): deja -1 en *pct si no hay batería/servicio. */
void od_bateria_leer(int *pct, gboolean *cargando);

/* BlueZ: primer adaptador que encuentre. -1 si no hay servicio/adaptador. */
int od_bluetooth_activado(void);
void od_bluetooth_fijar_activado(gboolean on);

/* logind: brillo del primer dispositivo backlight que encuentre, 0..1.
 * -1 en *valor si no hay backlight controlable. */
void od_brillo_leer(double *valor);
void od_brillo_fijar(double valor);

#endif
