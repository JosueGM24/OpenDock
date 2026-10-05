// Notch bajo la barra superior: reemplaza los banners de GNOME, muestra el
// aviso con muelles (ver linux/DESIGN.md) y abre un centro de notificaciones
// construido con las notificaciones ya existentes en Main.messageTray.

import St from 'gi://St';
import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import Pango from 'gi://Pango';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as Config from 'resource:///org/gnome/shell/misc/config.js';
import {Spring, SpringRunner} from './spring.js';
import {paletteFor, zetaFor} from './theme.js';
import {playSoundFile} from './sound.js';

const PEEK_WIDTH = 360;
const PEEK_HEIGHT = 54;
const PEEK_BODY_X = 52;
const PEEK_BODY_Y = 28;
const PEEK_BODY_LINE = 18;     // alto de línea del cuerpo (13 px)
const PEEK_LINES = 7;          // al expandirlo cabe el cuerpo entero
const PEEK_HOVER_MS = 260;     // cursor quieto encima antes de expandir
const PEEK_LEAVE_MS = 1200;    // al salir, se oculta tras este margen
const CENTER_WIDTH = 384;
const CARD_WIDTH = 348;
const CARD_HEIGHT = 70;
const CARD_GAP = 12;
const MAX_VISIBLE_CARDS = 6;
const AUTO_HIDE_MS = 4500;
const MINI_WIDTH = 110;
const MINI_HEIGHT = 9;
const QUICK_WIDTH = 272;
const QUICK_HEIGHT = 44;
const HOVER_STILL_MS = 450;
const MINI_SNAP_PX = 64;
const HOT_STRIP_WIDTH = 480;
const CARD_HOVER_SCALE = 1.035;
const CARD_OTHER_SCALE = 0.965;
const CARD_EXPAND_MS = 260;
const CARD_ACTION_HEIGHT = 26;
const TRASH_WIDTH = 40;
const TRASH_WIDTH_HOVER = 58;
const TRASH_COLOR = '#E5443C';
const TRASH_COLOR_HOVER = '#FF453A';
const DELETE_FADE_MS = 260;
const SCROLL_STEP_PX = 60;

function formatTime(date) {
    const hh = String(date.getHours()).padStart(2, '0');
    const mm = String(date.getMinutes()).padStart(2, '0');
    return `${hh}:${mm}`;
}

export class NotchManager {
    constructor(settings, extensionObject, panelHeight) {
        this._settings = settings;
        this._extensionObject = extensionObject;
        this._panelHeight = panelHeight;

        this._actor = null;
        this._iconBin = null;
        this._titleLabel = null;
        this._bodyLabel = null;
        this._timeLabel = null;
        this._contentBox = null;

        this._widthSpring = new Spring(380, 1, 0);
        this._heightSpring = new Spring(420, 1, 0);
        this._runner = new SpringRunner(dt => this._tick(dt));

        this._hideTimeoutId = null;
        this._peekHoverId = null;
        this._peekHovered = false;
        this._peekExpanded = false;
        this._peekExtra = 0;
        this._peekBody = '';
        this._sourceIds = new Map();
        this._trayIds = [];
        this._patched = false;
        this._originalShowNotification = undefined;

        this._center = null;
        this._centerOpen = false;

        this._hotStrip = null;
        this._mini = null;
        this._miniHoverId = null;
        this._miniActive = false;
        this._miniMode = 'hidden'; // 'hidden' | 'pill' | 'quick'
        this._miniXSpring = new Spring(240, 0.62, 0);
        this._miniRunner = new SpringRunner(dt => this._miniTick(dt));

        this._cardSprings = new Map(); // card actor -> Spring de escala
        this._cardRunner = new SpringRunner(dt => this._cardTick(dt));
        this._cardExpandTimeouts = new Map(); // card actor -> GLib source id
        this._hoveredCard = null;

        this._scrollSpring = new Spring(260, 0.62, 0);
        this._scrollRunner = new SpringRunner(dt => this._scrollTick(dt));
        this._scrollAdjustment = null;

        this._monitorsChangedId = null;
    }

    enable() {
        this._buildPeek();
        this._buildHotStrip();
        this._patchMessageTray();
        this._connectExistingSources();
        this._trayIds.push(Main.messageTray.connect('source-added',
            (_tray, source) => this._connectSource(source)));
        this._trayIds.push(Main.messageTray.connect('source-removed',
            (_tray, source) => this._disconnectSource(source)));
        this._monitorsChangedId = Main.layoutManager.connect('monitors-changed',
            () => this._relayout());
        this._relayout();
    }

    disable() {
        this._unpatchMessageTray();
        for (const id of this._trayIds)
            Main.messageTray.disconnect(id);
        this._trayIds = [];
        for (const source of [...this._sourceIds.keys()])
            this._disconnectSource(source);

        if (this._monitorsChangedId) {
            Main.layoutManager.disconnect(this._monitorsChangedId);
            this._monitorsChangedId = null;
        }
        if (this._hideTimeoutId) {
            GLib.source_remove(this._hideTimeoutId);
            this._hideTimeoutId = null;
        }
        if (this._miniHoverId) {
            GLib.source_remove(this._miniHoverId);
            this._miniHoverId = null;
        }
        if (this._peekHoverId) {
            GLib.source_remove(this._peekHoverId);
            this._peekHoverId = null;
        }
        this._runner.stop();
        this._miniRunner.stop();
        this._cardRunner.stop();
        this._scrollRunner.stop();
        for (const id of this._cardExpandTimeouts.values())
            GLib.source_remove(id);
        this._cardExpandTimeouts.clear();
        this._cardSprings.clear();
        this._closeCenter();

        for (const actor of [this._actor, this._hotStrip, this._mini]) {
            if (actor) {
                Main.layoutManager.removeChrome(actor);
                actor.destroy();
            }
        }
        this._actor = null;
        this._hotStrip = null;
        this._mini = null;
    }

    // ---- construcción de actores ----------------------------------------

    _palette() {
        return paletteFor(this._settings.get_string('material'));
    }

    _buildPeek() {
        const palette = this._palette();
        this._actor = new St.Widget({
            name: 'opendock-notch',
            reactive: true,
            visible: true,
            clip_to_allocation: true,
        });
        this._actor.set_style(`background-color: ${palette.background};`);
        this._actor.connect('button-press-event', () => {
            this._toggleCenter();
            return Clutter.EVENT_STOP;
        });
        this._actor.connect('enter-event', () => {
            this._onPeekEnter();
            return Clutter.EVENT_PROPAGATE;
        });
        this._actor.connect('leave-event', (_actor, event) => {
            if (this._actor.contains(event.get_related()))
                return Clutter.EVENT_PROPAGATE;
            this._onPeekLeave();
            return Clutter.EVENT_PROPAGATE;
        });

        this._contentBox = new St.BoxLayout({
            visible: false,
            x_expand: true,
            y_expand: true,
        });
        this._actor.add_child(this._contentBox);

        this._iconBin = new St.Bin({
            width: 30,
            height: 30,
            x: 12,
            y: (PEEK_HEIGHT - 30) / 2,
            style: `background-color: ${palette.card}; border-radius: 8px;`,
        });
        this._icon = new St.Icon({
            icon_name: 'dialog-information-symbolic',
            icon_size: 18,
        });
        this._iconBin.set_child(this._icon);
        this._actor.add_child(this._iconBin);

        this._titleLabel = new St.Label({
            x: 52, y: 8,
            style: `color: ${palette.text}; font-weight: 700; font-size: 14px;`,
        });
        this._actor.add_child(this._titleLabel);

        // Cerrado, una línea con puntos suspensivos; con el cursor encima se
        // parte en líneas y el recorte del alto lo va descubriendo al crecer.
        this._bodyLabel = new St.Label({
            x: PEEK_BODY_X, y: PEEK_BODY_Y,
            width: PEEK_WIDTH - PEEK_BODY_X - 16,
            height: PEEK_BODY_LINE,
            clip_to_allocation: true,
            style: `color: ${palette.secondary}; font-size: 13px; font-weight: 600;`,
        });
        this._bodyLabel.clutter_text.set_ellipsize(Pango.EllipsizeMode.END);
        this._actor.add_child(this._bodyLabel);

        this._timeLabel = new St.Label({
            y: 8,
            style: `color: ${palette.tertiary}; font-size: 12px;`,
        });
        this._actor.add_child(this._timeLabel);

        Main.layoutManager.addChrome(this._actor, {
            affectsStruts: false,
            trackFullscreen: false,
        });
        this._actor.set_size(0, 0);
    }

    _buildHotStrip() {
        // Franja central invisible bajo la barra: detecta el cursor para el
        // mini notch (110x9), que lo sigue (muelle k 240) y se imanta al
        // centro a menos de 64 px; quieto 450 ms, se abre la vista rápida.
        this._hotStrip = new St.Widget({
            name: 'opendock-notch-hotstrip',
            reactive: true,
            width: HOT_STRIP_WIDTH,
            height: 10,
            opacity: 0,
        });
        this._hotStrip.connect('enter-event', () => {
            this._onMiniEnter();
            return Clutter.EVENT_PROPAGATE;
        });
        this._hotStrip.connect('motion-event', (_actor, event) => {
            this._onMiniMotion(event);
            return Clutter.EVENT_PROPAGATE;
        });
        this._hotStrip.connect('leave-event', () => {
            this._onMiniLeave();
            return Clutter.EVENT_PROPAGATE;
        });
        Main.layoutManager.addChrome(this._hotStrip, {
            affectsStruts: false,
        });

        this._mini = new St.Widget({name: 'opendock-notch-mini', visible: false, reactive: false});
        Main.layoutManager.addChrome(this._mini, {
            affectsStruts: false,
        });
    }

    _relayout() {
        const monitor = Main.layoutManager.primaryMonitor;
        if (!monitor)
            return;
        this._monitor = monitor;
        if (this._hotStrip) {
            this._hotStrip.set_position(
                monitor.x + Math.round(monitor.width / 2 - this._hotStrip.width / 2),
                monitor.y + this._panelHeight);
        }
        this._positionPeek();
        this._positionMini();
        this._positionCenter();
    }

    _positionPeek() {
        if (!this._actor || !this._monitor)
            return;
        const w = this._actor.width;
        this._actor.set_position(
            this._monitor.x + Math.round(this._monitor.width / 2 - w / 2),
            this._monitor.y + this._panelHeight);
    }

    // ---- MessageTray ------------------------------------------------------

    _patchMessageTray() {
        const tray = Main.messageTray;
        if (!tray || typeof tray._showNotification !== 'function') {
            this._patched = false;
            return;
        }
        this._originalShowNotification = tray._showNotification;
        tray._showNotification = () => {};
        this._patched = true;
    }

    _unpatchMessageTray() {
        if (!this._patched)
            return;
        const tray = Main.messageTray;
        if (tray) {
            if (this._originalShowNotification)
                tray._showNotification = this._originalShowNotification;
            else
                delete tray._showNotification;
        }
        this._patched = false;
        this._originalShowNotification = undefined;
    }

    _connectExistingSources() {
        const tray = Main.messageTray;
        const sources = tray && tray.getSources ? tray.getSources() : [];
        for (const source of sources)
            this._connectSource(source);
    }

    _connectSource(source) {
        if (!source || this._sourceIds.has(source))
            return;
        const ids = [];
        try {
            ids.push(source.connect('notification-added',
                (_src, notification) => this._onNotification(notification)));
        } catch (e) { /* fuentes sin esta señal en alguna versión */ }
        try {
            ids.push(source.connect('destroy', () => this._disconnectSource(source)));
        } catch (e) { /* no crítico */ }
        this._sourceIds.set(source, ids);
    }

    _disconnectSource(source) {
        const ids = this._sourceIds.get(source);
        if (!ids)
            return;
        for (const id of ids) {
            try {
                source.disconnect(id);
            } catch (e) { /* ya destruida */ }
        }
        this._sourceIds.delete(source);
        if (this._centerOpen)
            this._refreshCenter();
    }

    _onNotification(notification) {
        if (!notification)
            return;
        const palette = this._palette();
        this._titleLabel.set_text(notification.title || '');
        this._peekBody = notification.body || '';
        this._peekExpanded = false;
        this._peekExtra = 0;
        this._setPeekBodyWrap(false);
        this._timeLabel.set_text(formatTime(new Date()));
        this._timeLabel.set_style(`color: ${palette.tertiary}; font-size: 12px;`);
        this._timeLabel.set_position(PEEK_WIDTH - 16 - this._timeLabel.width, 8);

        const gicon = notification.gicon || notification.iconName;
        if (gicon && this._icon)
            this._icon.set_gicon(typeof gicon === 'string' ? null : gicon);

        this._currentNotification = notification;
        this._openPeek();

        if (this._settings.get_boolean('sound-enabled'))
            this._playSound();

        if (this._centerOpen)
            this._refreshCenter();
    }

    _playSound() {
        try {
            const name = this._settings.get_string('sound-name');
            const volume = this._settings.get_double('sound-volume');
            const path = GLib.build_filenamev([this._extensionObject.path, 'sounds', `${name}.wav`]);
            if (GLib.file_test(path, GLib.FileTest.EXISTS))
                playSoundFile(path, volume);
        } catch (e) {
            logError(e, 'OpenDock: fallo al preparar el sonido');
        }
    }

    // ---- muelles del aviso -------------------------------------------------

    _openPeek() {
        const zetaW = zetaFor(this._settings.get_string('bounce'));
        const zetaH = Math.min(1, zetaW + 0.1);
        this._widthSpring.k = 380;
        this._widthSpring.zeta = zetaW;
        this._heightSpring.k = 420;
        this._heightSpring.zeta = zetaH;
        this._widthSpring.setTarget(PEEK_WIDTH);
        this._heightSpring.setTarget(PEEK_HEIGHT + this._peekExtra);
        this._peekOpen = true;
        if (!this._runner.running)
            this._runner.start();

        // con el cursor encima no se oculta; el aviso nuevo vuelve a expandirse
        if (this._peekHovered)
            this._onPeekEnter();
        else
            this._scheduleHide(AUTO_HIDE_MS);
    }

    _scheduleHide(ms) {
        if (this._hideTimeoutId)
            GLib.source_remove(this._hideTimeoutId);
        this._hideTimeoutId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
            this._hideTimeoutId = null;
            if (!this._centerOpen && !this._peekHovered)
                this._closePeek();
            return GLib.SOURCE_REMOVE;
        });
    }

    // ---- aviso expandido al pasar el cursor --------------------------------
    // Como en Windows: quieto encima 260 ms, la isla crece con su muelle hasta
    // enseñar el cuerpo completo (hasta 7 líneas) y no se oculta; al salir se
    // cierra de nuevo y desaparece 1,2 s después.

    _onPeekEnter() {
        this._peekHovered = true;
        if (this._hideTimeoutId) {
            GLib.source_remove(this._hideTimeoutId);
            this._hideTimeoutId = null;
        }
        if (this._peekHoverId)
            GLib.source_remove(this._peekHoverId);
        this._peekHoverId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, PEEK_HOVER_MS, () => {
            this._peekHoverId = null;
            if (this._peekHovered && this._peekOpen)
                this._setPeekExpanded(true);
            return GLib.SOURCE_REMOVE;
        });
    }

    _onPeekLeave() {
        this._peekHovered = false;
        if (this._peekHoverId) {
            GLib.source_remove(this._peekHoverId);
            this._peekHoverId = null;
        }
        this._setPeekExpanded(false);
        if (this._peekOpen)
            this._scheduleHide(PEEK_LEAVE_MS);
    }

    _setPeekBodyWrap(wrap) {
        if (!this._bodyLabel)
            return;
        this._bodyLabel.clutter_text.set_line_wrap(wrap);
        this._bodyLabel.set_text(wrap ? this._peekBody : this._peekBody.replace(/\s*\n\s*/g, ' '));
    }

    // Alto extra que necesita el cuerpo partido en líneas (0 si cabe en una).
    _peekNeed() {
        if (!this._peekBody)
            return 0;
        this._setPeekBodyWrap(true);
        const [, natural] = this._bodyLabel.clutter_text.get_preferred_height(this._bodyLabel.width);
        return Math.max(0, Math.min(Math.ceil(natural), PEEK_BODY_LINE * PEEK_LINES) - PEEK_BODY_LINE);
    }

    _setPeekExpanded(expanded) {
        if (this._peekExpanded === expanded)
            return;
        this._peekExpanded = expanded;
        this._peekExtra = expanded ? this._peekNeed() : 0;
        if (expanded && this._peekExtra === 0)
            this._setPeekBodyWrap(false);
        if (!this._peekOpen)
            return;
        const zeta = zetaFor(this._settings.get_string('bounce'));
        this._heightSpring.k = 340;
        this._heightSpring.zeta = Math.max(0.7, zeta);
        this._heightSpring.setTarget(PEEK_HEIGHT + this._peekExtra);
        if (!this._runner.running)
            this._runner.start();
    }

    _closePeek() {
        this._peekOpen = false;
        this._peekExpanded = false;
        this._peekExtra = 0;
        this._widthSpring.k = 300;
        this._widthSpring.zeta = 1;
        this._heightSpring.k = 300;
        this._heightSpring.zeta = 1;
        this._widthSpring.setTarget(0);
        this._heightSpring.setTarget(0);
        if (!this._runner.running)
            this._runner.start();
    }

    _tick(dt) {
        this._widthSpring.step(dt);
        this._heightSpring.step(dt);
        const w = Math.max(0, this._widthSpring.value);
        const h = Math.max(0, this._heightSpring.value);
        this._actor.set_size(w, h);
        this._positionPeek();
        // el cuerpo ocupa lo que la isla ha crecido; cerrado, vuelve a una línea
        const bottomPad = PEEK_HEIGHT - PEEK_BODY_Y - PEEK_BODY_LINE;
        this._bodyLabel.set_height(Math.max(PEEK_BODY_LINE, Math.round(h) - PEEK_BODY_Y - bottomPad));
        if (!this._peekExpanded && h <= PEEK_HEIGHT + 1 && this._bodyLabel.clutter_text.get_line_wrap())
            this._setPeekBodyWrap(false);

        const shoulder = Math.min(7, h / 2);
        this._actor.set_style(
            `background-color: ${this._palette().background}; ` +
            `border-radius: ${shoulder}px ${shoulder}px 16px 16px;`);

        this._contentBox.visible = h > 0.55 * PEEK_HEIGHT;

        const settled = this._widthSpring.isSettled() && this._heightSpring.isSettled();
        return settled;
    }

    // ---- mini notch / vista rápida ----------------------------------------
    // Pastilla 110x9 que sigue al cursor con un muelle (k 240) y se imanta
    // al centro a menos de 64 px; quieta 450 ms, crece a la vista rápida
    // 272x44 ("Notificaciones" + contador); moverse de nuevo la colapsa nota.

    _onMiniEnter() {
        this._miniActive = true;
        if (this._miniMode === 'hidden') {
            this._miniMode = 'pill';
            this._renderMini();
        }
        this._resetMiniStillTimer();
        if (!this._miniRunner.running)
            this._miniRunner.start();
    }

    _onMiniMotion(event) {
        if (!this._monitor)
            return;
        const [stageX] = event.get_coords();
        const centerX = this._monitor.x + this._monitor.width / 2;
        const dx = stageX - centerX;
        const maxRange = HOT_STRIP_WIDTH / 2 - MINI_WIDTH / 2;
        const target = Math.abs(dx) < MINI_SNAP_PX ? 0 : Math.max(-maxRange, Math.min(maxRange, dx));
        this._miniXSpring.setTarget(target);
        if (this._miniMode === 'quick') {
            this._miniMode = 'pill';
            this._renderMini();
        }
        this._resetMiniStillTimer();
        if (!this._miniRunner.running)
            this._miniRunner.start();
    }

    _resetMiniStillTimer() {
        if (this._miniHoverId)
            GLib.source_remove(this._miniHoverId);
        this._miniHoverId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, HOVER_STILL_MS, () => {
            this._miniHoverId = null;
            if (this._miniActive && this._miniMode === 'pill') {
                this._miniMode = 'quick';
                this._renderMini();
            }
            return GLib.SOURCE_REMOVE;
        });
    }

    _onMiniLeave() {
        this._miniActive = false;
        if (this._miniHoverId) {
            GLib.source_remove(this._miniHoverId);
            this._miniHoverId = null;
        }
        this._miniMode = 'hidden';
        this._miniRunner.stop();
        this._hideMini();
    }

    _renderMini() {
        if (!this._mini)
            return;
        const palette = this._palette();
        this._mini.destroy_all_children();
        if (this._miniMode === 'pill') {
            this._mini.set_style(`background-color: ${palette.card}; border-radius: 4.5px;`);
            this._mini.set_size(MINI_WIDTH, MINI_HEIGHT);
        } else if (this._miniMode === 'quick') {
            const count = this._notificationCount();
            this._mini.set_style(`background-color: ${palette.background}; border-radius: 16px;`);
            this._mini.set_size(QUICK_WIDTH, QUICK_HEIGHT);
            const label = new St.Label({
                text: `Notificaciones  ${count}`,
                style: `color: ${palette.text}; font-size: 13px; font-weight: 600;`,
                x_align: Clutter.ActorAlign.CENTER,
                y_align: Clutter.ActorAlign.CENTER,
                x_expand: true,
                y_expand: true,
            });
            this._mini.add_child(label);
        }
        this._mini.visible = this._miniMode !== 'hidden';
        this._positionMini();
    }

    _positionMini() {
        if (!this._mini || !this._monitor || this._miniMode === 'hidden')
            return;
        const width = this._miniMode === 'quick' ? QUICK_WIDTH : MINI_WIDTH;
        const centerX = this._monitor.x + Math.round(this._monitor.width / 2);
        const offset = this._miniMode === 'quick' ? 0 : Math.round(this._miniXSpring.value);
        this._mini.set_position(
            centerX + offset - Math.round(width / 2),
            this._monitor.y + this._panelHeight);
    }

    _miniTick(dt) {
        this._miniXSpring.step(dt);
        this._positionMini();
        return false;
    }

    _hideMini() {
        if (this._mini) {
            this._mini.visible = false;
            this._mini.destroy_all_children();
        }
    }

    _notificationCount() {
        let count = 0;
        for (const source of this._sourceIds.keys())
            count += (source.notifications || []).length;
        return count;
    }

    // ---- centro de notificaciones ------------------------------------------

    _toggleCenter() {
        if (this._centerOpen)
            this._closeCenter();
        else
            this._openCenter();
    }

    _openCenter() {
        if (this._center)
            return;
        const palette = this._palette();
        this._center = new St.BoxLayout({
            name: 'opendock-notch-center',
            vertical: true,
            reactive: true,
            style: `background-color: ${palette.background}; border-radius: 16px 16px 26px 26px;`,
            width: CENTER_WIDTH,
        });

        const header = new St.BoxLayout({height: 56, x_expand: true});
        const headerLabel = new St.Label({
            text: 'Notificaciones',
            y_align: Clutter.ActorAlign.CENTER,
            x_expand: true,
            style: `color: ${palette.text}; font-size: 17px; font-weight: 600; padding-left: 14px;`,
        });
        header.add_child(headerLabel);
        const clearButton = new St.Button({
            label: 'Borrar',
            style: `color: ${palette.text}; padding: 6px 10px;`,
            y_align: Clutter.ActorAlign.CENTER,
        });
        clearButton.connect('clicked', () => this._clearAll());
        header.add_child(clearButton);
        this._center.add_child(header);

        this._cardList = new St.BoxLayout({vertical: true, style: `spacing: ${CARD_GAP}px;`});
        const scroll = new St.ScrollView({
            height: MAX_VISIBLE_CARDS * (CARD_HEIGHT + CARD_GAP),
            width: CENTER_WIDTH,
        });
        // En GNOME 45 St.ScrollView sólo coloca al hijo que entra por
        // add_actor (set_child es el de St.Bin y lo deja sin colocar: el
        // centro se veía vacío). Desde 46 tiene su propio set_child.
        if (parseInt(Config.PACKAGE_VERSION, 10) < 46)
            scroll.add_actor(this._cardList);
        else
            scroll.set_child(this._cardList);
        this._center.add_child(scroll);

        // Scroll de 60 px por paso animado con un muelle (k 260) en vez de
        // saltar directo al valor del adjustment.
        const vscrollBar = scroll.vscroll ||
            (typeof scroll.get_vscroll_bar === 'function' ? scroll.get_vscroll_bar() : null);
        this._scrollAdjustment = vscrollBar ? vscrollBar.adjustment : null;
        if (this._scrollAdjustment)
            this._scrollSpring.jumpTo(this._scrollAdjustment.value);
        scroll.connect('scroll-event', (_actor, event) => this._onScroll(event));

        Main.layoutManager.addChrome(this._center, {
            affectsStruts: false,
        });
        this._centerOpen = true;
        this._refreshCenter();
        this._positionCenter();
    }

    _positionCenter() {
        if (!this._center || !this._monitor)
            return;
        this._center.set_position(
            this._monitor.x + Math.round(this._monitor.width / 2 - CENTER_WIDTH / 2),
            this._monitor.y + this._panelHeight + PEEK_HEIGHT + 8);
    }

    _onScroll(event) {
        if (!this._scrollAdjustment)
            return Clutter.EVENT_PROPAGATE;
        const direction = event.get_scroll_direction();
        let delta = 0;
        if (direction === Clutter.ScrollDirection.UP)
            delta = -SCROLL_STEP_PX;
        else if (direction === Clutter.ScrollDirection.DOWN)
            delta = SCROLL_STEP_PX;
        else
            return Clutter.EVENT_PROPAGATE;

        const adj = this._scrollAdjustment;
        const upper = Math.max(adj.lower, adj.upper - adj.page_size);
        const target = Math.max(adj.lower, Math.min(upper, this._scrollSpring.target + delta));
        this._scrollSpring.k = 260;
        this._scrollSpring.zeta = zetaFor(this._settings.get_string('bounce'));
        this._scrollSpring.setTarget(target);
        if (!this._scrollRunner.running)
            this._scrollRunner.start();
        return Clutter.EVENT_STOP;
    }

    _scrollTick(dt) {
        if (!this._scrollAdjustment)
            return true;
        this._scrollSpring.step(dt);
        this._scrollAdjustment.value = this._scrollSpring.value;
        return this._scrollSpring.isSettled();
    }

    _closeCenter() {
        this._cardRunner.stop();
        this._scrollRunner.stop();
        this._scrollAdjustment = null;
        for (const id of this._cardExpandTimeouts.values())
            GLib.source_remove(id);
        this._cardExpandTimeouts.clear();
        this._cardSprings.clear();
        this._hoveredCard = null;
        if (this._center) {
            Main.layoutManager.removeChrome(this._center);
            this._center.destroy();
            this._center = null;
            this._cardList = null;
        }
        this._centerOpen = false;
    }

    _refreshCenter() {
        if (!this._center || !this._cardList)
            return;
        this._cardList.destroy_all_children();
        this._cardSprings.clear();
        for (const id of this._cardExpandTimeouts.values())
            GLib.source_remove(id);
        this._cardExpandTimeouts.clear();
        this._hoveredCard = null;

        const palette = this._palette();
        const notifications = [];
        for (const source of this._sourceIds.keys()) {
            for (const notification of source.notifications || [])
                notifications.push(notification);
        }
        for (const notification of notifications)
            this._cardList.add_child(this._buildCard(notification, palette));
    }

    // ---- tarjetas del centro de notificaciones -----------------------------
    // Pasar el cursor: esta tarjeta a 1.035, las demás a 0.965 (muelle k 420);
    // a los 260 ms crece y muestra el cuerpo entero + botones de acción.
    // Papelera: franja de 40 -> 58 px (#E5443C -> #FF453A) al pasar el cursor;
    // borrar desliza y se desvanece en 0.26 s y las tarjetas de abajo suben.

    _buildCard(notification, palette) {
        const card = new St.Widget({
            name: 'opendock-notification-card',
            width: CARD_WIDTH,
            height: CARD_HEIGHT,
            reactive: true,
            style: `background-color: ${palette.card}; border-radius: 16px;`,
        });
        card.set_pivot_point(0.5, 0.5);
        card._opendockNotification = notification;
        // Al rehacer el centro se destruyen las tarjetas con el cursor encima,
        // y aún llega su leave-event: hay que poder reconocerlas.
        card.connect('destroy', () => {
            card._opendockDestroyed = true;
        });

        const text = new St.BoxLayout({
            vertical: true,
            width: CARD_WIDTH - TRASH_WIDTH,
            height: CARD_HEIGHT,
            style: 'padding: 8px 12px;',
        });
        const titleLabel = new St.Label({
            text: notification.title || '',
            style: `color: ${palette.text}; font-size: 13px; font-weight: 600;`,
        });
        const bodyLabel = new St.Label({
            text: notification.body || '',
            style: `color: ${palette.secondary}; font-size: 12px;`,
        });
        text.add_child(titleLabel);
        text.add_child(bodyLabel);
        card.add_child(text);
        card._opendockBodyLabel = bodyLabel;

        const actions = new St.BoxLayout({
            visible: false,
            style: 'spacing: 8px;',
        });
        const pillStyle = `height: ${CARD_ACTION_HEIGHT}px; border-radius: ${CARD_ACTION_HEIGHT / 2}px; ` +
            `background-color: ${palette.cardActive}; color: ${palette.text}; padding: 0 12px; font-size: 11px;`;
        const openPill = new St.Button({label: 'Abrir', style: pillStyle});
        openPill.connect('clicked', () => {
            this._activate(notification);
            return Clutter.EVENT_STOP;
        });
        const dismissPill = new St.Button({label: 'Descartar', style: pillStyle});
        dismissPill.connect('clicked', () => {
            this._dismiss(notification);
            return Clutter.EVENT_STOP;
        });
        actions.add_child(openPill);
        actions.add_child(dismissPill);
        card.add_child(actions);
        actions.set_position(52, CARD_HEIGHT - CARD_ACTION_HEIGHT - 8);
        card._opendockActions = actions;

        const trash = new St.Button({
            name: 'opendock-trash-strip',
            style: `background-color: ${TRASH_COLOR}; border-radius: 0 16px 16px 0;`,
            width: TRASH_WIDTH,
            height: CARD_HEIGHT,
        });
        trash.set_position(CARD_WIDTH - TRASH_WIDTH, 0);
        trash.connect('clicked', () => {
            this._dismiss(notification);
            return Clutter.EVENT_STOP;
        });
        trash.connect('enter-event', () => {
            trash.set_style(`background-color: ${TRASH_COLOR_HOVER}; border-radius: 0 16px 16px 0;`);
            trash.ease({
                width: TRASH_WIDTH_HOVER,
                duration: 150,
                mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            });
            trash.set_position(CARD_WIDTH - TRASH_WIDTH_HOVER, 0);
            return Clutter.EVENT_PROPAGATE;
        });
        trash.connect('leave-event', () => {
            trash.set_style(`background-color: ${TRASH_COLOR}; border-radius: 0 16px 16px 0;`);
            trash.ease({
                width: TRASH_WIDTH,
                duration: 150,
                mode: Clutter.AnimationMode.EASE_IN_QUAD,
            });
            trash.set_position(CARD_WIDTH - TRASH_WIDTH, 0);
            return Clutter.EVENT_PROPAGATE;
        });
        card.add_child(trash);

        card.connect('enter-event', () => {
            this._onCardEnter(card);
            return Clutter.EVENT_PROPAGATE;
        });
        card.connect('leave-event', (_actor, event) => {
            // Ignora el "leave" cuando el cursor solo pasó a un hijo (p.ej.
            // la papelera o los botones de acción), que también lo dispara.
            if (card.contains(event.get_related()))
                return Clutter.EVENT_PROPAGATE;
            this._onCardLeave(card);
            return Clutter.EVENT_PROPAGATE;
        });
        card.connect('button-press-event', (_actor, event) => {
            const source = event.get_source();
            if (source === trash || source === openPill || source === dismissPill)
                return Clutter.EVENT_PROPAGATE;
            this._activate(notification);
            return Clutter.EVENT_STOP;
        });

        this._cardSprings.set(card, new Spring(420, zetaFor(this._settings.get_string('bounce')), 1));
        return card;
    }

    _onCardEnter(card) {
        this._hoveredCard = card;
        for (const [c, spring] of this._cardSprings)
            spring.setTarget(c === card ? CARD_HOVER_SCALE : CARD_OTHER_SCALE);
        if (!this._cardRunner.running)
            this._cardRunner.start();

        const existing = this._cardExpandTimeouts.get(card);
        if (existing)
            GLib.source_remove(existing);
        const id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, CARD_EXPAND_MS, () => {
            this._cardExpandTimeouts.delete(card);
            if (this._hoveredCard === card)
                this._setCardExpanded(card, true);
            return GLib.SOURCE_REMOVE;
        });
        this._cardExpandTimeouts.set(card, id);
    }

    _onCardLeave(card) {
        if (this._hoveredCard === card)
            this._hoveredCard = null;
        for (const spring of this._cardSprings.values())
            spring.setTarget(1);
        if (!this._cardRunner.running)
            this._cardRunner.start();

        const id = this._cardExpandTimeouts.get(card);
        if (id) {
            GLib.source_remove(id);
            this._cardExpandTimeouts.delete(card);
        }
        this._setCardExpanded(card, false);
    }

    _setCardExpanded(card, expanded) {
        if (card._opendockDestroyed || card._opendockExpanded === expanded)
            return;
        card._opendockExpanded = expanded;
        if (card._opendockActions)
            card._opendockActions.visible = expanded;
        if (card._opendockBodyLabel)
            card._opendockBodyLabel.clutter_text.set_line_wrap(expanded);
        card.ease({
            height: expanded ? CARD_HEIGHT + 32 : CARD_HEIGHT,
            duration: 150,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
        });
    }

    _cardTick(dt) {
        let settled = true;
        for (const [card, spring] of this._cardSprings) {
            spring.step(dt);
            card.set_scale(spring.value, spring.value);
            if (!spring.isSettled())
                settled = false;
        }
        return settled;
    }

    _findCardFor(notification) {
        if (!this._cardList)
            return null;
        return this._cardList.get_children().find(c => c._opendockNotification === notification) || null;
    }

    _activate(notification) {
        try {
            if (typeof notification.activate === 'function')
                notification.activate();
        } catch (e) {
            logError(e, 'OpenDock: no se pudo activar la notificación');
        }
        this._closeCenter();
    }

    _destroyNotification(notification) {
        try {
            if (typeof notification.destroy === 'function')
                notification.destroy();
        } catch (e) {
            logError(e, 'OpenDock: no se pudo descartar la notificación');
        }
    }

    _dismiss(notification) {
        const card = this._findCardFor(notification);
        if (!card) {
            this._destroyNotification(notification);
            this._refreshCenter();
            return;
        }
        this._cardSprings.delete(card);
        const timeoutId = this._cardExpandTimeouts.get(card);
        if (timeoutId) {
            GLib.source_remove(timeoutId);
            this._cardExpandTimeouts.delete(card);
        }
        if (this._hoveredCard === card)
            this._hoveredCard = null;

        card.ease({
            translation_x: CARD_WIDTH,
            opacity: 0,
            duration: DELETE_FADE_MS,
            mode: Clutter.AnimationMode.EASE_IN_QUAD,
            onComplete: () => {
                card.ease({
                    height: 0,
                    duration: 150,
                    mode: Clutter.AnimationMode.EASE_IN_QUAD,
                    onComplete: () => {
                        this._destroyNotification(notification);
                        this._refreshCenter();
                    },
                });
            },
        });
    }

    _clearAll() {
        const notifications = [];
        for (const source of this._sourceIds.keys()) {
            for (const notification of [...(source.notifications || [])])
                notifications.push(notification);
        }
        for (const notification of notifications)
            this._destroyNotification(notification);
        this._refreshCenter();
    }
}
