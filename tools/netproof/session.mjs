// The online session's logic for each side, independent of what carries the
// bytes.  A transport is { datagram(bytes), reliable(msg, size) } plus the
// two receive entry points below; a clock is a function returning this
// device's own time in ms.  tools/netproof/link.mjs is one transport (a
// simulated link); a WebRTC data channel pair would be another.
//
// The engine (platform/lmpnet.c) does the sealing, prediction, rewinding,
// keyframes, hashing and verification.  This file does what needs a wire:
// redundancy and acknowledgement of inputs and sealed frames, time sync (the
// guest's lead over the host), repair requests and image transfer.
import zlib from 'zlib';

export const PERIOD = 1000 / 59.7275;
const T_INPUT = 0x31, T_SEALED = 0x32;

// --- engine adapter ---------------------------------------------------------
export function engineOf(inst) {
    const M = inst.M;
    return {
        frame: () => M._PortFrameNumber(),
        localInput: (f, k) => M._PortLnLocalInput(f, k),
        remoteInput: (f, k) => M._PortLnRemoteInput(f, k),
        sealedGet: (f) => inst.sealedGet(f),
        sealedEvent: (f, e) => M._PortLnSealedEvent(f, e[0], e[1], e[2], e[3], e[4]),
        sealed: (f, k0, k1, nev) => M._PortLnSealed(f, k0, k1, nev),
        remoteHash: (f, lo, hi) => M._PortLnRemoteHash(f, lo, hi),
        event: (t, a, b, c, d) => M._PortLnEvent(t, a, b, c, d),
        tel: () => inst.tel.read(),
        imageRequest: () => M._PortLnImageRequest(),
        image: () => inst.image(),
        loadImage: (b) => inst.loadImage(b),
    };
}

// --- wire -------------------------------------------------------------------
function encInput(g, keys, ack, ts) {
    const b = Buffer.alloc(14 + 2 * keys.length);
    b[0] = T_INPUT; b.writeUInt32LE(g >>> 0, 1); b[5] = keys.length;
    b.writeUInt32LE(ack >>> 0, 6); b.writeUInt32LE(ts >>> 0, 10);
    keys.forEach((k, i) => b.writeUInt16LE(k, 14 + 2 * i));
    return b;
}
function decInput(b) {
    const n = b[5], g = b.readUInt32LE(1), keys = [];
    for (let i = 0; i < n; i++) keys.push(b.readUInt16LE(14 + 2 * i));
    return { g, keys, ack: b.readUInt32LE(6), ts: b.readUInt32LE(10) };
}
function encSealed(P, first, recs, guestContig, marginMin, echoTs, holdMs) {
    let n = 23;
    for (const r of recs) n += 5 + (r.hashed ? 8 : 0) + r.ev.length * 17;
    const b = Buffer.alloc(n);
    b[0] = T_SEALED; b.writeUInt32LE(P >>> 0, 1); b.writeUInt32LE(first >>> 0, 5); b.writeUInt16LE(recs.length, 9);
    b.writeUInt32LE(guestContig >>> 0, 11); b.writeInt16LE(Math.max(-32768, Math.min(32767, marginMin)), 15);
    b.writeUInt32LE(echoTs >>> 0, 17); b.writeUInt16LE(Math.min(65535, Math.round(holdMs * 10)), 21);
    let o = 23;
    for (const r of recs) {
        b.writeUInt16LE(r.k0, o); b.writeUInt16LE(r.k1, o + 2); b[o + 4] = (r.hashed ? 1 : 0) | (r.ev.length << 1); o += 5;
        if (r.hashed) { b.writeUInt32LE(r.hlo >>> 0, o); b.writeUInt32LE(r.hhi >>> 0, o + 4); o += 8; }
        for (const e of r.ev) { b[o] = e[0]; for (let i = 1; i < 5; i++) b.writeInt32LE(e[i] | 0, o + 1 + 4 * (i - 1)); o += 17; }
    }
    return b;
}
function decSealed(b) {
    const P = b.readUInt32LE(1), first = b.readUInt32LE(5), n = b.readUInt16LE(9), recs = [];
    let o = 23;
    for (let i = 0; i < n; i++) {
        const r = { k0: b.readUInt16LE(o), k1: b.readUInt16LE(o + 2), hashed: b[o + 4] & 1, ev: [] };
        const nev = b[o + 4] >> 1; o += 5;
        if (r.hashed) { r.hlo = b.readUInt32LE(o); r.hhi = b.readUInt32LE(o + 4); o += 8; }
        for (let k = 0; k < nev; k++) { const e = [b[o]]; for (let j = 0; j < 4; j++) e.push(b.readInt32LE(o + 1 + 4 * j)); r.ev.push(e); o += 17; }
        recs.push(r);
    }
    return { P, first, recs, guestContig: b.readUInt32LE(11), marginMin: b.readInt16LE(15), echoTs: b.readUInt32LE(17), holdMs: b.readUInt16LE(21) / 10 };
}

// --- host ---------------------------------------------------------------------
export class HostSession {
    constructor(eng, tx, clock, opts = {}) {
        this.e = eng; this.tx = tx; this.clock = clock;
        this.o = Object.assign({ maxPerPacket: 64, resendWindow: 1024 }, opts);
        this.start = eng.frame();
        this.guestAck = this.start;       // guest has every sealed frame below this
        this.guestHigh = -1;              // highest guest input frame seen
        this.guestContig = 0;             // guest inputs contiguous below this
        this.seen = new Set();
        this.margins = [];                // {t, m}: first arrival, frames before the latch
        this.lastTs = 0; this.lastTsAt = 0;
        this.imageWanted = 0; this.imageReason = ''; this.imgInFlight = null;
        this.st = { packets: 0, recs: 0, images: 0, imageRaw: 0, imageComp: 0, compressMs: 0, repairReqs: 0, viewReqs: 0, inputsIn: 0, inputsUseless: 0 };
        this.started = false;
    }
    onDatagram(buf) {
        if (buf[0] !== T_INPUT) return;
        const now = this.clock(), m = decInput(buf), next = this.e.frame();
        const first = m.g - m.keys.length + 1;
        this.st.inputsIn++;
        for (let i = 0; i < m.keys.length; i++) {
            const f = first + i;
            if (f < this.start) continue;
            if (!this.seen.has(f)) {
                this.seen.add(f);
                this.margins.push({ t: now, m: f - next });
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
        this.lastTs = m.ts; this.lastTsAt = now;
    }
    onReliable(msg) {
        if (msg.t === 'repair') { this.st.repairReqs++; if (this.imgInFlight === null || msg.frame >= this.imgInFlight) this.wantImage('repair:' + msg.reason); else this.st.repairDup = (this.st.repairDup || 0) + 1; }
        else if (msg.t === 'viewreq') { this.st.viewReqs++; this.e.event(2, msg.w, msg.h, 0, 0); }
    }
    wantImage(reason) {
        if (!this.imageWanted) { this.imageWanted = 1; this.imageReason = reason; this.e.imageRequest(); }
    }
    // Before each frame the host simulates: its own pad for that frame.
    beforeFrame(f, keys) { this.e.localInput(f, keys); }
    // After each tick (one or more frames): images, then the sealed stream.
    afterTick() {
        const now = this.clock(), P = this.e.frame();
        if (this.imageWanted) {
            const img = this.e.image();
            if (img) {
                const t0 = performance.now();
                const comp = zlib.deflateRawSync(img.bytes, { level: 6 });
                this.st.compressMs += performance.now() - t0;
                this.st.images++; this.st.imageRaw = img.bytes.length; this.st.imageComp = comp.length;
                this.tx.reliable({ t: 'image', F: img.frame, raw: img.bytes.length, comp, reason: this.imageReason }, comp.length + 32);
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
        while (this.margins.length && this.margins[0].t < now - 1000) this.margins.shift();
        const mmin = this.margins.length ? Math.min(...this.margins.map((x) => x.m)) : 127;
        let first = Math.max(this.guestAck, P - this.o.resendWindow, this.start);
        const recs = [];
        for (let f = first; f < P && recs.length < this.o.maxPerPacket; f++) {
            const r = this.e.sealedGet(f);
            if (!r) { if (!recs.length) first = f + 1; else break; continue; }
            recs.push(r);
        }
        const pkt = encSealed(P, first, recs, this.guestContig, mmin, this.lastTs, now - this.lastTsAt);
        this.st.packets++; this.st.recs += recs.length;
        this.tx.datagram(pkt);
    }
}

// --- guest ----------------------------------------------------------------------
export class GuestSession {
    constructor(eng, tx, clock, opts = {}) {
        this.e = eng; this.tx = tx; this.clock = clock;
        this.o = Object.assign({ redundancy: 32, marginInit: 2, marginMin: 1, marginMax: 60,
                                  maxExtrap: 15, burstMax: 1200, lockstep: false, leadCap: null }, opts);
        this.margin = this.o.marginInit;
        this.hostP = -1; this.recvAt = 0; this.hostAck = 0;
        this.rtt = null; this.rtts = [];
        this.lastAdj = -1e9; this.goodSince = null;
        this.repairInFlight = 0; this.repairAt = 0;
        this.joined = false; this.lastLocal = -1; this.keyRing = new Uint16Array(4096); this.offs = [];
        this.st = { packets: 0, recs: 0, holds: 0, skips: 0, skipFrames: 0, bursts: 0, burstFrames: 0, capHolds: 0,
                    lockHolds: 0, images: 0, imageApplyMs: 0, imageBytes: 0, repairReqs: 0, marginUps: 0, marginDowns: 0,
                    leadSamples: [], marginSamples: [], firstImageAt: null };
    }
    onDatagram(buf) {
        if (buf[0] !== T_SEALED) return;
        const now = this.clock(), m = decSealed(buf);
        this.st.packets++;
        for (let i = 0; i < m.recs.length; i++) {
            const f = m.first + i, r = m.recs[i];
            for (const ev of r.ev) this.e.sealedEvent(f, ev);
            this.e.sealed(f, r.k0, r.k1, r.ev.length);
            if (r.hashed) this.e.remoteHash(f, r.hlo, r.hhi);
        }
        this.st.recs += m.recs.length;
        if (m.P > this.hostP) { this.hostP = m.P; this.recvAt = now; }
        // host clock offset seen by this packet; the least-delayed packet of
        // the last second is the best estimate (jitter only ever delays)
        this.offs.push({ t: now, o: m.P - now / PERIOD });
        while (this.offs.length && this.offs[0].t < now - 1000) this.offs.shift();
        if (m.guestContig > this.hostAck) this.hostAck = m.guestContig;
        if (m.echoTs) {
            const rtt = now - m.echoTs / 10 - m.holdMs;
            if (rtt >= 0 && rtt < 5000) {
                this.rtts.push(rtt); if (this.rtts.length > 64) this.rtts.shift();
                this.rtt = Math.min(...this.rtts.slice(-16));     // min filter: queueing noise out
            }
        }
        this.adapt(now, m.marginMin);
    }
    // Margin (frames of slack the guest's inputs should arrive with): up at
    // once when the host saw late inputs, down slowly when it has slack.
    adapt(now, mmin) {
        if (mmin === 127) return;
        if (mmin < this.o.marginMin && now - this.lastAdj > 250) {
            this.margin = Math.min(this.o.marginMax, this.margin + Math.min(4, this.o.marginMin - mmin));
            this.lastAdj = now; this.goodSince = null; this.st.marginUps++;
        } else if (mmin > this.o.marginMin + 2) {
            if (this.goodSince === null) this.goodSince = now;
            if (now - this.goodSince > 1000 && now - this.lastAdj > 500 && this.margin > this.o.marginInit) {
                this.margin--; this.lastAdj = now; this.goodSince = now; this.st.marginDowns++;
            }
        } else this.goodSince = null;
    }
    onReliable(msg) {
        if (msg.t !== 'image') return;
        const t0 = performance.now();
        const raw = zlib.inflateRawSync(msg.comp);
        this.e.loadImage(raw);
        this.st.imageApplyMs += performance.now() - t0;
        this.st.images++; this.st.imageBytes = msg.comp.length;
        this.repairInFlight = 0;
        this.joined = true;
        this.imagePending = true;   // applied at the next frame top: run exactly one frame first
        if (this.st.firstImageAt === null) this.st.firstImageAt = this.clock();
    }
    // How many frames to simulate on this vsync.
    framesToRun(nominal) {
        if (!this.joined) return 0;
        if (this.imagePending) { this.imagePending = false; return 1; }
        const now = this.clock(), P = this.e.frame(), tel = this.e.tel();
        if (this.hostP < 0) return 0;
        // the host's frame now: the best offset of the last second, but never
        // extrapolated more than maxExtrap frames past the last packet (a
        // silent host -- outage, freeze -- is not assumed to be running)
        const best = this.offs.length ? Math.max(...this.offs.map((x) => x.o)) : this.hostP - this.recvAt / PERIOD;
        const hostNow = Math.min(best + now / PERIOD, this.hostP + this.o.maxExtrap);
        const rttF = (this.rtt === null ? 0 : this.rtt) / PERIOD;
        const target = hostNow + rttF + this.margin;
        const diff = target - (P + nominal - 1);
        let n = nominal;
        if (diff > 8) { n = Math.min(Math.ceil(diff) + nominal - 1, this.o.burstMax); this.st.bursts++; this.st.burstFrames += n - nominal; }
        else if (diff >= 2.5) { n = nominal + 1; this.st.skips++; this.st.skipFrames++; }
        else if (diff <= -2.5) { n = nominal - 1; this.st.holds++; }
        // never predict further than the history can rewind
        const cap = this.o.leadCap ?? 120;
        const lead = (P + n) - tel.sealedUpTo;
        if (lead > cap) { const m2 = Math.max(0, n - (lead - cap)); if (m2 < n) this.st.capHolds++; n = m2; }
        if (this.o.lockstep) { const m2 = Math.max(0, Math.min(n, tel.sealedUpTo - P)); if (m2 < n) this.st.lockHolds++; n = m2; }
        this.st.leadSamples.push(P - tel.sealedUpTo);
        this.st.marginSamples.push(this.margin);
        return Math.max(0, n);
    }
    beforeFrame(f, keys) { this.e.localInput(f, keys); this.lastLocal = f; this.keyRing[f & 4095] = keys; }
    afterTick() {
        const now = this.clock(), tel = this.e.tel();
        if (tel.repairWanted && (!this.repairInFlight || now - this.repairAt > 3000)) {
            this.repairInFlight = 1; this.repairAt = now; this.st.repairReqs++;
            this.tx.reliable({ t: 'repair', reason: tel.mismatches ? 'mismatch' : 'history', frame: tel.repairFrame }, 16);
        }
        if (this.lastLocal < 0) return;
        const g = this.lastLocal;
        const from = Math.max(this.hostAck, g - this.o.redundancy + 1, 0);
        const keys = [];
        for (let f = from; f <= g; f++) keys.push(this.keyAt(f));
        if (!keys.length) return;
        this.tx.datagram(encInput(g, keys, tel.sealedUpTo, Math.round(now * 10)));
    }
    keyAt(f) { return this.keyRing[f & 4095]; }
}
