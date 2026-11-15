#!/usr/bin/env node
// Online local multiplayer, proof-of-concept rig: reruns every gate and
// writes RIG-REPORT.md + rig-report.json.
//
//   node tools/netproof.mjs [--rom ROM] [--module build/katam-node.js]
//        [--out DIR] [--quick] [--skip-r11] [--only R2,R4] [--recording FILE]
//        [--seeds 3] [--jobs N]
//
// Wasm instances of the same build in one node process -- a host and one to
// three guests, each guest on its own seeded discrete-event link
// (tools/netproof/link.mjs) -- on a virtual clock,
// the session logic (tools/netproof/session.mjs) and the engine
// (platform/lmpnet.c).  Every run is reproducible from its config.
// R0-R12 are the two-device gates; R13 slots (3-4 devices, two local players
// on one device, churn, a full game, slot reuse, the slot-table ablation),
// R14 the link matrix on three devices, R15 feel per guest for 2/3/4 devices.
import fs from 'fs';
import path from 'path';
import os from 'os';
import { execFileSync, spawnSync } from 'child_process';
import { fileURLToPath } from 'url';
import { runSession, dist, pct } from './netproof/run.mjs';
import { buildScenario, loadRecording, BOOT_END, SCRIPTED_END } from './netproof/scenario.mjs';
import { layout, diffImages } from './netproof/localise.mjs';
import { createInstance } from './netproof/instance.mjs';
import { PERIOD } from './netproof/session.mjs';
import { CAUSES, NOTES } from './netproof/causes.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(here, '..');
const argv = process.argv.slice(2);
const flag = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 ? argv[i + 1] : d; };
const has = (k) => argv.includes('--' + k);
const ROM = flag('rom', process.env.ROM || '/bigdata/agent-tom/projects/katamport/decomp/baserom.gba');
const MODULE = path.resolve(flag('module', path.join(ROOT, 'build/katam-node.js')));
const OUT = path.resolve(flag('out', process.env.NETPROOF_OUT || path.join(ROOT, '../netproof-eval')));
const QUICK = has('quick');
const ONLY = flag('only', '') ? flag('only').split(',') : null;
const SEEDS = +flag('seeds', QUICK ? 1 : 3);
const RECORDING = null;
const AUDIT = path.join(ROOT, '../netaudit-wt/tests/recordings');
// --recording a.txt,b.txt (default: the audit agent's local-multiplayer
// recordings when they exist; --recording none for none)
const RECORDINGS = (flag('recording', '') ? (flag('recording') === 'none' ? [] : flag('recording').split(','))
    : ['localmp_tour.txt', 'localmp_menus.txt'].map((f) => path.join(AUDIT, f))).filter((f) => fs.existsSync(f));
const want = (g) => !ONLY || ONLY.includes(g);
const rom = fs.readFileSync(ROM);
const t0 = Date.now();
const log = (...a) => console.log(`[netproof ${((Date.now() - t0) / 1000).toFixed(0)}s]`, ...a);

const KIRBY = 0x02020EE0, KSIZE = 0x1A8;
const SCRIPTED = SCRIPTED_END - BOOT_END;          // frames of scripted scenario after the session starts
const MATRIX_FRAMES = QUICK ? SCRIPTED + 1800 : SCRIPTED + 3600;
const HEADLINE_FRAMES = QUICK ? SCRIPTED + 6000 : SCRIPTED + 36000;   // >= 10 min of free play

const runs = [];          // every run's summary (no instances)
const localisations = []; // every distinct mismatch, localised

function slim(r) {
    const o = Object.assign({}, r);
    delete o._host; delete o._guest; delete o.dumps;
    return o;
}

async function run(name, cfg, scenarioOpts = {}) {
    const sc = cfg.scenario || (RECORDING && cfg.useRecording ? loadRecording(RECORDING) : buildScenario(Object.assign({ seed: cfg.seed || 1, humanFrames: Math.max(0, (cfg.durationFrames || 6000) - SCRIPTED + 600) }, scenarioOpts)));
    const r = await runSession(Object.assign({ name, modulePath: MODULE, rom, scenario: sc }, cfg));
    const s = slim(r);
    runs.push(s);
    const tag = r.compare.mismatched ? `MISMATCH x${r.compare.mismatched} first@${r.compare.firstMismatch}` : 'identical';
    log(`${name}: ${r.compare.equal}/${r.compare.compared} frames ${tag}; rewinds ${r.guest.tel.rewinds}, images ${r.guest.tel.imagesLoaded}, wall ${r.wallSec}s`);
    if (r.compare.mismatched && !cfg.noLocalise) await localise(name, cfg, sc, r);
    r._host = r._guest = null;
    return s;
}

// Re-run the same deterministic session with full dumps at the first
// mismatching frame, and diff the two images.
async function localise(name, cfg, sc, r) {
    const f = r.compare.firstMismatch;
    const rr = await runSession(Object.assign({ name: name + '/localise', modulePath: MODULE, rom, scenario: sc }, cfg, { dumpAt: f, corrupt: (cfg.corrupt || []).map((c) => Object.assign({}, c, { done: false })) }));
    const d = rr.dumps || {};
    const rec = { run: name, frame: f, regions: r.mismatches[0] ? r.mismatches[0].regions : [], reproduced: rr.compare.firstMismatch === f };
    if (d.host && d.guest && d.host.frame === f && d.guest.frame === f) {
        const parts = layout(rr._host);
        const xs = rr._host.M._PortLnImageBytes() - 64 - (parts[parts.length - 1].offset + parts[parts.length - 1].size);
        if (xs > 0) parts.push({ name: 'host', base: 0, offset: parts[parts.length - 1].offset + parts[parts.length - 1].size, size: xs });
        const df = diffImages(d.host.bytes, d.guest.bytes, parts);
        rec.bytesByRegion = df.regions;
        rec.runs = df.runs.slice(0, 24);
        fs.mkdirSync(path.join(OUT, 'rig-tmp', 'dumps'), { recursive: true });
        const base = path.join(OUT, 'rig-tmp', 'dumps', `${name.replace(/[^\w.-]+/g, '_')}_f${f}`);
        fs.writeFileSync(base + '.host.bin', d.host.bytes);
        fs.writeFileSync(base + '.guest.bin', d.guest.bytes);
        rec.dumps = base + '.{host,guest}.bin';
    } else rec.note = 'dump not captured';
    localisations.push(rec);
    log(`  localised ${name} f${f}: ${JSON.stringify(rec.bytesByRegion || rec.note)} ${(rec.runs || []).slice(0, 4).map((x) => x.symbol).join(', ')}`);
}

// --- link conditions --------------------------------------------------------
const L = (oneWay, jitter, loss, extra = {}) => Object.assign({ oneWay, jitter, loss }, extra);
const matrix = [];
for (const ow of [0, 30, 60, 120, 200])
    for (const loss of [0, 0.02, 0.10])
        for (const rough of [false, true]) {
            if (QUICK && rough && loss === 0.02) continue;
            const j = rough ? Math.round(ow / 4) + 5 : 0;
            matrix.push({ id: `${ow}ms j${j} loss${loss * 100}%${rough ? ' reorder5% dup2%' : ''}`, link: L(ow, j, loss, rough ? { reorder: 0.05, reorderMs: 40, dup: 0.02 } : {}) });
        }
const FREE = (SCRIPTED + 300) * PERIOD;   // ms after the session start: free play, both players busy
const HEADLINE = L(120, 30, 0.02, { reorder: 0.02, reorderMs: 30, dup: 0.01 });

const report = { started: new Date().toISOString(), rom: path.basename(ROM), module: MODULE, quick: QUICK, gates: {}, notes: [] };

// ---------------------------------------------------------------------------
async function main() {
    fs.mkdirSync(OUT, { recursive: true });
    // R0: rig sanity
    if (want('R0')) {
        const a = await run('R0 lockstep, perfect link', { link: L(0, 0, 0), lockstep: true, durationFrames: MATRIX_FRAMES });
        const b = await run('R0 predicted, perfect link', { link: L(0, 0, 0), durationFrames: MATRIX_FRAMES });
        const c = await run('R0 guest renders, host does not', { link: L(0, 0, 0), durationFrames: QUICK ? 2000 : 4000, renderGuest: true });
        const ok = [a, b, c].every((r) => r.compare.mismatched === 0 && r.compare.compared >= r.compare.expected - 2);
        report.gates.R0 = { verdict: ok ? 'PASS' : 'FAIL', runs: [a, b, c].map(brief),
            text: `lockstep: ${a.compare.equal}/${a.compare.expected} frames identical, ${a.guest.tel.rewinds} rewinds; predicted: ${b.compare.equal}/${b.compare.expected}, ${b.guest.tel.rewinds} rewinds; guest rendering: ${c.compare.equal}/${c.compare.expected}` };
    }
    // R1: snapshot exactness: forced deep rewinds on a perfect link through
    // the scripted scenario (doors, carries, boss, death, pause, phone)
    if (want('R1')) {
        const a = await run('R1 forced rewind 150f every 37f', { link: L(0, 0, 0), forceRewind: { every: 37, depth: 150 }, durationFrames: MATRIX_FRAMES });
        const b = await run('R1 forced rewind 330f every 101f', { link: L(0, 0, 0), forceRewind: { every: 101, depth: 330 }, durationFrames: MATRIX_FRAMES });
        const c = await run('R1 keyframe every frame, rewind 20f every 7f', { link: L(0, 0, 0), kfEvery: 1, kfCount: 24, forceRewind: { every: 7, depth: 20 }, durationFrames: QUICK ? 2500 : MATRIX_FRAMES });
        const ok = [a, b, c].every((r) => r.compare.mismatched === 0 && r.guest.tel.rewindsForced > 10);
        report.gates.R1 = { verdict: ok ? 'PASS' : 'FAIL', runs: [a, b, c].map(brief),
            text: [a, b, c].map((r) => `${r.name}: ${r.guest.tel.rewindsForced} forced rewinds, ${r.guest.tel.replayedFrames} frames replayed, ${r.compare.equal}/${r.compare.compared} identical`).join('; '),
            coverage: a.coverage };
    }
    // R2: convergence matrix and special conditions
    if (want('R2')) {
        const m = [];
        for (const x of matrix) m.push(await run(`R2 ${x.id}`, { link: x.link, durationFrames: MATRIX_FRAMES, seed: 11 }));
        const sp = [];
        sp.push(await run('R2 outage 0.5s @120ms', { link: Object.assign({}, HEADLINE, { outages: [[FREE, 500], [FREE + 20000, 500]] }), durationFrames: MATRIX_FRAMES }));
        sp.push(await run('R2 outage 3s @120ms', { link: Object.assign({}, HEADLINE, { outages: [[FREE, 3000], [FREE + 25000, 3000]] }), durationFrames: MATRIX_FRAMES }));
        sp.push(await run('R2 guest frozen 5s', { link: HEADLINE, guest: { freezes: [[FREE, 5000]] }, durationFrames: MATRIX_FRAMES }));
        sp.push(await run('R2 host frozen 5s', { link: HEADLINE, host: { freezes: [[FREE, 5000]] }, durationFrames: MATRIX_FRAMES }));
        sp.push(await run('R2 guest at 30Hz', { link: HEADLINE, guest: { rate: 30 }, durationFrames: MATRIX_FRAMES }));
        sp.push(await run('R2 host at 30Hz', { link: HEADLINE, host: { rate: 30 }, durationFrames: MATRIX_FRAMES }));
        sp.push(await run('R2 guest clock +0.5%', { link: HEADLINE, guest: { drift: 0.005 }, durationFrames: MATRIX_FRAMES }));
        sp.push(await run('R2 guest clock -0.5%', { link: HEADLINE, guest: { drift: -0.005 }, durationFrames: MATRIX_FRAMES }));
        const hl = [];
        for (let s = 1; s <= SEEDS; s++)
            hl.push(await run(`R2 HEADLINE 120ms j30 loss2% seed ${s}`, { link: HEADLINE, durationFrames: HEADLINE_FRAMES, seed: s, progress: log }));
        for (const file of RECORDINGS) {
            const rec = loadRecording(file);
            hl.push(await run(`R2 recording ${path.basename(file)} (menus ${rec.menus}) @120ms`, { link: HEADLINE, scenario: rec, startFrame: Math.min(BOOT_END, rec.end - 100), durationFrames: Math.max(100, rec.end - BOOT_END - 240) }));
        }
        const all = [...m, ...sp, ...hl];
        const bad = all.filter((r) => r.compare.mismatched || r.compare.compared < r.compare.expected * 0.9);
        report.gates.R2 = { verdict: bad.length ? 'FAIL' : 'PASS', matrix: m.map(brief), special: sp.map(brief), headline: hl.map(brief),
            text: `${all.length} runs; ${all.filter((r) => !r.compare.mismatched).length} with every final frame identical; ` +
                  `frames compared ${all.reduce((n, r) => n + r.compare.compared, 0)}, mismatched ${all.reduce((n, r) => n + r.compare.mismatched, 0)}; ` +
                  `headline: ${hl.map((r) => `${r.frames.gameMinutes} min, ${r.compare.equal}/${r.compare.compared}`).join('; ')}` +
                  (bad.length ? `; FAILING: ${bad.map((r) => r.name).join(', ')}` : '') };
        report.feel = all.map(feelRow);
    }
    // R3: no silent drops -- force the history to be exceeded (no lead cap,
    // a 10 s outage) and show each one repaired
    if (want('R3')) {
        const a = await run('R3 history exceeded (no lead cap, 10s outage)', { link: Object.assign({}, HEADLINE, { outages: [[FREE, 10000]] }), durationFrames: MATRIX_FRAMES, guestOpts: { leadCap: 100000, maxExtrap: 100000 },
            // the engine's own path: the host keeps the guest through the outage (with the
            // page's 6 s liveness it would drop it and re-admit it with a join image -- R13)
            deadMs: Infinity });
        const all = runs.filter((r) => !r.name.includes('/localise'));
        const exceeded = all.reduce((n, r) => n + r.guest.tel.historyExceeded, 0);
        const unrepaired = all.filter((r) => r.guest.tel.historyExceeded && r.guest.tel.imagesLoaded < 2);
        const residual = a.mismatches.filter((m) => m.f >= a.guest.tel.floor).length;
        const ok = a.guest.tel.historyExceeded > 0 && a.guest.tel.imagesLoaded >= 2 && residual === 0 && !unrepaired.length;
        report.gates.R3 = { verdict: ok ? 'PASS' : 'FAIL', runs: [brief(a)],
            text: `all runs: ${exceeded} sealed frames older than history, in ${all.filter((r) => r.guest.tel.historyExceeded).length} runs; forced case: ${a.guest.tel.historyExceeded} exceeded -> ${a.guest.tel.imagesLoaded - 1} repairs by image, then ${a.compare.equal}/${a.compare.compared} final frames identical (${a.compare.mismatched} known-wrong frames before the repair, residual after it ${residual}); runs with an exceed and no repair: ${unrepaired.length}` };
    }
    // R4: injected corruption
    if (want('R4')) {
        const at = BOOT_END + SCRIPTED + 1200;
        const mk = (what, apply) => ({ what, frame: at, apply });
        const cases = [
            mk('RNG word (gRngVal ^= 0x5A5A)', (g) => g.poke32(0x030068D8, g.peek32(0x030068D8) ^ 0x5A5A)),
            mk('enemy x (+32 px)', (g) => { const p = g.M._malloc(16); const ok = g.M._PortLnFindEnemy(p); const a = g.M.HEAP32[p >> 2], ty = g.M.HEAP32[(p >> 2) + 1]; g.M._free(p); if (ok) g.poke32(a + 0x40, (g.peek32(a + 0x40) + 0x2000) >>> 0); g.lastCorrupt = ok ? `object 0x${(a >>> 0).toString(16)} type 0x${ty.toString(16)}` : 'no enemy in room'; }),
            mk('VRAM byte (BG char 0x0600C010 ^= 0xFF)', (g) => g.poke8(0x0600C010, g.peek8(0x0600C010) ^ 0xFF)),
            mk('sim-block byte (spare 0x0204001F)', (g) => g.poke8(0x0204001F, g.peek8(0x0204001F) ^ 0x80)),
        ];
        cases[1].frame = 3000;   // in Kracko's arena
        const res = [];
        for (const cz of cases) {
            const r = await run(`R4 ${cz.what}`, { link: HEADLINE, durationFrames: SCRIPTED + 1200 + (QUICK ? 1200 : 2400), corrupt: [cz], noLocalise: true });
            const c = r.corrupt[0] || {};
            const after = r.mismatches.filter((m) => c.repairT !== undefined && m.t > c.repairT + 1000);
            res.push({ what: cz.what, target: c.target, detected: c.detectT !== undefined, detectMs: c.detectT !== undefined ? +(c.detectT - c.t).toFixed(0) : null,
                       repairMs: c.repairT !== undefined ? +(c.repairT - c.t).toFixed(0) : null, mismatchedFrames: r.compare.mismatched,
                       residual: after.length, behindImages: r.host.session.behindImages || 0, regions: [...new Set(r.mismatches.flatMap((m) => m.regions))],
                       imageRaw: r.host.session.imageRaw, imageComp: r.host.session.imageComp,
                       applyMs: +(r.guest.session.imageApplyMs / Math.max(1, r.guest.session.images)).toFixed(2),
                       compressMs: +(r.host.session.compressMs / Math.max(1, r.host.session.images)).toFixed(2),
                       imageLoadUs: r.guest.tel.imageLoadUs, repairs: r.guest.tel.imagesLoaded - 1, run: r.name });
        }
        const ok = res.every((x) => x.residual === 0 && (x.detected ? x.repairMs !== null : x.mismatchedFrames === 0));
        report.gates.R4 = { verdict: ok ? 'PASS' : 'FAIL', cases: res,
            text: res.map((x) => `${x.what}: ${x.detected ? `detected in ${x.detectMs} ms, repaired at ${x.repairMs} ms, residual ${x.residual}` : `never diverged (${x.mismatchedFrames} mismatched frames)`}`).join('; ') };
    }
    // R5: start/join by image, and what has to travel besides memory
    if (want('R5')) {
        const base = { link: HEADLINE };
        const mid = await run('R5 join mid-game (after the scripted part)', Object.assign({}, base, { joinFrame: BOOT_END + SCRIPTED + 400, durationFrames: SCRIPTED + 400 + (QUICK ? 1800 : 3600) }));
        const open = await run('R5 join while the pause SETTINGS page is open', Object.assign({}, base, { joinFrame: 5330, durationFrames: 5330 - BOOT_END + 2400 }));
        const abl = [];
        const skips = [
            ['local-multiplayer switch', 0x01, BOOT_END + SCRIPTED + 400],
            ['new-menus switch', 0x02, 4600],
            ['save memory', 0x04, BOOT_END + SCRIPTED + 400],
            ['"save already loaded" flag', 0x08, BOOT_END + SCRIPTED + 400],
            ['session view list', 0x10, 8200],
            ['menu_poc host state', 0x20, 5330],
        ];
        for (const [what, mask, join] of skips) {
            const r = await run(`R5 ablation: without ${what}`, Object.assign({}, base, { joinFrame: join, durationFrames: join - BOOT_END + (QUICK ? 1500 : 2400), noLocalise: true,
                guestPre: (g) => g.M._PortLnSetImageSkip(mask) }));
            abl.push({ what, joinFrame: join, diverged: r.compare.mismatched > 0, firstMismatch: r.compare.firstMismatch,
                       regions: [...new Set(r.mismatches.flatMap((m) => m.regions))], repairs: r.guest.tel.imagesLoaded - 1 });
        }
        const ok = mid.compare.mismatched === 0 && open.compare.mismatched === 0;
        report.gates.R5 = { verdict: ok ? 'PASS' : 'FAIL', runs: [brief(mid), brief(open)], ablations: abl,
            text: `mid-game join at f${mid.frames.join}: ${mid.compare.equal}/${mid.compare.compared} identical; join with the SETTINGS page open: ${open.compare.equal}/${open.compare.compared}; image ${mid.host.session.imageRaw} B raw, ${mid.host.session.imageComp} B deflated` };
    }
    // R12: host and guest with DIFFERENT audio devices (and one with none):
    // the session's sound state must not depend on either (platform/audio.c,
    // the canonical device).  Every gate's condition still holds: every
    // final frame identical, no repair image needed.  Plus what mixing costs.
    if (want('R12')) {
        const desk = { rate: 48000, quantum: 128, live: true };
        const phone = { rate: 44100, quantum: 1024, jitter: 0.5, seed: 7, live: true, drift: 0.0003 };
        const cases = [
            ['R12 host desktop 48 kHz, guest phone 44.1 kHz (1024, jitter, +300 ppm)', { hostAudio: desk, guestAudio: phone }],
            ['R12 host phone 44.1 kHz, guest no audio device', { hostAudio: phone, guestAudio: null }],
            ['R12 host no audio device, guest desktop 48 kHz at 30 Hz', { hostAudio: null, guestAudio: desk, guest: { rate: 30 } }],
            ['R12 host desktop, guest phone, forced rewind 150f every 37f', { hostAudio: desk, guestAudio: phone, link: L(0, 0, 0), forceRewind: { every: 37, depth: 150 } }],
            ['R12 host desktop, guest phone, 3 s outage', { hostAudio: desk, guestAudio: phone, link: Object.assign({}, HEADLINE, { outages: [[FREE, 3000]] }) }],
            ['R12 recording localmp_menus (pause SETTINGS by both players), host desktop, guest phone', { hostAudio: desk, guestAudio: phone, rec: true }],
        ];
        const rs = [];
        for (const [name, cf] of cases) {
            const cfg = Object.assign({ link: HEADLINE, durationFrames: MATRIX_FRAMES }, cf);
            if (cf.rec) {
                const file = RECORDINGS.find((f) => /menus/.test(f));
                if (!file) continue;
                const rec = loadRecording(file);
                Object.assign(cfg, { scenario: rec, startFrame: Math.min(BOOT_END, rec.end - 100), durationFrames: Math.max(100, rec.end - BOOT_END - 240) });
            }
            rs.push(await run(name, cfg));
        }
        // the price of mixing a replayed frame: the same forced-rewind run
        // with replayed frames rendered state-only (shipped) and mixed
        const priced = [];
        for (const mix of [0, 1]) {
            const r = await run(`R12 cost: replayed frames ${mix ? 'mixed (not shipped)' : 'state-only (shipped)'}`,
                { link: L(0, 0, 0), hostAudio: desk, guestAudio: desk, forceRewind: { every: 37, depth: 150 }, durationFrames: QUICK ? 3000 : MATRIX_FRAMES,
                  guestPre: (g) => g.M._PortAudioReplayMixJs(mix), noLocalise: true });
            const a = r.audio.guest.mixer;
            priced.push({ mix, replayUsPerFrame: +(r.guest.tel.catchUpUsSum / Math.max(1, r.guest.tel.replayedFrames)).toFixed(1),
                          mixedRenders: a[0], stateRenders: a[1], usPerMixed: +(a[2] / Math.max(1, a[0])).toFixed(1),
                          usPerState: +(a[3] / Math.max(1, a[1])).toFixed(2), activeChannels: +(a[4] / Math.max(1, a[0] + a[1])).toFixed(1),
                          usResamplePerBlock: +(r.audio.guest.out[6] / Math.max(1, r.audio.guest.out[0])).toFixed(1),
                          identical: r.compare.mismatched === 0 });
        }
        const bad = rs.filter((r) => r.compare.mismatched || r.compare.compared < r.compare.expected * 0.9 || r.guest.tel.imagesLoaded > 1);
        const au = (r, side) => { const x = r.audio[side]; if (!x || !x.device) return `${side} none`;
            const o = x.out, d = x.device;
            return `${side} ${d.rate} Hz: ${o[0]} blocks out, ${o[2]} dropped, ${o[4]} muted, device underruns ${d.underruns} (${(1000 * d.dryFrames / d.rate).toFixed(0)} ms dry; after the first 10 s ${d.underrunsAfter10s}, ${(1000 * d.dryFramesAfter10s / d.rate).toFixed(0)} ms)`; };
        report.gates.R12 = { verdict: bad.length || priced.some((p) => !p.identical) ? 'FAIL' : 'PASS', runs: rs.map(brief), audio: rs.map((r) => ({ name: r.name, host: r.audio.host, guest: r.audio.guest })), priced,
            text: rs.map((r) => `${r.name}: ${r.compare.equal}/${r.compare.compared} identical, images ${r.guest.tel.imagesLoaded}; ${au(r, 'host')}; ${au(r, 'guest')}`).join('; ') +
                  `; mixing cost (guest, wasm, ${priced[0].activeChannels} active channels avg): mixed render ${priced[1].usPerMixed} us, state-only render ${priced[0].usPerState} us, resampling ${priced[0].usResamplePerBlock} us per live block; replay ${priced[0].replayUsPerFrame} us/frame state-only vs ${priced[1].replayUsPerFrame} us/frame mixed` +
                  (bad.length ? `; FAILING: ${bad.map((r) => r.name).join(', ')}` : '') };
    }
    // R13: slots -- three and four devices, a device with two local players,
    // churn (joins, leaves, link drops and returns, slot reuse), a full game,
    // and what the slot table must carry in an image
    if (want('R13')) {
        const B = BOOT_END, dur = QUICK ? SCRIPTED + 2400 : SCRIPTED + 6000;
        const rs = [];
        const three = await run('R13 3 devices @120ms', { link: HEADLINE, durationFrames: dur, guests: [{ dev: 1 }, { dev: 2 }] });
        const threeP = await run('R13 3 devices, perfect link', { link: L(0, 0, 0), durationFrames: QUICK ? 3000 : dur, guests: [{ dev: 1 }, { dev: 2 }] });
        const four = await run('R13 4 devices @120ms', { link: HEADLINE, durationFrames: dur, guests: [{ dev: 1 }, { dev: 2 }, { dev: 3 }] });
        const twoLocal = await run('R13 3 devices, guest 1 with two local players (4 Kirbys) @120ms', { link: HEADLINE, durationFrames: dur,
            guests: [{ dev: 1, seats: [[B + 300, 1, 1]] }, { dev: 2 }] });
        const fourR = await run('R13 4 devices, forced rewind 150f every 37f', { link: L(0, 0, 0), forceRewind: { every: 37, depth: 150 }, durationFrames: QUICK ? 3000 : dur,
            guests: [{ dev: 1 }, { dev: 2 }, { dev: 3 }] });
        // churn: guest 2 joins late and its link drops for 8 s (gone, then back);
        // guest 3 joins when the game is full and waits; the host's second
        // player comes and goes; guest 1 leaves and a fresh instance of it
        // comes back with the same device id (it asks for its old slot);
        // guest 3's second player is refused while four play
        const churn = await run('R13 churn: joins, leaves, a link drop and return, a full game, slot reuse @60ms', { link: L(60, 15, 0.02), durationFrames: QUICK ? 9000 : 14000,
            guests: [
                { dev: 1, leave: B + 2600, rejoin: B + 3400 },
                { dev: 2, join: B + 800, link: L(60, 15, 0.02, { outages: [[70000, 8000]] }) },
                { dev: 3, join: B + 1500, seats: [[B + 7000, 1, 1]] },
            ],
            hostSeats: [[B + 1000, 1, 1], [B + 6000, 1, 0]] });
        // the slot table must travel in an image: the host's own second player
        // has slot 1, so a guest joining gets slot 2 -- with the table left out
        // of the image it would take slot 1 for its own
        const ownMid = await run('R13 join after the host\'s second player took slot 1 @120ms', { link: HEADLINE, durationFrames: QUICK ? 3000 : 5000,
            joinFrame: B + 400, guests: [{ dev: 1 }], hostSeats: [[B + 1, 1, 1]] });
        const ownAbl = await run('R13 ablation: the same without the slot table in the image', { link: HEADLINE, durationFrames: QUICK ? 3000 : 5000, noLocalise: true,
            joinFrame: B + 400, guests: [{ dev: 1 }], hostSeats: [[B + 1, 1, 1]], guestPre: (g) => g.M._PortLnSetImageSkip(0x80) });
        // slot reuse: guest 2 then guest 1 leave; guest 2 comes back first and
        // gets its own slot 2, not the lower free slot 1; then guest 1 gets 1
        const reuse = await run('R13 slot reuse: two guests leave, each comes back to its own Kirby @60ms', { link: L(60, 15, 0.02), durationFrames: QUICK ? 4000 : 6000,
            guests: [{ dev: 1, leave: B + 1500, rejoin: B + 2600 }, { dev: 2, leave: B + 1200, rejoin: B + 2000 }, { dev: 3 }] });
        const adm = reuse.churn.filter((x) => x.what === 'admit' && x.why === 'hi');
        const reuseOk = adm.length === 2 && adm[0].dev === 2 && adm[0].slot === 2 && adm[1].dev === 1 && adm[1].slot === 1;
        rs.push(three, threeP, four, twoLocal, fourR, churn, ownMid, reuse);
        const okRun = (r) => r.compare.mismatched === 0 && r.compare.compared >= r.compare.expected * 0.9 && r.host.tel.hostRewinds === 0;
        const ch = churn.churn;
        const churnOk = ch.some((x) => x.what === 'dead') && ch.some((x) => x.why === 'back') && ch.some((x) => x.what === 'bye')
            && ch.some((x) => x.what === 'admit' && x.slot === -1) && churn.host.tel.seatFull > 0 && churn.coverage.humansMax === 4;
        const owners = (r) => r.owners.map((o) => (o < 0 ? 'cpu' : `${o >> 2}.${o & 3}`)).join(' ');
        report.gates.R13 = {
            verdict: rs.every(okRun) && churnOk && reuseOk && four.coverage.humansMax === 4 && twoLocal.coverage.humansMax === 4 && ownAbl.compare.mismatched > 0 ? 'PASS' : 'FAIL',
            runs: rs.map(brief), churn: ch, ablation: brief(ownAbl),
            perGuest: rs.map((r) => ({ name: r.name, guests: r.guests.map((g) => ({ name: g.name, joinAt: g.joinAt, leave: g.leaveFrame, compare: g.compare, rewindsPerMin: g.feel.rewindsPerMin, images: g.tel ? undefined : undefined })) })),
            text: rs.map((r) => `${r.name}: ${r.compare.equal}/${r.compare.compared} identical over ${r.guests.length} guest instance(s), Kirbys at the end ${owners(r)}, most people at once ${r.coverage.humansMax}`).join('; ') +
                  `; churn events: ${ch.map((x) => `${x.what}${x.why ? '(' + x.why + ')' : ''} dev ${x.dev}${x.slot !== undefined ? ' slot ' + x.slot : ''}${x.seat !== undefined ? ' seat ' + x.seat : ''} @f${x.f}`).join(', ')}; refused while full ${churn.host.tel.seatFull}` +
                  `; slot reuse: ${adm.map((x) => `device ${x.dev} back -> slot ${x.slot}`).join(', ')} (${reuseOk ? 'each its own' : 'NOT its own'})` +
                  `; ablation without the slot table: ${ownAbl.compare.mismatched} mismatched frames (${ownAbl.guest.tel.imagesLoaded - 1} repairs)`,
        };
    }
    // R14: the link matrix on a three-device session (both guests on the
    // same conditions, each its own link)
    if (want('R14')) {
        const m = [];
        for (const x of matrix) {
            if (QUICK && x.link.reorder) continue;
            m.push(await run(`R14 3 devices ${x.id}`, { link: x.link, durationFrames: MATRIX_FRAMES, seed: 11, guests: [{ dev: 1 }, { dev: 2 }] }));
        }
        const bad = m.filter((r) => r.compare.mismatched || r.compare.compared < r.compare.expected * 0.9);
        report.gates.R14 = { verdict: bad.length ? 'FAIL' : 'PASS', matrix: m.map(brief),
            text: `${m.length} three-device runs; ${m.filter((r) => !r.compare.mismatched).length} with every final frame identical on both guests; frames compared ${m.reduce((n, r) => n + r.compare.compared, 0)}, mismatched ${m.reduce((n, r) => n + r.compare.mismatched, 0)}` +
                  (bad.length ? `; FAILING: ${bad.map((r) => r.name).join(', ')}` : '') };
    }
    // R15: what the third and fourth player cost: the same play, link and seed
    // with 2, 3 and 4 devices; feel per guest
    if (want('R15')) {
        const dur = QUICK ? SCRIPTED + 3600 : SCRIPTED + 12000;
        const rows = [];
        for (const n of [2, 3, 4]) for (const [lname, link] of [['@120ms j30 loss2%', HEADLINE], ['@30ms', L(30, 5, 0)]]) {
            const r = await run(`R15 ${n} devices ${lname}`, { link, durationFrames: dur, seed: 3, guests: Array.from({ length: n - 1 }, (_, i) => ({ dev: i + 1 })) });
            for (const g of r.guests) rows.push(Object.assign({ devices: n, link: lname, run: r.name, guest: g.name, identical: g.compare.mismatched === 0 }, feelOf(g),
                { upBps: Math.round((g.link ? g.link.gh.bytes : 0) / Math.max(1, g.minutes * 60)), downBps: Math.round((g.link ? g.link.hg.bytes : 0) / Math.max(1, g.minutes * 60)) }));
        }
        report.gates.R15 = { verdict: 'MEASURED', rows,
            text: ['@120ms j30 loss2%', '@30ms'].map((ln) => [2, 3, 4].map((n) => {
                const rr = rows.filter((x) => x.devices === n && x.link === ln);
                const avg = (k) => +(rr.reduce((a, x) => a + x[k], 0) / Math.max(1, rr.length)).toFixed(1);
                return `${n} devices ${ln}: rewinds/min ${avg('rewindsPerMin')} (per guest ${rr.map((x) => x.rewindsPerMin).join('/')}), lead p50 ${rr.map((x) => x.lead.p50).join('/')}, own px p99 ${rr.map((x) => fmt(x.own.p99)).join('/')}, others mispredicted ${avg('mispredOtherPct')}%`;
            }).join('; ')).join('; ') };
    }
    // R6: authority -- across every run
    if (want('R6')) {
        const all = runs.filter((r) => !r.name.includes('/localise'));
        const hr = all.reduce((n, r) => n + r.host.tel.hostRewinds + r.host.tel.rewinds, 0);
        const hc = all.reduce((n, r) => Math.max(n, r.host.tel.catchUpFramesMax), 0);
        report.gates.R6 = { verdict: hr === 0 && hc === 0 ? 'PASS' : 'FAIL', text: `${all.length} runs: host rewinds ${hr}, host replayed frames ${hc}` };
    }
    // R8: cost
    if (want('R8')) report.gates.R8 = Object.assign({ verdict: 'MEASURED' }, await costs());
    // R9 / R10: from every run
    if (want('R9')) report.gates.R9 = sideEffects();
    if (want('R10')) report.gates.R10 = events();
    if (want('R7')) report.gates.R7 = { verdict: 'MEASURED', text: 'per link condition below (feel table)' };
    if (want('R11') && !has('skip-r11')) report.gates.R11 = r11();
    report.localisations = localisations;
    report.runs = runs;
    report.finished = new Date().toISOString();
    report.wallSec = Math.round((Date.now() - t0) / 1000);
    fs.writeFileSync(path.join(OUT, 'rig-report.json'), JSON.stringify(report, null, 1));
    fs.writeFileSync(path.join(OUT, 'RIG-REPORT.md'), markdown(report));
    log(`wrote ${path.join(OUT, 'RIG-REPORT.md')} and rig-report.json (${report.wallSec}s)`);
    for (const [g, v] of Object.entries(report.gates)) log(`${g} ${v.verdict}: ${v.text || ''}`);
}

function brief(r) {
    return { name: r.name, frames: r.compare.expected, compared: r.compare.compared, equal: r.compare.equal, mismatched: r.compare.mismatched,
             firstMismatch: r.compare.firstMismatch, gameMin: r.frames.gameMinutes, rewinds: r.guest.tel.rewinds, images: r.guest.tel.imagesLoaded,
             historyExceeded: r.guest.tel.historyExceeded, engineMismatches: r.guest.tel.mismatches, wallSec: r.wallSec };
}

function feelOf(g) {
    const f = g.feel;
    return { gameMin: g.minutes, rewindsPerMin: f.rewindsPerMin, replayedPerSec: f.replayedFramesPerSec, depth: f.depth, own: f.ownDisp, other: f.otherDisp,
             ownOverriddenPct: f.ownOverriddenPct, mispredHostPct: f.mispredHostPct, mispredOtherPct: f.mispredOtherPct, hostKeyLatePct: f.hostKeyLatePct,
             lead: g.lead, margin: g.margin, holdsPerMin: f.holdsPerMin, skipsPerMin: f.skipsPerMin, worstGuestStepMs: f.worstGuestStepMs,
             images: g.tel ? g.tel.imagesLoaded : null };
}

function feelRow(r) {
    const f = r.feel, g = r.guest;
    return { name: r.name, gameMin: r.frames.gameMinutes, identical: r.compare.mismatched === 0, rewindsPerMin: f.rewindsPerMin, replayedPerSec: f.replayedFramesPerSec,
             depth: f.depth, own: f.ownDisp, other: f.otherDisp, ownNonzero: f.ownDispNonzero, otherNonzero: f.otherDispNonzero,
             ownOverriddenPct: f.ownOverriddenPct, hostKeyLatePct: f.hostKeyLatePct, lead: g.lead, margin: g.margin, rtt: g.rtt,
             holdsPerMin: f.holdsPerMin, skipsPerMin: f.skipsPerMin, burstsPerMin: f.burstsPerMin, capHolds: f.capHolds,
             worstGuestStepMs: f.worstGuestStepMs, guestStep: g.stepMs, images: g.tel.imagesLoaded, bigCorrections: r.bigCorrections, roomChange: f.roomChangeCorrections, soundsBad: f.soundsMispredicted, soundsJudged: f.soundsJudged,
             upBps: Math.round(r.link.gh.bytes / Math.max(1, r.frames.gameMinutes * 60)), downBps: Math.round(r.link.hg.bytes / Math.max(1, r.frames.gameMinutes * 60)) };
}

async function costs() {
    // simulation alone, local multiplayer, render off, no session; then the
    // session's own per-frame work on top (hash every frame / every 16th)
    const sc = buildScenario({ seed: 5, humanFrames: 20000 });
    const out = {};
    for (const mode of ['plain', 'session-hash16', 'session-hash1']) {
        const h = await createInstance({ modulePath: MODULE, rom, name: 'cost' });
        h.M._PortLmpSetHumans(2);
        while (h.frame() < BOOT_END) { const f = h.frame(); h.M._PortSetKeys(sc.p1(f)); await h.step(); }
        if (mode !== 'plain') h.M._PortLnStart(1, 15, 24, mode === 'session-hash1' ? 1 : 0);
        const N = QUICK ? 4000 : 12000, f0 = h.frame();
        let t = performance.now();
        for (let i = 0; i < N; i++) {
            const f = h.frame();
            if (mode === 'plain') { h.M._PortSetKeys(sc.p1(f)); h.M._PortLmpSetHostKeys(1, sc.p2(f)); }
            else { h.M._PortLnLocalInput(f, sc.p1(f)); h.M._PortLnRemoteInput(f, sc.p2(f)); }
            await h.step();
        }
        out[mode] = +((performance.now() - t) * 1000 / N).toFixed(1);
        if (mode === 'session-hash1') {
            const tel = h.tel.read();
            out.hashUsAvg = +(tel.hashUsSum / tel.hashed).toFixed(1);
            out.hashUsMax = tel.hashUsMax;
            out.imageBytes = h.M._PortLnImageBytes();
        }
    }
    // an empty step: the shim's own cost (await round trip), for honesty
    const pure = await createInstance({ modulePath: MODULE, rom, name: 'cost2' });
    const all = runs.filter((r) => !r.name.includes('/localise') && r.guest.tel.kfSaved);
    const kfSave = all.reduce((a, r) => [a[0] + r.guest.tel.kfSaveUsSum, a[1] + r.guest.tel.kfSaved, Math.max(a[2], r.guest.tel.kfSaveUsMax)], [0, 0, 0]);
    const kfLoad = all.reduce((a, r) => [a[0] + r.guest.tel.kfLoadUsSum, a[1] + r.guest.tel.rewinds, Math.max(a[2], r.guest.tel.kfLoadUsMax)], [0, 0, 0]);
    const cu = all.reduce((a, r) => [a[0] + r.guest.tel.catchUpUsSum, a[1] + r.guest.tel.replayedFrames], [0, 0]);
    const t = all[0] ? all[0].guest.tel : {};
    out.keyframeSaveUs = { avg: +(kfSave[0] / Math.max(1, kfSave[1])).toFixed(1), max: kfSave[2], n: kfSave[1] };
    out.keyframeRestoreUs = { avg: +(kfLoad[0] / Math.max(1, kfLoad[1])).toFixed(1), max: kfLoad[2], n: kfLoad[1] };
    out.replayUsPerFrame = +(cu[0] / Math.max(1, cu[1])).toFixed(1);
    out.keyframeBytes = t.kfBytes; out.keyframes = t.kfCount;
    out.historyBytes = (t.kfBytes || 0) * (t.kfCount || 0);
    out.timelineBytes = 8192 * (60 + 11 * 8 + 12);
    out.historyFramesCovered = 15 * 24;
    const img = all.map((r) => r.host.session).filter((s) => s.images);
    out.imageRaw = img[0] ? img[0].imageRaw : null;
    out.imageCompressed = dist(img.map((s) => s.imageComp));
    out.worstGuestStepMsByRun = all.map((r) => [r.name, r.feel.worstGuestStepMs]);
    out.text = `sim ${out.plain} us/frame (wasm, local MP, render off, incl. ~JS step overhead); +session w/ hash every 16th ${out['session-hash16']} us; hash every frame ${out['session-hash1']} us (hash ${out.hashUsAvg} us avg); ` +
               `keyframe save ${out.keyframeSaveUs.avg} us avg / ${out.keyframeSaveUs.max} max, restore ${out.keyframeRestoreUs.avg} / ${out.keyframeRestoreUs.max}, replay ${out.replayUsPerFrame} us/frame; ` +
               `history ${(out.historyBytes / 1048576).toFixed(1)} MiB (${out.keyframes} x ${out.keyframeBytes} B, ${out.historyFramesCovered} frames) + timeline ${(out.timelineBytes / 1048576).toFixed(1)} MiB; image ${out.imageRaw} B raw, ${out.imageCompressed.p50} B deflated (p50)`;
    void pure;
    return out;
}

function sideEffects() {
    const all = runs.filter((r) => !r.name.includes('/localise'));
    const sum = (k, side = 'guest') => all.reduce((n, r) => n + (r[side].tel[k] || 0), 0);
    const o = {
        soundsLiveGuest: sum('soundsLive'), soundsRefire: sum('soundsRefire'), soundsSpurious: sum('soundsSpurious'), soundsMissed: sum('soundsMissed'),
        sramWritesGuest: sum('sramWrites'), sramWritesUnsealed: sum('sramWritesUnsealed'), sramWritesReplay: sum('sramWritesReplay'), sramWritesHost: sum('sramWrites', 'host'),
        settingsEditsGuest: sum('settingsApplied'), settingsEditsUnsealed: sum('settingsUnsealed'), settingsEditsReplay: sum('settingsReplay'),
        settingsAppliedToDeviceGuest: sum('settingsDeviceApplied'), settingsAppliedToDeviceHost: sum('settingsDeviceApplied', 'host'),
        audioBlocksMuted: sum('audioBlocksMuted'), guestMinutes: +all.reduce((n, r) => n + r.frames.gameMinutes, 0).toFixed(1),
    };
    // what the guest's speakers heard (platform/audio_ear.c), runs with a guest audio device
    const heard = all.filter((r) => r.audio && r.audio.guest && r.audio.guest.ear && r.audio.guest.device);
    const es = (k) => heard.reduce((n, r) => n + (r.audio.guest.ear[k] || 0), 0);
    o.speakers = { runs: heard.length, guestMinutes: +heard.reduce((n, r) => n + r.frames.gameMinutes, 0).toFixed(1),
        finalStarts: es('finalStarts'), heardFromStart: es('heardFromStart'), neverHeardFromStart: es('neverHeard'), heardTwice: es('doubles'),
        heardNotFinal: es('spurious'), heardNotFinalFramesSum: es('spurDurSum'), lateFramesSum: es('lateSum'), lateMaxFrames: Math.max(0, ...heard.map((r) => r.audio.guest.ear.lateMax || 0)),
        lateN: es('lateN'), reconciles: es('reconciles') };
    const sp = o.speakers;
    o.verdict = 'MEASURED';
    o.text = (sp.runs ? `guest speakers (${sp.runs} runs with a guest audio device, ${sp.guestMinutes} guest-minutes, platform/audio_ear.c): final starts ${sp.finalStarts}, heard from their start ${sp.heardFromStart} ` +
              `(late ${(sp.lateFramesSum / Math.max(1, sp.lateN)).toFixed(1)} frames on average, at most ${sp.lateMaxFrames}), never heard from their start ${sp.neverHeardFromStart}, heard twice ${sp.heardTwice}; ` +
              `heard but not final ${sp.heardNotFinal} (${(sp.heardNotFinalFramesSum / Math.max(1, sp.heardNotFinal)).toFixed(1)} frames on average before the cut); ` : '') +
             `over ${o.guestMinutes} guest-minutes: sounds started live ${o.soundsLiveGuest}; in replayed frames, same sound again (would re-fire) ${o.soundsRefire}, heard but not in the final timeline (mispredicted) ${o.soundsSpurious}, in the final timeline but never heard live ${o.soundsMissed}; ` +
             `SRAM writes on the guest ${o.sramWritesGuest} (${o.sramWritesUnsealed} from unsealed live frames, ${o.sramWritesReplay} from replays; host ${o.sramWritesHost}); ` +
             `pause-page setting edits on the guest ${o.settingsEditsGuest} (${o.settingsEditsUnsealed} unsealed, ${o.settingsEditsReplay} replayed) -- now simulation state, applied to a device only when final: guest ${o.settingsAppliedToDeviceGuest}, host ${o.settingsAppliedToDeviceHost}`;
    return o;
}

function events() {
    const all = runs.filter((r) => !r.name.includes('/localise'));
    const sum = (k, side = 'guest') => all.reduce((n, r) => n + (r[side].tel[k] || 0), 0);
    const late = sum('eventsLate'), sealed = sum('events', 'host'), applied = sum('eventsApplied');
    const withLate = all.filter((r) => r.guest.tel.eventsLate && !/^R3 |^R4 |ablation/.test(r.name));
    const ok = withLate.every((r) => r.compare.mismatched === 0) && sealed > 0;
    return { verdict: ok ? 'PASS' : 'FAIL', sealed, late, applied, rewindsForEvents: sum('rewindsEvent'),
             text: `${sealed} events sealed by hosts (warps, view changes, pokes), ${late} reached a guest after it had simulated that frame (${sum('rewindsEvent')} rewinds caused by an event alone); every run with late events converged: ${ok}` };
}

function r11() {
    const res = { verdict: 'FAIL', parts: {} };
    const env = Object.assign({}, process.env, { ROM });
    const sh = (cmd, args, opts = {}) => {
        const r = spawnSync(cmd, args, Object.assign({ cwd: ROOT, env, encoding: 'utf8', maxBuffer: 1 << 26 }, opts));
        return { rc: r.status, out: (r.stdout || '') + (r.stderr || '') };
    };
    log('R11: native build + make regress / regress-wasm / check_sim_state / lint-determinism');
    const b = sh('cmake', ['--build', 'build/lp64', '-j', String(os.cpus().length)]);
    res.parts.nativeBuild = b.rc === 0 ? 'ok' : 'FAILED';
    const reg = sh('make', ['regress', `ROM=${ROM}`]);
    const lines = reg.out.split('\n').filter((l) => /^(aim|attract|boot_to_level|play_rooms|tutorial_walk|warp_sweep|wide_view)\s/.test(l));
    res.parts.regress = lines;
    const regOk = lines.length === 7 && lines.every((l) => /\sOK\s/.test(l) || /^aim\s+DIVERGED\s+first divergent frame: sim@3318$/.test(l.trim()));
    const wasm = sh('make', ['regress-wasm', `ROM=${ROM}`]);
    const wl = wasm.out.split('\n').filter((l) => /^(aim|attract|boot_to_level|play_rooms|tutorial_walk|warp_sweep|wide_view)\s/.test(l));
    res.parts.regressWasm = wl;
    const wasmOk = wl.length === 7 && wl.every((l) => /\sOK\s/.test(l));
    const ss = sh('python3', ['tools/check_sim_state.py']);
    res.parts.checkSimState = ss.out.trim().split('\n').slice(-3);
    const ld = sh('make', ['lint-determinism']);
    res.parts.lintDeterminism = ld.out.trim().split('\n').slice(-3);
    res.verdict = regOk && wasmOk && ss.rc === 0 && ld.rc === 0 && b.rc === 0 ? 'PASS' : 'FAIL';
    res.text = `native build ${res.parts.nativeBuild}; regress: ${regOk ? 'as baseline (aim sim@3318 only, keys/vid/sram/pix match)' : 'DIFFERS'}; regress-wasm: ${wasmOk ? '7/7 OK' : 'DIFFERS'}; check_sim_state rc ${ss.rc}; lint-determinism rc ${ld.rc}`;
    return res;
}

// --- markdown ---------------------------------------------------------------
function md(rows, cols) {
    const head = '| ' + cols.map((c) => c[0]).join(' | ') + ' |\n|' + cols.map(() => '---').join('|') + '|\n';
    return head + rows.map((r) => '| ' + cols.map((c) => { const v = c[1](r); return v === undefined || v === null ? '' : String(v); }).join(' | ') + ' |').join('\n') + '\n';
}
const d3 = (d) => (d && d.n ? `${fmt(d.p50)}/${fmt(d.p90)}/${fmt(d.p99)}/${fmt(d.max)}` : '-');
const fmt = (v) => (v === null || v === undefined ? '-' : Number.isInteger(v) ? v : (+v).toFixed(1));

function markdown(R) {
    let s = `# Online local multiplayer: proof-of-concept rig report\n\n`;
    s += `Generated by \`node tools/netproof.mjs\` (${R.quick ? 'QUICK mode' : 'full'}) on ${R.finished}, ${R.wallSec} s wall. ROM: the player's own (${R.rom}); module ${path.relative(ROOT, R.module)}.\n`;
    s += `Every number below is measured by this run on a virtual clock (seeded, reproducible); nothing is cited.\n\n## Gates\n\n`;
    s += md(Object.entries(R.gates), [['gate', ([g]) => g], ['verdict', ([, v]) => `**${v.verdict}**`], ['numbers', ([, v]) => (v.text || '').replace(/\|/g, '/')]]);
    if (R.gates.R2) {
        s += `\n## R2 runs\n\n`;
        s += md([...R.gates.R2.matrix, ...R.gates.R2.special, ...R.gates.R2.headline], [['run', (r) => r.name], ['game min', (r) => r.gameMin], ['final frames identical', (r) => `${r.equal}/${r.compared} of ${r.frames}`],
            ['rewinds', (r) => r.rewinds], ['images (join+repairs)', (r) => r.images], ['history exceeded', (r) => r.historyExceeded], ['engine mismatches', (r) => r.engineMismatches], ['wall s', (r) => r.wallSec]]);
    }
    if (R.feel) {
        s += `\n## R7 feel, per link condition\n\nDisplacement is the jump of a Kirby on the guest's screen at a correction (rewind or image), in pixels, same room only: p50/p90/p99/max over corrections. "Own" is the guest's Kirby (player 2), "other" is player 1. Lead is guest frame minus its sealed frontier.\n\n`;
        s += md(R.feel, [['run', (r) => r.name], ['rewinds/min', (r) => r.rewindsPerMin], ['replayed f/s', (r) => r.replayedPerSec], ['depth p50/90/99/max', (r) => d3(r.depth)],
            ['own px', (r) => d3(r.own)], ['other px', (r) => d3(r.other)], ['own overridden %', (r) => r.ownOverriddenPct], ['guest key late at host %', (r) => r.hostKeyLatePct],
            ['lead p50/90/99/max', (r) => d3(r.lead)], ['holds/min', (r) => r.holdsPerMin], ['skips/min', (r) => r.skipsPerMin], ['bursts/min', (r) => r.burstsPerMin], ['worst guest step ms', (r) => r.worstGuestStepMs],
            ['corrections that changed a room', (r) => r.roomChange], ['sounds mispredicted / judged', (r) => `${r.soundsBad}/${r.soundsJudged}`], ['up/down B/s', (r) => `${r.upBps}/${r.downBps}`]]);
    }
    if (R.gates.R4) s += `\n## R4 corruption\n\n` + md(R.gates.R4.cases, [['injected', (x) => x.what], ['detected', (x) => x.detected], ['detect ms', (x) => x.detectMs], ['repaired ms', (x) => x.repairMs], ['mismatched frames', (x) => x.mismatchedFrames],
        ['residual', (x) => x.residual], ['regions', (x) => x.regions.join(' ')], ['image raw/deflated B', (x) => `${x.imageRaw}/${x.imageComp}`], ['deflate ms', (x) => x.compressMs], ['inflate+load ms', (x) => x.applyMs], ['engine load us', (x) => x.imageLoadUs]]);
    if (R.gates.R5) s += `\n## R5 what has to travel besides memory (ablations)\n\nEach row is a mid-game join with one item left out of the image load.\n\n` + md(R.gates.R5.ablations, [['left out', (x) => x.what], ['join frame', (x) => x.joinFrame], ['diverged', (x) => x.diverged], ['first mismatch', (x) => x.firstMismatch], ['regions', (x) => x.regions.join(' ')], ['repairs', (x) => x.repairs]]);
    if (R.gates.R12) {
        s += `\n## R12 different audio devices\n\nHost and guest each with their own audio device model (instance.mjs audioDevice: rate, callback size, jitter, clock drift; or none). Blocks out/dropped/muted: platform/audio.c's output side (dropped = held back and cross-faded because far too much was queued; muted = replayed frames). Device underruns: callbacks the model found the queue short for.\n\n`;
        s += md(R.gates.R12.audio, [['run', (x) => x.name], ['host device', (x) => x.host && x.host.device ? `${x.host.device.rate} Hz, ${x.host.out[0]} out, ${x.host.out[2]} dropped, ${x.host.device.underruns} underruns` : 'none'],
            ['guest device', (x) => x.guest && x.guest.device ? `${x.guest.device.rate} Hz, ${x.guest.out[0]} out, ${x.guest.out[2]} dropped, ${x.guest.out[4]} muted, ${x.guest.device.underruns} underruns (${(1000 * x.guest.device.dryFrames / x.guest.device.rate).toFixed(0)} ms dry; after 10 s: ${x.guest.device.underrunsAfter10s}, ${(1000 * x.guest.device.dryFramesAfter10s / x.guest.device.rate).toFixed(0)} ms)` : 'none'],
            ['guest renders mixed / state-only', (x) => x.guest ? `${x.guest.mixer[0]} / ${x.guest.mixer[1]}` : '']]);
        s += `\nMixing cost, measured on the guest (wasm, node): ` + R.gates.R12.priced.map((p) => `${p.mix ? 'replayed frames mixed (not shipped)' : 'replayed frames state-only (shipped)'}: replay ${p.replayUsPerFrame} us/frame, mixed render ${p.usPerMixed} us, state-only render ${p.usPerState} us, resampling ${p.usResamplePerBlock} us per live block, ${p.activeChannels} active channels avg, identical ${p.identical}`).join('; ') + '\n';
    }
    if (R.gates.R13) {
        s += `\n## R13 slots: runs\n\n` + md(R.gates.R13.runs.concat([R.gates.R13.ablation]), [['run', (r) => r.name], ['game min', (r) => r.gameMin], ['final frames identical (all guests)', (r) => `${r.equal}/${r.compared} of ${r.frames}`],
            ['rewinds (guest 1)', (r) => r.rewinds], ['images (guest 1)', (r) => r.images], ['engine mismatches (guest 1)', (r) => r.engineMismatches], ['wall s', (r) => r.wallSec]]);
        s += `\nChurn run, in order: ${R.gates.R13.churn.map((x) => `f${x.f} ${x.what}${x.why ? ' (' + x.why + ')' : ''} device ${x.dev}${x.slot !== undefined ? ' -> slot ' + x.slot : ''}${x.seat !== undefined ? ' seat ' + x.seat + (x.accepted !== undefined ? (x.accepted ? ' accepted' : ' refused') : '') : ''}`).join('; ')}\n`;
        s += `\nPer guest instance:\n\n` + md(R.gates.R13.perGuest.flatMap((r) => r.guests.map((g) => Object.assign({ run: r.name }, g))), [['run', (x) => x.run], ['guest', (x) => x.name], ['joined at', (x) => x.joinAt], ['left at', (x) => x.leave],
            ['identical', (x) => `${x.compare.equal}/${x.compare.compared} of ${x.compare.expected}`], ['rewinds/min', (x) => x.rewindsPerMin]]);
    }
    if (R.gates.R14) s += `\n## R14 link matrix, three devices\n\n` + md(R.gates.R14.matrix, [['run', (r) => r.name], ['final frames identical (both guests)', (r) => `${r.equal}/${r.compared} of ${r.frames}`], ['rewinds (guest 1)', (r) => r.rewinds], ['images (guest 1)', (r) => r.images], ['wall s', (r) => r.wallSec]]);
    if (R.gates.R15) {
        s += `\n## R15 the cost of more players (feel per guest, same play and link)\n\nOwn = this guest's first Kirby, other = player 1 (the host's). Mispredicted: frames whose sealed keys differed from the guest's guess, for player 1 / for the other guests' Kirbys.\n\n`;
        s += md(R.gates.R15.rows, [['devices', (x) => x.devices], ['link', (x) => x.link], ['guest', (x) => x.guest], ['identical', (x) => x.identical], ['rewinds/min', (x) => x.rewindsPerMin], ['replayed f/s', (x) => x.replayedPerSec],
            ['depth p50/90/99/max', (x) => d3(x.depth)], ['own px', (x) => d3(x.own)], ['other px', (x) => d3(x.other)], ['P1 mispredicted %', (x) => x.mispredHostPct], ['others mispredicted %', (x) => x.mispredOtherPct],
            ['own overridden %', (x) => x.ownOverriddenPct], ['key late at host %', (x) => x.hostKeyLatePct], ['lead p50/90/99/max', (x) => d3(x.lead)], ['worst step ms', (x) => x.worstGuestStepMs], ['up/down B/s', (x) => `${x.upBps}/${x.downBps}`]]);
    }
    if (R.gates.R8) s += `\n## R8 cost\n\n${R.gates.R8.text}\n\nWorst guest step (one presented frame including any replay) per run, ms: ${R.gates.R8.worstGuestStepMsByRun.map(([n, v]) => `${n}: ${v}`).join('; ')}\n`;
    if (R.gates.R9) s += `\n## R9 side effects under rewind\n\n${R.gates.R9.text}\n`;
    if (R.gates.R11) s += `\n## R11 feature off\n\n${R.gates.R11.text}\n\n\`\`\`\n${[...(R.gates.R11.parts.regress || []), '--- wasm', ...(R.gates.R11.parts.regressWasm || []), '---', ...(R.gates.R11.parts.checkSimState || []), ...(R.gates.R11.parts.lintDeterminism || [])].join('\n')}\n\`\`\`\n`;
    s += `\n## Mismatches localised in this run\n\n`;
    if (!R.localisations.length) s += 'None: no run had a final frame that differed (other than the R4 injections and R5 ablations, which are expected and listed above).\n';
    for (const x of R.localisations) {
        s += `- **${x.run}**, first differing frame ${x.frame} (${x.reproduced ? 'reproduced on rerun' : 'NOT reproduced on rerun'}); bytes by region ${JSON.stringify(x.bytesByRegion || {})}\n`;
        for (const r of (x.runs || []).slice(0, 8)) s += `  - ${r.region} ${r.addr} len ${r.len} ${r.symbol}: host ${r.host} guest ${r.guest}\n`;
    }
    s += `\n## Causes found and fixed while building the rig\n\n${CAUSES}\n`;
    s += `\n## Real-build requirements and limits\n${NOTES}\n`;
    s += `\n## Coverage (host side, headline seed 1)\n\n${R.gates.R2 && R.gates.R2.headline[0] ? '```\n' + JSON.stringify((R.runs.find((r) => r.name === R.gates.R2.headline[0].name) || {}).coverage, null, 0) + '\n```' : '-'}\n`;
    return s;
}


main().catch((e) => { console.error(e); process.exit(1); });
