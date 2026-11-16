#!/usr/bin/env node
// A 16-player online game on the 16-Kirby module, wasm in node: the host and
// 15 guests (16 devices, one person each) through the engine and the page's
// own session code (web/ln_session.js HostHub / HostSession / GuestSession),
// with what web/online.js gives a 16-player room: seats 16, 32 keyframes,
// bounded redundancy (copies 4, packets at most 1100 B).
//
//   node tools/netproof/sixteen_test.mjs [--rom ROM] [--module16 build/w16/katam-node.js]
//        [--cases together,freeroam] [--frames N] [--devices N]
//
// Each case: the game starts with the host and guests 1..7; guests 8..15
// join one by one mid-game, into seats past the live count (4 Kirbys at
// power-on, as on a fresh file: each such Kirby is made, then seated); then
// everyone plays (a scripted human per device) until the end.  Checks: no
// device trapped, every person seated (16 people at once), the Kirby count
// reached 16, every guest's every compared frame identical with the host's,
// and the frames compared cover the game.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { runSession } from './run.mjs';
import { buildScenario, kirbyAddrOf } from './scenario.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(here, '../..');
const argv = process.argv.slice(2);
const flag = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 ? argv[i + 1] : d; };
const ROM = path.resolve(flag('rom', process.env.ROM || '/bigdata/agent-tom/projects/katamport/decomp/baserom.gba'));
const MODULE16 = path.resolve(flag('module16', path.join(ROOT, 'build/w16/katam-node.js')));
const CASES = flag('cases', 'together').split(',');
const FRAMES = +flag('frames', 2400);
const N = +flag('devices', 16);
const LINK = { oneWay: 60, jitter: 15, loss: 0.02 };

let pass = 0, fail = 0;
const ok = (name, c, extra) => {
    if (c) pass++; else fail++;
    console.log(`  ${c ? 'ok  ' : 'FAIL'} ${name}${extra !== undefined ? '  (' + extra + ')' : ''}`);
    return c;
};

const rom = fs.readFileSync(ROM);
for (const mode of CASES) {
    const tag = `${N} devices ${mode}`;
    console.log(`# ${tag}: ${FRAMES} frames @${LINK.oneWay}ms one way, jitter ${LINK.jitter}, loss ${LINK.loss * 100}%`);
    if (!fs.existsSync(MODULE16)) { ok(`${tag}: the 16-Kirby node module exists`, false, MODULE16); continue; }
    const S = 1300;                     // scenario.mjs BOOT_END: the session's first frame
    const guests = Array.from({ length: N - 1 }, (_, i) => {
        const dev = i + 1;
        return dev < 8 ? { dev } : { dev, join: S + 200 + 60 * (dev - 8) };
    });
    let err = null, r = null, kMax = 0;
    const t0 = Date.now();
    try {
        r = await runSession({
            name: tag, modulePath: MODULE16, rom, rooms: mode === 'freeroam',
            scenario: buildScenario({ seed: 5, skipScripted: true, humanFrames: FRAMES + 600, kirbyAddr: kirbyAddrOf(16) }),
            guests, seats: 16, kirbys: 4, kfCount: 32, link: LINK, durationFrames: FRAMES,
            hostOpts: { copies: 4, maxPacketBytes: 1100 },
            onHostFrame: (h) => { const k = h.M._PortPocKirbysNow(); if (k > kMax) kMax = k; },
        });
    } catch (e) { err = e; }
    ok(`${tag}: no device trapped`, !err, err && String(err.stack || err).split('\n').slice(0, 4).join(' | '));
    if (err) continue;
    const people = r.owners.filter((o) => o >= 0).length;
    console.log(`  info ${tag}: wall ${((Date.now() - t0) / 1000).toFixed(1)} s, host frame ${r.frames.hostFinal}, owners ${r.owners.map((o) => (o < 0 ? 'cpu' : `${o >> 2}.${o & 3}`)).join(' ')}, ` +
                `Kirbys at most ${kMax}, seat waits ${JSON.stringify(r.seatWaits || [])}`);
    ok(`${tag}: ${N} people at once`, r.coverage.humansMax === N && people === N, `most ${r.coverage.humansMax}, at the end ${people}`);
    ok(`${tag}: the Kirby count rose to ${N}`, kMax >= N, String(kMax));
    const bad = r.guests.filter((g) => g.compare.mismatched !== 0 || g.compare.compared < g.compare.expected * 0.9);
    ok(`${tag}: every guest's every compared frame identical with the host's`, r.compare.mismatched === 0 && !bad.length,
       `${r.compare.equal}/${r.compare.compared}` + (bad.length ? '; ' + bad.map((g) => `${g.name} ${g.compare.equal}/${g.compare.compared} of ${g.compare.expected}`).join(', ') : ''));
    console.log(`  info ${tag}: per guest rewinds/min ${r.guests.map((g) => g.feel.rewindsPerMin).join(' ')}; replayed frames/s ${r.guests.map((g) => g.feel.replayedFramesPerSec).join(' ')}; ` +
                `images ${r.guestTels.map((t) => t.imagesLoaded).join(' ')}`);
}
console.log(`sixteen: ${pass} ok, ${fail} FAIL`);
process.exit(fail ? 1 : 0);
