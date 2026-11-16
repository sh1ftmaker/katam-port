#!/usr/bin/env node
// L in a level: the cell phone's warp star back to the hub, online, on the
// 16-Kirby module -- a rig gate on the engine (wasm in node, the session
// logic of the page: tools/netproof/run.mjs).
//
//   node tools/netproof/lwarp_test.mjs [--rom ROM] [--module16 build/w16/katam-node.js]
//        [--only rooms,shared] [--slot N --ability A [--rooms 0]]   (one rider: a probe)
//
// The ride's sprites take a per-player variant (`+ unk56`, kirby.c
// sub_0803F790 and the ability effects in code_0806F780.c), and the ROM has
// four: a Kirby 4.. holding an ability ran off the end of its hat's
// animation into the next one's data -- garbage command pointers, and the
// page's wasm trapped ("memory access out of bounds" at PortRlEmitSprite or
// in the animator) for everyone in the room.  Kirby's own star has eight
// variants, so without an ability 4..7 only drew the wrong star; 8.. trapped
// too (tests/wide_lwarp.sh covers those, natively).
//
// Cases (8 humans, host + 7 guests, every device building the GPU renderer's
// render list as the page's ENHANCED renderer does):
//   rooms   rooms mode.  Each of slots 4, 5, 6, 7, 0 (the host) and 2 is
//           warped into a level room of its own, given an ability by a sealed
//           poke (fire, ice, the six-variant one, none for 7), and holds L:
//           every one rides the star (animation 90) and lands in the hub
//           (room 801), the rides overlapping, while the others play on.  No
//           device traps, no sprite is ever turned away by the renderer's
//           safety net (rl_build.c BadFrame), every compared frame identical.
//   shared  one shared screen (no rooms mode): the host is warped into a
//           level, everyone with it; slot 5 takes an ability and L-warps home
//           (the others follow it there, as a shared screen does).
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { runSession } from './run.mjs';
import { buildScenario, human, BOOT_END, EV, kirbyAddrOf } from './scenario.mjs';
import { KEY } from './instance.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(here, '../..');
const argv = process.argv.slice(2);
const flag = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 ? argv[i + 1] : d; };
const ROM = flag('rom', process.env.ROM || '/bigdata/agent-tom/projects/katamport/decomp/baserom.gba');
const MODULE16 = path.resolve(flag('module16', path.join(ROOT, 'build/w16/katam-node.js')));
const ONLY = flag('only', 'rooms,shared').split(',');
const rom = fs.readFileSync(ROM);
const S = BOOT_END;                 // the session's first frame (run.mjs startFrame)
const LINK = { oneWay: 50, jitter: 15, loss: 0.01 };
const HUB = 801, KSIZE = 0x1A8, ABILITY = 0x103, RIDE = 90;
const KIRBY = kirbyAddrOf(16);
const LEVELS = [[104, 2, 9], [202, 3, 4], [101, 3, 7], [402, 4, 4], [502, 2, 7], [601, 4, 9], [704, 2, 8], [310, 4, 9]];

let pass = 0, fail = 0;
const ok = (name, c, extra) => {
    if (c) pass++; else fail++;
    console.log(`  ${c ? 'ok  ' : 'FAIL'} ${name}${extra !== undefined ? '  (' + extra + ')' : ''}`);
    return c;
};
// a trap inside a frame surfaces from the module's own callbacks, not from
// runSession's promise: a FAIL all the same
process.on('uncaughtException', (e) => {
    console.log(`  FAIL a device trapped: ${String(e.stack || e).split('\n').slice(0, 3).join(' | ')}`);
    console.log(`\n${pass} passed, ${fail + 1} failed`);
    process.exit(1);
});
const t0 = Date.now();
const say = (t) => console.log(`[lwarp ${((Date.now() - t0) / 1000).toFixed(0)}s] ${t}`);

// riders: [{ slot, ability, at, warp }] -- warped into a level at S+at
// (warp false: not warped; `lead` warps the host there instead, everyone
// following on a shared screen), hands off for 90 frames before S+at+300,
// given the ability 4 frames before it (a sealed poke: late, so no enemy
// knocks it out first), then L held: the press is an edge the
// game sees only in some states (idle, walking, ...; kirby.c sub_08056E40's
// callers) and the call is dropped if L is let go before the star comes
// (sub_080573D0), so three holds of 140 frames, 4 apart.  Everyone else
// plays as a person, never pressing L or Start.
const HOLD = 140, GAP = 4, TRIES = 3;
async function session(name, riders, rooms, lead = null) {
    const quiet = (seed) => { const h = human(seed, { start: 0, phone: 0 }); return (f) => h(f) & ~(KEY.L | KEY.START); };
    const pads = {}, events = [];
    for (let d = 0; d < 8; d++) pads[d] = quiet(70 + d);
    if (lead) events.push({ f: S + 100, type: EV.WARP, a: 0, b: lead[0], c: lead[1], d: lead[2] });
    riders.forEach((r, i) => {
        const room = lead || LEVELS[i % LEVELS.length], p = quiet(80 + r.slot), hold = S + r.at + 300;
        r.room = room[0]; r.holdAt = hold;
        if (!lead) events.push({ f: S + r.at, type: EV.WARP, a: r.slot, b: room[0], c: room[1], d: room[2] });
        if (r.ability) events.push({ f: hold - 4, type: EV.POKE, a: KIRBY + r.slot * KSIZE + ABILITY, b: r.ability, c: 0, d: 0 });
        pads[r.slot] = (f) => {
            const k = f - hold;
            if (k >= 0 && k < TRIES * (HOLD + GAP)) return k % (HOLD + GAP) < HOLD ? KEY.L : 0;
            return k >= -90 && k < 0 ? 0 : p(f);
        };
    });
    events.sort((a, b) => a.f - b.f);
    const sc = buildScenario({ skipScripted: true, humanFrames: 1 });
    sc.events = events;
    sc.pad = (dev, seat) => (seat === 0 && pads[dev] ? pads[dev] : null);
    const end = Math.max(...riders.map((r) => r.holdAt)) - S + TRIES * (HOLD + GAP) + 400;
    const tr = riders.map(() => []);
    const insts = [];
    let r = null, err = null;
    try {
        r = await runSession({
            name, modulePath: MODULE16, rom, scenario: sc, rooms, storyNormal: true, presentChecks: true,
            guests: [1, 2, 3, 4, 5, 6, 7].map((dev) => ({ dev })), seats: 8, kirbys: 8, kfCount: 16,
            link: LINK, durationFrames: end, hostOpts: { copies: 4, maxPacketBytes: 1100 },
            hostPre: (h) => insts.push(h), guestPre: (g) => insts.push(g),
            onHostFrame: (host, f) => riders.forEach((rd, i) => {
                const k = host.kirby(rd.slot);
                tr[i].push({ f, room: k.room, anim: k.anim, ab: host.peek8(KIRBY + rd.slot * KSIZE + ABILITY) });
            }),
        });
    } catch (e) { err = e; }
    ok(`${name}: no device trapped`, !err, err && String(err.stack || err).split('\n').slice(0, 4).join(' | '));
    if (err) return;
    for (const [i, rd] of riders.entries()) {
        const t = tr[i], who = `slot ${rd.slot}${rd.ability ? ` (ability ${rd.ability})` : ''}`;
        const before = t.filter((x) => x.f >= rd.holdAt - 20 && x.f < rd.holdAt);
        ok(`${name}: ${who} was in its level room ${rd.room} when it pressed L`,
           before.length && before.every((x) => x.room === rd.room), JSON.stringify(before.slice(-1)));
        const rode = t.find((x) => x.f >= rd.holdAt && x.anim === RIDE && x.room === rd.room);
        ok(`${name}: ${who} rode the warp star (animation ${RIDE})${rd.ability ? ' with its ability' : ''}`,
           rode && (!rd.ability || rode.ab === rd.ability), rode && `from f${rode.f - S}, ability ${rode.ab}`);
        const home = t.find((x) => x.f >= rd.holdAt && x.room === HUB);
        ok(`${name}: ${who} landed in the hub (room ${HUB})`, !!home,
           home ? `${home.f - rd.holdAt} frames after L` : JSON.stringify(t[t.length - 1]));
    }
    const nets = insts.flatMap((x) => x.logs.filter((l) => / not drawn -- /.test(l)).map((l) => `${x.name}: ${l}`));
    ok(`${name}: the renderer's safety net never fired (every sprite's frame was the ROM's)`, !nets.length, nets.slice(0, 3).join(' | '));
    ok(`${name}: every compared frame identical, on every guest`, r.compare.mismatched === 0 && r.compare.compared > 7 * (end - 100),
       `${r.compare.equal}/${r.compare.compared}`);
    ok(`${name}: no repairs`, r.guestTels.every((t) => t.imagesLoaded <= 1), r.guestTels.map((t) => t.imagesLoaded).join(','));
}

if (!fs.existsSync(MODULE16)) {
    ok('the 16-Kirby node module exists', false, MODULE16);
} else if (flag('slot', null) !== null) {
    say(`# probe: slot ${flag('slot')} ability ${flag('ability', 0)}`);
    await session('probe', [{ slot: +flag('slot'), ability: +flag('ability', 0), at: 100 }], flag('rooms', '1') === '1');
} else {
    if (ONLY.includes('rooms')) {
        say('# rooms: slots 4, 5, 6, 7, 0, 2 each L-warp from a level of their own, rooms mode');
        await session('rooms', [
            { slot: 4, ability: 1, at: 100 }, { slot: 5, ability: 1, at: 160 }, { slot: 6, ability: 9, at: 220 },
            { slot: 7, ability: 0, at: 280 }, { slot: 0, ability: 2, at: 340 }, { slot: 2, ability: 1, at: 400 },
        ], true);
    }
    if (ONLY.includes('shared')) {
        say('# shared: slot 5, one screen');
        await session('shared', [{ slot: 5, ability: 2, at: 100 }], false, [104, 2, 9]);
    }
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
