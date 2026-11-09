// DIAGNOSTIC ONLY (read-only): measures the protocol-v2 room netplay; changes
// no engine, client or server behaviour.  Written for docs/NETPLAY-HANDOFF.md
// ("Diagnosis").  Two real game instances (alpha = host, beta), the real
// client (web/net_session.js + web/net_game.js), the real room (in-process
// dev relay, or any v2 server with --url), paced at 59.73 Hz by a vsync shim
// like tools/netplay_v2_test.mjs.
//
//   node tools/netplay_diag.mjs build/katam-node.js <rom.gba>
//        [--url http://127.0.0.1:8787]   any v2 server (wrangler dev, deployed)
//        [--seconds 60]
//        [--link-a MS] [--link-b MS]     one-way latency added to that client
//        [--jitter MS]                   + uniform 0..MS per message (order kept)
//        [--input human|walker|idle]     human: runs of 6-40 frames, jumps,
//                                        attacks (no START); walker: the e2e
//                                        test's script
//        [--start-every F]               also tap START every F frames (R8)
//        [--sync-every MS] [--deadline MS] [--hash-every F]   room params
//        [--nosync]                      caps.sync=false: the room can judge
//                                        but never sync -> natural divergence
//        [--every-b N]                   beta presents every Nth vsync (slow device)
//        [--stall-b T:S]                 beta stops presenting at T s for S s
//                                        (tab hidden, phone locked, GC hitch)
//        [--json out.json] [--logs out.txt]
//
// What it records, per instance:
//   syncs by reason (sync_req), desync notices, every applied correction:
//     checkpoint C, the frame it was applied at, the rewind depth (at - C),
//     wall time from sync_req to apply, and the own/other Kirby displacement
//     across it;
//   per presented frame: own and other Kirby x/y (gKirbys[k].base.x/y, 24.8)
//     and "jumps" (a displacement of more than --jump px in one presented
//     frame within one room), attributed to sync / rollback / fast-forward /
//     none;
//   own-input overrides: frames where the room's sealed value for our seat
//     differs from what we stamped (the late-sender rule);
//   the local lead over the sealed frontier (now - frontier), percentiles;
//   engine stats (rollbacks, deepest, lateDrops, mispredictions).
// And across the two peers, at every checkpoint both hashed:
//   simHash (the referee's hash) equal?  full-image differing pages, and
//   differing bytes after the census masks (tests/census/masks.txt), with
//   the unmasked byte ranges named by address.

import { createRequire } from 'module';
import path from 'path';
import fs from 'fs';
import { fileURLToPath } from 'url';

const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const argv = process.argv.slice(2);
const flag = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 ? argv[i + 1] : d; };
const has = (k) => argv.includes('--' + k);
const BOOL = ['nosync'];
const [modulePath, romPath] = argv.filter((a, i) => !a.startsWith('--')
    && !(i > 0 && argv[i - 1].startsWith('--') && !BOOL.includes(argv[i - 1].slice(2))));
if (!modulePath || !romPath) {
    console.error('usage: node tools/netplay_diag.mjs <katam-node.js> <rom.gba> [--url U] [--seconds S] ...');
    process.exit(2);
}
const SECONDS = +flag('seconds', 60);
const JUMP_PX = +flag('jump', 8);
const JITTER = +flag('jitter', 0);
const INPUT = flag('input', 'human');
const START_EVERY = +flag('start-every', 0);
const createKatam = require(path.resolve(modulePath));
const KatamNet = require(path.join(here, '../web/net_session.js'));
const createNetGame = require(path.join(here, '../web/net_game.js'));
const rom = fs.readFileSync(romPath);

let relay = null, base = flag('url');
if (!base) {
    const { startRelay } = await import(path.join(here, '../netplay/dev-relay.mjs'));
    relay = await startRelay(0, { quiet: true });
    base = `http://127.0.0.1:${relay.port}`;
}

// --- census masks -> snapshot offsets ---------------------------------------
// Snapshot layout (platform/rollback.c SaveTo): EWRAM, IWRAM, IO, PLTT, VRAM,
// OAM, the sim block's used bytes, then DMA + PPU host state.
const REG = [
    ['ewram', 0x02000000, 0x40000], ['iwram', 0x03000000, 0x8000], ['io', 0x04000000, 0x400],
    ['pltt', 0x05000000, 0x400], ['vram', 0x06000000, 0x18000], ['oam', 0x07000000, 0x400],
];
let off = 0;
for (const r of REG) { r.push(off); off += r[2]; }
const SIM_OFF = off;        // sim block (0x02040000 in the map), then host state
function addrToOff(a) {
    for (const [, b, sz, o] of REG) if (a >= b && a < b + sz) return o + (a - b);
    if (a >= 0x02040000 && a < 0x02050000) return SIM_OFF + (a - 0x02040000);
    return -1;
}
function offToAddr(o) {
    for (const [, b, sz, ro] of REG) if (o >= ro && o < ro + sz) return b + (o - ro);
    return 0x02040000 + (o - SIM_OFF);     // sim block or host state beyond it
}
function regionOf(o) {
    for (const [n, , sz, ro] of REG) if (o >= ro && o < ro + sz) return n;
    return 'simblock/host';
}
const maskRanges = [];
for (const line of fs.readFileSync(path.join(here, '../tests/census/masks.txt'), 'utf8').split('\n')) {
    const m = /^([0-9A-F]{8}) ([0-9A-F]{8}) (\S+) (.*)$/.exec(line.trim());
    if (!m) continue;
    const lo = parseInt(m[1], 16), hi = parseInt(m[2], 16);
    if (lo >= 0x80000000) { maskRanges.push({ lo: -1, hi: -1, host: true, name: m[4] }); continue; }
    const a = addrToOff(lo), b = addrToOff(hi - 1);
    if (a >= 0 && b >= 0) maskRanges.push({ lo: a, hi: b + 1, cls: m[3], name: m[4].split(' --')[0] });
}
let maskBuf = null;
function masks(size, simUsed) {
    if (maskBuf && maskBuf.length === size) return maskBuf;
    maskBuf = new Uint8Array(size);
    for (const r of maskRanges) {
        if (r.host) { maskBuf.fill(1, SIM_OFF + simUsed); continue; }
        maskBuf.fill(1, r.lo, r.hi);
    }
    return maskBuf;
}

// --- vsync shim ---------------------------------------------------------------
let current = null;
const PERIOD = 1000 / 59.7275;
global.requestAnimationFrame = (cb) => {
    const inst = current;
    if (!inst) { setTimeout(cb, 1); return; }
    inst.raf = cb;
};
const instances = [];
const t0 = performance.now();
let tick = 0;
setInterval(() => {
    const due = Math.floor((performance.now() - t0) / PERIOD);
    while (tick < due) {
        tick++;
        for (const inst of instances) {
            if (!inst.raf) continue;
            if (inst.every > 1 && tick % inst.every) continue;
            if (inst.stall && (performance.now() - t0) / 1000 >= inst.stall[0] && (performance.now() - t0) / 1000 < inst.stall[0] + inst.stall[1]) continue;
            if (inst.Module.portNetHold > 0) { inst.Module.portNetHold--; continue; }
            const cb = inst.raf;
            inst.raf = null;
            cb(performance.now());
        }
    }
}, 2);

// --- a WebSocket with latency and order-preserving jitter ------------------------
function laggyWebSocket(ms, jitter) {
    // One FIFO per direction: a message is never delivered before the one
    // sent ahead of it (TCP's guarantee; independent setTimeouts break it).
    return class extends WebSocket {
        constructor(u) {
            super(u);
            const self = this;
            let handler = null;
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
                    last = Math.max(last, performance.now() + ms + Math.random() * jitter);
                    q.push({ at: last, v });
                    if (!armed) { armed = true; setTimeout(pump, Math.max(0, q[0].at - performance.now())); }
                };
            };
            const inq = fifo((e) => handler && handler.call(self, e));
            super.onmessage = (e) => inq(e);
            Object.defineProperty(this, 'onmessage', { set(f) { handler = f; }, get() { return handler; } });
            this._outq = fifo((d) => { if (this.readyState === 1) super.send(d); });
        }
        send(d) { this._outq(d); }
    };
}

// --- inputs ------------------------------------------------------------------------
function mulberry(a) { return () => { a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }
const walker = (seed) => (f) => {
    if (f < 2300) return 0;
    const phase = Math.floor((f + seed * 37) / 90) % 6;
    return [0x10, 0, 0x20, 0x11, 0, 0x01][phase] | ((f + seed) % 47 < 3 ? 1 : 0);
};
// Human-ish: a held direction for 6-40 frames, a jump now and then (held
// 4-20 frames), B taps; biased right so the Kirbys travel through doors.
function human(seed) {
    const rnd = mulberry(seed * 7919);
    let until = 0, dir = 0, jumpUntil = 0, bUntil = 0, last = -1, held = 0;
    return (f) => {
        if (f < 2300) return 0;
        if (f === last) return held;
        last = f;
        if (f >= until) {
            const r = rnd();
            dir = r < 0.5 ? 0x10 : r < 0.7 ? 0x20 : r < 0.8 ? 0x40 : r < 0.85 ? 0x80 : 0;
            until = f + 6 + Math.floor(rnd() * 34);
        }
        if (f >= jumpUntil && rnd() < 0.03) jumpUntil = f + 4 + Math.floor(rnd() * 16);
        if (f >= bUntil && rnd() < 0.01) bUntil = f + 3;
        let k = dir | (f < jumpUntil ? 1 : 0) | (f < bUntil ? 2 : 0);
        if (START_EVERY && f % START_EVERY < 3) k |= 8;
        held = k;
        return k;
    };
}
const script = (seed) => INPUT === 'walker' ? walker(seed) : INPUT === 'idle' ? (() => 0) : human(seed);

const SIM = 'netplay-diag';
const ROMHASH = (await import('crypto')).createHash('sha1').update(rom).digest('hex');
const KIRBY = 0x02020EE0, KSIZE = 424;

function kirbyPos(Module, k) {
    if (k < 0) return null;
    const dv = new DataView(Module.HEAPU8.buffer);
    const a = KIRBY + KSIZE * k;
    return { x: dv.getInt32(a + 0x40, true) / 256, y: dv.getInt32(a + 0x44, true) / 256, room: dv.getUint16(a + 0x60, true) };
}

function pct(a, p) { if (!a.length) return null; const s = a.slice().sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(p / 100 * s.length))]; }

function makeInstance(name, { code, create, linkMs = 0, seed }) {
    const inst = {
        name, logs: [], raf: null, reasons: {}, desyncs: [], corrections: [], jumps: [],
        leads: [], stamped: new Map(), overrides: 0, overrideFrames: 0, sealedOwn: 0,
        pendingCorr: null, lastPos: null, lastOther: null, lastRb: 0, lastFf: 0, syncReqAt: new Map(),
        presents: 0, ckpt: new Map(), ffwds: [], ffPending: null,
    };
    const log = (t) => { inst.logs.push(`[${name}] ${t}`); if (inst.logs.length > 4000) inst.logs.shift(); };
    let resolveRom;
    const input = script(seed);
    const Module = {
        portRomReady: new Promise((r) => { resolveRom = r; }),
        portAudioRate: 0,
        print: log, printErr: log,
        portDiag: (t) => log(t),
        portPresent() {
            current = inst;
            const f = Module._PortFrameNumber();
            Module._PortSetKeys(input(f));
            const g = inst.game;
            const ff0 = g.state.ffwd;
            if (inst.ffPending) { inst.ffwds.push({ skip: f - inst.ffPending.f, frontierAhead: inst.ffPending.ahead }); inst.ffPending = null; }
            g.tick();
            if (g.state.ffwd !== ff0 && f > 2300) inst.ffPending = { f, ahead: g.state.frontier - f };
            inst.presents++;
            const st = g.state;
            if (st.phase === 'live' && st.seat >= 0) {
                if (st.frontier > st.inputStart) inst.leads.push(f - st.frontier);
                const mine = st.kirby, other = mine === 0 ? 1 : 0;
                const p = kirbyPos(Module, mine), q = kirbyPos(Module, other);
                const es = g.engineStats();
                let cause = 'none';
                if (inst.pendingCorr) cause = 'sync';
                else if (es && es.rollbacks !== inst.lastRb) cause = 'rollback';
                else if (st.ffwd !== inst.lastFf) cause = 'ffwd';
                const jump = (a, b, who) => {
                    if (!a || !b || a.room !== b.room) return;
                    const d = Math.hypot(a.x - b.x, a.y - b.y);
                    if (d > JUMP_PX) inst.jumps.push({ t: +((performance.now() - t0) / 1000).toFixed(2), f, who, d: +d.toFixed(1), cause });
                    return d;
                };
                if (mine >= 0 && st.focus === mine) {
                    const dOwn = jump(p, inst.lastPos, 'own');
                    const dOther = jump(q, inst.lastOther, 'other');
                    if (inst.pendingCorr) {
                        inst.pendingCorr.ownDisp = dOwn == null ? null : +dOwn.toFixed(1);
                        inst.pendingCorr.otherDisp = dOther == null ? null : +dOther.toFixed(1);
                        inst.pendingCorr = null;
                    }
                }
                inst.lastPos = p; inst.lastOther = q;
                if (es) inst.lastRb = es.rollbacks;
                inst.lastFf = st.ffwd;
            }
        },
        portNetIdle() { if (inst.game) inst.game.idle(); },
        onRuntimeInitialized() {
            Module.HEAPU8.set(rom, 0x08000000);
            Module._PortRomLoaded(rom.length);
        },
    };
    inst.Module = Module;
    inst.net = KatamNet.connect({
        base, code, log,
        WebSocket: (linkMs || JITTER) ? laggyWebSocket(linkMs, JITTER) : undefined,
        hello: { sim: SIM, rom: ROMHASH, content: 'vanilla', name, platform: 'node',
                 caps: { seats: 4, sync: !has('nosync') }, want: 'play', create,
                 view: { w: 240, h: 160 } },
    });
    const net = inst.net;
    // Own-input overrides: what we stamped vs what the room sealed for us.
    const send = net.sendInput;
    net.sendInput = (first, keys) => {
        for (let i = 0; i < keys.length; i++) inst.stamped.set(first + i, keys[i]);
        if (inst.stamped.size > 20000) for (const k of [...inst.stamped.keys()].slice(0, 5000)) inst.stamped.delete(k);
        return send(first, keys);
    };
    inst.sealed = new Map();
    net.on('frames', (r) => {
        for (let i = 0; i < r.frames.length; i++) inst.sealed.set(r.firstFrame + i, r.frames[i].slice(0, 4));
        const s = net.seat;
        if (s < 0) return;
        for (let i = 0; i < r.frames.length; i++) {
            const f = r.firstFrame + i;
            if (!inst.stamped.has(f)) continue;
            inst.sealedOwn++;
            if (inst.stamped.get(f) !== r.frames[i][s]) inst.overrideFrames++;
        }
    });
    net.on('error', (e) => console.log(`[${name}] room error: ${KatamNet.explainError(e)}`));
    net.on('desync', (d) => inst.desyncs.push({ f: d.f, groups: d.groups.length }));
    net.on('sync_req', (m) => {
        inst.reasons[m.reason] = (inst.reasons[m.reason] || 0) + 1;
        inst.syncReqAt.set(m.id, { t: performance.now(), frame: inst.game ? Module._PortFrameNumber() : 0, f: m.f });
    });
    net.on('sync_cancel', (m) => { inst.reasons['cancel:' + m.why] = (inst.reasons['cancel:' + m.why] || 0) + 1; });
    instances.push(inst);
    createKatam(Module).then(() => {
        inst.game = createNetGame({
            Module, net, log, want: 'play',
            onHash: (f, h) => onCheckpoint(inst, f, h),
            onCorrection: (c) => {
                const req = [...inst.syncReqAt.values()].find((x) => x.f === c.frame);
                const rec = { C: c.frame, at: c.at, rewind: c.at - c.frame, reason: c.reason, pages: c.pages, bytes: c.bytes,
                              wallMs: req ? Math.round(performance.now() - req.t) : null,
                              t: +((performance.now() - t0) / 1000).toFixed(2) };
                inst.corrections.push(rec);
                inst.pendingCorr = rec;
            },
        });
        inst.game.ready.then(() => resolveRom(), (e) => { console.error(name, e); process.exit(1); });
    });
    return inst;
}

// --- cross-peer checkpoint comparison ------------------------------------------
const cmp = [];               // per checkpoint both reported
const unmaskedAgg = new Map();   // range start addr -> {bytes, times}
let simUsed = 0;
function onCheckpoint(inst, f, h) {
    const snap = inst.game.copies.get(f);
    if (!snap) return;
    inst.ckpt.set(f, { h, snap });
    // keep memory bounded
    if (inst.ckpt.size > 12) inst.ckpt.delete(inst.ckpt.keys().next().value);
    const [a, b] = instances;
    if (!a || !b) return;
    const A = a.ckpt.get(f), B = b.ckpt.get(f);
    if (!A || !B) return;
    a.ckpt.delete(f); b.ckpt.delete(f);
    const n = Math.min(A.snap.length, B.snap.length);
    if (!simUsed) simUsed = Math.max(0, n - SIM_OFF - 4096);   // host state is a few KiB at the end
    const M = masks(n, simUsed);
    let diffPages = 0, diffBytes = 0, unmasked = 0;
    const regions = {};
    let runStart = -1;
    const runs = [];
    for (let p = 0; p * 4096 < n; p++) {
        let pd = false;
        for (let i = p * 4096, e = Math.min(n, (p + 1) * 4096); i < e; i++) {
            if (A.snap[i] !== B.snap[i]) {
                pd = true; diffBytes++;
                const r = regionOf(i);
                regions[r] = (regions[r] || 0) + 1;
                if (!M[i]) {
                    unmasked++;
                    if (runStart < 0 || i > runs[runs.length - 1].e + 16) { runs.push({ s: i, e: i }); runStart = i; }
                    else runs[runs.length - 1].e = i;
                }
            }
        }
        if (pd) diffPages++;
    }
    for (const r of runs) {
        const k = offToAddr(r.s);
        const x = unmaskedAgg.get(k) || { len: 0, times: 0, region: regionOf(r.s) };
        x.len = Math.max(x.len, r.e - r.s + 1); x.times++;
        unmaskedAgg.set(k, x);
    }
    const parts = (u) => { const dv = new DataView(u.buffer, u.byteOffset, u.byteLength);
        const o = [dv.getUint32(0x40000 + 0x68D8, true)];
        for (let k = 0; k < 4; k++) o.push(dv.getInt32(0x20EE0 + 424 * k + 64, true), dv.getInt32(0x20EE0 + 424 * k + 68, true));
        return o; };
    const pa = parts(A.snap), pb = parts(B.snap);
    const which = [];
    if (pa[0] !== pb[0]) which.push('rng');
    for (let k = 0; k < 4; k++) if (pa[1 + 2 * k] !== pb[1 + 2 * k] || pa[2 + 2 * k] !== pb[2 + 2 * k])
        which.push('k' + k + ':' + ((pa[1 + 2 * k] - pb[1 + 2 * k]) / 256).toFixed(1) + ',' + ((pa[2 + 2 * k] - pb[2 + 2 * k]) / 256).toFixed(1));
    cmp.push({ f, simEqual: A.h === B.h, which, diffPages, diffBytes, unmaskedBytes: unmasked, unmaskedRuns: runs.length, regions });
}

// --- run ------------------------------------------------------------------------------
const params = { deadlineMs: +flag('deadline', 100), syncEveryMs: +flag('sync-every', 10000), hashEvery: +flag('hash-every', 30) };
const created = await KatamNet.createRoom(base);
if (!created.code) { console.error('create failed', created); process.exit(1); }
const CODE = created.code;
console.log(`[diag] room ${CODE} at ${base}; params ${JSON.stringify(params)}; link a=${flag('link-a', 0)} b=${flag('link-b', 0)} jitter=${JITTER}; input ${INPUT}${has('nosync') ? '; NOSYNC' : ''}`);
const alpha = makeInstance('alpha', { code: CODE, seed: 1, linkMs: +flag('link-a', 0), create: params });
await new Promise((r) => setTimeout(r, 300));
const beta = makeInstance('beta', { code: CODE, seed: 2, linkMs: +flag('link-b', 0) });
beta.every = +flag('every-b', 1);
if (flag('stall-b')) beta.stall = flag('stall-b').split(':').map(Number);
await new Promise((resolve) => {
    const iv = setInterval(() => {
        const r = alpha.net.roster;
        if (r && r.members.filter((m) => m.seat >= 0).length === 2 && alpha.net.mid === r.host) {
            clearInterval(iv); alpha.net.start(); resolve();
        }
    }, 50);
});
const tStart = performance.now();
await new Promise((r) => setTimeout(r, SECONDS * 1000));

// The two instances' input timelines (what each engine simulated with, all
// four columns), inputStart .. the older present minus the ring.
function timelineCompare() {
    const [a, b] = instances;
    const end = Math.min(a.Module._PortFrameNumber(), b.Module._PortFrameNumber()) - 40;
    let diff = 0, first = null;
    const cols = [0, 0, 0, 0];
    for (let f = 2200; f < end; f++)
        for (let p = 0; p < 4; p++)
            if (a.Module._PortRbInputAt(p, f) !== b.Module._PortRbInputAt(p, f)) {
                diff++; cols[p]++;
                if (first === null) first = [f, p, a.Module._PortRbInputAt(p, f), b.Module._PortRbInputAt(p, f)];
            }
    // ... and each engine against the sealed stream it was sent
    const vsSealed = {};
    for (const x of instances) {
        let n = 0, ex = null, missing = 0;
        for (let f = 2200; f < end; f++) {
            const v = x.sealed.get(f);
            if (!v) { missing++; continue; }
            for (let p = 0; p < 4; p++) {
                const e = x.Module._PortRbInputAt(p, f);
                if (e !== v[p]) { n++; if (!ex) ex = { f, p, engine: e, sealed: v[p], seat: x.net.seat }; }
            }
        }
        vsSealed[x.name] = { differing: n, missingSealed: missing, first: ex };
    }
    return { frames: end - 2200, differing: diff, perColumn: cols, first, vsSealed };
}

// --- report ----------------------------------------------------------------------------
const secs = (performance.now() - tStart) / 1000;
const core = relay ? [...relay.v2.rooms.values()][0].core : null;
function summary(x) {
    const st = x.game.state;
    const byCause = {};
    for (const j of x.jumps) { const k = j.who + ':' + j.cause; byCause[k] = (byCause[k] || 0) + 1; }
    const rew = x.corrections.map((c) => c.rewind);
    return {
        frame: x.Module._PortFrameNumber(), presents: x.presents, delay: st.delay, ffwd: st.ffwd, held: st.held,
        hashes: st.hashes, syncReasons: x.reasons, desyncNotices: x.desyncs.length,
        corrections: x.corrections.length,
        rewindFrames: { p50: pct(rew, 50), p90: pct(rew, 90), max: rew.length ? Math.max(...rew) : null },
        syncWallMs: { p50: pct(x.corrections.map((c) => c.wallMs).filter((v) => v != null), 50), max: Math.max(0, ...x.corrections.map((c) => c.wallMs || 0)) },
        correctionDisplacement: {
            own: x.corrections.map((c) => c.ownDisp).filter((v) => v != null),
            other: x.corrections.map((c) => c.otherDisp).filter((v) => v != null),
        },
        jumpsByCause: byCause,
        bigJumps: x.jumps.filter((j) => j.d > 24).slice(0, 12),
        ownOverride: { sealedFrames: x.sealedOwn, overridden: x.overrideFrames, pct: +(100 * x.overrideFrames / Math.max(1, x.sealedOwn)).toFixed(2) },
        leadOverFrontier: { p50: pct(x.leads, 50), p90: pct(x.leads, 90), p99: pct(x.leads, 99), max: Math.max(...x.leads) },
        engine: x.game.engineStats(),
        bytesPerSec: { up: Math.round(x.net.bytesUp / secs), down: Math.round(x.net.bytesDown / secs) },
        correctionsList: x.corrections.slice(0, 40),
        ffwdSkips: { n: (x.ffwds || []).length, p50: pct((x.ffwds || []).map((v) => v.skip), 50), max: Math.max(0, ...(x.ffwds || []).map((v) => v.skip)),
                     byFrontier: (x.ffwds || []).filter((v) => v.frontierAhead > 1).length },
    };
}
const top = [...unmaskedAgg.entries()].sort((a, b) => b[1].times - a[1].times).slice(0, 30)
    .map(([a, v]) => ({ addr: '0x' + a.toString(16).padStart(8, '0'), len: v.len, checkpoints: v.times, region: v.region }));
const res = {
    seconds: +secs.toFixed(1), base, params, input: INPUT, nosync: has('nosync'),
    link: { a: +flag('link-a', 0), b: +flag('link-b', 0), jitter: JITTER },
    alpha: summary(alpha), beta: summary(beta),
    room: core && { ...core.stats, meanSealMs: +(core.stats.sealLatencyMs / Math.max(1, core.stats.sealed)).toFixed(1) },
    peers: {
        checkpointsCompared: cmp.length,
        simHashMismatches: cmp.filter((c) => !c.simEqual).length,
        fullImageEqual: cmp.filter((c) => c.diffBytes === 0).length,
        unmaskedEqual: cmp.filter((c) => c.unmaskedBytes === 0).length,
        simEqualButUnmaskedDiffer: cmp.filter((c) => c.simEqual && c.unmaskedBytes > 0).length,
        diffPages: { p50: pct(cmp.map((c) => c.diffPages), 50), max: Math.max(0, ...cmp.map((c) => c.diffPages)) },
        unmaskedBytes: { p50: pct(cmp.map((c) => c.unmaskedBytes), 50), max: Math.max(0, ...cmp.map((c) => c.unmaskedBytes)) },
        firstSimMismatch: (cmp.find((c) => !c.simEqual) || {}).f || null,
        firstMismatches: cmp.filter((c) => !c.simEqual).slice(0, 8).map((c) => [c.f, c.which.join(' ')]),
        timeline: timelineCompare(),
        firstUnmaskedDiff: (cmp.find((c) => c.unmaskedBytes > 0) || {}).f || null,
        series: cmp.filter((_, i) => i % 10 === 0).map((c) => [c.f, c.simEqual ? 1 : 0, c.diffPages, c.unmaskedBytes]),
        topUnmasked: top,
    },
};
console.log(JSON.stringify(res, null, 1));
if (flag('json')) fs.writeFileSync(flag('json'), JSON.stringify(res, null, 1));
if (flag('logs')) fs.writeFileSync(flag('logs'), instances.map((x) => x.logs.join('\n')).join('\n'));
process.exit(0);
