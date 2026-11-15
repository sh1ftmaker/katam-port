/* The online session's logic for each side, independent of what carries the
 * bytes -- one file, loaded by the page (web/online.js, a <script>) and by
 * the node proof rig (tools/netproof/session.mjs, require), so the rig keeps
 * testing what ships.
 *
 * A transport is { datagram(bytes), reliable(msg, size) } plus the two
 * receive entry points below; a clock is a function returning this device's
 * own time in ms.  tools/netproof/link.mjs is one transport (a simulated
 * link); web/online.js's WebRTC data channels (or the relay) are another.
 *
 * The engine (platform/lmpnet.c) does the sealing, prediction, rewinding,
 * keyframes, hashing and verification.  This file does what needs a wire:
 * redundancy and acknowledgement of inputs and sealed frames, time sync (the
 * guest's lead over the host), repair requests and image transfer.
 *
 * Images are compressed with opts.codec = { deflate(bytes), inflate(bytes) }
 * (raw deflate).  Node passes zlib's synchronous calls; a page can hand the
 * guest an already inflated image as msg.raw and give the host a deflate that
 * returns a Promise (CompressionStream): the send then happens when it
 * resolves.  No Buffer, no zlib, no node API in here. */
(function (root, factory) {
    'use strict';
    if (typeof module === 'object' && module.exports)
        module.exports = factory();
    else
        root.KatamLnSession = factory();
}(typeof self !== 'undefined' ? self : this, function () {
    'use strict';

    var PERIOD = 1000 / 59.7275;
    var T_INPUT = 0x31, T_SEALED = 0x32;
    var now = (typeof performance !== 'undefined' && performance.now)
        ? function () { return performance.now(); } : function () { return Date.now(); };

    function dv(b) { return new DataView(b.buffer, b.byteOffset, b.byteLength); }

    // --- wire -----------------------------------------------------------------
    function encInput(g, keys, ack, ts) {
        var b = new Uint8Array(14 + 2 * keys.length), d = dv(b);
        b[0] = T_INPUT; d.setUint32(1, g >>> 0, true); b[5] = keys.length;
        d.setUint32(6, ack >>> 0, true); d.setUint32(10, ts >>> 0, true);
        for (var i = 0; i < keys.length; i++) d.setUint16(14 + 2 * i, keys[i], true);
        return b;
    }
    function decInput(b) {
        var d = dv(b), n = b[5], keys = [];
        for (var i = 0; i < n; i++) keys.push(d.getUint16(14 + 2 * i, true));
        return { g: d.getUint32(1, true), keys: keys, ack: d.getUint32(6, true), ts: d.getUint32(10, true) };
    }
    function encSealed(P, first, recs, guestContig, marginMin, echoTs, holdMs) {
        var n = 23, r, i, k;
        for (i = 0; i < recs.length; i++) n += 5 + (recs[i].hashed ? 8 : 0) + recs[i].ev.length * 17;
        var b = new Uint8Array(n), d = dv(b);
        b[0] = T_SEALED; d.setUint32(1, P >>> 0, true); d.setUint32(5, first >>> 0, true);
        d.setUint16(9, recs.length, true);
        d.setUint32(11, guestContig >>> 0, true);
        d.setInt16(15, Math.max(-32768, Math.min(32767, marginMin)), true);
        d.setUint32(17, echoTs >>> 0, true);
        d.setUint16(21, Math.min(65535, Math.round(holdMs * 10)), true);
        var o = 23;
        for (i = 0; i < recs.length; i++) {
            r = recs[i];
            d.setUint16(o, r.k0, true); d.setUint16(o + 2, r.k1, true);
            b[o + 4] = (r.hashed ? 1 : 0) | (r.ev.length << 1); o += 5;
            if (r.hashed) { d.setUint32(o, r.hlo >>> 0, true); d.setUint32(o + 4, r.hhi >>> 0, true); o += 8; }
            for (k = 0; k < r.ev.length; k++) {
                var e = r.ev[k];
                b[o] = e[0];
                for (var j = 1; j < 5; j++) d.setInt32(o + 1 + 4 * (j - 1), e[j] | 0, true);
                o += 17;
            }
        }
        return b;
    }
    function decSealed(b) {
        var d = dv(b), n = d.getUint16(9, true), recs = [], o = 23;
        for (var i = 0; i < n; i++) {
            var r = { k0: d.getUint16(o, true), k1: d.getUint16(o + 2, true), hashed: b[o + 4] & 1, ev: [] };
            var nev = b[o + 4] >> 1; o += 5;
            if (r.hashed) { r.hlo = d.getUint32(o, true); r.hhi = d.getUint32(o + 4, true); o += 8; }
            for (var k = 0; k < nev; k++) {
                var e = [b[o]];
                for (var j = 0; j < 4; j++) e.push(d.getInt32(o + 1 + 4 * j, true));
                r.ev.push(e); o += 17;
            }
            recs.push(r);
        }
        return { P: d.getUint32(1, true), first: d.getUint32(5, true), recs: recs,
                 guestContig: d.getUint32(11, true), marginMin: d.getInt16(15, true),
                 echoTs: d.getUint32(17, true), holdMs: d.getUint16(21, true) / 10 };
    }

    function assign(dst) {
        for (var a = 1; a < arguments.length; a++) {
            var s = arguments[a];
            if (s) for (var k in s) if (Object.prototype.hasOwnProperty.call(s, k)) dst[k] = s[k];
        }
        return dst;
    }

    // --- host -------------------------------------------------------------------
    function HostSession(eng, tx, clock, opts) {
        this.e = eng; this.tx = tx; this.clock = clock;
        this.o = assign({ maxPerPacket: 64, resendWindow: 1024, codec: null }, opts || {});
        this.start = eng.frame();
        this.guestAck = this.start;       // guest has every sealed frame below this
        this.guestHigh = -1;              // highest guest input frame seen
        this.guestContig = 0;             // guest inputs contiguous below this
        this.seen = new Set();
        this.margins = [];                // {t, m}: first arrival, frames before the latch
        this.lastTs = 0; this.lastTsAt = 0;
        this.imageWanted = 0; this.imageReason = ''; this.imgInFlight = null;
        this.st = { packets: 0, recs: 0, images: 0, imageRaw: 0, imageComp: 0, compressMs: 0, repairReqs: 0,
                    viewReqs: 0, inputsIn: 0, inputsUseless: 0 };
        this.started = false;
    }
    HostSession.prototype.onDatagram = function (buf) {
        if (buf[0] !== T_INPUT) return;
        var t = this.clock(), m = decInput(buf), next = this.e.frame();
        var first = m.g - m.keys.length + 1;
        this.st.inputsIn++;
        for (var i = 0; i < m.keys.length; i++) {
            var f = first + i;
            if (f < this.start) continue;
            if (!this.seen.has(f)) {
                this.seen.add(f);
                this.margins.push({ t: t, m: f - next });
                if (f < next) this.st.inputsUseless++;
            }
            this.e.remoteInput(f, m.keys[i]);
        }
        if (m.g > this.guestHigh) this.guestHigh = m.g;
        while (this.seen.has(this.guestContig) || this.guestContig < this.start) {
            this.seen.delete(this.guestContig - 2048);
            this.guestContig++;
        }
        if (m.ack > this.guestAck) this.guestAck = m.ack;
        if (this.imgInFlight !== null && m.ack >= this.imgInFlight) this.imgInFlight = null;
        this.lastTs = m.ts; this.lastTsAt = t;
    };
    HostSession.prototype.onReliable = function (msg) {
        if (msg.t === 'repair') {
            this.st.repairReqs++;
            if (this.imgInFlight === null || msg.frame >= this.imgInFlight) this.wantImage('repair:' + msg.reason);
            else this.st.repairDup = (this.st.repairDup || 0) + 1;
        } else if (msg.t === 'viewreq') { this.st.viewReqs++; this.e.event(2, msg.w, msg.h, 0, 0); }
    };
    HostSession.prototype.wantImage = function (reason) {
        if (!this.imageWanted) { this.imageWanted = 1; this.imageReason = reason; this.e.imageRequest(); }
    };
    // Before each frame the host simulates: its own pad for that frame.
    HostSession.prototype.beforeFrame = function (f, keys) { this.e.localInput(f, keys); };
    // After each tick (one or more frames): images, then the sealed stream.
    HostSession.prototype.afterTick = function () {
        var t = this.clock(), P = this.e.frame(), self = this;
        if (this.imageWanted) {
            var img = this.e.image();
            if (img) {
                var t0 = now();
                var c = this.o.codec.deflate(img.bytes);
                var reason = this.imageReason;
                var send = function (comp) {
                    self.st.imageComp = comp.length;
                    self.tx.reliable({ t: 'image', F: img.frame, raw: img.bytes.length, comp: comp, reason: reason },
                                     comp.length + 32);
                };
                this.st.images++; this.st.imageRaw = img.bytes.length;
                if (c && typeof c.then === 'function') c.then(function (comp) {
                    self.st.compressMs += now() - t0;
                    send(comp);
                });
                else { this.st.compressMs += now() - t0; send(c); }
                this.imageWanted = 0;
                this.margins = [];      // lateness from before the image says nothing about after it
                // what the guest holds from F on is the stream: resend from F
                this.guestAck = img.frame;
                this.imgInFlight = img.frame;      // until the guest acks past it
                this.imagesSent = (this.imagesSent || 0) + 1;
            }
        }
        // A guest so far behind that its next sealed frame is no longer held
        // gets the image instead (history exceeded on the host's side).
        if (this.imagesSent && this.imgInFlight === null && this.guestAck + this.o.resendWindow < P && !this.imageWanted) {
            this.st.behindImages = (this.st.behindImages || 0) + 1;
            this.wantImage('behind');
        }
        while (this.margins.length && this.margins[0].t < t - 1000) this.margins.shift();
        var mmin = 127;
        if (this.margins.length) {
            mmin = Infinity;
            for (var i = 0; i < this.margins.length; i++) if (this.margins[i].m < mmin) mmin = this.margins[i].m;
        }
        var first = Math.max(this.guestAck, P - this.o.resendWindow, this.start);
        var recs = [];
        for (var f = first; f < P && recs.length < this.o.maxPerPacket; f++) {
            var r = this.e.sealedGet(f);
            if (!r) { if (!recs.length) first = f + 1; else break; continue; }
            recs.push(r);
        }
        var pkt = encSealed(P, first, recs, this.guestContig, mmin, this.lastTs, t - this.lastTsAt);
        this.st.packets++; this.st.recs += recs.length;
        this.tx.datagram(pkt);
    };

    // --- guest --------------------------------------------------------------------
    function GuestSession(eng, tx, clock, opts) {
        this.e = eng; this.tx = tx; this.clock = clock;
        this.o = assign({ redundancy: 32, marginInit: 2, marginMin: 1, marginMax: 60,
                          maxExtrap: 15, burstMax: 1200, lockstep: false, leadCap: null, codec: null }, opts || {});
        this.margin = this.o.marginInit;
        this.hostP = -1; this.recvAt = 0; this.hostAck = 0;
        this.rtt = null; this.rtts = [];
        this.lastAdj = -1e9; this.goodSince = null;
        this.settling = false; this.settleUntil = -1e9; this.heldLast = false;
        this.repairInFlight = 0; this.repairAt = 0;
        this.joined = false; this.lastLocal = -1; this.keyRing = new Uint16Array(4096); this.offs = [];
        this.st = { packets: 0, recs: 0, holds: 0, skips: 0, skipFrames: 0, bursts: 0, burstFrames: 0, capHolds: 0,
                    lockHolds: 0, images: 0, imageApplyMs: 0, imageBytes: 0, repairReqs: 0, marginUps: 0, marginDowns: 0,
                    leadSamples: [], marginSamples: [], firstImageAt: null };
        // the page keeps no unbounded sample arrays (opts.samples = false)
        this.samples = this.o.samples !== false;
        this.lastLead = 0;
    }
    GuestSession.prototype.onDatagram = function (buf) {
        if (buf[0] !== T_SEALED) return;
        var t = this.clock(), m = decSealed(buf), i;
        this.st.packets++;
        for (i = 0; i < m.recs.length; i++) {
            var f = m.first + i, r = m.recs[i];
            for (var k = 0; k < r.ev.length; k++) this.e.sealedEvent(f, r.ev[k]);
            this.e.sealed(f, r.k0, r.k1, r.ev.length);
            if (r.hashed) this.e.remoteHash(f, r.hlo, r.hhi);
        }
        this.st.recs += m.recs.length;
        if (m.P > this.hostP) { this.hostP = m.P; this.recvAt = t; }
        // host clock offset seen by this packet; the least-delayed packet of
        // the last second is the best estimate (jitter only ever delays)
        this.offs.push({ t: t, o: m.P - t / PERIOD });
        while (this.offs.length && this.offs[0].t < t - 1000) this.offs.shift();
        if (m.guestContig > this.hostAck) this.hostAck = m.guestContig;
        if (m.echoTs) {
            var rtt = t - m.echoTs / 10 - m.holdMs;
            if (rtt >= 0 && rtt < 5000) {
                this.rtts.push(rtt); if (this.rtts.length > 64) this.rtts.shift();
                this.rtt = Math.min.apply(null, this.rtts.slice(-16));     // min filter: queueing noise out
            }
        }
        this.adapt(t, m.marginMin);
    };
    // Margin (frames of slack the guest's inputs should arrive with): up at
    // once when the host saw late inputs, down when it has slack -- by half
    // the spare slack at a time, so an inflated margin is gone in a few
    // seconds instead of one frame a second.
    //
    // Not while settling after an image: the inputs a guest sends while it is
    // still catching up to the host are late by construction, and the host
    // reports the worst of its last second.  Counting those raised the margin
    // by about 16 frames at every join, which then took 15 s to decay.
    GuestSession.prototype.adapt = function (t, mmin) {
        if (mmin === 127) return;
        if (mmin < this.o.marginMin) {
            if (this.settling || t < this.settleUntil) return;
            if (t - this.lastAdj > 250) {
                this.margin = Math.min(this.o.marginMax, this.margin + Math.min(4, this.o.marginMin - mmin));
                this.lastAdj = t; this.goodSince = null; this.st.marginUps++;
            }
        } else if (mmin > this.o.marginMin + 2) {
            if (this.goodSince === null) this.goodSince = t;
            if (t - this.goodSince > 1000 && t - this.lastAdj > 500 && this.margin > this.o.marginInit) {
                var step = Math.max(1, Math.floor((mmin - (this.o.marginMin + 2)) / 2));
                this.margin = Math.max(this.o.marginInit, this.margin - step);
                this.lastAdj = t; this.goodSince = t; this.st.marginDowns++;
            }
        } else this.goodSince = null;
    };
    // An image: msg.raw (bytes, already inflated) or msg.comp (raw deflate).
    GuestSession.prototype.onReliable = function (msg) {
        if (msg.t !== 'image') return;
        var t0 = now();
        var raw = msg.raw && msg.raw.length !== undefined ? msg.raw : this.o.codec.inflate(msg.comp);
        this.e.loadImage(raw);
        this.st.imageApplyMs += now() - t0;
        this.st.images++; this.st.imageBytes = msg.comp ? msg.comp.length : raw.length;
        this.repairInFlight = 0;
        this.joined = true;
        this.imagePending = true;   // applied at the next frame top: run exactly one frame first
        this.settling = true;       // catching up to the host: lateness now is not the link's
        if (this.st.firstImageAt === null) this.st.firstImageAt = this.clock();
    };
    // How many frames to simulate on this vsync.
    GuestSession.prototype.framesToRun = function (nominal) {
        if (!this.joined) return 0;
        if (this.imagePending) { this.imagePending = false; return 1; }
        var t = this.clock(), P = this.e.frame(), tel = this.e.tel();
        if (this.hostP < 0) return 0;
        // the host's frame now: the best offset of the last second, but never
        // extrapolated more than maxExtrap frames past the last packet (a
        // silent host -- outage, freeze -- is not assumed to be running)
        var best = -Infinity;
        for (var i = 0; i < this.offs.length; i++) if (this.offs[i].o > best) best = this.offs[i].o;
        if (!this.offs.length) best = this.hostP - this.recvAt / PERIOD;
        var hostNow = Math.min(best + t / PERIOD, this.hostP + this.o.maxExtrap);
        var rttF = (this.rtt === null ? 0 : this.rtt) / PERIOD;
        var target = hostNow + rttF + this.margin;
        var diff = target - (P + nominal - 1);
        var n = nominal;
        if (diff > 8) { n = Math.min(Math.ceil(diff) + nominal - 1, this.o.burstMax); this.st.bursts++; this.st.burstFrames += n - nominal; }
        else if (diff >= 2.5) { n = nominal + 1; this.st.skips++; this.st.skipFrames++; }
        else if (diff <= -2.5) {
            // a little ahead: half speed (every other vsync) rather than a
            // freeze; far ahead (the host stalled): stop
            if (nominal > 1 || diff <= -6 || !this.heldLast) { n = nominal - 1; this.st.holds++; this.heldLast = true; }
            else this.heldLast = false;
        }
        if (diff > -2.5) this.heldLast = false;
        // caught up after an image: the host's report still holds that second
        if (this.settling && diff > -2.5 && diff < 2.5) {
            this.settling = false;
            this.settleUntil = t + 1500 + (this.rtt === null ? 0 : this.rtt);
        }
        // never predict further than the history can rewind
        var cap = this.o.leadCap === null || this.o.leadCap === undefined ? 120 : this.o.leadCap;
        var lead = (P + n) - tel.sealedUpTo;
        if (lead > cap) { var m2 = Math.max(0, n - (lead - cap)); if (m2 < n) this.st.capHolds++; n = m2; }
        if (this.o.lockstep) { var m3 = Math.max(0, Math.min(n, tel.sealedUpTo - P)); if (m3 < n) this.st.lockHolds++; n = m3; }
        this.lastLead = P - tel.sealedUpTo;
        if (this.samples) {
            this.st.leadSamples.push(P - tel.sealedUpTo);
            this.st.marginSamples.push(this.margin);
        }
        return Math.max(0, n);
    };
    GuestSession.prototype.beforeFrame = function (f, keys) {
        this.e.localInput(f, keys); this.lastLocal = f; this.keyRing[f & 4095] = keys;
    };
    GuestSession.prototype.afterTick = function () {
        var t = this.clock(), tel = this.e.tel();
        if (tel.repairWanted && (!this.repairInFlight || t - this.repairAt > 3000)) {
            this.repairInFlight = 1; this.repairAt = t; this.st.repairReqs++;
            this.tx.reliable({ t: 'repair', reason: tel.mismatches ? 'mismatch' : 'history', frame: tel.repairFrame }, 16);
        }
        if (this.lastLocal < 0) return;
        var g = this.lastLocal;
        var from = Math.max(this.hostAck, g - this.o.redundancy + 1, 0);
        var keys = [];
        for (var f = from; f <= g; f++) keys.push(this.keyAt(f));
        if (!keys.length) return;
        this.tx.datagram(encInput(g, keys, tel.sealedUpTo, Math.round(t * 10)));
    };
    GuestSession.prototype.keyAt = function (f) { return this.keyRing[f & 4095]; };

    // --- an engine adapter over a running module (the page) ----------------------
    // tools/netproof/instance.mjs has the rig's own; this is the same calls.
    function cstr(M, p) {
        var s = '';
        for (var i = p; M.HEAPU8[i]; i++) s += String.fromCharCode(M.HEAPU8[i]);
        return s;
    }
    function engineOfModule(M) {
        var scratch = M._malloc(512);
        var names = null;
        function u32(i) { return M.HEAPU32[(scratch >> 2) + i]; }
        function s32(i) { return M.HEAP32[(scratch >> 2) + i]; }
        var eng = {
            frame: function () { return M._PortFrameNumber(); },
            localInput: function (f, k) { M._PortLnLocalInput(f, k); },
            remoteInput: function (f, k) { M._PortLnRemoteInput(f, k); },
            sealedGet: function (f) {
                if (!M._PortLnSealedGet(f, scratch)) return null;
                var rec = { k0: u32(0), k1: u32(1), nev: u32(2), ontime: u32(3), hlo: u32(4), hhi: u32(5),
                            hashed: u32(6), ev: [] };
                for (var i = 0; i < rec.nev; i++) {
                    M._PortLnSealedEventGet(f, i, scratch);
                    rec.ev.push([s32(0), s32(1), s32(2), s32(3), s32(4)]);
                }
                return rec;
            },
            sealedEvent: function (f, e) { M._PortLnSealedEvent(f, e[0], e[1], e[2], e[3], e[4]); },
            sealed: function (f, k0, k1, nev) { M._PortLnSealed(f, k0, k1, nev); },
            remoteHash: function (f, lo, hi) { M._PortLnRemoteHash(f, lo, hi); },
            event: function (t, a, b, c, d) { return M._PortLnEvent(t, a, b, c, d); },
            tel: function () {
                if (!names) names = cstr(M, M._PortLnTelemetryNames()).split(',').filter(Boolean);
                var p = M._PortLnTelemetry() >> 2, o = {};
                for (var i = 0; i < names.length; i++) o[names[i]] = M.HEAPU32[p + i];
                return o;
            },
            imageRequest: function () { return M._PortLnImageRequest(); },
            image: function () {
                var r = M._PortLnImageReady();
                if (!r) return null;
                var p = M._PortLnImagePtr(), n = M._PortLnImageBytes();
                return { frame: r - 1, bytes: M.HEAPU8.slice(p, p + n) };
            },
            loadImage: function (bytes) {
                var p = M._PortLnImageInBuffer();
                if (!p) return 0;
                M.HEAPU8.set(bytes, p);
                return M._PortLnImageLoad(bytes.length);
            },
        };
        return eng;
    }

    return {
        PERIOD: PERIOD, T_INPUT: T_INPUT, T_SEALED: T_SEALED,
        HostSession: HostSession, GuestSession: GuestSession,
        encInput: encInput, decInput: decInput, encSealed: encSealed, decSealed: decSealed,
        engineOfModule: engineOfModule,
    };
}));
