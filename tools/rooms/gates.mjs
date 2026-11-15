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
    else if (k === '--seeds') a.seeds = n().split(',').map(Number);
    else if (k === '--long') a.long = +n();
    else if (k === '--phone-old') a.phoneOld = true;
    else throw new Error('unknown ' + k);
}
const rom = fs.readFileSync(a.rom);
// frames from a call to the check that everyone is in the caller's room
const PHONE_WINDOW = 420;
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
// A door to the hub (OBJ_DOOR_TO_HUB, found by warping into every room and
// listing its objects): standing at it with a person elsewhere is what the
// party HUD's "THE DOOR WAITS FOR nP" hint is for (localmp.c PortLmpWaitingFor).
const HUB_DOOR = [390, 8, 9];

function roomsScenario({ seed, slots, frames, period = 1800, hint = false }) {
    const n = slots.length, end = BOOT_END + frames, ev = [], scripted = new Map(), hints = [];
    const wr = mulberry32(seed * 31 + 7);
    const tap = (slot, f, k, len = 6) => { for (let i = 0; i < len; i++) scripted.set(slot * 1e7 + f + i, k); };
    const quiet = (slot, f, len) => { for (let i = 0; i < len; i++) if (!scripted.has(slot * 1e7 + f + i)) scripted.set(slot * 1e7 + f + i, 0); };
    const calls = [], doors = [], answers = new Map();
    for (let g = BOOT_END + 30, i = 0; g < end - 400; g += period, i++) {
        const order = SPOTS.slice().sort(() => wr() - 0.5);
        for (let s = 0; s < n; s++)
            ev.push({ f: g + s, type: 1, a: s, b: order[s][0], c: order[s][1], d: order[s][2] });
        if (hint && i % 3 === 0) {
            // one person warped to the hub door, standing still, the others elsewhere
            const s0 = i % n, at = g + 200;
            ev.push({ f: at, type: 1, a: s0, b: HUB_DOOR[0], c: HUB_DOOR[1], d: HUB_DOOR[2] });
            quiet(s0, at - 10, 330);
            hints.push({ f: at, slot: s0 });
        }
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
                quiet(s, at - 30, a.phoneOld ? 260 : PHONE_WINDOW + 60);
                if (s === caller) { tap(s, at, KEY.R); if (!a.phoneOld) answers.set(s * 1e7 + at, { from: at + 12, to: at + 90, caller, at, dial: true }); }
                else if (a.phoneOld) tap(s, at + 40, KEY.R);
                else answers.set(s * 1e7 + at, { from: at + 30, to: at + PHONE_WINDOW - 40, caller, at });
            }
            calls.push({ f: at, caller });
        }
    }
    const pads = slots.map((_, s) => human(seed * 10 + s + 1, { phone: 0, start: 0.0002 }));
    // A callee answers the way a person does: while its own screen shows the
    // caller's call ringing (gUnk_02021580, the caller's slot) and it is not
    // in the caller's room, it taps R (6 frames in every 24).  A single tap
    // at a fixed frame (--phone-old) misses whenever the call starts later
    // than the tap (the dialling takes 28-45 frames), the callee is
    // floating, or another answerer in its room froze it (sub_08056C2C ->
    // sub_08033540 freezes everyone in the answerer's room for the answer).
    const answerKey = (s, f, obs) => {
        for (const w of calls) {
            const x = answers.get(s * 1e7 + w.f);
            if (!x || f < x.from || f > x.to) continue;
            const I = obs && obs.inst;
            if (!I) return 0;
            const me = I.kirby(s), ck = I.kirby(x.caller);
            if (x.dial) {
                // the caller, as a person: if no call is ringing and it is not
                // dialling (sub_0805701C), it tries again while it has battery
                const fn = I.peek32(0x02020EE0 + 0x1A8 * s + 0x78) >>> 0, bat = I.peek8(0x02020EE0 + 0x1A8 * s + 0xDC);
                if (I.peek8(0x02021580) === 0xFF && fn !== 0x0805701D && bat > 0 && me.hp > 0)
                    return ((f - x.at) % 12) < 6 ? KEY.R : 0;
                return 0;
            }
            if (I.peek8(0x02021580) === x.caller && me.room !== ck.room && me.hp > 0)
                return ((f - x.at) % 24) < 6 ? KEY.R : 0;
            return 0;
        }
        return null;
    };
    const padOf = (s) => (f, obs) => {
        if (f < BOOT_END) return s === 0 ? bootKey(f) : 0;
        const ans = answerKey(s, f, obs);
        if (ans !== null) return ans;
        const k = scripted.get(s * 1e7 + f);
        return k !== undefined ? k : pads[s](f);
    };
    return {
        end, events: ev.sort((x, y) => x.f - y.f), marks: [['rooms', BOOT_END, end]], localmp: 2, menus: 'off', calls, doors, hints,
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
    // The phone check, on the host (canonical): for each scripted call, what
    // the game did with it over PHONE_WINDOW frames, and why a call that did
    // not gather everyone did not (rule of the game, the script, or neither:
    // "UNEXPLAINED", which is what a bug would look like).
    const fnOf = (host, s) => host.peek32(0x02020EE0 + 0x1A8 * s + 0x78) >>> 0;
    const ANSWER_FNS = new Set([0x0805701D, 0x08058159]);   // sub_0805701C (dialling/answering), sub_08058158 (going to the caller)
    const phoneOpen = new Map();
    const phoneTrack = (host, f, k) => {
        for (const call of sc.calls || []) {
            if (call.f >= cfg.durationFrames + BOOT_END - PHONE_WINDOW) continue;
            if (f === call.f) phoneOpen.set(call.f, { call, rooms0: k.map((x) => x.room), bat: host.peek8(0x02020EE0 + 0x1A8 * call.caller + 0xDC),
                fn0: fnOf(host, call.caller), ring: null, answered: {}, died: {}, fns: {} });
            const t = phoneOpen.get(call.f);
            if (!t || t.done) continue;
            const ring = host.peek8(0x02021580);
            if (process.env.PHONE_DEBUG == call.f && (f - call.f) % 20 === 0) console.error('phone', f, ring, k.map((x) => x.room), [0, 1, 2, 3].slice(0, nSlots).map((s) => fnOf(host, s).toString(16)));
            if (t.ring === null && ring === call.caller) t.ring = f - call.f;
            if (t.ring !== null && t.ringEnd === undefined && ring !== call.caller) { t.ringEnd = f - call.f; t.endFn = fnOf(host, call.caller); t.endHp = k[call.caller].hp; }
            for (let s = 0; s < nSlots; s++) {
                if (s === call.caller) continue;
                if (k[s].hp <= 0) t.died[s] = true;
                const fn = fnOf(host, s);
                if (ring === call.caller && ANSWER_FNS.has(fn) && t.answered[s] === undefined) t.answered[s] = f - call.f;
                if (t.answered[s] === undefined) t.fns[s] = fn;
            }
            const late = f >= call.f + PHONE_WINDOW;
            if (late && (!k.some((x) => x.room === 0xFFFF || x.room < 0) || f >= call.f + PHONE_WINDOW + 60)) {
                t.done = true;
                const cr = k[call.caller].room, met = k.every((x) => x.room === cr);
                const why = [];
                if (met) why.push(new Set(t.rooms0).size === 1 ? 'already together' : 'gathered');
                else if (t.ring === null) why.push(t.bat === 0 ? 'no call: caller battery empty' : `no call: caller could not call (state ${t.fn0.toString(16)})`);
                else for (let s = 0; s < nSlots; s++) if (s !== call.caller && k[s].room !== cr) {
                    if (t.died[s]) why.push(`P${s + 1} died`);
                    else if (t.answered[s] === undefined) why.push(`P${s + 1} never in a state that answers (${t.fns[s].toString(16)})`);
                    else if (t.ringEnd !== undefined && t.endFn !== 0x08057AD1)
                        why.push(`P${s + 1} answered; the call was cut off (the caller left its waiting state sub_08057AD0 for ${t.endFn.toString(16)}${t.endHp <= 0 ? ', dead' : ''})`);
                    else why.push(`UNEXPLAINED: P${s + 1} answered at +${t.answered[s]} and is in ${k[s].room}, the caller in ${cr}`);
                }
                cov.phone.push({ f: call.f, caller: call.caller, met, placed: t.ring !== null, ringAt: t.ring, battery: t.bat, rooms0: t.rooms0,
                                 rooms: k.map((x) => x.room), why: why.join('; ') });
            }
        }
    };
    // The waiting hint: on the device whose screen is the visiting slot's,
    // frames where PortLmpWaitingFor says "the door waits for <someone else>".
    const hintRes = (sc.hints || []).map((h) => ({ f: h.f, slot: h.slot, frames: 0, door: 0, who: null, room: null }));
    const waitBuf = new Map();
    const hintSample = (inst, view, f) => {
        if (!inst.M._PortLmpWaitingFor) return;
        for (const h of hintRes) {
            if (h.slot !== view || f < h.f + 20 || f > h.f + 300) continue;
            let b = waitBuf.get(inst); if (!b) { b = inst.M._malloc(4); waitBuf.set(inst, b); }
            inst.M.HEAP32[b >> 2] = -1;
            const kind = inst.M._PortLmpWaitingFor(b), who = inst.M.HEAP32[b >> 2];
            h.frames++; h.room = inst.kirby(view).room;
            if (kind === 1 && who >= 0 && who !== view) { h.door++; h.who = who; }
        }
    };
    const onHostFrame = (host, f) => {
        hintSample(host, 0, f);
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
        phoneTrack(host, f, k);
        if (k.some((x) => x.room === 0xFFFF || x.room < 0)) return;
        const rooms = new Set(k.map((x) => x.room));
        k.forEach((x) => cov.rooms.add(x.room));
        cov.frames++;
        cov.distinct[rooms.size]++;
        if (rooms.size > 1) cov.apartFrames++; else cov.allTogether++;
        if (cov.lastRooms !== null && rooms.size < cov.lastRooms) cov.meetings++;
        if (cov.lastRooms !== null && rooms.size > cov.lastRooms) cov.partings++;
        cov.lastRooms = rooms.size;
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
        hintSample(inst, tel.view, f);
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
    const ph = cov.phone, placed = ph.filter((p) => p.placed);
    const reasons = {};
    for (const p of ph) if (!p.met || !p.placed) for (const w of p.why.split('; ')) { const key = w.replace(/^P\d /, 'P? ').replace(/\(.*\)|\+\d+|in \d+/g, ''); reasons[key] = (reasons[key] || 0) + 1; }
    out.hints = hintRes;
    if (hintRes.length) console.error(`${name}: waiting hint shown at ${hintRes.filter((h) => h.door > 0).length}/${hintRes.length} hub-door visits (` +
        hintRes.map((h) => `P${h.slot + 1}@${h.f} room ${h.room}: ${h.door}/${h.frames} frames`).join(', ') + ')');
    out.phoneSummary = { calls: ph.length, placed: placed.length, gathered: placed.filter((p) => p.met).length, reasons };
    console.error(`${name}: ${r.compare.equal}/${r.compare.compared} final frames identical; apart ${cov.apartFrames}/${cov.frames}, meetings ${cov.meetings}; ` +
        `phone: ${out.phoneSummary.gathered}/${placed.length} placed calls gathered everyone (${ph.length} scripted${Object.keys(reasons).length ? '; ' + Object.entries(reasons).map(([w, c]) => `${w} x${c}`).join(', ') : ''}); ${gl}; ${out.wallSec.toFixed(0)} s`);
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
    // the host with two local players (its second seat joins after the guest)
    { name: 'hostlocal2', slots: [[0, 0], [1, 0], [0, 1]], groups: [[0, 2]], frames: Q ? 9000 : 18000,
      hostSeats: [[BOOT_END + 200, 1, 1]], guests: [{ dev: 1 }] },
    // the waiting hint at a door to the hub, on the host's and the guest's screen
    { name: 'hint', slots: slotsFor(2), frames: 12000, hint: true, guests: [{ dev: 1 }] },
];
const runs = [];
// --seeds 1,2,3 runs every chosen plan once per seed (seed 1 keeps the
// historical names); --long K multiplies the free-roam runs' length (two,
// three, four, join, local2; corrupt and pool keep their fixed schedules).
const LONG = new Set(['two', 'three', 'four', 'join', 'local2', 'hostlocal2']);
for (const seed of a.seeds || [1]) for (const p0 of plan) {
    if (a.only && !a.only.includes(p0.name)) continue;
    const p = Object.assign({}, p0, { seed, frames: LONG.has(p0.name) && a.long ? Math.round(p0.frames * a.long) : p0.frames });
    const name = seed === 1 ? p.name : `${p.name}-s${seed}`;
    const sc = roomsScenario({ seed, slots: p.slots, frames: p.frames, hint: !!p.hint });
    const r = await roomsRun(name, Object.assign({ scenario: sc, durationFrames: p.frames - 10,
        recordTo: a.recordDir ? path.join(a.recordDir, name + '.txt') : null }, p));
    r.seed = seed; r.plan = p.name;
    runs.push(r);
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
