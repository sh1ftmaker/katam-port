// gates.mjs -- rooms mode online as it ships: the netproof rig's N-device
// session (tools/netproof/run.mjs) with the host's sealed rooms switch, each
// guest running its personal timeline and its canonical shadow in ONE wasm
// instance (platform/lmpnet.c "the canonical shadow").  Writes JSON for
// tools/rooms/report.py and prints a line per run.
//
//   node tools/rooms/gates.mjs --rom ROM --out FILE.json [--quick] [--only a,b]
//                              [--module build/katam-node.js]
//
// Gates (all on a virtual clock, seeded, reproducible):
//   RM1 two, three and four devices apart for long stretches, meeting
//       through doors and the phone: every final frame of every guest's
//       canonical shadow identical to the host's (whole image, no masks);
//       every personal digest equal to the shadow's; each screen on its own
//       player (in its room) whenever that player is in a room.
//   RM2 injected gameplay corruption in either timeline of a guest:
//       detected (shadow: whole-image hash vs the host; personal: digest vs
//       the shadow) and repaired (host image; rebuilt from the shadow).
//   RM3 a guest joins mid-game while the players are apart: on its own
//       player's screen.
//   RM4 a device with two local players: they share its screen -- one room
//       (the one-room rule per device), the shared camera on its anchor.
//   RM5 the cost per guest: the shadow's time and memory, per batch size.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { runSession } from '../netproof/run.mjs';
import { human, bootKey, BOOT_END } from '../netproof/scenario.mjs';
import { mulberry32 } from '../netproof/link.mjs';
import { KEY } from '../netproof/instance.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const a = { quick: false, module: path.join(ROOT, 'build/katam-node.js') };
for (let i = 2; i < process.argv.length; i++) {
    const k = process.argv[i], n = () => process.argv[++i];
    if (k === '--rom') a.rom = n();
    else if (k === '--out') a.out = n();
    else if (k === '--quick') a.quick = true;
    else if (k === '--only') a.only = n().split(',');
    else if (k === '--module') a.module = path.resolve(n());
    else if (k === '--frames') a.frames = +n();
    else if (k === '--record-dir') a.recordDir = n();
    else throw new Error('unknown ' + k);
}
const rom = fs.readFileSync(a.rom);
const LINK = { oneWay: 60, jitter: 30, loss: 0.02, reorder: 0.05, dup: 0.02 };

// Rooms (and a cell next to a door) in several areas, from the recordings.
const SPOTS = [[117, 44, 9], [203, 60, 5], [101, 3, 7], [104, 2, 9], [118, 3, 5], [106, 2, 9],
    [202, 3, 4], [205, 3, 3], [310, 4, 9], [402, 4, 4], [502, 2, 7], [601, 4, 9], [704, 2, 8], [134, 2, 7]];
const DOOR = [117, 44, 9];       // h8:R then Up: the door to 118

// The players apart: every period each person is warped to a room of their
// own; every other period a phone call (one calls with R, the others answer
// with R 40 frames later); every third a meeting by door (two people in 117,
// one takes the door, the other follows 200 frames later).  `slots` maps a
// Kirby slot to its (device, seat); `groups` lists slots that share a device
// (they are warped apart on purpose: the one-room rule must gather them).
function roomsScenario({ seed, slots, frames, period = 1800 }) {
    const n = slots.length, end = BOOT_END + frames, ev = [], scripted = new Map();
    const wr = mulberry32(seed * 31 + 7);
    const tap = (slot, f, k, len = 6) => { for (let i = 0; i < len; i++) scripted.set(slot * 1e7 + f + i, k); };
    const quiet = (slot, f, len) => { for (let i = 0; i < len; i++) if (!scripted.has(slot * 1e7 + f + i)) scripted.set(slot * 1e7 + f + i, 0); };
    const calls = [], doors = [];
    for (let g = BOOT_END + 30, i = 0; g < end - 400; g += period, i++) {
        const order = SPOTS.slice().sort(() => wr() - 0.5);
        for (let s = 0; s < n; s++)
            ev.push({ f: g + s, type: 1, a: s, b: order[s][0], c: order[s][1], d: order[s][2] });
        if (i % 3 === 2 && n >= 2) {
            // a meeting by door: slots 0 and the period's partner into 117
            const p = 1 + (i % (n - 1));
            ev.push({ f: g + 600, type: 1, a: 0, b: DOOR[0], c: DOOR[1], d: DOOR[2] });
            ev.push({ f: g + 601, type: 1, a: p, b: DOOR[0], c: DOOR[1], d: DOOR[2] });
            for (const [s, at] of [[0, g + 660], [p, g + 860]]) {
                quiet(s, at - 60, 60); tap(s, at, KEY.RIGHT, 8);
                for (let u = 0; u < 4; u++) tap(s, at + 14 + 12 * u, KEY.UP);
                quiet(s, at + 70, 60);
            }
            doors.push({ f: g + 600, a: 0, b: p });
        }
        if (i % 2 === 1) {
            const caller = Math.floor(i / 2) % n, at = g + 1300;
            for (let s = 0; s < n; s++) {
                quiet(s, at - 30, 260);
                if (s === caller) tap(s, at, KEY.R); else tap(s, at + 40, KEY.R);
            }
            calls.push({ f: at, caller });
        }
    }
    const pads = slots.map((_, s) => human(seed * 10 + s + 1, { phone: 0, start: 0.0002 }));
    const padOf = (s) => (f) => {
        if (f < BOOT_END) return s === 0 ? bootKey(f) : 0;
        const k = scripted.get(s * 1e7 + f);
        return k !== undefined ? k : pads[s](f);
    };
    return {
        end, events: ev.sort((x, y) => x.f - y.f), marks: [['rooms', BOOT_END, end]], localmp: 2, menus: 'off', calls, doors,
        p1: padOf(0), p2: padOf(1),
        pad: (dev, seat) => { const s = slots.findIndex(([d, t]) => d === dev && t === seat); return s >= 0 ? padOf(s) : null; },
    };
}

const corrupt = {
    rng: { what: 'RNG word (gRngVal ^= 0x5A5A)', addr: 0x030068D8, live: (g) => g.poke32(0x030068D8, (g.peek32(0x030068D8) ^ 0x5A5A) >>> 0),
           canon: (g) => g.M._PortLnCanonPoke(0x030068D8, 0x5A) },
    kirby: { what: "player 2's Kirby x (+16 px)", live: (g) => { const at = 0x02020EE0 + 0x1A8 + 64; g.poke32(at, (g.peek32(at) + (16 << 8)) >>> 0); },
             canon: (g) => g.M._PortLnCanonPoke(0x02020EE0 + 0x1A8 + 65, 0x10) },
    enemy: { what: 'enemy x (+32 px)', live: (g) => { const e = g.findEnemy(); if (e) g.poke32(e.addr + 64, (g.peek32(e.addr + 64) + (32 << 8)) >>> 0); return !!e; } },
    vram: { what: 'VRAM byte (BG char 0x0600C010 ^= 0xFF)', live: (g) => g.poke8(0x0600C010, g.peek8(0x0600C010) ^ 0xFF) },
    // F2: the shared task pool full on this device's own timeline only (a
    // screen crowded with its own effects): gameplay spawns there are lost
    hog: { what: 'task pool full on the personal timeline (F2)', live: (g) => { g.M._PortLmpTestHogTasks(200); return true; } },
    // F3: a CPU brain's object pointer (slot 3's +0x48) turned to another
    // live object on the personal timeline only, as a stale pointer into
    // reused memory would be
    brain: { what: "CPU brain's object pointer redirected (F3)", live: (g) => { const e = g.findEnemy(); if (!e) return false;
        g.poke32(0x02038590 + 3 * 0xF4 + 0x48, e.addr); return true; } },
};

// One run: the session plus the rooms observers.
async function roomsRun(name, cfg) {
    const t0 = Date.now();
    const sc = cfg.scenario, nSlots = cfg.slots.length;
    const cov = { apartFrames: 0, frames: 0, allTogether: 0, meetings: 0, partings: 0, lastRooms: null, distinct: [0, 0, 0, 0, 0],
                  phone: [], doorsMet: 0, groupApart: 0, groupFrames: 0, groupApartMax: 0, groupRun: 0, rooms: new Set() };
    const groups = cfg.groups || [];
    const rec = cfg.recordTo ? { lines: [], next: null } : null;
    const onHostFrame = (host, f) => {
        if (rec) {
            if (rec.next === null) rec.next = f - 1;
            for (; rec.next < f; rec.next++) {
                const x = host.sealedGet(rec.next);
                if (!x) continue;
                const F = rec.next;
                rec.lines.push(`k ${F} ${x.k0.toString(16).toUpperCase()}`);
                [x.k1, x.k2, x.k3].forEach((v, i) => { if (i + 1 < nSlots) rec.lines.push(`kp ${i + 1} ${F} ${v.toString(16).toUpperCase()}`); });
                for (const e of x.ev) if (e[0] === 1) rec.lines.push(`warp ${F} ${e[1]} ${e[2]} ${e[3]} ${e[4]}`);
            }
        }
        if (f < (cfg.joinFrame || BOOT_END) + 60) return;
        const k = [];
        for (let s = 0; s < nSlots; s++) k.push(host.kirby(s));
        if (k.some((x) => x.room === 0xFFFF || x.room < 0)) return;
        const rooms = new Set(k.map((x) => x.room));
        k.forEach((x) => cov.rooms.add(x.room));
        cov.frames++;
        cov.distinct[rooms.size]++;
        if (rooms.size > 1) cov.apartFrames++; else cov.allTogether++;
        if (cov.lastRooms !== null && rooms.size < cov.lastRooms) cov.meetings++;
        if (cov.lastRooms !== null && rooms.size > cov.lastRooms) cov.partings++;
        cov.lastRooms = rooms.size;
        for (const call of sc.calls || []) if (f === call.f + 300 && call.f < cfg.durationFrames + BOOT_END) {
            const met = k.every((x) => x.room === k[call.caller].room);
            cov.phone.push({ f: call.f, caller: call.caller, met, rooms: k.map((x) => x.room) });
        }
        for (const d of sc.doors || []) if (f === d.f + 700) { if (k[d.a].room === k[d.b].room) cov.doorsMet++; }
        for (const grp of groups) {
            cov.groupFrames++;
            const apart = new Set(grp.map((s) => k[s].room)).size > 1;
            if (apart) { cov.groupApart++; cov.groupRun++; cov.groupApartMax = Math.max(cov.groupApartMax, cov.groupRun); }
            else cov.groupRun = 0;
        }
    };
    // per guest: the screen, and the corruption bookkeeping
    const scr = {};
    const corr = [];
    const onGuestTick = (g) => {
        if (!g.gs.joined || !g.inst) return;
        const inst = g.inst, tel = inst.tel.read(), f = inst.frame();
        const s = scr[g.name] || (scr[g.name] = { checks: 0, wrong: 0, pending: 0, list: [], last: -1, quietUntil: 0, imgs: 0, reps: 0, viewSlots: new Set() });
        if (tel.imagesLoaded !== s.imgs || tel.persRepairs !== s.reps) { s.imgs = tel.imagesLoaded; s.reps = tel.persRepairs; s.quietUntil = f + 120; }
        if (f !== s.last) {
            s.last = f;
            const o = inst.observe(), v = tel.view, kb = inst.kirby(v);
            const inRoom = kb.room !== 0xFFFF && !(kb.flags & 0x100) && kb.hp > 0;
            const ok = o.localPlayer === v && (!inRoom || o.room === kb.room);
            s.viewSlots.add(v);
            if (f < s.quietUntil || !inRoom) { if (!ok) s.pending++; }
            else { s.checks++; if (!ok) { s.wrong++; if (s.list.length < 12) s.list.push({ f, lp: o.localPlayer, v, room: o.room, kroom: kb.room }); } }
        }
        for (const cz of cfg.corruptRooms || []) {
            if (g.dev !== (cz.dev || 1) || g.gen) continue;
            if (!cz.done && f >= cz.frame) {
                const ok = cz.target === 'canon' ? cz.c.canon(inst) : cz.c.live(inst);
                cz.done = true;
                const r = { what: cz.c.what, target: cz.target, dev: g.dev, frame: f, t: g.inst ? Date.now() : 0, vt: null, tel0: tel, applied: ok !== 0 && ok !== false, at: null };
                r.vt = cfg._sch ? cfg._sch() : null;
                corr.push(r); cz.rec = r;
            }
            const r = cz.rec;
            if (r && r.detectF === undefined) {
                const det = r.target === 'canon' ? tel.mismatches > r.tel0.mismatches : tel.digMismatch > r.tel0.digMismatch;
                if (det) r.detectF = f;
            }
            if (r && r.detectF !== undefined && r.repairF === undefined) {
                const rep = r.target === 'canon' ? tel.imagesLoaded > r.tel0.imagesLoaded : tel.persRepairs > r.tel0.persRepairs;
                if (rep) r.repairF = f;
            }
        }
    };
    const r = await runSession(Object.assign({ name, modulePath: a.module, rom, seed: cfg.seed || 1, menus: 'off', rooms: cfg.rooms !== false,
        link: LINK, startFrame: BOOT_END, onHostFrame, onGuestTick, progress: (m) => console.error(m) }, cfg));
    if (rec) {
        const boot = [];
        for (let f = 0, prev = -1; f < BOOT_END; f++) { const k = sc.p1(f, null); if (k !== prev) { boot.push(`k ${f} ${k.toString(16).toUpperCase()}`); prev = k; } }
        fs.writeFileSync(cfg.recordTo, ['katam-input 1', '# a rooms-mode session as the host sealed it (tools/rooms/gates.mjs); no-regress',
            `frames ${rec.next}`, `option localmp ${nSlots}`, `option roomsat ${BOOT_END}`, ...boot, ...rec.lines, 'end'].join('\n') + '\n');
    }
    const guests = r._G.filter((g) => g.inst).map((g) => {
        const tel = g.inst.tel.read(), gr = r.guests.find((x) => x.name === g.name) || {};
        const mins = Math.max(1e-9, gr.minutes || 0);
        return {
            name: g.name, dev: g.dev, compare: gr.compare, screen: scr[g.name] ? Object.assign({}, scr[g.name], { viewSlots: [...scr[g.name].viewSlots] }) : null,
            digest: { compared: tel.digCompared, equal: tel.digEqual, mismatched: tel.digMismatch, first: tel.digFirstMismatch, upTo: tel.digUpTo },
            canon: { frame: tel.canonFrame, runs: tel.canonRuns, frames: tel.canonFrames, usSum: tel.canonUsSum, usMax: tel.canonUsMax,
                     swapUsSum: tel.canonSwapUsSum, perMinUs: Math.round(tel.canonUsSum / mins), perFrameUs: tel.canonFrames ? +(tel.canonUsSum / tel.canonFrames).toFixed(2) : null,
                     swapPerRunUs: tel.canonRuns ? +(tel.canonSwapUsSum / tel.canonRuns).toFixed(1) : null },
            logs: g.inst.logs.filter((l) => /rooms:|differs/.test(l)).slice(-12),
            persRepairs: tel.persRepairs, images: tel.imagesLoaded, mismatches: tel.mismatches, rewinds: tel.rewinds, reviews: g.inst.M._PortLmpReviews(),
            stepMs: gr.stepMs, stepSumMs: +g.stepMs.reduce((x, y) => x + y, 0).toFixed(1), minutes: gr.minutes, kfBytes: tel.kfBytes, kfCount: tel.kfCount,
            ear: (g.inst.audioStats() || {}).ear || null, sounds: { judged: tel.soundsJudged, refire: tel.soundsRefire, spurious: tel.soundsSpurious, missed: tel.soundsMissed },
        };
    });
    const hostTel = r.host.tel;
    const out = {
        name, wallSec: (Date.now() - t0) / 1000, frames: r.frames, compare: r.compare, mismatches: r.mismatches.slice(0, 10),
        guests, owners: r.owners, churn: r.churn,
        coverage: Object.assign({}, cov, { rooms: [...cov.rooms].sort((x, y) => x - y), lastRooms: undefined, groupRun: undefined,
                                            phoneCalls: (sc.calls || []).length, doorMeetings: (sc.doors || []).length, phoneUsed: r.coverage.phone }),
        corrupt: corr.map((x) => ({ what: x.what, target: x.target, dev: x.dev, frame: x.frame, applied: x.applied, detectFrames: x.detectF !== undefined ? x.detectF - x.frame : null,
                                    repairFrames: x.repairF !== undefined ? x.repairF - x.frame : null })),
        hostStepMs: r.host.stepMs, hostImages: hostTel.imagesSaved, aborted: r.aborted || null,
        hostLogs: r.hostLogs.filter((l) => /rooms|lmpnet/.test(l)).slice(-8),
    };
    const gl = guests.map((g) => `${g.name}: canon ${g.compare.equal}/${g.compare.compared}, digest ${g.digest.equal}/${g.digest.compared}` +
        `, screen wrong ${g.screen ? g.screen.wrong : '?'}/${g.screen ? g.screen.checks : '?'}, views ${g.screen ? g.screen.viewSlots : ''}, repairs ${g.persRepairs}, images ${g.images}` +
        `, shadow ${g.canon.perFrameUs} us/f`).join('; ');
    console.error(`${name}: ${r.compare.equal}/${r.compare.compared} final frames identical; apart ${cov.apartFrames}/${cov.frames}, meetings ${cov.meetings}; ${gl}; ${out.wallSec.toFixed(0)} s`);
    return out;
}

const Q = a.quick;
const slotsFor = (n) => [[0, 0], [1, 0], [2, 0], [3, 0]].slice(0, n);
const plan = [
    { name: 'two', slots: slotsFor(2), frames: a.frames || (Q ? 9000 : 30000), guests: [{ dev: 1 }] },
    { name: 'three', slots: slotsFor(3), frames: Q ? 9000 : 24000, guests: [{ dev: 1 }, { dev: 2 }] },
    { name: 'four', slots: slotsFor(4), frames: Q ? 9000 : 24000, guests: [{ dev: 1 }, { dev: 2 }, { dev: 3 }] },
    { name: 'corrupt', slots: slotsFor(3), frames: 9000, guests: [{ dev: 1 }, { dev: 2 }], lockstep: true,
      corruptRooms: [
          { target: 'canon', frame: 2600, c: corrupt.rng }, { target: 'live', frame: 3400, c: corrupt.rng },
          { target: 'live', frame: 4300, c: corrupt.kirby }, { target: 'canon', frame: 5200, c: corrupt.kirby },
          { target: 'live', frame: 6100, c: corrupt.enemy }, { target: 'live', frame: 7000, c: corrupt.vram },
          { target: 'live', frame: 7900, c: corrupt.rng, dev: 2 }] },
    { name: 'pool', slots: slotsFor(3), frames: 6000, guests: [{ dev: 1 }, { dev: 2 }], lockstep: true,
      corruptRooms: [{ target: 'live', frame: 2400, c: corrupt.hog }, { target: 'live', frame: 4200, c: corrupt.brain, dev: 2 }] },
    { name: 'join', slots: slotsFor(3), frames: 9000, guests: [{ dev: 1 }, { dev: 2, join: BOOT_END + 3100 }] },
    { name: 'local2', slots: [[0, 0], [1, 0], [1, 1]], groups: [[1, 2]], frames: Q ? 9000 : 18000,
      guests: [{ dev: 1, seats: [[BOOT_END + 200, 1, 1]] }] },
];
const runs = [];
for (const p of plan) {
    if (a.only && !a.only.includes(p.name)) continue;
    const sc = roomsScenario({ seed: 1, slots: p.slots, frames: p.frames });
    runs.push(await roomsRun(p.name, Object.assign({ scenario: sc, durationFrames: p.frames - 10,
        recordTo: a.recordDir ? path.join(a.recordDir, p.name + '.txt') : null }, p)));
}
// RM5: the shadow's cost by batch size (two devices, 9000 frames)
if (!a.only || a.only.includes('cost')) {
    // as the page runs it: the guest hashes every 16th frame (flags 0)
    for (const [batch, rooms] of [[4, true], [16, true], [64, true], [16, false]]) {
        const sc = roomsScenario({ seed: 2, slots: slotsFor(2), frames: 9000 });
        const r = await roomsRun(`cost-${rooms ? 'b' + batch : 'together'}`, { scenario: sc, slots: slotsFor(2), durationFrames: 8990, guests: [{ dev: 1 }],
            guestFlags: 0, rooms, guestPre: (inst) => inst.M._PortLnSetCanonBatch(batch) });
        r.batch = batch; r.roomsOn = rooms;
        runs.push(r);
    }
}
fs.writeFileSync(a.out, JSON.stringify({ when: new Date().toISOString(), module: a.module, runs }, null, 1));
process.exit(0);
