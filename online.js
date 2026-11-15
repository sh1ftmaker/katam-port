/* Online two-player play: the page's half.
 *
 * The game's own front end has the screens (Start Game > MULTIPLAYER >
 * ONLINE > HOST GAME / JOIN GAME: platform/online_ui.c, drawn by
 * platform/menu_poc.c).  This file does what they ask for:
 *
 *   rooms      a relay room (web/net_session.js, netplay/session.mjs) gives a
 *              4-letter code, gates on build and ROM at hello, and relays
 *              `signal` messages between its two members.
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
 * Invite links: ?join=CODE joins straight after the ROM is loaded.
 * Test knobs: ?netlag=ms (one way, added to everything this page sends),
 * ?netjitter=ms (uniform 0..ms on datagrams), ?netloss=% (datagrams),
 * ?netrelay=1 (never try WebRTC), ?netserver=<base> (another room server).
 *
 * window.katamOnline.stats() is the telemetry; the game menu's Game page
 * shows the same as text (Module.portUiQuery('online')). */
(function () {
    'use strict';
    var Net = window.KatamNet, LS = window.KatamLnSession;
    if (!Net || !LS) return;
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
    var UI = { IDLE: 0, H_CREATING: 1, H_WAITING: 2, H_JOINING: 3, H_READY: 4, H_NOSERVER: 5,
               J_CONNECTING: 6, J_WAITING: 7, J_STARTING: 8, J_NOGAME: 9, J_FULL: 10, J_BUILD: 11,
               J_ROM: 12, J_FAILED: 13, J_HOSTLEFT: 14 };
    var EV = { HOST_OPEN: 1, HOST_CLOSE: 2, HOST_START: 3, COPY_LINK: 4, JOIN: 5, JOIN_CANCEL: 6 };
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
        peer: null,            /* host: {mid, sid, link, ready, at}; guest: {mid, sid, link} */
        session: false,        /* the engine session is on (PortLnStart done) */
        startWanted: false,
        eng: null, hs: null, gs: null,
        inbox: [], accept: false,
        t0: 0, ticks: 0, reanchors: 0,
        startedAt: 0, lastView: -1, invite: null,
        uiState: 0, banner: null,
        stats: { relayIn: 0, relayOut: 0, p2pIn: 0, p2pOut: 0, bytesIn: 0, bytesOut: 0, dropped: 0,
                 joins: 0, guestDrops: 0, imagesSent: 0, imagesIn: 0 },
    };

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
        var sim = window.KATAM_SIM ? Promise.resolve(window.KATAM_SIM)
            : fetch('katam.wasm').then(function (r) { return r.arrayBuffer(); }).then(sha1)
                .then(function (h) { return 'dev.w' + h.slice(0, 12); }, function () { return 'dev'; });
        verP = Promise.all([sim, sha1(rom)]).then(function (v) { return { sim: v[0], rom: v[1] }; });
        return verP;
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
        this.lastRecv = nowMs(); this.rtt = null; this.relNext = 0;
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
        S.stats.bytesOut += bytes.length;
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
            var hdr = { t: 'image', F: msg.F, raw: msg.raw, reason: msg.reason, len: comp.length, n: n };
            S.stats.imagesSent++;
            S.stats.bytesOut += comp.length;
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
        S.stats.bytesIn += bytes.length;
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
        S.stats.bytesIn += u8.length;
        if (im.parts.length < im.hdr.n) return;
        this.img[via] = null;
        var comp = new Uint8Array(im.got), o = 0;
        for (var i = 0; i < im.parts.length; i++) { comp.set(im.parts[i], o); o += im.parts[i].length; }
        S.stats.imagesIn++;
        if (this.onReliable) this.onReliable({ t: 'image', F: im.hdr.F, raw: im.hdr.raw, reason: im.hdr.reason, comp: comp });
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
                if (!S.hs || it.peer !== S.peer) continue;
                if (it.d) S.hs.onDatagram(it.d); else S.hs.onReliable(it.m);
            } else if (S.gs) {
                if (it.img) { S.gs.onReliable({ t: 'image', raw: it.img }); continue; }
                if (it.d && S.accept) S.gs.onDatagram(it.d);
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
                if (S.hs) S.hs.afterTick();
            } else if (S.gs) {
                if (S.gs.joined && !S.gs.imagePending) S.accept = true;
                S.gs.afterTick();
            }
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
        /* before each simulated frame: this device's pad into the session */
        frame: function () {
            if (!S.session) return;
            var f = M._PortFrameNumber(), keys = M._PortCurrentKeys() & 0x3FF, e = engine();
            M._PortSetRenderEnabled(ln.burst > 0 ? 0 : 1);
            if (S.role === 'host') {
                if (S.hs) S.hs.beforeFrame(f, keys); else e.localInput(f, keys);
                /* player 2 idles while nobody is there */
                if (!S.peer || !S.peer.ready) e.remoteInput(f, 0);
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
        if (role === 1) {
            S.lastView = M._PortSimViewRequested() >>> 0;
            if (S.peer && S.peer.ready) newHostSession();
        } else {
            S.gs = new LS.GuestSession(engine(), txOf(S.peer.link), clock,
                                       { codec: CODEC, leadCap: KF_EVERY * (KF_COUNT - 2) - 10, samples: false });
            /* the host's save memory is in the image: never store it here */
            if (window.shell && shell.save && shell.save.hold) shell.save.hold();
            else if (M._shell && M._shell.save && M._shell.save.hold) M._shell.save.hold();
        }
        log('session on: ' + S.role + ' from frame ' + M._PortFrameNumber());
    }

    /* --- host ------------------------------------------------------------------ */
    function newHostSession() {
        S.hs = new LS.HostSession(engine(), txOf(S.peer.link), clock, { codec: CODEC });
        S.hs.wantImage('join');
        S.stats.joins++;
    }

    function hostOpen() {
        if (S.role === 'host' && S.net) { ui(S.peer && S.peer.ready ? UI.H_READY : UI.H_WAITING, S.code); return; }
        reset();
        S.role = 'host'; S.phase = 'hosting';
        ui(UI.H_CREATING);
        var gen = ++S.gen;
        versions().then(function (v) {
            return Net.createRoom(BASE).then(function (r) { return { v: v, r: r }; });
        }).then(function (x) {
            if (gen !== S.gen) return;
            if (!x.r.code) { log('no room: ' + Net.explainError(x.r)); ui(UI.H_NOSERVER); S.phase = 'failed'; return; }
            S.code = x.r.code;
            var net = S.net = Net.connect({ base: BASE, code: S.code, hello: hello(x.v), log: log });
            S.ver = x.v;
            net.on('welcome', function () {
                if (gen !== S.gen) return;
                log('hosting ' + S.code);
                if (!S.session) ui(S.peer && S.peer.ready ? UI.H_READY : UI.H_WAITING, S.code);
            });
            net.on('error', function (e) {
                log('room: ' + Net.explainError(e));
                if (!S.session) ui(UI.H_NOSERVER);
            });
            net.on('signal', hostSignal);
            net.on('roster', function (m) {
                if (!S.peer) return;
                var here = m.members.some(function (x) { return x.mid === S.peer.mid && x.connected; });
                if (!here && S.peer.ready && nowMs() - S.peer.at > 3000) guestGone('left the room');
            });
        });
    }

    function hostSignal(m) {
        var d = m.data || {};
        if (d.k === 'hi') {
            var v = S.ver;
            if (d.sim !== v.sim || d.rom !== v.rom) {
                S.net.send({ t: 'signal', to: m.from, data: { k: 'bad', sid: d.sid, field: d.sim !== v.sim ? 'sim' : 'rom' } });
                return;
            }
            if (S.peer && S.peer.mid !== m.from && !S.peer.link.dead()) {
                S.net.send({ t: 'signal', to: m.from, data: { k: 'busy', sid: d.sid } });
                return;
            }
            if (S.peer) dropPeer(false);
            var link = new Link(S.net, m.from, d.sid, false, !!d.relay);
            var peer = S.peer = { mid: m.from, sid: d.sid, link: link, ready: false, at: nowMs() };
            link.onReliable = function (msg) {
                if (peer !== S.peer) return;
                if (msg.t === 'ready') {
                    peer.ready = true;
                    log('player 2 is in (' + link.path() + ')');
                    if (!S.session) ui(UI.H_READY, S.code);
                    else { newHostSession(); banner('Player 2 joined', 2500); }
                    return;
                }
                if (msg.t === 'bye') { guestGone('left'); return; }
                S.inbox.push({ peer: peer, m: msg });
            };
            link.onDatagram = function (b) { if (peer === S.peer) S.inbox.push({ peer: peer, d: b }); };
            S.net.send({ t: 'signal', to: m.from, data: { k: 'hiok', sid: d.sid, relay: KNOB.relay } });
            if (!S.session) ui(UI.H_JOINING, S.code);
            else banner('Player 2 is joining...', 4000);
            return;
        }
        if (!S.peer || d.sid !== S.peer.sid || m.from !== S.peer.mid) return;
        if (d.k === 'bye') { guestGone('left'); return; }
        S.peer.link.signal(d);
    }

    function dropPeer(bye) {
        if (!S.peer) return;
        S.peer.link.close(bye);
        S.peer = null;
        S.hs = null;
    }

    function guestGone(why) {
        if (!S.peer) return;
        log('player 2 ' + why);
        dropPeer(false);
        S.stats.guestDrops++;
        if (S.session) banner('Player 2 left. They can come back with code ' + S.code, 6000);
        else ui(UI.H_WAITING, S.code);
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
        ['join', 'netlag', 'netjitter', 'netloss', 'netrelay'].forEach(function (k) { u.searchParams.delete(k); });
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
                if (e.code === 'version') return joinFail(UI.J_BUILD, 'The host runs a different build. Refresh the page (both of you).');
                if (e.code === 'full') return joinFail(UI.J_FULL, 'That game is full.');
                if (e.code === 'unknown') return joinFail(UI.J_NOGAME, 'No game with the code ' + code + '.');
                joinFail(UI.J_FAILED, Net.explainError(e));
            });
            net.on('welcome', function (w, again) {
                if (gen !== S.gen || again && S.peer) return;
                var sid = rid();
                S.hostMid = net.host;
                net.send({ t: 'signal', to: net.host, data: { k: 'hi', sid: sid, sim: S.ver.sim, rom: S.ver.rom, relay: KNOB.relay } });
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
                if (d.k === 'busy') { clearTimeout(hiTimer); return joinFail(UI.J_FULL, 'That game already has two players.'); }
                if (d.k === 'bad') {
                    clearTimeout(hiTimer);
                    return d.field === 'rom' ? joinFail(UI.J_ROM, 'Your ROM is not the same as the host\'s.')
                        : joinFail(UI.J_BUILD, 'The host runs a different build. Refresh the page (both of you).');
                }
                if (d.k === 'hiok') {
                    if (d.sid !== S.pendingSid || S.peer) return;
                    clearTimeout(hiTimer);
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
            if (msg.t !== 'image') return;
            if (!S.session) { ui(UI.J_STARTING); if (S.invite) banner('Getting the game...', 0); }
            var t0 = nowMs();
            CODEC.inflate(msg.comp).then(function (raw) {
                if (peer !== S.peer) return;
                S.lastImageMs = nowMs() - t0;
                S.inbox.push({ img: raw });
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
        if (S.session) banner('The host left the game. Reload the page to play again.', 0);
        else { ui(UI.J_HOSTLEFT); if (S.invite) banner('The host left. Reload the page to try again.', 0); }
    }

    function joinCancel() {
        if (S.role !== 'guest' || S.session) return;
        if (S.peer) S.peer.link.close(true);
        reset();
        ui(UI.IDLE);
        banner(null);
    }

    function reset() {
        S.gen = (S.gen || 0) + 1;
        if (S.peer) { S.peer.link.close(true); S.peer = null; }
        if (S.net) { try { S.net.leave(); } catch (e) { /* gone */ } S.net = null; }
        S.role = null; S.phase = 'idle'; S.code = null; S.hs = null; S.pendingSid = null;
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
            }
        }
    }
    setInterval(function () {
        if (S.role === 'host' && S.peer && S.peer.link.dead()) guestGone('stopped answering');
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
        if (S.net) { try { S.net.leave(); } catch (e) { /* gone */ } }
    });

    /* --- invite links ------------------------------------------------------------ */
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
    function stats() {
        var t = tel(), link = S.peer && S.peer.link, secs = S.session ? (nowMs() - S.startedAt) / 1000 : 0;
        var o = {
            role: S.role, phase: S.phase, code: S.code, session: S.session,
            path: link ? link.path() : null, dcOpen: !!(link && link.p2p), linkDecided: !!(link && link.decided),
            linkRttMs: link && link.rtt !== null ? Math.round(link.rtt) : null,
            peer: !!(S.peer && (S.role === 'guest' || S.peer.ready)),
            frame: up() ? M._PortFrameNumber() : 0, sessionSec: +secs.toFixed(1), startFrame: S.startFrame,
            knobs: KNOB, net: Object.assign({}, S.stats), reanchors: S.reanchors, banner: S.banner,
            screen: up() ? M._PortOuiScreen() : 0,
        };
        if (t) {
            var mins = Math.max(secs / 60, 1e-9);
            o.sealedUpTo = t.sealedUpTo; o.verifiedUpTo = t.verifiedUpTo; o.verified = t.verified;
            o.mismatches = t.mismatches; o.repairWanted = t.repairWanted; o.firstMismatch = t.firstMismatch; o.lastMismatch = t.lastMismatch; o.historyExceeded = t.historyExceeded;
            o.rewinds = t.rewinds; o.rewindsPerMin = +(t.rewinds / mins).toFixed(1);
            o.replayedFrames = t.replayedFrames; o.imagesLoaded = t.imagesLoaded;
            o.repairs = S.role === 'guest' ? Math.max(0, t.imagesLoaded - 1) : Math.max(0, S.stats.imagesSent - S.stats.joins);
            o.ownOverridden = t.ownOverridden; o.sealedRecv = t.sealedRecv;
            o.ownOverriddenPct = +(100 * t.ownOverridden / Math.max(1, t.sealedRecv)).toFixed(2);
            o.hostKeyLatePct = +(100 * t.hostKeyMissing / Math.max(1, t.hostSealed)).toFixed(2);
            o.soundsMissed = t.soundsMissed; o.soundsSpurious = t.soundsSpurious;
        }
        if (S.gs) {
            var g = S.gs;
            o.lead = g.lastLead; o.margin = g.margin; o.rttMs = g.rtt === null ? null : +g.rtt.toFixed(1);
            o.holds = g.st.holds; o.skips = g.st.skips; o.bursts = g.st.bursts; o.capHolds = g.st.capHolds;
            o.repairReqs = g.st.repairReqs; o.joined = g.joined; o.lastImageMs = S.lastImageMs;
        }
        if (S.hs) {
            o.hostImages = S.hs.st.images; o.repairReqs = S.hs.st.repairReqs; o.inputsIn = S.hs.st.inputsIn;
            o.lastImageBytes = S.hs.st.imageComp;
        }
        return o;
    }
    function statsText() {
        if (!S.role) return '';
        var o = stats(), L = [];
        L.push((o.role === 'host' ? 'Hosting' : 'Joined') + ' game ' + (o.code || '-') + ', ' + o.phase +
               (o.peer ? '' : ' (no player 2)'));
        L.push('Path: ' + (o.path === 'p2p' ? 'direct (WebRTC)' : o.path ? 'relay (room server)' : '-') +
               (o.linkRttMs !== null ? ', link round trip ' + o.linkRttMs + ' ms' : ''));
        if (o.session) {
            if (o.role === 'guest')
                L.push('Round trip ' + (o.rttMs === null ? '-' : o.rttMs + ' ms') + ', lead ' + o.lead + ' frames' +
                       ', holds ' + o.holds + ' skips ' + o.skips + ' bursts ' + o.bursts);
            L.push('Rewinds ' + o.rewinds + ' (' + o.rewindsPerMin + '/min), repairs ' + o.repairs +
                   ', mismatches ' + o.mismatches + (o.repairWanted ? ' (repair pending)' : ''));
            if (o.role === 'guest') L.push('Own input overridden ' + o.ownOverriddenPct + '%');
            else L.push('Player 2 key late at the latch ' + o.hostKeyLatePct + '%');
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

    window.katamOnline = {
        stats: stats, statsText: statsText, hashes: hashes, regions: regions,
        inSession: function () { return S.session; },
        /* tests and the console */
        host: hostOpen, join: function (c) { guestJoin(Net.normalizeCode(c) || c, false); },
        invite: function () { return S.code ? inviteLink() : null; },
        _S: S,
    };
}());
