// Integrador de muelles (ver linux/DESIGN.md):
// a = (objetivo - x)*k - v*2*sqrt(k)*zeta, masa 1, Euler semiimplícito,
// 2 subpasos de dt/2, dt <= 0.05 s.

import GLib from 'gi://GLib';

export const ZETA = {
    soft: 0.85,
    normal: 0.62,
    bouncy: 0.40,
};

export class Spring {
    constructor(k, zeta, value = 0) {
        this.k = k;
        this.zeta = zeta;
        this.value = value;
        this.velocity = 0;
        this.target = value;
    }

    setTarget(target) {
        this.target = target;
    }

    jumpTo(value) {
        this.value = value;
        this.target = value;
        this.velocity = 0;
    }

    step(dt) {
        dt = Math.min(dt, 0.05);
        const sub = dt / 2;
        for (let i = 0; i < 2; i++) {
            const a = (this.target - this.value) * this.k -
                this.velocity * 2 * Math.sqrt(this.k) * this.zeta;
            this.velocity += a * sub;
            this.value += this.velocity * sub;
        }
        return this.value;
    }

    isSettled(epsilon = 0.002) {
        return Math.abs(this.target - this.value) < epsilon &&
            Math.abs(this.velocity) < epsilon;
    }
}

// Transición corta de propiedades de un actor (ancho, alto, opacidad...), con
// nuestro propio temporizador. No se usa actor.ease(): GNOME lo deja en un salto
// seco cuando las animaciones del escritorio están apagadas (máquinas virtuales,
// gráficos por software, ajuste de accesibilidad) y OpenDock se quedaba sin ellas.
const _tweens = new Map();   // actor -> {sourceId, destroyId, items}

function _dropTween(actor, state) {
    if (_tweens.get(actor) !== state)
        return;
    _tweens.delete(actor);
    if (state.sourceId) {
        GLib.source_remove(state.sourceId);
        state.sourceId = null;
    }
    if (state.destroyId) {
        try {
            actor.disconnect(state.destroyId);
        } catch (e) {
            // el actor ya no existe
        }
        state.destroyId = null;
    }
}

function _tweenTick(actor, state) {
    const now = GLib.get_monotonic_time();
    const done = [];
    for (const item of [...state.items]) {
        const t = Math.max(0, Math.min(1, (now - item.start) / (item.duration * 1000)));
        const e = item.easeIn ? t * t : t * (2 - t);
        for (const [name, [from, to]] of Object.entries(item.props)) {
            const v = from + (to - from) * e;
            actor[name] = name === 'opacity' ? Math.round(v) : v;
        }
        if (t >= 1) {
            state.items.splice(state.items.indexOf(item), 1);
            done.push(item);
        }
    }
    for (const item of done) {
        if (item.onComplete)
            item.onComplete();
    }
    if (_tweens.get(actor) !== state)
        return GLib.SOURCE_REMOVE;       // destruido o cancelado en un onComplete
    if (state.items.length)
        return GLib.SOURCE_CONTINUE;
    state.sourceId = null;
    _dropTween(actor, state);
    return GLib.SOURCE_REMOVE;
}

export function tween(actor, props, duration, {easeIn = false, onComplete = null} = {}) {
    if (!actor)
        return;
    let state = _tweens.get(actor);
    if (!state) {
        state = {sourceId: null, destroyId: null, items: []};
        _tweens.set(actor, state);
        state.destroyId = actor.connect('destroy', () => {
            state.destroyId = null;
            _dropTween(actor, state);
        });
    }
    // una propiedad sólo la lleva la transición más nueva
    for (const item of state.items) {
        for (const name of Object.keys(props))
            delete item.props[name];
    }
    const item = {props: {}, start: GLib.get_monotonic_time(), duration: Math.max(1, duration), easeIn, onComplete};
    for (const [name, to] of Object.entries(props))
        item.props[name] = [actor[name], to];
    state.items.push(item);
    if (!state.sourceId)
        state.sourceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 16, () => _tweenTick(actor, state));
}

// Al desactivar la extensión no puede quedar ningún temporizador vivo.
export function cancelAllTweens() {
    for (const [actor, state] of [..._tweens])
        _dropTween(actor, state);
}

// Conduce una o varias springs a 60 Hz con GLib.timeout_add, y se detiene
// sola cuando todas quedan asentadas (para no dejar temporizadores vivos).
export class SpringRunner {
    constructor(onTick, onSettled = null) {
        this._onTick = onTick;
        this._onSettled = onSettled;
        this._sourceId = null;
        this._lastTime = null;
    }

    start() {
        if (this._sourceId)
            return;
        this._lastTime = GLib.get_monotonic_time();
        this._sourceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 16, () => {
            const now = GLib.get_monotonic_time();
            const dt = (now - this._lastTime) / 1000000;
            this._lastTime = now;
            const settled = this._onTick(dt);
            if (settled) {
                this._sourceId = null;
                if (this._onSettled)
                    this._onSettled();
                return GLib.SOURCE_REMOVE;
            }
            return GLib.SOURCE_CONTINUE;
        });
    }

    stop() {
        if (this._sourceId) {
            GLib.source_remove(this._sourceId);
            this._sourceId = null;
        }
    }

    get running() {
        return this._sourceId !== null;
    }
}
