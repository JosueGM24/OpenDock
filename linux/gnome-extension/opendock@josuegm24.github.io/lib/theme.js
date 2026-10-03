// Colores y materiales compartidos (ver linux/DESIGN.md: tabla "Materiales").

import Gio from 'gi://Gio';

const INTERFACE_SCHEMA = 'org.gnome.desktop.interface';

export function isSystemDark() {
    try {
        const source = Gio.SettingsSchemaSource.get_default();
        if (!source || !source.lookup(INTERFACE_SCHEMA, true))
            return true;
        const settings = new Gio.Settings({schema_id: INTERFACE_SCHEMA});
        const scheme = settings.get_string('color-scheme');
        return scheme !== 'prefer-light';
    } catch (e) {
        return true;
    }
}

// rgba() en 0..1 para St/Cairo a partir de un color hex "#RRGGBB".
export function hexToRgba(hex, alpha = 1) {
    const value = hex.replace('#', '');
    const r = parseInt(value.substring(0, 2), 16) / 255;
    const g = parseInt(value.substring(2, 4), 16) / 255;
    const b = parseInt(value.substring(4, 6), 16) / 255;
    return [r, g, b, alpha];
}

export function cssRgba(hex, alpha = 1) {
    const value = hex.replace('#', '');
    const r = parseInt(value.substring(0, 2), 16);
    const g = parseInt(value.substring(2, 4), 16);
    const b = parseInt(value.substring(4, 6), 16);
    return `rgba(${r}, ${g}, ${b}, ${alpha})`;
}

const PALETTES = {
    oled: {
        background: '#000000',
        card: '#1C1C1E',
        cardActive: '#2C2C2E',
        text: '#FFFFFF',
        secondary: '#AEAEB2',
        tertiary: '#6E6E73',
    },
    glass: {
        background: '#1C1C1E',
        backgroundAlpha: 0.52,
        card: '#2C2C2E',
        cardActive: '#3A3A3C',
        text: '#FFFFFF',
        secondary: '#AEAEB2',
        tertiary: '#6E6E73',
    },
    'system-dark': {
        background: '#1C1C1E',
        card: '#1C1C1E',
        cardActive: '#2C2C2E',
        text: '#FFFFFF',
        secondary: '#AEAEB2',
        tertiary: '#6E6E73',
    },
    'system-light': {
        background: '#F2F2F7',
        card: '#FFFFFF',
        cardActive: '#E5E5EA',
        text: '#000000',
        secondary: '#6C6C70',
        tertiary: '#A1A1A6',
    },
};

export const ACCENTS = {
    blue: '#0A84FF',
    purple: '#BF5AF2',
    red: '#FF375F',
    orange: '#FF9F0A',
    green: '#30D158',
    gray: '#98989D',
};

export function paletteFor(material) {
    if (material === 'system')
        return isSystemDark() ? PALETTES['system-dark'] : PALETTES['system-light'];
    return PALETTES[material] || PALETTES.oled;
}

export function zetaFor(bounce) {
    switch (bounce) {
    case 'soft': return 0.85;
    case 'bouncy': return 0.40;
    case 'normal':
    default: return 0.62;
    }
}
