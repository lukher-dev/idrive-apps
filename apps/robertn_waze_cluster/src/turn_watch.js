// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 lukher-dev
'use strict';
/**
 * turn_watch - tells the render whether a turn is close, for the HUD map
 * that shows only near turns. Runs from the app's start, page shown or not,
 * while setActive(true) (the HUD map on, with Show HUD map only near turns
 * ticked).
 *
 * It reads, read-only, the maneuvers BMW navigation keeps for its own HUD
 * view (GuidanceEventList Main_Instance, view HUD.RB.Main, which the
 * phone's navigation feeds): the view's config once (getViewConfig, PR
 * #94's fallback when it does not read), then queryContent every queryMs.
 * A call not answered within CALL_MS counts as failed; six failed queries
 * in a row read the config again and count as no maneuver. A start that
 * fails (no proxy yet at boot) is tried again every RETRY_MS while active.
 * TurnGate (turn_gate.js) decides, at the distance setDistance(m) gives
 * (the render's hud_near_m; DEFAULT_NEAR_M while none is known): the
 * running gate takes a new one at once, open or closed as it was, and a
 * gate made when the watch starts again starts with it. Each change goes
 * to send() as {"cmd":"turn","near":"1"|"0"}, and the current value again every
 * resendMs and on resend() (the page's render link coming up), so a render
 * that restarted hears it. Inactive, nothing is polled or sent, and a
 * near turn is taken back once.
 *
 * Each maneuver is logged once ("turn: intersection 350 m (Main St)"), and
 * the first raw reply of each route, so a drive records what the car sends.
 * The clock is monotonic: the head unit's wall clock jumps at time sync.
 */
const EventEmitter = require('events');
const { parseManeuver, TurnGate } = require('./turn_gate.js');

const IFACE = 'de.bmw.infotainment.navigation.guiding.GuidanceEventList';
const INSTANCE = 'Main_Instance';
const VIEW = 'HUD.RB.Main';
const CALL_MS = 3000;
const RETRY_MS = 20000;
const FAILS_BEFORE_RECONFIG = 6;
const RAW_MAX = 2000;
/* The distance before a turn the map shows from while none is known, in metres. */
const DEFAULT_NEAR_M = 1000;
/* What the HUD view's own config asks for, if it cannot be read (PR #94). */
const FALLBACK_CONFIG = {
    viewport: {
        startPoint: { anchorPoint: { standardReferencePoint: 'CCP' }, offsets: [] }, maxElementCount: 1, endPoints: [],
        includedTypes: ['INCLUDE_DESTINATIONS', 'INCLUDE_ACTIVE_MANEUVERS']
    },
    options: ['ENABLE_PRIORITIZED_VIEW'], minimumUpdateIntervalMs: 500, segmentItemRasterLength: 0
};

function monotonicMs() {
    const t = process.hrtime();
    return t[0] * 1e3 + t[1] / 1e6;
}

class TurnWatch extends EventEmitter {
    /* options: builders (oap-sdk someipBuilders), send(msg), log ({info, warn}),
     * queryMs, resendMs, now(), timers ({setInterval, clearInterval,
     * setTimeout, clearTimeout}), gate (a TurnGate, whose distance the watch sets as for its own). */
    constructor(options) {
        super();
        const o = options || {};
        this._builders = o.builders;
        this._send = o.send || function () {};
        this._log = o.log || { info() {}, warn() {} };
        this._queryMs = o.queryMs || 500;
        this._resendMs = o.resendMs || 30000;
        this._now = o.now || monotonicMs;
        this._timers = o.timers || { setInterval, clearInterval, setTimeout, clearTimeout };
        this._nearM = DEFAULT_NEAR_M;
        this._newGate = () => {
            const gate = o.gate || new TurnGate();
            gate.setDistance(this._nearM);
            return gate;
        };
        this._gate = this._newGate();
        this._active = false;
        this._stopped = false;
        this._gen = 0;
        this._proxy = null;
        this._proxyPromise = null;
        this._config = null;
        this._busy = null;
        this._errors = 0;
        this._pollTimer = null;
        this._resendTimer = null;
        this._retryTimer = null;
        this.status = 'off';
        this.error = null;
        this.near = false;
        this.maneuver = null;
    }

    setActive(on) {
        if (!!on === this._active || (on && this._stopped)) {
            return;
        }
        this._active = !!on;
        this._gen++;
        if (this._active) {
            this._start(this._gen);
            return;
        }
        this._stopTimers();
        this._busy = null;
        this._gate = this._newGate();
        this.maneuver = null;
        if (this.near) {
            this.near = false;
            this._send({ cmd: 'turn', near: '0' });
        }
        this._setStatus('off');
    }

    /* How far before a turn the map shows, in metres: anything but a positive number is DEFAULT_NEAR_M. */
    setDistance(m) {
        const d = typeof m === 'number' && m > 0 ? m : DEFAULT_NEAR_M;
        if (d !== this._nearM) {
            this._nearM = d;
            this._gate.setDistance(d);
        }
    }

    /* The current value again, now: the page's render link has come up. */
    resend() {
        if (this._active && this.status === 'available') {
            this._send({ cmd: 'turn', near: this.near ? '1' : '0' });
        }
    }

    stop() {
        this.setActive(false);
        this._stopped = true;
        this._shutdown(this._proxy);
        this._proxy = null;
    }

    _shutdown(p) {
        try {
            if (p && p.shutdown) {
                p.shutdown();
            }
        } catch (e) {
            /* going anyway */
        }
    }

    /* One proxy, shared by every start; one that comes after stop() is shut down. */
    _proxyReady() {
        if (!this._proxyPromise) {
            this._proxyPromise = Promise.resolve()
                .then(() => this._builders.buildTypedProxy(IFACE, INSTANCE))
                .then((p) => Promise.resolve(p.init()).then(() => {
                    if (this._stopped) {
                        this._shutdown(p);
                        throw new Error('stopped');
                    }
                    this._proxy = p;
                    return p;
                }));
            this._proxyPromise.catch(() => {
                this._proxyPromise = null;
            });
        }
        return this._proxyPromise;
    }

    _start(gen) {
        this._setStatus('connecting');
        const live = () => this._active && gen === this._gen;
        this._proxyReady()
            .then((p) => {
                if (!live()) return null;
                this._setStatus('waiting for navigation');
                return p.waitUntilAvailable();
            })
            .then(() => {
                if (!live()) return;
                this.error = null;
                this._setStatus('available');
                this._pollTimer = this._timers.setInterval(() => this._tick(), this._queryMs);
                this._resendTimer = this._timers.setInterval(() => this.resend(), this._resendMs);
                this._tick();
            })
            .catch((e) => {
                if (!live()) return;
                this.error = (e && e.message) || String(e);
                this._log.warn(`turn: GuidanceEventList unavailable: ${this.error}; again in ${RETRY_MS / 1000} s`);
                this._setStatus('unavailable');
                this._retryTimer = this._timers.setTimeout(() => {
                    this._retryTimer = null;
                    if (live()) this._start(gen);
                }, RETRY_MS);
            });
    }

    _stopTimers() {
        if (this._pollTimer) this._timers.clearInterval(this._pollTimer);
        if (this._resendTimer) this._timers.clearInterval(this._resendTimer);
        if (this._retryTimer) this._timers.clearTimeout(this._retryTimer);
        this._pollTimer = this._resendTimer = this._retryTimer = null;
    }

    /* A call that settles within CALL_MS; a synchronous throw is a rejection. */
    _call(name, params) {
        return new Promise((resolve, reject) => {
            const timer = this._timers.setTimeout(() => reject(new Error(`${name}: no answer in ${CALL_MS} ms`)), CALL_MS);
            const settle = (fn) => (v) => {
                this._timers.clearTimeout(timer);
                fn(v);
            };
            Promise.resolve()
                .then(() => {
                    /* the raw call is what the SDK takes on the car (PR #94); the typed method otherwise */
                    const raw = this._proxy && this._proxy._someIpProxy;
                    if (raw && typeof raw.callMethod === 'function') return raw.callMethod(name, params, CALL_MS);
                    if (this._proxy && typeof this._proxy[name] === 'function') return this._proxy[name](params);
                    throw new Error('no method call path');
                })
                .then(settle(resolve), settle(reject));
        });
    }

    _tick() {
        if (this._busy || !this._active) {
            return;
        }
        const token = {};
        this._busy = token;
        const gen = this._gen;
        const done = () => {
            if (this._busy === token) this._busy = null;
        };
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
                } else if (gen === this._gen) {
                    this._decide(null);
                }
            })
            .catch((e) => this._log.warn(`turn: ${(e && e.message) || e}`))
            .then(done);
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
        const flipped = this._decide(m);
        const moved = !before !== !m || (m && before && (before.kind !== m.kind ||
            Math.round(before.metres || 0) !== Math.round(m.metres || 0)));
        if (moved && !flipped) {
            this._emitChange();
        }
    }

    /* The gate given m; a change sent and emitted. A query that failed
     * gives it null with the last maneuver kept, so its hold runs while the
     * service does not answer. Returns whether near changed. */
    _decide(m) {
        const near = this._gate.update(m, this._now());
        if (near === this.near) {
            return false;
        }
        this.near = near;
        this._send({ cmd: 'turn', near: near ? '1' : '0' });
        this._emitChange();
        return true;
    }

    _setStatus(s) {
        if (s !== this.status) {
            this.status = s;
            this._emitChange();
        }
    }

    /* A listener that throws (the page drawing) never stops the watch. */
    _emitChange() {
        try {
            this.emit('change');
        } catch (e) {
            this._log.warn(`turn: a change listener failed: ${(e && e.message) || e}`);
        }
    }
}

module.exports = { TurnWatch, IFACE, INSTANCE, VIEW, DEFAULT_NEAR_M };
