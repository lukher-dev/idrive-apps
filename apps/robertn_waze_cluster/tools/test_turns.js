// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 lukher-dev
'use strict';
/**
 * The HUD map near turns: distances read in metres from what BMW's
 * GuidanceEventList shows (metric, imperial, fractions, a decimal comma;
 * anything else none), the first maneuver of a queryContent reply, and the
 * gate: opened by a turn close ahead (a motorway exit from further), kept
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

let passed = 0;
async function check(name, fn) {
    await fn();
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

/* The gate's answer at each step of a fixture, from t 0. */
function replay(name, gate) {
    const f = fixture(name);
    const g = gate || new TurnGate();
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
    await check('toMetres: fractions of a mile', () => {
        assert(near(toMetres('¼', 'mi'), 402.336));
        assert(near(toMetres('1/4', 'MILES'), 402.336));
        assert(near(toMetres('1 1/2', 'mi'), 2414.016));
        assert(near(toMetres('1½', 'mi'), 2414.016));
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

    await check('approach opens at 400 m', () => {
        const g = new TurnGate();
        assert.deepStrictEqual([600, 450, 401, 400].map((m, i) => g.update(man('intersection', m), i * 500)),
            [false, false, false, true]);
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
    await check('closes after 10 s past hideM', () => {
        const g = new TurnGate();
        assert.strictEqual(g.update(man('intersection', 100), 0), true);
        assert.strictEqual(g.update(man('intersection', 900), 1000), true);
        assert.strictEqual(g.update(man('intersection', 900), 10999), true);
        assert.strictEqual(g.update(man('intersection', 900), 11000), false);
    });
    await check('stays open up to hideM, for a branch further', () => {
        const g = new TurnGate();
        assert.strictEqual(g.update(man('intersection', 100), 0), true);
        assert.strictEqual(g.update(man('intersection', 800), 1000), true);
        assert.strictEqual(g.update(man('intersection', 800), 60000), true);
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
        assert.strictEqual(g.update(man('intersection', 900), 1000), true);
        assert.strictEqual(g.update(man('intersection', 700), 9000), true);
        assert.strictEqual(g.update(man('intersection', 900), 10000), true);
        assert.strictEqual(g.update(man('intersection', 900), 19999), true);
        assert.strictEqual(g.update(man('intersection', 900), 20000), false);
    });
    await check('closed, it opens again at the next turn', () => {
        const g = new TurnGate();
        g.update(man('intersection', 100), 0);
        g.update(null, 1000);
        assert.strictEqual(g.update(null, 11000), false);
        assert.strictEqual(g.update(man('intersection', 390), 12000), true);
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

    console.log(`${passed}/${passed} passed`);
}

main().then(() => process.exit(0), (e) => {
    console.log(`FAIL ${e && e.stack}`);
    process.exit(1);
});
