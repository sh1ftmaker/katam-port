// review_test.mjs -- does the re-view (platform/localmp.c PortLmpReview) put a
// device on the right screen?
//
//   node tools/rooms/review_test.mjs --rom ROM --rec REC --at F[,F2...] [--view 1]
//        [--frames N] [--settle 30] [--out FILE.json]
//
// Three instances replay REC with rooms mode on, rendering:
//   R   shows player `view` from power-on (the device that was always there);
//   J   shows player 1 (slot 0) until frame F, then is told it is player
//       `view`'s device (PortLmpSetView) -- what loading another device's
//       image amounts to: the gameplay is the same, the screen is someone
//       else's;
//   S   shows player 1 throughout (control: what J would show without it).
// Per frame: the rendered picture's hash (pix, platform/web/host_web.c) of
// each, the bytes of VRAM, palette RAM, OAM and the I/O block that differ
// between J and R, and the gameplay probes (Kirbys, RNG, objects) of J and R.
// For each F: frames until J's picture equals R's, and whether it then stays
// equal; whether gameplay ever differs.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { createInstance } from '../netproof/instance.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const a = { at: [], view: 1, settle: 30, frames: 0, module: path.join(ROOT, 'build/katam-node.js') };
for (let i = 2; i < process.argv.length; i++) {
    const k = process.argv[i], n = () => process.argv[++i];
    if (k === '--rom') a.rom = n();
    else if (k === '--rec') a.rec = n();
    else if (k === '--at') a.at = n().split(',').map(Number);
    else if (k === '--view') a.view = +n();
    else if (k === '--frames') a.frames = +n();
    else if (k === '--settle') a.settle = +n();
    else if (k === '--out') a.out = n();
    else if (k === '--dump') { a.dump = n().split(',').map(Number); }
    else if (k === '--dump-dir') a.dumpDir = n();
    else throw new Error('unknown argument ' + k);
}
const rom = fs.readFileSync(a.rom), text = fs.readFileSync(a.rec);

async function dev(name, view) {
    const st = { pix: 0, frame: -1 };
    const inst = await createInstance({
        modulePath: a.module, rom, name, render: true,
        modulePre(M) {
            const orig = M.onRuntimeInitialized;
            M.onRuntimeInitialized = function () {
                const p = M._malloc(text.length + 1);
                M.HEAPU8.set(text, p);
                M.recFrames = M._PortInputLogLoad(p, text.length);
                M._free(p);
                M._PortLmpSetRooms(1);
                M._PortLmpSetView(view);
                M._PortSetFrameStateLog(1);
                M.portFrameState = (frame, keys, sim, vid, sram, pix) => { st.pix = pix >>> 0; st.frame = frame; };
                orig();
            };
        },
    });
    inst.st = st;
    return inst;
}

function diffBytes(A, B, base, n) {
    const x = Buffer.from(A.M.HEAPU8.buffer, base, n), y = Buffer.from(B.M.HEAPU8.buffer, base, n);
    if (x.equals(y)) return 0;
    let d = 0;
    for (let i = 0; i < n; i++) if (x[i] !== y[i]) d++;
    return d;
}

function probe(I) {
    const k = [0, 1, 2, 3].map((i) => I.kirby(i));
    const o = I.observe();
    return JSON.stringify([k.map((q) => [q.x, q.y, q.room, q.hp]), o.rng]);
}

const R = await dev('R', a.view), J = await dev('J', 0), S = await dev('S', 0);
const total = a.frames || R.M.recFrames;
const rows = [];
const at = new Set(a.at);
let gameplayDiffer = null;
for (let f = 0; f < total; f++) {
    if (at.has(f)) J.M._PortLmpSetView(a.view);
    if (at.has(f + 1) && f > 0) { /* nothing */ }
    await R.step(); await J.step(); await S.step();
    const fr = R.frame();
    const row = { f: fr, rPix: R.st.pix, jPix: J.st.pix, sPix: S.st.pix,
                  vram: diffBytes(J, R, 0x06000000, 0x18000), pltt: diffBytes(J, R, 0x05000000, 0x400),
                  oam: diffBytes(J, R, 0x07000000, 0x400), io: diffBytes(J, R, 0x04000000, 0x400),
                  rooms: [R.kirby(0).room, R.kirby(a.view).room], jLocal: J.observe().localPlayer };
    if (probe(J) !== probe(R) && gameplayDiffer === null) gameplayDiffer = fr;
    if (a.dump && a.dump.includes(fr)) {
        const REG = [[0x02000000, 0x40000], [0x03000000, 0x8000], [0x04000000, 0x400], [0x05000000, 0x400],
                     [0x06000000, 0x18000], [0x07000000, 0x400], [0x02040000, 0x1000]];
        for (const [tag, I] of [['J', J], ['R', R]])
            fs.writeFileSync(`${a.dumpDir}/${tag}.${fr}.bin`, Buffer.concat(REG.map(([b, n]) => Buffer.from(I.M.HEAPU8.buffer, b, n))));
    }
    rows.push(row);
}
const res = [];
for (const F of a.at) {
    const after = rows.filter((r) => r.f > F);
    const firstEq = after.find((r) => r.jPix === r.rPix);
    const settled = firstEq ? after.filter((r) => r.f >= firstEq.f && r.f < firstEq.f + a.settle) : [];
    const next = a.at.filter((x) => x > F).sort((x, y) => x - y)[0] ?? Infinity;
    const window = after.filter((r) => r.f < next);
    res.push({
        at: F, rooms: (rows.find((r) => r.f === F) || {}).rooms,
        framesToSamePicture: firstEq ? firstEq.f - F : null,
        settledFrames: settled.length, settledAllSame: settled.every((r) => r.jPix === r.rPix),
        framesSameAfter: window.filter((r) => r.jPix === r.rPix).length, framesAfter: window.length,
        controlSameAsR: window.filter((r) => r.sPix === r.rPix).length,
        residual: window.length ? window[window.length - 1] : null,
    });
}
const out = { rec: path.relative(ROOT, path.resolve(a.rec)), view: a.view, frames: total, reviews: J.M._PortLmpReviews(),
              gameplayDiffer, res };
console.log(JSON.stringify(out, null, 1));
if (a.out) fs.writeFileSync(a.out, JSON.stringify({ out, rows }));
process.exit(0);
