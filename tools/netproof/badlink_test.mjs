#!/usr/bin/env node
// Bad connections at party size: does one player's (or several players')
// bad link make the game worse for everyone else?  A rig gate, wasm in node,
// over the engine and the page's own session code (web/ln_session.js HostHub
// / HostSession / GuestSession, the page's liveness rules), 8 devices on the
// 16-Kirby module as web/online.js runs an 8-player room (seats 8, 16
// keyframes, bounded redundancy: copies 4, packets at most 1100 B).
//
//   node tools/netproof/badlink_test.mjs [--rom ROM] [--module16 build/w16/katam-node.js]
//        [--jobs N] [--only control,one,...] [--frames N] [--out DIR]
//
// The host and 7 guests, one person each, all playing as people do (the
// rig's scripted humans) for --frames frames (default 2100, 35 s), on a
// virtual clock: every run is deterministic, and the same seed gives every
// device the same pad script and every link the same random stream in every
// case, so the cases differ only by the links named below.
//
//   GOOD   25 ms one way (50 ms round trip), jitter 8, loss 0.5 %
//   BAD_A  150 ms one way (300 ms round trip), jitter 30, loss 5 %
//   BAD_B  75 ms one way (150 ms round trip), jitter 60 (heavy), loss 1 %
//   BAD_C  250 ms one way (500 ms round trip), jitter 20, loss 10 %
//          (a relayed link at its worst: the rig has no relay, so its
//          latency and loss stand for it)
//
// Cases (each its own process, --jobs at a time):
//   control   every guest GOOD
//   one       guest 7 BAD_A, guests 1-6 GOOD
//   aloneB    guest 5 BAD_B, the rest GOOD     (BAD_B on its own)
//   aloneC    guest 6 BAD_C, the rest GOOD     (BAD_C on its own)
//   several   guests 5, 6, 7 BAD_B, BAD_C, BAD_A; guests 1-4 GOOD
//
// Checks (against control, with the case's good guests):
//   - the host: the same frames run, never held (it never waits for anyone)
//   - the good guests: holds, skips, bursts, rewinds per minute, rewind
//     depth, misprediction of the others within a small tolerance of the
//     same guests in control; no repair image, no mismatch
//   - every bad guest stays in the game: never to the CPU, never gave up,
//     lead bounded (never at the history cap; p90 within its round trip +
//     jitter + 20 frames), at most one repair image, frames identical
//   - several: each bad guest no worse than in its own case alone (one /
//     aloneB / aloneC), within the same tolerance
// The numbers are printed side by side at the end (one line per metric).
import fs from 'fs';
import os from 'os';
import path from 'path';
import { spawn } from 'child_process';
import { fileURLToPath } from 'url';

const here = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(here, '../..');
const SELF = fileURLToPath(import.meta.url);
const argv = process.argv.slice(2);
const flag = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 ? argv[i + 1] : d; };
const ROM = path.resolve(flag('rom', process.env.ROM || '/bigdata/agent-tom/projects/katamport/decomp/baserom.gba'));
const MODULE16 = path.resolve(flag('module16', path.join(ROOT, 'build/w16/katam-node.js')));
const FRAMES = +flag('frames', 2100);
const DEVICES = 8;

const GOOD = { oneWay: 25, jitter: 8, loss: 0.005 };
const BAD = {
    A: { oneWay: 150, jitter: 30, loss: 0.05 },
    B: { oneWay: 75, jitter: 60, loss: 0.01 },
    C: { oneWay: 250, jitter: 20, loss: 0.10 },
};
// case -> { guest dev: bad link name }
const CASES = {
    control: {},
    one: { 7: 'A' },
    aloneB: { 5: 'B' },
    aloneC: { 6: 'C' },
    several: { 5: 'B', 6: 'C', 7: 'A' },
};
// where each bad link is alone (several's comparison)
const ALONE = { A: 'one', B: 'aloneB', C: 'aloneC' };

// --- one case, in this process ---------------------------------------------------------
async function runCase(name) {
    const { runSession } = await import('./run.mjs');
    const { buildScenario, kirbyAddrOf } = await import('./scenario.mjs');
    const { PERIOD } = await import('./session.mjs');
    const rom = fs.readFileSync(ROM);
    if (!fs.existsSync(MODULE16)) return { error: `no module ${MODULE16}` };
    const bad = CASES[name];
    const guests = [];
    for (let d = 1; d < DEVICES; d++)
        guests.push({ dev: d, link: Object.assign({}, bad[d] ? BAD[bad[d]] : GOOD), badLink: bad[d] || null });
    const sc = buildScenario({ seed: 5, skipScripted: true, humanFrames: FRAMES + 600, kirbyAddr: kirbyAddrOf(16) });
    const kfCount = 16, kfEvery = 15;
    let r;
    try {
        r = await runSession({
            name, modulePath: MODULE16, rom, scenario: sc, durationFrames: FRAMES, guests, link: GOOD,
            liveness: {}, seats: DEVICES, kirbys: DEVICES, kfCount, kfEvery, storyNormal: true,
            hostOpts: { copies: 4, maxPacketBytes: 1100 },
        });
    } catch (e) { return { error: String(e.stack || e).split('\n').slice(0, 4).join(' | ') }; }
    const churn = r.churn.filter((x) => x.what !== 'admit' || x.why !== 'start');
    const hs = r.host.session.perDev || [];
    const perDev = {};
    r.guests.forEach((g, i) => {
        if (g.gen !== 0) return;
        const s = g.session, f = g.feel, tel = r.guestTels[i];
        // the host's count of this guest's keys that came after their frame
        // (web/ln_session.js HostSession: the connection readout's late share)
        const h = hs.find((x) => x.name === g.name) || {};
        perDev[g.dev] = {
            badLink: bad[g.dev] || null,
            holdsPerMin: f.holdsPerMin, skipsPerMin: f.skipsPerMin, burstsPerMin: f.burstsPerMin, capHolds: s.capHolds,
            rewindsPerMin: f.rewindsPerMin, replayedPerSec: f.replayedFramesPerSec,
            depthP90: f.depth.p90 ?? 0, depthMax: f.depth.max ?? 0,
            mispredOtherPct: f.mispredOtherPct, ownOverriddenPct: f.ownOverriddenPct,
            lateAtHostPct: h.inputFrames ? +(100 * h.inputsUseless / h.inputFrames).toFixed(2) : null,
            leadP50: g.lead.p50 ?? null, leadP90: g.lead.p90 ?? null, leadMax: g.leadMax,
            margin: g.margin.p50 ?? null, rttMs: g.rtt === null ? null : Math.round(g.rtt),
            images: tel.imagesLoaded, repairs: Math.max(0, tel.imagesLoaded - 1), repairReqs: s.repairReqs,
            mismatched: g.compare.mismatched, compared: g.compare.compared, expected: g.compare.expected,
            gaveUp: g.gaveUp, toCpu: churn.filter((x) => x.what === 'cpu' && x.dev === g.dev).length,
        };
    });
    return {
        name, wallSec: r.wallSec,
        leadCap: kfEvery * (kfCount - 2) - 10,
        host: { frames: r.frames.hostFinal - r.frames.start, heldTicks: r.hostHeld, reanchors: r.host.reanchors,
                rewinds: r.host.tel.hostRewinds, resends: hs.reduce((a, x) => a + (x.resends || 0), 0),
                liveness: r.liveness },
        churn, devs: perDev, compare: r.compare, owners: r.owners,
    };
}

// --- the comparisons -----------------------------------------------------------------------
// a guest metric is "within tolerance" of its reference: at most rel above it
// plus abs (noise: the bad Kirby plays differently, so the game differs a little)
const TOL = {
    holdsPerMin: [0.25, 6], skipsPerMin: [0.25, 6], burstsPerMin: [0.5, 0.5], rewindsPerMin: [0.25, 15],
    replayedPerSec: [0.3, 5], depthP90: [0.2, 3], mispredOtherPct: [0.3, 1.0], repairs: [0, 0],
};
const within = (v, ref, [rel, abs]) => v <= ref * (1 + rel) + abs;
const mean = (xs) => (xs.length ? xs.reduce((a, b) => a + b, 0) / xs.length : 0);

function checks(R, ok) {
    const C = R.control;
    if (!C) return;
    const goodDevs = (k) => Object.keys(k.devs).map(Number).filter((d) => !k.devs[d].badLink);
    for (const name of Object.keys(R)) {
        const k = R[name];
        ok(name, 'every compared frame identical', k.compare.mismatched === 0 && k.compare.compared > 0,
           `${k.compare.equal}/${k.compare.compared}`);
        ok(name, 'nobody to the CPU, nobody gave up', !k.churn.some((x) => ['cpu', 'gone', 'gaveup', 'dead'].includes(x.what)),
           JSON.stringify(k.churn));
        ok(name, 'every guest in a seat at the end', k.owners.slice(1, DEVICES).every((o) => o >= 0), JSON.stringify(k.owners));
        ok(name, 'the host never held', k.host.heldTicks === 0, `${k.host.heldTicks} ticks`);
        if (name === 'control') {
            for (const d of goodDevs(k))
                ok(name, `guest ${d}: no repair image`, k.devs[d].repairs === 0, String(k.devs[d].repairs));
            continue;
        }
        ok(name, 'the host ran the same frames as in control', Math.abs(k.host.frames - C.host.frames) <= 2,
           `${k.host.frames} vs ${C.host.frames}`);
        // the good guests: each against itself in control, and their mean
        const good = goodDevs(k);
        for (const d of good) {
            const g = k.devs[d], c = C.devs[d];
            const over = Object.keys(TOL).filter((m) => !within(g[m], c[m], TOL[m]));
            ok(name, `good guest ${d} not degraded vs control`, !over.length && g.mismatched === 0,
               over.map((m) => `${m} ${g[m]} vs ${c[m]}`).join(', ') || 'all within');
        }
        for (const m of ['rewindsPerMin', 'holdsPerMin', 'skipsPerMin', 'depthP90']) {
            const a = mean(good.map((d) => k.devs[d][m])), b = mean(good.map((d) => C.devs[d][m]));
            const [rel, abs] = TOL[m];
            ok(name, `good guests' mean ${m} within tolerance of control`, a <= b * (1 + rel / 2) + abs / 2,
               `${a.toFixed(2)} vs ${b.toFixed(2)}`);
        }
        // the bad guests stay in the game, lead bounded, no repair storm
        for (const d of Object.keys(k.devs).map(Number).filter((x) => k.devs[x].badLink)) {
            const g = k.devs[d], L = BAD[g.badLink];
            const leadBound = Math.ceil((2 * L.oneWay + 2 * L.jitter) / (1000 / 59.7275)) + 20;
            ok(name, `bad guest ${d} (${g.badLink}) in the game`, !g.gaveUp && !g.toCpu && g.compared > g.expected * 0.8,
               `compared ${g.compared}/${g.expected}`);
            ok(name, `bad guest ${d} (${g.badLink}) lead bounded`, g.leadMax < k.leadCap && g.leadP90 <= leadBound,
               `p90 ${g.leadP90} (bound ${leadBound}), max ${g.leadMax} (cap ${k.leadCap})`);
            ok(name, `bad guest ${d} (${g.badLink}) no repair storm`, g.repairs <= 1 && g.repairReqs <= 2,
               `repairs ${g.repairs}, requests ${g.repairReqs}`);
            if (name === 'several') {
                const A = R[ALONE[g.badLink]];
                if (!A) continue;
                const a = Object.values(A.devs).find((x) => x.badLink === g.badLink);
                const over = ['holdsPerMin', 'skipsPerMin', 'rewindsPerMin', 'depthP90', 'repairs']
                    .filter((m) => !within(g[m], a[m], TOL[m]));
                ok(name, `bad guest ${d} (${g.badLink}) no worse than alone (${ALONE[g.badLink]})`, !over.length,
                   over.map((m) => `${m} ${g[m]} vs ${a[m]}`).join(', ') || 'all within');
            }
        }
    }
}

function table(R) {
    const names = Object.keys(CASES).filter((n) => R[n]);
    const L = [];
    const pad = (s, n) => String(s).padStart(n);
    L.push('host:' + names.map((n) => ` ${n} frames ${R[n].host.frames} held ${R[n].host.heldTicks} resends ${R[n].host.resends}`).join(' |'));
    const metrics = ['holdsPerMin', 'skipsPerMin', 'burstsPerMin', 'rewindsPerMin', 'replayedPerSec', 'depthP90', 'depthMax',
                     'mispredOtherPct', 'ownOverriddenPct', 'lateAtHostPct', 'leadP50', 'leadP90', 'leadMax', 'rttMs', 'repairs'];
    L.push(pad('dev', 4) + pad('metric', 17) + names.map((n) => pad(n, 9)).join(''));
    for (let d = 1; d < DEVICES; d++)
        for (const m of metrics)
            L.push(pad(d, 4) + pad(m, 17) + names.map((n) => {
                const x = R[n].devs[d];
                return pad(x ? `${x[m]}${x.badLink ? x.badLink : ''}` : '-', 9);
            }).join(''));
    return L.join('\n');
}

// --- the driver ------------------------------------------------------------------------------
const one = flag('case', null);
if (one) {
    const res = await runCase(one);
    fs.writeFileSync(flag('json', '/dev/stdout'), JSON.stringify(res));
    process.exit(res.error ? 1 : 0);
}
const ONLY = flag('only', null);
const todo = Object.keys(CASES).filter((k) => !ONLY || ONLY.split(',').includes(k));
const JOBS = Math.max(1, +flag('jobs', Math.min(5, os.cpus().length)));
const OUT = flag('out', null);
const dir = OUT ? path.resolve(OUT) : fs.mkdtempSync(path.join(os.tmpdir(), 'badlink-'));
fs.mkdirSync(dir, { recursive: true });
const t0 = Date.now();
let pass = 0, fail = 0, next = 0;
const R = {};
const ok = (k, name, c, extra) => {
    if (c) pass++; else fail++;
    console.log(`  ${c ? 'ok  ' : 'FAIL'} ${k}: ${name}${extra !== undefined ? '  (' + extra + ')' : ''}`);
};
async function worker() {
    while (next < todo.length) {
        const k = todo[next++];
        const jf = path.join(dir, `${k}.json`), lf = path.join(dir, `${k}.log`);
        const code = await new Promise((resolve) => {
            const fd = fs.openSync(lf, 'w');
            const p = spawn(process.execPath, [SELF, '--case', k, '--json', jf, '--rom', ROM, '--module16', MODULE16, '--frames', String(FRAMES)],
                            { cwd: ROOT, stdio: ['ignore', fd, fd] });
            p.on('exit', (c) => { fs.closeSync(fd); resolve(c === null ? 128 : c); });
        });
        let res = null;
        try { res = JSON.parse(fs.readFileSync(jf, 'utf8')); } catch (e) { /* crashed */ }
        if (res && !res.error) R[k] = res;
        else ok(k, 'ran', false, res ? res.error : `exit ${code}: ${fs.readFileSync(lf, 'utf8').split('\n').slice(-6).join(' | ')}`);
        console.log(`[badlink ${((Date.now() - t0) / 1000).toFixed(0)}s] ${k} done (exit ${code}, ${res && res.wallSec} s)`);
    }
}
await Promise.all(Array.from({ length: Math.min(JOBS, todo.length) }, worker));
checks(R, ok);
fs.writeFileSync(path.join(dir, 'badlink.json'), JSON.stringify(R, null, 1));
console.log('\n' + table(R));
console.log(`results in ${dir}`);
console.log(`\n${pass} passed, ${fail} failed (badlink, ${((Date.now() - t0) / 1000).toFixed(0)} s)`);
process.exit(fail ? 1 : 0);
