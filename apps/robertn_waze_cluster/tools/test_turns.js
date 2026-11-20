// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 lukher-dev
'use strict';
/**
 * The HUD map near turns: distances read in metres from what BMW's
 * GuidanceEventList shows (metric, imperial, fractions, a decimal comma;
 * anything else none), the first maneuver of a queryContent reply, and the
 * gate: opened by a turn close ahead (1000 m by default, or the distance
 * set, a motorway exit from at least 1000 m; a new distance taken while it
 * runs, open or closed as it was), kept
 * open through the turn while its distance blanks and between turns close
 * together, closed only after a while past the turn, at the end of the
 * route, or on a maneuver that is not a turn. The timelines are
 * tools/data/turns-*.json: one reply a step.
 *
 *   node tools/test_turns.js
 */
const assert = require('assert');
const path = require('path');
const { parseManeuver, toMetres, TurnGate, TRIGGER_KINDS } = require('../src/turn_gate.js');
const { TurnWatch } = require('../src/turn_watch.js');

let passed = 0;
const failed = [];
async function check(name, fn) {
    try {
        await fn();
    } catch (e) {
        failed.push(name);
        console.log(`FAIL ${name}: ${(e && e.message || String(e)).split('\n')[0]}`);
        return;
    }
    passed++;
    console.log(`ok   ${name}`);
}

const near = (a, b) => Math.abs(a - b) < 0.01;
const fixture = (name) => require(path.join(__dirname, 'data', `turns-${name}.json`));
/* A reply with one maneuver, as the HUD view returns it. */
const reply = (kind, distance, unit, text) => ({
    viewContent: [{ eventsInView: [{ [kind]: { formattedDistance: { distance, unit }, description: { text: text || '' } } }] }]
});
const man = (kind, metres) => ({ kind, metres, text: '' });

/* The gate's answer at each step of a fixture, from t 0: by default a gate that opens at 400 m. */
function replay(name, gate) {
    const f = fixture(name);
    const g = gate || new TurnGate({ showM: 400 });
    return f.replies.map((r, i) => g.update(parseManeuver(r), i * f.stepMs));
}

async function main() {
    await check('toMetres: metres and kilometres, a decimal comma', () => {
        assert.strictEqual(toMetres('350', 'm'), 350);
        assert.strictEqual(toMetres('1,5', 'KILOMETERS'), 1500);
        assert.strictEqual(toMetres('2', 'km'), 2000);
        assert.strictEqual(toMetres('120', 'METERS'), 120);
    });
    await check('toMetres: miles, feet and yards', () => {
        assert(near(toMetres('0.2', 'mi'), 321.869));
        assert(near(toMetres('1000', 'ft'), 304.8));
        assert(near(toMetres('500', 'YARDS'), 457.2));
    });
    await check('toMetres: BMW\'s unit names (METER, KM, MILE_UK, MILE_US, YARD, FEET)', () => {
        assert.strictEqual(toMetres('350', 'METER'), 350);
        assert.strictEqual(toMetres('1.200', 'METER'), 1200);
        assert.strictEqual(toMetres('1,5', 'KM'), 1500);
        assert(near(toMetres('0.5', 'MILE_UK'), 804.672));
        assert(near(toMetres('0.2', 'MILE_US'), 321.869));
        assert(near(toMetres('¼', 'MILE_US'), 402.336));
        assert(near(toMetres('1 1/2', 'MILE_UK'), 2414.016));
        assert(near(toMetres('300', 'YARD'), 274.32));
        assert(near(toMetres('1,000', 'YARD'), 914.4));
        assert(near(toMetres('500', 'FEET'), 152.4));
        assert(near(toMetres('1,500', 'FEET'), 457.2));
    });
    await check('parseManeuver: a distance in BMW\'s miles, yards and feet', () => {
        assert(near(parseManeuver(reply('intersection', '0.3', 'MILE_US', 'Elm St')).metres, 482.803));
        assert(near(parseManeuver(reply('branch', '1', 'MILE_UK')).metres, 1609.344));
        assert(near(parseManeuver(reply('roundabout', '200', 'YARD')).metres, 182.88));
        assert(near(parseManeuver(reply('intersection', '800', 'FEET')).metres, 243.84));
    });
    await check('the gate opens on a turn in miles: a quarter mile within 1000 m, a mile not', () => {
        const g = new TurnGate();
        assert.strictEqual(g.update(parseManeuver(reply('intersection', '1', 'MILE_US')), 0), false);
        assert.strictEqual(g.update(parseManeuver(reply('intersection', '¼', 'MILE_US')), 500), true);
    });
    await check('toMetres: fractions of a mile', () => {
        assert(near(toMetres('¼', 'mi'), 402.336));
        assert(near(toMetres('1/4', 'MILES'), 402.336));
        assert(near(toMetres('1 1/2', 'mi'), 2414.016));
        assert(near(toMetres('1½', 'mi'), 2414.016));
    });
    await check('toMetres: digit grouping in metres, feet and yards', () => {
        assert(near(toMetres('1,500', 'ft'), 457.2));
        assert.strictEqual(toMetres('1.200', 'm'), 1200);
        assert(near(toMetres('1,000', 'yd'), 914.4));
        assert.strictEqual(toMetres('1,5', 'km'), 1500);
        assert.strictEqual(toMetres('1.500', 'km'), 1.5 * 1000);
        assert(near(toMetres('0.5', 'mi'), 804.672));
    });
    await check('toMetres: none for no number, an unknown unit or none', () => {
        assert.strictEqual(toMetres('', 'm'), null);
        assert.strictEqual(toMetres('now', 'm'), null);
        assert.strictEqual(toMetres('3', 'FURLONG'), null);
        assert.strictEqual(toMetres('3', ''), null);
        assert.strictEqual(toMetres(undefined, undefined), null);
        assert.strictEqual(toMetres('1/0', 'mi'), null);
    });

    await check('parseManeuver: the first maneuver, its distance and text', () => {
        assert.deepStrictEqual(parseManeuver(reply('intersection', '350', 'm', 'Main St')),
            { kind: 'intersection', metres: 350, text: 'Main St' });
        assert.deepStrictEqual(parseManeuver(reply('laneChange', '200', 'm')), { kind: 'laneChange', metres: 200, text: '' });
    });
    await check('parseManeuver: none for no reply or no known maneuver', () => {
        assert.strictEqual(parseManeuver(null), null);
        assert.strictEqual(parseManeuver({}), null);
        assert.strictEqual(parseManeuver({ viewContent: [] }), null);
        assert.strictEqual(parseManeuver({ viewContent: [{ eventsInView: [{ somethingElse: {} }] }] }), null);
    });
    await check('parseManeuver: a single view, not a list', () => {
        const r = reply('roundabout', '90', 'm');
        assert.strictEqual(parseManeuver({ viewContent: r.viewContent[0] }).kind, 'roundabout');
    });
    await check('the turn kinds', () => {
        assert.deepStrictEqual(TRIGGER_KINDS, ['intersection', 'roundabout', 'branch', 'destinationManeuver']);
    });

    await check('approach opens at 1000 m by default', () => {
        const g = new TurnGate();
        assert.deepStrictEqual([1500, 1200, 1001, 1000].map((m, i) => g.update(man('intersection', m), i * 500)),
            [false, false, false, true]);
    });
    await check('the set distance: a turn opens at it, a motorway exit at it but never under 1000 m', () => {
        const at = (d, kind, metres) => {
            const g = new TurnGate();
            g.setDistance(d);
            return g.update(man(kind, metres), 0);
        };
        assert.deepStrictEqual([[200, 201], [200, 200], [400, 400], [1200, 1201], [1200, 1200], [2000, 2000]]
            .map(([d, m]) => at(d, 'intersection', m)), [false, true, true, false, true, true]);
        assert.deepStrictEqual([[200, 1000], [200, 1001], [1400, 1400], [1400, 1401], [2000, 2000]]
            .map(([d, m]) => at(d, 'branch', m)), [true, false, true, false, true]);
        const g = new TurnGate();
        g.setDistance(600);
        assert.deepStrictEqual([g.showM, g.branchShowM, g.extraHideM], [600, 1000, 400]);
        g.setDistance(1800);
        assert.deepStrictEqual([g.showM, g.branchShowM, g.extraHideM], [1800, 1800, 400]);
    });
    await check('a new distance applies to the running gate, open or closed as it was', () => {
        const g = new TurnGate();
        g.setDistance(400);
        assert.strictEqual(g.update(man('intersection', 300), 0), true);
        g.setDistance(200);
        assert.strictEqual(g.open, true, 'still open');
        assert.strictEqual(g.update(man('intersection', 550), 500), true, 'kept open within the new distance plus 400 m');
        assert.strictEqual(g.update(man('intersection', 650), 1000), true, 'past it: the hold');
        assert.strictEqual(g.update(man('intersection', 650), 11000), false);
        g.setDistance(800);
        assert.strictEqual(g.open, false, 'still closed');
        assert.strictEqual(g.update(man('intersection', 700), 11500), true, 'opens at the new distance');
    });
    await check('branch opens at 1000 m', () => {
        const g = new TurnGate();
        assert.deepStrictEqual([1200, 1001, 1000].map((m, i) => g.update(man('branch', m), i * 500)), [false, false, true]);
    });
    await check('does not open for laneChange at 100 m', () => {
        const g = new TurnGate();
        assert.strictEqual(g.update(man('laneChange', 100), 0), false);
        assert.strictEqual(g.update(man('passiveManeuver', 10), 500), false);
    });
    await check('unknown units never open', () => {
        const g = new TurnGate();
        assert.strictEqual(g.update(man('intersection', null), 0), false);
        assert.strictEqual(g.update(man('intersection', null), 60000), false);
    });
    await check('blank distance at the turn keeps it open', () => {
        const g = new TurnGate();
        assert.strictEqual(g.update(man('intersection', 50), 0), true);
        for (let t = 500; t <= 30000; t += 500) assert.strictEqual(g.update(man('intersection', null), t), true, `at ${t}`);
    });
    await check('a blank distance keeps it open at most 60 s, then the hold', () => {
        const g = new TurnGate();
        assert.strictEqual(g.update(man('destinationManeuver', 20), 0), true);
        assert.strictEqual(g.update(man('destinationManeuver', null), 1000), true);
        assert.strictEqual(g.update(man('destinationManeuver', null), 60999), true);
        assert.strictEqual(g.update(man('destinationManeuver', null), 61000), true);
        assert.strictEqual(g.update(man('destinationManeuver', null), 70999), true);
        assert.strictEqual(g.update(man('destinationManeuver', null), 71000), false);
        assert.strictEqual(g.update(man('destinationManeuver', null), 200000), false);
    });
    await check('closes after 10 s past hideM', () => {
        const g = new TurnGate();
        assert.strictEqual(g.update(man('intersection', 100), 0), true);
        assert.strictEqual(g.update(man('intersection', 1500), 1000), true);
        assert.strictEqual(g.update(man('intersection', 1500), 10999), true);
        assert.strictEqual(g.update(man('intersection', 1500), 11000), false);
    });
    await check('stays open up to hideM, for a branch further', () => {
        const g = new TurnGate();
        assert.strictEqual(g.update(man('intersection', 100), 0), true);
        assert.strictEqual(g.update(man('intersection', 1400), 1000), true);
        assert.strictEqual(g.update(man('intersection', 1400), 60000), true);
        assert.strictEqual(g.update(man('branch', 1400), 61000), true);
        assert.strictEqual(g.update(man('branch', 1400), 120000), true);
    });
    await check('route cancelled closes', () => {
        const g = new TurnGate();
        assert.strictEqual(g.update(man('roundabout', 200), 0), true);
        assert.strictEqual(g.update(null, 500), true);
        assert.strictEqual(g.update(null, 10499), true);
        assert.strictEqual(g.update(null, 10500), false);
    });
    await check('a maneuver that is not a turn closes after the hold', () => {
        const g = new TurnGate();
        assert.strictEqual(g.update(man('intersection', 100), 0), true);
        assert.strictEqual(g.update(man('laneChange', 100), 1000), true);
        assert.strictEqual(g.update(man('laneChange', 100), 11000), false);
    });
    await check('a turn back within the hold keeps it open, the hold starts again', () => {
        const g = new TurnGate();
        assert.strictEqual(g.update(man('intersection', 100), 0), true);
        assert.strictEqual(g.update(man('intersection', 1500), 1000), true);
        assert.strictEqual(g.update(man('intersection', 1300), 9000), true);
        assert.strictEqual(g.update(man('intersection', 1500), 10000), true);
        assert.strictEqual(g.update(man('intersection', 1500), 19999), true);
        assert.strictEqual(g.update(man('intersection', 1500), 20000), false);
    });
    await check('closed, it opens again at the next turn', () => {
        const g = new TurnGate();
        g.update(man('intersection', 100), 0);
        g.update(null, 1000);
        assert.strictEqual(g.update(null, 11000), false);
        assert.strictEqual(g.update(man('intersection', 990), 12000), true);
    });
    await check('the thresholds are options', () => {
        const g = new TurnGate({ showM: 200, branchShowM: 500, extraHideM: 100, holdMs: 1000 });
        assert.strictEqual(g.update(man('intersection', 250), 0), false);
        assert.strictEqual(g.update(man('intersection', 200), 100), true);
        assert.strictEqual(g.update(man('intersection', 301), 200), true);
        assert.strictEqual(g.update(man('intersection', 301), 1200), false);
    });

    await check('fixture approach: opens at 400 m, stays through the turn, closes 10 s past it', () => {
        const got = replay('approach');
        const opened = got.indexOf(true);
        assert.strictEqual(opened, 6);
        assert(got.slice(opened, 17).every(Boolean));
        assert.deepStrictEqual(got.slice(17), [true, true, true, true, true, true, true, true, true, true, false, false]);
    });
    await check('close turns stay open', () => {
        const got = replay('close');
        const opened = got.indexOf(true);
        assert.strictEqual(opened, 2);
        assert(got.slice(opened).every(Boolean), JSON.stringify(got));
    });
    await check('fixture cancelled: closes 10 s after the route ends', () => {
        const got = replay('cancelled');
        assert.deepStrictEqual(got, [false, true, true, true, true, true, true, true, true, true, true, true, true, false]);
    });

    await watchChecks();
    console.log(`${passed}/${passed + failed.length} passed`);
    if (failed.length) throw new Error(`${failed.length} failed`);
}

/* ---- TurnWatch against a fake GuidanceEventList and a fake clock ---- */

const flush = () => new Promise((resolve) => setImmediate(resolve));
function fakeClock() {
    let t = 0;
    const timers = [];
    return {
        now: () => t,
        timers: {
            setInterval(fn, ms) {
                const h = { fn, ms, next: t + ms };
                timers.push(h);
                return h;
            },
            clearInterval(h) {
                const i = timers.indexOf(h);
                if (i >= 0) timers.splice(i, 1);
            },
            setTimeout(fn, ms) {
                const h = { fn, ms, next: t + ms, once: true };
                timers.push(h);
                return h;
            },
            clearTimeout(h) {
                const i = timers.indexOf(h);
                if (i >= 0) timers.splice(i, 1);
            }
        },
        pending: () => timers.length,
        async advance(ms) {
            await flush();
            const end = t + ms;
            for (;;) {
                const due = timers.filter((h) => h.next <= end).sort((a, b) => a.next - b.next)[0];
                if (!due) break;
                t = due.next;
                due.next += due.ms;
                if (due.once) timers.splice(timers.indexOf(due), 1);
                due.fn();
                await flush();
            }
            t = end;
            await flush();
        }
    };
}
/* builders whose proxy answers queryContent with replyAt(now); opts: fail
 * (the build rejects), failBuilds (the first that many builds reject),
 * failQueries (that many queryContent calls reject), hangQueries (that many
 * never answer), throwQueries and throwConfig (that many queryContent or
 * getViewConfig calls throw at once), hangFrom (that query and every later
 * one never answer), slowBuild (the build resolves on b.release()). */
function fakeBuilders(clock, replyAt, opts) {
    const o = opts || {};
    const b = { builds: 0, configReads: 0, queries: 0, shutdowns: 0 };
    b.buildTypedProxy = (iface, instance) => {
        b.builds++;
        assert.strictEqual(iface, 'de.bmw.infotainment.navigation.guiding.GuidanceEventList');
        assert.strictEqual(instance, 'Main_Instance');
        if (o.fail || b.builds <= (o.failBuilds || 0)) return Promise.reject(new Error('no such service'));
        const proxy = {
            init: () => Promise.resolve(),
            waitUntilAvailable: () => Promise.resolve(),
            shutdown() { b.shutdowns++; },
            _someIpProxy: {
                callMethod(name, params) {
                    if (name === 'getViewConfig') {
                        b.configReads++;
                        if (o.throwConfig && b.configReads <= o.throwConfig) throw new Error('not connected');
                        assert.deepStrictEqual(params, { viewId: 'HUD.RB.Main' });
                        return Promise.resolve({ viewConfig: { viewport: { maxElementCount: 1 } } });
                    }
                    assert.strictEqual(name, 'queryContent');
                    /* the view's own config, or PR #94's fallback when it did not read */
                    assert(Array.isArray(params.viewConfigs) && params.viewConfigs.length === 1 && params.viewConfigs[0].viewport);
                    b.queries++;
                    if (o.failQueries && b.queries <= o.failQueries) return Promise.reject(new Error('timed out'));
                    if ((o.hangQueries && b.queries <= o.hangQueries) || (o.hangFrom && b.queries >= o.hangFrom)) return new Promise(() => {});
                    if (o.throwQueries && b.queries <= o.throwQueries) throw new Error('not connected');
                    return Promise.resolve(replyAt(clock.now()));
                }
            }
        };
        if (!o.slowBuild) return Promise.resolve(proxy);
        return new Promise((resolve) => { b.release = () => resolve(proxy); });
    };
    return b;
}
const fromFixture = (name) => {
    const f = fixture(name);
    return (t) => f.replies[Math.min(Math.floor(t / f.stepMs), f.replies.length - 1)];
};
function watch(clock, builders, extra) {
    const sent = [], logged = [];
    const w = new TurnWatch(Object.assign({
        builders, now: clock.now, timers: clock.timers,
        send: (m) => sent.push(Object.assign({ at: clock.now() }, m)),
        log: { info: (s) => logged.push(s), warn: (s) => logged.push(s) }
    }, extra || {}));
    return { w, sent, logged };
}

async function watchChecks() {
    await check('inactive: no proxy built, nothing sent', async () => {
        const c = fakeClock(), b = fakeBuilders(c, fromFixture('approach'));
        const { w, sent } = watch(c, b);
        await c.advance(5000);
        assert.strictEqual(b.builds, 0);
        assert.deepStrictEqual(sent, []);
        assert.strictEqual(w.status, 'off');
    });
    await check('approach fixture sends near 1 then near 0', async () => {
        const c = fakeClock(), b = fakeBuilders(c, fromFixture('approach'));
        const { w, sent } = watch(c, b);
        w.setDistance(400);
        w.setActive(true);
        await c.advance(29000);
        assert.deepStrictEqual(sent, [{ at: 6000, cmd: 'turn', near: '1' }, { at: 27000, cmd: 'turn', near: '0' }]);
        assert.strictEqual(w.status, 'available');
        assert.strictEqual(b.configReads, 1);
    });
    await check('resends every 30 s', async () => {
        const c = fakeClock(), b = fakeBuilders(c, () => reply('intersection', '100', 'm'));
        const { w, sent } = watch(c, b);
        w.setActive(true);
        await c.advance(65000);
        assert.deepStrictEqual(sent.map((m) => [m.at, m.near]), [[0, '1'], [30000, '1'], [60000, '1']]);
    });
    await check('resend() sends the current value at once, and nothing while inactive', async () => {
        const c = fakeClock(), b = fakeBuilders(c, () => ({ viewContent: [] }));
        const { w, sent } = watch(c, b);
        w.resend();
        assert.deepStrictEqual(sent, []);
        w.setActive(true);
        await c.advance(1000);
        w.resend();
        assert.deepStrictEqual(sent, [{ at: 1000, cmd: 'turn', near: '0' }]);
    });
    await check('setActive(false) while near sends near 0 once', async () => {
        const c = fakeClock(), b = fakeBuilders(c, () => reply('roundabout', '50', 'm'));
        const { w, sent } = watch(c, b);
        w.setActive(true);
        await c.advance(1000);
        w.setActive(false);
        await c.advance(60000);
        assert.deepStrictEqual(sent.map((m) => [m.at, m.near]), [[0, '1'], [1000, '0']]);
        assert.strictEqual(w.status, 'off');
        assert.strictEqual(w.near, false);
        const queries = b.queries;
        await c.advance(5000);
        assert.strictEqual(b.queries, queries);
    });
    await check('active again: the same proxy, polling again', async () => {
        const c = fakeClock(), b = fakeBuilders(c, () => reply('intersection', '100', 'm'));
        const { w, sent } = watch(c, b);
        w.setActive(true);
        await c.advance(1000);
        w.setActive(false);
        w.setActive(true);
        await c.advance(1000);
        assert.strictEqual(b.builds, 1);
        assert.deepStrictEqual(sent.map((m) => m.near), ['1', '0', '1']);
    });
    await check('proxy unavailable: status unavailable, nothing sent', async () => {
        const c = fakeClock(), b = fakeBuilders(c, () => null, { fail: true });
        const { w, sent } = watch(c, b);
        const statuses = [];
        w.on('change', () => statuses.push(w.status));
        w.setActive(true);
        await c.advance(5000);
        assert.strictEqual(w.status, 'unavailable');
        assert.deepStrictEqual(sent, []);
        assert(statuses.includes('unavailable'));
    });
    await check('six failed queries: config read again, treated as no maneuver', async () => {
        const c = fakeClock(), b = fakeBuilders(c, () => reply('intersection', '100', 'm'), { failQueries: 6 });
        const { w, sent } = watch(c, b);
        w.setActive(true);
        await c.advance(2500);
        assert.strictEqual(b.configReads, 1);
        assert.deepStrictEqual(sent, []);
        await c.advance(500);
        assert.strictEqual(b.configReads, 2);
        assert.deepStrictEqual(sent, [{ at: 3000, cmd: 'turn', near: '1' }]);
        assert.strictEqual(w.maneuver.kind, 'intersection');
    });
    await check('approach fixture at the default 1000 m: near from the first reply', async () => {
        const c = fakeClock(), b = fakeBuilders(c, fromFixture('approach'));
        const { w, sent } = watch(c, b);
        w.setActive(true);
        await c.advance(29000);
        assert.deepStrictEqual(sent, [{ at: 0, cmd: 'turn', near: '1' }, { at: 27000, cmd: 'turn', near: '0' }]);
    });
    await check('setDistance: the running gate takes it, open as it was; a new gate too; none known is 1000 m', async () => {
        const c = fakeClock(), b = fakeBuilders(c, () => reply('intersection', '700', 'm'));
        const { w, sent } = watch(c, b);
        w.setActive(true);
        await c.advance(1000);
        assert.deepStrictEqual(sent.map((m) => m.near), ['1'], '700 m: near at 1000 m');
        const gate = w._gate;
        w.setDistance(400);
        await c.advance(5000);
        assert(w._gate === gate && w.near === true && gate.showM === 400, 'the same gate, still open: 700 m is within 400 m plus 400 m');
        w.setActive(false);
        w.setActive(true);
        await c.advance(5000);
        assert.deepStrictEqual(sent.map((m) => m.near), ['1', '0'], 'a new gate at 400 m: 700 m not near');
        assert.deepStrictEqual([w._gate.showM, w._gate.branchShowM, w._gate.extraHideM], [400, 1000, 400]);
        w.setDistance(undefined);
        await c.advance(1000);
        assert.deepStrictEqual(sent.map((m) => m.near), ['1', '0', '1'], 'none known: 1000 m');
        w.setDistance(2000);
        assert.deepStrictEqual([w._gate.showM, w._gate.branchShowM], [2000, 2000]);
    });
    await check('the gate closing with no maneuver change emits change', async () => {
        const c = fakeClock(), b = fakeBuilders(c, fromFixture('cancelled'));
        const { w } = watch(c, b);
        let lastNear = null;
        w.on('change', () => { lastNear = w.near; });
        w.setActive(true);
        await c.advance(14000);
        assert.strictEqual(w.near, false);
        assert.strictEqual(lastNear, false);
    });
    await check('a query that never answers times out, polling goes on', async () => {
        const c = fakeClock(), b = fakeBuilders(c, () => reply('intersection', '100', 'm'), { hangQueries: 1 });
        const { w, sent } = watch(c, b);
        w.setActive(true);
        await c.advance(10000);
        assert(b.queries > 5, `queries ${b.queries}`);
        assert.strictEqual(sent[0].near, '1');
    });
    await check('the service stops answering while near: the map goes after the hold, not after six timeouts', async () => {
        const c = fakeClock(), b = fakeBuilders(c, () => reply('intersection', '100', 'm'), { hangFrom: 5 });
        const { w, sent } = watch(c, b);
        w.setActive(true);
        await c.advance(2000);
        assert.strictEqual(w.near, true);
        await c.advance(18000);
        assert.deepStrictEqual(sent.map((m) => m.near), ['1', '0']);
        assert(sent[1].at <= 16000, `hidden at ${sent[1].at} ms`);
        assert.strictEqual(w.maneuver.kind, 'intersection', 'the last maneuver kept until six failures');
    });
    await check('a change listener that throws does not stop polling', async () => {
        const c = fakeClock(), b = fakeBuilders(c, fromFixture('approach'));
        const { w, sent, logged } = watch(c, b);
        w.on('change', () => { throw new Error('render failed'); });
        w.setActive(true);
        await c.advance(29000);
        assert.deepStrictEqual(sent.map((m) => m.near), ['1', '0']);
        assert(logged.some((s) => s.includes('render failed')));
    });
    await check('a query that throws at once counts as failed, no crash', async () => {
        const c = fakeClock(), b = fakeBuilders(c, () => reply('intersection', '100', 'm'), { throwQueries: 2, throwConfig: 1 });
        const { w, sent } = watch(c, b);
        w.setActive(true);
        await c.advance(1000);
        assert.strictEqual(b.configReads, 1);
        await c.advance(1000);
        assert.strictEqual(w.status, 'available');
        assert.strictEqual(sent[0].near, '1');
    });
    await check('a failed start is tried again while active, its error cleared', async () => {
        const c = fakeClock(), b = fakeBuilders(c, () => reply('intersection', '100', 'm'), { failBuilds: 1 });
        const { w, sent } = watch(c, b);
        w.setActive(true);
        await c.advance(1000);
        assert.strictEqual(w.status, 'unavailable');
        await c.advance(20000);
        assert.strictEqual(b.builds, 2);
        assert.strictEqual(w.status, 'available');
        assert.strictEqual(w.error, null);
        assert.strictEqual(sent[0].near, '1');
    });
    await check('inactive: no retry of a failed start', async () => {
        const c = fakeClock(), b = fakeBuilders(c, () => null, { fail: true });
        const { w } = watch(c, b);
        w.setActive(true);
        await c.advance(1000);
        w.setActive(false);
        await c.advance(120000);
        assert.strictEqual(b.builds, 1);
        assert.strictEqual(c.pending(), 0);
    });
    await check('off and on during the build: one proxy', async () => {
        const c = fakeClock(), b = fakeBuilders(c, () => reply('intersection', '100', 'm'), { slowBuild: true });
        const { w, sent } = watch(c, b);
        w.setActive(true);
        await c.advance(100);
        w.setActive(false);
        w.setActive(true);
        b.release();
        await c.advance(1000);
        assert.strictEqual(b.builds, 1);
        assert.strictEqual(w.status, 'available');
        assert.deepStrictEqual(sent.map((m) => m.near), ['1']);
    });
    await check('stop during the build: the proxy is shut down when it comes', async () => {
        const c = fakeClock(), b = fakeBuilders(c, () => null, { slowBuild: true });
        const { w } = watch(c, b);
        w.setActive(true);
        await c.advance(100);
        w.stop();
        b.release();
        await c.advance(1000);
        assert.strictEqual(b.shutdowns, 1);
        assert.strictEqual(b.queries, 0);
    });
    await check('logs each maneuver once, and the first reply of a route raw', async () => {
        const c = fakeClock(), b = fakeBuilders(c, fromFixture('approach'));
        const { w, logged } = watch(c, b);
        w.setActive(true);
        await c.advance(29000);
        const turns = logged.filter((s) => s.startsWith('turn: '));
        assert.deepStrictEqual(turns, ['turn: intersection 900 m (Main St)', 'turn: intersection 1500 m (High St)']);
        const raw = logged.filter((s) => s.startsWith('turn reply: '));
        assert.strictEqual(raw.length, 1);
        assert(raw[0].includes('"distance":"900"'));
    });
}

main().then(() => process.exit(0), (e) => {
    console.log(`FAIL ${e && e.stack}`);
    process.exit(1);
});
