/* Netplay protocol v2, the client side: the lobby/session connection to a
 * room (netplay/session.mjs), with no game in it.  web/net_game.js drives
 * the engine from it; web/lobby.js draws it.
 *
 * UMD like the other netplay scripts: window.KatamNet in a page, require()
 * under node (tools/netplay_v2_test.mjs).  Node 22's global WebSocket and
 * fetch are what it uses there.
 *
 *   var net = KatamNet.connect({ base: 'https://relay', code: 'BCDF',
 *                                hello: {...}, log });
 *   net.on('roster', fn) ... net.sit(), net.start(), net.sendInput(f, keys)
 *
 * Wire formats are netplay/wire.mjs; the FRAMES decoder below must stay in
 * step with it (netplay/test/client.test.mjs runs both).
 */
(function (root, factory) {
    'use strict';
    if (typeof module === 'object' && module.exports)
        module.exports = factory();
    else
        root.KatamNet = factory();
}(typeof self !== 'undefined' ? self : this, function () {
    'use strict';

    var PROTO_VERSION = 2;
    var T_INPUT = 0x20, T_FRAMES = 0x21, T_ARCHIVE = 0x22, T_CHUNK = 0x30;
    var CHUNK_BYTES = 16 * 1024;

    /* The deployed v2 server.  ?relay= overrides (web/lobby.js). */
    var DEFAULT_BASE = 'https://katam-netplay.sh1ftmaker.workers.dev';

    /* --- room codes (netplay/codes.mjs, same rules) ------------------------ */
    var ALPHABET = 'BCDFGHJKMNPQRSTVWXZ';
    var LOOKALIKE = { '2': 'Z', '5': 'S', '6': 'G', '8': 'B', 'U': 'V' };
    var BLOCK = ['FVCK', 'FVK', 'CVNT', 'KKK', 'NGGR', 'NGR', 'FGGT', 'FGT', 'SHT',
                 'PSSY', 'DCK', 'TWT'];

    function normalizeCode(input) {
        if (typeof input !== 'string') return null;
        var s = input.trim();
        var m = /[?&#]room=([^&#\s]+)/i.exec(s);
        if (m) s = decodeURIComponent(m[1]);
        s = s.toUpperCase().replace(/[\s\-_.]/g, '');
        var out = '';
        for (var i = 0; i < s.length; i++) out += LOOKALIKE[s[i]] || s[i];
        if (out.length !== 4) return null;
        for (i = 0; i < 4; i++) if (ALPHABET.indexOf(out[i]) < 0) return null;
        for (i = 0; i < BLOCK.length; i++) if (out.indexOf(BLOCK[i]) >= 0) return null;
        return out;
    }

    function explainCode(input) {
        if (normalizeCode(input)) return null;
        var s = String(input || '').toUpperCase().replace(/[\s\-_.]/g, '');
        if (!s) return 'Enter the 4-letter code';
        if (s.length < 4) return (4 - s.length) + ' more letter' + (4 - s.length > 1 ? 's' : '');
        if (s.length > 4) return 'Codes are 4 letters';
        var bad = [];
        for (var i = 0; i < s.length; i++) {
            var c = LOOKALIKE[s[i]] || s[i];
            if (ALPHABET.indexOf(c) < 0 && bad.indexOf(s[i]) < 0) bad.push(s[i]);
        }
        if (bad.length) return 'Codes never contain ' + bad.join(' ') + ' (no vowels, no digits)';
        return 'Not a valid code';
    }

    /* --- binary --------------------------------------------------------------- */

    function encodeInput(first, keys) {
        var n = keys.length;
        var buf = new Uint8Array(6 + 2 * n);
        var dv = new DataView(buf.buffer);
        buf[0] = T_INPUT;
        dv.setUint32(1, first >>> 0, true);
        buf[5] = n;
        for (var i = 0; i < n; i++) dv.setUint16(6 + 2 * i, keys[i] & 0x3FF, true);
        return buf;
    }

    function FrameDecoder() { this.base = null; this.seats = 0; }
    FrameDecoder.prototype.decode = function (bytes) {
        if (bytes.length < 14 || (bytes[0] !== T_FRAMES && bytes[0] !== T_ARCHIVE)) return null;
        var dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        var key = bytes[1] & 1;
        var seats = dv.getUint16(2, true);
        var first = dv.getUint32(4, true);
        var n = dv.getUint16(8, true);
        var wm = dv.getUint32(10, true);
        if (key || !this.base || this.seats !== seats) {
            if (!key && this.base) return null;
            this.base = new Uint16Array(seats);
            this.seats = seats;
        }
        var mb = (seats + 7) >> 3;
        var frames = [];
        var o = 14, prev = this.base;
        for (var i = 0; i < n; i++) {
            if (o + mb > bytes.length) return null;
            var cur = prev.slice();
            var m = o;
            o += mb;
            for (var s = 0; s < seats; s++)
                if (bytes[m + (s >> 3)] & (1 << (s & 7))) {
                    cur[s] = dv.getUint16(o, true);
                    o += 2;
                }
            frames.push(cur);
            prev = cur;
        }
        if (o !== bytes.length) return null;
        this.base = prev;
        return { type: bytes[0], firstFrame: first, watermark: wm, frames: frames };
    };

    function encodeChunk(id, index, count, payload) {
        var buf = new Uint8Array(9 + payload.length);
        var dv = new DataView(buf.buffer);
        buf[0] = T_CHUNK;
        dv.setUint32(1, id >>> 0, true);
        dv.setUint16(5, index, true);
        dv.setUint16(7, count, true);
        buf.set(payload, 9);
        return buf;
    }

    function randomHex(n) {
        var b = new Uint8Array(n);
        (globalThis.crypto || require('crypto').webcrypto).getRandomValues(b);
        var s = '';
        for (var i = 0; i < n; i++) s += (b[i] < 16 ? '0' : '') + b[i].toString(16);
        return s;
    }

    /* --- HTTP ------------------------------------------------------------------ */

    function api(base, method, path, body) {
        return fetch(base.replace(/\/+$/, '') + path, {
            method: method,
            headers: body ? { 'Content-Type': 'application/json' } : undefined,
            body: body ? JSON.stringify(body) : undefined,
        }).then(function (r) {
            return r.json().catch(function () { return {}; }).then(function (j) {
                j.status = r.status;
                return j;
            });
        }, function (e) {
            return { status: 0, error: 'network', msg: 'Cannot reach the game server (' + e.message + ').' };
        });
    }

    function createRoom(base) { return api(base, 'POST', '/v2/rooms'); }
    function lookupRoom(base, code) { return api(base, 'GET', '/v2/rooms/' + encodeURIComponent(code)); }
    function resumeRoom(base, token) { return api(base, 'POST', '/v2/resume', { token: token }); }

    /* What a player should do about a refusal -- the lobby shows this. */
    function explainError(e) {
        if (!e) return '';
        if (e.code === 'version') {
            if (e.field === 'sim' || e.field === 'proto')
                return 'This game is running a different build of the port (room ' +
                    String(e.room).slice(0, 12) + ', you ' + String(e.yours).slice(0, 12) +
                    '). Reload this page to get the newest build. If it still differs, ' +
                    'ask the host to reload too -- everyone must run the same version.';
            if (e.field === 'rom')
                return 'Your ROM is a different dump from the host\'s. Everyone needs the ' +
                    'same Kirby & The Amazing Mirror ROM (room SHA-1 ' + String(e.room).slice(0, 8) +
                    '..., yours ' + String(e.yours).slice(0, 8) + '...).';
            if (e.field === 'content')
                return 'The host is playing with different mods or settings. Match their ' +
                    'mod list (and randomizer settings), then join again.';
            if (e.field === 'seats')
                return 'This build cannot hold ' + e.room + ' players; reload for the newest build.';
        }
        if (e.code === 'locked') return 'The host has locked this game. Ask them to unlock it.';
        if (e.code === 'kicked') return 'The host removed you from this game.';
        if (e.code === 'denied') return 'The host declined your request to join.';
        if (e.code === 'full') return 'This game is full.';
        if (e.code === 'rate') return e.msg || 'Too many attempts -- wait a minute and try again.';
        if (e.code === 'replaced') return 'You joined this game from another tab.';
        if (e.code === 'unknown' || e.error === 'unknown')
            return e.msg || 'No game with that code.';
        return e.msg || ('Disconnected (' + (e.code || e.error || 'error') + ')');
    }

    /* --- the connection ------------------------------------------------------- */

    /* opts: { base, code, hello: {sim, rom, content, name, platform, view,
     *         caps, want, create}, identity?, log?, WebSocket? } */
    function connect(opts) {
        var log = opts.log || function () {};
        var WS = opts.WebSocket || (typeof WebSocket !== 'undefined' ? WebSocket : null);
        var handlers = {};
        var sock = null;
        var dec = new FrameDecoder();
        var attempts = 0;
        var closed = false;
        var fatal = null;
        var pingTimer = null;
        var transfers = {};          /* id -> {count, got, parts, meta} */
        var self = {
            code: opts.code,
            identity: opts.identity || randomHex(16),
            state: 'connecting',     /* connecting | open | lost | closed */
            mid: -1, role: null, seat: -1, phase: null, params: null, host: -1,
            roster: null, rtt: null, frontier: 0, watermark: 0, stats: null,
            bytesUp: 0, bytesDown: 0, reconnects: 0, lastError: null,
        };

        function emit(name) {
            var args = Array.prototype.slice.call(arguments, 1);
            var hs = handlers[name] || [];
            for (var i = 0; i < hs.length; i++) {
                try { hs[i].apply(null, args); } catch (e) { log('[net] handler ' + name + ': ' + e.stack); }
            }
        }

        function url() {
            return opts.base.replace(/^http/, 'ws').replace(/\/+$/, '') +
                '/v2/rooms/' + encodeURIComponent(opts.code) + '/ws';
        }

        function tx(data) {
            if (!sock || sock.readyState !== 1) return false;
            self.bytesUp += data.length || data.byteLength || 0;
            sock.send(data);
            return true;
        }

        function json(o) { return tx(JSON.stringify(o)); }

        function open() {
            self.state = 'connecting';
            emit('state', self.state);
            sock = new WS(url());
            sock.binaryType = 'arraybuffer';
            sock.onopen = function () {
                var h = {};
                for (var k in opts.hello) h[k] = opts.hello[k];
                h.t = 'hello';
                h.proto = PROTO_VERSION;
                h.identity = self.identity;
                if (attempts) delete h.create;
                json(h);
                pingTimer = setInterval(ping, 2000);
                ping();
            };
            sock.onmessage = onMessage;
            sock.onclose = function (ev) {
                clearInterval(pingTimer);
                if (closed || fatal) {
                    self.state = 'closed';
                    emit('state', self.state, fatal);
                    return;
                }
                self.state = 'lost';
                emit('state', self.state);
                var delay = Math.min(5000, 300 << Math.min(attempts, 4));
                attempts++;
                self.reconnects++;
                log('[net] connection lost (' + (ev && ev.code) + ') -- retrying in ' + delay + ' ms');
                setTimeout(function () { if (!closed) open(); }, delay);
            };
            sock.onerror = function () { /* onclose follows */ };
        }

        function ping() { json({ t: 'ping', c: Date.now(), rtt: self.rtt }); }

        function onMessage(e) {
            if (typeof e.data === 'string') {
                self.bytesDown += e.data.length;
                var m;
                try { m = JSON.parse(e.data); } catch (err) { return; }
                switch (m.t) {
                case 'welcome':
                    var again = self.mid >= 0;
                    self.mid = m.mid; self.role = m.role; self.seat = m.seat;
                    self.phase = m.phase; self.params = m.params; self.host = m.host;
                    self.frontier = m.frontier;
                    attempts = 0;
                    self.state = 'open';
                    dec = new FrameDecoder();
                    emit('state', self.state);
                    emit('welcome', m, again);
                    break;
                case 'roster':
                    self.roster = m;
                    self.phase = m.phase;
                    self.host = m.host;
                    for (var i = 0; i < m.members.length; i++)
                        if (m.members[i].mid === self.mid) {
                            self.seat = m.members[i].seat;
                            self.role = m.members[i].role;
                        }
                    emit('roster', m);
                    break;
                case 'pong':
                    self.rtt = Date.now() - m.c;
                    break;
                case 'start':
                    self.phase = 'playing';
                    self.params = m.params;
                    dec = new FrameDecoder();
                    emit('start', m);
                    break;
                case 'archive_begin':
                    dec = new FrameDecoder();
                    emit('archive_begin', m);
                    break;
                case 'archive_end':
                    emit('archive_end', m);
                    break;
                case 'ev':
                    emit('event', m);
                    break;
                case 'stats':
                    self.stats = m;
                    emit('stats', m);
                    break;
                case 'error':
                    fatal = m;
                    self.lastError = m;
                    emit('error', m);
                    break;
                case 'snap':
                    transfers[m.id] = { meta: m, count: m.count, got: 0, parts: new Array(m.count) };
                    break;
                case 'sync_cancel':
                    delete transfers[m.id];
                    emit('sync_cancel', m);
                    break;
                default:
                    emit(m.t, m);     /* notice, desync, join_req, sync_req, snap_req, signal */
                }
                return;
            }
            var b = new Uint8Array(e.data);
            self.bytesDown += b.length;
            if (b[0] === T_CHUNK) {
                var dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
                var id = dv.getUint32(1, true), idx = dv.getUint16(5, true);
                var t = transfers[id];
                if (!t || idx >= t.count || t.parts[idx]) return;
                t.parts[idx] = b.slice(9);
                if (++t.got === t.count) {
                    delete transfers[id];
                    var size = 0, k;
                    for (k = 0; k < t.count; k++) size += t.parts[k].length;
                    var out = new Uint8Array(size), o = 0;
                    for (k = 0; k < t.count; k++) { out.set(t.parts[k], o); o += t.parts[k].length; }
                    emit('snapshot', t.meta, out);
                }
                return;
            }
            var r = dec.decode(b);
            if (!r) {
                log('[net] undecodable frames message -- asking for the archive again');
                dec = new FrameDecoder();
                return;
            }
            self.frontier = r.firstFrame + r.frames.length;
            self.watermark = r.watermark;
            emit(r.type === T_ARCHIVE ? 'archive' : 'frames', r);
        }

        open();

        self.on = function (name, fn) { (handlers[name] = handlers[name] || []).push(fn); return self; };
        self.send = json;
        self.sit = function (seat) { json({ t: 'sit', seat: seat }); };
        self.stand = function () { json({ t: 'stand' }); };
        self.ready = function (on) { json({ t: 'ready', ready: !!on }); };
        self.rename = function (name) { json({ t: 'name', name: name }); };
        self.start = function () { json({ t: 'start' }); };
        self.lock = function (on) { json({ t: 'lock', on: !!on }); };
        self.approval = function (on) { json({ t: 'approval', on: !!on }); };
        self.approve = function (mid, ok) { json({ t: ok === false ? 'deny' : 'approve', mid: mid }); };
        self.kick = function (mid) { json({ t: 'kick', mid: mid }); };
        self.settings = function (p) { json({ t: 'settings', params: p }); };
        self.view = function (w, h) { json({ t: 'view', w: w, h: h }); };
        self.requestArchive = function () { json({ t: 'archive' }); };
        self.sendInput = function (first, keys) { return tx(encodeInput(first, keys)); };
        self.sendHash = function (f, h) { json({ t: 'hash', f: f, h: h }); };
        self.syncMe = function (why) { json({ t: 'syncme', why: why }); };
        self.sendSnapshot = function (id, f, payload, h, kind) {
            var count = Math.max(1, Math.ceil(payload.length / CHUNK_BYTES));
            json({ t: 'snap', id: id, f: f, size: payload.length, count: count, h: h, kind: kind });
            for (var i = 0; i < count; i++)
                tx(encodeChunk(id, i, count, payload.subarray(i * CHUNK_BYTES, (i + 1) * CHUNK_BYTES)));
        };
        self.leave = function () {
            closed = true;
            json({ t: 'leave' });
            try { sock.close(); } catch (e) { /* gone */ }
        };
        self.close = function () {
            closed = true;
            clearInterval(pingTimer);
            try { sock.close(); } catch (e) { /* gone */ }
        };
        return self;
    }

    return {
        PROTO_VERSION: PROTO_VERSION, DEFAULT_BASE: DEFAULT_BASE, ALPHABET: ALPHABET,
        normalizeCode: normalizeCode, explainCode: explainCode, explainError: explainError,
        createRoom: createRoom, lookupRoom: lookupRoom, resumeRoom: resumeRoom,
        connect: connect, FrameDecoder: FrameDecoder, encodeInput: encodeInput,
        randomHex: randomHex,
    };
}));
