/*
 * Bandeja del sistema: anfitrión StatusNotifierItem (Tailscale, Discord,
 * Steam, Nextcloud, nm-applet...). Si nadie ofrece todavía
 * org.kde.StatusNotifierWatcher lo ofrecemos nosotros; si ya hay uno (el de
 * Plasma, waybar...), nos registramos como anfitrión en él.
 *
 * En la barra es un chevrón que despliega los iconos; clic los activa,
 * clic derecho abre su menú (com.canonical.dbusmenu) y el central hace la
 * activación secundaria.
 */
#ifndef OPENDOCK_BANDEJA_H
#define OPENDOCK_BANDEJA_H

#include <gtk/gtk.h>

/* Crea el botón del chevrón (oculto mientras no haya iconos). */
GtkWidget *od_bandeja_crear_boton(void);

/* Despliega o recoge los iconos (acción "bandeja", útil para un atajo). */
void od_bandeja_alternar(void);

#endif
