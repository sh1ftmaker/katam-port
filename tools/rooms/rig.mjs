// rig.mjs -- rooms mode online, design (c) of ROOMS-REPORT.md, in the netproof
// rig: host + guest canonical + guest personal over the simulated link
// (tools/rooms/rooms_run.mjs).  Writes JSON for tools/rooms/report.py.
//
//   node tools/rooms/rig.mjs --rom ROM --out FILE.json [--quick]
//
// Runs (all on a virtual clock, seeded, reproducible):
//   apart     tests/recordings/rooms_apart.txt through a 120 ms link: apart,
//             a door meeting, phone meetings both ways, pause apart, a boss
//             room and the goal room with the other player elsewhere;
//   wander    tests/recordings/rooms_wander.txt: 9000 frames apart, phone;
//   free      human-like play, both players warped to different rooms on
//             their own clocks, phone calls both ways, ~11 min of game;
//   corrupt   gameplay corruption of the canonical instance (detected by the
//             whole-image hash against the host, repaired by the host image)
//             and of the personal instance (detected by the gameplay digest
//             against the canonical instance, repaired by a host image and a
//             re-view), plus a personal VRAM byte (view state: not detected,
//             by design);
//   join      the guest joins mid-game while the players are apart: the
//             personal instance gets the host's image (the host's screen) and
//             must end on player 2's.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { runSession } from './rooms_run.mjs';
import { loadRecording, human, bootKey, BOOT_END, ROOMS, EV } from '../netproof/scenario.mjs';
import { mulberry32 } from '../netproof/link.mjs';
import { KEY } from '../netproof/instance.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const a = { quick: false };
for (let i = 2; i < process.argv.length; i++) {
    const k = process.argv[i], n = () => process.argv[++i];
    if (k === '--rom') a.rom = n();
    else if (k === '--out') a.out = n();
    else if (k === '--quick') a.quick = true;
    else if (k === '--only') a.only = n().split(',');
    else if (k === '--record-dir') a.recordDir = n();
    else throw new Error('unknown ' + k);
}
const rom = fs.readFileSync(a.rom);
const modulePath = path.join(ROOT, 'build/katam-node.js');
const LINK = { oneWay: 60, jitter: 30, loss: 0.02, reorder: 0.05, dup: 0.02 };

function freeScenario(seed, frames) {
    const h1 = human(seed * 2 + 1, { phone: 0.0006 }), h2 = human(seed * 2 + 2, { phone: 0.0006 });
    const wr = mulberry32(seed * 31 + 7);
    const ev = [], calls = [];
    const end = BOOT_END + frames;
    for (let g = BOOT_END + 60, i = 0; g < end; g += 1800, i++) {
        const r1 = ROOMS[Math.floor(wr() * ROOMS.length)];
        let r2 = ROOMS[Math.floor(wr() * ROOMS.length)];
        if (r2[0] === r1[0]) r2 = ROOMS[(ROOMS.indexOf(r2) + 1) % ROOMS.length];
        ev.push({ f: g, type: EV.WARP, a: 0, b: r1[0], c: r1[1], d: r1[2] });
        ev.push({ f: g + 700, type: EV.WARP, a: 1, b: r2[0], c: r2[1], d: r2[2] });
        // a phone meeting every other period, alternating who calls
        if (i % 2 === 1) calls.push([g + 1300, i % 4 === 1 ? 0 : 1]);
    }
    const callKey = (p, f) => {
        for (const [cf, who] of calls) {
            if (f >= cf && f < cf + 6 && who === p) return KEY.R;            // call
            if (f >= cf + 40 && f < cf + 46 && who !== p) return KEY.R;      // answer
            if (f >= cf && f < cf + 160) return 0;                            // hands off meanwhile
        }
        return null;
    };
    return {
        end, events: ev.sort((x, y) => x.f - y.f), marks: [['free-apart', BOOT_END, end]], localmp: 2, menus: 'off',
        p1: (f) => { if (f < BOOT_END) return bootKey(f); const c = callKey(0, f); return c !== null ? c : h1(f); },
        p2: (f) => { if (f < BOOT_END) return 0; const c = callKey(1, f); return c !== null ? c : h2(f); },
    };
}

const corruptRng = (who) => ({ what: 'RNG word (gRngVal ^= 0x5A5A)', apply: (g) => { g.poke32(0x030068D8, (g.peek32(0x030068D8) ^ 0x5A5A) >>> 0); } });
const corruptEnemy = { what: 'enemy x (+32 px)', apply: (g) => { const e = g.findEnemy(); if (e) { g.poke32(e.addr + 64, (g.peek32(e.addr + 64) + (32 << 8)) >>> 0); g.lastCorrupt = e; } } };
const corruptKirby = { what: "player 2's Kirby x (+16 px)", apply: (g) => { const at = 0x02020EE0 + 0x1A8 + 64; g.poke32(at, (g.peek32(at) + (16 << 8)) >>> 0); } };
const corruptVram = { what: 'VRAM byte (BG char 0x0600C010 ^= 0xFF)', apply: (g) => { g.poke8(0x0600C010, g.peek8(0x0600C010) ^ 0xFF); } };

const runs = [];
const rec = (f) => loadRecording(path.join(ROOT, 'tests/recordings', f), 'off');
const plan = [
    { name: 'apart', scenario: rec('rooms_apart.txt'), startFrame: BOOT_END, link: LINK, durationFrames: 8600 - BOOT_END - 10 },
    { name: 'wander', scenario: rec('rooms_wander.txt'), startFrame: BOOT_END, link: LINK, durationFrames: 11200 - BOOT_END - 10 },
    { name: 'free', scenario: freeScenario(3, a.quick ? 9000 : 40000), startFrame: BOOT_END, link: LINK,
      durationFrames: a.quick ? 9000 : 40000 },
    { name: 'corrupt', scenario: rec('rooms_apart.txt'), startFrame: BOOT_END, link: LINK, durationFrames: 8600 - BOOT_END - 10, lockstep: true,
      corrupt: [Object.assign({ frame: 2000 }, corruptRng()), Object.assign({ frame: 3300 }, corruptEnemy)],
      corruptPersonal: [Object.assign({ frame: 2400 }, corruptRng()), Object.assign({ frame: 3500 }, corruptEnemy),
                        Object.assign({ frame: 4300 }, corruptKirby), Object.assign({ frame: 6000 }, corruptVram)] },
    { name: 'join', scenario: rec('rooms_apart.txt'), startFrame: BOOT_END, joinFrame: 3300, link: LINK,
      durationFrames: 8600 - BOOT_END - 10 },
];
for (const p of plan) {
    if (a.only && !a.only.includes(p.name)) continue;
    const t0 = Date.now();
    const r = await runSession(Object.assign({ modulePath, rom, seed: 1, menus: 'off', progress: (m) => console.error(m),
        recordTo: a.recordDir ? path.join(a.recordDir, p.name + '.txt') : null }, p));
    const out = {
        name: p.name, wallSec: (Date.now() - t0) / 1000, frames: r.frames, compare: r.compare,
        mismatches: r.mismatches.slice(0, 20),
        personal: Object.assign({}, r.personal, { compare: Object.assign({}, r.personal.compare) }),
        apart: { frames: r.apart.frames, total: r.apart.total, meetings: r.apart.meetings, partings: r.apart.partings,
                 rooms1: [...r.apart.rooms1].sort((x, y) => x - y), rooms2: [...r.apart.rooms2].sort((x, y) => x - y) },
        corrupt: r.corrupt.map((x) => ({ what: x.what, frame: x.frame, personal: !!x.personal, t: x.t,
                                         detectMs: x.detectT !== undefined ? +(x.detectT - x.t).toFixed(1) : null,
                                         repairMs: x.repairT !== undefined ? +(x.repairT - x.t).toFixed(1) : null })),
        guest: { images: r.guest.session.images, repairReqs: r.guest.session.repairReqs, mismatches: r.guest.tel.mismatches,
                 rewinds: r.guest.tel.rewinds, imagesLoaded: r.guest.tel.imagesLoaded, stepMs: r.guest.stepMs },
        host: { images: r.host.session.images, repairReqs: r.host.session.repairReqs, stepMs: r.host.stepMs },
        coverage: { phone: r.coverage.phone, pauses: r.coverage.pauses, deaths: r.coverage.deaths, bossFrames: r.coverage.bossFrames,
                    warps: r.coverage.warps },
        feel: { rewindsPerMin: r.feel.rewindsPerMin, worstGuestStepMs: r.feel.worstGuestStepMs },
        hostLogs: r.hostLogs.slice(-5), guestLogs: r.guestLogs.slice(-5), aborted: r.aborted || null,
    };
    delete out.personal.compare.list;
    out.personal.compare.firstList = (r.personal.compare.list || []).slice(0, 10);
    runs.push(out);
    const pcmp = out.personal.compare;
    console.error(`${p.name}: canonical ${r.compare.equal}/${r.compare.compared} identical; personal digest ${pcmp.equal}/${pcmp.compared} ` +
                  `(${pcmp.mismatched} mismatched, ${pcmp.repairsAsked} repairs asked, ${out.personal.images.length} images); ` +
                  `apart ${out.apart.frames}/${out.apart.total} frames, ${out.apart.meetings} meetings; view wrong ${pcmp.viewWrong}/${pcmp.viewChecks}; ` +
                  `reviews ${out.personal.reviews}; ${out.wallSec.toFixed(1)} s`);
}
fs.writeFileSync(a.out, JSON.stringify({ when: new Date().toISOString(), runs }, null, 1));
process.exit(0);
