// Co-op stage 1, the client half: web/coop.js (jitter buffer, slot
// mapping, send policy, tab identity) and web/net_session.js's pose path
// (binary when the room lists 'pose', the `signal` fallback otherwise,
// liveness), against a fake engine and the in-process dev relay.
import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'module';
import { startRelay } from '../dev-relay.mjs';
import { encodePoseUp, decodePoseBody } from '../wire.mjs';

const require = createRequire(import.meta.url);
const Coop = require('../../web/coop.js');
const KatamNet = require('../../web/net_session.js');
const FRAME = Coop.FRAME_MS;
const PX = 256;

function poseBytes({ x = 0, y = 0, room = 1, hidden = false, tag = 0 } = {}, size = 36) {
    const b = new Uint8Array(size);
    const dv = new DataView(b.buffer);
    dv.setInt32(0, x, true); dv.setInt32(4, y, true); dv.setUint16(8, room, true);
    b[10] = hidden ? 1 : 0;
    b[11] = tag;
    return b;
}

test('web/coop.js and netplay/wire.mjs agree on POSE_UP', () => {
    const poses = [{ seq: 65535, bytes: poseBytes({ x: 5 }) }, { seq: 0, bytes: poseBytes({ x: -7 }) }];
    const a = Coop.encodePoseUp(3, poses), b = encodePoseUp(3, poses);
    assert.deepEqual([...a], [...b]);
    const d1 = Coop.decodePoseBody(a.subarray(1)), d2 = decodePoseBody(a.subarray(1));
    assert.deepEqual(d1.poses.map((p) => [p.seq, [...p.bytes]]), d2.poses.map((p) => [p.seq, [...p.bytes]]));
    assert.equal(Coop.poseX(d1.poses[1].bytes), -7);
});

// A virtual-time link: the sender's frame f is sent (2 per message) and
// arrives after lat + jitter, order kept; the receiver samples every frame.
function simulate({ frames = 600, latMs = 120, jitterMs = 40, path = (f) => ({ x: f * 2 * PX }),
                    senderStop = null, receiverStop = null, seed = 1 } = {}) {
    let s = seed;
    const rnd = () => { s = (s * 1103515245 + 12345) & 0x7FFFFFFF; return s / 0x7FFFFFFF; };
    const jb = new Coop.JitterBuffer();
    const arrivals = [];      // {at, seq, bytes, flags}
    let lastAt = 0, pend = [];
    const skew = 12345.6;     // the two clocks are unrelated
    for (let f = 0; f < frames; f++) {
        if (senderStop && f >= senderStop[0] && f < senderStop[1]) { pend = []; continue; }
        const t = f * FRAME;
        pend.push({ seq: Math.round((t + skew) / FRAME) & 0xFFFF, bytes: poseBytes(path(f)) });
        if (pend.length >= 2) {
            const at = Math.max(lastAt, t + latMs + rnd() * jitterMs);
            lastAt = at;
            for (const p of pend) arrivals.push({ at, ...p, flags: 0 });
            pend = [];
        }
    }
    const out = [];
    let i = 0;
    for (let f = 0; f < frames + 120; f++) {
        const t = f * FRAME + 0.3;
        while (i < arrivals.length && arrivals[i].at <= t) { const a = arrivals[i++]; jb.push(a.seq, a.bytes, a.at, a.flags); }
        if (receiverStop && f >= receiverStop[0] && f < receiverStop[1]) continue;
        const r = jb.sample(t);
        out.push({ f, state: r.state, x: r.bytes ? Coop.poseX(r.bytes) : null, room: r.bytes ? Coop.poseRoom(r.bytes) : null, tag: r.bytes ? r.bytes[11] : null, D: jb.D });
    }
    return { out, jb };
}

test('jitter buffer: follows within ~15 frames at 120 ms +40 ms jitter, smoothly, never backwards', () => {
    const { out } = simulate({});
    let maxLag = 0, back = 0, maxStep = 0, prev = null;
    for (const o of out) {
        if (o.f < 60 || o.f >= 600 || o.state !== 'pose') continue;
        const lag = o.f - o.x / (2 * PX);
        maxLag = Math.max(maxLag, lag);
        if (prev !== null) { if (o.x < prev) back++; maxStep = Math.max(maxStep, (o.x - prev) / PX); }
        prev = o.x;
    }
    assert.ok(maxLag <= 15, `lag ${maxLag.toFixed(1)} frames`);
    assert.equal(back, 0, 'went backwards');
    assert.ok(maxStep <= 6, `step ${maxStep.toFixed(1)} px (2 px/frame motion)`);
});

test('jitter buffer: a stalled sender freezes, goes absent after 1.5 s, and resumes at its newest pose', () => {
    const { out } = simulate({ frames: 900, senderStop: [300, 600] });
    const at = (f) => out.find((o) => o.f === f);
    // frozen (held / briefly extrapolated) soon after the stop
    const frozen = at(340), frozen2 = at(370);
    assert.equal(frozen.state, 'pose');
    assert.equal(frozen.x, frozen2.x);
    assert.ok(frozen.x <= (300 + Coop.EXTRAP_MAX) * 2 * PX);
    // absent once 1.5 s passed with nothing
    assert.equal(at(300 + 8 + Math.ceil(1500 / FRAME) + 2).state, 'absent');
    // back: the first pose shown is the newest one (no replay of a backlog)
    const back = out.find((o) => o.f > 600 && o.state === 'pose');
    assert.ok(back.f - 600 <= 15, `back after ${back.f - 600} frames`);
    assert.ok(back.x >= 600 * 2 * PX, 'resumed at the new position');
    const later = at(back.f + 30);
    assert.ok(back.f + 30 - later.x / (2 * PX) <= 15);
});

test('jitter buffer: a receiver frozen for 5 s jumps to the newest pose, no backlog', () => {
    const { out } = simulate({ frames: 900, receiverStop: [200, 500] });
    const first = out.find((o) => o.f >= 500);
    assert.equal(first.state, 'pose');
    assert.ok(first.f - first.x / (2 * PX) <= 15, `lag after the freeze ${first.f - first.x / (2 * PX)}`);
});

test('jitter buffer: room changes and teleports snap; hidden poses are not interpolated', () => {
    const path = (f) => f < 200 ? { x: f * 2 * PX, room: 1 } : f < 400 ? { x: 10 * PX, room: 2, tag: 7 }
        : f < 500 ? { x: 10 * PX + (f - 400) * 2 * PX, room: 2, hidden: f >= 450 } : { x: 9000 * PX, room: 2 };
    const { out } = simulate({ frames: 700, path, jitterMs: 0 });
    for (const o of out) {
        if (o.state !== 'pose') continue;
        if (o.room === 2 && o.tag !== 7 && o.f < 300) assert.fail('other bytes must come from the newer pose');
        // never a point between room 1's x (>= 2*200 px) and room 2's 10 px
        if (o.room === 1) assert.ok(o.x <= 200 * 2 * PX);
        if (o.room === 2) assert.ok(o.x >= 10 * PX);
        if (o.x > 300 * PX && o.x < 9000 * PX && o.f > 500) assert.fail('teleport interpolated: x ' + o.x / PX);
    }
});

test('jitter buffer: seq wraps at 65536', () => {
    const jb = new Coop.JitterBuffer();
    for (let i = 0; i < 40; i++) jb.push((65520 + i) & 0xFFFF, poseBytes({ x: i * PX }), i * FRAME + 100, 0);
    const r = jb.sample(40 * FRAME + 100);
    assert.equal(r.state, 'pose');
    assert.ok(Coop.poseX(r.bytes) > 30 * PX);
});

test('tab identity: a reload keeps it, a duplicated tab mints a fresh one, no Web Locks keeps it', async () => {
    const held = new Set();
    const locks = {
        request(name, opts, cb) {
            if (opts.ifAvailable) {
                const ok = !held.has(name);
                if (ok) held.add(name);
                return Promise.resolve(cb(ok ? { name } : null));
            }
            return new Promise((resolve) => {
                const tryIt = () => {
                    if (opts.signal && opts.signal.aborted) return resolve();
                    if (held.has(name)) return setTimeout(tryIt, 10);
                    held.add(name);
                    cb({ name });
                    resolve();
                };
                tryIt();
            });
        },
    };
    let n = 0;
    const mint = () => 'id' + (++n) + '-0123456789abcdef';
    const store = (v) => ({ v, get() { return this.v; }, set(x) { this.v = x; } });
    const a = store(null);
    const id1 = await Coop.claimIdentity({ storage: a, locks, mint, waitMs: 100 });
    assert.equal(id1, a.v);
    // a duplicate of that tab (sessionStorage copied) while it still holds the lock
    const dup = store(a.v);
    const id2 = await Coop.claimIdentity({ storage: dup, locks, mint, waitMs: 100 });
    assert.notEqual(id2, id1);
    assert.equal(dup.v, id2);
    // a reload: the old page let go
    held.delete('katam-coop-id-' + id1);
    assert.equal(await Coop.claimIdentity({ storage: store(id1), locks, mint, waitMs: 100 }), id1);
    // no Web Locks at all: the stored one
    assert.equal(await Coop.claimIdentity({ storage: store('keep-0123456789abcdef'), locks: null, mint }), 'keep-0123456789abcdef');
});

// --- live: the dev relay, real sockets, a fake engine -------------------------

function fakeModule(size = 36) {
    const HEAPU8 = new Uint8Array(8192);
    const SCRATCH = 2048;
    const M = {
        HEAPU8, local: { x: 0, y: 0, room: 5, valid: true }, puppets: {}, enabled: 0, calls: [],
        _PortCoopPoseSize: () => size,
        _PortCoopEnable: (on) => { M.enabled = on; },
        _PortCoopReady: () => 1,
        _PortCoopSetPuppet: (s, on) => { M.calls.push(['puppet', s, on]); M.puppets[s] = { on: !!on, pose: null, absent: true }; },
        _PortCoopSetPose: (s, p) => { const q = M.puppets[s]; q.pose = HEAPU8.slice(p, p + size); q.absent = false; },
        _PortCoopSetAbsent: (s) => { const q = M.puppets[s]; if (q) q.absent = true; },
        _PortCoopLocalPose: (p) => {
            if (!M.local.valid) return 0;
            HEAPU8.set(poseBytes(M.local, size), p);
            return 1;
        },
        _PortCoopPoseBuffer: () => SCRATCH,
    };
    return M;
}

// Wrap the global WebSocket: optional welcome rewrite (an old server, no
// features), count binary sends, and a black-hole switch.
function wrapWS(opts = {}) {
    const socks = [];
    class W extends WebSocket {
        constructor(u) {
            super(u);
            socks.push(this);
            this.binSent = 0;
            this.blackhole = false;
            let handler = null;
            super.onmessage = (e) => {
                if (this.blackhole || !handler) return;
                if (opts.oldServer && typeof e.data === 'string' && e.data.includes('"welcome"')) {
                    const m = JSON.parse(e.data);
                    delete m.features;
                    return handler.call(this, { data: JSON.stringify(m) });
                }
                handler.call(this, e);
            };
            Object.defineProperty(this, 'onmessage', { set(f) { handler = f; }, get() { return handler; } });
        }
        send(d) {
            if (this.blackhole) return;
            if (typeof d !== 'string') this.binSent++;
            super.send(d);
        }
    }
    W.socks = socks;
    return W;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(fn, ms, what) {
    const t0 = Date.now();
    while (!fn()) { if (Date.now() - t0 > ms) throw new Error('timeout: ' + what); await sleep(10); }
}

async function session(n, { oldServer = false, liveness = 0 } = {}) {
    const relay = await startRelay(0, { quiet: true });
    const base = `http://127.0.0.1:${relay.port}`;
    const { code } = await KatamNet.createRoom(base);
    const players = [];
    for (let i = 0; i < n; i++) {
        const WS = wrapWS({ oldServer });
        const Module = fakeModule();
        const net = KatamNet.connect({
            base, code, WebSocket: WS, liveness, backoff: () => 200,
            hello: { sim: Coop.SIM, rom: 'rom1', content: 'vanilla', name: 'p' + i, platform: 'node',
                     caps: { seats: 4, sync: false }, want: 'play' },
        });
        const coop = Coop.create({ Module, net });
        players.push({ Module, net, coop, WS });
        await until(() => net.state === 'open', 3000, 'open ' + i);
    }
    await until(() => players.every((p) => p.net.roster && p.net.roster.members.filter((m) => m.connected).length === n), 3000, 'roster');
    let f = 0;
    const timer = setInterval(() => {
        f++;
        for (const p of players) {
            if (p.frozen) continue;
            if (p.move) p.Module.local.x = f * 2 * PX;
            p.coop.tick();
        }
    }, FRAME);
    return { relay, players, code, stop() { clearInterval(timer); for (const p of players) p.net.close(); relay.close(); }, frame: () => f };
}

test('live: B\'s puppet follows A over binary poses; ~30 msg/s; one slot each', async () => {
    const s = await session(2);
    try {
        const [A, B] = s.players;
        A.move = true;
        assert.ok(A.net.hasFeature('pose'));
        await until(() => B.Module.puppets[1] && B.Module.puppets[1].pose, 3000, 'puppet pose');
        const sent0 = A.coop.stats.sent, t0 = Date.now();
        await sleep(1500);
        const rate = (A.coop.stats.sent - sent0) / ((Date.now() - t0) / 1000);
        assert.ok(rate > 20 && rate <= 32, `rate ${rate.toFixed(1)}/s`);
        const px = Coop.poseX(B.Module.puppets[1].pose) / (2 * PX);
        const lag = s.frame() - px;
        assert.ok(lag >= 0 && lag <= 15, `lag ${lag} frames`);
        assert.ok(A.WS.socks[0].binSent > 10, 'binary poses');
        assert.equal(B.coop.slotOf(A.net.mid), 1);
        assert.equal(A.coop.slotOf(B.net.mid), 1);
        assert.ok(A.Module.enabled);
    } finally { s.stop(); }
});

test('live: a room without the pose feature gets `signal` poses, never a binary one', async () => {
    const s = await session(2, { oldServer: true });
    try {
        const [A, B] = s.players;
        A.move = true;
        assert.ok(!A.net.hasFeature('pose'));
        await until(() => B.Module.puppets[1] && B.Module.puppets[1].pose && Coop.poseX(B.Module.puppets[1].pose) > 0, 3000, 'puppet via signal');
        await sleep(300);
        assert.equal(A.WS.socks[0].binSent, 0);
        assert.equal(B.WS.socks[0].binSent, 0);
        assert.equal(A.net.state, 'open');
    } finally { s.stop(); }
});

test('live: 5 players -- 3 shown, 1 not, slots freed and reused when a player leaves', async () => {
    const s = await session(5);
    try {
        const [A, B, C, D, E] = s.players;
        await sleep(300);
        const st = A.coop.status();
        assert.equal(st.players.length, 4);
        assert.equal(st.hidden, 1);
        assert.deepEqual([B, C, D].map((p) => A.coop.slotOf(p.net.mid)), [1, 2, 3]);
        assert.equal(A.coop.slotOf(E.net.mid), 0);
        C.net.leave();
        await until(() => A.coop.slotOf(E.net.mid) === 2, 3000, 'E takes the freed slot');
        assert.ok(A.Module.calls.some((c) => c[0] === 'puppet' && c[1] === 2 && c[2] === 0), 'slot freed');
        assert.equal(A.coop.status().hidden, 0);
        await until(() => A.Module.puppets[2] && !A.Module.puppets[2].absent, 3000, 'E drawn');
    } finally { s.stop(); }
});

test('live: a silent drop is noticed by the liveness check; reconnect keeps the mid', async () => {
    const s = await session(2, { liveness: 1500 });
    try {
        const [A, B] = s.players;
        A.move = true;
        await until(() => B.Module.puppets[1] && !B.Module.puppets[1].absent, 3000, 'puppet');
        const mid = B.net.mid;
        B.WS.socks[0].blackhole = true;
        await until(() => B.Module.puppets[1].absent, 4000, 'puppet absent while dark');
        await until(() => B.net.deadSockets >= 1 && B.net.state === 'open', 6000, 'reconnected');
        assert.equal(B.net.mid, mid);
        await until(() => !B.Module.puppets[1].absent && B.coop.slotOf(A.net.mid) === 1, 3000, 'puppet back');
        // a clean server-side close too
        const room = [...s.relay.v2.rooms.values()][0];
        const before = B.net.reconnects;
        for (const ws of room.socks.values()) if (ws !== undefined) { /* B's is the newest */ }
        [...room.socks.values()].at(-1).terminate();
        await until(() => B.net.reconnects > before && B.net.state === 'open', 4000, 'reconnect after close');
        assert.equal(B.net.mid, mid);
        await until(() => !B.Module.puppets[1].absent, 3000, 'puppet back after close');
    } finally { s.stop(); }
});

test('live: a paused tab says so once and then sends nothing', async () => {
    const s = await session(2);
    try {
        const [A, B] = s.players;
        A.move = true;
        await until(() => B.Module.puppets[1] && !B.Module.puppets[1].absent, 3000, 'puppet');
        A.coop.setPaused(true);
        await sleep(100);
        const n = A.coop.stats.sent;
        await sleep(500);
        assert.equal(A.coop.stats.sent, n);
        assert.ok(B.coop.status().players[0].away, 'shown as away');
        await until(() => B.Module.puppets[1].absent, 3000, 'absent after the stall time');
        A.coop.setPaused(false);
        await until(() => !B.Module.puppets[1].absent, 2000, 'back');
    } finally { s.stop(); }
});

test('no engine exports: the core stays inert and says what is missing', () => {
    const net = { on() {}, state: 'open', roster: null, mid: 1 };
    const c = Coop.create({ Module: { HEAPU8: new Uint8Array(16) }, net });
    assert.deepEqual(c.missing, Coop.REQUIRED);
    c.tick();
    c.stop();
});
