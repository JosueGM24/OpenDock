// Notch bajo la barra superior: reemplaza los banners de GNOME, muestra el
// aviso con muelles (ver linux/DESIGN.md) y abre un centro de notificaciones
// construido con las notificaciones ya existentes en Main.messageTray.

import St from 'gi://St';
import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Spring, SpringRunner} from './spring.js';
import {paletteFor, zetaFor} from './theme.js';
import {playSoundFile} from './sound.js';

const PEEK_WIDTH = 360;
const PEEK_HEIGHT = 54;
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
        this._sourceIds = new Map();
        this._trayIds = [];
        this._patched = false;
        this._originalShowNotification = undefined;

        this._center = null;
        this._centerOpen = false;

        this._hotStrip = null;
        this._mini = null;
        this._miniHoverId = null;
        this._miniSettings = {open: false, quick: false};

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
        this._runner.stop();
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
            style: `color: ${palette.text}; font-weight: 600; font-size: 14px;`,
        });
        this._actor.add_child(this._titleLabel);

        this._bodyLabel = new St.Label({
            x: 52, y: 28,
            style: `color: ${palette.secondary}; font-size: 12px;`,
        });
        this._actor.add_child(this._bodyLabel);

        this._timeLabel = new St.Label({
            y: 8,
            style: `color: ${palette.tertiary}; font-size: 12px;`,
        });
        this._actor.add_child(this._timeLabel);

        Main.layoutManager.addChrome(this._actor, {
            affectsStruts: false,
            affectsInputRegion: true,
            trackFullscreen: false,
        });
        this._actor.set_size(0, 0);
    }

    _buildHotStrip() {
        // Franja central invisible bajo la barra: detecta el cursor para el
        // mini notch (110x9) y la vista rápida a los 450 ms quieto.
        this._hotStrip = new St.Widget({
            name: 'opendock-notch-hotstrip',
            reactive: true,
            width: 200,
            height: 6,
            opacity: 0,
        });
        this._hotStrip.connect('enter-event', () => {
            this._onMiniEnter();
            return Clutter.EVENT_PROPAGATE;
        });
        this._hotStrip.connect('leave-event', () => {
            this._onMiniLeave();
            return Clutter.EVENT_PROPAGATE;
        });
        Main.layoutManager.addChrome(this._hotStrip, {
            affectsStruts: false,
            affectsInputRegion: true,
        });

        this._mini = new St.Widget({visible: false, reactive: false});
        Main.layoutManager.addChrome(this._mini, {
            affectsStruts: false,
            affectsInputRegion: false,
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
        this._bodyLabel.set_text(notification.body ? notification.body.replace(/\n/g, ' ') : '');
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
        this._heightSpring.setTarget(PEEK_HEIGHT);
        if (!this._runner.running)
            this._runner.start();

        if (this._hideTimeoutId)
            GLib.source_remove(this._hideTimeoutId);
        this._hideTimeoutId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, AUTO_HIDE_MS, () => {
            this._hideTimeoutId = null;
            if (!this._centerOpen)
                this._closePeek();
            return GLib.SOURCE_REMOVE;
        });
    }

    _closePeek() {
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

        const shoulder = Math.min(7, h / 2);
        this._actor.set_style(
            `background-color: ${this._palette().background}; ` +
            `border-radius: ${shoulder}px ${shoulder}px 16px 16px;`);

        this._contentBox.visible = h > 0.55 * PEEK_HEIGHT;

        const settled = this._widthSpring.isSettled() && this._heightSpring.isSettled();
        return settled;
    }

    // ---- mini notch / vista rápida ----------------------------------------

    _onMiniEnter() {
        if (this._miniHoverId)
            return;
        this._showMiniPill();
        this._miniHoverId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, HOVER_STILL_MS, () => {
            this._miniHoverId = null;
            this._showQuickView();
            return GLib.SOURCE_REMOVE;
        });
    }

    _onMiniLeave() {
        if (this._miniHoverId) {
            GLib.source_remove(this._miniHoverId);
            this._miniHoverId = null;
        }
        this._hideMini();
    }

    _showMiniPill() {
        if (!this._mini || !this._monitor)
            return;
        const palette = this._palette();
        this._mini.set_style(`background-color: ${palette.card}; border-radius: 4.5px;`);
        this._mini.set_size(MINI_WIDTH, MINI_HEIGHT);
        this._mini.set_position(
            this._monitor.x + Math.round(this._monitor.width / 2 - MINI_WIDTH / 2),
            this._monitor.y + this._panelHeight);
        this._mini.visible = true;
    }

    _showQuickView() {
        if (!this._mini || !this._monitor)
            return;
        const palette = this._palette();
        const count = this._notificationCount();
        this._mini.destroy_all_children();
        this._mini.set_style(`background-color: ${palette.background}; border-radius: 16px;`);
        this._mini.set_size(QUICK_WIDTH, QUICK_HEIGHT);
        this._mini.set_position(
            this._monitor.x + Math.round(this._monitor.width / 2 - QUICK_WIDTH / 2),
            this._monitor.y + this._panelHeight);
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
        scroll.set_child(this._cardList);
        this._center.add_child(scroll);

        Main.layoutManager.addChrome(this._center, {
            affectsStruts: false,
            affectsInputRegion: true,
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

    _closeCenter() {
        if (this._center) {
            Main.layoutManager.removeChrome(this._center);
            this._center.destroy();
            this._center = null;
        }
        this._centerOpen = false;
    }

    _refreshCenter() {
        if (!this._center || !this._cardList)
            return;
        this._cardList.destroy_all_children();
        const palette = this._palette();
        const notifications = [];
        for (const source of this._sourceIds.keys()) {
            for (const notification of source.notifications || [])
                notifications.push(notification);
        }
        for (const notification of notifications)
            this._cardList.add_child(this._buildCard(notification, palette));
    }

    _buildCard(notification, palette) {
        const card = new St.BoxLayout({
            width: CARD_WIDTH,
            height: CARD_HEIGHT,
            reactive: true,
            style: `background-color: ${palette.card}; border-radius: 16px;`,
        });
        const text = new St.BoxLayout({vertical: true, x_expand: true, style: 'padding: 8px 12px;'});
        text.add_child(new St.Label({
            text: notification.title || '',
            style: `color: ${palette.text}; font-size: 13px; font-weight: 600;`,
        }));
        text.add_child(new St.Label({
            text: notification.body || '',
            style: `color: ${palette.secondary}; font-size: 12px;`,
        }));
        card.add_child(text);

        const trash = new St.Button({
            label: '🗑',
            style: 'color: #E5443C; padding: 0 12px;',
            y_align: Clutter.ActorAlign.CENTER,
        });
        trash.connect('clicked', () => {
            this._dismiss(notification);
            return Clutter.EVENT_STOP;
        });
        card.add_child(trash);

        card.connect('button-press-event', (_actor, event) => {
            if (event.get_source() === trash)
                return Clutter.EVENT_PROPAGATE;
            this._activate(notification);
            return Clutter.EVENT_STOP;
        });
        return card;
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

    _dismiss(notification) {
        try {
            if (typeof notification.destroy === 'function')
                notification.destroy();
        } catch (e) {
            logError(e, 'OpenDock: no se pudo descartar la notificación');
        }
        this._refreshCenter();
    }

    _clearAll() {
        for (const source of this._sourceIds.keys()) {
            for (const notification of [...(source.notifications || [])])
                this._dismiss(notification);
        }
    }
}
