// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 lukher-dev
'use strict';
/**
 * turn_gate - whether a turn is close enough for the HUD map, from the
 * maneuvers BMW navigation keeps for its own HUD view (GuidanceEventList,
 * view HUD.RB.Main, which the phone's navigation feeds).
 *
 * parseManeuver(reply) takes the first maneuver of a queryContent reply,
 * as PR #94's guidance_feed.js reads it: its kind, its distance in metres
 * (null when the HUD shows none or one toMetres cannot read) and its text.
 *
 * TurnGate.update(maneuver, nowMs) opens at a turn (TRIGGER_KINDS) within
 * showM, or branchShowM for a motorway exit. Once open it stays open while
 * the next maneuver is a turn within that distance plus extraHideM, or a
 * turn whose distance does not read (the HUD blanks it at the junction)
 * for at most blankMaxMs; it closes after holdMs in a row of anything
 * else: no maneuver (the route ended), a maneuver that is not a turn, or a
 * turn further on.
 */

/* What the HUD view's events can be, as PR #94 found them on the car. */
const KINDS = ['intersection', 'roundabout', 'branch', 'destinationManeuver', 'laneChange', 'tollgate', 'border',
    'vehicleTransportation', 'passiveManeuver'];
/* The ones that are a turn worth the map. */
const TRIGGER_KINDS = ['intersection', 'roundabout', 'branch', 'destinationManeuver'];

const UNITS = {
    m: 1, meter: 1, meters: 1, metre: 1, metres: 1,
    km: 1000, kilometer: 1000, kilometers: 1000, kilometre: 1000, kilometres: 1000,
    mi: 1609.344, mile: 1609.344, miles: 1609.344,
    ft: 0.3048, foot: 0.3048, feet: 0.3048,
    yd: 0.9144, yard: 0.9144, yards: 0.9144
};
const FRACTIONS = { '¼': '1/4', '½': '1/2', '¾': '3/4' };

/* A shown distance in metres: "350" m, "1,500" ft, "1,5" km, "¼" or
 * "1 1/2" mi; null for no number, a unit not known here or none. */
function toMetres(distance, unit) {
    if (typeof distance !== 'string' || typeof unit !== 'string') {
        return null;
    }
    const k = UNITS[unit.trim().toLowerCase()];
    if (!k) {
        return null;
    }
    /* metres, feet and yards are shown whole: a separator before three digits groups them ("1,500 ft", "1.200 m") */
    const whole = k === UNITS.m || k === UNITS.ft || k === UNITS.yd;
    const grouped = whole ? distance.trim().replace(/[.,](?=\d{3}(?!\d))/g, '') : distance.trim();
    const s = grouped.replace(',', '.').replace(/[¼½¾]/g, (c) => ` ${FRACTIONS[c]}`).trim();
    const m = /^(\d+(?:\.\d+)?)?\s*(?:(\d+)\/(\d+))?$/.exec(s);
    if (!m || (m[1] === undefined && m[2] === undefined)) {
        return null;
    }
    let v = m[1] === undefined ? 0 : parseFloat(m[1]);
    if (m[2] !== undefined) {
        const d = parseInt(m[3], 10);
        if (!d) {
            return null;
        }
        v += parseInt(m[2], 10) / d;
    }
    return v * k;
}

/* The texts of a description (street names, signposts), at most a few. */
function texts(v, out) {
    const o = out || [];
    if (!v || o.length > 6) {
        return o;
    }
    if (Array.isArray(v)) {
        v.forEach((x) => texts(x, o));
    } else if (typeof v === 'object') {
        if (typeof v.text === 'string' && v.text.trim()) {
            o.push(v.text.trim());
        }
        Object.keys(v).forEach((k) => {
            if (k !== 'text' && k !== 'iconHandle' && typeof v[k] === 'object') {
                texts(v[k], o);
            }
        });
    }
    return o;
}

/* queryContent's reply -> { kind, metres, text } of its first maneuver, or null. */
function parseManeuver(reply) {
    const views = reply && reply.viewContent;
    const list = Array.isArray(views) ? views : views ? [views] : [];
    for (const view of list) {
        for (const ev of (view && view.eventsInView) || []) {
            const kind = ev && KINDS.find((k) => ev[k] && typeof ev[k] === 'object');
            if (!kind) {
                continue;
            }
            const e = ev[kind], fd = e.formattedDistance || {};
            return {
                kind,
                metres: toMetres(fd.distance, fd.unit === undefined || fd.unit === null ? '' : String(fd.unit)),
                text: texts(e.description).join(' ').slice(0, 80)
            };
        }
    }
    return null;
}

class TurnGate {
    constructor(options) {
        const o = options || {};
        this.showM = o.showM === undefined ? 400 : o.showM;
        this.branchShowM = o.branchShowM === undefined ? 1000 : o.branchShowM;
        this.extraHideM = o.extraHideM === undefined ? 400 : o.extraHideM;
        this.holdMs = o.holdMs === undefined ? 10000 : o.holdMs;
        this.blankMaxMs = o.blankMaxMs === undefined ? 60000 : o.blankMaxMs;
        this.open = false;
        this._leavingAt = null;
        this._blankSince = null;
    }

    /* maneuver: parseManeuver's, or null. Returns whether the map shows. */
    update(maneuver, nowMs) {
        const turn = !!maneuver && TRIGGER_KINDS.indexOf(maneuver.kind) >= 0;
        const showM = turn && maneuver.kind === 'branch' ? this.branchShowM : this.showM;
        if (!this.open) {
            this.open = turn && maneuver.metres !== null && maneuver.metres <= showM;
            this._leavingAt = this._blankSince = null;
            return this.open;
        }
        const blank = turn && maneuver.metres === null;
        if (!blank) {
            this._blankSince = null;
        } else if (this._blankSince === null) {
            this._blankSince = nowMs;
        }
        /* a blank distance that stays (a destination kept after arrival) holds it open no longer than blankMaxMs */
        if ((blank && nowMs - this._blankSince < this.blankMaxMs) || (turn && !blank && maneuver.metres <= showM + this.extraHideM)) {
            this._leavingAt = null;
            return true;
        }
        if (this._leavingAt === null) {
            this._leavingAt = nowMs;
        }
        if (nowMs - this._leavingAt >= this.holdMs) {
            this.open = false;
            this._leavingAt = this._blankSince = null;
        }
        return this.open;
    }
}

module.exports = { parseManeuver, toMetres, TurnGate, TRIGGER_KINDS, KINDS };
