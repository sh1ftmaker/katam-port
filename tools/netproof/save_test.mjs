#!/usr/bin/env node
// An online guest that already has a save of its own: does it survive the
// session?
//
//   node tools/netproof/save_test.mjs --rom ROM --save GUEST.sav
//        [--module build/katam-node.js] [--frames N] [--out DIR]
//
// Runs the page's OWN save code (web/shell.html, the "Save data" block,
// extracted and run as written) in both instances of a host/guest session
// (tools/netproof/run.mjs), over an in-memory stand-in for IndexedDB.  The
// guest's storage starts with GUEST.sav; the host's starts empty.  During the
// session every debounced write, the 5 s backstop scan and a tab hide/
// pagehide (forced every 50 save-memory writes, and at the end) are given
// every chance to write.  PASS when, after the session has ended:
//
//   1. the guest's storage still holds GUEST.sav byte for byte -- what the
//      next page load would restore -- and the page never wrote it;
//   2. the guest never restored GUEST.sav into save memory (no
//      portSramRestore call applied bytes after its session started);
//   3. every final frame matched (the session itself is unharmed: the host's
//      save is the world, SRAM is hashed);
//   4. the host's storage holds exactly the host's save memory (it persists
//      as usual).
//
// The same run on a build without the guard (--module of an older build)
// shows what it guards against: the guest's storage is overwritten.
import fs from 'fs';
import path from 'path';
import vm from 'vm';
import { fileURLToPath } from 'url';
import { runSession } from './run.mjs';
import { buildScenario, BOOT_END, SCRIPTED_END } from './scenario.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(here, '../..');
const argv = process.argv.slice(2);
const flag = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 ? argv[i + 1] : d; };
const ROM = flag('rom', process.env.ROM || '/bigdata/agent-tom/projects/katamport/decomp/baserom.gba');
const MODULE = path.resolve(flag('module', path.join(ROOT, 'build/katam-node.js')));
const SAVE = flag('save', null);
const FRAMES = +flag('frames', SCRIPTED_END - BOOT_END + 1200);
const OUT = flag('out', null);
if (!SAVE) { console.error('--save GUEST.sav is required (a played 64 KiB save)'); process.exit(2); }

const rom = fs.readFileSync(ROM);
const guestSave = new Uint8Array(fs.readFileSync(SAVE).subarray(0, 0x10000));

// The page's save block, as written.
const html = fs.readFileSync(path.join(ROOT, 'web/shell.html'), 'utf8');
const a = html.indexOf('// Save data.');
const b = html.indexOf('(function () {', a);
const e = html.indexOf('})();', b);
if (a < 0 || b < 0 || e < 0) { console.error('cannot find the save block in web/shell.html'); process.exit(2); }
const saveSrc = html.slice(b, e + 5);

// One page's worth of what that block touches.  `seed`: the record a
// previous visit left in this browser's storage for this ROM (or null).
function installPage(name, M, seed) {
    const st = { name, writes: 0, restoreCalls: 0, restoreApplied: 0, dirty: 0, logs: [], hidden: false, listeners: {},
                 key: null, record: seed ? seed.slice() : null };
    const el = () => ({ disabled: false, textContent: '', hidden: false, addEventListener() {}, click() {} });
    const els = {};
    const document = {
        getElementById: (id) => (els[id] = els[id] || el()),
        addEventListener: (ev, fn) => { (st.listeners[ev] = st.listeners[ev] || []).push(fn); },
        get hidden() { return st.hidden; },
        createElement: el, body: { appendChild() {}, removeChild() {} },
    };
    const window = { addEventListener: (ev, fn) => { (st.listeners[ev] = st.listeners[ev] || []).push(fn); } };
    const shell = {
        log: (t, lv) => st.logs.push(`${lv || 'info'}: ${t}`),
        note: (t, lv) => st.logs.push(`note ${lv || ''}: ${t}`),
        idb: {
            saveStore: 'saves',
            get: (s, k) => { st.key = k; return Promise.resolve(st.record ? { bytes: st.record.slice(), saved: 1 } : undefined); },
            put: (s, k, rec) => { st.writes++; st.record = new Uint8Array(rec.bytes); return Promise.resolve(); },
            del: (s, k) => { st.record = null; return Promise.resolve(); },
        },
    };
    M._shell = shell;
    const ctx = vm.createContext({ Module: M, document, window, setTimeout, clearTimeout,
        setInterval: (fn) => { st.scan = fn; return 0; },
        Uint8Array, Math, Date, Promise, String, Error, URL: { createObjectURL: () => '', revokeObjectURL() {} }, Blob: function () {} });
    vm.runInContext(saveSrc, ctx);
    const restore = M.portSramRestore, dirty = M.portSramDirty;
    M.portSramRestore = (p, n) => { st.restoreCalls++; const r = restore(p, n); if (r) st.restoreApplied++; return r; };
    st.fire = (ev) => { for (const fn of st.listeners[ev] || []) fn(); };
    M.portSramDirty = () => {
        st.dirty++;
        dirty();
        if (st.dirty % 50 === 0) st.fire('pagehide');      // the tab going away, mid-session
        if (st.dirty % 70 === 0) st.scan && st.scan();     // the backstop scan
    };
    st.ready = shell.saveForRom(rom, 'katam.gba');
    return st;
}

const pages = {};
let restoreAtJoin = -1;
const sc = buildScenario({ seed: 3, humanFrames: Math.max(0, FRAMES - (SCRIPTED_END - BOOT_END) + 600) });
const t0 = Date.now();
const r = await runSession({
    name: 'save test', modulePath: MODULE, rom, scenario: sc, link: { oneWay: 60, jitter: 15, loss: 0.02 },
    durationFrames: FRAMES,
    // each page's save block is up before its game runs (the guest's found
    // its stored save at ROM load: a returning player, at the title screen)
    hostModulePre: (M) => { pages.host = installPage('host', M, null); },
    guestModulePre: (M) => { pages.guest = installPage('guest', M, guestSave); },
    guestPre: () => { restoreAtJoin = pages.guest.restoreApplied; },
});
const guest = r._guest, host = r._host;
const G = pages.guest, H = pages.host;
const restoreAfterJoin = G.restoreApplied - restoreAtJoin;
// the session ends; the tab is hidden, then closed; every timer gets to run
guest.M._PortLnStop();
host.M._PortLnStop();
for (const P of [G, H]) { P.hidden = true; P.fire('visibilitychange'); P.fire('pagehide'); P.hidden = false; if (P.scan) P.scan(); }
await new Promise((res) => setTimeout(res, 1500));
for (const P of [G, H]) { P.fire('pagehide'); }
await new Promise((res) => setTimeout(res, 100));

const same = (x, y) => x && y && x.length === y.length && x.every((v, i) => v === y[i]);
const hostSram = host.M.HEAPU8.slice(0x09000000, 0x09010000);
const guestSram = guest.M.HEAPU8.slice(0x09000000, 0x09010000);
const res = {
    module: path.relative(ROOT, MODULE), frames: FRAMES, wallSec: (Date.now() - t0) / 1000,
    compare: r.compare,
    guest: { storageWrites: G.writes, sramDirtyCalls: G.dirty, restoreCalls: G.restoreCalls, restoreApplied: G.restoreApplied,
             restoredBeforeJoin: restoreAtJoin, restoredAfterJoin: restoreAfterJoin,
             storedIsOwnSave: same(G.record, guestSave), memoryIsHostSave: same(guestSram, hostSram), ownSaveInMemory: same(guestSram, guestSave),
             sramWritesTel: r.guest.tel.sramWrites },
    host: { storageWrites: H.writes, restoreCalls: H.restoreCalls, storedIsHostMemory: !!same(H.record, hostSram), sramWritesTel: r.host.tel.sramWrites },
};
const checks = {
    'guest storage still holds its own save, never written': res.guest.storedIsOwnSave && res.guest.storageWrites === 0,
    'guest never restored its own save over the session\'s': restoreAfterJoin === 0,
    'every final frame identical': r.compare.mismatched === 0 && r.compare.compared >= r.compare.expected * 0.9,
    'host storage holds the host\'s save memory': res.host.storedIsHostMemory && res.host.storageWrites > 0,
    'the session wrote save memory (the test means something)': res.guest.sramWritesTel > 0 && res.guest.sramDirtyCalls + res.guest.sramWritesTel > 0,
};
res.checks = checks;
res.verdict = Object.values(checks).every(Boolean) ? 'PASS' : 'FAIL';
console.log(JSON.stringify(res, null, 1));
if (OUT) { fs.mkdirSync(OUT, { recursive: true }); fs.writeFileSync(path.join(OUT, 'save-test.json'), JSON.stringify(res, null, 1)); }
process.exit(res.verdict === 'PASS' ? 0 : 1);
