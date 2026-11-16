#!/usr/bin/env node
// The pause menu online is each device's own (platform/lmpnet.c "the pause
// overlay"): a rig gate on the engine, wasm in node, the session logic of
// the page (tools/netproof/run.mjs).
//
//   node tools/netproof/pause_test.mjs [--rom ROM] [--module build/katam-node.js]
//        [--module16 build/w16/katam-node.js] [--only four,repair,eight]
//
// Cases (each one session, players driven by pads that react to their own
// device's screen, all others playing on as people do):
//   four    the 4-Kirby module, host + 2 guests over a lossy, jittery link,
//           every guest rewound 150 frames every 37 (a rollback during every
//           open overlay).  Guest 1 pauses: only its device shows the menu,
//           the shared game never pauses (no device's game ever shows an
//           interlude), everyone's frames go on and the other Kirbys move;
//           its slot's sealed keys are 0 while it is up.  Its pages and the
//           SETTINGS cursor move only after its own presses, while the host
//           mashes Select.  Guest 2 pauses as well: both navigate on their
//           own (the pause opens on SETTINGS: guest 1 stays, guest 2 turns
//           to the help page).  Guest 1 changes VOLUME on its SETTINGS > SOUND page: its
//           own setting, nobody else's.  Each closes with its own B / START
//           and plays on (its keys sealed again).  The host pauses: no guest
//           is touched, slot 0's sealed keys are 0 meanwhile.  Every final
//           frame of every guest identical with the host's, no repairs.
//   repair  host + 1 guest: the guest pauses and goes to SETTINGS, its game
//           is corrupted (the RNG word) and repaired by the host's image while
//           the overlay is up: the overlay is still up on the same page and
//           row, and closes as ever.
//   memory  the 16-Kirby module as a real 8-player room's guest has it: FREE
//           ROAM (the canonical shadow's two buffers), 16 keyframes, images:
//           the overlay's buffers are taken as the session starts and leave
//           room (at least 4 MiB free on every device), a guest pauses; and
//           a guest whose heap cannot hold them: the session runs on, START
//           opens nothing, nothing aborts.  (Live, the overlay's lazy malloc
//           on a FREE ROAM katam16 guest aborted the page: Aborted(OOM).)
//   eight   the 16-Kirby module, an 8-player room, host + 5 guests (8 Kirbys):
//           guest 4 (slot 4) and guest 5 (slot 5) pause, at once; guest 4 to
//           SETTINGS and back; the host mashes; then the host pauses.
//           Identical frames throughout.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { runSession } from './run.mjs';
import { buildScenario, human, BOOT_END, EV } from './scenario.mjs';
import { KEY } from './instance.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(here, '../..');
const argv = process.argv.slice(2);
const flag = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 ? argv[i + 1] : d; };
const ROM = flag('rom', process.env.ROM || '/bigdata/agent-tom/projects/katamport/decomp/baserom.gba');
const MODULE = path.resolve(flag('module', path.join(ROOT, 'build/katam-node.js')));
const MODULE16 = path.resolve(flag('module16', path.join(ROOT, 'build/w16/katam-node.js')));
const ONLY = flag('only', 'four,repair,eight,memory').split(',');
const rom = fs.readFileSync(ROM);
const S = BOOT_END;                 // the session's first frame (run.mjs startFrame)
const LINK = { oneWay: 60, jitter: 20, loss: 0.02, reorder: 0.02, reorderMs: 20 };

let pass = 0, fail = 0;
const ok = (name, c, extra) => {
    if (c) pass++; else fail++;
    console.log(`  ${c ? 'ok  ' : 'FAIL'} ${name}${extra !== undefined ? '  (' + extra + ')' : ''}`);
    return c;
};
const t0 = Date.now();
const say = (t) => console.log(`[pause ${((Date.now() - t0) / 1000).toFixed(0)}s] ${t}`);

// A device's pad: a person playing (no Start), and while it has steps to do
// or its overlay is up, only the steps.  A step: from relative frame `at`,
// tap `keys` (held 6 frames) every `every` frames until `until(ovl)`, at most
// `max` taps (no `until`: once).  Every tap is logged.
function device(seed, steps) {
    const h = human(seed, { start: 0 });
    const d = { taps: [], trace: [], i: 0, n: 0, last: -1e9, holding: 0 };
    d.pad = (f, obs) => {
        const r = f - S, o = obs && obs.ovl ? obs.ovl : { state: 0 };
        if (obs) d.trace.push({ f, st: o.state, menu: o.menuId, row: o.row, model: o.model, il: obs.interlude, pauser: o.pauser });
        if (f - d.last < 6) return d.holding;
        for (;;) {
            const s = steps[d.i];
            if (!s || r < s.at || f - d.last < (s.every || 50))
                break;
            if ((s.until && s.until(o)) || (!s.until && d.n >= 1)) { d.i++; d.n = 0; continue; }
            if (d.n >= (s.max || 8)) { d.taps.push({ f, keys: 0, gaveUp: s.name }); d.i++; d.n = 0; continue; }
            d.n++; d.last = f; d.holding = s.keys;
            d.taps.push({ f, keys: s.keys, name: s.name });
            return s.keys;
        }
        const s = steps[d.i];
        if (s && r >= s.at - 30) return 0;          // about to act: hands off
        if (o.state) return 0;                      // the overlay up: only the steps
        return h(f);
    };
    return d;
}
const tap = (name, at, keys, until, extra = {}) => Object.assign({ name, at, keys, until }, extra);
const open = (o) => o.state === 2;
const closed = (o) => o.state === 0;

// The sealed key of slot s in host frame f (PortLnSealedGetN's words).
function sealedKey(host, f, s) {
    const M = host.M, p = M._malloc(512);
    let k = null;
    if (M._PortLnSealedGetN(f, p)) {
        const w = (i) => M.HEAPU32[(p >> 2) + i];
        k = s < 2 ? w(s) : s < 4 ? w(7 + s - 2) : w(10 + s - 4);
    }
    M._free(p);
    return k;
}

// Windows of a device's trace where its overlay was in a state.
function spans(tr, st) {
    const out = [];
    let a = null;
    for (const x of tr) {
        if (x.st === st && a === null) a = x.f;
        if (x.st !== st && a !== null) { out.push([a, x.f]); a = null; }
    }
    if (a !== null) out.push([a, tr[tr.length - 1].f + 1]);
    return out;
}
// Every change of `key` in the open overlay comes within `win` frames after
// one of the device's own taps (rewinds and other devices move nothing).
function changesFollowTaps(d, key, win = 70) {
    const bad = [];
    let prev = null;
    for (const x of d.trace) {
        if (x.st !== 2) { prev = null; continue; }
        const v = x[key];
        if (prev !== null && v !== prev && !d.taps.some((t) => t.keys && x.f >= t.f && x.f - t.f <= win))
            bad.push({ f: x.f, from: prev, to: v });
        prev = v;
    }
    return bad;
}

function scenarioWith(pads, events) {
    const sc = buildScenario({ skipScripted: true, humanFrames: 1 });
    sc.events = events;
    sc.pad = (dev, seat) => (seat === 0 && pads[dev] ? pads[dev].pad : null);
    return sc;
}

async function caseFour() {
    say('# four: the 4-Kirby module, host + 2 guests, rewinds during every overlay');
    const g1 = device(11, [
        tap('g1 start', 400, KEY.START, open, { every: 40 }),
        tap('g1 to SETTINGS', 520, KEY.SELECT, (o) => o.menuId === 8),   // (it opens there)
        tap('g1 down to SOUND', 900, KEY.DOWN, (o) => o.row === 1, { every: 40 }),
        tap('g1 open SOUND', 960, KEY.A, (o) => o.model === 11, { every: 40 }),
        tap('g1 volume down', 1040, KEY.LEFT),
        tap('g1 close', 1200, KEY.START, closed, { every: 60 }),
    ]);
    const g2 = device(12, [
        tap('g2 start', 650, KEY.START, open, { every: 40 }),
        tap('g2 page', 760, KEY.SELECT),
        tap('g2 close', 1300, KEY.B, closed, { every: 60 }),
    ]);
    const hostSteps = [];
    for (let r = 450; r < 1150; r += 23) hostSteps.push(tap('host mash', r, KEY.SELECT, undefined, { every: 1 }));
    hostSteps.push(tap('host start', 1600, KEY.START, open, { every: 40 }));
    hostSteps.push(tap('host close', 2000, KEY.START, closed, { every: 60 }));
    const hd = device(10, hostSteps);
    const words = { 0: [], 1: [], 2: [] }, sealed = [], pos = [];
    const r = await runSession({
        name: 'pause four', modulePath: MODULE, rom, scenario: scenarioWith({ 0: hd, 1: g1, 2: g2 },
            [{ f: S + 200, type: EV.WARP, a: 0, b: 117, c: 44, d: 9 }]),
        guests: [{ dev: 1 }, { dev: 2 }], link: LINK, durationFrames: 2500, forceRewind: { every: 37, depth: 150 },
        onHostFrame: (host, f) => {
            sealed.push({ f: f - 1, k: [0, 1, 2].map((s) => sealedKey(host, f - 1, s)) });
            pos.push({ f, k: [0, 1, 2].map((i) => { const k = host.kirby(i); return [k.x, k.y, k.room]; }) });
            words[0].push([f, host.M._PortPocSettingsWord() >>> 0]);
        },
        onGuestTick: (g) => { if (!g.gen) words[g.dev].push([g.inst.frame(), g.inst.M._PortPocSettingsWord() >>> 0]); },
    });
    const devs = { host: hd, g1, g2 };
    if (process.env.PAUSE_DEBUG) {
        console.log('g1 taps', JSON.stringify(g1.taps));
        console.log('g1 trace', JSON.stringify(g1.trace.filter((x) => x.f >= S + 940 && x.f < S + 1100 && x.f % 5 === 0)));
    }
    for (const [n, d] of Object.entries(devs))
        ok(`four: ${n}'s steps all done`, !d.taps.some((t) => t.gaveUp), JSON.stringify(d.taps.filter((t) => t.gaveUp)));
    ok('four: no device\'s game ever paused (no interlude anywhere)', Object.values(devs).every((d) => d.trace.every((x) => !x.il)));
    const o1 = spans(g1.trace, 2), o2 = spans(g2.trace, 2), oh = spans(hd.trace, 2);
    ok('four: guest 1 opened its overlay once, as it pressed Start', o1.length === 1 && o1[0][0] - (S + 400) < 60, JSON.stringify(o1));
    ok('four: guest 2 opened its overlay once', o2.length === 1, JSON.stringify(o2));
    ok('four: the host opened its overlay once', oh.length === 1 && oh[0][0] >= S + 1600, JSON.stringify(oh));
    ok('four: while guest 1\'s is up, the host shows none', hd.trace.filter((x) => x.f >= o1[0][0] && x.f < o1[0][1]).every((x) => x.st === 0));
    ok('four: two overlays up at once (guests 1 and 2)', o2.length && o2[0][0] < o1[0][1]);
    ok('four: while the host\'s is up, no guest shows one', [g1, g2].every((d) => d.trace.filter((x) => oh.length && x.f >= oh[0][0] && x.f < oh[0][1]).every((x) => x.st === 0)));
    ok('four: guest 1\'s fork paused for guest 1 (slot 1)', g1.trace.filter((x) => x.st === 2).every((x) => x.pauser === 1));
    ok('four: guest 1 reached SETTINGS (menuId 8) and SOUND (model 11)', g1.trace.some((x) => x.st === 2 && x.menu === 8) && g1.trace.some((x) => x.model === 11));
    ok('four: guest 1\'s pause opened on SETTINGS (no Select pressed)', !g1.taps.some((t) => t.keys & KEY.SELECT) && g1.trace.some((x) => x.st === 2 && x.menu === 8),
       JSON.stringify(g1.taps.filter((t) => t.keys & KEY.SELECT)));
    ok('four: guest 2\'s Select turned SETTINGS to the help page', g2.trace.some((x) => x.st === 2 && x.menu === 8) && g2.trace.some((x) => x.st === 2 && x.menu === 1));
    for (const [n, d] of [['guest 1', g1], ['guest 2', g2], ['host', hd]]) {
        const bm = changesFollowTaps(d, 'menu'), br = changesFollowTaps(d, 'row'), bo = changesFollowTaps(d, 'model');
        ok(`four: ${n}'s pages, rows and screens move only after its own presses (rewinds, others' pads: nothing)`,
           !bm.length && !br.length && !bo.length, JSON.stringify([bm, br, bo]).slice(0, 300));
    }
    // sealed keys of a slot are 0 while its overlay is up (from the frame after it opened)
    const notZero = (slot, sp) => sealed.filter((x) => x.f > sp[0] + 2 && x.f < sp[1] - 2 && x.k[slot] !== null && x.k[slot] !== 0).map((x) => [x.f, x.k[slot]]);
    ok('four: guest 1\'s slot sealed idle while its overlay is up', !notZero(1, o1[0]).length, JSON.stringify(notZero(1, o1[0]).slice(0, 12)));
    ok('four: the host\'s slot sealed idle while its overlay is up', oh.length && !notZero(0, oh[0]).length, JSON.stringify(oh.length ? notZero(0, oh[0]).slice(0, 12) : null));
    ok('four: no Start ever sealed (the shared game never pauses)', sealed.every((x) => x.k.every((k) => !(k & KEY.START))));
    ok('four: guest 1 plays again after closing (its slot sealed with keys)', sealed.some((x) => x.f > o1[0][1] + 30 && x.k[1]));
    const moved = (slot, a, b) => {
        const p = pos.filter((x) => x.f >= a && x.f < b).map((x) => x.k[slot]);
        return p.length > 2 && p.some((q) => q[0] !== p[0][0] || q[1] !== p[0][1]);
    };
    ok('four: during guest 1\'s overlay the host\'s and guest 2\'s Kirbys move', moved(0, o1[0][0], o1[0][1]) && moved(2, o1[0][0], o2[0][0]));
    ok('four: during the host\'s overlay both guests\' Kirbys move', oh.length && moved(1, oh[0][0], oh[0][1]) && moved(2, oh[0][0], oh[0][1]));
    const snd = (w) => `volume ${(w >>> 10) & 127} mute ${(w >>> 17) & 1}`;
    const w1 = words[1], last = (a) => a[a.length - 1][1], first = (a) => a[0][1];
    ok('four: guest 1\'s VOLUME changed on its own device', last(w1) !== first(w1) && ((last(w1) ^ first(w1)) & ~(0xFF << 10)) === 0,
       `${snd(first(w1))} -> ${snd(last(w1))}`);
    ok('four: nobody else\'s settings changed', last(words[0]) === first(words[0]) && last(words[2]) === first(words[2]));
    ok('four: frames advanced everywhere', r.frames.hostFinal >= S + 2500 && r.guests.every((g) => g.frame >= S + 2400));
    ok('four: every compared frame identical, none skipped for a repair', r.compare.mismatched === 0 && r.compare.compared > 4000,
       `${r.compare.equal}/${r.compare.compared}`);
    ok('four: the guests rewound (rollbacks during the overlays)', r.guestTels.every((t) => t.rewinds > 30), r.guestTels.map((t) => t.rewinds).join(','));
    ok('four: no repairs', r.guestTels.every((t) => t.imagesLoaded <= 1), r.guestTels.map((t) => t.imagesLoaded).join(','));
    const tl = r.guestTels[0];
    ok('four: guest 1 telemetry: 1 open, 1 close, fork frames run', tl.ovlOpens === 1 && tl.ovlCloses === 1 && tl.ovlFrames > 500,
       `opens ${tl.ovlOpens} closes ${tl.ovlCloses} frames ${tl.ovlFrames} swap ${(tl.ovlSwapUsSum / Math.max(1, tl.ovlFrames)).toFixed(0)} us avg, max ${tl.ovlSwapUsMax}`);
    ok('four: guest 1 telemetry: its page\'s edit carried out', tl.ovlEdits >= 1, tl.ovlEdits);
}

async function caseRepair() {
    say('# repair: an image load (repair) while the overlay is up');
    const g1 = device(21, [
        tap('g1 start', 300, KEY.START, open, { every: 40 }),
        tap('g1 to SETTINGS', 420, KEY.SELECT, (o) => o.menuId === 8),
        tap('g1 down', 700, KEY.DOWN, (o) => o.row === 2, { every: 40 }),
        tap('g1 close', 1500, KEY.START, closed, { every: 60 }),
    ]);
    const hd = device(20, []);
    let repairedAt = null, before = null, after = null;
    const r = await runSession({
        name: 'pause repair', modulePath: MODULE, rom, scenario: scenarioWith({ 0: hd, 1: g1 }, [{ f: S + 150, type: EV.WARP, a: 0, b: 117, c: 44, d: 9 }]),
        guests: [{ dev: 1 }], link: { oneWay: 40, jitter: 5 }, durationFrames: 1900, lockstep: true,
        corrupt: [{ what: 'RNG word', frame: S + 900, apply: (g) => { g.poke32(0x030068D8, g.peek32(0x030068D8) ^ 0x5A5A); } }],
        onGuestTick: (g) => {
            const t = g.inst.tel.read(), o = g.inst.overlay();
            if (repairedAt === null && t.imagesLoaded >= 2) { repairedAt = g.inst.frame(); after = o; }
            if (repairedAt === null && g.inst.frame() >= S + 880) before = o;
        },
    });
    ok('repair: the corruption was found and repaired by an image', repairedAt !== null && r.corrupt.length === 1 && r.corrupt[0].repairT !== undefined,
       `repaired at f${repairedAt}`);
    ok('repair: the overlay was up before and after, on the same page and row', before && after && before.state === 2 && after.state === 2
       && before.menuId === after.menuId && before.row === after.row && after.menuId === 8, JSON.stringify({ before, after }));
    ok('repair: it closed later with its own Start', g1.taps.some((t) => t.name === 'g1 close') && !g1.taps.some((t) => t.gaveUp));
    ok('repair: no interlude in either game', [g1, hd].every((d) => d.trace.every((x) => !x.il)));
}

async function caseEight() {
    if (!fs.existsSync(MODULE16)) { ok('eight: the 16-Kirby node module exists', false, MODULE16); return; }
    say('# eight: the 16-Kirby module, 8 seats, host + 5 guests; slots 4 and 5 pause');
    const g4 = device(44, [
        tap('g4 start', 500, KEY.START, open, { every: 40 }),
        tap('g4 to SETTINGS', 620, KEY.SELECT, (o) => o.menuId === 8),
        tap('g4 down', 1000, KEY.DOWN, (o) => o.row === 3, { every: 40 }),
        tap('g4 close', 1300, KEY.B, closed, { every: 60 }),
    ]);
    const g5 = device(45, [
        tap('g5 start', 560, KEY.START, open, { every: 40 }),
        tap('g5 page', 700, KEY.SELECT),
        tap('g5 close', 1100, KEY.START, closed, { every: 60 }),
    ]);
    const hs = [];
    for (let r = 550; r < 1250; r += 29) hs.push(tap('host mash', r, KEY.SELECT | KEY.DOWN, undefined, { every: 1 }));
    hs.push(tap('host start', 1500, KEY.START, open, { every: 40 }));
    hs.push(tap('host close', 1800, KEY.START, closed, { every: 60 }));
    const hd = device(40, hs);
    const pads = { 0: hd, 4: g4, 5: g5 };
    for (const d of [1, 2, 3]) pads[d] = device(40 + d, []);
    const sealed = [];
    const r = await runSession({
        name: 'pause eight', modulePath: MODULE16, rom, scenario: scenarioWith(pads, [{ f: S + 200, type: EV.WARP, a: 0, b: 117, c: 44, d: 9 }]),
        guests: [1, 2, 3, 4, 5].map((dev) => ({ dev })), seats: 8, kirbys: 8, kfCount: 16, link: LINK, durationFrames: 2100,
        hostOpts: { copies: 4, maxPacketBytes: 1100 },
        onHostFrame: (host, f) => sealed.push({ f: f - 1, k: [0, 4, 5].map((s) => sealedKey(host, f - 1, s)) }),
    });
    const slotOf = (d) => d.trace.filter((x) => x.st === 2).map((x) => x.pauser);
    ok('eight: guest 4\'s steps done', !g4.taps.some((t) => t.gaveUp), JSON.stringify(g4.taps.filter((t) => t.gaveUp)));
    ok('eight: guest 5\'s steps done', !g5.taps.some((t) => t.gaveUp), JSON.stringify(g5.taps.filter((t) => t.gaveUp)));
    ok('eight: host\'s steps done', !hd.taps.some((t) => t.gaveUp));
    ok('eight: guest 4 paused as slot 4, guest 5 as slot 5', slotOf(g4).length && slotOf(g4).every((s) => s === 4) && slotOf(g5).length && slotOf(g5).every((s) => s === 5));
    const o4 = spans(g4.trace, 2), o5 = spans(g5.trace, 2), oh = spans(hd.trace, 2);
    ok('eight: guests 4 and 5 paused at once', o4.length === 1 && o5.length === 1 && o5[0][0] < o4[0][1]);
    ok('eight: guest 4 reached SETTINGS and moved its cursor', g4.trace.some((x) => x.st === 2 && x.menu === 8 && x.row === 3));
    ok('eight: guest 4\'s pause opened on SETTINGS (no Select pressed)', !g4.taps.some((t) => t.keys & KEY.SELECT));
    for (const [n, d] of [['guest 4', g4], ['guest 5', g5], ['host', hd]])
        ok(`eight: ${n}'s pages and rows move only after its own presses`, !changesFollowTaps(d, 'menu').length && !changesFollowTaps(d, 'row').length);
    ok('eight: no game paused on any device', Object.values(pads).every((d) => d.trace.every((x) => !x.il)));
    ok('eight: the host showed nothing during 4\'s and 5\'s; 4 and 5 nothing during the host\'s',
       hd.trace.filter((x) => x.f >= o4[0][0] && x.f < o5[0][1]).every((x) => !x.st)
       && [g4, g5].every((d) => d.trace.filter((x) => oh.length && x.f >= oh[0][0] && x.f < oh[0][1]).every((x) => !x.st)));
    ok('eight: slot 4 sealed idle while guest 4\'s overlay is up', sealed.filter((x) => x.f > o4[0][0] + 2 && x.f < o4[0][1] - 2).every((x) => x.k[1] === 0));
    ok('eight: no Start ever sealed', sealed.every((x) => x.k.every((k) => !(k & KEY.START))));
    ok('eight: every compared frame identical', r.compare.mismatched === 0 && r.compare.compared > 5000, `${r.compare.equal}/${r.compare.compared}`);
}

async function caseMemory() {
    if (!fs.existsSync(MODULE16)) { ok('memory: the 16-Kirby node module exists', false, MODULE16); return; }
    const heap = (inst) => { const M = inst.M, p = M._malloc(16); M._PortLnHeapInfo(p); const a = Array.from(M.HEAPU32.subarray(p >> 2, (p >> 2) + 4)); M._free(p); return a; };
    for (const starved of [false, true]) {
        const tag = starved ? 'memory (a guest without room)' : 'memory';
        say(`# ${tag}: the 16-Kirby module, FREE ROAM, 16 keyframes, host + 2 guests`);
        const g1 = device(61, [tap('g1 start', 500, KEY.START, starved ? undefined : open, { every: 40 }),
                               ...(starved ? [] : [tap('g1 close', 800, KEY.START, closed, { every: 60 })])]);
        const hd = device(60, []), g2 = device(62, []);
        const low = { g: 1e12 };
        const r = await runSession({
            name: 'pause ' + tag, modulePath: MODULE16, rom, scenario: scenarioWith({ 0: hd, 1: g1, 2: g2 }, []),
            guests: [{ dev: 1 }, { dev: 2 }], seats: 8, kirbys: 8, kfCount: 16, rooms: true, link: LINK, durationFrames: 1100,
            hostOpts: { copies: 4, maxPacketBytes: 1100 },
            guestPre: (inst, dev) => { if (starved && dev === 1) inst.M._PortLnTestOverlayExtra(512 << 20); },
            onGuestTick: (g) => { const h = heap(g.inst); if (h[3] < low.g) low.g = h[3]; },
        });
        const tels = [r.host.tel, ...r.guestTels];
        if (!starved) {
            ok(`${tag}: every device took the overlay's buffers and kept 4 MiB free`, tels.every((t) => t.ovlHeapFreeKiB >= 4096 && !t.ovlNoMemory),
               tels.map((t) => t.ovlHeapFreeKiB + ' KiB').join(', '));
            ok(`${tag}: guests' heap never under 4 MiB free (shadow runs ${r.guestTels.map((t) => t.canonRuns).join('/')})`, low.g >= 4 << 20, `${(low.g / 1048576).toFixed(1)} MiB`);
            ok(`${tag}: guest 1 paused and closed`, spans(g1.trace, 2).length === 1 && !g1.taps.some((t) => t.gaveUp));
        } else {
            ok(`${tag}: the starved guest runs without the overlay`, r.guestTels[0].ovlHeapFreeKiB >= 0 && r.guestTels[0].ovlNoMemory >= 1 && r.guestTels[0].ovlOpens === 0,
               `noMemory ${r.guestTels[0].ovlNoMemory}, opens ${r.guestTels[0].ovlOpens}`);
            ok(`${tag}: its START opened nothing and paused nobody`, g1.trace.every((x) => !x.st && !x.il) && hd.trace.every((x) => !x.il));
        }
        ok(`${tag}: frames advanced, nothing aborted`, r.guests.every((g) => g.frame >= S + 1000), r.guests.map((g) => g.frame).join(','));
        ok(`${tag}: every compared frame identical`, r.compare.mismatched === 0 && r.compare.compared > 1500, `${r.compare.equal}/${r.compare.compared}`);
    }
}

if (ONLY.includes('memory')) await caseMemory();
if (ONLY.includes('four')) await caseFour();
if (ONLY.includes('repair')) await caseRepair();
if (ONLY.includes('eight')) await caseEight();
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
