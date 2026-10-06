// Dock inferior: favoritos + apps abiertas, lupa gaussiana, rebote de
// lanzamiento, indicadores y ocultación a la mitad / del todo con reserva
// de espacio de trabajo (ver linux/DESIGN.md, sección "Dock").
//
// Un dock en cada monitor (DockView), con las mismas apps y su propia lupa,
// ocultación y pantalla completa; DockManager lleva las señales comunes y
// rehace los docks al cambiar los monitores. Al dejar el cursor sobre una app
// abierta (o al pulsar una con varias ventanas) sale la vista previa de sus
// ventanas, con el muelle de OpenDock.

import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import Pango from 'gi://Pango';
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

// vista previa (los mismos números que la de Windows)
const PREVIEW_DELAY_MS = 400;     // cursor quieto sobre la app
const PREVIEW_LEAVE_MS = 300;     // fuera del icono y de la vista previa
const THUMB_W = 200;
const THUMB_H = 130;
const CARD_PAD = 10;
const CARD_GAP = 10;
const PREVIEW_LIFT = 10;          // separación sobre el dock

export class DockManager {
    constructor(settings, extensionObject) {
        this._settings = settings;
        this._extensionObject = extensionObject;
        this._views = [];
        this._settingsIds = [];
        this._appSystemIds = [];
        this._favoritesId = null;
        this._monitorsChangedId = null;
        this._fullscreenId = null;
        this._focusId = null;
    }

    enable() {
        for (const key of ['material', 'dock-icon-size', 'show-dock', 'dock-hide-mode']) {
            this._settingsIds.push(this._settings.connect(`changed::${key}`,
                () => this._rebuild()));
        }
        const appSystem = Shell.AppSystem.get_default();
        this._appSystemIds.push(appSystem.connect('app-state-changed', () => this._rebuild()));
        this._appSystemIds.push(appSystem.connect('installed-changed', () => this._rebuild()));
        this._favoritesId = AppFavorites.getAppFavorites().connect('changed', () => this._rebuild());
        this._monitorsChangedId = Main.layoutManager.connect('monitors-changed', () => this._syncViews());
        this._fullscreenId = global.display.connect('in-fullscreen-changed', () => this._syncFullscreen());
        this._syncViews();
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
        if (this._fullscreenId) {
            global.display.disconnect(this._fullscreenId);
            this._fullscreenId = null;
        }
        this._destroyViews();
    }

    _destroyViews() {
        for (const view of this._views)
            view.destroy();
        this._views = [];
    }

    // Un dock por monitor: al conectar, quitar o reordenar monitores se rehacen todos.
    _syncViews() {
        this._destroyViews();
        const monitors = Main.layoutManager.monitors;
        for (let i = 0; i < monitors.length; i++)
            this._views.push(new DockView(this._settings, this._extensionObject, i));
        this._rebuild();
        this._syncFullscreen();
    }

    _rebuild() {
        for (const view of this._views)
            view.rebuild();
    }

    _syncFullscreen() {
        for (const view of this._views)
            view.setFullscreen(global.display.get_monitor_in_fullscreen(view.monitorIndex));
    }
}

class DockView {
    constructor(settings, extensionObject, monitorIndex) {
        this._settings = settings;
        this._extensionObject = extensionObject;
        this.monitorIndex = monitorIndex;
        this._monitor = Main.layoutManager.monitors[monitorIndex];
        this._menu = null;
        this._menuIdleId = null;
        this._rebuildPending = false;
        this._icons = [];
        this._hideTimeoutId = null;
        this._hovering = false;
        this._fullscreen = false;
        this._offsetSpring = new Spring(260, 0.95, 0);
        this._runner = new SpringRunner(dt => this._tick(dt));
        this._preview = null;
        this._previewTimeoutId = null;
        this._previewLeaveId = null;
        this._previewSuppress = null;    // icono recién pulsado: sin vista previa hasta salir de él
        this._buildActor();
    }

    destroy() {
        for (const id of ['_hideTimeoutId', '_menuIdleId', '_previewTimeoutId', '_previewLeaveId']) {
            if (this[id]) {
                GLib.source_remove(this[id]);
                this[id] = null;
            }
        }
        this._runner.stop();
        this._closePreview(true);
        this._destroyMenu(true);
        this._menuManager = null;
        if (this._actor) {
            Main.layoutManager.removeChrome(this._actor);
            this._actor.destroy();
            this._actor = null;
        }
        this._icons = [];
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
        // La pantalla completa la lleva setFullscreen(), monitor a monitor.
        Main.layoutManager.addChrome(this._actor, {affectsStruts: true});
        this._menuManager = new PopupMenu.PopupMenuManager(this._actor);
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

    _updateVisible() {
        if (this._actor)
            this._actor.visible = this._settings.get_boolean('show-dock') && !this._fullscreen;
    }

    // Una ventana a pantalla completa en este monitor: su dock se aparta (el de los demás, no).
    setFullscreen(fullscreen) {
        if (fullscreen === this._fullscreen)
            return;
        this._fullscreen = fullscreen;
        if (fullscreen) {
            this._closePreview();
            this._menu?.close(BoxPointer.PopupAnimation.NONE);
        }
        this._updateVisible();
    }

    rebuild() {
        // con el menú abierto, su icono no puede desaparecer: se rehace al cerrarlo
        if (this._menu) {
            this._rebuildPending = true;
            return;
        }
        this._updateVisible();
        if (!this._settings.get_boolean('show-dock'))
            return;

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
        if (this._preview)
            this._preview.refresh();
    }

    _buildIcon(app, baseSize) {
        const running = app.get_n_windows() > 0;
        const focused = running && global.display.focus_window &&
            app.get_windows().includes(global.display.focus_window);

        const wrapper = new St.BoxLayout({vertical: true, reactive: true});
        const texture = app.create_icon_texture(baseSize);
        const iconButton = new St.Button({child: texture, track_hover: true});
        iconButton.connect('clicked', () => this._activate(app, iconButton));
        iconButton.connect('button-press-event', (_actor, event) => {
            if (event.get_button() !== Clutter.BUTTON_SECONDARY)
                return Clutter.EVENT_PROPAGATE;
            this._closePreview();
            this._openMenu(app, iconButton);
            return Clutter.EVENT_STOP;
        });
        iconButton.connect('notify::hover', () => this._onIconHover(app, iconButton));
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
        // varias ventanas: se elige en la vista previa (como en la barra de tareas)
        if (app.get_n_windows() >= 2) {
            if (this._preview && this._preview.app === app)
                this._closePreview();
            else
                this._openPreview(app, iconButton);
            return;
        }
        this._closePreview();
        this._previewSuppress = iconButton;
        const wasRunning = app.get_n_windows() > 0;
        if (!wasRunning)
            this._bounce(iconButton);
        try {
            app.activate();
        } catch (e) {
            logError(e, `OpenDock: no se pudo abrir ${app.get_id()}`);
        }
    }

    // ---- vista previa de las ventanas --------------------------------------

    _onIconHover(app, iconButton) {
        if (iconButton.hover) {
            this._cancelPreviewLeave();
            if (this._previewSuppress === iconButton || this._menu || app.get_n_windows() === 0)
                return;
            if (this._preview) {          // ya abierta: cambia al momento de app
                if (this._preview.app !== app)
                    this._openPreview(app, iconButton);
                return;
            }
            if (this._previewTimeoutId)
                GLib.source_remove(this._previewTimeoutId);
            this._previewTimeoutId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, PREVIEW_DELAY_MS, () => {
                this._previewTimeoutId = null;
                if (iconButton.hover && !this._menu && app.get_n_windows() > 0)
                    this._openPreview(app, iconButton);
                return GLib.SOURCE_REMOVE;
            });
        } else {
            if (this._previewSuppress === iconButton)
                this._previewSuppress = null;
            if (this._previewTimeoutId) {
                GLib.source_remove(this._previewTimeoutId);
                this._previewTimeoutId = null;
            }
            this._schedulePreviewLeave();
        }
    }

    _openPreview(app, iconButton) {
        if (this._previewTimeoutId) {
            GLib.source_remove(this._previewTimeoutId);
            this._previewTimeoutId = null;
        }
        this._cancelPreviewLeave();
        this._closePreview(true);
        const preview = new WindowPreview(this, app, iconButton);
        if (preview.empty) {
            preview.close(true);
            return;
        }
        this._preview = preview;
        this._show();
    }

    _closePreview(now = false) {
        const preview = this._preview;
        if (!preview)
            return;
        this._preview = null;
        preview.close(now);
        if (!now && !this._hovering)
            this._scheduleHide();
    }

    _cancelPreviewLeave() {
        if (this._previewLeaveId) {
            GLib.source_remove(this._previewLeaveId);
            this._previewLeaveId = null;
        }
    }

    // se cierra un momento después de salir del icono y de la vista previa
    _schedulePreviewLeave() {
        if (!this._preview)
            return;
        this._cancelPreviewLeave();
        this._previewLeaveId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, PREVIEW_LEAVE_MS, () => {
            this._previewLeaveId = null;
            const p = this._preview;
            if (p && !p.hovered && !p.iconButton.hover)
                this._closePreview();
            return GLib.SOURCE_REMOVE;
        });
    }

    previewHoverChanged() {
        if (this._preview?.hovered)
            this._cancelPreviewLeave();
        else
            this._schedulePreviewLeave();
    }

    previewEmptied(preview) {
        if (this._preview === preview)
            this._closePreview();
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
            this.rebuild();
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
            if (!this._hovering && !this._menu && !this._preview)
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
        if (this._actor && this._monitor)
            this._actor.translation_y = this._offsetSpring.value;
        return this._offsetSpring.isSettled();
    }

    get palette() {
        return this._palette();
    }

    get bounce() {
        return this._settings.get_string('bounce');
    }

    get monitor() {
        return this._monitor;
    }

    get dockTop() {
        return this._monitor.y + this._monitor.height - PANEL_HEIGHT - MARGIN_BOTTOM;
    }
}

// Vista previa: una tarjeta por ventana con su imagen en vivo (Clutter.Clone del
// actor de la ventana), icono, título y ✕. Nace del icono con el muelle de OpenDock
// (rebote según el ajuste), la tarjeta señalada crece (1,035) y las demás se
// apartan (0,965), y al cerrarse vuelve al icono.
class WindowPreview {
    constructor(view, app, iconButton) {
        this._view = view;
        this.app = app;
        this.iconButton = iconButton;
        this.hovered = false;
        this.empty = false;
        this._closing = false;
        this._fit = 1;
        this._refreshId = null;
        this._cards = [];
        this._hot = null;
        this._windowIds = [];
        this._iconDestroyId = iconButton.connect('destroy', () => view.previewEmptied(this));

        const zeta = zetaFor(view.bounce);
        this._scale = new Spring(380, zeta, 0.6);
        this._fade = new Spring(420, 1.0, 0);
        this._scale.setTarget(1);
        this._fade.setTarget(1);
        this._runner = new SpringRunner(dt => this._tick(dt), () => this._settled());

        const palette = view.palette;
        this.actor = new St.BoxLayout({
            name: 'opendock-preview',
            reactive: true,
            track_hover: true,
            style: `background-color: ${cssRgba(palette.background, 0.92)}; border-radius: 18px; ` +
                `padding: ${CARD_PAD}px; spacing: ${CARD_GAP}px;`,
            opacity: 0,
        });
        this.actor.connect('notify::hover', () => {
            this.hovered = this.actor.hover;
            view.previewHoverChanged();
        });
        Main.layoutManager.addTopChrome(this.actor);
        this._build();
        this._runner.start();
    }

    _build() {
        const windows = this.app.get_windows().filter(w => !w.skip_taskbar);
        this.actor.destroy_all_children();
        this._cards = [];
        this._hot = null;
        for (const win of windows)
            this._cards.push(this._buildCard(win));
        this._watchWindows(windows);
        this.empty = !this._cards.length;
        if (this.empty) {
            this._view.previewEmptied(this);
            return;
        }
        this._place();
    }

    refresh() {
        if (!this._closing)
            this._build();
    }

    _watchWindows(windows) {
        this._unwatchWindows();
        for (const win of windows) {
            // la ✕ pide cerrar; la tarjeta se va cuando la ventana se cierra de verdad
            const id = win.connect('unmanaged', () => {
                if (this._refreshId)
                    return;
                this._refreshId = GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
                    this._refreshId = null;
                    this.refresh();
                    return GLib.SOURCE_REMOVE;
                });
            });
            this._windowIds.push([win, id]);
        }
    }

    _unwatchWindows() {
        for (const [win, id] of this._windowIds) {
            try {
                win.disconnect(id);
            } catch (e) {
                // la ventana ya no existe
            }
        }
        this._windowIds = [];
    }

    _buildCard(win) {
        const palette = this._view.palette;
        const card = new St.Button({
            reactive: true,
            track_hover: true,
            can_focus: false,
            style: 'border-radius: 12px; padding: 6px;',
        });
        const box = new St.BoxLayout({vertical: true, style: 'spacing: 6px;'});
        card.set_child(box);

        const header = new St.BoxLayout({style: 'spacing: 6px;'});
        header.add_child(this.app.create_icon_texture(16));
        const title = new St.Label({
            text: win.get_title() || this.app.get_name(),
            style: `color: ${palette.text}; font-size: 12px; font-weight: 600;`,
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        });
        title.clutter_text.ellipsize = Pango.EllipsizeMode.END;
        header.add_child(title);
        const close = new St.Button({
            label: '✕',
            reactive: true,
            track_hover: true,
            opacity: 0,
            style: `color: ${palette.text}; font-size: 11px; border-radius: 9px; ` +
                'width: 18px; height: 18px; padding: 0;',
        });
        close.connect('notify::hover', () => close.set_style(
            `color: ${close.hover ? '#FFFFFF' : palette.text}; font-size: 11px; border-radius: 9px; ` +
            `width: 18px; height: 18px; padding: 0; background-color: ${close.hover ? '#FF453A' : 'transparent'};`));
        close.connect('clicked', () => win.delete(global.get_current_time()));
        header.add_child(close);
        box.add_child(header);

        // imagen en vivo; las minimizadas no pintan nada: su icono en grande
        const thumb = new St.Widget({width: THUMB_W, height: THUMB_H});
        const actor = win.get_compositor_private();
        if (actor && !win.minimized) {
            const [w, h] = actor.get_size();
            const s = Math.min(THUMB_W / Math.max(1, w), THUMB_H / Math.max(1, h));
            const clone = new Clutter.Clone({source: actor, width: w * s, height: h * s});
            clone.set_position(Math.round((THUMB_W - w * s) / 2), Math.round((THUMB_H - h * s) / 2));
            thumb.add_child(clone);
        } else {
            const icon = this.app.create_icon_texture(64);
            icon.set_position((THUMB_W - 64) / 2, (THUMB_H - 64) / 2);
            thumb.add_child(icon);
        }
        box.add_child(thumb);

        const data = {card, close, win, scale: new Spring(420, zetaFor(this._view.bounce), 1)};
        card.set_pivot_point(0.5, 0.5);
        card.connect('notify::hover', () => {
            close.opacity = card.hover ? 255 : 0;
            card.set_style(`border-radius: 12px; padding: 6px; background-color: ${card.hover ? 'rgba(255,255,255,0.08)' : 'transparent'};`);
            this._hot = card.hover ? data : (this._hot === data ? null : this._hot);
            this._retarget();
        });
        card.connect('clicked', () => {
            Main.activateWindow(win);
            this._view.previewEmptied(this);
        });
        card.connect('button-release-event', (_a, event) => {
            if (event.get_button() !== Clutter.BUTTON_MIDDLE)
                return Clutter.EVENT_PROPAGATE;
            win.delete(global.get_current_time());
            return Clutter.EVENT_STOP;
        });
        this.actor.add_child(card);
        return data;
    }

    // la señalada crece y las demás se apartan, con el muelle del tema
    _retarget() {
        for (const c of this._cards)
            c.scale.setTarget(this._hot ? (c === this._hot ? 1.035 : 0.965) : 1);
        if (!this._runner.running)
            this._runner.start();
    }

    // encima del icono, dentro de su monitor
    _place() {
        const m = this._view.monitor;
        const [, natW] = this.actor.get_preferred_width(-1);
        const maxW = m.width - 24;
        // muchas ventanas: se encogen para caber en el monitor
        this._fit = natW > maxW ? maxW / natW : 1;
        const w = natW * this._fit;
        const [, natH] = this.actor.get_preferred_height(natW);
        const [ix] = this.iconButton.get_transformed_position();
        const cx = ix + this.iconButton.width / 2;
        const x = Math.max(m.x + 12, Math.min(m.x + m.width - 12 - w, Math.round(cx - w / 2)));
        const y = this._view.dockTop - PREVIEW_LIFT - natH * this._fit;
        this.actor.set_position(x, y);
        // nace del icono: el pivote, en el punto de la tarjeta más cercano a él
        this.actor.set_pivot_point(Math.max(0, Math.min(1, (cx - x) / Math.max(1, w))), 1);
    }

    close(now) {
        if (this._closing)
            return;
        this._closing = true;
        if (now || !this.actor) {
            this._destroy();
            return;
        }
        this.actor.reactive = false;
        this._scale.setTarget(0.6);
        this._fade.setTarget(0);
        this._scale.zeta = 1.0;         // de vuelta al icono, sin rebote
        if (!this._runner.running)
            this._runner.start();
    }

    _tick(dt) {
        this._scale.step(dt);
        this._fade.step(dt);
        let settled = this._scale.isSettled(0.004) && this._fade.isSettled(0.01);
        if (this.actor) {
            const s = Math.max(0, this._scale.value);
            const k = this._fit;
            this.actor.set_scale(s * k, s * k);
            this.actor.opacity = Math.round(255 * Math.max(0, Math.min(1, this._fade.value)));
        }
        for (const c of this._cards) {
            c.scale.step(dt);
            c.card.set_scale(c.scale.value, c.scale.value);
            if (!c.scale.isSettled())
                settled = false;
        }
        if (this._closing && this._fade.value < 0.03)
            return true;
        return settled;
    }

    _settled() {
        if (this._closing)
            this._destroy();
    }

    _destroy() {
        this._runner.stop();
        if (this._refreshId) {
            GLib.source_remove(this._refreshId);
            this._refreshId = null;
        }
        this._unwatchWindows();
        if (this._iconDestroyId) {
            try {
                this.iconButton.disconnect(this._iconDestroyId);
            } catch (e) {
                // el icono ya se destruyó
            }
            this._iconDestroyId = null;
        }
        if (this.actor) {
            Main.layoutManager.removeChrome(this.actor);
            this.actor.destroy();
            this.actor = null;
        }
        this._cards = [];
    }
}
