#!/usr/bin/env node
// parallel.mjs -- the netproof rig (tools/netproof.mjs) on many cores: the
// same runs, the same gates, the same report, in a fraction of the wall time.
//
//   node tools/netproof/parallel.mjs [--rom ROM] [--module build/katam-node.js]
//        [--out DIR] [--jobs N] [--times FILE] [netproof options: --quick
//        --seeds N --only R2,R4 --skip-r11 --recording ...]
//
// 1. `netproof.mjs --plan` lists every run main() makes (index, name, size);
// 2. the runs are dealt to N worker processes, longest first (by the wall
//    seconds each run took last time -- FILE, default tools/netproof/run_times.json
//    -- or by its size in frames x devices when it has none);
// 3. each worker runs `netproof.mjs --shard i,j,.. --shard-out` (every other
//    run is a stub there) and saves its runs' results;
// 4. `netproof.mjs --merge` takes every run from those files and computes
//    every gate exactly as a sequential run would, then writes RIG-REPORT.md
//    and rig-report.json in DIR.
// Exit status 1 when a worker fails or any gate's verdict is FAIL.  Workers'
// logs go to DIR/shard-K.log; the merge's to stdout.
import fs from 'fs';
import os from 'os';
import path from 'path';
import { spawn } from 'child_process';
import { fileURLToPath } from 'url';

const here = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(here, '../..');
const NETPROOF = path.join(ROOT, 'tools', 'netproof.mjs');
const argv = process.argv.slice(2);
const own = new Set(['--jobs', '--times', '--out']);
const flag = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 ? argv[i + 1] : d; };
const OUT = path.resolve(flag('out', path.join(ROOT, '../netproof-eval')));
const JOBS = Math.max(1, +flag('jobs', Math.max(2, Math.floor(os.cpus().length / 2))));
const TIMES = flag('times', path.join(here, 'run_times.json'));
// everything else goes to netproof.mjs as it is
const pass = [];
for (let i = 0; i < argv.length; i++) {
    if (own.has(argv[i])) { i++; continue; }
    pass.push(argv[i]);
}
const t0 = Date.now();
const log = (...a) => console.log(`[parallel ${((Date.now() - t0) / 1000).toFixed(0)}s]`, ...a);

function node(args, logFile) {
    return new Promise((resolve) => {
        const out = logFile ? fs.openSync(logFile, 'w') : 'inherit';
        const p = spawn(process.execPath, [NETPROOF, ...args], { cwd: ROOT, stdio: ['ignore', out, out] });
        p.on('exit', (code, sig) => { if (logFile) fs.closeSync(out); resolve(code === null ? 128 : code); void sig; });
    });
}

fs.mkdirSync(OUT, { recursive: true });
const work = path.join(OUT, 'shards');
fs.rmSync(work, { recursive: true, force: true });
fs.mkdirSync(work, { recursive: true });

// 1. plan
const planFile = path.join(work, 'plan.json');
if (await node([...pass, '--out', OUT, '--plan', planFile], path.join(work, 'plan.log'))) {
    console.log(fs.readFileSync(path.join(work, 'plan.log'), 'utf8'));
    process.exit(1);
}
const plan = JSON.parse(fs.readFileSync(planFile, 'utf8'));
let times = {};
try { times = JSON.parse(fs.readFileSync(TIMES, 'utf8')); } catch (e) { /* none yet */ }
// a run with no recorded time: its size, scaled by the recorded runs' s/size
const known = plan.filter((r) => times[r.name] > 0);
const perCost = known.length ? known.reduce((a, r) => a + times[r.name], 0) / known.reduce((a, r) => a + r.cost, 0) : 1 / 6000;
for (const r of plan) r.est = times[r.name] > 0 ? times[r.name] : r.cost * perCost;

// 2. longest first onto the least loaded worker
const n = Math.min(JOBS, plan.length);
const shards = Array.from({ length: n }, () => ({ idx: [], est: 0 }));
for (const r of plan.slice().sort((a, b) => b.est - a.est)) {
    const s = shards.reduce((m, x) => (x.est < m.est ? x : m), shards[0]);
    s.idx.push(r.i); s.est += r.est;
}
const total = plan.reduce((a, r) => a + r.est, 0);
log(`${plan.length} runs, ~${total.toFixed(0)} s of work on ${n} workers (longest worker ~${Math.max(...shards.map((s) => s.est)).toFixed(0)} s, longest run ~${Math.max(...plan.map((r) => r.est)).toFixed(0)} s)`);

// 3. the workers
const files = shards.map((_, k) => path.join(work, `shard-${k}.json`));
const rcs = await Promise.all(shards.map((s, k) =>
    node([...pass, '--out', OUT, '--shard', s.idx.sort((a, b) => a - b).join(','), '--shard-out', files[k]], path.join(work, `shard-${k}.log`))));
const bad = rcs.map((rc, k) => [rc, k]).filter(([rc]) => rc !== 0);
if (bad.length) {
    for (const [rc, k] of bad) {
        log(`worker ${k} exited ${rc}; the end of its log:`);
        console.log(fs.readFileSync(path.join(work, `shard-${k}.log`), 'utf8').split('\n').slice(-15).join('\n'));
    }
    process.exit(1);
}
log('workers done');
// every run line, in run order, for the log
const lines = [];
for (let k = 0; k < n; k++)
    for (const l of fs.readFileSync(path.join(work, `shard-${k}.log`), 'utf8').split('\n'))
        if (/^\[netproof \d+s\] R\d+ .*(identical|MISMATCH)/.test(l) || /localised/.test(l)) lines.push(l.replace(/^\[netproof \d+s\] /, `  [w${k}] `));
console.log(lines.join('\n'));

// 4. merge: every gate, the report
const rc = await node([...pass, '--out', OUT, '--merge', files.join(',')], null);
if (rc) { log(`merge exited ${rc}`); process.exit(1); }
const rep = JSON.parse(fs.readFileSync(path.join(OUT, 'rig-report.json'), 'utf8'));
// this run's times, for the next one's dealing
const t = {};
for (const r of rep.runs) if (r.wallSec) t[r.name] = r.wallSec;
fs.writeFileSync(path.join(OUT, 'run_times.json'), JSON.stringify(t, null, 1) + '\n');
const fails = Object.entries(rep.gates).filter(([, v]) => v.verdict === 'FAIL').map(([g]) => g);
log(`${Object.keys(rep.gates).length} gates, ${fails.length ? 'FAIL: ' + fails.join(' ') : 'none FAIL'}; ${((Date.now() - t0) / 1000).toFixed(0)} s wall`);
process.exit(fails.length ? 1 : 0);
