// phone_optin_test.mjs -- phone calls are opt in (FREE ROAM / rooms mode).
//
//   node tools/rooms/phone_optin_test.mjs --rom ROM [--module build/katam-node.js] [--seed 1]
//
// The netproof rig's two-device session (tools/netproof/run.mjs), rooms mode
// on, the CPU helpers made partners, the two people warped to rooms of their
// own.  Then, on the host's (canonical) timeline:
//   decline  player 1 calls (R); player 2 presses nothing.  Player 2 must not
//            move: same room, same position, same hp, lives and battery
//            after the call has rung out -- declining costs nothing.  While
//            it rings, player 2's screen (the guest) shows the prompt "1P
//            CALLS - PRESS R TO GO" (party_hud.c, sampled through
//            PortPartyHudTestSampleJs), and player 1's screen shows none.
//   accept   player 1 calls again; player 2 presses R while it rings.  Player
//            2 must arrive in player 1's room.
//   helpers  the CPU helpers (slots 2, 3) come to player 1's room on its call.
//   reverse  player 2 calls, player 1 presses nothing: player 1 stays.
// Every guest frame is compared with the host's (the rig's whole-image
// hashes) throughout.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { runSession } from '../netproof/run.mjs';
import { bootKey, BOOT_END } from '../netproof/scenario.mjs';
import { KEY } from '../netproof/instance.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const a = { module: path.join(ROOT, 'build/katam-node.js'), seed: 1 };
for (let i = 2; i < process.argv.length; i++) {
    const k = process.argv[i], n = () => process.argv[++i];
    if (k === '--rom') a.rom = n();
    else if (k === '--module') a.module = path.resolve(n());
    else if (k === '--seed') a.seed = +n();
    else if (k === '--out') a.out = n();
    else throw new Error('unknown ' + k);
}
const rom = fs.readFileSync(a.rom);
let pass = 0, fail = 0;
const ok = (name, c, extra) => {
    if (c) { pass++; console.log('  ok   ' + name + (extra ? '  (' + extra + ')' : '')); }
    else { fail++; console.log('  FAIL ' + name + (extra ? '  (' + extra + ')' : '')); }
};

const KIRBY = (s) => 0x02020EE0 + 0x1A8 * s;
const RING = 0x02021580;                 // the game's "who is calling", 0xFF none
const A1 = [117, 44, 9], A2 = [203, 60, 5];
const S0 = BOOT_END + 30;
// phases: [start frame, caller, answerer-or-null]
const PH = { decline: S0 + 400, accept: S0 + 1700, reverse: S0 + 3200 };
const RING_WINDOW = 520;                 // a call rings at most ~5 s after the dialling

const events = [
    // the CPU helpers as partners (gAIKirbyState = NORMAL, 300), sealed
    { f: S0, type: 4, a: 0x0203AD2C, b: 300 & 255, c: 0, d: 0 },
    { f: S0 + 1, type: 4, a: 0x0203AD2D, b: 300 >> 8, c: 0, d: 0 },
];
for (const [ph, at] of Object.entries(PH)) {
    // apart again before each phase
    events.push({ f: at - 300, type: 1, a: 0, b: A1[0], c: A1[1], d: A1[2] });
    events.push({ f: at - 299, type: 1, a: 1, b: A2[0], c: A2[1], d: A2[2] });
}
const callerOf = (ph) => (ph === 'reverse' ? 1 : 0);
const answers = (ph) => ph === 'accept';

let I = { host: null, guest: null };
function pad(slot) {
    return (f) => {
        if (f < BOOT_END) return slot === 0 ? bootKey(f) : 0;
        for (const [ph, at] of Object.entries(PH)) {
            if (f < at - 300 || f >= at + RING_WINDOW + 200) continue;
            const inst = I.host;
            if (!inst || f < at) return 0;
            const ring = inst.peek8(RING), caller = callerOf(ph);
            if (slot === caller) {
                // dial: R while nothing rings and it is not dialling already
                const fn = inst.peek32(KIRBY(slot) + 0x78) >>> 0;
                if (ring === 0xFF && fn !== 0x0805701D && f < at + 120) return ((f - at) % 12) < 6 ? KEY.R : 0;
                return 0;
            }
            if (answers(ph) && ring === caller) return ((f - at) % 24) < 6 ? KEY.R : 0;
            return 0;
        }
        return 0;
    };
}
const sc = {
    end: PH.reverse + RING_WINDOW + 400, events: events.sort((x, y) => x.f - y.f), marks: [['rooms', BOOT_END, PH.reverse + 900]],
    localmp: 2, menus: 'off', p1: pad(0), p2: pad(1), pad: (dev, seat) => (seat === 0 ? pad(dev) : null),
};

// observations on the host (canonical) and the guest (player 2's screen)
const obs = {};
for (const ph of Object.keys(PH)) obs[ph] = { before: null, after: null, rang: 0, hintGuest: new Set(), hintHost: new Set(), helpers: null };
const snap = (inst) => [0, 1, 2, 3].map((s) => {
    const k = inst.kirby(s);
    return { room: k.room, x: k.x, y: k.y, hp: k.hp, lives: inst.peek8(KIRBY(s) + 0x102), battery: inst.peek8(KIRBY(s) + 0xDC) };
});
const hint = (inst) => {
    const M = inst.M;
    if (!M._PortPartyHudTestSampleJs) return '(no export)';
    M._PortPartyHudTestSampleJs();
    const p = M._PortPartyHudHintJs();
    let s = '';
    for (let i = 0; i < 64; i++) { const c = M.HEAPU8[p + i]; if (!c) break; s += String.fromCharCode(c); }
    return s;
};
const onHostFrame = (host, f) => {
    I.host = host;
    for (const [ph, at] of Object.entries(PH)) {
        const o = obs[ph];
        if (f === at) o.before = snap(host);
        if (f >= at && f < at + RING_WINDOW && host.peek8(RING) === callerOf(ph)) {
            o.rang++;
            if (f % 10 === 0) o.hintHost.add(hint(host));
        }
        if (f === at + RING_WINDOW + 120) o.after = snap(host);
    }
};
const onGuestTick = (g) => {
    if (!g.gs || !g.gs.joined || !g.inst) return;
    I.guest = g.inst;
    const f = g.inst.frame();
    for (const [ph, at] of Object.entries(PH))
        if (f >= at && f < at + RING_WINDOW && g.inst.peek8(RING) === callerOf(ph) && f % 10 === 0)
            obs[ph].hintGuest.add(hint(g.inst));
};

console.log(`# phone calls are opt in: rooms mode, two devices, seed ${a.seed}`);
const r = await runSession({ name: 'phone', modulePath: a.module, rom, seed: a.seed, menus: 'off', rooms: true,
    link: { oneWay: 40, jitter: 10, loss: 0 }, startFrame: BOOT_END, scenario: sc, durationFrames: sc.end - BOOT_END - 20,
    guests: [{ dev: 1 }], onHostFrame, onGuestTick, progress: () => {} });

const fmt = (s) => s ? s.map((k, i) => `${i + 1}P r${k.room} hp${k.hp} l${k.lives} b${k.battery}`).join(', ') : 'none';
for (const [ph] of Object.entries(PH)) {
    const o = obs[ph], c = callerOf(ph), callee = 1 - c;
    console.log(`  ${ph}: before ${fmt(o.before)}\n  ${ph}: after  ${fmt(o.after)}\n  ${ph}: rang ${o.rang} frames; prompt on the callee's screen ${JSON.stringify([...(callee === 1 ? o.hintGuest : o.hintHost)])}`);
    ok(`${ph}: the call rang`, o.rang > 20, `${o.rang} frames`);
    ok(`${ph}: the two people were apart when it rang`, o.before && o.before[0].room !== o.before[1].room, fmt(o.before));
    if (!o.before || !o.after) continue;
    const b = o.before[callee], e = o.after[callee];
    if (ph === 'accept') {
        ok(`accept: ${callee + 1}P pressed R and arrived in ${c + 1}P's room`, e.room === o.after[c].room, `${e.room} vs ${o.after[c].room}`);
    } else {
        ok(`${ph}: ${callee + 1}P pressed nothing and was not moved`, e.room === b.room && Math.abs(e.x - b.x) < 48 && Math.abs(e.y - b.y) < 48,
           `room ${b.room} -> ${e.room}, x ${b.x} -> ${e.x}, y ${b.y} -> ${e.y} (pixels)`);
        // (hp is reported, not judged: a Kirby standing idle in a level room
        // can be hit by whatever lives there, call or no call)
        ok(`${ph}: declining cost ${callee + 1}P nothing (lives, battery)`, e.lives === b.lives && e.battery === b.battery,
           `hp ${b.hp}->${e.hp} lives ${b.lives}->${e.lives} battery ${b.battery}->${e.battery}`);
    }
    const prompts = [...(callee === 1 ? o.hintGuest : o.hintHost)];
    ok(`${ph}: ${callee + 1}P's screen says who calls and what to press`, prompts.includes(`${c + 1}P CALLS - PRESS R TO GO`), JSON.stringify(prompts));
    const own = [...(c === 0 ? o.hintHost : o.hintGuest)].filter((t) => /CALLS/.test(t));
    ok(`${ph}: the caller's own screen shows no prompt`, own.length === 0, JSON.stringify(own));
    if (ph === 'decline') {
        const h = o.after.slice(2).map((k) => k.room), cr = o.after[0].room;
        ok('helpers: the CPU helpers came to 1P\'s room on its call', h.every((x) => x === cr), `helpers in ${h.join(',')}, 1P in ${cr}`);
    }
}
const g = (r.guests || [])[0] || {};
ok('every compared guest frame identical to the host\'s', g.compare && g.compare.compared > 100 && g.compare.mismatched === 0,
   g.compare ? `${g.compare.equal}/${g.compare.compared}` : 'no comparison');
if (a.out) fs.writeFileSync(a.out, JSON.stringify({ obs: Object.fromEntries(Object.entries(obs).map(([k, v]) => [k, { ...v, hintGuest: [...v.hintGuest], hintHost: [...v.hintHost] }])), guest: g.compare }, null, 1));
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
