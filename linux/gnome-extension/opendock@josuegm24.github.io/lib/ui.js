// Piezas de interfaz compartidas.

import Clutter from 'gi://Clutter';
import St from 'gi://St';

// Caja vertical. St.BoxLayout tenía la propiedad `vertical` (GNOME 45-47), luego
// también `orientation` (48) y las versiones nuevas quitaron `vertical`: pasarla al
// crear la caja lanza un error. La orientación del gestor de disposición vale en todas.
export function vbox(params = {}) {
    const box = new St.BoxLayout(params);
    box.layout_manager.orientation = Clutter.Orientation.VERTICAL;
    return box;
}
