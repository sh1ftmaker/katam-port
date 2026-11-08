// Netplay v2 end to end: real game instances, a real room, a 4-letter code.
//
//   node tools/netplay_v2_test.mjs build/katam-node.js <rom.gba> [--url http://127.0.0.1:8787]
//        [--seconds 40] [--json out.json]
//
// Without --url the dev relay runs in-process (netplay/dev-relay.mjs); with
// it, any v2 server (e.g. `wrangler dev`).  The cast, all paced at the
// GBA's 59.73 Hz by a shared vsync shim:
//
//   alpha  creates the room, plays; the reference for "never stalls".
//   beta   joins by code (typed lower-case with a look-alike), plays, over
//          a bad link: 120 ms each way.  Partway through, its RAM is
//          corrupted (gRngVal) -- the injected desync.
//   gamma  arrives late, by code, on a slow device (30 Hz frame loop),
//          replays the archive, sits, plays.
//
// PASS:
//   - alpha never stalls: no gap between its frames reaches 150 ms, and its
//     rate stays >= 50/s over every second (rate dilation, the deliberate
//     held frame for time sync, costs at most 10%) -- nobody waits for anybody;
//   - the desync is detected by the room and repaired by a world sync
//     within T_REPAIR seconds;
//   - the slow joiner converges (its checkpoints match after its syncs);
//   - at the end every instance reports identical hashes for the last
//     checkpoints (the hash log tail agrees);
//   - numbers: bytes per client per second, sync count/bytes, seal stats.

import { createRequire } from 'module';
import path from 'path';
import fs from 'fs';
import { fileURLToPath } from 'url';

const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const argv = process.argv.slice(2);
const flag = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 ? argv[i + 1] : d; };
const [modulePath, romPath] = argv.filter((a, i) => !a.startsWith('--') && !(i > 0 && argv[i - 1].startsWith('--')));
if (!modulePath || !romPath) {
    console.error('usage: node tools/netplay_v2_test.mjs <katam-node.js> <rom.gba> [--url U]');
    process.exit(2);
}
const SECONDS = +flag('seconds', 40);
const T_REPAIR = 6;
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

// --- vsync shim: one frame per instance per 16.74 ms tick ----------------
// portPresent runs synchronously right before the instance registers its
// rAF, so `current` names the instance that owns the registration.
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
            if (inst.Module.portNetHold > 0) { inst.Module.portNetHold--; continue; }
            const cb = inst.raf;
            inst.raf = null;
            cb(performance.now());
        }
    }
}, 2);

// --- a WebSocket with latency, for the bad link -----------------------------
function laggyWebSocket(ms) {
    return class extends WebSocket {
        constructor(u) {
            super(u);
            const self = this;
            let handler = null;
            super.onmessage = (e) => setTimeout(() => handler && handler.call(self, e), ms);
            Object.defineProperty(this, 'onmessage', { set(f) { handler = f; }, get() { return handler; } });
        }
        send(d) { setTimeout(() => { if (this.readyState === 1) super.send(d); }, ms); }
    };
}

const SIM = 'netplay-v2-test';
const ROMHASH = (await import('crypto')).createHash('sha1').update(rom).digest('hex');

function makeInstance(name, { code, create, every = 1, linkMs = 0, script }) {
    const inst = { name, every, logs: [], hashes: new Map(), frames: [], fps: [], raf: null };
    const log = (t) => inst.logs.push(`[${name}] ${t}`);
    let resolveRom;
    const Module = {
        portRomReady: new Promise((r) => { resolveRom = r; }),
        portAudioRate: 0,
        print: log, printErr: log,
        portDiag: (t) => log(t),
        portPresent() {
            current = inst;
            const f = Module._PortFrameNumber();
            Module._PortSetKeys(script(f));
            inst.game.tick();
            inst.frames.push([performance.now(), f]);
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
        WebSocket: linkMs ? laggyWebSocket(linkMs) : undefined,
        hello: { sim: SIM, rom: ROMHASH, content: 'vanilla', name, platform: 'node',
                 caps: { seats: 4, sync: true }, want: 'play', create,
                 view: { w: 240, h: 160 } },
    });
    inst.net.on('error', (e) => console.log(`[${name}] room error: ${KatamNet.explainError(e)}`));
    inst.net.on('desync', (d) => log(`desync at ${d.f}: ${JSON.stringify(d.groups)}`));
    instances.push(inst);
    createKatam(Module).then(() => {
        inst.game = createNetGame({
            Module, net: inst.net, log, want: 'play',
            onHash: (f, h) => inst.hashes.set(f, h),
            onCorrection: (c) => { log(`correction at ${c.frame}: ${c.pages} pages, ${c.bytes} B (${c.reason})`); inst.lastCorrection = { ...c, t: performance.now() }; },
        });
        inst.game.ready.then(() => resolveRom(), (e) => { console.error(name, e); process.exit(1); });
    });
    return inst;
}

// Scripts: bursts of walking and jumping, different per player.
const walker = (seed) => (f) => {
    if (f < 2300) return 0;
    const phase = Math.floor((f + seed * 37) / 90) % 6;
    return [0x10, 0, 0x20, 0x11, 0, 0x01][phase] | ((f + seed) % 47 < 3 ? 1 : 0);
};

const created = await KatamNet.createRoom(base);
if (!created.code) { console.error('create failed', created); process.exit(1); }
const CODE = created.code;
console.log(`[test] room ${CODE} at ${base}`);
const typed = CODE.toLowerCase().replace('B', '8').replace('S', '5');     // look-alikes

const alpha = makeInstance('alpha', { code: CODE, script: walker(1),
    create: { deadlineMs: 100, syncEveryMs: 8000, hashEvery: 30 } });
await new Promise((r) => setTimeout(r, 300));
const beta = makeInstance('beta', { code: KatamNet.normalizeCode(typed), script: walker(2), linkMs: 120 });

// Start once both are seated.
await new Promise((resolve) => {
    const iv = setInterval(() => {
        const r = alpha.net.roster;
        if (r && r.members.filter((m) => m.seat >= 0).length === 2 && alpha.net.mid === r.host) {
            clearInterval(iv);
            alpha.net.start();
            resolve();
        }
    }, 50);
});
const tStart = performance.now();
console.log('[test] started');

let gamma = null, corruptedAt = null, corruptFrame = 0, repairedAt = null;
const tl = [];
const frameOf = (x) => (x.game && x.Module._PortFrameNumber) ? x.Module._PortFrameNumber() : 0;
await new Promise((resolve) => {
    const iv = setInterval(() => {
        const t = (performance.now() - tStart) / 1000;
        const fa = alpha.Module._PortFrameNumber && alpha.Module._PortFrameNumber();
        if (!gamma && t > +flag('gamma-at', 10)) {
            console.log(`[test] t=${t.toFixed(1)}s alpha at ${fa}: gamma joins by code (30 Hz device)`);
            gamma = makeInstance('gamma', { code: CODE, script: walker(3), every: 2 });
        }
        if (!corruptedAt && t > +flag('corrupt-at', 18)) {
            const b = beta.Module;
            corruptFrame = b._PortFrameNumber();
            const dv = new DataView(b.HEAPU8.buffer);
            dv.setUint32(0x030068D8, dv.getUint32(0x030068D8, true) ^ 0x5A5A1234, true);
            corruptedAt = performance.now();
            console.log(`[test] t=${t.toFixed(1)}s corrupted beta's gRngVal at its frame ${corruptFrame}`);
        }
        if (corruptedAt && !repairedAt) {
            // repaired: a checkpoint after the corruption where beta == alpha
            for (const [f, h] of beta.hashes)
                if (f > corruptFrame + 60 && alpha.hashes.get(f) === h && beta.lastCorrection && beta.lastCorrection.t > corruptedAt) {
                    repairedAt = performance.now();
                    console.log(`[test] desync repaired ${((repairedAt - corruptedAt) / 1000).toFixed(2)} s after the corruption (checkpoint ${f})`);
                    break;
                }
        }
        if (Math.floor(t) !== tl.length) tl.push({ t: Math.floor(t), a: fa, b: frameOf(beta), g: gamma && frameOf(gamma) });
        if (t >= SECONDS) { clearInterval(iv); resolve(); }
    }, 50);
});

// --- verdicts -----------------------------------------------------------------
const secs = (performance.now() - tStart) / 1000;
function minFps(inst, fromFrame) {
    // frames per second over 1 s windows, once live
    const fr = inst.frames.filter(([, f]) => f >= fromFrame);
    let worst = Infinity;
    for (let i = 0, j = 0; i < fr.length; i++) {
        while (fr[i][0] - fr[j][0] > 1000) j++;
        if (fr[i][0] - fr[0][0] >= 1000) worst = Math.min(worst, i - j);
    }
    return worst;
}
const alphaMin = minFps(alpha, 2300);
// A stall is a gap between presented frames; rate dilation (a held frame,
// at most 6 a second) is two periods, ~33 ms, and is not one.
function maxGap(inst, fromFrame) {
    const fr = inst.frames.filter(([, f]) => f >= fromFrame);
    let g = 0;
    for (let i = 1; i < fr.length; i++) g = Math.max(g, fr[i][0] - fr[i - 1][0]);
    return Math.round(g);
}
const lastCommon = [...alpha.hashes.keys()].filter((f) => beta.hashes.has(f) && gamma && gamma.hashes.has(f)).sort((x, y) => x - y);
// Checkpoints still inside the repair window at the end may be mid-repair.
const endFrame = Math.min(frameOf(alpha), frameOf(beta), frameOf(gamma));
const tail = lastCommon.filter((f) => f < endFrame - T_REPAIR * 60).slice(-10);
const tailAgree = tail.length >= 5 && tail.every((f) => alpha.hashes.get(f) === beta.hashes.get(f) && alpha.hashes.get(f) === gamma.hashes.get(f));
const mismatches = (x) => [...x.hashes].filter(([f, h]) => alpha.hashes.has(f) && alpha.hashes.get(f) !== h).length;
const core = relay ? [...relay.v2.rooms.values()][0].core : null;
const res = {
    code: CODE, seconds: +secs.toFixed(1),
    alphaMinFps: alphaMin, alphaMaxGapMs: maxGap(alpha, 2300), betaMaxGapMs: maxGap(beta, 2300),
    frames: { alpha: frameOf(alpha), beta: frameOf(beta), gamma: frameOf(gamma) },
    checkpoints: { alpha: alpha.hashes.size, beta: beta.hashes.size, gamma: gamma.hashes.size },
    mismatchedCheckpoints: { beta: mismatches(beta), gamma: mismatches(gamma) },
    tail: tail.map((f) => [f, alpha.hashes.get(f), beta.hashes.get(f), gamma.hashes.get(f)]),
    tailAgree,
    repairSeconds: repairedAt ? +((repairedAt - corruptedAt) / 1000).toFixed(2) : null,
    syncs: Object.fromEntries([alpha, beta, gamma].map((x) => [x.name, x.game ? { n: x.game.state.syncs, pages: x.game.state.syncPages, bytes: x.game.state.syncBytes, ffwd: x.game.state.ffwd, held: x.game.state.held, delay: x.game.state.delay } : null])),
    bytesPerSec: Object.fromEntries([alpha, beta, gamma].map((x) => [x.name, { up: Math.round(x.net.bytesUp / secs), down: Math.round(x.net.bytesDown / secs) }])),
    room: core && { sealed: core.stats.sealed, deadlineSeals: core.stats.deadlineSeals, fills: core.stats.fills, lateInputs: core.stats.lateInputs,
                    desyncs: core.stats.desyncs, syncs: core.stats.syncs, syncFails: core.stats.syncFails, syncBytes: core.stats.syncBytes,
                    meanSealMs: +(core.stats.sealLatencyMs / Math.max(1, core.stats.sealed)).toFixed(1) },
    timeline: tl.filter((_, i) => i % 5 === 0),
    states: Object.fromEntries([alpha, beta, gamma].map((x) => [x.name, { net: x.net.state, role: x.net.role, seat: x.net.seat, phase: x.net.phase, game: x.game && x.game.state.phase, engine: x.game && x.game.engineStats(), logs: x.logs.length }])),
};
console.log(JSON.stringify(res, null, 1));
if (flag('json')) fs.writeFileSync(flag('json'), JSON.stringify(res, null, 1));
const fails = [];
if (!(alphaMin >= 50)) fails.push(`alpha dropped to ${alphaMin} frames/s`);
if (!(res.alphaMaxGapMs < 150)) fails.push(`alpha stalled for ${res.alphaMaxGapMs} ms`);
if (!res.repairSeconds || res.repairSeconds > T_REPAIR) fails.push(`desync not repaired within ${T_REPAIR}s`);
if (!tailAgree) fails.push('hash log tails differ');
if (core && !core.stats.desyncs) fails.push('room never saw the desync');
if (fails.length) {
    for (const i of [alpha, beta, gamma]) console.log(i.logs.filter((l) => /net|correction|desync|rollback: (net|frame)/.test(l)).slice(-25).join('\n'));
    if (process.env.V2_DEBUG) console.log(gamma.logs.filter((l) => !/DMA/.test(l)).slice(0, 60).join('\n'));
    console.log('NETPLAY V2 TEST FAILED: ' + fails.join('; '));
    process.exit(1);
}
console.log('NETPLAY V2 TEST PASSED');
process.exit(0);
