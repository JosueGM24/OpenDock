#!/usr/bin/env python3
"""
Icono de bandeja falso (StatusNotifierItem + com.canonical.dbusmenu) para
la prueba de sway: se registra en org.kde.StatusNotifierWatcher y anota en
el archivo de registro (primer argumento) cada propiedad que le leen y cada
método que le llaman. Sale a los N segundos (segundo argumento, 30 por
defecto) o con SIGTERM.
"""
import signal
import sys

from gi.repository import Gio, GLib

REGISTRO = open(sys.argv[1] if len(sys.argv) > 1 else "/dev/stdout", "a", buffering=1)
SEGUNDOS = int(sys.argv[2]) if len(sys.argv) > 2 else 30

XML = """
<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconThemePath" type="s" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <method name="Activate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
    <method name="SecondaryActivate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
    <method name="ContextMenu"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
    <method name="Scroll"><arg type="i" direction="in"/><arg type="s" direction="in"/></method>
    <signal name="NewIcon"/>
  </interface>
  <interface name="com.canonical.dbusmenu">
    <method name="GetLayout">
      <arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="as" direction="in"/>
      <arg type="u" direction="out"/><arg type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="AboutToShow"><arg type="i" direction="in"/><arg type="b" direction="out"/></method>
    <method name="Event">
      <arg type="i" direction="in"/><arg type="s" direction="in"/>
      <arg type="v" direction="in"/><arg type="u" direction="in"/>
    </method>
  </interface>
</node>
"""

PROPIEDADES = {
    "Category": GLib.Variant("s", "ApplicationStatus"),
    "Id": GLib.Variant("s", "opendock-prueba"),
    "Title": GLib.Variant("s", "Icono de prueba"),
    "Status": GLib.Variant("s", "Active"),
    "IconName": GLib.Variant("s", "dialog-information"),
    "IconThemePath": GLib.Variant("s", ""),
    "Menu": GLib.Variant("o", "/MenuBar"),
    "ItemIsMenu": GLib.Variant("b", False),
}


def entrada(i, props, hijos=()):
    return GLib.Variant("(ia{sv}av)", (i, props, list(hijos)))


def disposicion():
    hijos = [
        GLib.Variant("v", entrada(1, {"label": GLib.Variant("s", "_Abrir")})),
        GLib.Variant("v", entrada(2, {"type": GLib.Variant("s", "separator")})),
        GLib.Variant("v", entrada(3, {"label": GLib.Variant("s", "Salir")})),
    ]
    return (1, (0, {"children-display": GLib.Variant("s", "submenu")}, hijos))


def al_llamar(con, remitente, ruta, interfaz, metodo, params, inv):
    REGISTRO.write(f"llamada {interfaz}.{metodo} {params}\n")
    if metodo == "GetLayout":
        inv.return_value(GLib.Variant("(u(ia{sv}av))", disposicion()))
    elif metodo == "AboutToShow":
        inv.return_value(GLib.Variant("(b)", (False,)))
    else:
        inv.return_value(None)


def al_leer(con, remitente, ruta, interfaz, prop):
    REGISTRO.write(f"propiedad {prop}\n")
    return PROPIEDADES.get(prop)


def main():
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    nodo = Gio.DBusNodeInfo.new_for_xml(XML)
    bus.register_object("/StatusNotifierItem", nodo.interfaces[0], al_llamar, al_leer, None)
    bus.register_object("/MenuBar", nodo.interfaces[1], al_llamar, None, None)
    bus.call_sync("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                  "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                  GLib.Variant("(s)", (bus.get_unique_name(),)), None,
                  Gio.DBusCallFlags.NONE, 2000, None)
    REGISTRO.write(f"registrado como {bus.get_unique_name()}\n")
    bucle = GLib.MainLoop()
    GLib.timeout_add_seconds(SEGUNDOS, bucle.quit)
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, bucle.quit)
    bucle.run()


if __name__ == "__main__":
    main()
