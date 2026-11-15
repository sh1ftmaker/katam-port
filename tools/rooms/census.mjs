// census.mjs -- which TASK TYPES differ in number between two devices that
// show different players (rooms mode), with the gameplay the same?  Those
// are view-only by measurement; every other task type must exist the same
// number of times on both, and the gameplay digest counts them
// (platform/localmp.c PortLmpDigestNow, part "tasks").
//
//   node tools/rooms/census.mjs --rom ROM --rec REC[,REC2...] [--views 0,1] [--out FILE.json]
//
// Two instances replay each recording with rooms mode on, one per view; per
// frame the multiset of task mains (PortLmpTaskMains) of each is compared.
// Prints every main whose count ever differed, with frames and peak, and the
// digest comparison (it must never differ).
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { createInstance } from '../netproof/instance.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const a = { views: [0, 1], module: path.join(ROOT, 'build/katam-node.js') };
for (let i = 2; i < process.argv.length; i++) {
    const k = process.argv[i], n = () => process.argv[++i];
    if (k === '--rom') a.rom = n();
    else if (k === '--rec') a.rec = n().split(',');
    else if (k === '--views') a.views = n().split(',').map(Number);
    else if (k === '--out') a.out = n();
    else if (k === '--module') a.module = n();
    else throw new Error('unknown ' + k);
}
const rom = fs.readFileSync(a.rom);
async function dev(name, view, text) {
    return createInstance({ modulePath: a.module, rom, name, modulePre(M) {
        const orig = M.onRuntimeInitialized;
        M.onRuntimeInitialized = function () {
            const p = M._malloc(text.length + 1);
            M.HEAPU8.set(text, p);
            M.recFrames = M._PortInputLogLoad(p, text.length);
            M._free(p);
            if (!/option roomsat/.test(text.toString())) M._PortLmpSetRooms(1);
            M._PortLmpSetView(view);
            orig();
        };
    } });
}
const census = (I, buf) => { const n = I.M._PortLmpTaskMains(buf, 256); const m = new Map(); for (let i = 0; i < n; i++) { const v = I.M.HEAPU32[(buf >> 2) + i]; m.set(v, (m.get(v) || 0) + 1); } return m; };
const all = new Map();
const out = { runs: [] };
for (const rec of a.rec) {
    const text = fs.readFileSync(rec);
    const A = await dev('A', a.views[0], text), B = await dev('B', a.views[1], text);
    const ba = A.M._malloc(1024), bb = B.M._malloc(1024);
    let digestDiff = 0, frames = 0, poolMax = [0, 0];
    for (let f = 0; f < A.M.recFrames; f++) {
        await A.step(); await B.step();
        const ca = census(A, ba), cb = census(B, bb);
        let na = 0, nb = 0; for (const v of ca.values()) na += v; for (const v of cb.values()) nb += v;
        poolMax = [Math.max(poolMax[0], na), Math.max(poolMax[1], nb)];
        for (const k of new Set([...ca.keys(), ...cb.keys()])) {
            const d = (ca.get(k) || 0) - (cb.get(k) || 0);
            if (!d) continue;
            const e = all.get(k) || { main: '0x' + k.toString(16), frames: 0, peak: 0, first: null, recs: new Set() };
            e.frames++; e.peak = Math.max(e.peak, Math.abs(d)); if (e.first === null) e.first = path.basename(rec) + '@' + f; e.recs.add(path.basename(rec));
            all.set(k, e);
        }
        const da = A.M._PortLmpDigestAt(A.frame()) >>> 0, db = B.M._PortLmpDigestAt(B.frame()) >>> 0;
        if (da && db && da !== db) digestDiff++;
        frames++;
    }
    out.runs.push({ rec: path.basename(rec), frames, digestDiff, poolMax });
    console.error(`${path.basename(rec)}: ${frames} frames, digest differs ${digestDiff}, task pool peak ${poolMax.join('/')} of 128`);
}
out.viewOnly = [...all.values()].map((e) => Object.assign(e, { recs: [...e.recs] })).sort((x, y) => y.frames - x.frames);
for (const e of out.viewOnly) console.error(`  ${e.main}: differs on ${e.frames} frames, by up to ${e.peak}, first ${e.first}`);
if (a.out) fs.writeFileSync(a.out, JSON.stringify(out, null, 1));
process.exit(0);
