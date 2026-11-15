// A discrete-event, seeded model of the network between two devices.
//
// Virtual time only (milliseconds, floating point): nothing here reads a
// wall clock, so a run is reproducible from its seed, runs as fast as the
// simulation can go, and is immune to CPU load.
//
//   datagrams  unreliable, unordered (a WebRTC data channel with
//              ordered:false, maxRetransmits:0): one-way latency + uniform
//              jitter (which reorders by itself), loss, duplication, extra
//              reordering, burst outages that drop everything.
//   reliable   ordered and reliable (a second, reliable data channel, for
//              images): latency + serialisation at a bandwidth + one RTT per
//              lost segment, never before the message sent ahead of it, and
//              nothing gets through an outage until it ends.

export function mulberry32(seed) {
    let a = seed >>> 0;
    return () => {
        a = (a + 0x6D2B79F5) >>> 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

// A min-heap of timed callbacks; ties keep insertion order.
export class Scheduler {
    constructor() { this.h = []; this.seq = 0; this.now = 0; }
    at(t, fn) {
        const e = { t, s: this.seq++, fn };
        const h = this.h;
        h.push(e);
        let i = h.length - 1;
        while (i > 0) {
            const p = (i - 1) >> 1;
            if (h[p].t < e.t || (h[p].t === e.t && h[p].s < e.s)) break;
            h[i] = h[p]; i = p;
        }
        h[i] = e;
    }
    pop() {
        const h = this.h;
        if (!h.length) return null;
        const top = h[0], last = h.pop();
        if (h.length) {
            let i = 0;
            for (;;) {
                const l = 2 * i + 1, r = l + 1;
                let m = i;
                const lt = (a, b) => a.t < b.t || (a.t === b.t && a.s < b.s);
                const cand = (k) => (k < h.length && lt(h[k], m === i ? last : h[m]) ? k : m);
                m = cand(l); m = cand(r);
                if (m === i) break;
                h[i] = h[m]; i = m;
            }
            h[i] = last;
        }
        this.now = top.t;
        return top;
    }
    get size() { return this.h.length; }
}

export const DEFAULT_LINK = {
    oneWay: 0,        // ms
    jitter: 0,        // ms, uniform 0..jitter added per datagram
    loss: 0,          // probability a datagram is lost
    dup: 0,           // probability a datagram is delivered twice
    reorder: 0,       // probability a datagram is held back an extra reorderMs
    reorderMs: 30,
    outages: [],      // [[startMs, durationMs], ...] both directions dead
    bandwidth: 4e6,   // bits/s for the reliable channel's serialisation
    segment: 16384,   // reliable channel segment (one loss = one RTT)
};

export class Link {
    constructor(sched, seed, cfg) {
        this.s = sched;
        this.rng = mulberry32(seed);
        this.c = Object.assign({}, DEFAULT_LINK, cfg);
        this.relAt = { hg: 0, gh: 0 };
        this.st = { hg: newStats(), gh: newStats() };
    }
    down(t) {
        for (const [a, d] of this.c.outages) if (t >= a && t < a + d) return a + d;
        return 0;
    }
    datagram(dir, bytes, deliver) {
        const t = this.s.now, st = this.st[dir], c = this.c;
        st.sent++; st.bytes += bytes.length + 28;   // + IP/UDP-ish overhead
        if (this.down(t)) { st.outageDrops++; return; }
        if (this.rng() < c.loss) { st.lost++; return; }
        const one = () => {
            let d = c.oneWay + this.rng() * c.jitter;
            if (c.reorder && this.rng() < c.reorder) { d += this.rng() * c.reorderMs; st.reordered++; }
            const at = t + d;
            if (this.down(at)) { st.outageDrops++; return; }
            this.s.at(at, () => deliver(bytes));
        };
        one();
        if (c.dup && this.rng() < c.dup) { st.duped++; one(); }
    }
    reliable(dir, msg, size, deliver) {
        const t = this.s.now, st = this.st[dir], c = this.c;
        st.relSent++; st.relBytes += size;
        let at = t + c.oneWay + this.rng() * c.jitter + (size * 8) / c.bandwidth * 1000;
        const segs = Math.max(1, Math.ceil(size / c.segment));
        for (let i = 0; i < segs; i++) if (this.rng() < c.loss) at += 2 * c.oneWay + 20;
        // nothing crosses an outage: it goes after the link is back
        for (let pass = 0; pass < 2; pass++)
            for (const [a, d] of c.outages)
                if (t < a + d && at > a) at = Math.max(at, a + d + c.oneWay + 20);
        at = Math.max(at, this.relAt[dir] + 0.001);
        this.relAt[dir] = at;
        this.s.at(at, () => deliver(msg));
    }
}

function newStats() {
    return { sent: 0, bytes: 0, lost: 0, duped: 0, reordered: 0, outageDrops: 0, relSent: 0, relBytes: 0 };
}
