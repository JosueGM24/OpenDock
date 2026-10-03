// Barra superior: alto 28, material y formato de hora "Jue 2 oct  14:05"
// (ver linux/DESIGN.md). Las Quick Settings de GNOME se mantienen como
// centro de control; aquí solo se restylea, no se reimplementan.

import St from 'gi://St';
import GLib from 'gi://GLib';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {isSystemDark} from './theme.js';

export const PANEL_HEIGHT = 28;

const MATERIAL_CLASSES = [
    'opendock-panel-oled',
    'opendock-panel-glass',
    'opendock-panel-system-dark',
    'opendock-panel-system-light',
];

const WEEKDAYS = ['Dom', 'Lun', 'Mar', 'Mié', 'Jue', 'Vie', 'Sáb'];
const MONTHS = ['ene', 'feb', 'mar', 'abr', 'may', 'jun', 'jul', 'ago', 'sep', 'oct', 'nov', 'dic'];

export function formatClock(date = new Date()) {
    const weekday = WEEKDAYS[date.getDay()];
    const day = date.getDate();
    const month = MONTHS[date.getMonth()];
    const hh = String(date.getHours()).padStart(2, '0');
    const mm = String(date.getMinutes()).padStart(2, '0');
    return `${weekday} ${day} ${month}  ${hh}:${mm}`;
}

export class PanelStyler {
    constructor(settings) {
        this._settings = settings;
        this._settingsIds = [];
        this._clockSourceId = null;
        this._clockLabel = null;
        this._styleApplied = false;
    }

    enable() {
        this._settingsIds.push(this._settings.connect('changed::material',
            () => this._applyStyle()));
        this._applyStyle();
        this._startClock();
    }

    disable() {
        for (const id of this._settingsIds)
            this._settings.disconnect(id);
        this._settingsIds = [];
        this._stopClock();
        this._removeStyle();
    }

    _applyStyle() {
        // Clases de stylesheet.css en vez de set_style(): el alto y el color
        // de fondo pasan así por la cascada real de temas, que es la que de
        // verdad gana sobre el min-height que algunos temas (p.ej. el de
        // Fedora) fijan para #panel; un estilo "en línea" no lo conseguía.
        const material = this._settings.get_string('material');
        const target = material === 'system'
            ? `opendock-panel-system-${isSystemDark() ? 'dark' : 'light'}`
            : `opendock-panel-${material}`;
        for (const cls of MATERIAL_CLASSES) {
            if (cls === target)
                Main.panel.add_style_class_name(cls);
            else
                Main.panel.remove_style_class_name(cls);
        }
        this._styleApplied = true;
    }

    _removeStyle() {
        if (!this._styleApplied)
            return;
        for (const cls of MATERIAL_CLASSES)
            Main.panel.remove_style_class_name(cls);
        this._styleApplied = false;
    }

    _findClockLabel() {
        const dateMenu = Main.panel.statusArea?.dateMenu;
        if (!dateMenu)
            return null;
        if (dateMenu._clockDisplay instanceof St.Label)
            return dateMenu._clockDisplay;
        // Respaldo por si el nombre interno cambia entre versiones de GNOME.
        const stack = [dateMenu];
        while (stack.length) {
            const actor = stack.pop();
            if (actor instanceof St.Label)
                return actor;
            const children = typeof actor.get_children === 'function' ? actor.get_children() : [];
            stack.push(...children);
        }
        return null;
    }

    _startClock() {
        this._clockLabel = this._findClockLabel();
        if (!this._clockLabel)
            return;
        const update = () => {
            if (this._clockLabel)
                this._clockLabel.set_text(formatClock());
            return GLib.SOURCE_CONTINUE;
        };
        update();
        this._clockSourceId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 1, update);
    }

    _stopClock() {
        if (this._clockSourceId) {
            GLib.source_remove(this._clockSourceId);
            this._clockSourceId = null;
        }
        this._clockLabel = null;
    }
}
