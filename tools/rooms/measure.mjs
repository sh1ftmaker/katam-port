// measure.mjs -- rooms mode (platform/localmp.c): two devices fed the same
// inputs that differ ONLY in whose room and camera they show.  What differs
// between their raw images, byte by byte, frame by frame?
//
//   node tools/rooms/measure.mjs --rom ROM --rec REC [--rec REC2 ...]
//        [--views 0,1] [--rooms 1] [--render 0] [--out DIR] [--dump-first N]
//        [--frames N] [--ref-rooms 0]
//
// Two wasm instances of build/katam-node.js (tools/netproof/instance.mjs),
// stepped in lockstep.  Both replay the recording with rooms mode on
// (PortLmpSetRooms); instance i shows player views[i] (PortLmpSetView).
// After every presented frame the GBA regions and the port sim block are
// compared byte for byte (the image lmpnet.c hashes, less SRAM and the
// out-of-map host state, which are compared through platform/audit.c's
// per-frame hashes).  For every byte that ever differs: first frame, last
// frame, frames differing, and the task that owned it at its first
// difference (heap memory).  Per frame: Kirby rooms and positions (to show
// the players separating) and the RNG.
//
// Output, per recording, in DIR/<rec>/: bytes.json (the per-byte table, as
// runs), frames.json (per frame: bytes differing per region, rooms), and
// raw dumps of both images at the first frames where a byte differs
// outside the "view so far" set, for gdb (A.F.bin, B.F.bin: the regions
// concatenated in REGIONS order).  tools/rooms/classify.py names and
// classifies them.  ROM-derived: keep DIR out of the repo.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { createInstance } from '../netproof/instance.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

function args() {
    const a = { rec: [], views: [0, 1], rooms: 1, render: 0, out: null, dumpFirst: 6, frames: 0,
                module: path.join(ROOT, 'build/katam-node.js'), dumpAt: [] };
    const v = process.argv.slice(2);
    for (let i = 0; i < v.length; i++) {
        const k = v[i], n = () => v[++i];
        if (k === '--rom') a.rom = n();
        else if (k === '--rec') a.rec.push(n());
        else if (k === '--views') a.views = n().split(',').map(Number);
        else if (k === '--rooms') a.rooms = +n();
        else if (k === '--render') a.render = +n();
        else if (k === '--out') a.out = n();
        else if (k === '--dump-first') a.dumpFirst = +n();
        else if (k === '--dump-at') a.dumpAt = n().split(',').map(Number);
        else if (k === '--frames') a.frames = +n();
        else if (k === '--module') a.module = n();
        else if (k === '--rooms-test') a.roomsTest = +n();
        else if (k === '--parts') a.parts = true;
        else if (k === '--watch') a.watch = n().split(',').map((w) => { const [ad, sz] = w.split(':'); return [parseInt(ad, 16), +(sz || 4)]; });
        else throw new Error('unknown argument ' + k);
    }
    if (!a.rom || !a.rec.length || !a.out) throw new Error('need --rom, --rec, --out');
    return a;
}

export const REGIONS = [
    ['ewram', 0x02000000, 0x40000], ['iwram', 0x03000000, 0x8000], ['io', 0x04000000, 0x400],
    ['pltt', 0x05000000, 0x400], ['vram', 0x06000000, 0x18000], ['oam', 0x07000000, 0x400],
    ['simblk', 0x02040000, 0x1000],
];
const AUDIT_COLS = ['image', 'ewram', 'iwram', 'io', 'pltt', 'vram', 'oam', 'sim', 'sram', 'iocanon', 'dmahost', 'ppuhost'];

async function makeDevice(a, rom, text, view, name) {
    return createInstance({
        modulePath: a.module, rom, name, render: !!a.render,
        modulePre(M) {
            const orig = M.onRuntimeInitialized;
            M.onRuntimeInitialized = function () {
                const p = M._malloc(text.length + 1);
                M.HEAPU8.set(text, p);
                const n = M._PortInputLogLoad(p, text.length);
                M._free(p);
                if (n < 0) throw new Error('replay load failed');
                M.recFrames = n;
                if (a.rooms) M._PortLmpSetRooms(1);
                M._PortLmpSetView(view);
                if (a.roomsTest) M._PortLmpRoomsTest(a.roomsTest);
                M._PortAuditEnable(1);
                M._PortSetFrameStateLog(1);
                M.portFrameState = () => {};
                orig();
            };
        },
    });
}

function owner(M, addr) {
    // platform/census.c Owner(): the task whose struct holds addr.
    let best = 0, bestMain = 0;
    for (let i = 0; i < 0x80; i++) {
        const t = 0x030019F0 + i * 0x14;
        const off = M.HEAPU8[t + 6] | (M.HEAPU8[t + 7] << 8);
        const fn = M.HEAPU32[(t + 8) >> 2] >>> 0;
        const flags = M.HEAPU8[t + 0x12] | (M.HEAPU8[t + 0x13] << 8);
        if (!fn) continue;
        const at = (flags & 0x10) ? 0x02000000 + (off << 2) : 0x03000000 + off;
        if (at <= addr && addr - at < 0x1000 && at > best) { best = at; bestMain = fn; }
    }
    return [bestMain, best];
}

// Gameplay probes that do not depend on where the heap put things: every
// live object (struct Object, kind 1) as [type, x, y, room, hp], with and
// without its owning player (base.unk56), as a sorted multiset.
const FNID_ObjectMain = 0x08099B81;
function objects(M) {
    const H = M.HEAPU8, D = new DataView(H.buffer);
    const out = [];
    for (let i = 0; i < 0x80; i++) {
        const t = 0x030019F0 + i * 0x14;
        if ((D.getUint32(t + 8, true) >>> 0) !== FNID_ObjectMain) continue;
        const off = D.getUint16(t + 6, true), flags = D.getUint16(t + 0x12, true);
        const o = (flags & 0x10) ? 0x02000000 + (off << 2) : 0x03000000 + off;
        if (H[o] !== 1) continue;
        const r = [H[o + 130], D.getInt32(o + 64, true) >> 8, D.getInt32(o + 68, true) >> 8,
                   D.getUint16(o + 96, true), D.getInt16(o + 128, true), H[o + 86]];
        r.addr = o;
        out.push(r);
    }
    const key = (withOwner) => out.map((r) => (withOwner ? r : r.slice(0, 5)).join(',')).sort().join(';');
    // Where they are: same objects at the same addresses?  And the task
    // table as a multiset of (main, priority): which tasks one device has.
    const placed = out.map((r) => r.join(',') + '@' + r.addr).sort().join(';');
    const tasks = {};
    for (let i = 0; i < 0x80; i++) {
        const t = 0x030019F0 + i * 0x14;
        const fn = D.getUint32(t + 8, true) >>> 0;
        if (!fn) continue;
        const k = fn.toString(16) + '/' + D.getUint16(t + 0x10, true).toString(16);
        tasks[k] = (tasks[k] || 0) + 1;
    }
    return { set: key(false), owned: key(true), placed, tasks, n: out.length };
}

async function runOne(a, rom, rec) {
    const text = fs.readFileSync(rec);
    const name = path.basename(rec, '.txt');
    const dir = path.join(a.out, name);
    fs.mkdirSync(dir, { recursive: true });
    const t0 = Date.now();
    const A = await makeDevice(a, rom, text, a.views[0], 'A');
    const B = await makeDevice(a, rom, text, a.views[1], 'B');
    const total = a.frames || A.M.recFrames;
    let size = 0;
    const off = REGIONS.map(([, , n]) => { const o = size; size += n; return o; });
    const first = new Int32Array(size).fill(-1), last = new Int32Array(size).fill(-1), count = new Uint32Array(size);
    const own = new Map();          // byte index -> [main, base] at first difference
    const frames = [];
    const hashA = A.M._malloc(64), hashB = B.M._malloc(64);
    const auditFirst = {}, auditCount = {};
    const taskOnly = {};
    let digestFrames = 0;
    const peakTasks = [0, 0];
    let dumps = 0, simUsed = 0x1000;
    const CH = 256;
    for (let f = 0; f < total; f++) {
        await A.step();
        await B.step();
        const fa = A.frame(), fb = B.frame();
        if (fa !== fb) throw new Error(`frames apart: ${fa} ${fb}`);
        simUsed = Math.min(0x1000, A.M.HEAPU32[(0x02040008) >> 2] >>> 0);
        const per = {};
        let newBytes = 0;
        for (let r = 0; r < REGIONS.length; r++) {
            const [rn, base, n0] = REGIONS[r];
            const n = rn === 'simblk' ? simUsed : n0;
            const ba = Buffer.from(A.M.HEAPU8.buffer, base, n), bb = Buffer.from(B.M.HEAPU8.buffer, base, n);
            if (ba.equals(bb)) continue;
            let nd = 0;
            for (let c = 0; c < n; c += CH) {
                const e = Math.min(n, c + CH);
                if (ba.subarray(c, e).equals(bb.subarray(c, e))) continue;
                for (let i = c; i < e; i++) {
                    if (ba[i] === bb[i]) continue;
                    const k = off[r] + i;
                    nd++;
                    if (first[k] < 0) {
                        first[k] = f; newBytes++;
                        if (rn === 'ewram' || rn === 'iwram') own.set(k, owner(A.M, base + i));
                    }
                    last[k] = f;
                    count[k]++;
                }
            }
            per[rn] = nd;
        }
        A.M._PortAuditHashes(hashA); B.M._PortAuditHashes(hashB);
        for (let c = 8; c < AUDIT_COLS.length; c++) {
            const x = A.M.HEAPU32[(hashA >> 2) + c], y = B.M.HEAPU32[(hashB >> 2) + c];
            if (x !== y) {
                const cn = AUDIT_COLS[c];
                if (!(cn in auditFirst)) auditFirst[cn] = f;
                auditCount[cn] = (auditCount[cn] || 0) + 1;
                per['a:' + cn] = 1;
            }
        }
        const ks = [0, 1, 2, 3].map((i) => A.kirby(i)), kb = [0, 1, 2, 3].map((i) => B.kirby(i));
        const oa = A.observe(), ob = B.observe();
        const row = { f: fa, d: per, rooms: ks.map((k) => k.room), xy: ks.map((k) => [k.x, k.y]),
                      hp: ks.map((k) => k.hp), rng: oa.rng, lp: [oa.localPlayer, ob.localPlayer],
                      humans: oa.lmpHumans, pauser: oa.pauser, interlude: oa.interlude };
        const kd = [];
        for (let i = 0; i < 4; i++)
            if (ks[i].x !== kb[i].x || ks[i].y !== kb[i].y || ks[i].room !== kb[i].room || ks[i].hp !== kb[i].hp) kd.push(i);
        if (kd.length) row.kirbyDiffer = kd;
        if (oa.rng !== ob.rng) row.rngDiffer = [oa.rng, ob.rng];
        if (A.M._PortLmpDigestAt) {
            const da = A.M._PortLmpDigestAt(fa) >>> 0, db = B.M._PortLmpDigestAt(fa) >>> 0;
            if (da && db) { digestFrames++; if (da !== db) row.digestDiffer = 1; }
            if (a.parts) {
                A.M._PortLmpDigestNow(); B.M._PortLmpDigestNow();
                const pa = A.M._PortLmpDigestParts() >> 2, pb = B.M._PortLmpDigestParts() >> 2;
                const dp = [0, 1, 2, 3].filter((i) => A.M.HEAPU32[pa + i] !== B.M.HEAPU32[pb + i]);
                if (dp.length) row.partsDiffer = dp;
            }
        }
        const ja = objects(A.M), jb = objects(B.M);
        if (ja.set !== jb.set) row.objSetDiffer = [ja.n, jb.n];
        else if (ja.owned !== jb.owned) row.objOwnerDiffer = 1;
        else if (ja.placed !== jb.placed) row.objPlaceDiffer = 1;
        const only = [];
        for (const k of new Set([...Object.keys(ja.tasks), ...Object.keys(jb.tasks)]))
            if ((ja.tasks[k] || 0) !== (jb.tasks[k] || 0)) only.push(k + ':' + (ja.tasks[k] || 0) + '/' + (jb.tasks[k] || 0));
        if (only.length) { row.taskDiffer = only; for (const k of only) taskOnly[k.split(':')[0]] = (taskOnly[k.split(':')[0]] || 0) + 1; }
        if (a.watch) {
            const wd = [];
            for (const [ad, sz] of a.watch) {
                const va = Buffer.from(A.M.HEAPU8.buffer, ad, sz).toString('hex'), vb = Buffer.from(B.M.HEAPU8.buffer, ad, sz).toString('hex');
                if (va !== vb) wd.push([ad.toString(16), va, vb]);
            }
            if (wd.length) row.watch = wd;
        }
        peakTasks[0] = Math.max(peakTasks[0], A.M.HEAPU8[0x03002E7C]);
        peakTasks[1] = Math.max(peakTasks[1], B.M.HEAPU8[0x03002E7C]);
        frames.push(row);
        if ((newBytes && dumps < a.dumpFirst) || a.dumpAt.includes(fa)) {
            for (const [tag, I] of [['A', A], ['B', B]]) {
                const parts = REGIONS.map(([, base, n]) => Buffer.from(I.M.HEAPU8.buffer, base, n));
                fs.writeFileSync(path.join(dir, `${tag}.${fa}.bin`), Buffer.concat(parts));
            }
            if (newBytes) dumps++;
        }
    }
    // Runs of differing bytes: contiguous, within one region, one owner.
    const runs = [];
    for (let r = 0; r < REGIONS.length; r++) {
        const [rn, base, n] = REGIONS[r];
        let s = -1, sOwn = '';
        const close = (e) => {
            let fmin = 1 << 30, lmax = -1, cmax = 0, csum = 0;
            const owners = {};
            for (let j = s; j < e; j++) {
                const kk = off[r] + j;
                fmin = Math.min(fmin, first[kk]); lmax = Math.max(lmax, last[kk]);
                cmax = Math.max(cmax, count[kk]); csum += count[kk];
                const o = own.get(kk);
                if (o) { const key = o[0].toString(16) + '@' + o[1].toString(16); owners[key] = (owners[key] || 0) + 1; }
            }
            runs.push({ region: rn, lo: base + s, hi: base + e, first: fmin, last: lmax, framesMax: cmax,
                        byteFrames: csum, owners });
            s = -1;
        };
        for (let i = 0; i < n; i++) {
            const k = off[r] + i;
            if (first[k] < 0) { if (s >= 0) close(i); continue; }
            const ok = String(own.get(k) || '');
            if (s >= 0 && ok !== sOwn) close(i);
            if (s < 0) { s = i; sOwn = ok; }
        }
        if (s >= 0) close(n);
    }
    const meta = { rec: path.relative(ROOT, path.resolve(rec)), frames: total, views: a.views, rooms: a.rooms,
                   render: a.render, wallS: (Date.now() - t0) / 1000, audit: { first: auditFirst, count: auditCount },
                   reviews: [A.M._PortLmpReviews(), B.M._PortLmpReviews()],
                   framesWithDiff: frames.filter((r) => Object.keys(r.d).length).length,
                   firstKirbyDiffer: (frames.find((r) => r.kirbyDiffer) || {}).f ?? null,
                   firstRngDiffer: (frames.find((r) => r.rngDiffer) || {}).f ?? null,
                   firstObjSetDiffer: (frames.find((r) => r.objSetDiffer) || {}).f ?? null,
                   firstObjOwnerDiffer: (frames.find((r) => r.objOwnerDiffer) || {}).f ?? null,
                   framesObjSetDiffer: frames.filter((r) => r.objSetDiffer).length,
                   framesObjOwnerDiffer: frames.filter((r) => r.objOwnerDiffer).length,
                   framesObjPlaceDiffer: frames.filter((r) => r.objPlaceDiffer).length,
                   framesTaskDiffer: frames.filter((r) => r.taskDiffer).length, taskOnly,
                   peakTasks, digestFrames, framesDigestDiffer: frames.filter((r) => r.digestDiffer).length,
                   firstDigestDiffer: (frames.find((r) => r.digestDiffer) || {}).f ?? null };
    fs.writeFileSync(path.join(dir, 'bytes.json'), JSON.stringify({ meta, runs }, null, 0));
    fs.writeFileSync(path.join(dir, 'frames.json'), JSON.stringify(frames));
    console.log(`${name}: ${total} frames, ${runs.length} differing runs, frames with a difference ${meta.framesWithDiff}, ` +
                `first Kirby difference ${meta.firstKirbyDiffer}, RNG ${meta.firstRngDiffer}, object set ${meta.firstObjSetDiffer} ` +
                `(${meta.framesObjSetDiffer} frames), object owners ${meta.firstObjOwnerDiffer} (${meta.framesObjOwnerDiffer} frames), ` +
                `objects at other addresses ${meta.framesObjPlaceDiffer} frames, task sets differ ${meta.framesTaskDiffer} frames, ` +
                `digest differs ${meta.framesDigestDiffer}/${meta.digestFrames} frames, ${meta.wallS.toFixed(1)} s`);
}

const a = args();
const rom = fs.readFileSync(a.rom);
for (const rec of a.rec) await runOne(a, rom, rec);
process.exit(0);
