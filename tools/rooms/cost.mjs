// cost.mjs -- the two ways to give a guest its canonical instance, measured:
//   (1) a second wasm instance (the prototype's rig): memory = a whole
//       program (its linear memory, the ROM copied in, the game's state);
//   (2) the canonical shadow (shipped, platform/lmpnet.c): one more state
//       image in the same instance, swapped in for a batch of sealed frames.
// Prints RSS per instance as node sees it (process.memoryUsage), the wasm
// memory's size, the shadow's buffers, and the per-frame times of a render-
// off simulated frame, a whole-image save/load and a hash.
//
//   node tools/rooms/cost.mjs --rom ROM [--module build/katam-node.js]
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { createInstance } from '../netproof/instance.mjs';
import { bootKey, BOOT_END } from '../netproof/scenario.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const a = { module: path.join(ROOT, 'build/katam-node.js') };
for (let i = 2; i < process.argv.length; i++) {
    const k = process.argv[i], n = () => process.argv[++i];
    if (k === '--rom') a.rom = n(); else if (k === '--module') a.module = n(); else throw new Error('unknown ' + k);
}
const rom = fs.readFileSync(a.rom);
const mb = (x) => +(x / 1048576).toFixed(1);
const gc = () => { if (global.gc) global.gc(); };
async function boot(name) {
    const I = await createInstance({ modulePath: a.module, rom, name });
    I.M._PortLmpSetHumans(2);
    for (let f = 0; f < BOOT_END; f++) { I.M._PortSetKeys(bootKey(f)); await I.step(); }
    return I;
}
gc();
const r0 = process.memoryUsage();
const A = await boot('A');
gc();
const r1 = process.memoryUsage();
const B = await boot('B');
gc();
const r2 = process.memoryUsage();
const out = {
    wasmMemoryMiB: mb(A.M.HEAPU8.length),
    rssFirstMiB: mb(r1.rss - r0.rss), rssSecondMiB: mb(r2.rss - r1.rss),
    externalSecondMiB: mb(r2.external + r2.arrayBuffers - r1.external - r1.arrayBuffers),
};
// per-frame costs on B: render off, plain local multiplayer
const N = 3000, t0 = performance.now();
for (let i = 0; i < N; i++) { B.M._PortSetKeys(i & 64 ? 16 : 0); await B.step(); }
out.simUsPerFrame = +((performance.now() - t0) * 1000 / N).toFixed(1);
// the shadow's swap: a session guest's save/load of one state, via its telemetry
B.M._PortLnStart(1, 15, 24, 2);         // host with keyframes: the same SaveState
for (let i = 0; i < 600; i++) await B.step();
const tel = B.tel.read();
out.kfBytes = tel.kfBytes;
out.shadowBuffersMiB = mb(2 * (tel.kfBytes + 65536));
out.stateSaveUsAvg = +(tel.kfSaveUsSum / Math.max(1, tel.kfSaved)).toFixed(1);
out.hashUsAvg = +(tel.hashUsSum / Math.max(1, tel.hashed)).toFixed(1);
console.log(JSON.stringify(out, null, 1));
process.exit(0);
