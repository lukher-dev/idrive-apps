// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 lukher-dev
'use strict';
/**
 * turn_watch - tells the render whether a turn is close, for the HUD map
 * that shows only near turns. Runs from the app's start, page shown or not,
 * while setActive(true) (the HUD map on, with Only near turns ticked).
 *
 * It reads, read-only, the maneuvers BMW navigation keeps for its own HUD
 * view (GuidanceEventList Main_Instance, view HUD.RB.Main, which the
 * phone's navigation feeds): the view's config once (getViewConfig, PR
 * #94's fallback when it does not read), then queryContent every queryMs.
 * Six failed queries in a row read the config again and count as no
 * maneuver. TurnGate (turn_gate.js) decides; each change goes to send() as
 * {"cmd":"turn","near":"1"|"0"}, and the current value again every
 * resendMs and on resend() (the page's render link coming up), so a render
 * that restarted hears it. Inactive, nothing is polled or sent, and a
 * near turn is taken back once.
 *
 * Each maneuver is logged once ("turn: intersection 350 m (Main St)"), and
 * the first raw reply of each route, so a drive records what the car sends.
 */
const EventEmitter = require('events');
const { parseManeuver, TurnGate } = require('./turn_gate.js');

const IFACE = 'de.bmw.infotainment.navigation.guiding.GuidanceEventList';
const INSTANCE = 'Main_Instance';
const VIEW = 'HUD.RB.Main';
const CALL_MS = 3000;
const FAILS_BEFORE_RECONFIG = 6;
const RAW_MAX = 2000;
/* What the HUD view's own config asks for, if it cannot be read (PR #94). */
const FALLBACK_CONFIG = {
    viewport: {
        startPoint: { anchorPoint: { standardReferencePoint: 'CCP' }, offsets: [] }, maxElementCount: 1, endPoints: [],
        includedTypes: ['INCLUDE_DESTINATIONS', 'INCLUDE_ACTIVE_MANEUVERS']
    },
    options: ['ENABLE_PRIORITIZED_VIEW'], minimumUpdateIntervalMs: 500, segmentItemRasterLength: 0
};

class TurnWatch extends EventEmitter {
    /* options: builders (oap-sdk someipBuilders), send(msg), log ({info, warn}),
     * queryMs, resendMs, now(), timers ({setInterval, clearInterval}), gate. */
    constructor(options) {
        super();
        const o = options || {};
        this._builders = o.builders;
        this._send = o.send || function () {};
        this._log = o.log || { info() {}, warn() {} };
        this._queryMs = o.queryMs || 500;
        this._resendMs = o.resendMs || 30000;
        this._now = o.now || Date.now;
        this._timers = o.timers || { setInterval, clearInterval };
        this._newGate = () => o.gate || new TurnGate();
        this._gate = this._newGate();
        this._active = false;
        this._gen = 0;
        this._proxy = null;
        this._config = null;
        this._busy = false;
        this._errors = 0;
        this._pollTimer = null;
        this._resendTimer = null;
        this.status = 'off';
        this.error = null;
        this.near = false;
        this.maneuver = null;
    }

    setActive(on) {
        if (!!on === this._active) {
            return;
        }
        this._active = !!on;
        this._gen++;
        if (this._active) {
            this._start(this._gen);
            return;
        }
        this._stopTimers();
        this._gate = this._newGate();
        this.maneuver = null;
        if (this.near) {
            this.near = false;
            this._send({ cmd: 'turn', near: '0' });
        }
        this._setStatus('off');
    }

    /* The current value again, now: the page's render link has come up. */
    resend() {
        if (this._active && this.status === 'available') {
            this._send({ cmd: 'turn', near: this.near ? '1' : '0' });
        }
    }

    stop() {
        this.setActive(false);
        try {
            if (this._proxy && this._proxy.shutdown) {
                this._proxy.shutdown();
            }
        } catch (e) {
            /* going anyway */
        }
        this._proxy = null;
    }

    _start(gen) {
        this._setStatus('connecting');
        const live = () => this._active && gen === this._gen;
        const proxy = this._proxy ? Promise.resolve(this._proxy) : Promise.resolve()
            .then(() => this._builders.buildTypedProxy(IFACE, INSTANCE))
            .then((p) => Promise.resolve(p.init()).then(() => {
                this._proxy = p;
                return p;
            }));
        proxy
            .then((p) => {
                if (!live()) return null;
                this._setStatus('waiting for navigation');
                return p.waitUntilAvailable();
            })
            .then(() => {
                if (!live()) return;
                this._setStatus('available');
                this._pollTimer = this._timers.setInterval(() => this._tick(), this._queryMs);
                this._resendTimer = this._timers.setInterval(() => this.resend(), this._resendMs);
                this._tick();
            })
            .catch((e) => {
                if (!live()) return;
                this.error = (e && e.message) || String(e);
                this._log.warn(`turn: GuidanceEventList unavailable: ${this.error}`);
                this._setStatus('unavailable');
            });
    }

    _stopTimers() {
        if (this._pollTimer) this._timers.clearInterval(this._pollTimer);
        if (this._resendTimer) this._timers.clearInterval(this._resendTimer);
        this._pollTimer = this._resendTimer = null;
    }

    _call(name, params) {
        /* the raw call is what the SDK takes on the car (PR #94); the typed method otherwise */
        const raw = this._proxy && this._proxy._someIpProxy;
        if (raw && typeof raw.callMethod === 'function') {
            return Promise.resolve(raw.callMethod(name, params, CALL_MS));
        }
        if (this._proxy && typeof this._proxy[name] === 'function') {
            return Promise.resolve(this._proxy[name](params));
        }
        return Promise.reject(new Error('no method call path'));
    }

    _tick() {
        if (this._busy || !this._active) {
            return;
        }
        this._busy = true;
        const gen = this._gen;
        const config = this._config ? Promise.resolve(this._config) : this._call('getViewConfig', { viewId: VIEW })
            .then((r) => (r && r.viewConfig && r.GenericListError !== 'UNKNOWN_VIEW_ID' ? r.viewConfig : FALLBACK_CONFIG),
                () => FALLBACK_CONFIG)
            .then((c) => (this._config = c));
        config
            .then((c) => this._call('queryContent', { viewConfigs: [c] }))
            .then((r) => {
                this._errors = 0;
                if (gen === this._gen) this._update(parseManeuver(r), r);
            }, () => {
                if (++this._errors >= FAILS_BEFORE_RECONFIG) {
                    this._errors = 0;
                    this._config = null;
                    if (gen === this._gen) this._update(null, null);
                }
            })
            .then(() => {
                this._busy = false;
            });
    }

    _update(m, raw) {
        const before = this.maneuver;
        if (m && (!before || before.kind !== m.kind || before.text !== m.text)) {
            const metres = m.metres === null ? '?' : String(Math.round(m.metres));
            this._log.info(`turn: ${m.kind} ${metres} m${m.text ? ` (${m.text})` : ''}`);
            if (!before) {
                this._log.info(`turn reply: ${JSON.stringify(raw).slice(0, RAW_MAX)}`);
            }
        } else if (!m && before) {
            this._log.info('turn: none');
        }
        this.maneuver = m;
        const near = this._gate.update(m, this._now());
        const moved = !before !== !m || (m && before && (before.kind !== m.kind ||
            Math.round(before.metres || 0) !== Math.round(m.metres || 0)));
        if (near !== this.near) {
            this.near = near;
            this._send({ cmd: 'turn', near: near ? '1' : '0' });
        }
        if (moved || near !== this.near) {
            this.emit('change');
        }
    }

    _setStatus(s) {
        if (s !== this.status) {
            this.status = s;
            this.emit('change');
        }
    }
}

module.exports = { TurnWatch, IFACE, INSTANCE, VIEW };
