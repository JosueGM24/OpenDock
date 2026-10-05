// Dock inferior: favoritos + apps abiertas, lupa gaussiana, rebote de
// lanzamiento, indicadores y ocultación a la mitad / del todo con reserva
// de espacio de trabajo (ver linux/DESIGN.md, sección "Dock").

import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';
import St from 'gi://St';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as AppFavorites from 'resource:///org/gnome/shell/ui/appFavorites.js';
import * as BoxPointer from 'resource:///org/gnome/shell/ui/boxpointer.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {Spring, SpringRunner} from './spring.js';
import {paletteFor, zetaFor, cssRgba} from './theme.js';

const PANEL_HEIGHT = 60;
const MARGIN_BOTTOM = 12;
const BASE_ICON = 38;
const GAP = 12;
const PAD_X = 16;
const PAD_Y = 11;
const HIDE_DELAY_MS = 450;

export class DockManager {
    constructor(settings, extensionObject) {
        this._settings = settings;
        this._extensionObject = extensionObject;
        this._menu = null;
        this._menuManager = null;
        this._menuIdleId = null;
        this._rebuildPending = false;
        this._actor = null;
        this._iconBox = null;
        this._icons = [];
        this._settingsIds = [];
        this._appSystemIds = [];
        this._favoritesId = null;
        this._monitorsChangedId = null;
        this._hideTimeoutId = null;
        this._offsetSpring = new Spring(260, 0.95, 0);
        this._runner = new SpringRunner(dt => this._tick(dt));
        this._hovering = false;
    }

    enable() {
        this._buildActor();
        for (const key of ['material', 'dock-icon-size', 'show-dock', 'dock-hide-mode']) {
            this._settingsIds.push(this._settings.connect(`changed::${key}`,
                () => this._rebuild()));
        }
        const appSystem = Shell.AppSystem.get_default();
        this._appSystemIds.push(appSystem.connect('app-state-changed', () => this._rebuild()));
        this._appSystemIds.push(appSystem.connect('installed-changed', () => this._rebuild()));
        this._favoritesId = AppFavorites.getAppFavorites().connect('changed', () => this._rebuild());
        this._monitorsChangedId = Main.layoutManager.connect('monitors-changed', () => this._relayout());
        this._rebuild();
        this._relayout();
    }

    disable() {
        for (const id of this._settingsIds)
            this._settings.disconnect(id);
        this._settingsIds = [];
        const appSystem = Shell.AppSystem.get_default();
        for (const id of this._appSystemIds)
            appSystem.disconnect(id);
        this._appSystemIds = [];
        if (this._favoritesId) {
            AppFavorites.getAppFavorites().disconnect(this._favoritesId);
            this._favoritesId = null;
        }
        if (this._monitorsChangedId) {
            Main.layoutManager.disconnect(this._monitorsChangedId);
            this._monitorsChangedId = null;
        }
        if (this._hideTimeoutId) {
            GLib.source_remove(this._hideTimeoutId);
            this._hideTimeoutId = null;
        }
        this._runner.stop();
        if (this._menuIdleId) {
            GLib.source_remove(this._menuIdleId);
            this._menuIdleId = null;
        }
        this._destroyMenu(true);
        this._menuManager = null;
        if (this._actor) {
            Main.layoutManager.removeChrome(this._actor);
            this._actor.destroy();
            this._actor = null;
        }
    }

    _palette() {
        return paletteFor(this._settings.get_string('material'));
    }

    _buildActor() {
        this._actor = new St.Widget({
            name: 'opendock-dock',
            reactive: true,
            height: PANEL_HEIGHT,
        });
        this._actor.connect('enter-event', () => {
            this._hovering = true;
            if (this._hideTimeoutId) {
                GLib.source_remove(this._hideTimeoutId);
                this._hideTimeoutId = null;
            }
            this._show();
            return Clutter.EVENT_PROPAGATE;
        });
        this._actor.connect('leave-event', () => {
            this._hovering = false;
            this._scheduleHide();
            return Clutter.EVENT_PROPAGATE;
        });
        this._actor.connect('motion-event', (_actor, event) => {
            this._onMotion(event);
            return Clutter.EVENT_PROPAGATE;
        });

        this._iconBox = new St.BoxLayout({
            style: `spacing: ${GAP}px;`,
            style_class: 'opendock-icon-box',
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._actor.add_child(this._iconBox);

        // Reserva la mitad del alto del panel como zona de trabajo, igual
        // que una AppBar: las ventanas maximizadas no quedan tapadas.
        Main.layoutManager.addChrome(this._actor, {
            affectsStruts: true,
            trackFullscreen: true,
        });
        this._menuManager = new PopupMenu.PopupMenuManager(this._actor);
    }

    _relayout() {
        const monitor = Main.layoutManager.primaryMonitor;
        if (!monitor || !this._actor)
            return;
        this._monitor = monitor;
        this._layoutIcons();
    }

    _layoutIcons() {
        if (!this._actor || !this._monitor)
            return;
        const width = this._iconBox.width + PAD_X * 2;
        const height = PANEL_HEIGHT;
        const radius = Math.round(height * 0.30);
        const palette = this._palette();
        this._actor.set_style(
            `background-color: ${cssRgba(palette.background, 0.75)}; border-radius: ${radius}px;`);
        this._actor.set_size(width, height);
        this._iconBox.set_position(PAD_X, PAD_Y);
        this._actor.set_position(
            this._monitor.x + Math.round(this._monitor.width / 2 - width / 2),
            this._monitor.y + this._monitor.height - height - MARGIN_BOTTOM);
    }

    _rebuild() {
        // con el menú abierto, su icono no puede desaparecer: se rehace al cerrarlo
        if (this._menu) {
            this._rebuildPending = true;
            return;
        }
        if (!this._settings.get_boolean('show-dock')) {
            if (this._actor)
                this._actor.visible = false;
            return;
        }
        if (this._actor)
            this._actor.visible = true;

        this._iconBox.destroy_all_children();
        this._icons = [];

        const baseSize = this._settings.get_int('dock-icon-size') || BASE_ICON;
        const favorites = AppFavorites.getAppFavorites().getFavorites();
        const running = Shell.AppSystem.get_default().get_running();

        const seen = new Set();
        const apps = [];
        for (const app of favorites) {
            apps.push({app, pinned: true});
            seen.add(app.get_id());
        }
        for (const app of running) {
            if (!seen.has(app.get_id())) {
                apps.push({app, pinned: false});
                seen.add(app.get_id());
            }
        }

        for (const entry of apps)
            this._icons.push(this._buildIcon(entry.app, baseSize));

        this._layoutIcons();
    }

    _buildIcon(app, baseSize) {
        const running = app.get_n_windows() > 0;
        const focused = running && global.display.focus_window &&
            app.get_windows().includes(global.display.focus_window);

        const wrapper = new St.BoxLayout({vertical: true, reactive: true});
        const texture = app.create_icon_texture(baseSize);
        const iconButton = new St.Button({child: texture});
        iconButton.connect('clicked', () => this._activate(app, iconButton));
        iconButton.connect('button-press-event', (_actor, event) => {
            if (event.get_button() !== Clutter.BUTTON_SECONDARY)
                return Clutter.EVENT_PROPAGATE;
            this._openMenu(app, iconButton);
            return Clutter.EVENT_STOP;
        });
        wrapper.add_child(iconButton);

        const indicator = new St.Widget({
            style: `background-color: ${this._palette().text}; border-radius: 2px;`,
            height: 3,
            opacity: running ? (focused ? 242 : 140) : 0,
            width: focused ? 16 : 6,
            x_align: Clutter.ActorAlign.CENTER,
        });
        wrapper.add_child(indicator);

        wrapper._opendock = {app, baseSize, iconButton, indicator};
        this._iconBox.add_child(wrapper);
        return wrapper;
    }

    _activate(app, iconButton) {
        const wasRunning = app.get_n_windows() > 0;
        if (!wasRunning)
            this._bounce(iconButton);
        try {
            app.activate();
        } catch (e) {
            logError(e, `OpenDock: no se pudo abrir ${app.get_id()}`);
        }
    }

    // ---- menú del clic derecho -------------------------------------------
    // Como el de Windows: ventanas abiertas, acciones de la app (.desktop),
    // nueva ventana, anclar, ajustes del dock, cerrar y finalizar tarea.

    _openMenu(app, source) {
        this._destroyMenu();
        const menu = new PopupMenu.PopupMenu(source, 0.5, St.Side.BOTTOM);
        const palette = this._palette();
        menu.box.set_style(
            `background-color: ${cssRgba(palette.background, 0.92)}; color: ${palette.text}; ` +
            'border-radius: 16px; padding: 6px;');
        Main.uiGroup.add_child(menu.actor);
        menu.actor.hide();
        this._menuManager.addMenu(menu);
        this._menu = menu;

        const time = () => global.get_current_time();
        const windows = app.get_windows();
        const appInfo = app.get_app_info();
        const favorites = AppFavorites.getAppFavorites();
        const id = app.get_id();

        const title = new PopupMenu.PopupMenuItem(app.get_name(), {reactive: false});
        title.label.set_style('font-weight: 700;');
        menu.addMenuItem(title);

        if (windows.length > 0) {
            menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
            for (const win of windows) {
                const item = menu.addAction(win.get_title() || app.get_name(),
                    () => Main.activateWindow(win));
                if (win === global.display.focus_window)
                    item.setOrnament(PopupMenu.Ornament.DOT);
            }
        }

        const actions = appInfo ? appInfo.list_actions() : [];
        if (actions.length > 0) {
            menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
            for (const action of actions) {
                menu.addAction(appInfo.get_action_name(action),
                    () => app.launch_action(action, time(), -1));
            }
        }

        menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        if (windows.length === 0)
            menu.addAction('Abrir', () => this._activate(app, source));
        else if (app.can_open_new_window())
            menu.addAction('Nueva ventana', () => app.open_new_window(-1));
        if (favorites.isFavorite(id))
            menu.addAction('Quitar del dock', () => favorites.removeFavorite(id));
        else if (appInfo)
            menu.addAction('Anclar al dock', () => favorites.addFavorite(id));
        menu.addAction('Ajustes del dock', () => {
            try {
                this._extensionObject?.openPreferences();
            } catch (e) {
                logError(e, 'OpenDock: no se pudieron abrir los ajustes');
            }
        });

        if (windows.length > 0) {
            menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
            menu.addAction(windows.length > 1 ? 'Cerrar todas las ventanas' : 'Cerrar ventana',
                () => windows.forEach(w => w.delete(time())));
            // Finalizar tarea: Mutter mata el cliente de cada ventana (SIGKILL al
            // proceso), sin lanzar `kill` ni ningún otro programa.
            menu.addAction('Finalizar tarea', () => windows.forEach(w => w.kill()));
        }

        menu.connect('open-state-changed', (_menu, open) => {
            if (open)
                return;
            // se destruye cuando termina de cerrarse, fuera de esta señal
            if (this._menuIdleId)
                GLib.source_remove(this._menuIdleId);
            this._menuIdleId = GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
                this._menuIdleId = null;
                if (this._menu === menu)
                    this._destroyMenu();
                return GLib.SOURCE_REMOVE;
            });
        });
        source.connect('destroy', () => {
            if (this._menu === menu)
                this._destroyMenu();
        });
        menu.open(BoxPointer.PopupAnimation.FULL);
    }

    _destroyMenu(disabling = false) {
        const menu = this._menu;
        if (!menu)
            return;
        this._menu = null;
        menu.destroy();
        if (disabling) {
            this._rebuildPending = false;
            return;
        }
        if (this._rebuildPending && this._actor) {
            this._rebuildPending = false;
            this._rebuild();
        }
        if (!this._hovering)
            this._scheduleHide();
    }

    _bounce(actor) {
        const baseSize = actor.height || BASE_ICON;
        let amplitude = 0.75 * baseSize;
        const hop = remaining => {
            if (remaining <= 0)
                return;
            actor.ease({
                translation_y: -amplitude,
                duration: 190,
                mode: Clutter.AnimationMode.EASE_OUT_QUAD,
                onComplete: () => {
                    actor.ease({
                        translation_y: 0,
                        duration: 190,
                        mode: Clutter.AnimationMode.EASE_IN_QUAD,
                        onComplete: () => {
                            amplitude *= 0.62;
                            hop(remaining - 1);
                        },
                    });
                },
            });
        };
        hop(3);
    }

    _onMotion(event) {
        if (!this._icons.length)
            return;
        const [stageX] = event.get_coords();
        const [actorX] = this._actor.get_transformed_position();
        const localX = stageX - actorX - PAD_X;
        const step = (this._icons[0]?.width || BASE_ICON) + GAP;
        if (!step || step <= 0)
            return;
        const f = localX / step;
        this._icons.forEach((wrapper, i) => {
            const d = (i - f) / 1.55;
            const scale = 1 + 0.5 * Math.exp(-(d * d));
            wrapper.ease({
                scale_x: scale,
                scale_y: scale,
                duration: 90,
                mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            });
        });
    }

    _show() {
        const zeta = zetaFor(this._settings.get_string('bounce'));
        this._offsetSpring.k = 260;
        this._offsetSpring.zeta = zeta;
        this._offsetSpring.setTarget(0);
        if (!this._runner.running)
            this._runner.start();
    }

    _scheduleHide() {
        const mode = this._settings.get_string('dock-hide-mode');
        if (mode === 'never')
            return;
        if (this._hideTimeoutId)
            GLib.source_remove(this._hideTimeoutId);
        this._hideTimeoutId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, HIDE_DELAY_MS, () => {
            this._hideTimeoutId = null;
            if (!this._hovering && !this._menu)
                this._hide(mode);
            return GLib.SOURCE_REMOVE;
        });
    }

    _hide(mode) {
        const offset = mode === 'full' ? PANEL_HEIGHT + MARGIN_BOTTOM : PANEL_HEIGHT / 2;
        this._offsetSpring.k = 140;
        this._offsetSpring.zeta = 0.95;
        this._offsetSpring.setTarget(offset);
        if (!this._runner.running)
            this._runner.start();
    }

    _tick(dt) {
        this._offsetSpring.step(dt);
        if (this._actor && this._monitor) {
            this._actor.translation_y = this._offsetSpring.value;
        }
        return this._offsetSpring.isSettled();
    }
}
