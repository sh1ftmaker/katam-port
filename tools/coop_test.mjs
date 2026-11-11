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
//   enemies    stage 3, shared enemies.  Both warp into a room with
//              enemies (--enemy-room 104 --enemy-cell 2:9) and stand still.
//              A (lowest member id) owns the room: B's copies of A's enemies
//              become puppets within 2 s and sit where A had them within the
//              last second (<= 2 px: they are played back late, like Kirbys);
//              an enemy killed on A dies on B within 1.5 s; a hit on one of
//              B's puppets takes the same hp off A's copy.
//   bosses     stage 4, shared boss health.  Both warp next to a miniboss
//              (--boss-room 230 --boss-cell 24:13, type 0x38).  Its fight has
//              begun in both games, so B's copy is a puppet of A's, where
//              A's was a moment ago, and A's goes after the nearer player.
//              Damage on A's copy comes off B's, damage on B's comes off A's
//              (through A's own hit reaction), and lethal damage on A kills
//              B's copy.
//   helpers    CPU Kirbys.  A (lowest member id, in a level, past its
//              tutorial) runs them: B's free slots show A's CPU Kirbys,
//              each within 2 px of where A had it in the last 40 frames
//              and in the same room, and B runs no CPU Kirby of its own.
//              A back in its tutorial hands them to B (B's slots go back to
//              its own CPUs); A past it again takes them back; a warp does
//              not move them.  B in another room presses R (the phone): A's
//              game sends its CPU Kirbys there, and B sees them arrive.
//   fx         spawned objects.  A attacks (B held: inhale, then a copied
//              ability's attack); what A's attack spawns is captured on A
//              and drawn on B as ghosts (engine counters on both sides).
//              A's sounds play on B, and the inhale's stop stops B's copy.
//   room       the live room.  Both in room 106 next to its star blocks: A
//              inhales them, and the blocks A breaks break in B's game;
//              B leaves and comes back, and gets the room as A has it.  A
//              takes Beam and lets it go: B's game makes the star too, with
//              A's ability; B swallowing its copy removes A's.  In room 310
//              A stands on the small button: it is down in B's game too.
//   shots      enemies after any player, and their shots hurt everyone.
//              Both in a room with Shotzos (--shot-room 113), B near one,
//              A further off; A owns the room.  A's game aims that Shotzo
//              at B's puppet, and a bullet of A's hurts B in B's own game
//              (B's Shotzos are puppets: B fired none of its own).
//   progress   stage 2, shared progress.  Both files are moved past the
//              tutorial (gAIKirbyState 300) and progress is written straight
//              into memory, as the game's own collectors do: a shard and a
//              chest on A reach B, a big switch on B reaches A, a vitality
//              chest on A gives B one heart (max HP follows), the same
//              vitality chest opened on both at once gives each one heart,
//              not two, and a third player C joining with progress of their
//              own ends with everyone holding the union (C gets both hearts
//              from the full record).  Skipped when the build has no
//              PortCoopWorld* exports.
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
const ONLY = flag('only', 'follow,freezeA,freezeB,reconnect,drop,enemies,bosses,progress,colors,helpers,fx,shots,room').split(',');
const BOSS_ROOM = +flag('boss-room', 230);
const [BOSS_CX, BOSS_CY] = flag('boss-cell', '24:13').split(':').map(Number);
const SHOT_ROOM = +flag('shot-room', 113);
const [SHOT_AX, SHOT_AY] = flag('shot-a', '3:5').split(':').map(Number);
const [SHOT_BX, SHOT_BY] = flag('shot-b', '13:5').split(':').map(Number);
const TILE_ROOM = +flag('tile-room', 106);
const [TILE_AX, TILE_AY] = flag('tile-a', '43:9').split(':').map(Number);
const [TILE_BX, TILE_BY] = flag('tile-b', '38:9').split(':').map(Number);
const BTN_ROOM = +flag('button-room', 310);
const [BTN_AX, BTN_AY] = flag('button-a', '20:9').split(':').map(Number);
const [BTN_BX, BTN_BY] = flag('button-b', '8:9').split(':').map(Number);
const EN_ROOM = +flag('enemy-room', 104);
const [EN_CX, EN_CY] = flag('enemy-cell', '2:9').split(':').map(Number);
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
    const inst = { name, raf: null, frozen: false, logs: [], track: [], link: { sock: null, blackhole: false }, script };
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
        if (inst.script) Module._PortSetKeys(inst.script(f));
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
                              ready: !!Module._PortCoopReady(),
                              k: FAKE ? null : [1, 2, 3].map((s) => kirbyPos(Module, s)) });
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
    /* The playtest boot is a new file, still in its tutorial; puppets and
     * shared progress start once a file is past it (gAIKirbyState 300). */
    for (const i of [A, B]) { i.Module.HEAPU8[0x0203AD2C] = 300 & 255; i.Module.HEAPU8[0x0203AD2D] = 300 >> 8; }
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

// --- stage 2: shared progress ----------------------------------------------------
const W = { story: 0x0203AD2C, treasures: 0x02038960, slots: 0x02028C10 };
const u8at = (i, a) => i.Module.HEAPU8[a];
const u16at = (i, a) => i.Module.HEAPU8[a] | (i.Module.HEAPU8[a + 1] << 8);
const set16 = (i, a, v) => { i.Module.HEAPU8[a] = v & 255; i.Module.HEAPU8[a + 1] = (v >> 8) & 255; };
const orAt = (i, a, bits) => { i.Module.HEAPU8[a] |= bits; };
const chestAddr = (id) => W.treasures + (id >> 3);
const hasChest = (i, id) => !!(u8at(i, chestAddr(id)) & (1 << (id & 7)));
const shardOf = (i) => u8at(i, W.treasures + 0x10);
const vitOf = (i) => u16at(i, W.treasures + 0x20);
const maxHpOf = (i) => { const v = i.Module.HEAPU8[KIRBY + 0x101]; return v > 127 ? v - 256 : v; };
function giveVitality(i, id) { orAt(i, chestAddr(id), 1 << (id & 7)); set16(i, W.treasures + 0x20, vitOf(i) + 1); }
const noticed = (i, re) => i.logs.some((l) => re.test(l));

async function progress() {
    if (FAKE || Coop.WORLD.some((n) => typeof A.Module[n] !== 'function')) {
        say('progress: build has no PortCoopWorld* exports -- skipped');
        return;
    }
    const ids = new Uint8Array(16), buf = A.Module._malloc(16);
    const nv = A.Module._PortCoopWorldVitalityIds(buf, 16);
    ids.set(A.Module.HEAPU8.slice(buf, buf + 16));
    A.Module._free(buf);
    check('progress', 'the ROM has vitality chests', nv >= 2, `${nv}: ${[...ids.slice(0, Math.min(nv, 16))].join(' ')}`);
    if (nv < 2) return;
    const [v0, v1] = ids;
    for (const i of [A, B]) set16(i, W.story, 300);
    const tracking = await until(() => A.Module._PortCoopWorldTracking() && B.Module._PortCoopWorldTracking(), 3000);
    check('progress', 'both files tracked once past the tutorial', tracking);
    await until(() => A.coop.worldStats.sentFull > 0 && B.coop.worldStats.sentFull > 0, 3000);
    await sleep(500);

    // A finds a shard and a chest
    orAt(A, W.treasures + 0x10, 1 << 2);
    orAt(A, chestAddr(5), 1 << 5);
    let ok = await until(() => (shardOf(B) & 4) && hasChest(B, 5), 2000);
    check('progress', 'A\'s shard and chest reach B', !!ok, `B shards ${shardOf(B).toString(2)}`);
    check('progress', 'B is told about the shard', await until(() => noticed(B, /found a Mirror Shard/), 1000));

    // B presses a big switch (world state slot 3)
    orAt(B, W.slots + 3 * 4, 1);
    ok = await until(() => u8at(A, W.slots + 12) === 1, 2000);
    check('progress', 'B\'s big switch reaches A', !!ok);

    // a vitality chest on A
    const va = vitOf(A), vb = vitOf(B);
    giveVitality(A, v0);
    ok = await until(() => vitOf(B) === vb + 1 && hasChest(B, v0), 2000);
    await sleep(200);
    check('progress', 'A\'s vitality chest gives B one heart', !!ok && vitOf(B) === vb + 1, `B ${vb} -> ${vitOf(B)}, A ${va} -> ${vitOf(A)}`);
    check('progress', 'B\'s max HP follows', maxHpOf(B) === vitOf(B) + 6, `maxHp ${maxHpOf(B)}`);

    // the same vitality chest opened on both at once
    const va2 = vitOf(A), vb2 = vitOf(B);
    giveVitality(A, v1);
    giveVitality(B, v1);
    await sleep(1500);
    check('progress', 'one chest opened on both at once: one heart each', vitOf(A) === va2 + 1 && vitOf(B) === vb2 + 1,
          `A ${va2} -> ${vitOf(A)}, B ${vb2} -> ${vitOf(B)}`);

    // C joins with progress of its own
    const C = await makeInstance('C', { code, script: () => 0 });
    if (!await until(() => C.net.state === 'open' && booted(C) && C.logs.some((l) => l.includes('warp: player 0')), 90000)) {
        check('progress', 'C joins and reaches gameplay', false);
        C.net.close();
        return;
    }
    await sleep(1000);
    const vc = vitOf(C);
    set16(C, W.story, 300);
    orAt(C, W.treasures + 0x10, 1 << 6);
    orAt(C, chestAddr(70), 1 << (70 & 7));
    ok = await until(() => [A, B, C].every((i) => (shardOf(i) & 0x44) === 0x44 && hasChest(i, 70) && hasChest(i, 5)
                                                && hasChest(i, v0) && hasChest(i, v1) && u8at(i, W.slots + 12) === 1), 5000);
    check('progress', 'after C joins, all three hold the union', !!ok,
          [A, B, C].map((i) => `${i.name}: shards ${shardOf(i).toString(2)} vit ${vitOf(i)}`).join('; '));
    check('progress', 'C gets both hearts from the full record, once', vitOf(C) === vc + 2, `C ${vc} -> ${vitOf(C)}`);
    await sleep(1000);
    check('progress', 'and keeps them (a second full record adds nothing)', vitOf(C) === vc + 2 && vitOf(A) === va2 + 1,
          `C ${vitOf(C)}, A ${vitOf(A)}`);
    say(`progress stats: A ${JSON.stringify(A.coop.worldStats)}; C ${JSON.stringify(C.coop.worldStats)}`);
    C.net.close();
    instances.splice(instances.indexOf(C), 1);
}

// --- stage 3: shared enemies --------------------------------------------------------
function enemyList(i) {
    const M = i.Module, buf = M._malloc(20 * 64);
    const n = M._PortCoopEnemyList(buf, 64);
    const b = M.HEAPU8.slice(buf, buf + n * 20);
    M._free(buf);
    const dv = new DataView(b.buffer), out = [];
    for (let k = 0; k < n; k++) {
        const o = k * 20;
        out.push({ idx: dv.getUint16(o, true), x: dv.getInt32(o + 2, true), y: dv.getInt32(o + 6, true),
                   hp: dv.getInt16(o + 10, true), anim: b[o + 12], flags: b[o + 13], puppet: !!(b[o + 13] & 0x80),
                   dying: !!(b[o + 13] & 0x40), addr: dv.getUint32(o + 14, true), type: dv.getUint16(o + 18, true) });
    }
    return out;
}
const roomOf = (i) => kirbyPos(i.Module, 0).room;
function setHp(i, addr, hp) { const dv = new DataView(i.Module.HEAPU8.buffer); dv.setInt16(addr + 0x80, hp, true); }
function killObj(i, addr) {
    const dv = new DataView(i.Module.HEAPU8.buffer);
    dv.setInt16(addr + 0x80, 0, true);
    dv.setUint32(addr + 0x0C, dv.getUint32(addr + 0x0C, true) | 0x2000, true);
}

async function enemies() {
    if (FAKE || Coop.ENEMY.some((n) => typeof A.Module[n] !== 'function')) {
        say('enemies: build has no PortCoopEnemy* exports -- skipped');
        return;
    }
    A.script = B.script = () => 0;
    for (const i of [A, B]) i.Module._PortWarp(0, EN_ROOM, EN_CX, EN_CY);
    const there = await until(() => roomOf(A) === EN_ROOM && roomOf(B) === EN_ROOM, 8000);
    check('enemies', `both warped into room ${EN_ROOM}`, there, `A ${roomOf(A)} B ${roomOf(B)}`);
    if (!there) return;
    const live = (l) => l.filter((e) => e.idx !== 0xFFFF && !e.dying);
    const have = await until(() => live(enemyList(A)).length > 0 && live(enemyList(B)).length > 0, 6000);
    say(`A's enemies: ${JSON.stringify(enemyList(A).map((e) => [e.idx, e.type, e.x >> 8, e.y >> 8, e.hp]))}`);
    check('enemies', 'both games have the room\'s enemies', have);
    if (!have) return;
    const owners = await until(() => A.coop.enemyOwner() === A.net.mid && B.coop.enemyOwner() === A.net.mid, 4000);
    check('enemies', 'both agree A owns the room', owners, `A says ${A.coop.enemyOwner()}, B says ${B.coop.enemyOwner()}, A is ${A.net.mid}`);
    const pup = await until(() => live(enemyList(B)).some((e) => e.puppet), 3000);
    /* B plays A's enemies back a link's latency plus a few frames late, so a
     * puppet is compared with where A's copy was during the last second */
    const hist = [];
    for (const te = performance.now() + 1000; performance.now() < te;) {
        hist.push({ t: performance.now(), l: live(enemyList(A)) });
        await sleep(8);
    }
    const la = live(enemyList(A)), lb = live(enemyList(B));
    const pairs = lb.filter((e) => e.puppet).map((e) => ({ b: e, a: la.find((x) => x.idx === e.idx) })).filter((p) => p.a);
    const errs = pairs.map((p) => Math.min(...hist.map((h) => {
        const a = h.l.find((x) => x.idx === p.b.idx);
        return a ? Math.hypot(a.x - p.b.x, a.y - p.b.y) / PX : Infinity;
    })));
    check('enemies', 'B\'s copies of A\'s enemies are puppets', pup && pairs.length > 0,
          `${pairs.length} of ${lb.length} on B, ${la.length} on A; stats ${JSON.stringify(B.coop.enemyStats)}`);
    check('enemies', 'and sit where A had them a moment ago (<= 2 px)', errs.length > 0 && Math.max(...errs) <= 2,
          errs.map((e) => e.toFixed(1)).join(' ') + ' px');
    if (!pairs.length) return;

    // a hit on one of B's puppets reaches A's copy
    const hp0 = live(enemyList(A)).find((e) => e.idx === pairs[0].a.idx);
    const target = pairs.find((p) => p.a.hp >= 2) || pairs[0];
    const before = la.find((e) => e.idx === target.a.idx).hp;
    setHp(B, target.b.addr, target.b.hp - 1);
    const hit = await until(() => { const e = enemyList(A).find((x) => x.idx === target.a.idx); return e && e.hp === before - 1; }, 2000);
    check('enemies', 'a hit on B\'s puppet takes the hp off A\'s copy', hit,
          `A hp ${before} -> ${(enemyList(A).find((x) => x.idx === target.a.idx) || {}).hp}; B stats ${JSON.stringify(B.coop.enemyStats)}`);
    void hp0;

    // a kill on A kills B's copy
    const victim = live(enemyList(A)).find((e) => live(enemyList(B)).some((x) => x.idx === e.idx));
    if (!victim) { check('enemies', 'an enemy both games have, to kill', false); return; }
    killObj(A, victim.addr);
    const died = await until(() => !live(enemyList(B)).some((x) => x.idx === victim.idx), 1500);
    check('enemies', 'an enemy killed on A dies on B within 1.5 s', died,
          `idx ${victim.idx}; A stats ${JSON.stringify(A.coop.enemyStats)}; B kills in ${B.Module._PortCoopEnemyStat(3)}`);
    A.script = walker;
    B.script = () => 0;
}

async function bosses() {
    if (FAKE || Coop.ENEMY.some((n) => typeof A.Module[n] !== 'function')) {
        say('bosses: build has no PortCoopEnemy* exports -- skipped');
        return;
    }
    A.script = B.script = () => 0;
    A.Module._PortWarp(0, BOSS_ROOM, BOSS_CX, BOSS_CY);
    B.Module._PortWarp(0, BOSS_ROOM, BOSS_CX + 2, BOSS_CY);     /* B a little nearer to it */
    const there = await until(() => roomOf(A) === BOSS_ROOM && roomOf(B) === BOSS_ROOM, 8000);
    check('bosses', `both warped into room ${BOSS_ROOM}`, there, `A ${roomOf(A)} B ${roomOf(B)}`);
    if (!there) return;
    const boss = (i) => enemyList(i).find((e) => e.type >= 0x38 && e.type <= 0x52 && e.hp > 0);
    const both = await until(() => boss(A) && boss(B), 8000);
    check('bosses', 'both games have the miniboss', !!both,
          `A ${JSON.stringify(enemyList(A).map((e) => [e.idx, e.type, e.hp]))} B ${JSON.stringify(enemyList(B).map((e) => [e.idx, e.type, e.hp]))}`);
    if (!both) return;
    await sleep(500);
    let a = boss(A), b = boss(B);
    const flagsOf = (i, x) => new DataView(i.Module.HEAPU8.buffer).getUint32(x.addr + 8, true);
    say(`bosses: A's ${JSON.stringify(a)} flags ${flagsOf(A, a).toString(16)}; B's ${JSON.stringify(b)} flags ${flagsOf(B, b).toString(16)}`);
    /* a miniboss whose fight has begun is shared like an enemy: B's copy is
     * a puppet of A's, where A's was a moment ago */
    const pup = await until(() => boss(B) && boss(B).puppet, 3000);
    const hist = [];
    for (const te = performance.now() + 1500; performance.now() < te;) {
        const x = boss(A);
        if (x) hist.push({ x: x.x, y: x.y });
        await sleep(8);
    }
    b = boss(B);
    const err = b ? Math.min(...hist.map((h) => Math.hypot(h.x - b.x, h.y - b.y) / PX)) : Infinity;
    const moved = hist.length ? Math.hypot(hist[0].x - hist.at(-1).x, hist[0].y - hist.at(-1).y) / PX : 0;
    check('bosses', 'the miniboss is shared: B\'s copy is a puppet of A\'s, where A\'s was a moment ago (<= 2 px)', pup && err <= 2,
          `puppet ${pup}, error ${err.toFixed(1)} px, A's moved ${moved.toFixed(0)} px in 1.5 s`);
    a = boss(A);
    const ka = kirbyPos(A.Module, 0), kb = kirbyPos(B.Module, 0);
    const bSlot = A.coop.cpuState().slots.indexOf(B.net.mid);
    const nearer = Math.abs(kb.x - a.x) + Math.abs(kb.y - a.y) < Math.abs(ka.x - a.x) + Math.abs(ka.y - a.y) ? bSlot : 0;
    const pick = A.Module._PortCoopTestTarget(a.addr);
    check('bosses', 'A\'s miniboss goes after whichever player is nearer', pick === nearer,
          `picks slot ${pick}, nearer is slot ${nearer} (B is slot ${bSlot})`);
    a = boss(A); b = boss(B);
    const hb = b.hp;
    setHp(A, a.addr, a.hp - 2);
    let ok = await until(() => { const x = boss(B); return x && x.hp === hb - 2; }, 2000);
    check('bosses', 'damage on A\'s copy comes off B\'s', ok, `B ${hb} -> ${(boss(B) || {}).hp}`);
    a = boss(A); b = boss(B);
    const ha = a.hp;
    setHp(B, b.addr, b.hp - 1);
    ok = await until(() => { const x = boss(A); return x && x.hp === ha - 1; }, 2000);
    check('bosses', 'damage on B\'s copy comes off A\'s', ok, `A ${ha} -> ${(boss(A) || {}).hp}`);
    a = boss(A);
    setHp(A, a.addr, 0);
    ok = await until(() => !boss(B), 3000);
    check('bosses', 'lethal damage on A kills B\'s copy', ok,
          `B ${JSON.stringify(enemyList(B).map((e) => [e.idx, e.type, e.hp, e.flags]))}; B boss hits in ${B.Module._PortCoopEnemyStat(7)}`);
    A.script = walker;
}

/* colors: the player's own Kirby colour (platform/kirby_color.c) and the
 * name tags (platform/ui/coop_tags.c).  A picks a custom colour: A's Kirby
 * wears it (OBJ palette 0 in A's palette RAM), and B draws A's puppet in
 * exactly the same palette (B's OBJ palette 1); then one of the game's
 * fourteen, then back to the seat colour.  B's tag for A is there, on the
 * picture, named A, in A's colour. */
const PLTT_OBJ = 0x05000200;
const pal16 = (i, slot) => Array.from(new Uint16Array(i.Module.HEAPU8.buffer, PLTT_OBJ + 32 * slot, 16));
const sameBody = (a, b) => a.slice(2, 12).every((v, k) => (v & 0x7FFF) === (b[2 + k] & 0x7FFF));
async function colors() {
    if (FAKE || typeof A.Module._PortKirbyColorChoose !== 'function') {
        say('colors: build has no PortKirbyColor* exports -- skipped');
        return;
    }
    A.script = B.script = () => 0;
    const MA = A.Module;
    /* entry 3 is the body fill, the colour picked; a hurt flash (left over
     * from an earlier phase) swaps the whole palette for a few frames, so
     * every check waits for the exact colours rather than for equality */
    const body = (i, slot) => pal16(i, slot)[3] & 0x7FFF;
    const fmt = (i, slot) => pal16(i, slot).slice(2, 12).map((v) => v.toString(16)).join(' ');
    /* the game's own colours as the engine reads them from the ROM (none
     * are written down here): the body fill as BGR555, and as '#rrggbb' */
    const sw = (v) => MA._PortKirbyColorSwatch(v, 0);
    const fill = (rgb) => Math.round(((rgb >> 16) & 255) * 31 / 255) | (Math.round(((rgb >> 8) & 255) * 31 / 255) << 5)
        | (Math.round((rgb & 255) * 31 / 255) << 10);
    const css = (rgb) => '#' + ('00000' + rgb.toString(16)).slice(-6);
    const shows = (fill, css) => body(A, 0) === fill && body(B, 1) === fill && sameBody(pal16(B, 1), pal16(A, 0))
        && B.coop.colorOf(A.net.mid) === css;
    MA._PortKirbyColorChoose(14, 0x3070ff, 0xffd000);
    let ok = await until(() => body(A, 0) === 0x7DC6, 2000);
    check('colors', 'A\'s Kirby wears A\'s custom colour', ok, fmt(A, 0));
    ok = await until(() => shows(0x7DC6, '#3173ff'), 4000);
    check('colors', 'B draws A\'s puppet in the same palette, and B\'s page knows the colour', ok,
          `A ${fmt(A, 0)} | B ${fmt(B, 1)} | ${B.coop.colorOf(A.net.mid)}`);
    let tg = null, mine = null;
    ok = await until(() => {
        tg = B.coop.tags();
        mine = tg && tg.list.find((g) => g.mid === A.net.mid);
        return !!mine && mine.color === '#3173ff';
    }, 2000);
    check('colors', 'B has a name tag for A, on the picture, in A\'s colour',
          ok && mine.name === 'A' && !mine.off && mine.x >= 0 && mine.x <= 1 && mine.y >= 0 && mine.y <= 1,
          JSON.stringify(tg));
    MA._PortKirbyColorChoose(1, 0, 0);
    ok = await until(() => MA._PortKirbyColorLocal(0, 0) === 1 && shows(fill(sw(1)), css(sw(1))), 4000);
    check('colors', 'one of the game\'s fourteen (yellow) reaches B too', ok,
          `A index ${MA._PortKirbyColorLocal(0, 0)}, A ${fmt(A, 0)} | B ${fmt(B, 1)} | ${B.coop.colorOf(A.net.mid)}`);
    MA._PortKirbyColorChoose(-1, 0, 0);
    ok = await until(() => MA._PortKirbyColorLocal(0, 0) === 0 && shows(fill(sw(0)), css(sw(0))), 4000);
    check('colors', 'back to the game\'s: A is the seat colour (pink) again, on both', ok,
          `A index ${MA._PortKirbyColorLocal(0, 0)}, A ${fmt(A, 0)} | B ${fmt(B, 1)}`);
    A.script = walker;
}

// --- CPU Kirbys and spawned objects -----------------------------------------------
const aiState = (i, v) => { i.Module.HEAPU8[0x0203AD2C] = v & 255; i.Module.HEAPU8[0x0203AD2D] = v >> 8; };
function cpuSlots(i, owner) {
    const st = i.coop.cpuState(), out = [];
    st.slots.forEach((k, s) => {
        const m = typeof k === 'string' && k.match(/^c(\d+):(\d)$/);
        if (m && +m[1] === owner) out.push({ slot: s, id: +m[2] });
    });
    return out;
}
async function helpers() {
    if (FAKE || Coop.FX.some((n) => typeof A.Module[n] !== 'function')) {
        say('helpers: build has no PortCoopFx*/SlotPose exports -- skipped');
        return;
    }
    A.script = B.script = () => 0;
    for (const i of [A, B]) i.Module._PortWarp(0, ROOM, CELL_X, CELL_Y);
    await until(() => roomOf(A) === ROOM && roomOf(B) === ROOM, 8000);
    let ok = await until(() => B.coop.cpuState().auth === A.net.mid && A.coop.cpuState().auth === A.net.mid
                              && cpuSlots(B, A.net.mid).length === 2, 6000);
    check('helpers', 'A runs the CPU Kirbys; B shows A\'s two in its free slots', ok,
          `A ${JSON.stringify(A.coop.cpuState())} B ${JSON.stringify(B.coop.cpuState())}`);
    if (!ok) return;
    /* A walks so its helpers move; B's copy of each must sit where A had it a
     * little earlier (it is played back late, like a player) */
    A.script = walker;
    await sleep(2500);
    const t1 = performance.now() + 3000, errs = [];
    let rooms = 0, samples = 0;
    while (performance.now() < t1) {
        await sleep(100);
        const bt = B.track.at(-1);
        for (const h of cpuSlots(B, A.net.mid)) {
            const bp = bt.k[h.slot - 1];
            let best = Infinity, sameRoom = false;
            for (const e of A.track.slice(-40)) {
                const ap = e.k[h.id - 1];
                if (ap.room !== bp.room) continue;
                sameRoom = true;
                best = Math.min(best, Math.hypot((ap.x - bp.x) / PX, (ap.y - bp.y) / PX));
            }
            samples++;
            if (sameRoom) rooms++;
            errs.push(best);
        }
    }
    errs.sort((a, b) => a - b);
    const p95 = errs[Math.floor(errs.length * 0.95)];
    check('helpers', 'B\'s copies follow A\'s CPU Kirbys (same room, p95 <= 2 px of a recent A position)',
          samples > 20 && rooms === samples && p95 <= 2, `samples ${samples}, same room ${rooms}, p95 ${p95?.toFixed(2)} px`);
    const st = B.coop.cpuState();
    check('helpers', 'B runs no CPU Kirby of its own (every slot shows someone or nobody)',
          st.slots.slice(1).every((k) => k !== null), JSON.stringify(st.slots));
    /* A goes back into its tutorial: it can no longer run them, B takes over */
    aiState(A, 299);
    ok = await until(() => B.coop.cpuState().auth === B.net.mid && B.coop.cpuState().slots.slice(2).every((k) => k === null), 8000);
    check('helpers', 'A back in its tutorial: B runs the CPU Kirbys itself', ok, JSON.stringify(B.coop.cpuState()));
    aiState(A, 300);
    ok = await until(() => B.coop.cpuState().auth === A.net.mid && cpuSlots(B, A.net.mid).length === 2, 5000);
    /* a warp hides A's Kirby and reloads the room: the helpers must stay A's */
    let flips = 0;
    const watch = setInterval(() => { if (B.coop.cpuState().auth !== A.net.mid || A.coop.cpuState().auth !== A.net.mid) flips++; }, 50);
    for (const i of [A, B]) i.Module._PortWarp(0, ROOM, CELL_X + 2, CELL_Y);
    await sleep(3000);
    clearInterval(watch);
    check('helpers', 'a warp does not move the CPU Kirbys to the other game', flips === 0 && cpuSlots(B, A.net.mid).length === 2,
          `flips ${flips}, B ${JSON.stringify(B.coop.cpuState())}`);
    check('helpers', 'A past its tutorial again: A runs them, B shows A\'s again', ok, JSON.stringify(B.coop.cpuState()));
    /* the phone: B, in another room, calls; A's game sends its CPU Kirbys,
     * and B sees them arrive in its room */
    A.script = () => 0;
    B.Module._PortWarp(0, EN_ROOM, EN_CX, EN_CY);
    await until(() => roomOf(B) === EN_ROOM, 8000);
    await sleep(1500);
    const R_BTN = 0x100;
    B.script = (f) => (f % 60) < 20 ? R_BTN : 0;
    const helpersIn = (i, slots) => slots.every((s) => kirbyPos(i.Module, s).room === EN_ROOM);
    ok = await until(() => helpersIn(A, [2, 3]), 6000);
    B.script = () => 0;
    check('helpers', 'the phone: B calls from another room, A\'s game sends its CPU Kirbys to B\'s room', ok,
          `A's helpers in ${[2, 3].map((s) => kirbyPos(A.Module, s).room)}, B in ${roomOf(B)}`);
    ok = await until(() => cpuSlots(B, A.net.mid).every((h) => kirbyPos(B.Module, h.slot).room === EN_ROOM), 4000);
    check('helpers', 'and B sees them arrive (its puppets of them are in its room)', ok,
          JSON.stringify(cpuSlots(B, A.net.mid).map((h) => kirbyPos(B.Module, h.slot))));
    A.script = walker;
}

async function fx() {
    if (FAKE || Coop.FX.some((n) => typeof A.Module[n] !== 'function')) {
        say('fx: build has no PortCoopFx* exports -- skipped');
        return;
    }
    const B_BTN = 0x02, A_BTN = 0x01;
    A.script = B.script = () => 0;
    for (const i of [A, B]) i.Module._PortWarp(0, EN_ROOM, EN_CX, EN_CY);
    await until(() => roomOf(A) === EN_ROOM && roomOf(B) === EN_ROOM, 8000);
    await sleep(1000);
    const snap = (i) => { const x = i.coop.fxStats(); return { page: { ...x.page, unmapped: undefined }, engine: x.engine.slice() }; };
    const a0 = snap(A), b0 = snap(B);
    for (const i of [A, B]) { const u = i.coop.fxStats().page.unmapped; for (const k in u) delete u[k]; }
    /* inhale for a while, facing the room, then let go and tap B: whatever
     * A's Kirby spawns (the inhale, a spat star, dust) */
    A.script = (f) => ((f % 240) < 150 ? B_BTN : ((f % 240) < 170 ? 0 : ((f % 20) < 4 ? B_BTN : 0))) | ((f % 480) < 240 ? 0x10 : 0x20) * ((f % 240) >= 200 ? 1 : 0);
    await sleep(4000);
    const a = snap(A), b = snap(B);
    const capA = a.engine[0] - a0.engine[0], drawnB = b.engine[3] - b0.engine[3];
    const ok = capA > 100 && drawnB >= capA * 0.5;
    const fails = (i) => { const M = i.Module, r = []; for (let k = 0; k < 15; k++) r.push(M._PortCoopFxStat(32 + k));
                           const log = []; for (let j = 0; j < 8; j++) log.push([0, 1, 2, 3].map((q) => M._PortCoopFxStat(48 + 4 * j + q)));
                           const pl = []; for (let j = 0; j < 8; j++) pl.push([0, 1, 2, 3].map((q) => M._PortCoopFxStat(80 + 4 * j + q)));
                           return `reasons ${r} last ${JSON.stringify(log)} pal ${JSON.stringify(pl)}`; };
    say(`fx A ${JSON.stringify(a)} ${fails(A)}\nfx B ${JSON.stringify(b)} ${fails(B)}`);
    const dec = (u) => { if (!u) return 'none'; const r = []; for (let k = 0; k < u[2]; k++) { const o = 4 + k * 18;
        r.push([u[o] | (u[o + 1] << 8), u[o + 2], u[o + 3], (u[o + 4] | (u[o + 5] << 8)) << 16 >> 16, (u[o + 6] | (u[o + 7] << 8)) << 16 >> 16,
                u[o + 8] | (u[o + 9] << 8), u[o + 10], u[o + 16] | (u[o + 17] << 8)]); } return JSON.stringify(r); };
    say(`A last fx frame [anim,var,id,x,y,cursor,pal,src]: ${dec(A.coop.fxStats().lastOut)}`);
    say(`unmapped on B ${JSON.stringify(B.coop.fxStats().page.unmapped)} B ${JSON.stringify(B.coop.cpuState())}; on A ${JSON.stringify(A.coop.fxStats().page.unmapped)}`);
    check('fx', 'A\'s spawned objects are captured on A and drawn on B (4 s of A attacking; B draws >= half as many)', ok,
          `A captured ${a.engine[0] - a0.engine[0]}, sent ${a.page.framesSent - a0.page.framesSent}; ` +
          `B got ${b.page.framesRecv - b0.page.framesRecv}, set ${b.page.framesSet - b0.page.framesSet}, drawn ${b.engine[3] - b0.engine[3]}, ` +
          `skipped pal ${b.engine[4] - b0.engine[4]} frame ${b.engine[5] - b0.engine[5]} vram ${b.engine[6] - b0.engine[6]} ` +
          `queue ${b.engine[7] - b0.engine[7]} room ${b.engine[8] - b0.engine[8]}`);
    const sndA = a.engine[15] - a0.engine[15], playB = b.engine[16] - b0.engine[16], stopB = b.engine[17] - b0.engine[17];
    check('fx', 'A\'s sounds play on B (A reported some, B played some)', sndA > 0 && playB > 0,
          `A reported ${sndA}; B played ${playB}, skipped ${b.engine[18] - b0.engine[18]}`);
    check('fx', 'A letting go of the inhale stops B\'s copy of it', stopB > 0,
          `B stopped ${stopB}, safety-net stops ${b.engine[19] - b0.engine[19]}`);
    A.script = walker;
}

/* --- enemies after any player, and their shots ---------------------------------- */
const hpOf = (i, k = 0) => (i.Module.HEAPU8[KIRBY + KSIZE * k + 0x100] << 24) >> 24;
async function shots() {
    if (FAKE || typeof A.Module._PortCoopTestTarget !== 'function') {
        say('shots: build has no PortCoopTestTarget -- skipped');
        return;
    }
    A.script = B.script = () => 0;
    A.Module._PortWarp(0, SHOT_ROOM, SHOT_AX, SHOT_AY);
    B.Module._PortWarp(0, SHOT_ROOM, SHOT_BX, SHOT_BY);
    const there = await until(() => roomOf(A) === SHOT_ROOM && roomOf(B) === SHOT_ROOM, 8000);
    check('shots', `both warped into room ${SHOT_ROOM}`, there, `A ${roomOf(A)} B ${roomOf(B)}`);
    if (!there) return;
    await sleep(1500);
    const shotzos = (i) => enemyList(i).filter((e) => e.type === 0x35 && !e.dying && e.idx !== 0xFFFF);
    const kp = (i) => kirbyPos(i.Module, 0);
    const bk = kp(B), ak = kp(A);
    say(`shots: A at ${ak.x >> 8},${ak.y >> 8}, B at ${bk.x >> 8},${bk.y >> 8}; ` +
        `A's Shotzos ${JSON.stringify(shotzos(A).map((e) => [e.idx, e.x >> 8, e.y >> 8, e.puppet]))}; ` +
        `B's ${JSON.stringify(shotzos(B).map((e) => [e.idx, e.x >> 8, e.y >> 8, e.puppet]))}`);
    const near = shotzos(A).sort((p, q) => Math.abs(p.x - bk.x) - Math.abs(q.x - bk.x))[0];
    const bSlot = A.coop.cpuState().slots.indexOf(B.net.mid);
    const mine = near ? A.Module._PortCoopTestTarget(near.addr) : -2;
    check('shots', 'A\'s Shotzo nearest B goes after B (A\'s game picks B\'s puppet)',
          !!near && bSlot > 0 && mine === bSlot && Math.abs(near.x - bk.x) < Math.abs(near.x - ak.x),
          `Shotzo ${near ? near.idx : 'none'}; picks slot ${mine}, B is slot ${bSlot}`);
    const pup = await until(() => shotzos(B).length > 0 && shotzos(B).every((e) => e.puppet), 3000);
    check('shots', 'B\'s Shotzos are puppets of A\'s', pup, JSON.stringify(shotzos(B).map((e) => [e.idx, e.puppet])));
    const e0 = B.coop.fxStats().engine.slice(), a0 = A.coop.fxStats().engine.slice();
    const hp0 = hpOf(B);
    const hit = await until(() => hpOf(B) < hp0, 15000);
    const e1 = B.coop.fxStats().engine, a1 = A.coop.fxStats().engine;
    check('shots', 'a bullet of A\'s Shotzo hurts B in B\'s own game', hit && e1[13] > e0[13],
          `B hp ${hp0} -> ${hpOf(B)}; A reported ${a1[11] - a0[11]} hazards; B tested ${e1[12] - e0[12]}, ` +
          `hit ${e1[13] - e0[13]}, skipped ${e1[14] - e0[14]}`);
    A.script = walker;
}

/* --- the live room: blocks, stars, buttons ------------------------------------------ */
function tilesOf(i) {
    const M = i.Module, buf = M._malloc(1600), n = M._PortCoopTestTiles(buf, 800), out = new Set();
    for (let k = 0; k < n; k++) out.add(M.HEAPU8[buf + 2 * k] + ',' + M.HEAPU8[buf + 2 * k + 1]);
    M._free(buf);
    return out;
}
function objectsOf(i, type) {
    const M = i.Module, buf = M._malloc(16 * 32), n = M._PortCoopTestObjects(type, buf, 32), out = [];
    const dv = new DataView(M.HEAPU8.buffer);
    for (let k = 0; k < n; k++) {
        const o = buf + 16 * k;
        out.push({ addr: dv.getUint32(o, true), x: dv.getInt16(o + 4, true), y: dv.getInt16(o + 6, true),
                   anim: M.HEAPU8[o + 8], ability: M.HEAPU8[o + 9], idx: M.HEAPU8[o + 10] });
    }
    M._free(buf);
    return out;
}
async function room() {
    if (FAKE || typeof A.Module._PortCoopTestTiles !== 'function') {
        say('room: build has no PortCoopTestTiles -- skipped');
        return;
    }
    const B_BTN = 0x02, SELECT = 0x04;
    A.script = B.script = () => 0;
    A.Module._PortWarp(0, TILE_ROOM, TILE_AX, TILE_AY);
    B.Module._PortWarp(0, TILE_ROOM, TILE_BX, TILE_BY);
    let ok = await until(() => roomOf(A) === TILE_ROOM && roomOf(B) === TILE_ROOM, 8000);
    check('room', `both warped into room ${TILE_ROOM}`, ok, `A ${roomOf(A)} B ${roomOf(B)}`);
    if (!ok) return;
    await sleep(1500);
    const a0 = tilesOf(A), b0 = tilesOf(B);
    say(`room: blocks ${[...a0].join(' ')}; A at ${kirbyPos(A.Module, 0).x >> 12},${kirbyPos(A.Module, 0).y >> 12}`);
    check('room', 'both games have the room\'s blocks', a0.size > 0 && a0.size === b0.size && [...a0].every((k) => b0.has(k)),
          `A ${a0.size}, B ${b0.size}`);
    /* A faces the blocks and inhales */
    const f0 = A.Module._PortFrameNumber();
    A.script = (f) => (f - f0 < 8 ? 0x10 : B_BTN);
    ok = await until(() => tilesOf(A).size < a0.size, 6000);
    await sleep(300);
    A.script = () => 0;
    const a1 = tilesOf(A), broken = [...a0].filter((k) => !a1.has(k));
    check('room', 'A breaks blocks in its own game (inhaling them)', ok && broken.length > 0, `broken ${broken.join(' ')}`);
    if (!broken.length) return;
    ok = await until(() => broken.every((k) => !tilesOf(B).has(k)), 3000);
    check('room', 'the blocks A breaks break in B\'s game too', ok,
          `B still has ${broken.filter((k) => tilesOf(B).has(k)).join(' ') || 'none'}; ${JSON.stringify(B.coop.roomStats())}`);
    /* B leaves: its copy of the room is gone; back in, it loads whole and gets A's breaks */
    B.Module._PortWarp(0, EN_ROOM, EN_CX, EN_CY);
    await until(() => roomOf(B) === EN_ROOM, 8000);
    await sleep(1000);
    B.Module._PortWarp(0, TILE_ROOM, TILE_BX, TILE_BY);
    await until(() => roomOf(B) === TILE_ROOM, 8000);
    ok = await until(() => broken.every((k) => !tilesOf(B).has(k)), 5000);
    check('room', 'B walking back in gets the room as A has it (A\'s breaks sent to it)', ok,
          `B has ${tilesOf(B).size} blocks; A snapshots sent ${A.coop.roomStats().page.snapsSent}, B got ${B.coop.roomStats().page.snapsRecv}`);
    /* A spits out the block it inhaled, takes Beam and lets it go */
    const s0 = A.Module._PortFrameNumber();
    A.script = (f) => (f - s0 > 10 && f - s0 < 18 ? B_BTN : 0);
    await sleep(1000);
    A.script = () => 0;
    const gave = A.Module._PortCoopTestAbility(7);
    await sleep(2000);
    const abil = () => A.Module.HEAPU8[KIRBY + 0x103];
    say(`room: A took Beam: ${gave}, ability now ${abil()}`);
    const g0 = A.Module._PortFrameNumber();
    A.script = (f) => (f - g0 < 10 ? SELECT : 0);
    const starsA = () => objectsOf(A, 0xA3), starsB = () => objectsOf(B, 0xA3);
    ok = await until(() => starsA().length > 0 && starsB().length > 0, 3000);
    A.script = () => 0;
    const sa = starsA(), sb = starsB();
    check('room', 'A lets its ability go: B\'s game makes the star too, with A\'s ability', ok && sb.length > 0 && sb[0].ability === 7,
          `A ${JSON.stringify(sa)} B ${JSON.stringify(sb)}; A ability ${abil()}; A ${JSON.stringify(A.coop.roomStats())} B ${JSON.stringify(B.coop.roomStats())}`);
    if (sb.length) {
        B.Module._PortCoopTestSwallow(sb[0].addr);
        ok = await until(() => starsA().length === 0, 3000);
        check('room', 'B swallows its copy: A\'s star is removed', ok, `A ${JSON.stringify(starsA())}; ${JSON.stringify(A.coop.roomStats())}`);
    }
    /* the button */
    A.Module._PortWarp(0, BTN_ROOM, BTN_AX, BTN_AY);
    B.Module._PortWarp(0, BTN_ROOM, BTN_BX, BTN_BY);
    ok = await until(() => roomOf(A) === BTN_ROOM && roomOf(B) === BTN_ROOM, 8000);
    await sleep(1500);
    const btnA = objectsOf(A, 0x6D), btnB = objectsOf(B, 0x6D);
    say(`room: A at ${kirbyPos(A.Module, 0).x >> 8},${kirbyPos(A.Module, 0).y >> 8}, B at ${kirbyPos(B.Module, 0).x >> 8},${kirbyPos(B.Module, 0).y >> 8}; ` +
        `buttons on A ${JSON.stringify(btnA)} on B ${JSON.stringify(btnB)}`);
    const down = (l) => l.some((o) => o.anim === 1);
    ok = await until(() => down(objectsOf(B, 0x6D)), 3000);
    check('room', 'A stands on the button: it is down in B\'s game too', ok && down(objectsOf(A, 0x6D)),
          `A ${JSON.stringify(objectsOf(A, 0x6D))} B ${JSON.stringify(objectsOf(B, 0x6D))}; B held by puppets ${B.Module._PortCoopRoomStat(5)}`);
    A.script = walker;
}

const phases = { follow, freezeA, freezeB, reconnect, drop, enemies, bosses, progress, colors, helpers, fx, shots, room };
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
