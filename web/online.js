/* Online play, up to four players on up to four devices -- or eight on
 * eight in an 8-player game (EXPERIMENTAL) -- the page's half.
 *
 * The game's own front end has the screens (Start Game > MULTIPLAYER >
 * ONLINE > HOST GAME / JOIN GAME: platform/online_ui.c, drawn by
 * platform/menu_poc.c).  This file does what they ask for:
 *
 *   rooms      a relay room (web/net_session.js, netplay/session.mjs) gives a
 *              4-letter code, gates on build and ROM at hello, and relays
 *              `signal` messages between its members: the host and up to
 *              three guests, a star (guests talk only to the host).
 *   transport  WebRTC data channels between the two browsers -- one
 *              unreliable + unordered for inputs and sealed frames, one
 *              reliable + ordered for images and control -- signalled over
 *              the room.  STUN only (stun.cloudflare.com), no TURN: if the
 *              channels are not open within 5 s, or fail later, the same
 *              traffic goes over the room's `signal` path instead (images in
 *              chunks well under the relay's 64 KiB message limit).
 *   session    the proven logic (web/ln_session.js, the file the node rig
 *              tests): the host is authoritative and never rewinds, the
 *              guest runs ahead and rewinds (platform/lmpnet.c).  It drives
 *              the frame loop through Module.portLn (platform/web/host_web.c
 *              PortWebAwaitFrame): 0..N frames per display tick on the guest.
 *
 * Slots.  Kirby 1 is the host's.  A guest that connects gets the next free
 * Kirby for its first player; more people on any device ask through
 * katamOnline.requestSlot(seat, on) -- or, the same thing, the device
 * layer's PortLmpRequestHuman(seat, on) (platform/localmp.c, "the device
 * layer's door"), which the input-device core already calls.  A seat is a
 * local player on this device: 0 = this device's own keys
 * (Module._PortSetKeys), 1..3 = the pads given to
 * Module._PortLmpSetHostKeys(seat, mask).  The host decides which Kirby
 * (platform/lmpnet.c PortLnSeatRequest); every device applies it on the
 * same sealed frame.  A guest leaving, or its link going quiet for 6 s,
 * hands its Kirbys to the CPU; back with the same code (the same tab, or a
 * reload of it), it gets a Kirby again -- the one it had, if still free.
 *
 * Modules.  Four-player games run on the ordinary module (katam.wasm);
 * 8-player games on the 16-Kirby one (katam16.wasm: web/shell.html picks it
 * for ?mod=16 or SETTINGS > EXPERIMENTAL > CPU KIRBYS above 3).  A host on
 * the 16-Kirby module hosts an 8-player game; ONLINE > HOST 8 PLAYERS on
 * the ordinary one reloads the page into it (and 4 PLAYER GAMES back).
 * Every peer of a game runs the same module (the sim id names it: KATAM_SIM
 * or KATAM_SIM16).  A joiner on the other module -- told so by the invite
 * link (?mod=16), the public list's `mod`, or the room's version gate (the
 * room's sim id is the other module's) -- reloads into it with ?join=CODE,
 * once.  The wire carries every slot's keys (web/ln_session.js) and, in an
 * 8-player game, each sealed record about four times (bounded redundancy).
 *
 * Invite links: ?join=CODE joins straight after the ROM is loaded.
 * Test knobs: ?netlag=ms (one way, added to everything this page sends),
 * ?netjitter=ms (uniform 0..ms on datagrams), ?netloss=% (datagrams),
 * ?netrelay=1 (never try WebRTC), ?netserver=<base> (another room server).
 *
 * window.katamOnline.stats() is the telemetry; the developer menu (?dev=1, F1): Game page
 * shows the same as text (Module.portUiQuery('online')). */
(function () {
    'use strict';
    var Net = window.KatamNet, LS = window.KatamLnSession;
    if (!Net || !LS) return;
    var KB = window.KatamBrowse || null;       /* server browser: web/online_browse.js */
    var M = window.Module;
    var q = new URLSearchParams(location.search);
    /* ?netserver=<base>: another room server (netplay/dev-relay.mjs, a
     * test deployment).  Not ?relay=: that one also switches on the parked
     * link-cable hookup in web/shell.html, which would wire the cable. */
    var BASE = (q.get('netserver') || Net.DEFAULT_BASE).replace(/\/+$/, '').replace(/\/parties\/.*$/, '');
    var KNOB = {
        lag: Math.max(0, +q.get('netlag') || 0),
        jitter: Math.max(0, +q.get('netjitter') || 0),
        loss: Math.min(1, Math.max(0, (+q.get('netloss') || 0) / 100)),
        relay: q.get('netrelay') === '1',
    };
    var PERIOD = LS.PERIOD;
    var KF_EVERY = 15, KF_COUNT = 24;          /* as the rig: 360 frames of history, ~12 MiB */
    var P2P_WAIT_MS = 5000, DEAD_MS = 6000, PING_MS = 1000;
    var ICE = [{ urls: 'stun:stun.cloudflare.com:3478' }];
    var RELAY_CHUNK = 24 * 1024, DC_CHUNK = 16 * 1024;
    var CONTENT = 'lmpnet1';

    /* platform/port/online_ui.h */
    var UI = { J_REMOVED: 18, IDLE: 0, H_CREATING: 1, H_WAITING: 2, H_JOINING: 3, H_READY: 4, H_NOSERVER: 5,
               J_CONNECTING: 6, J_WAITING: 7, J_STARTING: 8, J_NOGAME: 9, J_FULL: 10, J_BUILD: 11,
               J_ROM: 12, J_FAILED: 13, J_HOSTLEFT: 14 };
    var EV = { HOST_OPEN: 1, HOST_CLOSE: 2, HOST_START: 3, COPY_LINK: 4, JOIN: 5, JOIN_CANCEL: 6, OTHER_MODULE: 7 };
    var NOTE = { NONE: 0, COPIED: 1, NOCOPY: 2 };

    function log(t) {
        t = '[online] ' + t;
        console.log(t);
        if (M.portDiag) { try { M.portDiag(t, 0); } catch (e) { /* console only */ } }
    }
    function nowMs() { return performance.now(); }
    function up() { return !!(M.portRuntimeUp && M._PortOuiSet); }

    var S = {
        role: null,            /* 'host' | 'guest' */
        phase: 'idle',         /* idle | hosting | joining | waiting | playing | hostleft | failed */
        code: null, net: null,
        peer: null,            /* guest: the host {mid, sid, link} */
        peers: {},             /* host: by room member id, {mid, sid, link, ready, at, dev, tok} */
        banned: {},            /* host: tab tokens of devices it removed (this room) */
        tokDev: {},            /* host: a returning device's id, by its tab token */
        session: false,        /* the engine session is on (PortLnStart done) */
        startWanted: false,
        eng: null, hub: null, gs: null, owners: noOwners(),
        inbox: [], accept: false,
        t0: 0, ticks: 0, reanchors: 0,
        startedAt: 0, lastView: -1, invite: null,
        uiState: 0, banner: null,
        stats: { relayIn: 0, relayOut: 0, p2pIn: 0, p2pOut: 0, bytesIn: 0, bytesOut: 0, dropped: 0,
                 joins: 0, guestDrops: 0, imagesSent: 0, imagesIn: 0, slotChanges: 0, refusedFull: 0 },
    };
    /* The 16-Kirby module (web/shell.html; it may have fallen back to the
     * ordinary one, so asked each time): its games hold 8 people */
    function wide() { return !!window.KATAM_KIRBYS16 && !(up() && M._PortLnSlots && M._PortLnSlots() <= 4); }
    function modName() { return wide() ? '16' : '4'; }
    function seats() { return wide() ? 8 : 4; }
    function maxGuests() {
        var d = up() && M._PortLnMaxDevices ? M._PortLnMaxDevices() : 4;
        return Math.min(seats(), d) - 1;
    }
    /* slots the page watches: the seats (people sit only in slots below them) */
    function nSlots() { return Math.min(seats(), up() && M._PortLnSlots ? M._PortLnSlots() : 4); }
    function noOwners() { var o = [0]; for (var i = 1; i < 8; i++) o.push(-1); return o; }
    var phone = (function () { try { return matchMedia('(pointer: coarse)').matches; } catch (e) { return false; } }());
    /* this tab's token: a reload of it is the same device to the host */
    var TOK = (function () {
        try {
            var t = sessionStorage.getItem('katamLnTok');
            if (!t) { t = Math.random().toString(36).slice(2, 12); sessionStorage.setItem('katamLnTok', t); }
            return t;
        } catch (e) { return Math.random().toString(36).slice(2, 12); }
    }());

    function ui(state, code) {
        S.uiState = state;
        if (!up()) return;
        var c = code || '';
        M._PortOuiSet(state, c.charCodeAt(0) || 0, c.charCodeAt(1) || 0, c.charCodeAt(2) || 0, c.charCodeAt(3) || 0);
    }
    var noteTimer = 0;
    function note(n) {
        if (!up()) return;
        M._PortOuiNote(n);
        clearTimeout(noteTimer);
        if (n) noteTimer = setTimeout(function () { M._PortOuiNote(0); }, 2500);
    }

    /* --- a banner over the picture (invite joins, in-game events) ---------- */
    function banner(text, ms) {
        var el = document.getElementById('lnbanner');
        if (!el) {
            el = document.createElement('div');
            el.id = 'lnbanner';
            el.style.cssText = 'position:fixed;left:50%;bottom:12%;transform:translateX(-50%);z-index:70;' +
                'max-width:min(92vw,560px);padding:10px 16px;border-radius:12px;pointer-events:none;' +
                'background:rgba(24,16,64,.86);border:3px solid #f8d850;color:#fff;text-align:center;' +
                'font:bold 15px/1.35 ui-monospace,Menlo,Consolas,monospace;letter-spacing:.06em;' +
                'text-transform:uppercase;box-shadow:0 4px 0 rgba(0,0,0,.35);display:none';
            document.body.appendChild(el);
        }
        clearTimeout(S.bannerTimer);
        S.banner = text || null;
        if (!text) { el.style.display = 'none'; return; }
        el.textContent = text;
        el.style.display = 'block';
        if (ms) S.bannerTimer = setTimeout(function () { el.style.display = 'none'; S.banner = null; }, ms);
    }

    /* --- versions: the room's hello gate, re-checked peer to peer ------------ */
    var verP = null;
    function hex(buf) {
        var a = new Uint8Array(buf), s = '';
        for (var i = 0; i < a.length; i++) s += (a[i] < 16 ? '0' : '') + a[i].toString(16);
        return s;
    }
    function sha1(bytes) {
        if (!crypto.subtle) return Promise.resolve('nosubtle');
        return crypto.subtle.digest('SHA-1', bytes).then(hex);
    }
    function versions() {
        if (verP) return verP;
        var romSize = (M._shell && M._shell.rom && M._shell.rom.size) || 16 * 1024 * 1024;
        var rom = M.HEAPU8.slice(0x08000000, 0x08000000 + romSize);
        /* KATAM_SIM (tools/stamp_build.py) includes the wasm's own id: an
         * image holds this build's pointers, so nothing else may join */
        /* both modules' ids: this page's, and the other one's (a room on the
         * other module is recognised by it, and reloaded into) */
        var mine = simOf(wide()), other = simOf(!wide());
        verP = Promise.all([mine, sha1(rom), other]).then(function (v) { return { sim: v[0], rom: v[1], other: v[2] }; });
        return verP;
    }
    function simOf(w16) {
        if (window.KATAM_SIM) {
            if (!w16) return Promise.resolve(window.KATAM_SIM);
            return Promise.resolve(window.KATAM_SIM16 || window.KATAM_SIM + '.k16');
        }
        var file = w16 ? 'katam16.wasm' : 'katam.wasm';
        return fetch(file).then(function (r) { if (!r.ok) throw new Error(r.status); return r.arrayBuffer(); }).then(sha1)
            .then(function (h) { return 'dev.w' + h.slice(0, 12) + (w16 ? '.k16' : ''); }, function () { return w16 ? 'dev.k16' : 'dev'; });
    }

    /* --- the other module: reload into it, once per game code ----------------- */
    function hop(mod, code, why) {
        var key = 'katamModHop', t = Date.now(), last = null;
        try { last = JSON.parse(sessionStorage.getItem(key) || 'null'); } catch (e) { last = null; }
        if (last && last.code === code && t - last.t < 60000) {
            log('module: already reloaded for ' + code + ' a moment ago -- not again');
            return false;
        }
        try { sessionStorage.setItem(key, JSON.stringify({ code: code, mod: mod, t: t })); } catch (e) { /* once is enough */ }
        log('module: game ' + (code || '') + ' runs on the ' + (mod === '16' ? '8-player' : 'ordinary') + ' module (' + why + ') -- reloading into it');
        S.stats.moduleHops = (S.stats.moduleHops || 0) + 1;
        banner(mod === '16' ? 'That is an 8-player game: loading the 8-player module...' : 'That is a 4-player game: loading the ordinary module...', 0);
        var u = new URL(location.href);
        u.searchParams.delete('kirbys');
        u.searchParams.set('mod', mod);
        if (code) u.searchParams.set('join', code); else u.searchParams.delete('join');
        if (S.net || S.peer) { try { reset(); } catch (e) { /* going anyway */ } }
        setTimeout(function () { location.href = u.toString(); }, 50);
        return true;
    }
    function hello(v) {
        return { sim: v.sim, rom: v.rom, content: CONTENT, name: 'Player', platform: 'web',
                 caps: { seats: 4 } };
    }

    /* --- image codec: raw deflate where the browser has it -------------------- */
    var CODEC = {
        deflate: function (bytes) {
            if (typeof CompressionStream === 'undefined') {
                var o = new Uint8Array(bytes.length + 1); o[0] = 0; o.set(bytes, 1);
                return Promise.resolve(o);
            }
            var s = new Blob([bytes]).stream().pipeThrough(new CompressionStream('deflate-raw'));
            return new Response(s).arrayBuffer().then(function (b) {
                var o = new Uint8Array(b.byteLength + 1); o[0] = 1; o.set(new Uint8Array(b), 1);
                return o;
            });
        },
        inflate: function (bytes) {
            if (bytes[0] === 0) return Promise.resolve(bytes.slice(1));
            var s = new Blob([bytes.subarray(1)]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
            return new Response(s).arrayBuffer().then(function (b) { return new Uint8Array(b); });
        },
    };

    function b64(u8) {
        var s = '';
        for (var i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
        return btoa(s);
    }
    function unb64(s) {
        var bin = atob(s), u8 = new Uint8Array(bin.length);
        for (var i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
        return u8;
    }
    function rid() { return Math.random().toString(36).slice(2, 10); }

    /* --- the link to the other device ------------------------------------------
     * Sends over the data channels when they are open, else over the room.
     * Receives from both.  Test knobs delay / drop what this side sends. */
    function Link(net, peerMid, sid, isGuest, forceRelay) {
        var self = this;
        this.net = net; this.mid = peerMid; this.sid = sid; this.guest = isGuest;
        this.pc = null; this.dcU = null; this.dcR = null;
        this.p2p = false; this.decided = false; this.closed = false;
        this.lastRecv = nowMs(); this.rtt = null; this.relNext = 0; this.bIn = 0; this.bOut = 0;
        this.onDatagram = null; this.onReliable = null; this.onDecided = null;
        this.img = { dc: null, relay: null };
        this.iceQ = []; this.haveRemote = false;
        this.pingTimer = setInterval(function () { self.ping(); }, PING_MS);
        /* decided a turn later: the caller sets onDecided after this returns */
        if (KNOB.relay || forceRelay) { setTimeout(function () { self.decide('relay (forced)'); }, 0); return; }
        try { this.startP2p(); } catch (e) {
            log('WebRTC unavailable: ' + e.message);
            setTimeout(function () { self.decide('relay (no WebRTC)'); }, 0);
            return;
        }
        this.decideTimer = setTimeout(function () { if (!self.p2p) self.decide('relay (no direct link in 5 s)'); }, P2P_WAIT_MS);
    }
    Link.prototype.path = function () { return this.p2p ? 'p2p' : 'relay'; };
    Link.prototype.decide = function (why) {
        if (this.decided) return;
        this.decided = true;
        clearTimeout(this.decideTimer);
        log('link: ' + why);
        if (this.onDecided) this.onDecided();
    };
    Link.prototype.sig = function (d) {
        d.sid = this.sid;
        S.stats.relayOut++;
        this.net.send({ t: 'signal', to: this.mid, data: d });
    };
    Link.prototype.startP2p = function () {
        var self = this;
        var pc = this.pc = new RTCPeerConnection({ iceServers: ICE });
        this.dcU = pc.createDataChannel('u', { negotiated: true, id: 0, ordered: false, maxRetransmits: 0 });
        this.dcR = pc.createDataChannel('r', { negotiated: true, id: 1, ordered: true });
        [this.dcU, this.dcR].forEach(function (dc) {
            dc.binaryType = 'arraybuffer';
            dc.onopen = function () { self.checkOpen(); };
            dc.onclose = function () { self.lost('data channel closed'); };
            dc.onerror = function () { /* onclose follows */ };
        });
        this.dcU.onmessage = function (e) { self.recvDatagram(new Uint8Array(e.data), 'p2p'); };
        this.dcR.onmessage = function (e) { self.recvReliable(e.data, 'p2p'); };
        pc.onicecandidate = function (e) { if (e.candidate) self.sig({ k: 'ice', c: e.candidate.toJSON() }); };
        pc.onconnectionstatechange = function () {
            var st = pc.connectionState;
            if (st === 'failed' || st === 'closed') self.lost('peer connection ' + st);
            else if (st === 'disconnected') {
                clearTimeout(self.discTimer);
                self.discTimer = setTimeout(function () {
                    if (pc.connectionState === 'disconnected') self.lost('peer connection disconnected');
                }, 2500);
            }
        };
        if (this.guest) {
            pc.createOffer().then(function (o) { return pc.setLocalDescription(o); })
                .then(function () { self.sig({ k: 'sdp', d: { type: pc.localDescription.type, sdp: pc.localDescription.sdp } }); })
                .catch(function (e) { log('offer failed: ' + e.message); self.decide('relay (offer failed)'); });
        }
    };
    Link.prototype.checkOpen = function () {
        if (this.closed || !this.dcU || !this.dcR) return;
        if (this.dcU.readyState === 'open' && this.dcR.readyState === 'open' && !this.p2p) {
            this.p2p = true;
            log('link: direct (WebRTC data channels open)');
            this.decide('p2p');
        }
    };
    Link.prototype.lost = function (why) {
        if (!this.p2p && this.decided) return;
        if (this.p2p) log('link: direct path lost (' + why + ') -- the relay carries it now');
        this.p2p = false;
        this.decide('relay (' + why + ')');
    };
    Link.prototype.signal = function (d) {
        var self = this, pc = this.pc;
        this.lastRecv = nowMs();
        S.stats.relayIn++;
        switch (d.k) {
        case 'sdp':
            if (!pc) return;
            pc.setRemoteDescription(d.d).then(function () {
                self.haveRemote = true;
                self.iceQ.splice(0).forEach(function (c) { pc.addIceCandidate(c).catch(function () {}); });
                if (d.d.type === 'offer')
                    return pc.createAnswer().then(function (a) { return pc.setLocalDescription(a); }).then(function () {
                        self.sig({ k: 'sdp', d: { type: pc.localDescription.type, sdp: pc.localDescription.sdp } });
                    });
            }).catch(function (e) { log('sdp: ' + e.message); });
            return;
        case 'ice':
            if (!pc) return;
            if (!self.haveRemote) self.iceQ.push(d.c);
            else pc.addIceCandidate(d.c).catch(function () {});
            return;
        case 'd':
            this.recvDatagram(unb64(d.b), 'relay');
            return;
        case 'r':
            this.recvReliable(d.m, 'relay');
            return;
        case 'ic':
            this.recvChunk(unb64(d.b), 'relay');
            return;
        }
    };
    /* outgoing, through the test knobs */
    Link.prototype.later = function (ms, fn) { if (ms > 0) setTimeout(fn, ms); else fn(); };
    Link.prototype.datagram = function (bytes) {
        var self = this;
        if (this.closed) return;
        if (KNOB.loss && Math.random() < KNOB.loss) { S.stats.dropped++; return; }
        var d = KNOB.lag + Math.random() * KNOB.jitter;
        S.stats.bytesOut += bytes.length; this.bOut += bytes.length;
        this.later(d, function () {
            if (self.closed) return;
            if (self.p2p && self.dcU.readyState === 'open') {
                try { self.dcU.send(bytes); S.stats.p2pOut++; return; } catch (e) { /* relay */ }
            }
            self.sig({ k: 'd', b: b64(bytes) });
        });
    };
    /* reliable, in order: control objects, and images as a header + chunks */
    Link.prototype.reliable = function (msg) {
        var self = this, t = nowMs();
        if (this.closed) return;
        var at = Math.max(this.relNext, t + KNOB.lag);
        this.relNext = at;
        this.later(at - t, function () { if (!self.closed) self.sendReliableNow(msg); });
    };
    Link.prototype.sendReliableNow = function (msg) {
        var p2p = this.p2p && this.dcR.readyState === 'open';
        if (msg.t === 'image') {
            var comp = msg.comp, size = p2p ? DC_CHUNK : RELAY_CHUNK, n = Math.max(1, Math.ceil(comp.length / size));
            var hdr = { t: 'image', F: msg.F, raw: msg.raw, reason: msg.reason, len: comp.length, n: n, dev: msg.dev };
            S.stats.imagesSent++;
            S.stats.bytesOut += comp.length; this.bOut += comp.length;
            if (p2p) {
                try {
                    this.dcR.send(JSON.stringify(hdr));
                    for (var i = 0; i < n; i++) this.dcR.send(comp.subarray(i * size, (i + 1) * size));
                    S.stats.p2pOut += n + 1;
                    return;
                } catch (e) { log('image over the data channel failed (' + e.message + ') -- the relay'); }
                size = RELAY_CHUNK; n = Math.max(1, Math.ceil(comp.length / size)); hdr.n = n;
            }
            this.sig({ k: 'r', m: hdr });
            for (var j = 0; j < n; j++) this.sig({ k: 'ic', b: b64(comp.subarray(j * size, (j + 1) * size)) });
            return;
        }
        if (p2p) {
            try { this.dcR.send(JSON.stringify(msg)); S.stats.p2pOut++; return; } catch (e) { /* relay */ }
        }
        this.sig({ k: 'r', m: msg });
    };
    Link.prototype.recvDatagram = function (bytes, via) {
        this.lastRecv = nowMs();
        S.stats.bytesIn += bytes.length; this.bIn += bytes.length;
        if (via === 'p2p') S.stats.p2pIn++;
        if (this.onDatagram) this.onDatagram(bytes);
    };
    Link.prototype.recvReliable = function (data, via) {
        this.lastRecv = nowMs();
        if (via === 'p2p') S.stats.p2pIn++;
        if (typeof data !== 'string' && !(data && data.t)) { this.recvChunk(new Uint8Array(data), via); return; }
        var m = typeof data === 'string' ? JSON.parse(data) : data;
        if (m.t === 'image') {
            this.img[via] = { hdr: m, parts: [], got: 0 };
            return;
        }
        if (m.t === 'ping') { this.reliable({ t: 'pong', c: m.c }); return; }
        if (m.t === 'pong') { this.rtt = nowMs() - m.c; return; }
        if (this.onReliable) this.onReliable(m);
    };
    Link.prototype.recvChunk = function (u8, via) {
        var im = this.img[via];
        if (!im) return;
        im.parts.push(u8); im.got += u8.length;
        S.stats.bytesIn += u8.length; this.bIn += u8.length;
        if (im.parts.length < im.hdr.n) return;
        this.img[via] = null;
        var comp = new Uint8Array(im.got), o = 0;
        for (var i = 0; i < im.parts.length; i++) { comp.set(im.parts[i], o); o += im.parts[i].length; }
        S.stats.imagesIn++;
        if (this.onReliable) this.onReliable({ t: 'image', F: im.hdr.F, raw: im.hdr.raw, reason: im.hdr.reason, comp: comp, dev: im.hdr.dev });
    };
    Link.prototype.ping = function () { if (!this.closed) this.reliable({ t: 'ping', c: nowMs() }); };
    Link.prototype.dead = function () { return nowMs() - this.lastRecv > DEAD_MS; };
    Link.prototype.close = function (bye) {
        if (this.closed) return;
        if (bye) { try { this.sig({ k: 'bye' }); } catch (e) { /* gone */ } }
        this.closed = true;
        clearInterval(this.pingTimer); clearTimeout(this.decideTimer); clearTimeout(this.discTimer);
        try { if (this.pc) this.pc.close(); } catch (e) { /* gone */ }
    };

    /* --- the engine session ------------------------------------------------------ */
    function clock() { return nowMs(); }
    function engine() { return S.eng || (S.eng = LS.engineOfModule(M)); }
    function tel() { return S.session ? engine().tel() : null; }

    function txOf(link) {
        return {
            datagram: function (b) { link.datagram(b); },
            reliable: function (m) { link.reliable(m); },
        };
    }

    function dueTicks(t) {
        if (!S.t0) { S.t0 = t; S.ticks = 0; return 1; }
        var due = Math.floor((t - S.t0 + 2) / PERIOD);
        var k = due - S.ticks;
        if (k > 8) { S.ticks = due - 1; k = 1; S.reanchors++; }
        if (k < 0) k = 0;
        S.ticks += k;
        return k;
    }

    function drain() {
        var box = S.inbox;
        if (!box.length) return;
        S.inbox = [];
        for (var i = 0; i < box.length; i++) {
            var it = box[i];
            if (S.role === 'host') {
                var p = it.peer, hs = S.hub && p && S.peers[p.mid] === p ? S.hub.get(p.dev) : null;
                if (!hs) continue;
                if (it.d) hs.onDatagram(it.d); else hs.onReliable(it.m);
            } else if (S.gs) {
                if (it.img) { S.gs.onReliable({ t: 'image', raw: it.img, dev: it.dev }); continue; }
                if (it.m) { S.gs.onReliable(it.m); continue; }
                if (it.d && S.accept) S.gs.onDatagram(it.d);
            }
        }
    }

    /* This device's local seats' keys for the frame about to run: seat 0 is
     * the console's own keys, seats 1..3 whatever the page or the device
     * layer gave PortLmpSetHostKeys -- read back here, so any input path
     * that sets those works unchanged. */
    function seatKeys() {
        var k = [M._PortCurrentKeys() & 0x3FF, 0, 0, 0];
        if (M._PortLmpHostKeys) for (var s = 1; s < 4; s++) k[s] = M._PortLmpHostKeys(s) & 0x3FF;
        return k;
    }

    /* Who plays which Kirby, as the sealed frames say: a notice when it changes. */
    function slotName(s) { return 'Player ' + (s + 1); }
    function watchSlots() {
        if (!S.session || !M._PortLnSlotOwner) return;
        var self = S.role === 'host' ? 0 : M._PortLnSelf(), ns = nSlots();
        for (var s = 1; s < ns; s++) {
            var o = M._PortLnSlotOwner(s), was = S.owners[s];
            if (o === was) continue;
            S.owners[s] = o;
            S.stats.slotChanges++;
            var mine = o >= 0 && (o >> 2) === self;
            /* one notice per change: the page's device notices may have said
             * it already (a controller here leaving), and a controller here
             * joining is said here, in their style, with its label */
            var kn = window.katamNotices;
            if (o >= 0 && was < 0) {
                log('slot ' + s + ': device ' + (o >> 2) + ' seat ' + (o & 3) + ' joins');
                if (kn && kn.once && !kn.once('join:' + s)) continue;
                if (mine && (o & 3) && kn && kn.show) {
                    var kd = window.katamDevices, pl = kd && kd.listPlayers ? kd.listPlayers()[o & 3] : null;
                    var lab = pl && pl.labels && pl.labels.length ? pl.labels[0] : '';
                    kn.show(slotName(s) + ' joined', s, (lab ? lab + ' - ' : '') + 'hold Select to leave');
                } else {
                    banner(mine ? ((o & 3) ? 'Your player ' + ((o & 3) + 1) + ' is ' + slotName(s) : 'You are ' + slotName(s))
                           : slotName(s) + ' joined', 3000);
                }
            } else if (o < 0) {
                log('slot ' + s + ': to the CPU');
                if (kn && kn.once && !kn.once('leave:' + s)) continue;
                banner(slotName(s) + ' left - the CPU has that Kirby', 3500);
            } else {
                banner(slotName(s) + ' changed hands', 2500);
            }
        }
    }

    var ln = M.portLn = {
        on: false, burst: 0,
        /* after the frames of a tick: start the session if it is due, send */
        after: function () {
            if (!S.session && S.startWanted) startEngine();
            drain();
            if (!S.session) return;
            if (S.role === 'host') {
                if (S.hub) S.hub.afterTick();
            } else if (S.gs) {
                if (S.gs.joined && !S.gs.imagePending && !S.accept) {
                    /* in the game: whatever said "connecting" or "getting
                     * the game" is done (it used to stay up for good) */
                    S.accept = true;
                    for (var s = 1; s < nSlots(); s++) S.owners[s] = M._PortLnSlotOwner(s);
                    var mine = M._PortLnSeatSlot(M._PortLnSelf(), 0);
                    banner(mine > 0 ? 'You are ' + slotName(mine) : 'You are in - waiting for a free Kirby', 2500);
                }
                /* seat wait (telemetry): from the host's yes to this device's Kirby playing */
                if (S.accept && S.seatWaitFrom && M._PortLnSeatSlot(M._PortLnSelf(), 0) > 0) {
                    S.stats.seatWaitMs = Math.round(nowMs() - S.seatWaitFrom);
                    S.seatWaitFrom = 0;
                }
                S.gs.afterTick();
            }
            if (S.role === 'host' || S.accept) watchSlots();
        },
        tick: function (t) {
            drain();
            if (!S.session) return 0;
            var k = dueTicks(t);
            if (!k) return 0;
            if (S.role === 'host') return Math.min(k, 3);
            if (!S.gs) return 0;
            if (S.phase === 'hostleft') return 0;
            return S.gs.framesToRun(k);
        },
        /* before each simulated frame: this device's pads into the session */
        frame: function () {
            if (!S.session) return;
            var f = M._PortFrameNumber(), keys = seatKeys(), e = engine();
            M._PortSetRenderEnabled(ln.burst > 0 ? 0 : 1);
            if (S.role === 'host') {
                if (S.hub) S.hub.beforeFrame(f, keys); else e.localInput(f, keys[0], 0);
                /* the session view is the host's (sealed VIEW events) */
                var v = M._PortSimViewRequested() >>> 0;
                if (v !== S.lastView) {
                    S.lastView = v;
                    e.event(2, v >>> 16, v & 0xFFFF, 0, 0);
                }
            } else if (S.gs) {
                S.gs.beforeFrame(f, keys);
            }
        },
    };

    function startEngine() {
        S.startWanted = false;
        var role = S.role === 'host' ? 1 : 2;
        if (!M._PortLnStart(role, KF_EVERY, KF_COUNT, 0)) {
            log('could not start the session (no memory for the history?)');
            banner('Could not start the online game on this device (memory). Reload the page.', 0);
            ln.on = false;
            return;
        }
        S.session = true;
        S.startedAt = nowMs();
        S.startFrame = M._PortFrameNumber();
        S.t0 = 0;
        S.owners = noOwners();
        if (role === 1) {
            /* the room code, for the pause menus (session state from the
             * first frame: platform/lmpnet.c, platform/menu_poc.c) */
            if (M._PortLnSetRoomCode && S.code && S.code.length === 4)
                M._PortLnSetRoomCode(S.code.charCodeAt(0), S.code.charCodeAt(1), S.code.charCodeAt(2), S.code.charCodeAt(3));
            S.lastView = M._PortSimViewRequested() >>> 0;
            /* an 8-player game (the 16-Kirby module): eight seats, and each
             * sealed record sent about four times instead of until acked, in
             * packets under the data channel's fragmentation size */
            S.hub = new LS.HostHub(engine(), clock, wide() ? { codec: CODEC, seats: seats(), copies: 4, maxPacketBytes: 1100 }
                                                          : { codec: CODEC });
            /* HOST GAME > START, Left/Right: FREE ROAM is rooms mode, sealed
             * for every device at this first frame (platform/localmp.c);
             * ?rooms=1 is the same for tests */
            S.rooms = !!((M._PortOuiRoomsMode && M._PortOuiRoomsMode()) || q.get('rooms') === '1');
            if (S.rooms) M._PortLnEvent(7, 1, 0, 0, 0);
            /* everyone already in: their Kirbys from the first frame */
            forPeers(function (p) { if (p.ready) addToHub(p); });
            for (var s = 1; s < nSlots(); s++) S.owners[s] = M._PortLnSlotWant(s);
        } else {
            S.gs = new LS.GuestSession(engine(), txOf(S.peer.link), clock,
                                       { codec: CODEC, leadCap: KF_EVERY * (KF_COUNT - 2) - 10, samples: false });
            S.gs.onSeatFull = function (seat) {
                S.stats.refusedFull++;
                /* the device module had given that controller the seat: park
                 * it (its Start asks again) -- it says so on the page */
                var kd = window.katamDevices, pl = kd && kd.listPlayers ? kd.listPlayers()[seat] : null;
                if (seat >= 1 && pl && pl.devices.length) kd.dropOut(seat, 'full');
                else banner('No Kirby free - the game has ' + seats() + ' players', 3500);
            };
            /* the host's save memory is in the image: never store it here */
            if (window.shell && shell.save && shell.save.hold) shell.save.hold();
            else if (M._shell && M._shell.save && M._shell.save.hold) M._shell.save.hold();
        }
        log('session on: ' + S.role + ' from frame ' + M._PortFrameNumber());
    }

    /* --- host ------------------------------------------------------------------ */
    function forPeers(fn) { Object.keys(S.peers).forEach(function (k) { fn(S.peers[k]); }); }
    function peerList() { return Object.keys(S.peers).map(function (k) { return S.peers[k]; }); }
    function readyCount() { return peerList().filter(function (p) { return p.ready; }).length; }

    function addToHub(p) {
        var hs = S.hub.add(p.dev, txOf(p.link));
        S.stats.joins++;
        if (hs.slot < 0) {
            S.stats.refusedFull++;
            log('device ' + p.dev + ' is in, but every seat (' + seats() + ') is a person\'s: it plays when one is free');
        }
        return hs;
    }

    /* the HOST screen: who is in */
    function hostUi() {
        if (S.session || S.role !== 'host' || !S.code) return;
        var ps = peerList(), ready = readyCount();
        if (up() && M._PortOuiSetPlayers) M._PortOuiSetPlayers(1 + ready, ps.length - ready);
        ui(!ps.length ? UI.H_WAITING : ready ? UI.H_READY : UI.H_JOINING, S.code);
    }

    function hostOpen() {
        if (S.role === 'host' && S.net) { hostUi(); return; }
        reset();
        S.role = 'host'; S.phase = 'hosting';
        if (up() && M._PortOuiSetPlayers) M._PortOuiSetPlayers(1, 0);
        if (wide() && phone) banner('8-player game (experimental): a desktop host is best - a phone may stutter', 5000);
        ui(UI.H_CREATING);
        var gen = ++S.gen;
        versions().then(function (v) {
            return Net.createRoom(BASE).then(function (r) { return { v: v, r: r }; });
        }).then(function (x) {
            if (gen !== S.gen) return;
            if (!x.r.code) { log('no room: ' + Net.explainError(x.r)); ui(UI.H_NOSERVER); S.phase = 'failed'; return; }
            S.code = x.r.code;
            if (KB) KB.hosting(S.code, x.r.resume, function () { return 1 + readyCount(); },   /* server browser */
                               { max: seats(), mod: wide() ? '16' : null });
            var net = S.net = Net.connect({ base: BASE, code: S.code, hello: hello(x.v), log: log });
            S.ver = x.v;
            net.on('welcome', function () {
                if (gen !== S.gen) return;
                log('hosting ' + S.code);
                hostUi();
            });
            net.on('error', function (e) {
                log('room: ' + Net.explainError(e));
                if (!S.session) ui(UI.H_NOSERVER);
            });
            net.on('signal', hostSignal);
            net.on('roster', function (m) {
                forPeers(function (p) {
                    var here = m.members.some(function (x) { return x.mid === p.mid && x.connected; });
                    if (!here && p.ready && nowMs() - p.at > 3000) guestGone(p, 'left the room');
                });
            });
        });
    }

    /* a device id for a newcomer: the one this tab had (a reload), else the lowest free */
    function devFor(tok) {
        var used = {};
        forPeers(function (p) { used[p.dev] = true; });
        var d = tok && S.tokDev[tok];
        if (d && !used[d]) return d;
        for (d = 1; d <= maxGuests(); d++) if (!used[d]) return d;
        return -1;
    }

    function hostSignal(m, pwOk) {
        var d = m.data || {};
        /* server browser: a password's answer; a locked game challenges a `hi` */
        if (KB && d.k === 'pwr') {
            var net0 = S.net;
            KB.verify(m, d, net0, function (mm) { if (S.net === net0 && S.role === 'host') hostSignal(mm, true); });
            return;
        }
        if (d.k === 'hi') {
            var v = S.ver;
            if (d.sim !== v.sim || d.rom !== v.rom) {
                /* mod: this game's module, so a page on the other one can reload into it */
                S.net.send({ t: 'signal', to: m.from, data: { k: 'bad', sid: d.sid, field: d.sim !== v.sim ? 'sim' : 'rom', mod: modName() } });
                return;
            }
            /* a device the host removed: not back in this room (its tab
             * token; a new tab gets a new one -- a new room is the remedy) */
            if (d.tok && S.banned[d.tok]) {
                S.stats.refusedRemoved = (S.stats.refusedRemoved || 0) + 1;
                S.net.send({ t: 'signal', to: m.from, data: { k: 'removed', sid: d.sid } });
                return;
            }
            if (KB && !pwOk && KB.gate(m, d, S.net)) return;      /* server browser */
            var old = S.peers[m.from];
            if (old) dropPeer(old, false);                 /* the same member again: a fresh link */
            /* a tab that was here before (and whose link is dead) comes back as itself */
            forPeers(function (p) { if (d.tok && p.tok === d.tok) guestGone(p, 'came back'); });
            var dev = devFor(d.tok);
            /* full: every device id taken (three guests; seven in an 8-player
             * game), or (in play) no seat free for its player */
            var noKirby = S.session && M._PortLnFreeSlots() === 0;
            if (dev < 0 || noKirby) {
                S.stats.refusedFull++;
                S.net.send({ t: 'signal', to: m.from, data: { k: 'busy', sid: d.sid } });
                return;
            }
            if (d.tok) S.tokDev[d.tok] = dev;
            var link = new Link(S.net, m.from, d.sid, false, !!d.relay);
            var peer = { mid: m.from, sid: d.sid, link: link, ready: false, at: nowMs(), dev: dev, tok: d.tok || null };
            S.peers[m.from] = peer;
            link.onReliable = function (msg) {
                if (S.peers[peer.mid] !== peer) return;
                if (msg.t === 'ready') {
                    peer.ready = true;
                    log('device ' + peer.dev + ' is in (' + link.path() + ')');
                    if (!S.session) hostUi();
                    else addToHub(peer);
                    return;
                }
                if (msg.t === 'bye') { guestGone(peer, 'left'); return; }
                S.inbox.push({ peer: peer, m: msg });
            };
            link.onDatagram = function (b) { if (S.peers[peer.mid] === peer) S.inbox.push({ peer: peer, d: b }); };
            S.net.send({ t: 'signal', to: m.from, data: { k: 'hiok', sid: d.sid, relay: KNOB.relay, dev: dev } });
            if (!S.session) hostUi();
            else banner('A player is joining...', 3000);
            return;
        }
        var p = S.peers[m.from];
        if (!p || d.sid !== p.sid) return;
        if (d.k === 'bye') { guestGone(p, 'left'); return; }
        p.link.signal(d);
    }

    function dropPeer(p, bye) {
        if (!p || S.peers[p.mid] !== p) return;
        p.link.close(bye);
        delete S.peers[p.mid];
        if (S.hub && S.hub.get(p.dev)) S.hub.remove(p.dev);
    }

    function guestGone(p, why) {
        if (!p || S.peers[p.mid] !== p) return;
        log('device ' + p.dev + ' ' + why);
        var was = p.ready;
        dropPeer(p, false);
        S.stats.guestDrops++;
        /* in play, the Kirbys' notice comes from the sealed frames (watchSlots) */
        if (!S.session) hostUi();
        else if (!was) banner('A player could not connect', 3000);
    }

    function hostStart() {
        if (S.role !== 'host' || S.session) return;
        S.startWanted = true;
        ln.on = true;
        S.phase = 'playing';
    }

    function hostClose() {
        if (S.role !== 'host' || S.session) return;
        reset();
        ui(UI.IDLE);
    }

    function inviteLink() {
        var u = new URL(location.href);
        ['join', 'netlag', 'netjitter', 'netloss', 'netrelay', 'kirbys', 'mod'].forEach(function (k) { u.searchParams.delete(k); });
        if (wide()) u.searchParams.set('mod', '16');        /* the joiner's page loads this game's module */
        u.searchParams.set('join', S.code);
        return u.toString();
    }

    function copyLink() {
        if (!S.code) return;
        var link = inviteLink();
        S.lastInvite = link;
        var ok = function () { note(NOTE.COPIED); };
        var no = function () { note(NOTE.NOCOPY); banner('Invite: ' + link, 8000); };
        try {
            if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(link).then(ok, no);
            else no();
        } catch (e) { no(); }
    }

    /* --- guest -------------------------------------------------------------- */
    function joinFail(state, text) {
        S.phase = 'failed';
        if (S.peer) { S.peer.link.close(true); S.peer = null; }
        if (S.net) { try { S.net.close(); } catch (e) { /* gone */ } S.net = null; }
        ui(state);
        if (S.invite) banner(text, 0);
        log('join failed: ' + text);
    }

    function guestJoin(code, invite) {
        /* the public list said which module that game runs on */
        var lm = KB && KB.modOf ? KB.modOf(code) : null;
        if (lm && lm !== modName() && hop(lm, code, 'the public list')) return;
        reset();
        S.role = 'guest'; S.phase = 'joining'; S.code = code; S.invite = invite ? code : null;
        ui(UI.J_CONNECTING);
        if (invite) banner('Joining game ' + code + '...', 0);
        var gen = ++S.gen;
        versions().then(function (v) {
            S.ver = v;
            return Net.lookupRoom(BASE, code);
        }).then(function (r) {
            if (gen !== S.gen) return;
            if (r.status === 404) return joinFail(UI.J_NOGAME, 'No game with the code ' + code + '. Check the letters.');
            if (r.status !== 200) return joinFail(UI.J_FAILED, 'Could not reach the game server. Try again.');
            var net = S.net = Net.connect({ base: BASE, code: code, hello: hello(S.ver), log: log });
            var hiTimer = 0;
            net.on('error', function (e) {
                if (gen !== S.gen) return;
                if (e.code === 'version' && e.field === 'rom') return joinFail(UI.J_ROM, 'Your ROM is not the same as the host\'s.');
                /* the room's id is the other module's: that game runs there */
                if (e.code === 'version' && e.field === 'sim' && e.room && S.ver && e.room === S.ver.other
                    && hop(modName() === '16' ? '4' : '16', code, 'the room\'s version')) return;
                if (e.code === 'version') return joinFail(UI.J_BUILD, 'The host runs a different build. Refresh the page (both of you).');
                if (e.code === 'full') return joinFail(UI.J_FULL, 'That game is full.');
                if (e.code === 'unknown') return joinFail(UI.J_NOGAME, 'No game with the code ' + code + '.');
                joinFail(UI.J_FAILED, Net.explainError(e));
            });
            net.on('welcome', function (w, again) {
                if (gen !== S.gen || again && S.peer) return;
                var sid = rid();
                S.hostMid = net.host;
                net.send({ t: 'signal', to: net.host, data: { k: 'hi', sid: sid, sim: S.ver.sim, rom: S.ver.rom, relay: KNOB.relay, tok: TOK } });
                S.pendingSid = sid;
                clearTimeout(hiTimer);
                hiTimer = setTimeout(function () {
                    if (gen === S.gen && !S.peer) joinFail(UI.J_FAILED, 'The host does not answer. Try again.');
                }, 10000);
            });
            net.on('roster', function (m) {
                if (gen !== S.gen || !S.peer) return;
                var host = m.members.filter(function (x) { return x.mid === S.hostMid; })[0];
                if (host && !host.connected) S.hostAwayAt = S.hostAwayAt || nowMs();
                else S.hostAwayAt = 0;
            });
            net.on('signal', function (m) {
                if (gen !== S.gen || m.from !== S.hostMid) return;
                var d = m.data || {};
                /* server browser: a locked game asks for its password (web/online_browse.js) */
                if (KB && (d.k === 'pw' || d.k === 'pwbad' || d.k === 'pwlock')) {
                    if (d.sid !== S.pendingSid || S.peer) return;
                    clearTimeout(hiTimer);
                    if (d.k === 'pwlock') return joinFail(KB.UI.J_PWLOCK, 'Too many wrong passwords: the host stopped answering.');
                    KB.onChallenge(d, code, !!S.invite, function (h) {
                        if (gen !== S.gen || S.peer) return;
                        net.send({ t: 'signal', to: net.host, data: { k: 'pwr', sid: d.sid, h: h } });
                        ui(UI.J_CONNECTING);
                        hiTimer = setTimeout(function () {
                            if (gen === S.gen && !S.peer) joinFail(UI.J_FAILED, 'The host does not answer. Try again.');
                        }, 10000);
                    });
                    return;
                }
                if (d.k === 'busy') { clearTimeout(hiTimer); return joinFail(UI.J_FULL, 'That game is full.'); }
                if (d.k === 'removed') {
                    clearTimeout(hiTimer);
                    if (S.session) { removedByHost(); return; }
                    return joinFail(UI.J_REMOVED, 'The host removed you from this game.');
                }
                if (d.k === 'bad') {
                    clearTimeout(hiTimer);
                    if (d.field === 'sim' && d.mod && d.mod !== modName() && hop(d.mod, code, 'the host')) return;
                    return d.field === 'rom' ? joinFail(UI.J_ROM, 'Your ROM is not the same as the host\'s.')
                        : joinFail(UI.J_BUILD, 'The host runs a different build. Refresh the page (both of you).');
                }
                if (d.k === 'hiok') {
                    if (d.sid !== S.pendingSid || S.peer) return;
                    clearTimeout(hiTimer);
                    S.myDev = d.dev || null;
                    S.seatWaitFrom = nowMs();
                    if (wide() && phone) banner('8-player game (experimental): a phone may stutter', 4000);
                    guestLink(net, d.sid, !!d.relay);
                    return;
                }
                if (!S.peer || d.sid !== S.peer.sid) return;
                if (d.k === 'bye') { hostGone('closed the game'); return; }
                S.peer.link.signal(d);
            });
        });
    }

    function guestLink(net, sid, forceRelay) {
        var link = new Link(net, S.hostMid, sid, true, forceRelay);
        var peer = S.peer = { mid: S.hostMid, sid: sid, link: link };
        link.onDecided = function () {
            if (peer !== S.peer) return;
            link.reliable({ t: 'ready' });
            S.phase = 'waiting';
            ui(UI.J_WAITING);
            if (S.invite) banner('Connected (' + (link.p2p ? 'direct' : 'relay') + '). Waiting for the host to start...', 0);
            log('connected to the host (' + link.path() + '), waiting for the game');
        };
        link.onDatagram = function (b) { if (peer === S.peer && S.session) S.inbox.push({ d: b }); };
        link.onReliable = function (msg) {
            if (peer !== S.peer) return;
            if (msg.t === 'bye') { hostGone('closed the game'); return; }
            if (msg.t === 'seatfull') { S.inbox.push({ m: msg }); return; }
            /* every slot's name, from the host: presentation only, so at once
             * (it may come before the image does) */
            if (msg.t === 'names') { var e = engine(); if (e.setSlotNames && typeof msg.n === 'string') e.setSlotNames(msg.n); return; }
            if (msg.t !== 'image') return;
            if (!S.session) { ui(UI.J_STARTING); if (S.invite) banner('Getting the game...', 0); }
            var t0 = nowMs();
            CODEC.inflate(msg.comp).then(function (raw) {
                if (peer !== S.peer) return;
                S.lastImageMs = nowMs() - t0;
                S.inbox.push({ img: raw, dev: msg.dev });
                S.phase = 'playing';
                if (!S.session) { S.startWanted = true; ln.on = true; }
            }, function (e) { log('image would not inflate: ' + e.message); });
        };
    }

    function hostGone(why) {
        if (S.phase === 'hostleft') return;
        log('the host ' + why);
        S.phase = 'hostleft';
        if (S.peer) { S.peer.link.close(false); }
        if (S.session) {
            banner('The host left the game. Press A or Start to go back to the title.', 0);
            leaveButton(true);
            padExit();
        }
        else { ui(UI.J_HOSTLEFT); if (S.invite) banner('The host left. Reload the page to try again.', 0); }
    }
    /* The game no longer runs (nothing comes from the host), so its menus
     * cannot take a key: a fresh A, B or Start on this device's pad or keys
     * is the way out, as the button is for a mouse or a finger. */
    function padExit() {
        var was = up() ? M._PortCurrentKeys() & 0x0B : 0x0B, t = setInterval(function () {
            if (S.phase !== 'hostleft' || !S.session || !up()) { clearInterval(t); return; }
            var k = M._PortCurrentKeys() & 0x0B;          /* A, B, Start */
            if (k & ~was) { clearInterval(t); leave('the host had left'); return; }
            was = k;
        }, 50);
    }

    /* --- leaving: back to the title, without reloading the page ------------------
     * The host ends the game for everyone (each guest hears 'bye' and gets the
     * same way out); a guest leaves alone (the host hands its Kirbys to the
     * CPU).  Either way this device's game goes back to power-on and boots to
     * the title (platform/lmpnet.c PortLnGoHome: its own save, its own view),
     * and the ONLINE screens can host or join again. */
    function leaveButton(on) {
        var b = document.getElementById('lnleave');
        if (!b && on) {
            b = document.createElement('button');
            b.id = 'lnleave';
            b.type = 'button';
            b.textContent = 'Back to the title';
            b.style.cssText = 'position:fixed;left:50%;bottom:calc(12% - 52px);transform:translateX(-50%);z-index:71;' +
                'padding:8px 18px;border-radius:12px;border:3px solid #f8d850;background:#f8d850;color:#181040;' +
                'font:bold 15px/1.2 ui-monospace,Menlo,Consolas,monospace;letter-spacing:.06em;text-transform:uppercase;' +
                'box-shadow:0 4px 0 rgba(0,0,0,.35);cursor:pointer';
            b.addEventListener('click', function () { leave('the host had left'); });
            document.body.appendChild(b);
        }
        if (b) b.style.display = on ? 'block' : 'none';
    }
    function leave(why) {
        if (!S.role && !S.session) return false;
        var was = S.role, inGame = S.session;
        log((was === 'host' ? 'ending the online game' : 'leaving the online game') + (why ? ' (' + why + ')' : ''));
        reset();                                /* links closed with a bye, the room left */
        S.inbox = []; S.startWanted = false; S.accept = false; S.gs = null; S.hub = null; S.eng = S.eng || null;
        S.owners = noOwners(); S.myDev = null; S.hostAwayAt = 0; S.tokDev = {};
        if (inGame) {
            S.session = false;
            ln.on = false; ln.burst = 0;
            if (up() && M._PortLnGoHome && !M._PortLnGoHome()) log('no power-on picture: the game stays where it is');
            /* the save memory is this device's own again: storing it may resume */
            var sh = window.shell || M._shell;
            if (sh && sh.save && sh.save.release) sh.save.release();
        }
        if (up() && M._PortCardSessionEnd) M._PortCardSessionEnd();
        if (up() && M._PortOuiReset) M._PortOuiReset();
        ui(UI.IDLE);
        leaveButton(false);
        banner(was === 'host' && inGame ? 'Online game ended' : 'You left the online game', 2500);
        S.stats.leaves = (S.stats.leaves || 0) + 1;
        return true;
    }

    /* --- removing a guest (the host's SETTINGS > ONLINE list) -------------------
     * The host's pause menu asks (platform/menu_poc.c), and the host's final
     * frame carries it out here, once: that device's link closes, its Kirbys
     * go to the CPU as when a guest leaves, and its tab token may not come
     * back into this room. */
    function kick(slot) {
        if (S.role !== 'host' || !S.session || !M._PortLnSlotOwner) return false;
        var o = M._PortLnSlotOwner(slot | 0), dev = o < 0 ? -1 : o >> 2, done = false;
        if (dev < 1) return false;
        forPeers(function (p) {
            if (p.dev !== dev || done) return;
            done = true;
            if (p.tok) S.banned[p.tok] = true;
            try { S.net.send({ t: 'signal', to: p.mid, data: { k: 'removed', sid: p.sid } }); } catch (e) { /* gone */ }
            S.stats.removed = (S.stats.removed || 0) + 1;
            guestGone(p, 'removed by the host');
        });
        if (done) banner('Player removed', 2500);
        return done;
    }
    /* this guest: the host removed it -- the title, with a plain message */
    function removedByHost() {
        if (S.role !== 'guest') return;
        leave('removed by the host');
        S.stats.removedByHost = (S.stats.removedByHost || 0) + 1;
        banner('REMOVED BY THE HOST', 5000);
    }

    function joinCancel() {
        if (S.role !== 'guest' || S.session) return;
        if (S.peer) S.peer.link.close(true);
        reset();
        ui(UI.IDLE);
        banner(null);
    }

    function reset() {
        if (KB) KB.ended();                     /* server browser: off the list */
        S.gen = (S.gen || 0) + 1;
        if (S.peer) { S.peer.link.close(true); S.peer = null; }
        if (S.net) { try { S.net.leave(); } catch (e) { /* gone */ } S.net = null; }
        forPeers(function (p) { p.link.close(true); });
        S.peers = {};
        S.role = null; S.phase = 'idle'; S.code = null; S.hub = null; S.pendingSid = null;
        S.banned = {};
    }

    /* The page again on the other module, at the title (the game restarts):
     * katamOnline.otherModule() too, for a page button or a test. */
    function otherModule() {
        var mod = wide() ? '4' : '16';
        log('module: to the ' + (mod === '16' ? '8-player' : 'ordinary') + ' module (the page reloads)');
        if (S.role) { try { reset(); } catch (e) { /* going anyway */ } }
        banner(mod === '16' ? 'Loading the 8-player module: then MULTIPLAYER > ONLINE > HOST GAME' : 'Loading the ordinary module...', 0);
        var u = new URL(location.href);
        ['kirbys', 'mod', 'join'].forEach(function (k) { u.searchParams.delete(k); });
        u.searchParams.set('mod', mod);
        setTimeout(function () { location.href = u.toString(); }, 50);
        return true;
    }

    /* --- polling: the game's requests, and liveness ---------------------------- */
    function poll() {
        if (!up()) return;
        var ev;
        while ((ev = M._PortOuiPoll())) {
            switch (ev) {
            case EV.HOST_OPEN: hostOpen(); break;
            case EV.HOST_CLOSE: hostClose(); break;
            case EV.HOST_START: hostStart(); break;
            case EV.COPY_LINK: copyLink(); break;
            case EV.JOIN: {
                var c = '';
                for (var i = 0; i < 4; i++) c += String.fromCharCode(M._PortOuiJoinCode(i));
                guestJoin(c, false);
                break;
            }
            case EV.JOIN_CANCEL: joinCancel(); break;
            /* ONLINE > HOST 8 PLAYERS (the ordinary module) or 4 PLAYER GAMES
             * (the 16-Kirby one): the page again, on the other module */
            case EV.OTHER_MODULE: otherModule(); break;
            }
        }
    }
    setInterval(function () {
        if (S.role === 'host') forPeers(function (p) { if (p.link.dead()) guestGone(p, 'stopped answering'); });
        if (S.role === 'guest' && S.peer && S.phase !== 'hostleft' && S.phase !== 'failed') {
            if (S.peer.link.dead() || (S.hostAwayAt && nowMs() - S.hostAwayAt > 3000)) hostGone('stopped answering');
        }
    }, 500);

    var present = M.portPresent;
    M.portPresent = function (ptr, w, h) {
        if (present) present(ptr, w, h);
        try { poll(); } catch (e) { log('poll: ' + (e.stack || e)); }
    };
    window.addEventListener('pagehide', function () {
        if (S.peer) S.peer.link.close(true);
        forPeers(function (p) { p.link.close(true); });
        if (S.net) { try { S.net.leave(); } catch (e) { /* gone */ } }
    });

    /* --- invite links ------------------------------------------------------------ */
    /* (on either module: a game on the other one reloads the page into it) */
    var joinParam = q.get('join');
    if (joinParam) {
        var code = Net.normalizeCode(joinParam);
        if (!code) banner('"' + joinParam + '" is not a game code (4 letters, no vowels).', 0);
        else {
            banner('Load your ROM to join game ' + code, 0);
            var wait = setInterval(function () {
                if (!up() || !M._PortFrameNumber || M._PortFrameNumber() < 30) return;
                clearInterval(wait);
                guestJoin(code, true);
            }, 200);
        }
    }

    /* --- telemetry ------------------------------------------------------------------ */
    function pct(a, b) { return +(100 * a / Math.max(1, b)).toFixed(2); }
    function slotsNow() {
        var out = [];
        for (var s = 0; s < nSlots(); s++) {
            var o = S.session && M._PortLnSlotOwner ? M._PortLnSlotOwner(s) : (s === 0 ? 0 : -1);
            out.push(o < 0 ? null : { dev: o >> 2, seat: o & 3 });
        }
        return out;
    }
    function peerStats(p, t) {
        var hs = S.hub ? S.hub.get(p.dev) : null, l = p.link, slots = [];
        for (var s = 1; s < nSlots(); s++) if (S.session && (M._PortLnSlotOwner(s) >> 2) === p.dev && M._PortLnSlotOwner(s) >= 0) slots.push(s);
        var secs = Math.max(1e-3, (nowMs() - (p.at || S.startedAt || nowMs())) / 1000);
        var o = { dev: p.dev, ready: p.ready, slots: slots, path: l.path(), dcOpen: !!l.p2p,
                  linkRttMs: l.rtt !== null ? Math.round(l.rtt) : null, bytesIn: l.bIn, bytesOut: l.bOut, silentMs: Math.round(nowMs() - l.lastRecv),
                  /* per peer, since it connected: what this host sends it and hears from it */
                  upBps: Math.round(l.bOut / secs), downBps: Math.round(l.bIn / secs) };
        if (hs) {
            o.inputsIn = hs.st.inputsIn; o.images = hs.st.images; o.repairReqs = hs.st.repairReqs;
            o.lastImageBytes = hs.st.imageComp; o.seatReqs = hs.st.seatReqs || 0; o.seatFull = hs.st.seatFull || 0;
            o.guestAck = hs.guestAck; o.guestContig = hs.guestContig;
            /* bounded redundancy (8-player games): retransmits from the ack, its timeout */
            o.resends = hs.st.resends; o.resentRecs = hs.st.resentRecs; o.rtoMs = hs.st.rtoMs; o.ackRttMs = hs.st.srttMs;
            o.maxPacket = hs.st.maxPacket; o.recsPerPacket = +(hs.st.recs / Math.max(1, hs.st.packets)).toFixed(2);
            o.seatWaitMs = hs.st.seatWaitMs === undefined ? null : hs.st.seatWaitMs;
        }
        if (t) {
            var miss = t['d' + p.dev + 'KeyMissing'] || 0, on = t['d' + p.dev + 'KeyOnTime'] || 0;
            o.keyLatePct = pct(miss, miss + on);
        }
        return o;
    }
    function stats() {
        var t = tel(), link = S.peer && S.peer.link, secs = S.session ? (nowMs() - S.startedAt) / 1000 : 0;
        var o = {
            role: S.role, phase: S.phase, code: S.code, session: S.session,
            path: link ? link.path() : null, dcOpen: !!(link && link.p2p), linkDecided: !!(link && link.decided),
            linkRttMs: link && link.rtt !== null ? Math.round(link.rtt) : null,
            peer: S.role === 'guest' ? !!S.peer : readyCount() > 0,
            frame: up() ? M._PortFrameNumber() : 0, sessionSec: +secs.toFixed(1), startFrame: S.startFrame,
            knobs: KNOB, net: Object.assign({}, S.stats), reanchors: S.reanchors, banner: S.banner,
            screen: up() ? M._PortOuiScreen() : 0,
            slots: slotsNow(), self: S.session && M._PortLnSelf ? M._PortLnSelf() : (S.role === 'host' ? 0 : S.myDev || null),
            module: modName(), seats: seats(), kirbys: up() && M._PortPocKirbysNow ? M._PortPocKirbysNow() : null,
        };
        o.players = o.slots.filter(Boolean).length;
        if (S.role === 'host') {
            o.peers = peerList().map(function (p) { return peerStats(p, t); });
            o.upBps = o.peers.reduce(function (a, p) { return a + p.upBps; }, 0);
            o.downBps = o.peers.reduce(function (a, p) { return a + p.downBps; }, 0);
            o.devices = 1 + o.peers.filter(function (p) { return p.ready; }).length;
            if (S.hub) o.seatWaits = S.hub.st.seatWaits.slice();
            if (o.peers.length) { var p1 = o.peers[0]; o.path = p1.path; o.dcOpen = p1.dcOpen; o.linkRttMs = p1.linkRttMs; }
        }
        if (t) {
            var mins = Math.max(secs / 60, 1e-9);
            o.sealedUpTo = t.sealedUpTo; o.verifiedUpTo = t.verifiedUpTo; o.verified = t.verified;
            o.mismatches = t.mismatches; o.repairWanted = t.repairWanted; o.firstMismatch = t.firstMismatch; o.lastMismatch = t.lastMismatch; o.historyExceeded = t.historyExceeded;
            o.rewinds = t.rewinds; o.rewindsPerMin = +(t.rewinds / mins).toFixed(1);
            o.rewindsPerSec = +(t.rewinds / Math.max(secs, 1e-9)).toFixed(2);
            o.replayAvg = +(t.replayedFrames / Math.max(1, t.rewinds)).toFixed(1);      /* frames replayed per rewind: its depth */
            o.replayedFrames = t.replayedFrames; o.imagesLoaded = t.imagesLoaded;
            o.repairs = S.role === 'guest' ? Math.max(0, t.imagesLoaded - 1) : Math.max(0, S.stats.imagesSent - S.stats.joins);
            o.ownOverridden = t.ownOverridden; o.sealedRecv = t.sealedRecv;
            o.ownOverriddenPct = pct(t.ownOverridden, t.sealedRecv);
            o.mispredHostPct = pct(t.mispredHost, t.sealedRecv); o.mispredOtherPct = pct(t.mispredOther, t.sealedRecv);
            o.hostKeyLatePct = pct(t.hostKeyMissing, t.hostKeyMissing + t.hostKeyOnTime);
            o.soundsMissed = t.soundsMissed; o.soundsSpurious = t.soundsSpurious;
            o.seatGrants = t.seatGrants; o.seatReleases = t.seatReleases; o.seatFull = t.seatFull; o.hostRewinds = t.hostRewinds;
            /* rooms mode (FREE ROAM): this device's screen and, on a guest,
             * its game against the canonical one (platform/lmpnet.c) */
            o.rooms = M._PortLmpRooms ? M._PortLmpRooms() : 0;
            if (o.rooms) {
                o.view = t.view; o.digCompared = t.digCompared; o.digEqual = t.digEqual; o.digMismatch = t.digMismatch;
                o.persRepairs = t.persRepairs; o.canonFrame = t.canonFrame; o.canonRuns = t.canonRuns;
                o.canonUsPerFrame = t.canonFrames ? +(t.canonUsSum / t.canonFrames).toFixed(1) : null;
            }
        }
        if (S.gs) {
            var g = S.gs;
            o.lead = g.lastLead; o.margin = g.margin; o.rttMs = g.rtt === null ? null : +g.rtt.toFixed(1);
            o.holds = g.st.holds; o.skips = g.st.skips; o.bursts = g.st.bursts; o.capHolds = g.st.capHolds;
            o.repairReqs = g.st.repairReqs; o.joined = g.joined; o.lastImageMs = S.lastImageMs;
            o.seatAsks = g.st.seatAsks || 0; o.seatFullNotes = g.st.seatFull || 0;
            o.upBps = Math.round((g.st.bytesOut || 0) / Math.max(secs, 1e-3)); o.downBps = Math.round((g.st.bytesIn || 0) / Math.max(secs, 1e-3));
            o.seatWaitMs = S.stats.seatWaitMs === undefined ? null : S.stats.seatWaitMs;
        }
        if (S.hub) {
            var p0 = S.hub.get(1) || S.hub.get(S.hub.devs()[0]);
            if (p0) { o.hostImages = p0.st.images; o.repairReqs = p0.st.repairReqs; o.inputsIn = p0.st.inputsIn; o.lastImageBytes = p0.st.imageComp; }
        }
        return o;
    }
    function statsText() {
        if (!S.role) return '';
        var o = stats(), L = [];
        var who = o.slots.map(function (x, i) { return 'P' + (i + 1) + ' ' + (x ? (x.dev === o.self ? 'you' : x.dev === 0 ? 'host' : 'dev ' + x.dev) + (x.seat ? '.' + x.seat : '') : 'cpu'); });
        L.push((o.role === 'host' ? 'Hosting' : 'Joined') + ' game ' + (o.code || '-') + ', ' + o.phase +
               (o.session ? ', players ' + o.players + '/' + o.seats : '') + (o.module === '16' ? ' (8-player module)' : ''));
        if (o.session) L.push(who.join(', '));
        if (o.role === 'host') {
            if (!o.peers.length) L.push('No guests connected');
            o.peers.forEach(function (p) {
                L.push('Dev ' + p.dev + (p.slots.length ? ' (P' + p.slots.map(function (s) { return s + 1; }).join(',P') + ')' : '') + ': ' +
                       (p.path === 'p2p' ? 'direct' : 'relay') + (p.linkRttMs !== null ? ' ' + p.linkRttMs + ' ms' : '') +
                       (p.ready ? '' : ', connecting') + (p.keyLatePct !== undefined ? ', late ' + p.keyLatePct + '%' : '') +
                       (p.images !== undefined ? ', images ' + p.images : '') + (p.repairReqs ? ', repairs ' + p.repairReqs : ''));
            });
        } else {
            L.push('Path: ' + (o.path === 'p2p' ? 'direct (WebRTC)' : o.path ? 'relay (room server)' : '-') +
                   (o.linkRttMs !== null ? ', link round trip ' + o.linkRttMs + ' ms' : ''));
        }
        if (o.session) {
            if (o.role === 'guest')
                L.push('Round trip ' + (o.rttMs === null ? '-' : o.rttMs + ' ms') + ', lead ' + o.lead + ' frames' +
                       ', holds ' + o.holds + ' skips ' + o.skips + ' bursts ' + o.bursts);
            L.push('Rewinds ' + o.rewinds + ' (' + o.rewindsPerMin + '/min, ' + o.replayAvg + ' frames each), repairs ' + o.repairs +
                   ', mismatches ' + o.mismatches + (o.repairWanted ? ' (repair pending)' : ''));
            if (o.upBps !== undefined) L.push('Bytes/s up ' + o.upBps + ', down ' + o.downBps +
                   (o.role === 'host' && o.peers.length ? ', retransmits ' + o.peers.reduce(function (a, p) { return a + (p.resends || 0); }, 0) : ''));
            if (o.role === 'guest') L.push('Own input overridden ' + o.ownOverriddenPct + '%, others mispredicted ' + o.mispredHostPct + '% / ' + o.mispredOtherPct + '%');
            else L.push('Guest keys late at the latch ' + o.hostKeyLatePct + '%');
        }
        if (KNOB.lag || KNOB.jitter || KNOB.loss || KNOB.relay)
            L.push('Test knobs: +' + KNOB.lag + ' ms, jitter ' + KNOB.jitter + ' ms, loss ' + (KNOB.loss * 100) + '%' +
                   (KNOB.relay ? ', relay forced' : ''));
        return L.join('\n');
    }
    /* tests: hashed frames in [from, to): [frame, lo, hi, final] */
    function hashes(from, to) {
        if (!S.session) return [];
        var p = M._malloc(256), out = [];
        for (var f = from - (from & 15) + ((from & 15) ? 16 : 0); f < to; f += 16) {
            var r = M._PortLnHashAt(f, p);
            if (r) out.push([f, M.HEAPU32[p >> 2], M.HEAPU32[(p >> 2) + 1], r === 2 ? 1 : 0]);
        }
        M._free(p);
        return out;
    }

    /* tests: frame f's hash by region {name: 'lo:hi'}, and whether final */
    function regions(f) {
        if (!S.session) return null;
        var p = M._malloc(256), names = '', a = M._PortLnRegionNames(), o = {};
        for (var i = a; M.HEAPU8[i]; i++) names += String.fromCharCode(M.HEAPU8[i]);
        var r = M._PortLnHashAt(f, p);
        if (r) names.split(',').forEach(function (n, k) {
            o[n] = M.HEAPU32[(p >> 2) + 2 + 2 * k] + ':' + M.HEAPU32[(p >> 2) + 3 + 2 * k];
        });
        M._free(p);
        return r ? { final: r === 2, rg: o } : null;
    }

    /* The seam for local players during a session (the device layer, or a
     * test): seat 1..3 of THIS device wants a Kirby (on) or gives it back
     * (off).  Its keys are whatever Module._PortLmpSetHostKeys(seat, ...)
     * holds.  The host answers on a sealed frame; katamOnline.slots() shows
     * the result.  Returns false if there is no session or the host is full.
     * The same as Module._PortLmpRequestHuman(seat, on) during a session. */
    function requestSlot(seat, on) {
        if (!S.session || !up()) return false;
        return !!M._PortLmpRequestHuman(seat, on === false || on === 0 ? 0 : 1);
    }

    window.katamOnline = {
        stats: stats, statsText: statsText, hashes: hashes, regions: regions,
        inSession: function () { return S.session; },
        /* leave (a guest) or end (the host) the online game: back to the title */
        leave: leave, kick: kick, role: function () { return S.role; },
        otherModule: otherModule, module: modName, seats: seats,
        requestSlot: requestSlot, slots: slotsNow,
        /* tests and the console */
        host: hostOpen, join: function (c) { guestJoin(Net.normalizeCode(c) || c, false); },
        cancelJoin: joinCancel,                 /* server browser: the password box's Cancel */
        invite: function () { return S.code ? inviteLink() : null; },
        _S: S,
    };
}());
