#!/usr/bin/env node
// What the guest's speakers hear in an online session, against the host's:
// the reconciler (platform/audio_ear.c) off and on, per link condition.
//
//   node tools/netproof/sound_eval.mjs --out DIR [--rom ROM] [--module M]
//        [--before OLD_MODULE] [--frames N] [--only 30,60,120,forced,jitter]
//
// For each run: host and guest each with a desktop audio device model
// (48 kHz, 128-frame callbacks), the same scripted two-player session over a
// simulated link (tools/netproof/run.mjs), and
//   - the ear's counters (final starts heard from their beginning, how late,
//     never heard from the beginning, heard twice, heard-but-not-final and
//     for how long),
//   - captures of what each device was handed, as WAV (DIR/*.wav; ROM-derived,
//     keep them out of the repository) with each pushed block's render tag,
//   - per render and per music player, the energy each device's speakers got
//     (the ear's probe; on the host a pass-through), and every sound start
//     the host's final timeline has,
// and from those, per start: where (how late) and how well the guest's
// player stream matches the host's over the sound's first frames, and per
// block the spectral distance between the two mixed captures.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { runSession } from './run.mjs';
import { buildScenario, BOOT_END, SCRIPTED_END } from './scenario.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(here, '../..');
const argv = process.argv.slice(2);
const flag = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 ? argv[i + 1] : d; };
const ROM = flag('rom', process.env.ROM || '/bigdata/agent-tom/projects/katamport/decomp/baserom.gba');
const MODULE = path.resolve(flag('module', path.join(ROOT, 'build/katam-node.js')));
const BEFORE = flag('before', null);
const OUT = path.resolve(flag('out', path.join(ROOT, '../netproof-eval/sound-tmp')));
const FRAMES = +flag('frames', (SCRIPTED_END - BOOT_END) + 3600);
const ONLY = flag('only', '30,60,120,jitter,forced').split(',');
const WAVS = flag('wavs', '60,120').split(',');
const MODES = flag('modes', '0,1').split(',').map(Number);
const rom = fs.readFileSync(ROM);
fs.mkdirSync(OUT, { recursive: true });
const t0 = Date.now();
const log = (...a) => console.log(`[sound ${((Date.now() - t0) / 1000).toFixed(0)}s]`, ...a);

const DESK = { rate: 48000, quantum: 128, live: true };
const DMAXLAG = 56;

function cstr(M, p) { let s = ''; for (let i = p; M.HEAPU8[i]; i++) s += String.fromCharCode(M.HEAPU8[i]); return s; }

// One side's recorder: pushes (with tags), probe records, start log.
function recorder(name) {
    const r = { name, inst: null, chunks: [], tags: [], samples: 0, probe: new Map(), probeSeen: 0 };
    r.spec = Object.assign({}, DESK, {
        onPush(f32) {
            const M = r.inst && r.inst.M;
            const tag = M && M._PortAudioPushTagJs ? M._PortAudioPushTagJs() >>> 0 : 0xFFFFFFFF;
            r.chunks.push(f32.slice());
            r.tags.push([tag, r.samples, f32.length >> 1]);
            r.samples += f32.length >> 1;
            r.drain();
        },
    });
    r.drain = () => {
        const M = r.inst && r.inst.M;
        if (!M || !M._PortAudioEarProbeCountJs) return;
        const n = M._PortAudioEarProbeCountJs() >>> 0, cap = M._PortAudioEarProbeCapJs(), base = M._PortAudioEarProbeRingJs();
        for (let k = Math.max(r.probeSeen, n - cap); k < n; k++) {
            const off = base + (k % cap) * 24;
            const tag = M.HEAPU32[off >> 2];
            const e = Array.from(M.HEAPF32.subarray((off >> 2) + 1, (off >> 2) + 6));
            r.probe.set(tag, e);
        }
        r.probeSeen = n;
    };
    r.pre = (mode) => (inst) => {
        r.inst = inst;
        if (inst.M._PortAudioEarProbeJs) inst.M._PortAudioEarProbeJs(1);
        if (inst.M._PortAudioEarJs) inst.M._PortAudioEarJs(mode);
    };
    return r;
}

function writeWav(file, chunks, samples, rate) {
    const b = Buffer.alloc(44 + samples * 4);
    b.write('RIFF', 0); b.writeUInt32LE(36 + samples * 4, 4); b.write('WAVE', 8);
    b.write('fmt ', 12); b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(2, 22);
    b.writeUInt32LE(rate, 24); b.writeUInt32LE(rate * 4, 28); b.writeUInt16LE(4, 32); b.writeUInt16LE(16, 34);
    b.write('data', 36); b.writeUInt32LE(samples * 4, 40);
    let o = 44;
    for (const c of chunks) for (let i = 0; i < c.length; i++) {
        const v = Math.max(-1, Math.min(1, c[i]));
        b.writeInt16LE(Math.round(v * 32767), o); o += 2;
    }
    fs.writeFileSync(file, b);
}

// --- spectra of one pushed block (by tag) ---------------------------------------
const NB = 24;
function bands(x) {   // x: Float32Array mono, length <= 1024 -> log energy in NB log-spaced bands
    const n = 1024, re = new Float64Array(n), im = new Float64Array(n);
    for (let i = 0; i < Math.min(n, x.length); i++) re[i] = x[i] * (0.5 - 0.5 * Math.cos(2 * Math.PI * i / Math.min(n, x.length)));
    fft(re, im);
    const out = new Float64Array(NB);
    for (let k = 1; k < n / 2; k++) {
        const f = k * 48000 / n;
        const b = Math.min(NB - 1, Math.max(0, Math.floor(NB * Math.log(f / 40) / Math.log(20000 / 40))));
        out[b] += re[k] * re[k] + im[k] * im[k];
    }
    for (let b = 0; b < NB; b++) out[b] = 10 * Math.log10(out[b] + 1e-9);
    return out;
}
function fft(re, im) {
    const n = re.length;
    for (let i = 1, j = 0; i < n; i++) { let bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; } }
    for (let len = 2; len <= n; len <<= 1) {
        const a = -2 * Math.PI / len, wr = Math.cos(a), wi = Math.sin(a);
        for (let i = 0; i < n; i += len) {
            let cr = 1, ci = 0;
            for (let k = 0; k < len / 2; k++) {
                const ur = re[i + k], ui = im[i + k];
                const vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci, vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi; re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                const t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}
function blockSpectra(rec) {
    const m = new Map();
    for (let i = 0; i < rec.tags.length; i++) {
        const [tag] = rec.tags[i], c = rec.chunks[i];
        if (tag === 0xFFFFFFFF) continue;
        const mono = new Float32Array(c.length >> 1);
        for (let k = 0; k < mono.length; k++) mono[k] = 0.5 * (c[2 * k] + c[2 * k + 1]);
        let e = 0; for (const v of mono) e += v * v;
        m.set(tag, { b: bands(mono), rms: Math.sqrt(e / Math.max(1, mono.length)) });
    }
    return m;
}
function specDist(a, b) { let s = 0; for (let k = 0; k < NB; k++) s += Math.abs(Math.max(a[k], -60) - Math.max(b[k], -60)); return s / NB; }

const pctl = (a, p) => { if (!a.length) return null; const s = Float64Array.from(a).sort(); return +s[Math.min(s.length - 1, Math.floor(p / 100 * s.length))].toFixed(2); };
const mean = (a) => a.length ? +(a.reduce((x, y) => x + y, 0) / a.length).toFixed(3) : null;

// --- per start: the guest's player stream against the host's ----------------------
// e: energy per render (probe), per player.  For a host start at slot s on
// player p, the host's stream over [s, s+K) is the reference; the guest's
// over [s+lag, s+lag+K) for lag 0..DMAXLAG.  Distance: mean |dB| difference
// (floor -20 dB under the reference peak).  Lag: the best match.  Attack:
// does the guest's first block at that lag carry the host's first block
// (within 6 dB)?
function eventMatch(host, guest, starts, firstSlot, lastSlot) {
    const K = 8, res = [];
    const db = (v) => 10 * Math.log10(v + 1);
    for (const ev of starts) {
        const { s, p } = ev;
        if (s < firstSlot || s + K + DMAXLAG > lastSlot) continue;
        const ref = [];
        for (let k = 0; k < K; k++) { const h = host.probe.get(s + k); ref.push(h ? db(h[p]) : 0); }
        const peak = Math.max(...ref);
        if (peak < 40) continue;                 // inaudible start (silent sequence)
        const floor = peak - 20;
        let best = Infinity, bestLag = -1, lag0 = null;
        for (let lag = 0; lag <= DMAXLAG; lag++) {
            let d = 0;
            for (let k = 0; k < K; k++) {
                const g = guest.probe.get(s + lag + k);
                d += Math.abs(Math.max(ref[k], floor) - Math.max(g ? db(g[p]) : 0, floor));
            }
            d /= K;
            if (lag === 0) lag0 = d;
            if (d < best - 0.05) { best = d; bestLag = lag; }
        }
        const g0 = guest.probe.get(s + bestLag);
        const attack = g0 ? Math.abs(db(g0[p]) - ref[0]) <= 6 || ref[0] < floor : false;
        res.push({ s, p, lag: bestLag, dist: best, lag0, attack, match: best <= 3 });
    }
    return res;
}

function summarizeEvents(ev) {
    const m = ev.filter((e) => e.match);
    return {
        starts: ev.length,
        matchedWithin3dB: m.length, matchedPct: +(100 * m.length / Math.max(1, ev.length)).toFixed(1),
        attackPct: +(100 * ev.filter((e) => e.match && e.attack).length / Math.max(1, ev.length)).toFixed(1),
        onTimePct: +(100 * ev.filter((e) => e.match && e.lag === 0).length / Math.max(1, ev.length)).toFixed(1),
        lagOfMatched: { p50: pctl(m.map((e) => e.lag), 50), p90: pctl(m.map((e) => e.lag), 90), max: m.length ? Math.max(...m.map((e) => e.lag)) : null },
        distBest: { mean: mean(ev.map((e) => e.dist)), p90: pctl(ev.map((e) => e.dist), 90) },
        distAtLag0: { mean: mean(ev.map((e) => e.lag0)), p90: pctl(ev.map((e) => e.lag0), 90) },
        byPlayer: [0, 1, 2, 3].map((p) => { const x = ev.filter((e) => e.p === p); return { n: x.length, matchedPct: +(100 * x.filter((e) => e.match).length / Math.max(1, x.length)).toFixed(1) }; }),
    };
}

function compareMixed(host, guest, firstSlot, lastSlot) {
    const H = blockSpectra(host), G = blockSpectra(guest);
    const d0 = [], dl = [];
    for (let t = firstSlot; t <= lastSlot; t++) {
        const g = G.get(t), h = H.get(t);
        if (!g || !h || (g.rms < 1e-4 && h.rms < 1e-4)) continue;
        d0.push(specDist(g.b, h.b));
        let best = Infinity;
        for (let lag = 0; lag <= DMAXLAG; lag++) { const hh = H.get(t - lag); if (hh) best = Math.min(best, specDist(g.b, hh.b)); }
        dl.push(best);
    }
    return { blocks: d0.length, specDistLag0dB: { mean: mean(d0), p90: pctl(d0, 90), p99: pctl(d0, 99) },
             specDistLagTolerantdB: { mean: mean(dl), p90: pctl(dl, 90), p99: pctl(dl, 99) },
             over6dBLag0Pct: +(100 * d0.filter((x) => x > 6).length / Math.max(1, d0.length)).toFixed(2),
             over6dBLagTolPct: +(100 * dl.filter((x) => x > 6).length / Math.max(1, dl.length)).toFixed(2) };
}

// Music continuity: the music player's (0) stream on the guest against the
// host's at lag 0, block by block; and clicks in the guest's mixed capture.
// Blocks near a music start (where a late start is by design) are left out:
// this is about music that is already playing.
function musicContinuity(host, guest, firstSlot, lastSlot, starts) {
    const db = (v) => 10 * Math.log10(v + 1);
    const dev = [], near = new Set();
    for (const e of starts) if (e.p === 0) for (let t = e.s - 5; t < e.s + DMAXLAG + 120; t++) near.add(t);
    for (let t = firstSlot; t <= lastSlot; t++) {
        if (near.has(t)) continue;
        const h = host.probe.get(t), g = guest.probe.get(t);
        if (!h || !g || h[0] < 1e4) continue;
        dev.push(Math.abs(db(g[0]) - db(h[0])));
    }
    return { blocks: dev.length, devdB: { mean: mean(dev), p99: pctl(dev, 99), max: dev.length ? +Math.max(...dev).toFixed(2) : null },
             over1dB: dev.filter((x) => x > 1).length, over3dB: dev.filter((x) => x > 3).length };
}
function clicks(rec) {
    // a sample-to-sample step far above the local level (mono)
    let n = 0, prev = 0, lvl = 1e-3;
    for (const c of rec.chunks) for (let i = 0; i < c.length; i += 2) {
        const v = 0.5 * (c[i] + c[i + 1]), d = Math.abs(v - prev);
        lvl = lvl * 0.999 + d * 0.001;
        if (d > 0.08 && d > 12 * lvl) n++;
        prev = v;
    }
    return n;
}

const EARNAMES = (M) => M._PortAudioEarNamesJs ? cstr(M, M._PortAudioEarNamesJs()).split(',').filter(Boolean) : [];
function earStats(inst) {
    const M = inst.M;
    if (!M._PortAudioEarNamesJs) return null;
    const names = EARNAMES(M), p = M._PortAudioStatsJs(2) >> 2, o = {};
    names.forEach((n, i) => { o[n] = M.HEAPU32[p + i]; });
    return o;
}

async function one(name, cfg, mode, modulePath, wav) {
    const host = recorder('host'), guest = recorder('guest');
    // the host's final starts: every start it notes (it never replays)
    const starts = [];
    const sc = buildScenario({ seed: cfg.seed || 1, humanFrames: Math.max(0, FRAMES - (SCRIPTED_END - BOOT_END) + 600) });
    const r = await runSession(Object.assign({ name, modulePath, rom, scenario: sc, durationFrames: FRAMES,
        hostAudio: host.spec, guestAudio: guest.spec,
        hostPre: (i) => { host.pre(mode)(i); },
        guestPre: (i) => { guest.pre(mode)(i); },
        noLocalise: true }, cfg));
    host.drain(); guest.drain();
    const hostEar = earStats(r._host), guestEar = earStats(r._guest);
    // host starts from the host's probe log: the ear keeps them in its
    // records; read every slot's songs straight from the host's rec ring
    const H = r._host.M;
    if (H._PortAudioEarStartsJs) {
        const n = H._PortAudioEarStartsJs(0) >>> 0, base = H._PortAudioEarStartsJs(1) >>> 0, cap = H._PortAudioEarStartsJs(2) >>> 0;
        for (let k = Math.max(0, n - cap); k < n; k++) {
            const off = (base >> 2) + (k % cap) * 3;
            starts.push({ s: H.HEAPU32[off], p: H.HEAPU32[off + 1], song: H.HEAPU32[off + 2] });
        }
    }
    const never = [];
    const G = r._guest.M;
    if (G._PortAudioEarDebugJs) {
        const n = G._PortAudioEarDebugJs(0) >>> 0, base = G._PortAudioEarDebugJs(1) >>> 0, cap = G._PortAudioEarDebugJs(2) >>> 0;
        for (let k = Math.max(0, n - cap); k < n; k++) { const off = (base >> 2) + (k % cap) * 4; never.push(Array.from(G.HEAPU32.subarray(off, off + 4))); }
    }
    const join = r.frames.join, end = r.frames.end;
    const firstSlot = join + 120, lastSlot = end - 60;
    const ev = starts.length ? eventMatch(host, guest, starts, firstSlot, lastSlot) : [];
    const out = {
        name, mode, link: cfg.link, forceRewind: cfg.forceRewind || null, frames: r.frames,
        identical: r.compare.mismatched === 0, compared: r.compare.compared, rewinds: r.guest.tel.rewinds,
        replayedFrames: r.guest.tel.replayedFrames, catchUpUsPerFrame: +(r.guest.tel.catchUpUsSum / Math.max(1, r.guest.tel.replayedFrames)).toFixed(2),
        r9: { live: r.guest.tel.soundsLive, refire: r.guest.tel.soundsRefire, spurious: r.guest.tel.soundsSpurious, missed: r.guest.tel.soundsMissed, judged: r.guest.tel.soundsJudged },
        ear: guestEar, hostEar,
        mixer: r.audio.guest && r.audio.guest.mixer, out: r.audio.guest && r.audio.guest.out,
        events: ev.length ? summarizeEvents(ev) : null,
        mixed: compareMixed(host, guest, firstSlot, lastSlot),
        music: musicContinuity(host, guest, firstSlot, lastSlot, starts),
        clicks: { host: clicks(host), guest: clicks(guest) },
        device: { host: r.audio.host && r.audio.host.device, guest: r.audio.guest && r.audio.guest.device },
        wallSec: r.wallSec,
        debug: { never, unmatched: ev.filter((e) => !e.match).slice(0, 60) },
    };
    if (wav) {
        const tagf = (rec) => rec.tags.map((t) => t.join(' ')).join('\n') + '\n';
        writeWav(path.join(OUT, `${wav}.guest.wav`), guest.chunks, guest.samples, 48000);
        fs.writeFileSync(path.join(OUT, `${wav}.guest.tags.txt`), tagf(guest));
        writeWav(path.join(OUT, `${wav}.host.wav`), host.chunks, host.samples, 48000);
        fs.writeFileSync(path.join(OUT, `${wav}.host.tags.txt`), tagf(host));
        out.wav = { guest: `${wav}.guest.wav`, host: `${wav}.host.wav` };
        // raw for byte comparisons
        const raw = (rec) => Buffer.concat(rec.chunks.map((c) => Buffer.from(c.buffer, c.byteOffset, c.byteLength)));
        fs.writeFileSync(path.join(OUT, `${wav}.guest.f32`), raw(guest));
        fs.writeFileSync(path.join(OUT, `${wav}.host.f32`), raw(host));
    }
    r._host = r._guest = null;
    log(`${name}: identical=${out.identical} rewinds=${out.rewinds} ear=${guestEar ? `final ${guestEar.finalStarts} heardFromStart ${guestEar.heardFromStart} never ${guestEar.neverHeard} spurious ${guestEar.spurious} doubles ${guestEar.doubles}` : '-'} events=${out.events ? out.events.matchedPct + '%' : '-'} wall ${out.wallSec}s`);
    return out;
}

const L = (oneWay, jitter, loss, extra = {}) => Object.assign({ oneWay, jitter, loss }, extra);
const conds = [];
for (const ow of [30, 60, 120]) if (ONLY.includes(String(ow))) conds.push({ id: `${ow}ms`, ow, cfg: { link: L(ow, 0, 0) } });
if (ONLY.includes('jitter')) conds.push({ id: '120ms j30 loss2%', ow: 'j', cfg: { link: L(120, 30, 0.02, { reorder: 0.02, reorderMs: 30, dup: 0.01 }) } });
if (ONLY.includes('forced')) conds.push({ id: '60ms + forced rewind 150f every 37f', ow: 'f', cfg: { link: L(60, 0, 0), forceRewind: { every: 37, depth: 150 } } });

const report = { started: new Date().toISOString(), module: MODULE, before: BEFORE, frames: FRAMES, runs: [] };
for (const c of conds) {
    const wavOn = WAVS.includes(String(c.ow));
    if (BEFORE && wavOn) report.runs.push(await one(`${c.id} OLD BUILD`, c.cfg, 1, path.resolve(BEFORE), `${c.ow}ms-oldbuild`));
    for (const m of MODES)
        report.runs.push(await one(`${c.id} reconciler ${m ? 'on' + (m > 1 ? ' policy ' + (m >> 1) : '') : 'off'}`, c.cfg, m, MODULE,
                                   wavOn && m < 2 ? `${c.ow}ms-${m ? 'after' : 'before'}` : null));
    fs.writeFileSync(path.join(OUT, 'sound-eval.json'), JSON.stringify(report, null, 1));
}
report.wallSec = (Date.now() - t0) / 1000;
fs.writeFileSync(path.join(OUT, 'sound-eval.json'), JSON.stringify(report, null, 1));
log('done', path.join(OUT, 'sound-eval.json'));
