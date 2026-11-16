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
    // Input (guest -> host): its keys for frames g-n+1..g, for each local seat
    // in `mask` (bit per seat, seat 0 = the console's own keys), frame-major.
    //   u8 T_INPUT, u32 g, u8 n, u32 ack, u32 ts, u8 mask, n x popcount(mask) x u16
    // `frames` is an array of key arrays indexed by seat (a plain number is
    // seat 0's key, mask 1).
    function popcount(m) { var n = 0; for (; m; m &= m - 1) n++; return n; }
    function encInput(g, frames, ack, ts, mask) {
        mask = mask === undefined ? 1 : mask & 15;
        var per = popcount(mask), b = new Uint8Array(15 + 2 * per * frames.length), d = dv(b), o = 15;
        b[0] = T_INPUT; d.setUint32(1, g >>> 0, true); b[5] = frames.length;
        d.setUint32(6, ack >>> 0, true); d.setUint32(10, ts >>> 0, true); b[14] = mask;
        for (var i = 0; i < frames.length; i++) {
            var fr = frames[i];
            for (var s = 0; s < 4; s++) {
                if (!(mask & (1 << s))) continue;
                d.setUint16(o, (typeof fr === 'number' ? (s === 0 ? fr : 0) : (fr[s] || 0)) & 0x3FF, true); o += 2;
            }
        }
        return b;
    }
    function decInput(b) {
        var d = dv(b), n = b[5], mask = b[14], frames = [], o = 15;
        for (var i = 0; i < n; i++) {
            var fr = [0, 0, 0, 0];
            for (var s = 0; s < 4; s++) if (mask & (1 << s)) { fr[s] = d.getUint16(o, true); o += 2; }
            frames.push(fr);
        }
        return { g: d.getUint32(1, true), mask: mask, keys: frames, ack: d.getUint32(6, true), ts: d.getUint32(10, true) };
    }
    // Sealed (host -> one guest): 23-byte header (frontier P, first frame,
    // count, that guest's contiguous inputs, its lateness, the echo), then per
    // frame: u8 flags (bits 0-3 slots 0..3 whose keys follow -- a slot not
    // listed has none pressed, so a CPU slot or an idle player costs nothing;
    // bit 4 hashed; bit 5 events follow; bit 6 a mask byte for slots 4..11
    // follows; bit 7 one for slots 12..19), [u8 nev], [u8 mask 4..11],
    // [u8 mask 12..19], u16 per listed slot in slot order, [u32 lo, u32 hi],
    // nev x 17 bytes.  Two idle players: 1 byte a frame.  A frame whose slots
    // 4.. are idle (always so on the 4-Kirby module, which has none) is the
    // same bytes as before those bits had a meaning.
    var XSLOTS = 20;          // what the two mask bytes reach (slots 0..19)
    function recKeys(r) { return r.k ? r.k : [r.k0 | 0, r.k1 | 0, r.k2 | 0, r.k3 | 0]; }
    // one record's bytes on the wire
    function recBytes(r) {
        var kk = recKeys(r), n = 1 + (r.ev.length ? 1 : 0) + (r.hashed ? 8 : 0) + r.ev.length * 17, x1 = 0, x2 = 0;
        for (var s = 0; s < kk.length && s < XSLOTS; s++) if (kk[s]) { n += 2; if (s >= 12) x2 = 1; else if (s >= 4) x1 = 1; }
        return n + x1 + x2;
    }
    function encSealed(P, first, recs, guestContig, marginMin, echoTs, holdMs) {
        var n = 23, r, i, k, s;
        for (i = 0; i < recs.length; i++) n += recBytes(recs[i]);
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
            var keys = recKeys(r), m = 0, x1 = 0, x2 = 0, ns = Math.min(keys.length, XSLOTS);
            for (s = 0; s < ns; s++) if (keys[s]) {
                if (s < 4) m |= 1 << s; else if (s < 12) x1 |= 1 << (s - 4); else x2 |= 1 << (s - 12);
            }
            b[o++] = m | (r.hashed ? 16 : 0) | (r.ev.length ? 32 : 0) | (x1 ? 64 : 0) | (x2 ? 128 : 0);
            if (r.ev.length) b[o++] = r.ev.length;
            if (x1) b[o++] = x1;
            if (x2) b[o++] = x2;
            for (s = 0; s < ns; s++) if (keys[s]) { d.setUint16(o, keys[s], true); o += 2; }
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
            var fl = b[o++], nev = fl & 32 ? b[o++] : 0, kk = [0, 0, 0, 0];
            var x1 = fl & 64 ? b[o++] : 0, x2 = fl & 128 ? b[o++] : 0, s;
            var mask = (fl & 15) | (x1 << 4) | (x2 << 12);
            for (s = 0; s < XSLOTS; s++) if (mask & (1 << s)) {
                while (kk.length <= s) kk.push(0);
                kk[s] = d.getUint16(o, true); o += 2;
            }
            var r = { k0: kk[0], k1: kk[1], k2: kk[2], k3: kk[3], hashed: fl & 16 ? 1 : 0, ev: [] };
            if (kk.length > 4) r.k = kk;
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
    // One per guest device (opts.dev, 1 .. the engine's devices - 1): that
    // guest's inputs in, its sealed stream and images out.  Several share the
    // one engine (HostHub).
    //
    // Redundancy of the sealed stream.  opts.copies 0 (the default): every
    // record from the guest's ack on goes in every packet until the guest acks
    // it (up to maxPerPacket) -- what the 4-Kirby module has always done.
    // opts.copies k: each record goes in k consecutive packets and then stops;
    // the guest's ack (its contiguous sealed frontier, in every input packet)
    // is the negative acknowledgement: when the record at the ack has had its k
    // copies and is still not acked one retransmit timeout after it was last
    // sent, everything from the ack is sent again (once, then the timeout
    // doubles until the ack moves).  The timeout follows the ack's round trip
    // (srtt + 4 rttvar + 2 frames, 60..1000 ms; Karn: retransmitted records
    // give no sample).  Until the guest has acked its join or repair image
    // the old rule holds (it may drop records while it inflates the image).
    // opts.maxPacketBytes: no packet larger (records are not split), so a
    // datagram stays under the data channel's fragmentation size.
    var RING = 4096, RTO_MIN = 60, RTO_MAX = 1000, RTO_INIT = 300;
    function HostSession(eng, tx, clock, opts) {
        this.e = eng; this.tx = tx; this.clock = clock;
        this.o = assign({ maxPerPacket: 64, resendWindow: 1024, codec: null, dev: 1, copies: 0, maxPacketBytes: 0 }, opts || {});
        if (this.o.copies) {
            this.sTag = new Int32Array(RING).fill(-1); this.sCnt = new Uint8Array(RING);
            this.sFirst = new Float64Array(RING); this.sLast = new Float64Array(RING);
            this.srtt = null; this.rttvar = 0; this.rtoBase = RTO_INIT; this.rto = RTO_INIT;
        }
        this.dev = this.o.dev;
        this.start = eng.frame();
        this.lastHeard = clock();
        this.guestAck = this.start;       // guest has every sealed frame below this
        this.guestHigh = -1;              // highest guest input frame seen
        this.guestContig = 0;             // guest inputs contiguous below this
        this.seen = new Set();
        this.margins = [];                // {t, m}: first arrival, frames before the latch
        this.lastTs = 0; this.lastTsAt = 0;
        this.imageWanted = 0; this.imageReason = ''; this.imgInFlight = null;
        this.st = { packets: 0, recs: 0, images: 0, imageRaw: 0, imageComp: 0, compressMs: 0, repairReqs: 0,
                    viewReqs: 0, inputsIn: 0, inputsUseless: 0,
                    bytesOut: 0, bytesIn: 0, maxPacket: 0, resends: 0, resentRecs: 0, rtoMs: 0, srttMs: 0 };
        this.started = false;
    }
    // the guest's ack moved to `ack`: a round-trip sample from the newest
    // record it covers (sent once only), and the timeout back to its base
    HostSession.prototype.ackMoved = function (ack, t) {
        var f = ack - 1, i = f & (RING - 1);
        if (this.sTag[i] === f && this.sCnt[i] <= this.o.copies) {
            var x = t - this.sFirst[i];
            if (this.srtt === null) { this.srtt = x; this.rttvar = x / 2; }
            else { this.rttvar = 0.75 * this.rttvar + 0.25 * Math.abs(this.srtt - x); this.srtt = 0.875 * this.srtt + 0.125 * x; }
            this.rtoBase = Math.max(RTO_MIN, Math.min(RTO_MAX, this.srtt + 4 * this.rttvar + 2 * PERIOD));
            this.st.srttMs = Math.round(this.srtt);
        }
        this.rto = this.rtoBase;
    };
    // bounded redundancy: the first frame this packet carries, from lo (the
    // guest's ack, or later)
    HostSession.prototype.boundedFirst = function (lo, P, t) {
        var k = this.o.copies, f = lo;
        while (f < P && this.sTag[f & (RING - 1)] === f && this.sCnt[f & (RING - 1)] >= k) f++;
        if (f === lo) return lo;
        // the record at the ack has had its copies: lost every time, or its
        // ack is still on the way.  One timeout after its last copy: again.
        if (!this.o.noRetransmit && t - this.sLast[lo & (RING - 1)] >= this.rto) {      // (noRetransmit: the rig's ablation)
            this.st.resends++; this.st.resentRecs += f - lo;
            this.rto = Math.min(RTO_MAX, this.rto * 2);
            return lo;
        }
        return f;
    };
    HostSession.prototype.noteSent = function (first, n, t) {
        for (var f = first; f < first + n; f++) {
            var i = f & (RING - 1);
            if (this.sTag[i] !== f) { this.sTag[i] = f; this.sCnt[i] = 0; this.sFirst[i] = t; }
            if (this.sCnt[i] < 255) this.sCnt[i]++;
            this.sLast[i] = t;
        }
    };
    HostSession.prototype.onDatagram = function (buf) {
        if (buf[0] !== T_INPUT) return;
        var t = this.clock(), m = decInput(buf), next = this.e.frame();
        var first = m.g - m.keys.length + 1;
        this.st.inputsIn++;
        this.st.bytesIn += buf.length;
        this.lastHeard = t;
        for (var i = 0; i < m.keys.length; i++) {
            var f = first + i;
            if (f < this.start) continue;
            if (!this.seen.has(f)) {
                this.seen.add(f);
                this.margins.push({ t: t, m: f - next });
                if (f < next) this.st.inputsUseless++;
            }
            for (var s = 0; s < 4; s++)
                if (m.mask & (1 << s)) this.e.remoteInput(f, m.keys[i][s], this.dev, s);
        }
        if (m.g > this.guestHigh) this.guestHigh = m.g;
        while (this.seen.has(this.guestContig) || this.guestContig < this.start) {
            this.seen.delete(this.guestContig - 2048);
            this.guestContig++;
        }
        if (m.ack > this.guestAck) {
            this.guestAck = m.ack;
            if (this.o.copies) this.ackMoved(m.ack, t);
        }
        if (this.imgInFlight !== null && m.ack >= this.imgInFlight) this.imgInFlight = null;
        this.lastTs = m.ts; this.lastTsAt = t;
    };
    HostSession.prototype.onReliable = function (msg) {
        if (msg.t === 'repair') {
            this.st.repairReqs++;
            if (this.imgInFlight === null || msg.frame >= this.imgInFlight) this.wantImage('repair:' + msg.reason);
            else this.st.repairDup = (this.st.repairDup || 0) + 1;
        } else if (msg.t === 'viewreq') { this.st.viewReqs++; this.e.event(2, msg.w, msg.h, 0, 0); }
        else if (msg.t === 'seat') {
            // a local join (or leave) on that guest: the host decides the slot
            this.st.seatReqs = (this.st.seatReqs || 0) + 1;
            var slot = this.e.seatRequest(this.dev, msg.seat | 0, msg.on ? 1 : 0);
            if (msg.on && slot < 0) { this.st.seatFull = (this.st.seatFull || 0) + 1; this.tx.reliable({ t: 'seatfull', seat: msg.seat | 0 }, 16); }
        } else if (msg.t === 'card') {
            // that device's seats' player cards (name, colour): the host seals
            // each slot's colour from its owner's card (platform/player_card.c)
            this.st.cards = (this.st.cards || 0) + 1;
            if (this.e.cardSet && typeof msg.p === 'string') this.e.cardSet(this.dev, msg.p);
        } else if (msg.t === 'pset') {
            // that device's settings, for the pause SETTINGS page: the host
            // seals them for the slots it plays (platform/lmpnet.c PSET)
            this.st.psets = (this.st.psets || 0) + 1;
            if (this.e.deviceSettings && typeof msg.v === 'number') this.e.deviceSettings(this.dev, msg.v >>> 0, (msg.x || 0) >>> 0);
        }
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
                    self.tx.reliable({ t: 'image', F: img.frame, raw: img.bytes.length, comp: comp, reason: reason, dev: self.dev },
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
        var bounded = this.o.copies && this.imgInFlight === null && this.imagesSent;
        if (bounded) first = this.boundedFirst(first, P, t);
        var recs = [], bytes = 23, cap = this.o.maxPacketBytes || Infinity;
        for (var f = first; f < P && recs.length < this.o.maxPerPacket; f++) {
            var r = this.e.sealedGet(f);
            if (!r) { if (!recs.length) first = f + 1; else break; continue; }
            if (cap !== Infinity) {
                var nb = recBytes(r);
                if (recs.length && bytes + nb > cap) break;
                bytes += nb;
            }
            recs.push(r);
        }
        if (this.o.copies) this.noteSent(first, recs.length, t);
        var pkt = encSealed(P, first, recs, this.guestContig, mmin, this.lastTs, t - this.lastTsAt);
        this.st.packets++; this.st.recs += recs.length;
        this.st.bytesOut += pkt.length;
        if (pkt.length > this.st.maxPacket) this.st.maxPacket = pkt.length;
        if (this.o.copies) this.st.rtoMs = Math.round(this.rto);
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
        this.joined = false; this.lastLocal = -1; this.keyRing = new Uint16Array(4 * 4096); this.offs = [];
        this.dev = null; this.onSeatFull = null;
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
        this.st.bytesIn = (this.st.bytesIn || 0) + buf.length;
        for (i = 0; i < m.recs.length; i++) {
            var f = m.first + i, r = m.recs[i];
            for (var k = 0; k < r.ev.length; k++) this.e.sealedEvent(f, r.ev[k]);
            // slots 4.. (a wide module): the N-wide call; else the 4-wide one
            if (this.e.sealedN) this.e.sealedN(f, recKeys(r), r.ev.length);
            else this.e.sealed(f, r.k0, r.k1, r.ev.length, r.k2, r.k3);
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
        if (msg.t === 'seatfull') {
            this.st.seatFull = (this.st.seatFull || 0) + 1;
            if (this.onSeatFull) this.onSeatFull(msg.seat);
            return;
        }
        if (msg.t === 'names') {
            // every slot's name, from the host (presentation only)
            this.st.names = (this.st.names || 0) + 1;
            if (this.e.setSlotNames && typeof msg.n === 'string') this.e.setSlotNames(msg.n);
            return;
        }
        if (msg.t !== 'image') return;
        var t0 = now();
        // the device id the host gave this guest, before its image lands
        if (msg.dev) { this.dev = msg.dev; this.e.setSelf(msg.dev); }
        var raw = msg.raw && msg.raw.length !== undefined ? msg.raw : this.o.codec.inflate(msg.comp);
        this.e.loadImage(raw);
        this.st.imageApplyMs += now() - t0;
        this.st.images++; this.st.imageBytes = msg.comp ? msg.comp.length : raw.length;
        this.repairInFlight = 0;
        this.joined = true;
        this.imagePending = true;   // applied at the next frame top: run exactly one frame first
        this.cardSent = null;       // the host may have forgotten this device's cards: again
        this.psetSent = null;       // and its settings
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
    // keys: this device's seat 0 key, or an array of its seats' keys
    GuestSession.prototype.beforeFrame = function (f, keys) {
        var o = (f & 4095) * 4;
        for (var s = 0; s < 4; s++) {
            var k = typeof keys === 'number' ? (s === 0 ? keys : 0) : (keys[s] || 0);
            if (s === 0 || typeof keys !== 'number') this.e.localInput(f, k, s);
            this.keyRing[o + s] = k;
        }
        this.lastLocal = f;
    };
    GuestSession.prototype.afterTick = function () {
        var t = this.clock(), tel = this.e.tel();
        if (tel.repairWanted && (!this.repairInFlight || t - this.repairAt > 3000)) {
            this.repairInFlight = 1; this.repairAt = t; this.st.repairReqs++;
            this.tx.reliable({ t: 'repair', reason: tel.mismatches ? 'mismatch' : 'history', frame: tel.repairFrame }, 16);
        }
        // this device's own seat requests (a local join or leave), to the host
        var a;
        while ((a = this.e.seatAskPoll ? this.e.seatAskPoll() : 0)) {
            this.st.seatAsks = (this.st.seatAsks || 0) + 1;
            this.tx.reliable({ t: 'seat', seat: (a - 1) & 3, on: (a - 1) >> 2 ? 1 : 0 }, 16);
        }
        // this device's player cards, to the host, whenever they change
        if (this.joined && this.e.cardProfiles) {
            var pr = this.e.cardProfiles();
            if (pr !== this.cardSent) {
                this.cardSent = pr;
                this.st.cardsSent = (this.st.cardsSent || 0) + 1;
                this.tx.reliable({ t: 'card', p: pr }, 16 + pr.length);
            }
        }
        // this device's settings (the pause SETTINGS page shows the pauser's
        // device's), to the host, whenever they change
        if (this.joined && this.e.settingsWord) {
            var sw = this.e.settingsWord();
            if (sw !== null && sw.k !== this.psetSent) {
                this.psetSent = sw.k;
                this.st.psetsSent = (this.st.psetsSent || 0) + 1;
                this.tx.reliable({ t: 'pset', v: sw.v, x: sw.x }, 32);
            }
        }
        if (this.lastLocal < 0) return;
        var g = this.lastLocal, mask = this.e.selfSeats ? this.e.selfSeats() & 15 : 1;
        var from = Math.max(this.hostAck, g - this.o.redundancy + 1, 0);
        var keys = [];
        for (var f = from; f <= g; f++) keys.push(this.keysAt(f));
        if (!keys.length) return;
        var pkt = encInput(g, keys, tel.sealedUpTo, Math.round(t * 10), mask | 1);
        this.st.bytesOut = (this.st.bytesOut || 0) + pkt.length;
        this.tx.datagram(pkt);
    };
    GuestSession.prototype.keyAt = function (f) { return this.keyRing[(f & 4095) * 4]; };
    GuestSession.prototype.keysAt = function (f) { var o = (f & 4095) * 4; return [this.keyRing[o], this.keyRing[o + 1], this.keyRing[o + 2], this.keyRing[o + 3]]; };

    // --- the host's side of several guests --------------------------------------
    // A star: one HostSession per guest device, the slots decided by the
    // engine (PortLnSeatRequest).  add() gives a connecting device its first
    // player's slot (the next free one, or the one it had) and a join image;
    // remove() hands its Kirbys to the CPU.  The caller says who is
    // connected; the hub keeps no timers.
    //
    // opts.seats: the people the room holds (4, or 8 on a wide module).  The
    // engine may seat a person in any slot below that, now that the wire
    // carries every slot's keys (PortLnSetSeatSlots); devices are 1 ..
    // min(seats, the engine's devices) - 1.
    function HostHub(eng, clock, opts) {
        this.e = eng; this.clock = clock; this.o = opts || {};
        this.peers = {};
        this.st = { adds: 0, removes: 0, full: 0, seatWaits: [] };
        var slots = eng.slots ? eng.slots() : 4;
        this.seats = Math.max(1, Math.min(this.o.seats || 4, slots));
        this.maxDev = Math.max(1, Math.min(this.seats, eng.maxDevices ? eng.maxDevices() : 4));
        if (eng.setSeatSlots) eng.setSeatSlots(this.seats);
        eng.seatRequest(0, 0, 1);          // the host's own player: slot 0; the host now manages the slots
    }
    HostHub.prototype.add = function (dev, tx, opts) {
        if (this.peers[dev]) this.remove(dev);
        var hs = new HostSession(this.e, tx, this.clock, assign({}, this.o, opts || {}, { dev: dev }));
        hs.slot = this.e.seatRequest(dev, 0, 1);
        hs.addedAt = this.clock(); hs.seatedAt = null;
        if (hs.slot < 0) this.st.full++;
        hs.wantImage('join');
        this.peers[dev] = hs;
        this.st.adds++;
        return hs;
    };
    HostHub.prototype.remove = function (dev) {
        if (!this.peers[dev]) return;
        delete this.peers[dev];
        this.e.deviceGone(dev);
        if (this.e.cardDrop) this.e.cardDrop(dev);
        this.st.removes++;
    };
    HostHub.prototype.get = function (dev) { return this.peers[dev] || null; };
    HostHub.prototype.devs = function () { return Object.keys(this.peers).map(Number).sort(); };
    HostHub.prototype.count = function () { return this.devs().length; };
    // a device id for a newcomer: `prefer` (a returning device) if free, else the lowest
    HostHub.prototype.freeDev = function (prefer) {
        var top = this.maxDev - 1;
        if (prefer >= 1 && prefer <= top && !this.peers[prefer]) return prefer;
        for (var d = 1; d <= top; d++) if (!this.peers[d]) return d;
        return -1;
    };
    // the host's own seats' keys for frame f (seat 0 = the console's)
    HostHub.prototype.beforeFrame = function (f, keys) {
        for (var s = 0; s < 4; s++) {
            var k = typeof keys === 'number' ? (s === 0 ? keys : 0) : (keys[s] || 0);
            if (s === 0 || typeof keys !== 'number') this.e.localInput(f, k, s);
        }
    };
    HostHub.prototype.afterTick = function () {
        var ds = this.devs();
        for (var i = 0; i < ds.length; i++) {
            var hs = this.peers[ds[i]];
            // a device that came in while every seat was a person's: a slot when one frees
            if (hs.slot < 0) hs.slot = this.e.freeSlots() > 0 ? this.e.seatRequest(hs.dev, 0, 1) : -1;
            // seat wait: from its arrival to its Kirby playing (a slot past the
            // live count waits for its Kirby to be made)
            if (hs.seatedAt === null && hs.slot >= 0 && this.e.slotOwner && this.e.slotOwner(hs.slot) === hs.dev * 4
                && (!this.e.kirbysNow || this.e.kirbysNow() > hs.slot)) {
                hs.seatedAt = this.clock();
                hs.st.seatWaitMs = Math.round(hs.seatedAt - hs.addedAt);
                hs.st.seatSlot = hs.slot;
                this.st.seatWaits.push({ dev: hs.dev, slot: hs.slot, ms: hs.st.seatWaitMs });
                if (this.st.seatWaits.length > 32) this.st.seatWaits.shift();
            }
            hs.afterTick();
        }
        // every slot's name to every guest, when it changes (the host names
        // the slots from the cards: platform/player_card.c)
        var names = this.e.slotNames ? this.e.slotNames() : null;
        if (names !== null)
            for (i = 0; i < ds.length; i++) {
                var p = this.peers[ds[i]];
                if (p.namesSent === names) continue;
                p.namesSent = names;
                p.tx.reliable({ t: 'names', n: names }, 16 + names.length);
            }
    };

    // --- an engine adapter over a running module (the page) ----------------------
    // tools/netproof/instance.mjs has the rig's own; this is the same calls.
    function cstr(M, p) {
        var s = '';
        for (var i = p; M.HEAPU8[i]; i++) s += String.fromCharCode(M.HEAPU8[i]);
        return s;
    }
    function putStr(M, buf, size, str) {
        var n = Math.min(str.length, size - 1);
        for (var i = 0; i < n; i++) M.HEAPU8[buf + i] = str.charCodeAt(i) & 0x7F;
        M.HEAPU8[buf + n] = 0;
        return buf;
    }
    // the player-card calls (platform/player_card.c), absent in an older build
    function cardCalls(M, eng) {
        if (!M._PortCardSeatProfiles) return eng;
        var sbuf = M._malloc(256);
        eng.cardProfiles = function () { return cstr(M, M._PortCardSeatProfiles()); };
        eng.cardSet = function (dev, p) { M._PortCardHostSetProfiles(dev, putStr(M, sbuf, 256, p)); };
        eng.cardDrop = function (dev) { M._PortCardHostDropDevice(dev); };
        eng.slotNames = function () { return cstr(M, M._PortCardSlotNames()); };
        eng.setSlotNames = function (n) { M._PortCardSetSlotNames(putStr(M, sbuf, 256, n)); };
        return settingsCalls(M, eng);
    }
    // the pause SETTINGS page's per-device copies (platform/menu_poc.c,
    // platform/lmpnet.c), absent in an older build or with the menus off
    function settingsCalls(M, eng) {
        if (!M._PortPocSettingsWord || !M._PortLnDeviceSettings) return eng;
        eng.settingsWord = function () {
            if (!(M._PortPocMenusOn && M._PortPocMenusOn())) return null;
            var v = M._PortPocSettingsWord() >>> 0, x = M._PortPocSettingsExtWord ? M._PortPocSettingsExtWord() >>> 0 : 0;
            return { v: v, x: x, k: v + ':' + x };
        };
        eng.deviceSettings = function (dev, v, x) { M._PortLnDeviceSettings(dev, v >>> 0, (x || 0) >>> 0); };
        return eng;
    }
    // The N-wide calls (docs/design/EIGHT-SEATS-ENGINE.md), on a module that
    // has them: its slots and devices, the seats the host may give, and -- on
    // a module with more than 4 Kirby slots -- the sealed record of every slot
    // (PortLnSealedGetN / PortLnSealedN).  The 4-Kirby module keeps the
    // 4-wide calls (sealedGet / sealed) exactly as before.
    function wideCalls(M, eng) {
        if (!M._PortLnSlots) return eng;
        var n = M._PortLnSlots();
        eng.slots = function () { return n; };
        eng.maxDevices = function () { return M._PortLnMaxDevices ? M._PortLnMaxDevices() : 4; };
        // the Kirbys in play (a seat past them waits for its Kirby to be made)
        if (M._PortPocKirbysNow) eng.kirbysNow = function () { return M._PortPocKirbysNow(); };
        if (M._PortLnSetSeatSlots) eng.setSeatSlots = function (k) { return M._PortLnSetSeatSlots(k); };
        if (n <= 4 || !M._PortLnSealedGetN || !M._PortLnSealedN) return eng;
        var wb = M._malloc(4 * (12 + n)), kb = M._malloc(2 * n);
        eng.sealedGet = function (f) {
            if (!M._PortLnSealedGetN(f, wb)) return null;
            var w = wb >> 2, U = M.HEAPU32, k = [U[w], U[w + 1], U[w + 7], U[w + 8]];
            for (var i = 4; i < n; i++) k.push(U[w + 10 + i - 4]);
            var rec = { k0: k[0], k1: k[1], nev: U[w + 2], ontime: U[w + 3], hlo: U[w + 4], hhi: U[w + 5],
                        hashed: U[w + 6], k2: k[2], k3: k[3], k: k, ev: [] };
            for (i = 0; i < rec.nev; i++) {
                M._PortLnSealedEventGet(f, i, wb);
                rec.ev.push([M.HEAP32[w], M.HEAP32[w + 1], M.HEAP32[w + 2], M.HEAP32[w + 3], M.HEAP32[w + 4]]);
            }
            return rec;
        };
        eng.sealedN = function (f, keys, nev) {
            var h = kb >> 1, m = Math.min(keys.length, n), H = M.HEAPU16 || new Uint16Array(M.HEAPU8.buffer);
            for (var i = 0; i < n; i++) H[h + i] = i < m ? keys[i] & 0x3FF : 0;
            M._PortLnSealedN(f, kb, n, nev);
        };
        return eng;
    }
    function engineOfModule(M) {
        var scratch = M._malloc(512);
        var names = null;
        function u32(i) { return M.HEAPU32[(scratch >> 2) + i]; }
        function s32(i) { return M.HEAP32[(scratch >> 2) + i]; }
        var eng = {
            frame: function () { return M._PortFrameNumber(); },
            localInput: function (f, k, seat) { M._PortLnLocalInputSeat(f, seat | 0, k); },
            remoteInput: function (f, k, dev, seat) { M._PortLnRemoteInputSeat(f, dev === undefined ? 1 : dev, seat | 0, k); },
            sealedGet: function (f) {
                if (!M._PortLnSealedGet(f, scratch)) return null;
                var rec = { k0: u32(0), k1: u32(1), nev: u32(2), ontime: u32(3), hlo: u32(4), hhi: u32(5),
                            hashed: u32(6), k2: u32(7), k3: u32(8), ev: [] };
                for (var i = 0; i < rec.nev; i++) {
                    M._PortLnSealedEventGet(f, i, scratch);
                    rec.ev.push([s32(0), s32(1), s32(2), s32(3), s32(4)]);
                }
                return rec;
            },
            sealedEvent: function (f, e) { M._PortLnSealedEvent(f, e[0], e[1], e[2], e[3], e[4]); },
            sealed: function (f, k0, k1, nev, k2, k3) { M._PortLnSealed4(f, k0, k1, k2 | 0, k3 | 0, nev); },
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
            seatRequest: function (dev, seat, on) { return M._PortLnSeatRequest(dev, seat, on); },
            deviceGone: function (dev) { M._PortLnDeviceGone(dev); },
            freeSlots: function () { return M._PortLnFreeSlots(); },
            setSelf: function (dev) { M._PortLnSetSelf(dev); },
            selfSeats: function () { return M._PortLnSelfSeats(); },
            seatAskPoll: function () { return M._PortLnSeatAskPoll(); },
            slotOwner: function (s) { return M._PortLnSlotOwner(s); },
        };
        return wideCalls(M, cardCalls(M, eng));
    }

    return {
        PERIOD: PERIOD, T_INPUT: T_INPUT, T_SEALED: T_SEALED,
        HostSession: HostSession, GuestSession: GuestSession, HostHub: HostHub,
        encInput: encInput, decInput: decInput, encSealed: encSealed, decSealed: decSealed,
        engineOfModule: engineOfModule, cardCalls: cardCalls, wideCalls: wideCalls, recBytes: recBytes, recKeys: recKeys,
    };
}));
