// What the two players do.  A scenario is, per player, a function from
// (frame, what that player's own screen shows) to the pad, plus a list of
// host-side events by frame (warps, view changes, pokes).  Everything is a
// deterministic function of the seed and of the device's own state, so a run
// repeats exactly; a player reacting to its own (possibly predicted) screen
// is what a person does.
import fs from 'fs';
import { mulberry32 } from './link.mjs';
import { KEY } from './instance.mjs';

const M = { A: 1, B: 2, s: 4, S: 8, R: 16, L: 32, U: 64, D: 128, r: 256, l: 512 };
export const EV = { WARP: 1, VIEW: 2, LMP: 3, POKE: 4 };
export const BOOT_END = 1300;
export const SCRIPTED_END = 10100;
export const ROOMS = [[101, 3, 7], [104, 2, 9], [106, 2, 9], [202, 3, 4], [205, 3, 3], [310, 4, 9],
    [402, 4, 4], [502, 2, 7], [601, 4, 9], [704, 2, 8], [113, 89, 7], [203, 59, 5], [325, 4, 20],
    [118, 3, 5], [225, 13, 7], [134, 2, 7], [415, 77, 1], [617, 4, 9]];
const KIRBY = 0x02020EE0, KSIZE = 0x1A8, HP = 0x100;

// The power-on to first-level taps the committed recordings use.
export function bootKey(f) { return f >= 120 && (f - 120) % 20 < 10 ? 0x11 : 0; }

// drive.py's grammar (local-mp-eval): @F +N taps hN:K warp:ROOM:X:Y end.
// Taps here are held 6 frames (not 4): a tap must survive a late packet.
function parseScript(script, f0, keys, warps, player) {
    let f = f0;
    const mask = (s) => [...s].reduce((m, ch) => m | M[ch], 0);
    for (const tok of script.split(/\s+/).filter(Boolean)) {
        if (tok.startsWith('@')) f = +tok.slice(1);
        else if (tok.startsWith('+')) f += +tok.slice(1);
        else if (tok.startsWith('shot:')) { /* screenshots: ignored */ }
        else if (tok.startsWith('warp:')) { const [, r, x, y] = tok.split(':').map(Number); warps.push({ f, type: EV.WARP, a: player, b: r, c: x, d: y }); f += 1; }
        else if (tok.startsWith('h') && tok.includes(':')) { const [n, k] = tok.slice(1).split(':'); keys.push([f, mask(k)], [f + +n, 0]); f += +n + 6; }
        else if (tok === 'end') break;
        else { keys.push([f, mask(tok)], [f + 6, 0]); f += 12; }
    }
    return f;
}

function table(pairs) {
    pairs.sort((a, b) => a[0] - b[0]);
    return (f) => {
        let lo = 0, hi = pairs.length - 1, v = 0;
        while (lo <= hi) { const m = (lo + hi) >> 1; if (pairs[m][0] <= f) { v = pairs[m][1]; lo = m + 1; } else hi = m - 1; }
        return v;
    };
}

// Human-like: held directions (biased right), jumps and floats, inhale /
// attack holds, taps of A and B, Up at doors now and then, a rare L/R (the
// cell phone) and a rare Start (the pause menu, closed again by B later).
export function human(seed, opts = {}) {
    const rnd = mulberry32(seed * 7919 + 13);
    const o = Object.assign({ start: 0.0004, phone: 0.0004, up: 0.006 }, opts);
    let until = 0, dir = 0, jumpUntil = 0, bUntil = 0, aTap = 0, upUntil = 0, extra = 0, extraUntil = 0, last = -1, held = 0;
    return (f) => {
        if (f === last) return held;
        last = f;
        if (f >= until) {
            const r = rnd();
            dir = r < 0.45 ? KEY.RIGHT : r < 0.68 ? KEY.LEFT : r < 0.76 ? KEY.UP : r < 0.82 ? KEY.DOWN : 0;
            until = f + 6 + Math.floor(rnd() * 40);
        }
        if (f >= jumpUntil && rnd() < 0.03) jumpUntil = f + 4 + Math.floor(rnd() * 24);
        if (f >= bUntil && rnd() < 0.012) bUntil = f + 3 + Math.floor(rnd() * (rnd() < 0.3 ? 40 : 6));
        if (f >= aTap && rnd() < 0.01) aTap = f + 6;
        if (f >= upUntil && rnd() < o.up) upUntil = f + 8;
        if (f >= extraUntil) {
            extra = 0;
            const r = rnd();
            if (r < o.start) { extra = KEY.START; extraUntil = f + 6; }
            else if (r < o.start + o.phone) { extra = KEY.R; extraUntil = f + 6; }
            else if (r < o.start + o.phone + 0.002) { extra = KEY.B; extraUntil = f + 6; }  // also leaves a pause
        }
        let k = dir | (f < jumpUntil ? KEY.A : 0) | (f < bUntil ? KEY.B : 0) | (f < aTap && f >= aTap - 3 ? KEY.A : 0)
            | (f < upUntil ? KEY.UP : 0) | (f < extraUntil ? extra : 0);
        held = k & 0x3FF;
        return held;
    };
}

// A reactive pause bot: open the pause menu, walk the Select cycle to the
// SETTINGS page (menuId 8), change a value and change it back, leave with B.
function pauseBot(at, seedKeys) {
    let state = 'idle', t = 0, selects = 0, lastPress = -100;
    const tap = (f, k) => { lastPress = f; return k; };
    return (f, obs) => {
        if (f < at || state === 'done') return null;
        if (f - lastPress < 14) return f - lastPress < 6 ? seedKeys : 0;   // hold a tap 6 frames, then release
        switch (state) {
        case 'idle': state = 'opening'; t = f; seedKeys = KEY.START; return tap(f, KEY.START);
        case 'opening':
            if (obs.interlude && f - t > 60) { state = 'cycle'; t = f; }
            if (f - t > 400) { state = 'done'; return 0; }
            return 0;
        case 'cycle':
            if (obs.menuId === 8) { state = 'set'; t = f; return 0; }
            if (selects > 6 || f - t > 900) { state = 'leave'; return 0; }
            if (f - t > 50 * (selects + 1)) { selects++; seedKeys = KEY.SELECT; return tap(f, KEY.SELECT); }
            return 0;
        case 'set': if (f - t > 50) { state = 'set2'; t = f; seedKeys = KEY.DOWN; return tap(f, KEY.DOWN); } return 0;
        case 'set2': if (f - t > 30) { state = 'set3'; t = f; seedKeys = KEY.RIGHT; return tap(f, KEY.RIGHT); } return 0;
        case 'set3': if (f - t > 40) { state = 'set4'; t = f; seedKeys = KEY.LEFT; return tap(f, KEY.LEFT); } return 0;
        case 'set4': if (f - t > 40) { state = 'leave'; t = f; } return 0;
        case 'leave':
            if (!obs.interlude) { state = 'done'; return 0; }
            if (f - t > 30) { t = f; seedKeys = KEY.B; return tap(f, KEY.B); }
            return 0;
        }
        return 0;
    };
}

// Walk to the nearest enemy that gives an ability, inhale (hold B), swallow
// (Down); again until player 1 has an ability.  obs.me / obs.enemy come from
// the device's own memory (tools/netproof/run.mjs).
function abilityBot(from, to) {
    let phase = 'approach', t = 0, lastX = null, stuck = 0;
    return (f, obs) => {
        if (f < from || f >= to) return null;
        if (!obs || !obs.me) return 0;
        if (obs.ability && obs.ability[0]) return 0;
        const e = obs.enemy;
        if (phase === 'inhale') { if (f - t < 50) return KEY.B; phase = 'swallow'; t = f; }
        if (phase === 'swallow') { if (f - t < 12) return KEY.DOWN; phase = 'approach'; t = f; }
        stuck = lastX === obs.me.x ? stuck + 1 : 0;
        lastX = obs.me.x;
        if (!e) return KEY.RIGHT | (stuck > 15 && (f & 8) ? KEY.A : 0);
        const dx = e.x - obs.me.x, dy = e.y - obs.me.y;
        const dir = dx > 0 ? KEY.RIGHT : KEY.LEFT;
        const jump = (stuck > 15 || dy < -16) && (f & 8) ? KEY.A : 0;
        if (Math.abs(dx) > 36 || Math.abs(dy) > 20) return dir | jump;
        if (f - t < 4) return dir;            // face it
        phase = 'inhale'; t = f;
        return KEY.B;
    };
}

// The full scenario.  `humanFrames` of free play follow the scripted part.
export function buildScenario({ seed = 1, humanFrames = 36000, skipScripted = false } = {}) {
    const k1 = [], k2 = [], ev = [], marks = [];
    let f = BOOT_END;
    const p1b = [], p2b = [];
    if (!skipScripted) {
        // A. doors in two areas, led by each player, apart and together in
        //    between (local-mp-eval doors3.sh), so the straggler rule carries.
        parseScript(`@1300 warp:117:44:9 @1340 h8:R U U U U @1440 h140:R @1590 h140:L
                     @1950 warp:203:60:5 @2060 h30:L U U U U`, 0, k1, ev, 0);
        parseScript(`@1750 h30:L U U U U @1990 h10:R U U U U @2150 h120:L`, 0, k2, ev, 1);
        marks.push(['doors', 1300, 2400]);
        // B. a boss: Kracko's arena (rooms from tools/make_recordings.py AIM),
        //    both players fighting; player 2 at 1 HP to make a death certain.
        ev.push({ f: 2450, type: EV.WARP, a: 0, b: 324, c: 4, d: 19 });
        ev.push({ f: 3300, type: EV.POKE, a: KIRBY + KSIZE + HP, b: 1, c: 0, d: 0 });
        marks.push(['boss', 2450, 5000]);
        f = 5000;
        // C. pause by player 1, then by player 2, in a room with map pages.
        ev.push({ f: 5000, type: EV.WARP, a: 0, b: 117, c: 44, d: 9 });
        p1b.push(pauseBot(5100, 0));
        p2b.push(pauseBot(6400, 0));
        marks.push(['pause-p1', 5100, 6300], ['pause-p2', 6400, 7700]);
        // D. the cell phone, each player (R calls the CPU helpers).
        k2.push([7750, KEY.R], [7760, 0]);
        k1.push([8150, KEY.R], [8160, 0]);
        marks.push(['phone', 7750, 8300]);
        // E. the session view changes (a VIEW event), wide and back.
        ev.push({ f: 8100, type: EV.VIEW, a: 480, b: 320, c: 0, d: 0 });
        // G. abilities: player 1 walks to an enemy that gives one, inhales
        //    and swallows it (a reactive bot), through three rooms.
        ev.push({ f: 8300, type: EV.WARP, a: 0, b: 104, c: 2, d: 9 });
        ev.push({ f: 8900, type: EV.WARP, a: 0, b: 106, c: 2, d: 9 });
        ev.push({ f: 9500, type: EV.WARP, a: 0, b: 202, c: 3, d: 4 });
        p1b.push(abilityBot(8360, SCRIPTED_END));
        marks.push(['ability', 8300, SCRIPTED_END]);
        f = SCRIPTED_END;
    }
    const scriptedEnd = f;
    const h1 = human(seed * 2 + 1), h2 = human(seed * 2 + 2);
    const freeEnd = scriptedEnd + humanFrames;
    // F. free play: both players human-like, a warp every 40 s through level
    //    rooms, the view toggled now and then.
    const wr = mulberry32(seed * 31 + 7);
    for (let g = scriptedEnd + 60, i = 0; g < freeEnd; g += 2400, i++) {
        const r = ROOMS[Math.floor(wr() * ROOMS.length)];
        ev.push({ f: g, type: EV.WARP, a: 0, b: r[0], c: r[1], d: r[2] });
        if (i % 3 === 1) ev.push({ f: g + 900, type: EV.VIEW, a: i % 2 ? 240 : 480, b: i % 2 ? 160 : 320, c: 0, d: 0 });
    }
    marks.push(['free', scriptedEnd, freeEnd]);
    const t1 = table(k1), t2 = table(k2);
    const bots = (list, f, obs) => { for (const b of list) { const v = b(f, obs); if (v !== null && v !== undefined) return v; } return null; };
    return {
        end: freeEnd, marks, events: ev.sort((a, b) => a.f - b.f),
        p1: (f, obs) => { if (f < BOOT_END) return bootKey(f); if (f >= scriptedEnd) return h1(f);
                          const b = bots(p1b, f, obs || {}); return b !== null ? b : t1(f); },
        p2: (f, obs) => { if (f < BOOT_END) return 0; if (f >= scriptedEnd) return h2(f);
                          const b = bots(p2b, f, obs || {}); return b !== null ? b : t2(f); },
    };
}

// A recorded local-multiplayer session (inputlog text: k, kp 1, warp, view,
// option localmp).  Player 1 is the host's pad, kp 1 the guest's.
export function loadRecording(file, menus) {
    const k1 = [], k2 = [], ev = [];
    let frames = 0, localmp = 0;
    for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
        const p = line.trim().split(/\s+/);
        if (p[0] === 'k') k1.push([+p[1], parseInt(p[2], 16)]);
        else if (p[0] === 'kp' && p[1] === '1') k2.push([+p[2], parseInt(p[3], 16)]);
        else if (p[0] === 'warp') ev.push({ f: +p[1], type: EV.WARP, a: +p[2], b: +p[3], c: +p[4], d: +p[5] });
        else if (p[0] === 'view') ev.push({ f: +p[1], type: EV.VIEW, a: +p[2], b: +p[3], c: 0, d: 0 });
        else if (p[0] === 'frames') frames = +p[1];
        else if (p[0] === 'option' && p[1] === 'localmp') localmp = +p[2];
    }
    const t1 = table(k1), t2 = table(k2);
    const txt = fs.readFileSync(file, 'utf8');
    menus = menus || (/KATAM_MENU_POC|menus? on/i.test(txt) ? 'boot' : 'off');
    return { end: frames, events: ev.sort((a, b) => a.f - b.f), marks: [['recording', 0, frames]], localmp, recorded: file, menus,
             p1: (f) => t1(f), p2: (f) => t2(f) };
}
