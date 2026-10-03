// Esquinas redondeadas: 4 St.DrawingArea por monitor, sin entrada, que
// enmascaran (en negro / color del material) el cuadrado exterior a un
// cuarto de círculo de radio `corner-radius`. La misma técnica que usan
// "Rounded Window Corners" / "Rounded Corners".

import St from 'gi://St';
import Cairo from 'gi://cairo';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {isSystemDark} from './theme.js';

const CORNERS = ['top-left', 'top-right', 'bottom-left', 'bottom-right'];

export class CornerManager {
    constructor(settings) {
        this._settings = settings;
        this._areas = [];
        this._monitorsChangedId = null;
        this._settingsIds = [];
    }

    enable() {
        this._monitorsChangedId = Main.layoutManager.connect('monitors-changed',
            () => this._rebuild());
        for (const key of ['corner-radius', 'material', 'show-corners']) {
            this._settingsIds.push(this._settings.connect(`changed::${key}`,
                () => this._rebuild()));
        }
        this._rebuild();
    }

    disable() {
        if (this._monitorsChangedId) {
            Main.layoutManager.disconnect(this._monitorsChangedId);
            this._monitorsChangedId = null;
        }
        for (const id of this._settingsIds)
            this._settings.disconnect(id);
        this._settingsIds = [];
        this._destroyAll();
    }

    _destroyAll() {
        for (const area of this._areas)
            area.destroy();
        this._areas = [];
    }

    _colorFor(_corner) {
        const material = this._settings.get_string('material');
        if (material === 'glass')
            return [0x1C / 255, 0x1C / 255, 0x1E / 255];
        if (material === 'system') {
            return isSystemDark()
                ? [0x1C / 255, 0x1C / 255, 0x1E / 255]
                : [0xF2 / 255, 0xF2 / 255, 0xF7 / 255];
        }
        // oled (por defecto): negro puro en todas las esquinas.
        return [0, 0, 0];
    }

    _rebuild() {
        this._destroyAll();
        if (!this._settings.get_boolean('show-corners'))
            return;
        const radius = this._settings.get_int('corner-radius');
        if (radius <= 0)
            return;
        for (const monitor of Main.layoutManager.monitors) {
            for (const corner of CORNERS)
                this._areas.push(this._createCorner(monitor, corner, radius));
        }
    }

    _createCorner(monitor, corner, radius) {
        const [r, g, b] = this._colorFor(corner);
        const area = new St.DrawingArea({
            name: `opendock-corner-${corner}`,
            width: radius,
            height: radius,
            reactive: false,
            visible: true,
        });
        area.connect('repaint', widget => this._paint(widget, corner, radius, r, g, b));

        let x, y;
        switch (corner) {
        case 'top-left':
            x = monitor.x;
            y = monitor.y;
            break;
        case 'top-right':
            x = monitor.x + monitor.width - radius;
            y = monitor.y;
            break;
        case 'bottom-left':
            x = monitor.x;
            y = monitor.y + monitor.height - radius;
            break;
        case 'bottom-right':
        default:
            x = monitor.x + monitor.width - radius;
            y = monitor.y + monitor.height - radius;
            break;
        }

        Main.layoutManager.uiGroup.add_child(area);
        area.set_position(x, y);
        // Siempre por encima de las ventanas y de cualquier otra cosa en uiGroup.
        Main.layoutManager.uiGroup.set_child_above_sibling(area, null);
        area.queue_repaint();
        return area;
    }

    _paint(area, corner, radius, r, g, b) {
        const cr = area.get_context();

        // Limpiar el lienzo (transparente).
        cr.save();
        cr.setOperator(Cairo.Operator.CLEAR);
        cr.paint();
        cr.restore();

        // Relleno completo del cuadrado con el color del material.
        cr.setOperator(Cairo.Operator.OVER);
        cr.setSourceRGBA(r, g, b, 1);
        cr.rectangle(0, 0, radius, radius);
        cr.fill();

        // Se recorta (CLEAR) el cuarto de círculo que mira hacia el centro
        // de la pantalla, dejando opaca solo la "L" exterior: así el borde
        // del monitor se ve redondeado.
        let cx, cy;
        switch (corner) {
        case 'top-left':
            cx = radius; cy = radius; break;
        case 'top-right':
            cx = 0; cy = radius; break;
        case 'bottom-left':
            cx = radius; cy = 0; break;
        case 'bottom-right':
        default:
            cx = 0; cy = 0; break;
        }
        cr.setOperator(Cairo.Operator.CLEAR);
        cr.arc(cx, cy, radius, 0, 2 * Math.PI);
        cr.fill();

        cr.$dispose();
    }
}
