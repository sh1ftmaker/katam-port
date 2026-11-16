#!/usr/bin/env node
// The host's network goes away for a while (bad internet for a bit): a rig
// gate on the page's liveness rules (web/ln_session.js HostLiveness and
// guestWait, run.mjs c.liveness) over the engine, wasm in node.
//
//   node tools/netproof/hostaway_test.mjs [--rom ROM] [--module build/katam-node.js]
//        [--module16 build/w16/katam-node.js] [--jobs N] [--only 4dev-*,one10]
//        [--out DIR] [--bound MS]
//
// The host's network cut = the same outage on every guest's link at once
// (the star has no other links), 8 s into the session, while everyone plays
// as people do.  At 3 and 4 devices on the 4-Kirby module and 8 on the
// 16-Kirby one (an 8-player room: seats 8, 16 keyframes, bounded redundancy),
// in TOGETHER and FREE ROAM:
//   cut 5, 10, 30 s   no guest gives up, nobody's Kirby goes to the CPU, the
//                     host holds its game while every guest is quiet (no
//                     frames on stale keys), every compared frame identical
//                     on every guest and frames compared well past the
//                     restore, at most one repair image per guest (two images
//                     with the join's), every guest back in step (within 30
//                     frames of the host, sealed frames caught up) within
//                     --bound ms of the restore (default 3000).
//   cut 45 s          every guest gives up (the page's HOST LEFT) when the
//                     grace runs out, the host lets them all go at the same
//                     point (their Kirbys to the CPU), and plays on alone:
//                     frames advancing, no guest left in its hub, every slot
//                     past its own the CPU's.
//   one10             4 devices, guest 1's own link cut 10 s while the others
//                     play: its Kirby to the CPU at 6 s (the host does not
//                     hold), its device kept, back in with one image when its
//                     link returns; the others never notice.  No give-up,
//                     identical frames.
// Each case is its own process (--jobs at a time); a summary line at the end.
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
const MODULE = path.resolve(flag('module', path.join(ROOT, 'build/katam-node.js')));
const MODULE16 = path.resolve(flag('module16', path.join(ROOT, 'build/w16/katam-node.js')));
// back in step: measured 0.07-0.29 s of virtual time after the restore in
// every case (the host held, so there is no catch-up burst); 1 s is the bound
const BOUND = +flag('bound', 1000);
const CUT_AT = 8000;                 // ms after the session's start
const LINK = { oneWay: 60, jitter: 15, loss: 0.02 };

function cases() {
    const out = [];
    for (const n of [3, 4, 8]) for (const mode of ['together', 'freeroam']) for (const secs of [5, 10, 30, 45])
        out.push({ name: `${n}dev-${mode}-cut${secs}`, n, rooms: mode === 'freeroam', secs, kind: 'cut' });
    out.push({ name: 'one10', n: 4, rooms: false, secs: 10, kind: 'one' });
    return out;
}
const match = (name, pats) => pats.some((p) => new RegExp('^' + p.split('*').map((x) => x.replace(/[.+?^${}()|[\]\\]/g, '\\$&')).join('.*') + '$').test(name));

// --- one case, in this process --------------------------------------------------
async function runCase(k) {
    const { runSession } = await import('./run.mjs');
    const { buildScenario, kirbyAddrOf } = await import('./scenario.mjs');
    const { PERIOD, LIVE } = await import('./session.mjs');
    const rom = fs.readFileSync(ROM);
    let pass = 0, fail = 0;
    const ok = (name, c, extra) => {
        if (c) pass++; else fail++;
        console.log(`  ${c ? 'ok  ' : 'FAIL'} ${k.name}: ${name}${extra !== undefined ? '  (' + extra + ')' : ''}`);
        return c;
    };
    const wide = k.n > 4;
    const mod = wide ? MODULE16 : MODULE;
    if (!fs.existsSync(mod)) { ok('the node module exists', false, mod); return { pass, fail }; }
    const restore = CUT_AT + k.secs * 1000;
    const tail = k.secs >= 45 ? 10000 : 14000;
    const durationFrames = Math.round((restore + tail) / PERIOD);
    const sc = buildScenario({ seed: 3, skipScripted: true, humanFrames: durationFrames + 600, kirbyAddr: kirbyAddrOf(wide ? 16 : 4) });
    const out = [CUT_AT, k.secs * 1000];
    const guests = [];
    for (let d = 1; d < k.n; d++)
        guests.push({ dev: d, link: Object.assign({}, LINK, (k.kind === 'cut' || d === 1) ? { outages: [out] } : {}) });
    const tl = {};                   // per guest: samples every 100 ms of virtual time
    const last = {};
    let hostF = 0, hostFAtRestore = null, err = null, r = null;
    const insts = [];
    try {
        r = await runSession(Object.assign({
            name: k.name, modulePath: mod, rom, scenario: sc, rooms: k.rooms, durationFrames, guests,
            link: LINK, liveness: {}, hostPre: (h) => insts.push(h), guestPre: (g) => insts.push(g),
            onHostFrame: (h, f) => { hostF = f; },
            onGuestTick: (g) => {
                const t = g.link.s.now;
                if (hostFAtRestore === null && t >= restore) hostFAtRestore = hostF;
                if (last[g.name] !== undefined && t - last[g.name] < 100) return;
                last[g.name] = t;
                const tel = g.inst.tel.read();
                (tl[g.name] = tl[g.name] || []).push({ t, f: g.inst.frame(), hostF, sealed: tel.sealedUpTo, cmp: g.cmpNext });
            },
        }, wide ? { seats: 8, kirbys: 8, kfCount: 16, storyNormal: true, hostOpts: { copies: 4, maxPacketBytes: 1100 } } : {}));
    } catch (e) { err = e; }
    ok('no device trapped', !err, err && String(err.stack || err).split('\n').slice(0, 4).join(' | '));
    if (err) return { pass, fail };
    const churn = (what) => r.churn.filter((x) => x.what === what);
    const gs = r.guests.filter((g) => g.gen === 0);
    const imgs = r.guestTels.map((t) => t.imagesLoaded);
    const heldMs = Math.round(r.hostHeld * PERIOD);
    // back in step: per guest, from the restore to its first sample within 30
    // frames of the host with its sealed frames caught up
    const back = {}, frozen = {};
    for (const [name, xs] of Object.entries(tl)) {
        const b = xs.find((x) => x.t > restore && Math.abs(x.f - x.hostF) < 30 && x.sealed >= x.hostF - 30);
        back[name] = b ? Math.round(b.t - restore) : null;
        let from = null, most = 0;
        for (let i = 1; i < xs.length; i++) {
            if (xs[i].f === xs[i - 1].f) { if (from === null) from = xs[i - 1].t; }
            else if (from !== null) { most = Math.max(most, xs[i].t - from); from = null; }
        }
        frozen[name] = Math.round(most);
    }
    const cmpEnd = Object.fromEntries(Object.entries(tl).map(([n, xs]) => [n, xs[xs.length - 1].cmp]));
    const info = { hostHeldMs: heldMs, liveness: r.liveness, backMs: back, frozenMs: frozen, images: imgs,
                   compare: r.compare, churn: r.churn.filter((x) => x.what !== 'admit' || x.why !== 'start'),
                   hostFAtRestore, hostFinal: r.frames.hostFinal, owners: r.owners, wallSec: r.wallSec };
    console.log(`  info ${k.name}: ${JSON.stringify(info)}`);
    ok('every compared frame identical', r.compare.mismatched === 0 && r.compare.compared > 0, `${r.compare.equal}/${r.compare.compared}`);
    if (k.kind === 'one') {
        const g1 = gs.find((g) => g.dev === 1);
        ok('no guest gave up', !churn('gaveup').length, JSON.stringify(churn('gaveup')));
        ok('the host never held (the others were heard)', heldMs === 0, `${heldMs} ms`);
        const cpu = churn('cpu');
        ok('guest 1\'s Kirby to the CPU once, at about 6 s', cpu.length === 1 && cpu[0].dev === 1
           && Math.abs(cpu[0].t - CUT_AT - LIVE.deadMs) < 700, JSON.stringify(cpu));
        const bk = r.churn.find((x) => x.what === 'admit' && x.why === 'back');
        ok('guest 1 back in (a join image) soon after its link returned', bk && bk.dev === 1 && bk.t > restore && bk.t - restore < 2500,
           JSON.stringify(bk));
        ok('nobody forgotten', !churn('gone').length);
        ok('guest 1: at most one image past its join', !!g1 && imgs[0] <= 2, imgs.join(','));
        ok('the others: no repair', imgs.slice(1).every((n) => n <= 1), imgs.join(','));
        ok('guest 1 compared past its return', cmpEnd.guest1 > (hostFAtRestore || 0) + 300, `${cmpEnd.guest1} vs ${hostFAtRestore}`);
        ok('guest 1 back in step', back.guest1 !== null && back.guest1 <= BOUND + 1000, `${back.guest1} ms`);
        return { pass, fail, info };
    }
    if (k.secs < 45) {
        ok('no guest gave up', !churn('gaveup').length, JSON.stringify(churn('gaveup')));
        ok('no Kirby to the CPU, nobody forgotten', !churn('cpu').length && !churn('gone').length,
           JSON.stringify(r.churn.filter((x) => ['cpu', 'gone', 'dead'].includes(x.what))));
        ok('the host held while every guest was quiet', heldMs > (k.secs - 1.5) * 1000 && heldMs < k.secs * 1000 + 1000, `${heldMs} ms`);
        ok('at most one repair image per guest', imgs.every((n) => n <= 2), imgs.join(','));
        const late = Object.entries(cmpEnd).filter(([, c]) => !(c > hostFAtRestore + 300));
        ok('every guest compared well past the restore', !late.length, JSON.stringify(cmpEnd) + ' restore f' + hostFAtRestore);
        const worst = Math.max(...Object.values(back).map((x) => (x === null ? Infinity : x)));
        ok(`every guest back in step within ${BOUND} ms of the restore`, worst <= BOUND, JSON.stringify(back));
        return { pass, fail, info, worst };
    }
    // 45 s: the grace runs out
    const up = churn('gaveup'), gone = churn('gone');
    const want = LIVE.graceMs;
    ok('every guest gave up (HOST LEFT)', up.length === k.n - 1, JSON.stringify(up));
    ok('... when the grace ran out', up.every((x) => Math.abs(x.t - CUT_AT - want) < 800), up.map((x) => x.t - CUT_AT).join(','));
    ok('the host let every guest go at the same point', gone.length === k.n - 1 && gone.every((x) => Math.abs(x.t - CUT_AT - want) < 1200)
       && r.liveness && r.liveness.expired === 1, JSON.stringify(gone));
    ok('no Kirby to the CPU before that (the host held)', !churn('cpu').length);
    const drop = gone.length ? gone[0].f : null;
    ok('the host plays on alone afterwards', drop !== null && r.frames.hostFinal - drop > (restore + tail - CUT_AT - want) / PERIOD * 0.8,
       `${drop} -> ${r.frames.hostFinal}`);
    ok('the host is not holding at the end', r.liveness && !r.liveness.holdingAtEnd);
    ok('every slot past the host\'s is the CPU\'s', r.owners.slice(1).every((o) => o < 0), JSON.stringify(r.owners));
    return { pass, fail, info };
}

// --- the driver -----------------------------------------------------------------------
const one = flag('case', null);
if (one) {
    const k = cases().find((x) => x.name === one);
    process.on('uncaughtException', (e) => {
        console.log(`  FAIL ${one}: a device trapped: ${String(e.stack || e).split('\n').slice(0, 3).join(' | ')}`);
        console.log(`\n0 passed, 1 failed`);
        process.exit(1);
    });
    const res = await runCase(k);
    const jf = flag('json', null);
    if (jf) fs.writeFileSync(jf, JSON.stringify(res));
    console.log(`\n${res.pass} passed, ${res.fail} failed`);
    process.exit(res.fail ? 1 : 0);
}
const ONLY = flag('only', null);
const todo = cases().filter((k) => !ONLY || match(k.name, ONLY.split(',')));
const JOBS = Math.max(1, +flag('jobs', Math.min(5, os.cpus().length)));
const OUT = flag('out', null);
const dir = OUT ? path.resolve(OUT) : fs.mkdtempSync(path.join(os.tmpdir(), 'hostaway-'));
fs.mkdirSync(dir, { recursive: true });
const t0 = Date.now();
// the slow ones first (8 devices, long cuts)
todo.sort((a, b) => b.n * (b.secs + 20) - a.n * (a.secs + 20));
let pass = 0, fail = 0, next = 0;
const results = {};
async function worker() {
    while (next < todo.length) {
        const k = todo[next++];
        const jf = path.join(dir, `${k.name}.json`), lf = path.join(dir, `${k.name}.log`);
        const code = await new Promise((resolve) => {
            const fd = fs.openSync(lf, 'w');
            const p = spawn(process.execPath, [SELF, '--case', k.name, '--json', jf, '--rom', ROM, '--module', MODULE,
                                               '--module16', MODULE16, '--bound', String(BOUND)],
                            { cwd: ROOT, stdio: ['ignore', fd, fd] });
            p.on('exit', (c) => { fs.closeSync(fd); resolve(c === null ? 128 : c); });
        });
        const text = fs.readFileSync(lf, 'utf8');
        for (const l of text.split('\n')) if (/^\s+(ok|FAIL|info)/.test(l)) console.log(l);
        let res = null;
        try { res = JSON.parse(fs.readFileSync(jf, 'utf8')); } catch (e) { /* crashed */ }
        if (res) { pass += res.pass; fail += res.fail; results[k.name] = res; }
        else { fail++; console.log(`  FAIL ${k.name}: no result (exit ${code}); ${text.split('\n').slice(-6).join(' | ')}`); }
        console.log(`[hostaway ${((Date.now() - t0) / 1000).toFixed(0)}s] ${k.name} done (exit ${code})`);
    }
}
await Promise.all(Array.from({ length: Math.min(JOBS, todo.length) }, worker));
fs.writeFileSync(path.join(dir, 'hostaway.json'), JSON.stringify(results, null, 1));
const worst = Object.entries(results).filter(([, r]) => r.worst !== undefined).map(([n, r]) => [n, r.worst]).sort((a, b) => b[1] - a[1]);
if (worst.length) console.log(`back in step, worst cases: ${worst.slice(0, 5).map(([n, w]) => `${n} ${w} ms`).join(', ')}`);
console.log(`results in ${dir}`);
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
