// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 lukher-dev
'use strict';
/**
 * render_sender - the one way the app's background parts (the driver
 * profile's map settings, the turn watch) reach the render's control
 * socket, which takes one owner at a time and answers a second connection
 * "busy" (waze-on-cluster control.h).
 *
 * send(msgs) writes on the page's own link while it is up; while the page
 * has its link open but not up yet, the messages wait for it (a second
 * connection now would make the page's be refused); with the page closed,
 * one short connection of its own carries every waiting message and ends
 * holdMs later. Messages sent after that end wait for the next one. A newer
 * {"cmd":"turn"} replaces one still waiting. Without a render (its socket
 * absent or refusing) the waiting messages are dropped: the render reads
 * the saved settings when it starts, and the turn watch sends again.
 */
const net = require('net');

const HOLD_MS = 800;
const MAX_WAITING = 64;

class RenderSender {
    /* options: path (the render's socket), link (the page's LineSocket, or
     * null), holdMs, log ({info, warn}). */
    constructor(options) {
        const o = options || {};
        this._path = o.path;
        this._link = o.link || null;
        this._holdMs = o.holdMs || HOLD_MS;
        this._log = o.log || { info() {}, warn() {} };
        this._waiting = [];
        this._short = null;
        if (this._link) {
            this._link.on('connect', () => this._flush());
            this._link.on('closed', () => this._flush());
        }
    }

    send(msgs) {
        (Array.isArray(msgs) ? msgs : [msgs]).forEach((m) => {
            if (m && m.cmd === 'turn') {
                this._waiting = this._waiting.filter((w) => w.cmd !== 'turn');
            }
            this._waiting.push(m);
        });
        if (this._waiting.length > MAX_WAITING) {
            this._waiting = this._waiting.slice(-MAX_WAITING);
        }
        this._flush();
    }

    _flush() {
        if (!this._waiting.length) {
            return;
        }
        const link = this._link;
        if (link && link.connected()) {
            const msgs = this._waiting;
            this._waiting = [];
            msgs.forEach((m) => link.send(m));
            return;
        }
        if (link && link.isOpen()) {
            return;   /* the page's link is coming up: it takes them */
        }
        if (!this._short) {
            this._openShort();
        } else if (this._short.up && !this._short.ending) {
            this._writeShort();
        }
    }

    _openShort() {
        const sock = net.connect(this._path);
        const short = { sock, up: false, ending: false };
        this._short = short;
        const hard = setTimeout(() => sock.destroy(), this._holdMs * 3);
        sock.on('connect', () => {
            short.up = true;
            this._writeShort();
            setTimeout(() => {
                short.ending = true;
                sock.end();
            }, this._holdMs);
        });
        sock.on('error', () => {});
        sock.on('close', () => {
            clearTimeout(hard);
            if (this._short === short) {
                this._short = null;
            }
            if (!short.up) {
                if (this._waiting.length) {
                    this._log.info(`render not reachable: ${this._waiting.length} message(s) dropped`);
                }
                this._waiting = [];
                return;
            }
            this._flush();
        });
    }

    _writeShort() {
        const msgs = this._waiting;
        this._waiting = [];
        msgs.forEach((m) => this._short.sock.write(JSON.stringify(m) + '\n'));
    }
}

module.exports = { RenderSender, HOLD_MS };
