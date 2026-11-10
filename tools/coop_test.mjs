// Co-op stage 1 (puppets) end to end: two real game instances in one co-op
// room, each running its own game; B sees A as a puppet Kirby.
//
//   node tools/coop_test.mjs build/katam-node.js <rom.gba>
//        [--url http://127.0.0.1:8787]   any v2 server (wrangler dev); default:
//                                        the dev relay, in-process
//        [--link-b 120] [--jitter 40]    B's one-way latency, + 0..J ms per
//                                        message (order kept, like TCP)
//        [--room 225 --cell 13:7]        where both boot (PortPlaytestBoot)
//        [--only follow,freezeA,freezeB,reconnect,drop]
//        [--json out.json]
//   node tools/coop_test.mjs --fake-engine     the same timeline against a fake
//                                        engine (no wasm, no ROM): checks the
//                                        harness and the transport only
//
// Never against the deployed worker by default: the live room is shared.
//
// The cast, paced at 59.73 Hz by a vsync shim (as tools/netplay_diag.mjs):
//   A  walks and jumps (scripted keys); creates the room
//   B  stands still; joins by code over the laggy link
//
// Phases and PASS rules (N = frames B's puppet may trail A: 15 at 120 ms):
//   follow     8 s of A moving.  The best lag L between A's own pose and the
//              pose B applies to puppet slot 1 is <= N; after the shift the
//              p95 position error is <= 2 px; no step > 8 px within a room;
//              A sends <= 32 msg/s; the engine's slot-1 Kirby is where the
//              applied pose says (gKirbys[1], when the build has it).
//   freezeA    A presents nothing for 5 s.  B's puppet stops moving, is
//              absent within 2.5 s, and after A is back B shows A within N
//              frames again (no stale backlog).
//   freezeB    B presents nothing for 5 s.  Within 30 frames of B coming back,
//              B's puppet is within N frames of A (it jumped to the newest).
//   reconnect  B's socket is closed at the room.  B is back with the same
//              mid and the puppet drawn within 3 s.
//   drop       B's link turns into a black hole both ways (no close).  The
//              liveness check (8 s) reconnects B: same mid, puppet back within
//              12 s.
// Missing PortCoop* exports in the build: says which, exits 2 (not a crash).

import { createRequire } from 'module';
import path from 'path';
import fs from 'fs';
import crypto from 'crypto';
import { fileURLToPath } from 'url';

const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const argv = process.argv.slice(2);
const flag = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 ? argv[i + 1] : d; };
const has = (k) => argv.includes('--' + k);
const BOOL = ['fake-engine'];
const pos = argv.filter((a, i) => !a.startsWith('--')
    && !(i > 0 && argv[i - 1].startsWith('--') && !BOOL.includes(argv[i - 1].slice(2))));
const FAKE = has('fake-engine');
const [modulePath, romPath] = pos;
if (!FAKE && (!modulePath || !romPath)) {
    console.error('usage: node tools/coop_test.mjs <katam-node.js> <rom.gba> [--url U] [--link-b MS] [--jitter MS]\n' +
                  '       node tools/coop_test.mjs --fake-engine');
    process.exit(2);
}
const LINK_B = +flag('link-b', 120);
const JITTER = +flag('jitter', 40);
const ROOM = +flag('room', 225);
const [CELL_X, CELL_Y] = flag('cell', '13:7').split(':').map(Number);
const ONLY = flag('only', 'follow,freezeA,freezeB,reconnect,drop').split(',');
const PERIOD = 1000 / 59.7275;
const N = Math.ceil(LINK_B / PERIOD) + 2 + 6;     // one way + batching + buffer (D at this jitter)
const BOOT_DONE = 1300 + 90;                    // PORT_PLAYTEST_WARP_FRAME + settle
const PX = 256;

const KatamNet = require(path.join(here, '../web/net_session.js'));
const Coop = require(path.join(here, '../web/coop.js'));

const t0 = performance.now();
const secs = () => +((performance.now() - t0) / 1000).toFixed(2);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(fn, ms) {
    const s = performance.now();
    while (!fn()) { if (performance.now() - s > ms) return false; await sleep(5); }
    return true;
}
const say = (t) => console.log(`[${secs().toFixed(2).padStart(6)}] ${t}`);

// --- the engine: real, or fake -----------------------------------------------
let createKatam = null, rom = null;
if (!FAKE) {
    createKatam = require(path.resolve(modulePath));
    rom = fs.readFileSync(romPath);
}

// --- room ---------------------------------------------------------------------
let relay = null, base = flag('url');
if (!base) {
    const { startRelay } = await import(path.join(here, '../netplay/dev-relay.mjs'));
    relay = await startRelay(0, { quiet: true });
    base = `http://127.0.0.1:${relay.port}`;
}

// --- vsync shim ------------------------------------------------------------------
// portPresent runs synchronously right before the instance registers its
// rAF, so `current` names the instance that owns the registration.  While
// an instance boots (frames < BOOT_DONE) it runs unpaced.
let current = null;
global.requestAnimationFrame = (cb) => {
    const inst = current;
    if (!inst) { setTimeout(cb, 1); return; }
    if (inst.turbo()) { setImmediate(() => { current = inst; cb(performance.now()); }); return; }
    inst.raf = cb;
};
const instances = [];
let tick = 0;
setInterval(() => {
    const due = Math.floor((performance.now() - t0) / PERIOD);
    while (tick < due) {
        tick++;
        for (const inst of instances) {
            if (inst.frozen) continue;
            if (inst.fake) { inst.fakeFrame(); continue; }
            if (!inst.raf) continue;
            const cb = inst.raf;
            inst.raf = null;
            cb(performance.now());
        }
    }
}, 2);

// --- a WebSocket with latency, order-preserving jitter, and a black hole -----------
function laggyWebSocket(ms, jitter, holder) {
    return class extends WebSocket {
        constructor(u) {
            super(u);
            const self = this;
            let handler = null;
            holder.sock = this;
            const fifo = (deliver) => {
                const q = [];
                let last = 0, armed = false;
                const pump = () => {
                    armed = false;
                    const now = performance.now();
                    while (q.length && q[0].at <= now + 0.5) deliver(q.shift().v);
                    if (q.length) { armed = true; setTimeout(pump, Math.max(0, q[0].at - performance.now())); }
                };
                return (v) => {
                    if (holder.blackhole) return;
                    last = Math.max(last, performance.now() + ms + Math.random() * jitter);
                    q.push({ at: last, v });
                    if (!armed) { armed = true; setTimeout(pump, Math.max(0, q[0].at - performance.now())); }
                };
            };
            const inq = fifo((e) => { if (!holder.blackhole && handler) handler.call(self, e); });
            super.onmessage = (e) => inq(e);
            Object.defineProperty(this, 'onmessage', { set(f) { handler = f; }, get() { return handler; } });
            this._outq = fifo((d) => { if (this.readyState === 1 && !holder.blackhole) super.send(d); });
        }
        send(d) { this._outq(d); }
    };
}

// --- inputs ---------------------------------------------------------------------
// Walks right and left in 2 s legs, jumping now and then: always moving.
const RIGHT = 0x10, LEFT = 0x20, JUMP = 0x01;
function walker(f) {
    const leg = Math.floor(f / 120) % 2;
    return (leg ? LEFT : RIGHT) | (f % 97 < 12 ? JUMP : 0);
}

// --- reading poses ----------------------------------------------------------------
const KIRBY = 0x02020EE0, KSIZE = 424;
function kirbyPos(Module, k) {
    const dv = new DataView(Module.HEAPU8.buffer);
    const a = KIRBY + KSIZE * k;
    return { x: dv.getInt32(a + 0x40, true), y: dv.getInt32(a + 0x44, true), room: dv.getUint16(a + 0x60, true) };
}
const hdr = (b) => b ? { x: Coop.poseX(b), y: Coop.poseY(b), room: Coop.poseRoom(b), hidden: Coop.poseHidden(b) } : null;

// --- a fake engine (--fake-engine): the exports, a Kirby that walks ----------------
function fakeEngine(inst) {
    const size = 36, HEAPU8 = new Uint8Array(4096), SCR = 1024;
    const st = { x: 100 * PX, y: 80 * PX, room: ROOM, frame: 0, keys: 0, puppets: {} };
    const write = (p, s) => {
        const dv = new DataView(HEAPU8.buffer);
        dv.setInt32(p, s.x, true); dv.setInt32(p + 4, s.y, true); dv.setUint16(p + 8, s.room, true);
        HEAPU8[p + 10] = 0; HEAPU8[p + 11] = 1;
    };
    const M = {
        HEAPU8, st,
        _PortFrameNumber: () => st.frame,
        _PortSetKeys: (k) => { st.keys = k; },
        _PortCoopPoseSize: () => size, _PortCoopEnable: () => {}, _PortCoopReady: () => 1,
        _PortCoopSetPuppet: (s, on) => { st.puppets[s] = on ? { x: 0, y: 0, room: 0, absent: true } : null; },
        _PortCoopSetPose: (s, p) => { const q = st.puppets[s]; if (!q) return; Object.assign(q, hdr(HEAPU8.slice(p, p + size)), { absent: false }); },
        _PortCoopSetAbsent: (s) => { const q = st.puppets[s]; if (q) q.absent = true; },
        _PortCoopLocalPose: (p) => { write(p, st); return 1; },
        _PortCoopPoseBuffer: () => SCR,
    };
    inst.fake = true;
    inst.fakeFrame = () => {
        st.frame++;
        if (st.keys & RIGHT) st.x += 2 * PX;
        if (st.keys & LEFT) st.x -= 2 * PX;
        st.y = 80 * PX - Math.round(Math.abs(Math.sin(st.frame / 12)) * 12 * PX);
        M.portPresent();
    };
    // engine puppet position: what SetPose was given (applied a frame later in the real one)
    inst.enginePuppet = (s) => st.puppets[s] && !st.puppets[s].absent ? { ...st.puppets[s] } : null;
    return M;
}

// --- an instance -------------------------------------------------------------------
const ROMHASH = rom ? crypto.createHash('sha1').update(rom).digest('hex') : 'fake-rom';

async function makeInstance(name, { code, linkMs = 0, jitter = 0, script = null }) {
    const inst = { name, raf: null, frozen: false, logs: [], track: [], link: { sock: null, blackhole: false } };
    const log = (t) => { inst.logs.push(`[${secs()}] [${name}] ${t}`); if (inst.logs.length > 3000) inst.logs.shift(); };
    let Module;
    if (FAKE) {
        Module = fakeEngine(inst);
        inst.turbo = () => false;
    } else {
        let resolveRom;
        Module = {
            portRomReady: new Promise((r) => { resolveRom = r; }),
            portAudioRate: 0,
            print: log, printErr: log, portDiag: (t) => log(t),
            onRuntimeInitialized() {
                Module.HEAPU8.set(rom, 0x08000000);
                Module._PortRomLoaded(rom.length);
                Module._PortPlaytestBoot(ROOM, CELL_X, CELL_Y);
                resolveRom();
            },
        };
        inst.turbo = () => !!Module._PortFrameNumber && Module._PortFrameNumber() < BOOT_DONE;
        inst.enginePuppet = (s) => kirbyPos(Module, s);
    }
    Module.portPresent = function () {
        current = inst;
        const f = Module._PortFrameNumber();
        if (script) Module._PortSetKeys(script(f));
        if (inst.coop) {
            inst.coop.tick();
            const t = performance.now();
            // A's own pose, as the engine reports it (what it sends)
            let mine = null;
            if (inst.coop.missing.length === 0) {
                const p = Module._PortCoopPoseBuffer() + 7 * inst.coop.size;
                if (Module._PortCoopLocalPose(p)) mine = hdr(Module.HEAPU8.slice(p, p + inst.coop.size));
            }
            inst.track.push({ t, f, mine, applied: hdr(inst.coop.applied(1)), engine: inst.enginePuppet ? inst.enginePuppet(1) : null,
                              ready: !!Module._PortCoopReady() });
            if (inst.track.length > 60 * 120) inst.track.splice(0, 60 * 30);
        }
    };
    inst.Module = Module;
    if (!FAKE) {
        instances.push(inst);
        await createKatam(Module);
        current = inst;
    } else {
        instances.push(inst);
    }
    const missing = Coop.REQUIRED.filter((n) => typeof Module[n] !== 'function');
    if (missing.length) {
        console.log(`SKIP: ${modulePath || 'build'} has no co-op engine exports (${missing.join(', ')}).`);
        console.log('      Rebuild build/katam-node.js once the engine half (platform/coop.c, PORT_EXPORTS) has landed.');
        process.exit(2);
    }
    inst.net = KatamNet.connect({
        base, code, log, liveness: 8000, backoff: KatamNet.jitteredBackoff,
        WebSocket: laggyWebSocket(linkMs, jitter, inst.link),
        hello: { sim: Coop.SIM, rom: ROMHASH, content: 'vanilla', name, platform: 'node',
                 caps: { seats: 4, sync: false }, want: 'play' },
    });
    inst.net.on('error', (e) => say(`${name}: room error: ${KatamNet.explainError(e)}`));
    inst.coop = Coop.create({ Module, net: inst.net, log });
    return inst;
}

// --- run ----------------------------------------------------------------------------
const results = [];
function check(phase, what, ok, detail = '') {
    results.push({ phase, what, ok, detail });
    say(`${ok ? 'ok  ' : 'FAIL'} ${phase}: ${what}${detail ? ' -- ' + detail : ''}`);
}

const created = await KatamNet.createRoom(base);
if (!created.code) { console.error('cannot create a room:', created); process.exit(1); }
const code = created.code;
say(`room ${code} on ${base}${FAKE ? ' (fake engine)' : ''}; B link ${LINK_B} ms + ${JITTER} jitter; N = ${N} frames`);
const A = await makeInstance('A', { code, script: walker });
const B = await makeInstance('B', { code, linkMs: LINK_B, jitter: JITTER, script: () => 0 });

const both = () => A.net.state === 'open' && B.net.state === 'open';
const booted = (i) => i.track.length && i.track.at(-1).ready;
if (!await until(() => both() && booted(A) && booted(B), 90000)) {
    say(`did not get both into the room and into gameplay (A ${A.net.state} ready ${booted(A)}, B ${B.net.state} ready ${booted(B)})`);
    console.log([...A.logs.slice(-15), ...B.logs.slice(-15)].join('\n'));
    process.exit(1);
}
// The playtest warp is queued at frame 1300 but lands when the game allows it; wait for it, or the
// follow phase measures the teleport itself.
if (!FAKE) {
    const warped = (i) => i.logs.some((l) => l.includes('warp: player 0'));
    if (!await until(() => warped(A) && warped(B), 30000)) say('warning: no warp seen on both instances');
    await sleep(1500);
}
say(`both in gameplay; A mid ${A.net.mid}, B mid ${B.net.mid}, features ${JSON.stringify(A.net.features)}`);
await until(() => B.coop.slotOf(A.net.mid) === 1 && B.track.at(-1).applied, 5000);

// A's own pose at time t (nearest earlier sample)
function aAt(t) {
    const tr = A.track;
    let lo = 0, hi = tr.length - 1;
    if (!tr.length || tr[0].t > t) return null;
    while (lo < hi) { const m = (lo + hi + 1) >> 1; if (tr[m].t <= t) lo = m; else hi = m - 1; }
    const a = tr[lo].mine, b = tr[lo + 1] && tr[lo + 1].mine;
    if (!a || !b || a.hidden || b.hidden || a.room !== b.room) return a;
    const f = Math.min(1, (t - tr[lo].t) / Math.max(1e-3, tr[lo + 1].t - tr[lo].t));
    return { ...a, x: a.x + (b.x - a.x) * f, y: a.y + (b.y - a.y) * f };
}
// best lag in frames over B's samples in [from, to)
function lagOver(from, to) {
    const bs = B.track.filter((s) => s.t >= from && s.t < to && s.applied && !s.applied.hidden);
    let best = null;
    for (let L = 0; L <= 40; L += 0.25) {
        const errs = [];
        for (const s of bs) {
            const a = aAt(s.t - L * PERIOD);
            if (!a || a.hidden || a.room !== s.applied.room) continue;
            errs.push(Math.hypot(a.x - s.applied.x, a.y - s.applied.y) / PX);
        }
        if (errs.length < bs.length * 0.8 || !errs.length) continue;
        errs.sort((x, y) => x - y);
        const p95 = errs[Math.floor(errs.length * 0.95)];
        if (!best || p95 < best.p95) best = { L, p95, n: errs.length };
    }
    return best;
}
function maxStep(from, to, track = B.track, key = 'applied') {
    let m = 0, prev = null, at = 0, room = -1;
    for (const s of track) {
        if (s.t < from || s.t >= to) continue;
        const a = s[key];
        if (prev && a && !a.hidden && !prev.hidden && a.room === prev.room) {
            const d = Math.hypot(a.x - prev.x, a.y - prev.y) / PX;
            if (d > m) { m = d; at = s.t; room = a.room; }
        }
        prev = a;
    }
    maxStep.at = at; maxStep.room = room;
    return m;
}

async function follow() {
    const sent0 = A.coop.stats.sent, ts = performance.now();
    await sleep(8000);
    const te = performance.now();
    const rate = (A.coop.stats.sent - sent0) / ((te - ts) / 1000);
    const lag = lagOver(ts + 1000, te);
    check('follow', `B's puppet follows A within ${N} frames`, !!lag && lag.L <= N, lag ? `lag ${lag.L} frames, p95 error ${lag.p95.toFixed(2)} px over ${lag.n} frames` : 'no comparable samples');
    check('follow', 'p95 position error after the shift <= 2 px', !!lag && lag.p95 <= 2, lag ? lag.p95.toFixed(2) + ' px' : '');
    const step = maxStep(ts + 1000, te);
    const stepAt = maxStep.at, stepRoom = maxStep.room;
    const own = maxStep(ts, te, A.track, 'mine');
    check('follow', 'no step > 8 px within a room', step <= 8, `max ${step.toFixed(1)} px/frame at +${((stepAt - ts) / 1000).toFixed(2)} s room ${stepRoom}; A's own max ${own.toFixed(1)} at +${((maxStep.at - ts) / 1000).toFixed(2)} s room ${maxStep.room}`);
    check('follow', 'A sends <= 32 msg/s', rate <= 32 && rate > 10, `${rate.toFixed(1)} msg/s, ${A.coop.stats.sentPoses} poses in ${A.coop.stats.sent} messages`);
    // the engine draws the puppet where it was told (applied a frame later)
    let worst = [];
    const tr = B.track.filter((s) => s.t >= ts + 1000 && s.t < te);
    for (let i = 2; i < tr.length; i++) {
        const e = tr[i].engine;
        if (!e) continue;
        let d = Infinity;
        for (let k = 0; k <= 2; k++) {
            const a = tr[i - k].applied;
            if (a && !a.hidden) d = Math.min(d, Math.hypot(e.x - a.x, e.y - a.y) / PX);
        }
        if (d < Infinity) worst.push(d);
    }
    worst.sort((x, y) => x - y);
    const p95 = worst.length ? worst[Math.floor(worst.length * 0.95)] : null;
    check('follow', 'the engine\'s slot-1 Kirby is at the applied pose (p95 <= 1 px)', p95 !== null && p95 <= 1,
          p95 === null ? 'no engine samples' : `p95 ${p95.toFixed(2)} px over ${worst.length} frames`);
}

async function freezeA() {
    const ts = performance.now();
    A.frozen = true;
    await sleep(400);
    const x0 = B.track.at(-1).applied;
    const gone = await until(() => !B.track.at(-1).applied, 3000);
    const tAbs = (performance.now() - ts) / 1000;
    check('freezeA', 'B\'s puppet holds, then is absent within 2.5 s', gone && tAbs <= 2.5 && !!x0, `absent after ${tAbs.toFixed(2)} s`);
    await sleep(5000 - (performance.now() - ts));
    A.frozen = false;
    const tb = performance.now();
    const back = await until(() => B.track.at(-1).applied, 3000);
    check('freezeA', 'puppet back after A resumes', back, `${((performance.now() - tb) / 1000).toFixed(2)} s`);
    await sleep(1500);
    const lag = lagOver(tb + 500, performance.now());
    check('freezeA', `then within ${N} frames of A (no stale backlog)`, !!lag && lag.L <= N, lag ? `lag ${lag.L}, p95 ${lag.p95.toFixed(2)} px` : 'none');
}

async function freezeB() {
    B.frozen = true;
    await sleep(5000);
    B.frozen = false;
    const tb = performance.now();
    await sleep(30 * PERIOD);
    // lag over the first frames after B resumes
    const lag = lagOver(tb, performance.now());
    check('freezeB', `within 30 frames of B resuming, B shows A within ${N} frames`, !!lag && lag.L <= N,
          lag ? `lag ${lag.L}, p95 ${lag.p95.toFixed(2)} px` : 'none');
}

function bServerSocket() {
    if (!relay) return null;
    for (const r of relay.v2.rooms.values())
        for (const [id, ws] of r.socks)
            if (r.core.conns.get(id) && r.core.conns.get(id).mid === B.net.mid) return ws;
    return null;
}

async function reconnect() {
    const ws = bServerSocket();
    if (!ws) { check('reconnect', 'B\'s room socket found', false, '--url given: needs the in-process relay'); return; }
    const mid = B.net.mid, rc = B.net.reconnects;
    const ts = performance.now();
    ws.close(1012, 'test');
    await until(() => B.net.reconnects > rc, 3000);
    const ok = await until(() => B.net.state === 'open' && B.net.reconnects > rc && B.track.at(-1).applied && B.coop.slotOf(A.net.mid) === 1, 5000);
    const dt = (performance.now() - ts) / 1000;
    check('reconnect', 'clean close: back, same mid, puppet drawn within 3 s', ok && B.net.mid === mid && dt <= 3,
          `${dt.toFixed(2)} s, mid ${mid} -> ${B.net.mid}`);
}

async function drop() {
    const mid = B.net.mid, dead = B.net.deadSockets;
    const ts = performance.now();
    B.link.blackhole = true;
    const noticed = await until(() => B.net.deadSockets > dead, 11000);
    const tn = (performance.now() - ts) / 1000;
    B.link.blackhole = false;
    check('drop', 'silent drop noticed by the liveness check', noticed, `after ${tn.toFixed(2)} s`);
    const ok = await until(() => B.net.state === 'open' && B.track.at(-1).applied && B.coop.slotOf(A.net.mid) === 1, 6000);
    const dt = (performance.now() - ts) / 1000;
    check('drop', 'back with the same mid, puppet drawn within 12 s', ok && B.net.mid === mid && dt <= 12,
          `${dt.toFixed(2)} s, mid ${mid} -> ${B.net.mid}`);
}

const phases = { follow, freezeA, freezeB, reconnect, drop };
for (const p of ONLY) {
    if (!phases[p]) { console.error('unknown phase ' + p); process.exit(2); }
    say(`--- ${p}`);
    await phases[p]();
    await sleep(1500);
}

const fails = results.filter((r) => !r.ok);
if (flag('json')) fs.writeFileSync(flag('json'), JSON.stringify({ code, results, A: A.coop.stats, B: B.coop.stats }, null, 2));
say(`${fails.length ? 'FAIL' : 'PASS'}: ${results.length - fails.length}/${results.length} checks`);
if (fails.length) console.log([...A.logs.slice(-10), ...B.logs.slice(-20)].join('\n'));
A.net.close(); B.net.close();
if (relay) relay.close();
process.exit(fails.length ? 1 : 0);
