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
