#!/usr/bin/env node
// gates_parallel.mjs -- tools/rooms/gates.mjs with every (plan, seed) in its
// own process, and a verdict.
//
//   node tools/rooms/gates_parallel.mjs --rom ROM --out DIR
//        [--only two,three,...] [--seeds 1,2,3] [--long K] [--jobs N]
//        [--module build/katam-node.js]
//
// Each run is the same run gates.mjs makes (same plan, seed, length); its
// JSON goes to DIR/<name>.json and its log to DIR/<name>.log, and all of them
// to DIR/rooms-gates.json in gates.mjs's own shape (for tools/rooms/report.py).
// A run FAILS when:
//   - it aborted, or a guest's canonical shadow differs from the host's on a
//     final frame (compare.mismatched), or a guest's personal digest differs
//     from its shadow's (free-roam runs: no corruption is injected);
//   - a guest's screen was not on its own player in that player's room on a
//     checked frame (screen.wrong);
//   - a phone call that did not gather everyone has no known reason
//     ("UNEXPLAINED", gates.mjs phoneTrack);
//   - hint: the waiting hint was not shown at a hub-door visit;
//   - corrupt/pool: an injected corruption that the session must see
//     (everything but a VRAM byte on the personal timeline, which is not
//     gameplay state and is not in the digest) was not detected and repaired.
// Exit status 1 if any run fails or a process exits non-zero.
import fs from 'fs';
import os from 'os';
import path from 'path';
import { spawn } from 'child_process';
import { fileURLToPath } from 'url';

const here = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(here, '../..');
const a = { only: 'two,three,four,join,local2,hostlocal2,hint', seeds: '1', long: null, jobs: Math.max(2, os.cpus().length >> 1), module: null };
for (let i = 2; i < process.argv.length; i++) {
    const k = process.argv[i].replace(/^--/, ''), v = process.argv[++i];
    if (!(k in a) && k !== 'rom' && k !== 'out') throw new Error('unknown --' + k);
    a[k] = v;
}
const OUT = path.resolve(a.out);
fs.mkdirSync(OUT, { recursive: true });
const LONG = new Set(['two', 'three', 'four', 'join', 'local2', 'hostlocal2']);
// the longest first: rough frames x devices of each plan in gates.mjs
const SIZE = { two: 2 * 30000, three: 3 * 24000, four: 4 * 24000, join: 3 * 9000, local2: 2 * 18000, hostlocal2: 2 * 18000, hint: 2 * 12000, corrupt: 3 * 9000, pool: 3 * 6000, cost: 4 * 2 * 9000 };
const jobs = [];
for (const s of a.seeds.split(',').map(Number)) for (const p of a.only.split(','))
    jobs.push({ plan: p, seed: s, name: s === 1 ? p : `${p}-s${s}`, size: (SIZE[p] || 50000) * (LONG.has(p) && a.long ? +a.long : 1) });
jobs.sort((x, y) => y.size - x.size);

const t0 = Date.now();
function runOne(j) {
    return new Promise((resolve) => {
        const args = [path.join(here, 'gates.mjs'), '--rom', a.rom, '--only', j.plan, '--seeds', String(j.seed), '--out', path.join(OUT, j.name + '.json')];
        if (a.long) args.push('--long', String(a.long));
        if (a.module) args.push('--module', a.module);
        const fd = fs.openSync(path.join(OUT, j.name + '.log'), 'w');
        const t = Date.now();
        const p = spawn(process.execPath, args, { cwd: ROOT, stdio: ['ignore', fd, fd] });
        p.on('exit', (code) => { fs.closeSync(fd); j.rc = code; j.secs = (Date.now() - t) / 1000; resolve(); });
    });
}
const queue = jobs.slice();
await Promise.all(Array.from({ length: Math.min(+a.jobs, jobs.length) }, async () => { while (queue.length) await runOne(queue.shift()); }));

function judge(r) {
    const why = [];
    if (r.aborted) why.push('aborted: ' + JSON.stringify(r.aborted));
    const injected = r.corrupt && r.corrupt.length;
    if (!injected && r.compare.mismatched) why.push(`canonical ${r.compare.mismatched} mismatched frames (first ${r.compare.firstMismatch})`);
    for (const g of r.guests) {
        if (!injected && g.digest.mismatched) why.push(`${g.name}: personal digest ${g.digest.mismatched} mismatched`);
        if (!g.screen || g.screen.wrong) why.push(`${g.name}: screen wrong ${g.screen ? g.screen.wrong : '?'}/${g.screen ? g.screen.checks : '?'}`);
    }
    for (const w of Object.keys((r.phoneSummary || {}).reasons || {})) if (/UNEXPLAINED/.test(w)) why.push('phone: ' + w);
    if (r.plan === 'hint' && !(r.hints.length && r.hints.every((h) => h.door > 0))) why.push('waiting hint not shown at every hub-door visit: ' + JSON.stringify(r.hints));
    for (const c of r.corrupt || []) {
        if (!c.applied) { why.push(`corruption not applied: ${c.what}`); continue; }
        if (c.target === 'live' && /VRAM/.test(c.what)) continue;
        if (c.detectFrames === null || c.repairFrames === null) why.push(`${c.target} ${c.what} @${c.frame}: detected ${c.detectFrames}, repaired ${c.repairFrames}`);
    }
    return why;
}

const all = [];
let failed = 0;
for (const j of jobs.sort((x, y) => (x.seed - y.seed) || x.plan.localeCompare(y.plan))) {
    let runs = [];
    try { runs = JSON.parse(fs.readFileSync(path.join(OUT, j.name + '.json'), 'utf8')).runs; } catch (e) { /* none */ }
    if (j.rc !== 0 || !runs.length) {
        failed++;
        const tail = fs.readFileSync(path.join(OUT, j.name + '.log'), 'utf8').split('\n').slice(-6).join('\n    ');
        console.log(`FAIL ${j.name}: exit ${j.rc}, ${runs.length} runs (${j.secs.toFixed(0)} s)\n    ${tail}`);
        continue;
    }
    for (const r of runs) {
        all.push(r);
        const why = judge(r);
        if (why.length) failed++;
        const g = r.guests.map((x) => `${x.name} canon ${x.compare ? x.compare.equal + '/' + x.compare.compared : '?'} digest ${x.digest.equal}/${x.digest.compared} screen wrong ${x.screen ? x.screen.wrong : '?'}`).join('; ');
        console.log(`${why.length ? 'FAIL' : 'ok  '} ${r.name}: ${r.compare.equal}/${r.compare.compared} final frames identical; ${g}; ${j.secs.toFixed(0)} s` +
            (why.length ? '\n    ' + why.join('\n    ') : ''));
    }
}
fs.writeFileSync(path.join(OUT, 'rooms-gates.json'), JSON.stringify({ when: new Date().toISOString(), module: a.module, runs: all }, null, 1));
console.log(`${all.length} runs, ${failed} failing, ${((Date.now() - t0) / 1000).toFixed(0)} s wall`);
process.exit(failed ? 1 : 0);
