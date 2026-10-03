import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {CornerManager} from './lib/corners.js';
import {PanelStyler, PANEL_HEIGHT} from './lib/panel.js';
import {NotchManager} from './lib/notch.js';
import {DockManager} from './lib/dock.js';

export default class OpenDockExtension extends Extension {
    enable() {
        this._settings = this.getSettings();

        this._corners = new CornerManager(this._settings);
        this._panel = new PanelStyler(this._settings);
        this._notch = new NotchManager(this._settings, this, PANEL_HEIGHT);
        this._dock = new DockManager(this._settings);

        // Cada pieza se habilita por separado y de forma defensiva: un fallo
        // en una no debe dejar a las demás sin poder deshabilitarse.
        for (const piece of [this._corners, this._panel, this._notch, this._dock]) {
            try {
                piece.enable();
            } catch (e) {
                logError(e, 'OpenDock: fallo al activar un componente');
            }
        }
    }

    disable() {
        for (const piece of [this._dock, this._notch, this._panel, this._corners]) {
            try {
                piece?.disable();
            } catch (e) {
                logError(e, 'OpenDock: fallo al desactivar un componente');
            }
        }
        this._dock = null;
        this._notch = null;
        this._panel = null;
        this._corners = null;
        this._settings = null;
    }
}
