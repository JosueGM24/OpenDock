import Adw from 'gi://Adw';
import Gio from 'gi://Gio';
import Gtk from 'gi://Gtk';

import {ExtensionPreferences} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

const SOUND_NAMES = [
    'alert', 'ambient', 'celeb1', 'celeb2', 'celeb3', 'complete',
    'confirm', 'deco1', 'deco2', 'intense', 'party', 'simple1', 'simple2',
];

export default class OpenDockPreferences extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        const settings = this.getSettings();
        const page = new Adw.PreferencesPage();
        window.add(page);

        page.add(this._buildAppearanceGroup(settings));
        page.add(this._buildComponentsGroup(settings));
        page.add(this._buildSoundGroup(settings));
    }

    _buildAppearanceGroup(settings) {
        const group = new Adw.PreferencesGroup({title: 'Apariencia'});

        const materialValues = ['oled', 'glass', 'system'];
        const materialRow = new Adw.ComboRow({
            title: 'Material',
            subtitle: 'OLED, vidrio o seguir al sistema',
            model: Gtk.StringList.new(['OLED', 'Vidrio', 'Sistema']),
        });
        materialRow.selected = Math.max(0, materialValues.indexOf(settings.get_string('material')));
        materialRow.connect('notify::selected', () => {
            settings.set_string('material', materialValues[materialRow.selected]);
        });
        group.add(materialRow);

        const bounceValues = ['soft', 'normal', 'bouncy'];
        const bounceRow = new Adw.ComboRow({
            title: 'Rebote',
            subtitle: 'Amortiguación de los muelles',
            model: Gtk.StringList.new(['Suave', 'Normal', 'Bouncy']),
        });
        bounceRow.selected = Math.max(0, bounceValues.indexOf(settings.get_string('bounce')));
        bounceRow.connect('notify::selected', () => {
            settings.set_string('bounce', bounceValues[bounceRow.selected]);
        });
        group.add(bounceRow);

        const radiusRow = new Adw.SpinRow({
            title: 'Radio de las esquinas',
            adjustment: new Gtk.Adjustment({lower: 0, upper: 32, step_increment: 1}),
        });
        settings.bind('corner-radius', radiusRow, 'value', Gio.SettingsBindFlags.DEFAULT);
        group.add(radiusRow);

        return group;
    }

    _buildComponentsGroup(settings) {
        const group = new Adw.PreferencesGroup({title: 'Componentes'});

        const toggles = [
            ['show-corners', 'Esquinas redondeadas'],
            ['show-notch', 'Notch y centro de notificaciones'],
            ['show-dock', 'Dock'],
        ];
        for (const [key, title] of toggles) {
            const row = new Adw.SwitchRow({title});
            settings.bind(key, row, 'active', Gio.SettingsBindFlags.DEFAULT);
            group.add(row);
        }

        const hideValues = ['never', 'half', 'full'];
        const hideRow = new Adw.ComboRow({
            title: 'Ocultar el dock',
            model: Gtk.StringList.new(['Nunca', 'A la mitad', 'Del todo']),
        });
        hideRow.selected = Math.max(0, hideValues.indexOf(settings.get_string('dock-hide-mode')));
        hideRow.connect('notify::selected', () => {
            settings.set_string('dock-hide-mode', hideValues[hideRow.selected]);
        });
        group.add(hideRow);

        const iconSizeRow = new Adw.SpinRow({
            title: 'Tamaño de icono del dock',
            adjustment: new Gtk.Adjustment({lower: 24, upper: 64, step_increment: 1}),
        });
        settings.bind('dock-icon-size', iconSizeRow, 'value', Gio.SettingsBindFlags.DEFAULT);
        group.add(iconSizeRow);

        return group;
    }

    _buildSoundGroup(settings) {
        const group = new Adw.PreferencesGroup({title: 'Sonido'});

        const enabledRow = new Adw.SwitchRow({title: 'Reproducir sonido en las notificaciones'});
        settings.bind('sound-enabled', enabledRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        group.add(enabledRow);

        const soundRow = new Adw.ComboRow({
            title: 'Sonido',
            model: Gtk.StringList.new(SOUND_NAMES),
        });
        soundRow.selected = Math.max(0, SOUND_NAMES.indexOf(settings.get_string('sound-name')));
        soundRow.connect('notify::selected', () => {
            settings.set_string('sound-name', SOUND_NAMES[soundRow.selected]);
        });
        group.add(soundRow);

        const volumeRow = new Adw.SpinRow({
            title: 'Volumen',
            digits: 2,
            adjustment: new Gtk.Adjustment({lower: 0, upper: 1, step_increment: 0.05}),
        });
        settings.bind('sound-volume', volumeRow, 'value', Gio.SettingsBindFlags.DEFAULT);
        group.add(volumeRow);

        return group;
    }
}
