// first_diff.mjs -- replay a rooms recording on two views (0 and 1) and print
// the first frames whose gameplay differs, with Kirby 2's unkD9 / idleTimer /
// stateFn / unkC (F4 in ROOMS-REPORT.md).  node first_diff.mjs ROM REC UPTO [VA VB]
// (views VA and VB, default 0 and 1; e.g. 0 2 for a three-device recording)
import fs from 'fs';
import { createInstance } from '../netproof/instance.mjs';
const [rom, rec, upto] = [fs.readFileSync(process.argv[2]), fs.readFileSync(process.argv[3]), +process.argv[4]];
async function dev(view) { return createInstance({ modulePath: new URL('../../build/katam-node.js', import.meta.url).pathname, rom, name: 'v' + view, modulePre(M) {
  const o = M.onRuntimeInitialized; M.onRuntimeInitialized = function () { const p = M._malloc(rec.length + 1); M.HEAPU8.set(rec, p); M._PortInputLogLoad(p, rec.length); M._PortLmpSetView(view); o(); }; } }); }
const A = await dev(+(process.argv[5] ?? 0)), B = await dev(+(process.argv[6] ?? 1));
const parts = (I) => { const p = I.M._PortLmpDigestParts() >> 2; return Array.from(I.M.HEAPU32.subarray(p, p + 5)); };
let reported = 0;
for (let f = 0; f < upto; f++) {
  await A.step(); await B.step();
  const a = parts(A), b = parts(B);
  // parts are chained: report first differing index
  const k = a.findIndex((x, i) => x !== b[i]);
  const ka = [0,1,2,3].map(i => A.kirby(i)), kb = [0,1,2,3].map(i => B.kirby(i));
  const kd = ka.findIndex((x, i) => JSON.stringify(x) !== JSON.stringify(kb[i]));
  if (f > upto - 12) console.log('t', [A,B].map(I=>{const b=0x02020EE0+0x1A8; return [I.M.HEAPU8[b+217], I.M.HEAP16[(b+214)>>1], I.M.HEAPU32[(b+120)>>2].toString(16), I.M.HEAPU32[(b+12)>>2].toString(16)]}), A.frame(), A.observe().rng.toString(16), B.observe().rng.toString(16), JSON.stringify([A.kirby(0),A.kirby(1)].map(x=>[x.anim,x.flags.toString(16),x.hp])), JSON.stringify([B.kirby(0),B.kirby(1)].map(x=>[x.anim,x.flags.toString(16),x.hp])), A.observe().localPlayer, B.observe().localPlayer, A.observe().interlude, B.observe().interlude);
  if ((k >= 0 || kd >= 0 || A.observe().rng !== B.observe().rng) && reported++ < 6) {
    console.log('f', A.frame(), 'part', k, 'kirby', kd, 'rng', A.observe().rng.toString(16), B.observe().rng.toString(16), JSON.stringify(ka.map(x=>[x.x,x.y,x.room])), JSON.stringify(kb.map(x=>[x.x,x.y,x.room])));
    if (reported === 1) { // dump EWRAM+IWRAM diffs
      for (const [base, n] of [[0x02000000, 0x40000], [0x03000000, 0x8000], [0x02040000, 0x400]]) {
        const x = A.M.HEAPU8.subarray(base, base + n), y = B.M.HEAPU8.subarray(base, base + n); let c = 0, l = [];
        for (let i = 0; i < n; i++) if (x[i] !== y[i]) { c++; if (l.length < 0) l.push(i); }
        console.log(' region', base.toString(16), 'bytes differ', c);
      }
    }
  }
  if (reported >= 6) break;
}
process.exit(0);
