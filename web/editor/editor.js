// The KATAM level editor (/editor/): a browser front end over the authoring
// library (tools/katam/lib, the same code as the `katam` CLI).
//
// Everything happens in this browser and nothing is uploaded:
//   - the player's ROM (the copy the game page remembers, or one picked here)
//     is decompiled in memory, so tiles and backgrounds are drawn from it;
//   - a project (katam-project/1: room JSON files) is edited on a canvas and
//     kept in IndexedDB between visits; Save downloads it as a .zip;
//   - Play compiles the project's rooms (katam-native/1) and hands them to the
//     game, which runs in a frame (play/, web/editor/play.js) and boots
//     straight into the room at the start flag.
//
// The tile palette is the tileset's 16x16 pieces as the original rooms use
// them, each with the collision it usually has there: painting a piece
// paints its collision too.  The collision brush overrides it.
const CFG = Object.assign({ lib: 'katam/lib/', data: 'data.json', play: 'play/', build: 'dev' }, window.KATAM_ED || {});
const BASE = new URL('.', import.meta.url).href;

// ------------------------------------------------------------------ helpers
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => Array.from(r.querySelectorAll(s));
function h(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === null || v === undefined || v === false) continue;
    if (k === 'class') e.className = v;
    else if (k === 'html') e.innerHTML = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (k === 'value') e.value = v;
    else if (k === 'checked' || k === 'disabled' || k === 'selected') e[k] = !!v;
    else e.setAttribute(k, v === true ? '' : v);
  }
  for (const k of kids.flat()) if (k !== null && k !== undefined && k !== false) e.append(k.nodeType ? k : document.createTextNode(String(k)));
  return e;
}
const setKids = (el, ...kids) => el.replaceChildren(...kids.flat().filter((k) => k !== null && k !== undefined && k !== false));
const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
const debounce = (fn, ms) => { let t = 0; const f = (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms); }; f.cancel = () => clearTimeout(t); return f; };
const lsGet = (k, d) => { try { const v = localStorage.getItem(k); return v === null ? d : JSON.parse(v); } catch (e) { return d; } };
const lsSet = (k, v) => { try { localStorage.setItem(k, JSON.stringify(v)); } catch (e) { /* storage off: this session only */ } };

let toastTimer = 0;
function toast(msg, ms = 2600) {
  const t = $('#toast'); t.textContent = msg; t.hidden = false;
  clearTimeout(toastTimer); toastTimer = setTimeout(() => { t.hidden = true; }, ms);
}
function status(t) { $('#st-text').textContent = t; }

function idb(name, version, stores) {
  return new Promise((res, rej) => {
    if (!window.indexedDB) return rej(new Error('IndexedDB is not available'));
    const rq = indexedDB.open(name, version);
    rq.onupgradeneeded = () => { for (const s of stores) if (!rq.result.objectStoreNames.contains(s)) rq.result.createObjectStore(s); };
    rq.onsuccess = () => res(rq.result);
    rq.onerror = () => rej(rq.error || new Error('could not open browser storage'));
    rq.onblocked = () => rej(new Error('browser storage is busy in another tab'));
  });
}
const idbReq = (db, store, mode, fn) => new Promise((res, rej) => {
  const rq = fn(db.transaction(store, mode).objectStore(store));
  rq.onsuccess = () => res(rq.result); rq.onerror = () => rej(rq.error);
});
// The game page's own database (web/shell.html "Remembering the ROM"): one
// record {bytes, name, saved} under 'current'.  Shared, so a ROM picked on
// either page serves both, and the play frame finds it by itself.
const romDb = () => idb('katam.rom', 2, ['rom', 'save']);
const edDb = () => idb('katam.editor', 1, ['kv']);

async function sha1(bytes) {
  const d = await crypto.subtle.digest('SHA-1', bytes);
  return Array.from(new Uint8Array(d), (b) => b.toString(16).padStart(2, '0')).join('');
}

// zip: stored entries out, stored or deflated entries in
const CRC = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
const crc32 = (b) => { let c = ~0; for (let i = 0; i < b.length; i++) c = CRC[(c ^ b[i]) & 255] ^ (c >>> 8); return ~c >>> 0; };
function makeZip(files) {
  const enc = new TextEncoder(), parts = [], central = [];
  let off = 0;
  for (const f of files) {
    const name = enc.encode(f.name), crc = crc32(f.bytes), n = f.bytes.length;
    const lh = new DataView(new ArrayBuffer(30));
    lh.setUint32(0, 0x04034b50, true); lh.setUint16(4, 20, true); lh.setUint16(6, 0x0800, true); lh.setUint16(12, 0x21, true);
    lh.setUint32(14, crc, true); lh.setUint32(18, n, true); lh.setUint32(22, n, true); lh.setUint16(26, name.length, true);
    parts.push(new Uint8Array(lh.buffer), name, f.bytes);
    const ch = new DataView(new ArrayBuffer(46));
    ch.setUint32(0, 0x02014b50, true); ch.setUint16(4, 20, true); ch.setUint16(6, 20, true); ch.setUint16(8, 0x0800, true); ch.setUint16(14, 0x21, true);
    ch.setUint32(16, crc, true); ch.setUint32(20, n, true); ch.setUint32(24, n, true); ch.setUint16(28, name.length, true); ch.setUint32(42, off, true);
    central.push(new Uint8Array(ch.buffer), name);
    off += 30 + name.length + n;
  }
  const csize = central.reduce((a, p) => a + p.length, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true); end.setUint16(8, files.length, true); end.setUint16(10, files.length, true); end.setUint32(12, csize, true); end.setUint32(16, off, true);
  return new Blob([...parts, ...central, new Uint8Array(end.buffer)], { type: 'application/zip' });
}
function readZip(buf, inflate) {
  const b = new Uint8Array(buf), dv = new DataView(buf), out = [];
  let e = b.length - 22;
  while (e >= 0 && dv.getUint32(e, true) !== 0x06054b50) e--;
  if (e < 0) throw new Error('not a zip file');
  let p = dv.getUint32(e + 16, true);
  for (let i = 0, n = dv.getUint16(e + 10, true); i < n; i++) {
    const method = dv.getUint16(p + 10, true), csize = dv.getUint32(p + 20, true);
    const nlen = dv.getUint16(p + 28, true), xlen = dv.getUint16(p + 30, true), clen = dv.getUint16(p + 32, true), local = dv.getUint32(p + 42, true);
    const name = new TextDecoder().decode(b.subarray(p + 46, p + 46 + nlen));
    p += 46 + nlen + xlen + clen;
    if (name.endsWith('/') || !/\.json$/i.test(name)) continue;
    const start = local + 30 + dv.getUint16(local + 26, true) + dv.getUint16(local + 28, true);
    const raw = b.subarray(start, start + csize);
    let data;
    if (method === 0) data = raw;
    else if (method === 8) { const z = new Uint8Array(raw.length + 2); z[0] = 0x78; z[1] = 0x01; z.set(raw, 2); data = inflate(z); }
    else throw new Error(`${name}: compression method ${method} (use stored or deflate)`);
    out.push({ name, text: new TextDecoder().decode(data) });
  }
  return out;
}

// original icons (no game art on the page)
const ICON = {
  select: '<path d="M6 3l14 9-6 1.5 3.5 7-3 1.5-3.5-7L6 19z" fill="#fff" stroke="#2a1f5c" stroke-width="1.6" stroke-linejoin="round"/>',
  brush: '<path d="M20.5 3.5c-3 0-9 5-11 8l3 3c3-2 8-8 8-11z" fill="#4aa3ff" stroke="#2a1f5c" stroke-width="1.4"/><path d="M8.6 12.6c-2.6.2-4 2-4 4.2 0 1.2-.8 2-1.8 2.4 3 1.4 7.400.4 8.2-3.2z" fill="#7b5bf0" stroke="#2a1f5c" stroke-width="1.4"/>',
  rect: '<rect x="3.5" y="5.500" width="17" height="13" rx="1.500" fill="none" stroke="#2a1f5c" stroke-width="2" stroke-dasharray="3.2 2.400"/>',
  fill: '<path d="M10 3l8 8-6.500 6.500a2 2 0 01-2.800 0L4.500 13.300a2 2 0 010-2.800z" fill="#ffd23f" stroke="#2a1f5c" stroke-width="1.500"/><path d="M19 14c1.500 2 2.500 3.400 2.500 4.500a2.500 2.500 0 01-5 0c0-1.100 1-2.500 2.500-4.500z" fill="#4aa3ff" stroke="#2a1f5c" stroke-width="1.200"/>',
  eraser: '<path d="M14.500 4l6 6-9.500 9.500H6.500L3 16z" fill="#ff8fb3" stroke="#2a1f5c" stroke-width="1.500" stroke-linejoin="round"/><path d="M8 10.500l6 6" stroke="#2a1f5c" stroke-width="1.500"/>',
  collision: '<rect x="3" y="3" width="8" height="8" rx="1.500" fill="#4aa3ff"/><rect x="13" y="3" width="8" height="8" rx="1.500" fill="#7b5bf0"/><rect x="3" y="13" width="8" height="8" rx="1.500" fill="#7b5bf0"/><rect x="13" y="13" width="8" height="8" rx="1.500" fill="#4aa3ff"/>',
  object: '<circle cx="12" cy="13" r="8" fill="#ff7a59" stroke="#2a1f5c" stroke-width="1.500"/><circle cx="9.500" cy="11.500" r="1.300" fill="#2a1f5c"/><circle cx="14.500" cy="11.500" r="1.300" fill="#2a1f5c"/><path d="M9 15.500q3 2 6 0" stroke="#2a1f5c" stroke-width="1.400" fill="none" stroke-linecap="round"/>',
  door: '<path d="M6 21V8a6 6 0 0112 0v13z" fill="#c98a3a" stroke="#2a1f5c" stroke-width="1.500"/><circle cx="14.800" cy="14" r="1.200" fill="#ffd23f" stroke="#2a1f5c" stroke-width=".8"/>',
  eyedropper: '<path d="M15 4.500l4.500 4.500-2 2-1-1-7.500 7.500-3.500 1 1-3.500L14 7.500l-1-1z" fill="#8fe3c0" stroke="#2a1f5c" stroke-width="1.500" stroke-linejoin="round"/>',
  layers: '<path d="M12 4l9 4.500-9 4.500-9-4.500z" fill="currentColor"/><path d="M4 13l8 4 8-4M4 16.500l8 4 8-4" stroke="currentColor" stroke-width="1.800" fill="none"/>',
  map: '<circle cx="12" cy="5" r="2.500" fill="currentColor"/><circle cx="5" cy="19" r="2.500" fill="currentColor"/><circle cx="19" cy="19" r="2.500" fill="currentColor"/><path d="M12 7.500V13M12 13l-6 4M12 13l6 4" stroke="currentColor" stroke-width="1.800" fill="none"/>',
  gear: '<path d="M12 8.500a3.500 3.500 0 100 7 3.500 3.500 0 000-7zm8.500 3.500l-2.100-.7-.5-1.300 1-2-1.800-1.800-2 1-1.300-.5L13 4.500h-2l-.7 2.200-1.300.5-2-1L5.200 8l1 2-.5 1.300-2.200.7v2l2.200.7.5 1.300-1 2L7 19.800l2-1 1.300.5.700 2.200h2l.700-2.200 1.300-.5 2 1 1.800-1.800-1-2 .5-1.300 2.200-.7z" fill="currentColor"/>',
};
const ico = (n) => `<svg viewBox="0 0 24 24" aria-hidden="true">${ICON[n]}</svg>`;

// --------------------------------------------------------------------- state
const S = {
  L: null, data: null, io: null, vocab: null, registry: null, vanilla: null, rom: null,
  proj: null, bind: null, ctx: null,
  room: null,               // { id, path, doc, W, H, art: [token], col: [value], num }
  gfx: null,                // { key, tiles, banks, cache: Map(token -> ImageData), artCv, bgCv }
  tool: 'select', tile: null, tileCol: 13, colBrush: 13, objType: 'waddle_dee_1',
  sel: null,                // { kind: 'object' | 'door', i }
  zoom: lsGet('katam.ed.zoom', 2), show: Object.assign({ collision: true, objects: true, camera: false, grid: true }, lsGet('katam.ed.show', {})),
  followCol: true, tab: 'tiles', filter: 'all', search: '',
  undo: [], redo: [], hover: null, drag: null, pick: null,
  findings: { errors: [], warnings: [] },
  starts: lsGet('katam.ed.starts', {}),
  dicts: new Map(), colClass: [], colRep: {}, objInfo: new Map(), looks: null,
};
window.katamEditor = S;   // for the console and the tests

const CLASSES = [
  ['air', 'Air (nothing)', null], ['solid', 'Solid (block)', [64, 150, 255]], ['platform', 'One-way (platform)', [70, 210, 90]],
  ['slope', 'Slope', [255, 160, 50]], ['hazard', 'Hazard (damage)', [255, 70, 90]], ['ladder', 'Ladder', [255, 230, 60]],
  ['water', 'Water', [60, 220, 230]], ['breakable', 'Breakable', [200, 130, 60]], ['door', 'Door', [255, 255, 255]], ['other', 'Other', [220, 90, 255]],
];
const CLASS_COLOR = Object.fromEntries(CLASSES.map((c) => [c[0], c[2]]));
function classOfBits(bits) {
  if (!bits || bits === 'empty' || bits === 'unused') return 'air';
  if (bits.includes('hazard')) return 'hazard';
  if (bits.includes('door')) return 'door';
  if (bits.includes('water')) return 'water';
  if (bits.includes('ladder')) return 'ladder';
  if (bits.includes('breakable')) return 'breakable';
  if (bits.includes('top_solid')) return 'platform';
  if (bits.includes('slope')) return 'slope';
  if (bits.includes('solid')) return 'solid';
  return 'other';
}
const CAT_COLOR = { enemy: '#ff8a5c', miniboss: '#d58cff', boss: '#b46bff', item: '#7df0c0', gadget: '#ffd23f', door: '#c8f76a', trigger: '#9fd0ff', camera: '#9fd0ff', effect: '#d0d0e8', other: '#d0d0e8' };
const CAT_ORDER = ['enemy', 'miniboss', 'boss', 'item', 'gadget', 'door', 'trigger', 'camera', 'effect', 'other'];
const hex2 = (n) => '0x' + n.toString(16).toUpperCase().padStart(2, '0');
const nice = (s) => String(s).replace(/_/g, ' ');

// ---------------------------------------------------------------------- boot
const curtain = (msg, { pick = false, err = false } = {}) => {
  $('#curtain').classList.remove('hidden');
  const m = $('#cur-msg'); m.textContent = msg; m.className = err ? 'err' : '';
  $('#cur-pick').hidden = !pick;
};

async function loadLibrary() {
  const names = ['io', 'png', 'zlib', 'rom', 'vocab', 'registry', 'pipeline', 'project', 'validate', 'native', 'rompack', 'room', 'gfx', 'grid', 'tiles', 'json'];
  const mods = await Promise.all(names.map((n) => import(new URL(CFG.lib + n + '.mjs', BASE).href)));
  const L = Object.fromEntries(names.map((n, i) => [n, mods[i]]));
  L.png.setZlib({ deflate: L.zlib.deflate, inflate: L.zlib.inflate });
  return L;
}

async function storedRom() {
  try { const db = await romDb(); const rec = await idbReq(db, 'rom', 'readonly', (s) => s.get('current')); db.close(); return rec && rec.bytes ? { bytes: new Uint8Array(rec.bytes), name: rec.name } : null; } catch (e) { return null; }
}
async function rememberRom(bytes, name) {
  try { const db = await romDb(); await idbReq(db, 'rom', 'readwrite', (s) => s.put({ bytes, name, saved: Date.now() }, 'current')); db.close(); return true; } catch (e) { return false; }
}

async function boot() {
  $('#st-build').textContent = 'build ' + CFG.build;
  curtain('Loading the editor…');
  const [data, L] = await Promise.all([fetch(new URL(CFG.data, BASE)).then((r) => { if (!r.ok) throw new Error('data.json: ' + r.status); return r.json(); }), loadLibrary()]);
  S.data = data; S.L = L;
  S.io = L.io.memoryIO();
  for (const [n, v] of Object.entries(data.schemas)) S.io.writeText('/schema/' + n, JSON.stringify(v));
  for (const [n, v] of Object.entries(data.semantics)) S.io.writeText('/semantics/' + n, JSON.stringify(v));
  S.vocab = L.vocab.indexVocab(data.vocab);
  S.registry = L.registry.loadRegistry(S.io, '/semantics');
  S.colClass = S.vocab.collision.map((c) => classOfBits(c.bits));
  const named = S.registry.collisionEnum();
  for (const [cls] of CLASSES) {
    let best = -1, bestN = -1;
    for (const [v, e] of Object.entries(named)) if (S.colClass[v] === cls && (e.cells || 0) > bestN) { best = Number(v); bestN = e.cells || 0; }
    if (best >= 0) S.colRep[cls] = best;
  }
  S.colNames = named;
  const types = (data.semantics['object_params.json'] || {}).types || {};
  for (const [name, num] of Object.entries(S.vocab.objectTypes)) {
    const t = types[hex2(num)] || {};
    S.objInfo.set(name, { num, category: t.category || (num < 0x5e ? 'enemy' : 'other'), description: t.description || '', params: (t.params || []).filter((p) => p.name) });
  }

  let rom = await storedRom();
  while (true) {
    if (!rom) {
      curtain('Choose your Kirby & The Amazing Mirror ROM to begin.', { pick: true });
      rom = await new Promise((res) => {
        $('#romfile').onchange = async function () { const f = this.files[0]; this.value = ''; if (f) res({ bytes: new Uint8Array(await f.arrayBuffer()), name: f.name, picked: true }); };
      });
    }
    curtain('Checking the ROM…');
    const sum = await sha1(rom.bytes);
    if (sum === L.rom.VANILLA_SHA1) break;
    curtain(`That file (sha1 ${sum.slice(0, 12)}…) is not the supported dump: Kirby & The Amazing Mirror (USA). Choose another file.`, { pick: true, err: true });
    rom = await new Promise((res) => {
      $('#romfile').onchange = async function () { const f = this.files[0]; this.value = ''; if (f) res({ bytes: new Uint8Array(await f.arrayBuffer()), name: f.name, picked: true }); };
    });
  }
  S.rom = rom.bytes;
  if (rom.picked) S.romStored = await rememberRom(rom.bytes, rom.name); else S.romStored = true;

  curtain('Reading the game from your ROM (in this browser)…');
  await new Promise((r) => setTimeout(r, 30));
  const t0 = performance.now();
  const romTables = L.rom.parseRomTables(data.romTables);
  const pack = L.rompack.packFromRom(S.rom, romTables, data.manifest);
  L.pipeline.decompileVanilla({ io: S.io, romBytes: S.rom, romTables, pack, vocab: S.vocab, registry: S.registry, outDir: '/vanilla' });
  S.vanilla = L.project.loadProject(S.io, '/vanilla');
  console.log(`[editor] game decompiled in ${Math.round(performance.now() - t0)} ms`);

  let saved = null;
  try { const db = await edDb(); saved = await idbReq(db, 'kv', 'readonly', (s) => s.get('project')); db.close(); } catch (e) { /* none */ }
  if (saved && saved.files && saved.files.length) { putFiles(saved.files); S.wantRoom = saved.cur; } else putExample();
  buildUi();
  rebind();
  const rooms = roomIds();
  openRoom(rooms.includes(S.wantRoom) ? S.wantRoom : rooms[0]);
  $('#curtain').classList.add('hidden');
  $('#app').classList.remove('loading');
  if (lsGet('katam.ed.zoom', 0) === 0) zoomFit();
  validateSoon();
}

// ------------------------------------------------------------------- project
const projPath = (id) => '/project/' + S.L.project.pathOf(id);
function clearProject() { for (const p of S.io.list('/project')) S.io.files.delete(p); }
function putFiles(files) { clearProject(); for (const [p, t] of files) S.io.writeText(p, t); }
function putExample() {
  clearProject();
  for (const [p, t] of Object.entries(S.data.examples)) S.io.writeText('/project/' + p.replace(/^[^/]+\//, ''), t);
}
function rebind() {
  const P = S.L.project;
  S.proj = P.loadProject(S.io, '/project');
  S.bind = new P.Bindings(S.vocab, [S.vanilla, S.proj]);
  S.ctx = P.makeContext({ vocab: S.vocab, registry: S.registry, bindings: S.bind });
  if (S.room) S.room.num = S.bind.num.get(S.room.id);
}
const roomIds = () => [...S.proj.docs.keys()].filter((id) => id.startsWith('room:')).sort((a, b) => S.bind.num.get(a) - S.bind.num.get(b));
const roomDoc = (id) => (S.room && S.room.id === id ? S.room.doc : (S.proj.docs.get(id) || {}).doc);
const roomLabel = (id) => { const d = roomDoc(id); return `${S.bind.num.get(id)} ${d && d.name ? '(' + d.name + ')' : id.split('/').pop()}`; };
function targetLabel(to) {
  const r = S.bind.resolve(to);
  if (r && S.proj.docs.has(r)) return 'room ' + S.bind.num.get(r);
  if (r) return 'room ' + S.bind.num.get(r) + ' (original)';
  return to || '?';
}

const persist = debounce(async () => {
  try {
    const files = S.io.list('/project').map((p) => [p, S.io.readText(p)]);
    const db = await edDb(); await idbReq(db, 'kv', 'readwrite', (s) => s.put({ files, cur: S.room && S.room.id, saved: Date.now() }, 'project')); db.close();
  } catch (e) { console.warn('[editor] autosave failed:', e); }
}, 500);

/** Write the open room back into the project files. */
function flush() {
  const R = S.room;
  if (!R || !R.dirty) return;
  const G = S.L.grid, d = R.doc, empty = dict(d.tileset).empty;
  d.size = { width: R.W, height: R.H };
  d.art = G.encodeGrid(R.art, R.W, R.H, { suggest: (m) => (m === empty ? '.' : null) });
  d.collision = G.encodeGrid(R.col, R.W, R.H, { suggest: (v) => S.ctx.collisionGlyph(v), toLegend: (v) => S.ctx.collisionName(v) });
  S.io.writeText(R.path, S.L.json.writeJson(d));
  R.dirty = false;
  persist();
}

function openRoom(id, { keepUndo = false } = {}) {
  flush();
  const T = S.L.tiles, G = S.L.grid;
  const path = projPath(id);
  const doc = JSON.parse(S.io.readText(path));
  const W = doc.size.width, H = doc.size.height;
  let art, col;
  try {
    art = G.decodeGrid(doc.art, W, H, 'art').map((s) => T.metaToken(T.metaValues(s)));
    col = G.decodeGrid(doc.collision, W, H, 'collision').map((v) => S.ctx.collisionValue(v));
  } catch (e) { toast('This room could not be opened: ' + e.message, 6000); return false; }
  doc.objects = doc.objects || []; doc.doors = doc.doors || [];
  S.room = { id, path, doc, W, H, art, col, num: S.bind.num.get(id), dirty: false };
  if (!keepUndo) { S.undo = []; S.redo = []; }
  S.sel = null; S.hover = null;
  loadGfx();
  const D = dict(doc.tileset);
  if (!S.tile || !D.map.has(S.tile)) { const first = D.list.find((e) => e.cls === 'solid') || D.list[0]; S.tile = first ? first.tok : D.empty; S.tileCol = first ? first.col : 0; }
  sizeCanvas();
  refreshAll();
  persist();
  return true;
}

function touch() { S.room.dirty = true; flushSoon(); validateSoon(); thumbsSoon(); requestRender(); }
const flushSoon = debounce(() => { flush(); refreshStatus(); }, 400);

function snapshot() {
  const R = S.room;
  return { W: R.W, H: R.H, art: R.art.slice(), col: R.col.slice(), objects: JSON.stringify(R.doc.objects), doors: JSON.stringify(R.doc.doors) };
}
function restore(s) {
  const R = S.room, resized = R.W !== s.W || R.H !== s.H;
  R.W = s.W; R.H = s.H; R.art = s.art.slice(); R.col = s.col.slice();
  R.doc.objects = JSON.parse(s.objects); R.doc.doors = JSON.parse(s.doors);
  if (S.sel && !selected()) S.sel = null;
  if (resized) sizeCanvas();
  redrawArt(); touch(); refreshInspector(); refreshMap();
}
function pushUndo() { S.undo.push(snapshot()); if (S.undo.length > 80) S.undo.shift(); S.redo = []; refreshUndo(); }
function undo() { if (!S.undo.length) return; S.redo.push(snapshot()); restore(S.undo.pop()); refreshUndo(); }
function redo() { if (!S.redo.length) return; S.undo.push(snapshot()); restore(S.redo.pop()); refreshUndo(); }
function refreshUndo() { $('#undo').disabled = !S.undo.length; $('#redo').disabled = !S.redo.length; }

// ---------------------------------------------------------------- graphics
const rgb5 = (hex) => { const c = S.L.gfx.bgr555(hex); return [c & 31, (c >> 5) & 31, (c >> 10) & 31].map((v) => (v << 3) | (v >> 2)); };
const docOf = (id) => { const r = S.bind.resolve(id); return r ? S.bind.docs.get(r) : null; };
const gfxCache = new Map();

function loadGfx() {
  const d = S.room.doc, key = d.tileset + '|' + d.palette + '|' + d.background;
  let g = gfxCache.get(key);
  if (!g) {
    g = { key, tiles: new Uint8Array(0), banks: [], cache: new Map(), bg: null };
    try {
      const ts = docOf(d.tileset), pal = docOf(d.palette);
      if (ts) g.tiles = S.L.gfx.pngToTiles(S.io.readBytes(`${ts.base}/${ts.doc.image}`), ts.doc.tileCount, ts.doc.image);
      if (pal) g.banks = pal.doc.banks.map((b) => b.map(rgb5));
      g.tileCount = ts ? ts.doc.tileCount : 0;
    } catch (e) { console.warn('[editor] tileset:', e); }
    try {
      const bg = docOf(d.background);
      if (bg) {
        const b = bg.doc, tiles = S.L.gfx.pngToTiles(S.io.readBytes(`${bg.base}/${b.image}`), b.tileCount, b.image);
        const cols = b.palette.colors.map(rgb5), off = b.palette.offset, w = b.size.width, hh = b.size.height;
        const im = new ImageData(w * 8, hh * 8);
        b.tilemap.forEach((row, ty) => row.trim().split(/\s+/).forEach((tok, tx) => {
          const v = S.L.tiles.valueOf(tok), t = v & 0x3ff, hf = v & 0x400, vf = v & 0x800, bank = v >> 12;
          if (t * 32 + 32 > tiles.length) return;
          for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++) {
            const sx = hf ? 7 - x : x, sy = vf ? 7 - y : y, by = tiles[t * 32 + sy * 4 + (sx >> 1)], ci = sx & 1 ? by >> 4 : by & 15;
            const c = cols[bank * 16 + ci - off] || [24, 24, 40], o = ((ty * 8 + y) * im.width + tx * 8 + x) * 4;
            im.data[o] = c[0]; im.data[o + 1] = c[1]; im.data[o + 2] = c[2]; im.data[o + 3] = 255;
          }
        }));
        g.bg = h('canvas', { width: im.width, height: im.height });
        g.bg.getContext('2d').putImageData(im, 0, 0);
      }
    } catch (e) { console.warn('[editor] background:', e); }
    gfxCache.set(key, g);
  }
  S.gfx = g;
}

function metaImage(tok, g = S.gfx) {
  let im = g.cache.get(tok);
  if (im) return im;
  im = new ImageData(16, 16);
  let vals;
  try { vals = S.L.tiles.metaValues(tok); } catch (e) { vals = null; }
  for (let q = 0; q < 4; q++) {
    const ox = (q & 1) * 8, oy = (q >> 1) * 8;
    const v = vals ? vals[q] : 0, t = v & 0x3ff, hf = v & 0x400, vf = v & 0x800, bank = g.banks[v >> 12];
    const bad = !vals || t * 32 + 32 > g.tiles.length;
    for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++) {
      const o = ((oy + y) * 16 + ox + x) * 4;
      if (bad) { im.data[o] = 255; im.data[o + 2] = 255; im.data[o + 3] = (x + y) & 1 ? 255 : 90; continue; }
      const sx = hf ? 7 - x : x, sy = vf ? 7 - y : y, by = g.tiles[t * 32 + sy * 4 + (sx >> 1)], ci = sx & 1 ? by >> 4 : by & 15;
      if (!ci) continue;
      const c = (bank && bank[ci]) || [255, 0, 255];
      im.data[o] = c[0]; im.data[o + 1] = c[1]; im.data[o + 2] = c[2]; im.data[o + 3] = 255;
    }
  }
  g.cache.set(tok, im);
  return im;
}

function redrawArt() {
  const R = S.room;
  if (!S.artCv) S.artCv = document.createElement('canvas');
  S.artCv.width = R.W * 16; S.artCv.height = R.H * 16;
  const c = S.artCv.getContext('2d');
  for (let i = 0; i < R.art.length; i++) c.putImageData(metaImage(R.art[i]), (i % R.W) * 16, ((i / R.W) | 0) * 16);
}
function drawCell(i) { S.artCv.getContext('2d').putImageData(metaImage(S.room.art[i]), (i % S.room.W) * 16, ((i / S.room.W) | 0) * 16); }

/** Draw a whole room (art over background) into a canvas at 1:1, for thumbnails. */
function roomPicture(id) {
  const cur = S.room && S.room.id === id;
  let W, H, art, g = S.gfx;
  if (cur) ({ W, H, art } = S.room);
  else {
    const d = roomDoc(id); W = d.size.width; H = d.size.height;
    try { art = S.L.grid.decodeGrid(d.art, W, H); } catch (e) { art = []; }
    const keep = S.gfx, keepRoom = S.room;
    S.room = { doc: d }; loadGfx(); g = S.gfx; S.gfx = keep; S.room = keepRoom;
  }
  const cv = h('canvas', { width: W * 16, height: H * 16 }), c = cv.getContext('2d');
  paintBackground(c, g, W * 16, H * 16);
  const t = h('canvas', { width: W * 16, height: H * 16 }), tc = t.getContext('2d');
  for (let i = 0; i < art.length; i++) tc.putImageData(metaImage(art[i], g), (i % W) * 16, ((i / W) | 0) * 16);
  c.drawImage(t, 0, 0);
  return cv;
}
function paintBackground(c, g, w, hgt) {
  c.fillStyle = '#18182a'; c.fillRect(0, 0, w, hgt);
  if (!g.bg) return;
  for (let y = hgt - g.bg.height; y > -g.bg.height; y -= g.bg.height) for (let x = 0; x < w; x += g.bg.width) c.drawImage(g.bg, x, y);
}

// ------------------------------------------------- the tileset's pieces
/** For one tileset: every 16x16 piece the original rooms build with it, how
 *  often, with which collision, and how often with air above (a top edge). */
function dict(tsId) {
  let D = S.dicts.get(tsId);
  if (D) return D;
  const map = new Map(), G = S.L.grid, T = S.L.tiles;
  let rooms = 0;
  for (const [id, d] of S.vanilla.docs) {
    const doc = d.doc;
    if (!id.startsWith('room:') || doc.tileset !== tsId) continue;
    const W = doc.size.width, H = doc.size.height;
    let art, col;
    try { art = G.decodeGrid(doc.art, W, H); col = G.decodeGrid(doc.collision, W, H).map((v) => S.ctx.collisionValue(v)); } catch (e) { continue; }
    rooms++;
    for (let i = 0; i < art.length; i++) {
      let e = map.get(art[i]);
      if (!e) map.set(art[i], e = { tok: art[i], n: 0, cols: new Map(), airAbove: 0 });
      e.n++; e.cols.set(col[i], (e.cols.get(col[i]) || 0) + 1);
      if (i >= W && S.colClass[col[i - W]] === 'air') e.airAbove++;
    }
  }
  for (const e of map.values()) {
    let best = 0, bn = -1;
    for (const [v, n] of e.cols) if (n > bn) { best = v; bn = n; }
    e.col = best; e.cls = S.colClass[best]; e.top = e.airAbove / e.n;
    e.tok = T.metaToken(T.metaValues(e.tok));
  }
  const list = [...map.values()].sort((a, b) => b.n - a.n);
  const norm = new Map(list.map((e) => [e.tok, e]));
  const air = list.find((e) => e.cls === 'air');
  const solids = list.filter((e) => e.cls === 'solid');
  D = {
    map: norm, list, rooms,
    empty: air ? air.tok : '000.0 000.0 000.0 000.0',
    fill: (solids.find((e) => e.top < 0.15) || solids[0] || {}).tok,
    top: (solids.find((e) => e.top > 0.6) || solids[0] || {}).tok,
  };
  S.dicts.set(tsId, D);
  return D;
}

/** The (tileset, palette, background) combinations the original game uses. */
function looks() {
  if (S.looks) return S.looks;
  const m = new Map();
  for (const [id, d] of S.vanilla.docs) {
    if (!id.startsWith('room:')) continue;
    const doc = d.doc, key = doc.tileset + '|' + doc.palette + '|' + doc.background;
    let e = m.get(key);
    if (!e) m.set(key, e = { key, tileset: doc.tileset, palette: doc.palette, background: doc.background, music: doc.music, n: 0, area: id.slice(5).split('/')[0], room: doc.bind && doc.bind.roomId });
    e.n++;
  }
  S.looks = [...m.values()].sort((a, b) => (a.area < b.area ? -1 : a.area > b.area ? 1 : b.n - a.n));
  for (const e of S.looks) e.label = `${nice(e.area)} — like room ${e.room} (${e.n} room${e.n > 1 ? 's' : ''})`;
  return S.looks;
}

// ---------------------------------------------------------------- the view
let renderQueued = false;
function requestRender() { if (!renderQueued) { renderQueued = true; requestAnimationFrame(() => { renderQueued = false; render(); }); } }

function sizeCanvas() {
  const R = S.room, cv = $('#cv');
  const cw = R.W * 16 * S.zoom, ch = R.H * 16 * S.zoom;
  let k = Math.min(window.devicePixelRatio || 1, 2);
  while (k > 1 && cw * ch * k * k > 9e6) k -= 0.5;
  S.dpr = Math.max(1, k);
  cv.width = Math.round(cw * S.dpr); cv.height = Math.round(ch * S.dpr);
  cv.style.width = cw + 'px'; cv.style.height = ch + 'px';
  redrawArt();
  requestRender();
}

function startCell() {
  const R = S.room;
  let s = S.starts[R.id];
  if (!s) {
    s = [Math.min(2, R.W - 1), Math.max(0, R.H - 4)];
    for (const id of roomIds()) for (const d of roomDoc(id).doors || []) if (S.bind.resolve(d.to) === R.id && d.arrive) s = [d.arrive.x - 1, d.arrive.y - 1];
  }
  return [clamp(s[0], 0, R.W - 1), clamp(s[1], 0, R.H - 1)];
}

function pill(c, text, x, y, fg = '#2a1f5c', bg = '#fff') {
  c.font = '700 5px ui-rounded, system-ui, sans-serif';
  const w = c.measureText(text).width + 5;
  const px = clamp(x - w / 2, 1, S.room.W * 16 - w - 1), py = Math.max(1, y);
  c.fillStyle = bg; c.strokeStyle = '#2a1f5c'; c.lineWidth = 0.6;
  c.beginPath(); c.roundRect(px, py, w, 8, 2.500); c.fill(); c.stroke();
  c.fillStyle = fg; c.textBaseline = 'middle'; c.textAlign = 'left'; c.fillText(text, px + 2.500, py + 4.300);
}

function render() {
  const R = S.room, cv = $('#cv'), c = cv.getContext('2d');
  if (!R) return;
  const k = S.zoom * S.dpr, W = R.W * 16, H = R.H * 16;
  c.setTransform(k, 0, 0, k, 0, 0);
  c.imageSmoothingEnabled = false;
  paintBackground(c, S.gfx, W, H);
  c.drawImage(S.artCv, 0, 0);

  if (S.show.collision || S.tool === 'collision') {
    for (let i = 0; i < R.col.length; i++) {
      const col = CLASS_COLOR[S.colClass[R.col[i]]];
      if (!col) continue;
      const x = (i % R.W) * 16, y = ((i / R.W) | 0) * 16;
      c.fillStyle = `rgba(${col[0]},${col[1]},${col[2]},.38)`; c.fillRect(x, y, 16, 16);
      c.strokeStyle = `rgba(${col[0]},${col[1]},${col[2]},.85)`; c.lineWidth = 0.7; c.strokeRect(x + 0.350, y + 0.350, 15.300, 15.300);
    }
  }
  if (S.show.grid) {
    c.lineWidth = 1 / k; c.strokeStyle = 'rgba(255,255,255,.22)';
    c.beginPath();
    for (let x = 0; x <= R.W; x++) { c.moveTo(x * 16, 0); c.lineTo(x * 16, H); }
    for (let y = 0; y <= R.H; y++) { c.moveTo(0, y * 16); c.lineTo(W, y * 16); }
    c.stroke();
  }
  const [sx, sy] = startCell();
  if (S.show.camera) {
    const cx = clamp(sx * 16 + 8 - 120, 0, Math.max(0, W - 240)), cy = clamp(sy * 16 + 8 - 100, 0, Math.max(0, H - 160));
    c.strokeStyle = '#fff'; c.lineWidth = 1.200; c.setLineDash([5, 3]); c.strokeRect(cx, cy, Math.min(240, W), Math.min(160, H)); c.setLineDash([]);
    pill(c, '240 × 160 (game screen)', cx + 46, cy + 3, '#fff', '#2a1f5c');
  }
  // doors
  R.doc.doors.forEach((d, i) => {
    const on = S.sel && S.sel.kind === 'door' && S.sel.i === i;
    for (const [x, y] of d.cells || []) {
      c.strokeStyle = on ? '#ffd23f' : '#fff'; c.lineWidth = on ? 1.800 : 1.200;
      c.strokeRect(x * 16 + 1, y * 16 + 1, 14, 14);
    }
    if (d.cells && d.cells[0]) pill(c, '→ ' + targetLabel(d.to), d.cells[0][0] * 16 + 8, d.cells[0][1] * 16 - 10);
  });
  // objects
  if (S.show.objects || S.tool === 'object') {
    R.doc.objects.forEach((o, i) => {
      const name = typeof o.type === 'number' ? S.vocab.objectTypeName[o.type] || String(o.type) : o.type;
      const info = S.objInfo.get(name) || { category: 'other' };
      const on = S.sel && S.sel.kind === 'object' && S.sel.i === i;
      c.fillStyle = CAT_COLOR[info.category] || CAT_COLOR.other; c.strokeStyle = on ? '#ffd23f' : '#2a1f5c'; c.lineWidth = on ? 1.800 : 1;
      c.beginPath(); c.roundRect(o.x - 7, o.y - 7, 14, 14, 4); c.fill(); c.stroke();
      c.fillStyle = '#1b1440'; c.font = '900 6px ui-rounded, system-ui, sans-serif'; c.textAlign = 'center'; c.textBaseline = 'middle';
      c.fillText(initials(name), o.x, o.y + 0.500);
      if (on || S.zoom >= 3) pill(c, name, o.x, o.y - 17);
    });
  }
  // the start flag
  c.fillStyle = '#4cd964'; c.strokeStyle = '#12301a'; c.lineWidth = 0.800;
  c.beginPath(); c.moveTo(sx * 16 + 4, sy * 16 + 15); c.lineTo(sx * 16 + 4, sy * 16 + 1); c.lineTo(sx * 16 + 14, sy * 16 + 4.500); c.lineTo(sx * 16 + 5, sy * 16 + 8); c.lineTo(sx * 16 + 5, sy * 16 + 15); c.closePath(); c.fill(); c.stroke();
  // hover / drag previews
  if (S.drag && S.drag.rect) {
    const [x0, y0, x1, y1] = S.drag.rect;
    c.strokeStyle = '#ffd23f'; c.lineWidth = 1.500; c.setLineDash([4, 2]);
    c.strokeRect(Math.min(x0, x1) * 16, Math.min(y0, y1) * 16, (Math.abs(x1 - x0) + 1) * 16, (Math.abs(y1 - y0) + 1) * 16); c.setLineDash([]);
  } else if (S.hover && S.tool !== 'select') {
    c.strokeStyle = S.pick ? '#4cd964' : '#ffd23f'; c.lineWidth = 1.200; c.strokeRect(S.hover[0] * 16 + 0.500, S.hover[1] * 16 + 0.500, 15, 15);
  }
  if (S.pick && S.hover) pill(c, 'arrive here', S.hover[0] * 16 + 8, S.hover[1] * 16 - 10, '#12301a', '#4cd964');
}
const initials = (name) => { const p = String(name).split('_').filter((w) => w && !/^\d+$/.test(w)); return ((p[0] || '?')[0] + (p[1] ? p[1][0] : (p[0] || '')[1] || '')).toUpperCase(); };

function setZoom(z, anchor) {
  z = clamp(z, 1, 6);
  if (z === S.zoom) return;
  const sc = $('#scroll'), old = S.zoom;
  const ax = anchor ? anchor[0] : sc.clientWidth / 2, ay = anchor ? anchor[1] : sc.clientHeight / 2;
  const rx = (sc.scrollLeft + ax) / old, ry = (sc.scrollTop + ay) / old;
  S.zoom = z; lsSet('katam.ed.zoom', z);
  sizeCanvas();
  sc.scrollLeft = rx * z - ax; sc.scrollTop = ry * z - ay;
  $('#zoomsel').value = String(z);
  refreshStatus();
}

// ------------------------------------------------------------------- editing
const idx = (x, y) => y * S.room.W + x;
const inRoom = (x, y) => x >= 0 && y >= 0 && x < S.room.W && y < S.room.H;
function setCell(x, y, tok, col) {
  if (!inRoom(x, y)) return;
  const R = S.room, i = idx(x, y);
  if (tok !== null && R.art[i] !== tok) { R.art[i] = tok; drawCell(i); }
  if (col !== null) R.col[i] = col;
}
function applyTool(x, y) {
  const D = dict(S.room.doc.tileset);
  if (S.tool === 'brush' || S.tool === 'rect') setCell(x, y, S.tile, S.followCol ? S.tileCol : null);
  else if (S.tool === 'eraser') setCell(x, y, D.empty, 0);
  else if (S.tool === 'collision') setCell(x, y, null, S.colBrush);
}
function line(x0, y0, x1, y1, fn) {
  const dx = Math.abs(x1 - x0), dy = -Math.abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  let e = dx + dy;
  for (;;) { fn(x0, y0); if (x0 === x1 && y0 === y1) break; const e2 = 2 * e; if (e2 >= dy) { e += dy; x0 += sx; } if (e2 <= dx) { e += dx; y0 += sy; } }
}
function flood(x, y) {
  const R = S.room, from = R.art[idx(x, y)], fromCol = R.col[idx(x, y)];
  if (from === S.tile && (!S.followCol || fromCol === S.tileCol)) return;
  const seen = new Uint8Array(R.W * R.H), stack = [[x, y]];
  while (stack.length) {
    const [a, b] = stack.pop();
    if (!inRoom(a, b)) continue;
    const i = idx(a, b);
    if (seen[i] || R.art[i] !== from || R.col[i] !== fromCol) continue;
    seen[i] = 1; setCell(a, b, S.tile, S.followCol ? S.tileCol : null);
    stack.push([a + 1, b], [a - 1, b], [a, b + 1], [a, b - 1]);
  }
}
const selected = () => (S.sel ? (S.sel.kind === 'object' ? S.room.doc.objects[S.sel.i] : S.room.doc.doors[S.sel.i]) : null);
function objectAt(px, py) {
  const o = S.room.doc.objects;
  for (let i = o.length - 1; i >= 0; i--) if (Math.abs(o[i].x - px) <= 8 && Math.abs(o[i].y - py) <= 8) return i;
  return -1;
}
const doorAt = (x, y) => S.room.doc.doors.findIndex((d) => (d.cells || []).some((c) => c[0] === x && c[1] === y));
function uniqueId(base) {
  const ids = new Set(S.room.doc.objects.map((o) => o.id));
  base = base.replace(/[^a-z0-9_]/g, '_');
  for (let n = 1; ; n++) { const id = n === 1 && !/\d$/.test(base) ? base : `${base.replace(/_?\d+$/, '')}_${n}`; if (!ids.has(id)) return id; }
}
function nextDespawn() {
  let m = 0;
  for (const o of S.room.doc.objects) m = Math.max(m, o.despawnBit || 0, (o.raw && o.raw.unk3) || 0);
  return m + 1;
}
function addObject(type, x, y) {
  const o = { id: uniqueId(type), type, x: x * 16 + 8, y: y * 16 + 8, despawnBit: nextDespawn() };
  S.room.doc.objects.push(o);
  return S.room.doc.objects.length - 1;
}
function addDoor(x, y) {
  const R = S.room, others = roomIds().filter((id) => id !== R.id);
  const to = others[0] || R.id, td = roomDoc(to);
  let arrive = { x: Math.min(3, td.size.width - 1), y: Math.max(1, td.size.height - 3) };
  const back = (td.doors || []).find((d) => S.bind.resolve(d.to) === R.id && d.cells && d.cells[0]);
  if (back) arrive = { x: back.cells[0][0], y: back.cells[0][1] + 1 };
  R.doc.doors.push({ to, arrive, cells: [[x, y]] });
  setCell(x, y, null, S.colClass[255] === 'door' || S.colRep.door === undefined ? 255 : S.colRep.door);
  if (objectAt(x * 16 + 8, y * 16 + 8) < 0) addObject('door', x, y);
  return R.doc.doors.length - 1;
}
function deleteSelected() {
  const R = S.room, s = S.sel;
  if (!s) return;
  pushUndo();
  if (s.kind === 'object') R.doc.objects.splice(s.i, 1);
  else {
    const d = R.doc.doors[s.i];
    for (const [x, y] of d.cells || []) {
      if (inRoom(x, y) && S.colClass[R.col[idx(x, y)]] === 'door') setCell(x, y, null, 0);
      const oi = objectAt(x * 16 + 8, y * 16 + 8);
      if (oi >= 0 && (S.objInfo.get(R.doc.objects[oi].type) || {}).category === 'door') R.doc.objects.splice(oi, 1);
    }
    R.doc.doors.splice(s.i, 1);
  }
  S.sel = null; touch(); refreshInspector(); refreshMap();
}

function setTool(t) {
  S.tool = t;
  $$('#tools .tool[data-tool]').forEach((b) => b.classList.toggle('on', b.dataset.tool === t));
  $('#cv').style.cursor = t === 'select' ? 'default' : 'crosshair';
  refreshLegend(); requestRender();
}

// pointer input: one finger / the mouse uses the tool, two fingers pan
const pointers = new Map();
function cellOf(e) {
  const r = $('#cv').getBoundingClientRect();
  const px = (e.clientX - r.left) / S.zoom, py = (e.clientY - r.top) / S.zoom;
  return { px, py, x: Math.floor(px / 16), y: Math.floor(py / 16) };
}
function onDown(e) {
  const cv = $('#cv'), sc = $('#scroll');
  document.body.classList.remove('side-open', 'bottom-open');
  pointers.set(e.pointerId, [e.clientX, e.clientY]);
  cv.setPointerCapture(e.pointerId);
  if (pointers.size === 2) {           // a second finger: this is a pan, not a stroke
    if (S.drag && S.drag.undone !== true && S.drag.changed) { restore(S.undo.pop()); refreshUndo(); }
    S.drag = { pan: true };
    return;
  }
  const p = cellOf(e);
  if (e.button === 1 || e.button === 2 || S.space) { S.drag = { pan: true, mouse: [e.clientX, e.clientY, sc.scrollLeft, sc.scrollTop] }; return; }
  if (!inRoom(p.x, p.y)) return;
  if (S.pick) return finishPick(p.x, p.y);
  const R = S.room;
  switch (S.tool) {
    case 'select': {
      const [sx, sy] = startCell();
      const oi = S.show.objects ? objectAt(p.px, p.py) : -1, di = doorAt(p.x, p.y);
      if (oi >= 0) { S.sel = { kind: 'object', i: oi }; S.drag = { move: 'object', first: true }; }
      else if (di >= 0) { S.sel = { kind: 'door', i: di }; S.drag = {}; }
      else if (p.x === sx && p.y === sy) { S.sel = null; S.drag = { move: 'start' }; }
      else { S.sel = null; S.drag = { pan: true, mouse: [e.clientX, e.clientY, sc.scrollLeft, sc.scrollTop] }; }
      refreshInspector(); requestRender();
      break;
    }
    case 'brush': case 'eraser': case 'collision':
      pushUndo(); S.drag = { paint: true, last: [p.x, p.y], changed: true };
      applyTool(p.x, p.y); touch();
      break;
    case 'rect': pushUndo(); S.drag = { rect: [p.x, p.y, p.x, p.y], changed: true }; requestRender(); break;
    case 'fill': pushUndo(); flood(p.x, p.y); touch(); break;
    case 'eyedropper': {
      const i = idx(p.x, p.y);
      S.tile = R.art[i]; S.tileCol = R.col[i]; S.colBrush = R.col[i];
      setTool('brush'); refreshTab(); toast('Picked this tile and its collision.');
      break;
    }
    case 'object': {
      const oi = objectAt(p.px, p.py);
      if (oi >= 0) S.sel = { kind: 'object', i: oi };
      else { pushUndo(); S.sel = { kind: 'object', i: addObject(S.objType, p.x, p.y) }; touch(); }
      S.drag = { move: 'object', first: oi >= 0 };
      refreshInspector(); requestRender();
      break;
    }
    case 'door': {
      let di = doorAt(p.x, p.y);
      if (di < 0) { pushUndo(); di = addDoor(p.x, p.y); touch(); refreshMap(); }
      S.sel = { kind: 'door', i: di }; refreshInspector(); requestRender();
      break;
    }
  }
}
function onMove(e) {
  const sc = $('#scroll');
  const prev = pointers.get(e.pointerId);
  if (prev && pointers.size === 2) {     // two-finger pan
    sc.scrollLeft -= (e.clientX - prev[0]) / 2; sc.scrollTop -= (e.clientY - prev[1]) / 2;
    pointers.set(e.pointerId, [e.clientX, e.clientY]);
    return;
  }
  if (prev) pointers.set(e.pointerId, [e.clientX, e.clientY]);
  const p = cellOf(e), d = S.drag;
  const hv = inRoom(p.x, p.y) ? [p.x, p.y] : null;
  if (!S.hover || !hv || S.hover[0] !== hv[0] || S.hover[1] !== hv[1]) { S.hover = hv; refreshStatus(); requestRender(); }
  if (!d || !prev) return;
  if (d.pan && d.mouse) { sc.scrollLeft = d.mouse[2] - (e.clientX - d.mouse[0]); sc.scrollTop = d.mouse[3] - (e.clientY - d.mouse[1]); return; }
  const x = clamp(p.x, 0, S.room.W - 1), y = clamp(p.y, 0, S.room.H - 1);
  if (d.paint) { line(d.last[0], d.last[1], x, y, applyTool); d.last = [x, y]; touch(); }
  else if (d.rect) { d.rect[2] = x; d.rect[3] = y; requestRender(); }
  else if (d.move === 'object' && S.sel) {
    const o = selected();
    if (o && (o.x !== x * 16 + 8 || o.y !== y * 16 + 8)) { if (d.first) { pushUndo(); d.first = false; } o.x = x * 16 + 8; o.y = y * 16 + 8; touch(); refreshInspector(true); }
  } else if (d.move === 'start') { S.starts[S.room.id] = [x, y]; lsSet('katam.ed.starts', S.starts); requestRender(); }
}
function onUp(e) {
  pointers.delete(e.pointerId);
  const d = S.drag;
  if (pointers.size) return;
  S.drag = null;
  if (d && d.rect) {
    const [x0, y0, x1, y1] = d.rect;
    for (let y = Math.min(y0, y1); y <= Math.max(y0, y1); y++) for (let x = Math.min(x0, x1); x <= Math.max(x0, x1); x++) applyTool(x, y);
    touch();
  }
  requestRender();
}

// ------------------------------------------------------------------ dialogs
function modal(title, body, actions) {
  const m = $('#modal');
  const close = () => { m.hidden = true; m.replaceChildren(); };
  setKids(m, h('div', { class: 'box' }, h('h2', null, title), body,
    h('div', { class: 'acts' }, actions.map((a) => h('button', { class: 'btn' + (a.primary ? ' primary' : '') + (a.danger ? ' danger' : ''), onclick: () => { if (!a.run || a.run() !== false) close(); } }, a.label)))));
  m.hidden = false;
  return close;
}
const confirmBox = (title, text, yes, run) => modal(title, h('p', null, text), [{ label: 'Cancel' }, { label: yes, danger: true, run }]);

function lookSelect(cur) {
  return h('select', null, looks().map((l) => h('option', { value: l.key, selected: l.key === cur }, l.label)));
}
function musicSelect(cur) {
  const songs = Object.keys(S.vocab.songs).filter((s) => s.startsWith('mus_'));
  if (cur && !songs.includes(cur)) songs.unshift(cur);
  return h('select', null, songs.map((s) => h('option', { value: s, selected: s === cur }, nice(String(s).replace(/^mus_/, '')))));
}
const slug = (s) => String(s).toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '') || 'room';
function namespace() {
  const ids = roomIds();
  if (ids.length) return ids[0].slice(5).split('/')[0];
  return slug((S.proj.project && S.proj.project.id || 'my_levels').split('.').pop());
}

function newRoomDialog() {
  const cur = S.room ? S.room.doc : null;
  const name = h('input', { type: 'text', value: 'New room', maxlength: 40 });
  const w = h('input', { type: 'number', min: 15, max: 190, value: 30 }), hh = h('input', { type: 'number', min: 10, max: 120, value: 14 });
  const look = lookSelect(cur ? cur.tileset + '|' + cur.palette + '|' + cur.background : 'tileset:vanilla/29|palette:vanilla/29|background:vanilla/28');
  modal('New room', h('div', { class: 'form' },
    h('label', null, 'Name'), name,
    h('label', null, 'Size'), h('div', { class: 'row' }, w, '×', hh, h('span', { class: 'dim' }, 'tiles (16 px each)')),
    h('label', null, 'Looks like'), look,
    h('p', { class: 'dim wide' }, 'The look sets the tile set, its colours and the background, taken from a room of the original game.')),
  [{ label: 'Cancel' }, { label: 'Create room', primary: true, run: () => {
    const W = clamp(Number(w.value) | 0, 15, 190), H = clamp(Number(hh.value) | 0, 10, 120);
    if (W * H > 1950) { toast(`${W} × ${H} is ${W * H} tiles; the game holds 1950 per room.`); return false; }
    const lk = looks().find((l) => l.key === look.value);
    createRoom(name.value.trim() || 'New room', W, H, lk);
  } }]);
  name.select();
}
function createRoom(name, W, H, lk) {
  flush();
  const ns = namespace();
  let s = slug(name), id = `room:${ns}/${s}`;
  for (let n = 2; S.bind.docs.has(id); n++) id = `room:${ns}/${s}_${n}`;
  const D = dict(lk.tileset), art = [], col = [];
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const floor = y >= H - 3;
    art.push(!floor ? D.empty : y === H - 3 ? D.top || D.fill || D.empty : D.fill || D.empty);
    col.push(floor ? 13 : 0);
  }
  const G = S.L.grid;
  const doc = {
    format: 'katam-room/1', id, name, size: { width: W, height: H }, music: lk.music, tileset: lk.tileset, palette: lk.palette, background: lk.background,
    art: G.encodeGrid(art, W, H, { suggest: (m) => (m === D.empty ? '.' : null) }),
    collision: G.encodeGrid(col, W, H, { suggest: (v) => S.ctx.collisionGlyph(v), toLegend: (v) => S.ctx.collisionName(v) }),
    objects: [], doors: [],
  };
  if (!S.io.exists('/project/project.json')) S.io.writeText('/project/project.json', S.L.json.writeJson({ format: 'katam-project/1', id: 'my_levels', name: 'My levels', base: 'vanilla' }));
  S.io.writeText(projPath(id), S.L.json.writeJson(doc));
  rebind(); openRoom(id); validateSoon();
  toast('Room ' + S.bind.num.get(id) + ' created.');
}

function copyVanillaDialog() {
  const ids = [...S.vanilla.docs.keys()].filter((i) => i.startsWith('room:')).sort((a, b) => S.bind.num.get(a) - S.bind.num.get(b));
  const sel = h('select', null, ids.map((id) => h('option', { value: id }, `${S.bind.num.get(id)} — ${nice(id.slice(5).split('/')[0])}`)));
  modal('Copy a room from the original game', h('div', { class: 'form' }, h('label', null, 'Room'), sel,
    h('p', { class: 'dim wide' }, 'The copy becomes a new room of your project (numbered 983 and up). Its doors still lead to the original rooms.')),
  [{ label: 'Cancel' }, { label: 'Copy room', primary: true, run: () => {
    flush();
    const src = JSON.parse(S.io.readText(S.vanilla.docs.get(sel.value).abs));
    const n = src.bind && src.bind.roomId, ns = namespace();
    let id = `room:${ns}/copy_${n}`;
    for (let k = 2; S.bind.docs.has(id); k++) id = `room:${ns}/copy_${n}_${k}`;
    delete src.bind; delete src.nav; delete src.fgLayerRoomTag;
    src.id = id; src.name = `Copy of room ${n}`;
    if (!S.io.exists('/project/project.json')) S.io.writeText('/project/project.json', S.L.json.writeJson({ format: 'katam-project/1', id: 'my_levels', name: 'My levels', base: 'vanilla' }));
    S.io.writeText(projPath(id), S.L.json.writeJson(src));
    rebind(); openRoom(id); validateSoon();
  } }]);
}

function roomSettingsDialog() {
  const R = S.room, d = R.doc;
  const name = h('input', { type: 'text', value: d.name || '', maxlength: 40 });
  const w = h('input', { type: 'number', min: 15, max: 190, value: R.W }), hh = h('input', { type: 'number', min: 10, max: 120, value: R.H });
  const anchor = h('select', null, h('option', { value: 'top' }, 'add / remove rows at the top'), h('option', { value: 'bottom' }, 'add / remove rows at the bottom'));
  const music = musicSelect(d.music);
  const key = d.tileset + '|' + d.palette + '|' + d.background;
  const look = lookSelect(key);
  if (!looks().some((l) => l.key === key)) look.prepend(h('option', { value: key, selected: true }, 'custom (keep as it is)'));
  modal('Room settings', h('div', { class: 'form' },
    h('label', null, 'Name'), name,
    h('label', null, 'Size'), h('div', { class: 'row' }, w, '×', hh, h('span', { class: 'dim' }, 'tiles')),
    h('label', null, 'Resize'), anchor,
    h('label', null, 'Music'), music,
    h('label', null, 'Looks like'), look,
    h('p', { class: 'dim wide' }, 'Changing the look swaps the tile set under the tiles you have painted: repaint the room afterwards. New columns are added on the right.')),
  [{ label: 'Delete room', danger: true, run: () => { deleteRoomDialog(); } }, { label: 'Cancel' }, { label: 'Apply', primary: true, run: () => {
    const W = clamp(Number(w.value) | 0, 15, 190), H = clamp(Number(hh.value) | 0, 10, 120);
    if (W * H > 1950) { toast(`${W} × ${H} is ${W * H} tiles; the game holds 1950 per room.`); return false; }
    pushUndo();
    d.name = name.value.trim() || d.name; d.music = music.value;
    if (look.value !== key) { const l = looks().find((x) => x.key === look.value); d.tileset = l.tileset; d.palette = l.palette; d.background = l.background; loadGfx(); S.tile = null; }
    if (W !== R.W || H !== R.H) resizeRoom(W, H, anchor.value);
    R.dirty = true; flush(); rebind(); openRoom(R.id, { keepUndo: true }); validateSoon();
  } }]);
}
function resizeRoom(W, H, anchor) {
  const R = S.room, D = dict(R.doc.tileset), dy = anchor === 'top' ? H - R.H : 0;
  const art = new Array(W * H).fill(D.empty), col = new Array(W * H).fill(0);
  for (let y = 0; y < R.H; y++) for (let x = 0; x < Math.min(W, R.W); x++) {
    const ny = y + dy;
    if (ny < 0 || ny >= H) continue;
    art[ny * W + x] = R.art[y * R.W + x]; col[ny * W + x] = R.col[y * R.W + x];
  }
  const d = R.doc;
  d.objects = d.objects.map((o) => ({ ...o, y: o.y + dy * 16 })).filter((o) => o.x < W * 16 && o.y >= 0 && o.y < H * 16);
  d.doors = d.doors.map((r) => ({ ...r, cells: (r.cells || []).map(([x, y]) => [x, y + dy]) })).filter((r) => r.cells.every(([x, y]) => x < W && y >= 0 && y < H));
  if (dy) for (const id of roomIds()) {       // doors elsewhere that arrive here move with the rows
    if (id === R.id) continue;
    const od = roomDoc(id); let ch = false;
    for (const r of od.doors || []) if (S.bind.resolve(r.to) === R.id && r.arrive) { r.arrive.y = clamp(r.arrive.y + dy, 0, H - 1); ch = true; }
    if (ch) S.io.writeText(projPath(id), S.L.json.writeJson(od));
  }
  delete d.nav;
  if (S.starts[R.id]) { S.starts[R.id][1] += dy; lsSet('katam.ed.starts', S.starts); }
  R.W = W; R.H = H; R.art = art; R.col = col;
}
function deleteRoomDialog() {
  const R = S.room;
  if (roomIds().length < 2) { toast('A project needs at least one room.'); return; }
  setTimeout(() => confirmBox('Delete this room?', `Room ${R.num} (${R.doc.name || R.id}) will be removed from the project. Doors that lead to it will need a new target.`, 'Delete room', () => {
    S.io.files.delete(R.path); S.room = null; rebind(); openRoom(roomIds()[0]); validateSoon(); persist();
  }), 0);
}

// -------------------------------------------------------------- side panels
function buildTools() {
  const T = [['select', 'Select'], ['brush', 'Tile Brush'], ['rect', 'Rectangle'], ['fill', 'Fill'], ['eraser', 'Eraser'], ['collision', 'Collision Brush'], ['object', 'Place Object'], ['door', 'Link Door'], ['eyedropper', 'Eyedropper']];
  const nav = $('#tools');
  setKids(nav, 
    ...T.map(([id, label]) => h('button', { class: 'tool', 'data-tool': id, title: label, 'aria-label': label, html: ico(id) + label, onclick: () => { setTool(id); if (id === 'object' && S.tab !== 'objects' && S.tab !== 'enemies') setTab('enemies'); if (id === 'brush' || id === 'rect' || id === 'fill') setTab('tiles'); } })),
    h('span', { class: 'gap' }),
    h('button', { class: 'tool sub', title: 'Layers', html: ico('layers') + 'Layers', onclick: (e) => openMenu(e.currentTarget, 'view') }),
    h('button', { class: 'tool sub', title: 'Area Map', html: ico('map') + 'Area Map', onclick: () => { document.body.classList.toggle('bottom-open'); $('#areamap').scrollIntoView({ block: 'nearest' }); } }),
    h('button', { class: 'tool sub', title: 'Room settings', html: ico('gear') + 'Settings', onclick: roomSettingsDialog }));
}

function setTab(t) {
  S.tab = t; S.search = '';
  $$('.tab').forEach((b) => b.classList.toggle('on', b.dataset.tab === t));
  refreshTab();
}
const TILE_FILTERS = [['all', 'All'], ['solid', 'Ground'], ['platform', 'Platforms'], ['slope', 'Slopes'], ['hazard', 'Hazards'], ['air', 'Scenery'], ['misc', 'Other'], ['used', 'In this room']];
function refreshTab() {
  const body = $('#tabbody');
  if (S.tab === 'tiles') {
    const D = dict(S.room.doc.tileset);
    const used = S.filter === 'used' ? new Set(S.room.art) : null;
    let list = D.list.filter((e) => S.filter === 'all' ? true : S.filter === 'used' ? used.has(e.tok) : S.filter === 'misc' ? !['solid', 'platform', 'slope', 'hazard', 'air'].includes(e.cls) : e.cls === S.filter);
    if (used) for (const t of used) if (!D.map.has(t)) list.push({ tok: t, n: 0, col: 0, cls: 'air' });
    const shown = list.slice(0, 360);
    const grid = h('div', { class: 'tilegrid' }, shown.map((e) => {
      const cv = h('canvas', { width: 16, height: 16, class: e.tok === S.tile ? 'on' : '', title: `${CLASSES.find((c) => c[0] === e.cls)[1]} · used ${e.n}× in the original game` });
      cv.getContext('2d').putImageData(metaImage(e.tok), 0, 0);
      cv.onclick = () => { S.tile = e.tok; S.tileCol = e.col; if (!['brush', 'rect', 'fill'].includes(S.tool)) setTool('brush'); refreshTab(); };
      return cv;
    }));
    const info = h('div', { class: 'tileinfo' });
    const pv = h('canvas', { width: 16, height: 16 }); pv.getContext('2d').putImageData(metaImage(S.tile), 0, 0);
    const e = D.map.get(S.tile);
    const csel = collisionSelect(S.tileCol, (v) => { S.tileCol = v; refreshTab(); });
    info.append(pv, h('b', null, `Tile piece · ${CLASSES.find((c) => c[0] === S.colClass[S.tileCol])[1]}`),
      h('span', { class: 'dim' }, `16 × 16 px (2 × 2 tiles)${e ? ` · used ${e.n}× in the original game` : ''}`),
      h('div', { class: 'form' }, h('label', null, 'Collision:'), csel,
        h('label', { class: 'chk wide' }, h('input', { type: 'checkbox', checked: S.followCol, onchange: (ev) => { S.followCol = ev.target.checked; } }), h('span', null, 'Paint the collision with the tile'))));
    setKids(body, 
      h('div', { class: 'filters' }, TILE_FILTERS.map(([id, label]) => h('button', { class: 'btn', 'aria-pressed': String(S.filter === id), onclick: () => { S.filter = id; refreshTab(); } }, label))),
      h('div', { class: 'scrolly' }, grid, list.length > shown.length ? h('p', { class: 'dim' }, `Showing the ${shown.length} most used of ${list.length}.`) : null,
        list.length ? null : h('p', { class: 'dim' }, 'No pieces of this kind in this tile set.')), info);
  } else if (S.tab === 'objects' || S.tab === 'enemies') {
    const want = S.tab === 'enemies' ? (c) => ['enemy', 'miniboss', 'boss'].includes(c) : (c) => !['enemy', 'miniboss', 'boss'].includes(c);
    const q = S.search.toLowerCase();
    const list = [...S.objInfo].filter(([n, i]) => want(i.category) && (!q || n.includes(q) || nice(n).includes(q))).sort((a, b) => (CAT_ORDER.indexOf(a[1].category) - CAT_ORDER.indexOf(b[1].category)) || a[1].num - b[1].num);
    const grid = h('div', { class: 'objgrid' }, list.map(([n, i]) => h('button', { class: 'obj' + (n === S.objType ? ' on' : ''), title: i.description || nice(n), onclick: () => { S.objType = n; setTool('object'); refreshTab(); } },
      h('i', { class: 'badge', style: `background:${CAT_COLOR[i.category] || CAT_COLOR.other}` }, initials(n)), h('span', null, nice(n)))));
    const search = h('input', { type: 'search', placeholder: 'Search…', value: S.search, oninput: (e) => { S.search = e.target.value; refreshTab(); const s = $('#tabbody input[type=search]'); s.focus(); s.setSelectionRange(s.value.length, s.value.length); } });
    setKids(body, search, h('div', { class: 'scrolly' }, grid), h('p', { class: 'dim', style: 'margin:0' }, 'Pick one, then tap the room to place it. Pictures of the game’s sprites are not shown yet: each kind has a coloured badge.'));
  } else {
    setKids(body, 
      ...roomIds().map((id) => h('div', { class: 'roomrow' + (id === S.room.id ? ' on' : ''), onclick: () => openRoom(id) }, h('b', null, 'Room ' + roomLabel(id)), h('span', { class: 'dim' }, (() => { const d = roomDoc(id); return `${d.size.width} × ${d.size.height}`; })()))),
      h('div', { class: 'row', style: 'display:flex;gap:6px;flex-wrap:wrap' },
        h('button', { class: 'btn', onclick: newRoomDialog }, '+ New room'),
        h('button', { class: 'btn', onclick: copyVanillaDialog }, 'Copy an original room…'),
        h('button', { class: 'btn', onclick: roomSettingsDialog }, 'Room settings…')));
  }
}
function collisionSelect(cur, onchange) {
  const sel = h('select', { onchange: (e) => onchange(Number(e.target.value)) });
  for (const [cls, label] of CLASSES) {
    const vals = Object.entries(S.colNames).filter(([v]) => S.colClass[v] === cls).sort((a, b) => (b[1].cells || 0) - (a[1].cells || 0));
    if (!vals.length) continue;
    sel.append(h('optgroup', { label }, vals.map(([v, e]) => h('option', { value: v, selected: Number(v) === cur }, nice(e.name)))));
  }
  if (!S.colNames[cur]) sel.prepend(h('option', { value: cur, selected: true }, 'value ' + cur));
  return sel;
}
function refreshLegend() {
  const leg = $('#legend');
  const chips = CLASSES.filter(([cls]) => S.colRep[cls] !== undefined && cls !== 'door' && cls !== 'other').map(([cls, label, col]) =>
    h('button', { class: 'chip' + (S.tool === 'collision' && S.colClass[S.colBrush] === cls ? ' on' : ''), title: 'Collision brush: ' + label, onclick: () => { S.colBrush = S.colRep[cls]; setTool('collision'); } },
      h('i', { style: col ? `background:rgba(${col},.75)` : 'background:repeating-linear-gradient(45deg,#fff3,#fff3 3px,#0000 3px,#0000 6px)' }), label));
  setKids(leg, h('b', null, 'Collision:'), ...chips,
    S.tool === 'collision' ? collisionSelect(S.colBrush, (v) => { S.colBrush = v; refreshLegend(); }) : null);
}

function refreshInspector(light) {
  const box = $('#inspector'), R = S.room, s = S.sel, o = selected();
  if (light && box.dataset.for === (s ? s.kind + s.i : '')) { const xi = $('#ins-x', box), yi = $('#ins-y', box); if (xi && o) { xi.value = (o.x - 8) / 16; yi.value = (o.y - 8) / 16; } return; }
  box.dataset.for = s ? s.kind + s.i : '';
  if (!o) { box.replaceChildren(); return; }
  const change = (fn) => (e) => { pushUndo(); fn(e.target); touch(); requestRender(); };
  if (s.kind === 'object') {
    const name = typeof o.type === 'number' ? S.vocab.objectTypeName[o.type] || o.type : o.type, info = S.objInfo.get(name) || { category: 'other', params: [] };
    const types = [...S.objInfo.keys()].sort();
    const rows = [
      h('label', null, 'Type:'), h('select', { onchange: change((t) => { o.type = t.value; delete o.params; refreshInspector(); }) }, types.map((t) => h('option', { value: t, selected: t === name }, nice(t)))),
      h('label', null, 'x:'), h('div', { class: 'row' }, h('input', { id: 'ins-x', type: 'number', step: 0.5, value: (o.x - 8) / 16, onchange: change((t) => { o.x = Math.round(Number(t.value) * 16 + 8); }) }), 'tiles'),
      h('label', null, 'y:'), h('div', { class: 'row' }, h('input', { id: 'ins-y', type: 'number', step: 0.5, value: (o.y - 8) / 16, onchange: change((t) => { o.y = Math.round(Number(t.value) * 16 + 8); }) }), 'tiles'),
    ];
    for (const p of info.params) {
      const v = o.params && o.params[p.name] !== undefined ? o.params[p.name] : '';
      rows.push(h('label', { title: p.description || '' }, nice(p.name) + ':'),
        h('input', { type: 'number', value: typeof v === 'number' ? v : '', placeholder: 'default', title: p.description || '', min: p.range ? p.range[0] : null, max: p.range ? p.range[1] : null,
          onchange: change((t) => { o.params = o.params || {}; if (t.value === '') delete o.params[p.name]; else o.params[p.name] = Number(t.value) | 0; if (!Object.keys(o.params).length) delete o.params; }) }));
    }
    setKids(box, h('h3', null, ['enemy', 'miniboss', 'boss'].includes(info.category) ? 'Selected Enemy' : 'Selected Object'),
      h('div', { style: 'display:flex;gap:10px;align-items:flex-start' }, h('i', { class: 'badge', style: `background:${CAT_COLOR[info.category] || CAT_COLOR.other}` }, initials(name)), h('div', { class: 'form', style: 'flex:1' }, rows)),
      info.description ? h('p', { class: 'dim' }, info.description) : null,
      h('button', { class: 'btn danger tiny', onclick: deleteSelected }, 'Delete'));
  } else {
    const ids = roomIds(), r = S.bind.resolve(o.to), inProj = r && S.proj.docs.has(r);
    const sel = h('select', { onchange: change((t) => {
      if (t.value === '@vanilla') { const n = prompt('Original room number (for example 803):', (r && !inProj ? S.bind.num.get(r) : '803')); const id = n && S.bind.resolve('room:vanilla/' + (Number(n) | 0)); if (!id) { toast('No original room has that number.'); } else o.to = id; }
      else { o.to = t.value; const td = roomDoc(o.to); o.arrive = { x: clamp(o.arrive.x, 0, td.size.width - 1), y: clamp(o.arrive.y, 1, td.size.height - 1) }; }
      refreshInspector(); refreshMap();
    }) }, ids.map((id) => h('option', { value: id, selected: id === r }, 'Room ' + roomLabel(id))),
    !inProj && r ? h('option', { value: r, selected: true }, targetLabel(o.to)) : null, h('option', { value: '@vanilla' }, 'An original room…'));
    setKids(box, h('h3', null, 'Selected Door'), h('div', { class: 'form' },
      h('label', null, 'Leads to:'), sel,
      h('label', null, 'Arrive at:'), h('div', { class: 'row' },
        h('input', { type: 'number', value: o.arrive.x - 1, min: 0, onchange: change((t) => { o.arrive.x = (Number(t.value) | 0) + 1; }) }),
        h('input', { type: 'number', value: o.arrive.y - 1, min: 0, onchange: change((t) => { o.arrive.y = (Number(t.value) | 0) + 1; }) }),
        inProj ? h('button', { class: 'btn tiny', onclick: () => startPick(s.i) }, 'Pick…') : null),
      h('p', { class: 'dim wide' }, 'Kirby comes out standing on the arrival tile of the target room. Press Up at the door to go through.')),
      h('button', { class: 'btn danger tiny', onclick: deleteSelected }, 'Delete door'));
  }
}
function startPick(doorIndex) {
  const from = S.room.id, to = S.bind.resolve(S.room.doc.doors[doorIndex].to);
  flush();
  S.pick = { from, doorIndex };
  if (to !== from) openRoom(to, { keepUndo: false });
  setTool('door'); status('Tap the tile where Kirby should arrive.'); toast('Tap the tile where Kirby should arrive.', 4000);
}
function finishPick(x, y) {
  const { from, doorIndex } = S.pick; S.pick = null;
  if (S.room.id !== from) openRoom(from);
  pushUndo();
  S.room.doc.doors[doorIndex].arrive = { x: x + 1, y: y + 1 };
  S.sel = { kind: 'door', i: doorIndex };
  touch(); refreshInspector(); refreshStatus();
}

function refreshRooms() {
  const sel = $('#roomsel');
  sel.replaceChildren(...roomIds().map((id) => h('option', { value: id, selected: id === S.room.id }, 'Room: ' + roomLabel(id))));
}
function refreshMap() {
  const ids = roomIds(), nodes = new Map(), edges = [];
  for (const id of ids) nodes.set(id, { id, label: String(S.bind.num.get(id)), ext: false });
  for (const id of ids) for (const d of roomDoc(id).doors || []) {
    const r = S.bind.resolve(d.to) || d.to;
    if (!nodes.has(r)) nodes.set(r, { id: r, label: String(S.bind.num.get(r) ?? '?'), ext: true });
    if (r !== id) edges.push([id, r]);
  }
  // columns by distance from the first room
  const depth = new Map(); let col = 0;
  for (const start of nodes.keys()) {
    if (depth.has(start)) continue;
    const q = [[start, col]];
    while (q.length) {
      const [n, dpt] = q.shift();
      if (depth.has(n)) continue;
      depth.set(n, dpt); col = Math.max(col, dpt + 1);
      for (const [a, b] of edges) { if (a === n && !depth.has(b)) q.push([b, dpt + 1]); if (b === n && !depth.has(a) && !nodes.get(n).ext) q.push([a, dpt + 1]); }
    }
  }
  const rows = new Map();
  for (const n of nodes.values()) { const c = depth.get(n.id), r = rows.get(c) || 0; rows.set(c, r + 1); n.x = 12 + c * 96; n.y = 8 + r * 40; }
  const w = Math.max(...[...nodes.values()].map((n) => n.x)) + 86, hgt = Math.max(...[...nodes.values()].map((n) => n.y)) + 38;
  const NS = 'http://www.w3.org/2000/svg', svg = document.createElementNS(NS, 'svg');
  svg.setAttribute('width', w); svg.setAttribute('height', hgt); svg.setAttribute('viewBox', `0 0 ${w} ${hgt}`);
  const el = (t, a, txt) => { const e = document.createElementNS(NS, t); for (const [k, v] of Object.entries(a)) e.setAttribute(k, v); if (txt) e.textContent = txt; return e; };
  for (const [a, b] of edges) { const A = nodes.get(a), B = nodes.get(b); svg.append(el('line', { x1: A.x + 37, y1: A.y + 14, x2: B.x + 37, y2: B.y + 14 })); }
  for (const n of nodes.values()) {
    const g = el('g', { class: 'node' + (n.id === S.room.id ? ' cur' : '') + (n.ext ? ' ext' : '') });
    g.append(el('rect', { x: n.x, y: n.y, width: 74, height: 28, rx: 8 }), el('text', { x: n.x + 37, y: n.y + 14 }, n.label));
    if (!n.ext) g.addEventListener('click', () => openRoom(n.id));
    svg.append(g);
  }
  setKids($('#areamap .body'), svg, h('p', { class: 'dim', style: 'margin:4px 0 0' }, 'Yellow: this room · dashed: a room of the original game · lines: doors'));
}
function refreshThumbs() {
  const body = $('#thumbs .body');
  setKids(body, ...roomIds().map((id) => {
    const pic = roomPicture(id);
    return h('div', { class: 'thumb' + (id === S.room.id ? ' on' : ''), onclick: () => openRoom(id) }, pic, roomLabel(id));
  }), h('div', { class: 'thumb new', onclick: newRoomDialog }, h('div', null, h('div', { style: 'font-size:22px' }, '+'), 'New room')));
}
const thumbsSoon = debounce(refreshThumbs, 900);
function refreshIssues() {
  const f = S.findings, n = f.errors.length + f.warnings.length;
  $('#issues h3').textContent = `Validation (${n ? n + ' issue' + (n > 1 ? 's' : '') : 'no issues'})`;
  const row = (sev) => (e) => h('div', { class: 'issue ' + sev }, sev === 'error' ? '⛔' : '⚠', h('div', null, `${e.code ? e.code + ': ' : ''}${e.message}`, h('small', null, [e.file, e.where, e.hint ? 'hint: ' + e.hint : ''].filter(Boolean).join(' · '))));
  setKids($('#issues .body'), ...f.errors.map(row('error')), ...f.warnings.map(row('warning')), n ? null : h('p', { class: 'allgood' }, '✓ This project is ready to play.'));
  const dot = $('#vdot');
  dot.className = 'dot ' + (f.errors.length ? 'err' : f.warnings.length ? 'warn' : 'ok');
  dot.textContent = f.errors.length ? String(f.errors.length) : f.warnings.length ? String(f.warnings.length) : '✓';
}
function refreshStatus() {
  const R = S.room;
  if (!R) return;
  if (S.pick) return;
  const used = new Set();
  for (const t of R.art) for (const p of t.split(' ')) used.add(p.slice(0, 3));
  status(`Room ${R.num} · ${R.W} × ${R.H} tiles · ${nice(R.doc.tileset.replace('tileset:vanilla/', 'tileset '))}: ${used.size}/${S.gfx.tileCount || '?'} tiles · zoom ${S.zoom * 100}%${S.hover ? ` · tile ${S.hover[0]}, ${S.hover[1]}` : ''}`);
}
function refreshAll() { refreshRooms(); refreshTab(); refreshLegend(); refreshInspector(); refreshMap(); refreshThumbs(); refreshStatus(); refreshUndo(); requestRender(); }

// ------------------------------------------------------------------- menus
function openMenu(anchor, which) {
  const m = $('#menu'), item = (label, run, dis) => h('button', { disabled: dis, onclick: () => { m.hidden = true; run(); } }, label);
  const tog = (label, key) => item((S.show[key] ? '☑ ' : '☐ ') + label, () => { S.show[key] = !S.show[key]; syncShow(); });
  const items = {
    file: () => [item('New project…', newProject), item('Open a project (.zip or .json)…', () => $('#openfile').click()), item('Open the example project', () => loadProjectFiles(null)), h('hr'),
      item('New room…', newRoomDialog), item('Copy a room from the original game…', copyVanillaDialog), h('hr'), item('Save: download the project (.zip)', saveZip), item('Open the game page', () => window.open('../', '_blank'))],
    edit: () => [item('Undo', undo, !S.undo.length), item('Redo', redo, !S.redo.length), h('hr'), item('Delete the selected object or door', deleteSelected, !S.sel), item('Clear this room…', clearRoom), item('Room settings…', roomSettingsDialog)],
    view: () => [tog('Collision', 'collision'), tog('Objects', 'objects'), tog('Camera (240 × 160)', 'camera'), tog('Grid', 'grid'), h('hr'), item('Zoom to fit', zoomFit), item('Zoom 100%', () => setZoom(1)), item('Zoom 300%', () => setZoom(3))],
  }[which]();
  setKids(m, ...items); m.hidden = false;
  const r = anchor.getBoundingClientRect();
  m.style.left = clamp(r.left, 6, innerWidth - m.offsetWidth - 6) + 'px';
  m.style.top = clamp(r.bottom + 4, 6, innerHeight - m.offsetHeight - 6) + 'px';
}
function syncShow() {
  lsSet('katam.ed.show', S.show);
  $('#sh-collision').checked = S.show.collision; $('#sh-objects').checked = S.show.objects; $('#sh-camera').checked = S.show.camera;
  $('#gridbtn').setAttribute('aria-pressed', String(S.show.grid));
  requestRender();
}
function zoomFit() {
  const sc = $('#scroll'), R = S.room;
  setZoom(clamp(Math.floor(Math.min(sc.clientWidth / (R.W * 16), sc.clientHeight / (R.H * 16))), 1, 6));
}
function clearRoom() {
  confirmBox('Clear this room?', 'Every tile, object and door of this room is removed. You can undo it.', 'Clear room', () => {
    pushUndo();
    const R = S.room, D = dict(R.doc.tileset);
    R.art.fill(D.empty); R.col.fill(0); R.doc.objects = []; R.doc.doors = []; S.sel = null;
    redrawArt(); touch(); refreshInspector(); refreshMap();
  });
}
function newProject() {
  confirmBox('Start a new project?', 'The project in this browser is replaced by one empty room. Save it first if you want to keep it.', 'New project', () => {
    flush(); clearProject(); S.room = null; S.starts = {}; lsSet('katam.ed.starts', S.starts);
    S.io.writeText('/project/project.json', S.L.json.writeJson({ format: 'katam-project/1', id: 'my_levels', name: 'My levels', base: 'vanilla' }));
    rebind();
    createRoom('First room', 30, 14, looks().find((l) => l.tileset === 'tileset:vanilla/29') || looks()[0]);
  });
}
function loadProjectFiles(files) {
  flush(); S.room = null;
  if (!files) putExample();
  else {
    clearProject();
    let n = 0;
    for (const f of files) {
      const name = f.name.replace(/\\/g, '/');
      if (/(^|\/)project\.json$/.test(name)) { S.io.writeText('/project/project.json', f.text); continue; }
      let doc = null;
      try { doc = JSON.parse(f.text); } catch (e) { continue; }
      if (doc && doc.id && S.L.project.parseId(doc.id)) { S.io.writeText('/project/' + S.L.project.pathOf(doc.id), f.text); n++; }
    }
    if (!S.io.exists('/project/project.json')) S.io.writeText('/project/project.json', S.L.json.writeJson({ format: 'katam-project/1', id: 'my_levels', name: 'My levels', base: 'vanilla' }));
    if (!n) { toast('No room files were found in that file.'); putExample(); }
  }
  rebind();
  const ids = roomIds();
  if (!ids.length || !openRoom(ids[0])) { putExample(); rebind(); openRoom(roomIds()[0]); }
  validateSoon(); persist();
}
async function openFiles(list) {
  try {
    const groups = await Promise.all(Array.from(list).map(async (f) => (/\.zip$/i.test(f.name) ? readZip(await f.arrayBuffer(), S.L.zlib.inflate) : [{ name: f.name, text: await f.text() }])));
    loadProjectFiles(groups.flat());
  } catch (e) { toast('Could not read that file: ' + e.message, 5000); }
}
function saveZip() {
  flush();
  const enc = new TextEncoder();
  const files = S.io.list('/project').map((p) => ({ name: p.slice('/project/'.length), bytes: enc.encode(S.io.readText(p)) }));
  const name = slug((S.proj.project && S.proj.project.name) || 'katam_project') + '.zip';
  const a = h('a', { href: URL.createObjectURL(makeZip(files)), download: name });
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 4000);
  toast(`Saved ${name} (${files.length} files). The project is also kept in this browser.`);
}

// ----------------------------------------------------- validate / compile / play
function validateNow() {
  flush(); rebind();
  const L = S.L, t0 = performance.now();
  let res;
  try { res = L.validate.validatePath({ io: S.io, target: '/project', bases: ['/vanilla'], vocab: S.vocab, registry: S.registry, schemaDir: '/schema' }); } catch (e) { res = { errors: [{ code: 'internal', message: e.message }], warnings: [] }; }
  const compiled = compileProject();
  for (const e of compiled.errors) if (!res.errors.some((x) => x.message === e.message)) res.errors.push({ severity: 'error', code: 'compile', file: e.file, where: e.where, message: e.message, hint: e.hint });
  res.warnings = res.warnings.filter((w) => w.code !== 'nav-default');
  S.findings = res; S.compiled = compiled;
  S.validateMs = Math.round(performance.now() - t0);
  refreshIssues();
  return res;
}
const validateSoon = debounce(() => { if (S.drag) return validateSoon(); validateNow(); }, 1100);

/** Compile the project's own rooms (the original game's are already in the ROM). */
function compileProject() {
  const out = { rooms: new Map(), tilesets: new Map(), backgrounds: new Map(), palettes: new Map(), objDesc: new Map(), errors: [], bindings: S.bind, vocab: S.vocab,
    vanillaLayers: new Set(['/vanilla']), projectIds: new Map([['/vanilla', 'vanilla'], ['/project', (S.proj.project && S.proj.project.id) || 'project']]) };
  for (const [id, d] of [...S.proj.docs].sort((a, b) => (a[0] < b[0] ? -1 : 1))) {
    if (!id.startsWith('room:')) continue;
    try { const c = S.L.room.compileRoom(d.doc, S.ctx); out.rooms.set(c.roomId, { id, project: d.project, ...c }); } catch (e) { out.errors.push({ file: d.rel, project: d.project, where: e.where || '', message: e.message, hint: e.hint }); }
  }
  return out;
}

const P = { frame: null, ready: false, pending: null };
function play() {
  const res = validateNow();
  if (res.errors.length) { document.body.classList.add('bottom-open'); toast('Fix the errors in the Validation list first.', 4000); return; }
  let bundle;
  try { bundle = S.L.pipeline.bundleOf(S.compiled); } catch (e) { toast('Could not build the rooms: ' + e.message, 6000); return; }
  const [sx, sy] = startCell();
  P.pending = { katamEd: 1, type: 'play', bundle: bundle.bytes, room: S.room.num, x: sx + 1, y: sy + 1 };
  $('#playview').hidden = false;
  $('#play-msg').textContent = 'Starting the game…';
  if (!P.frame) {
    P.ready = false;
    P.frame = h('iframe', { src: new URL(CFG.play, BASE).href, allow: 'autoplay; fullscreen; gamepad', title: 'Playtest' });
    $('#playview').prepend(P.frame);
  } else sendPlay();
  P.frame.focus();
}
function sendPlay() {
  if (!P.frame || !P.ready || !P.pending) return;
  P.frame.contentWindow.postMessage(P.pending, location.origin);
  P.pending = null;
}
function stopPlay(destroy) {
  $('#playview').hidden = true;
  P.pending = null;
  if (!P.frame) return;
  if (destroy || !P.ready) { P.frame.remove(); P.frame = null; P.ready = false; }
  else P.frame.contentWindow.postMessage({ katamEd: 1, type: 'pause' }, location.origin);   // kept: the next play is a warp
}
window.addEventListener('message', (e) => {
  if (e.origin !== location.origin || !e.data || e.data.katamPlay !== 1) return;
  const m = e.data;
  if (m.type === 'ready') { P.ready = true; sendPlay(); }
  else if (m.type === 'status') { $('#play-msg').textContent = m.text || ''; if (m.done) setTimeout(() => { if ($('#play-msg').textContent === m.text) $('#play-msg').textContent = ''; }, 2500); }
});

// -------------------------------------------------------------------- wiring
function buildUi() {
  buildTools();
  document.body.append(h('input', { type: 'file', id: 'openfile', hidden: true, multiple: true, accept: '.json,.zip,application/json,application/zip', onchange: function () { if (this.files.length) openFiles(this.files); this.value = ''; } }));
  $('#zoomsel').replaceChildren(...[1, 2, 3, 4, 5, 6].map((z) => h('option', { value: z, selected: z === S.zoom }, z * 100 + '%')));
  $('#zoomsel').onchange = (e) => setZoom(Number(e.target.value));
  $('#zin').onclick = () => setZoom(S.zoom + 1); $('#zout').onclick = () => setZoom(S.zoom - 1);
  $('#gridbtn').onclick = () => { S.show.grid = !S.show.grid; syncShow(); };
  for (const k of ['collision', 'objects', 'camera']) $('#sh-' + k).onchange = (e) => { S.show[k] = e.target.checked; syncShow(); };
  syncShow();
  $('#roomsel').onchange = (e) => openRoom(e.target.value);
  $('#undo').onclick = undo; $('#redo').onclick = redo;
  $('#validate').onclick = () => { const r = validateNow(); document.body.classList.add('bottom-open'); toast(r.errors.length || r.warnings.length ? `${r.errors.length} error(s), ${r.warnings.length} warning(s).` : 'No issues: ready to play.'); };
  $('#play').onclick = play; $('#save').onclick = saveZip;
  $('#play-back').onclick = () => stopPlay(false);
  $('#play-restart').onclick = () => { stopPlay(true); play(); };
  $$('.btn.menu').forEach((b) => { b.onclick = (e) => { e.stopPropagation(); openMenu(b, b.dataset.menu); }; });
  document.addEventListener('click', (e) => { if (!e.target.closest('#menu')) $('#menu').hidden = true; });
  $$('.tab').forEach((b) => { b.onclick = () => setTab(b.dataset.tab); });
  $('#tg-side').onclick = () => { document.body.classList.remove('bottom-open'); document.body.classList.toggle('side-open'); };
  $('#tg-bottom').onclick = () => { document.body.classList.remove('side-open'); document.body.classList.toggle('bottom-open'); };
  for (const [sel, cls] of [['#side', 'side-open'], ['#bottom', 'bottom-open']]) $(sel).prepend(h('button', { class: 'btn tiny only-compact', style: 'align-self:flex-end', onclick: () => document.body.classList.remove(cls) }, '✕ Close'));
  const cv = $('#cv');
  cv.addEventListener('pointerdown', onDown); cv.addEventListener('pointermove', onMove);
  cv.addEventListener('pointerup', onUp); cv.addEventListener('pointercancel', onUp);
  cv.addEventListener('pointerleave', () => { if (!S.drag) { S.hover = null; requestRender(); } });
  cv.addEventListener('contextmenu', (e) => e.preventDefault());
  $('#scroll').addEventListener('wheel', (e) => { if (!e.ctrlKey && !e.metaKey) return; e.preventDefault(); const r = $('#scroll').getBoundingClientRect(); setZoom(S.zoom + (e.deltaY < 0 ? 1 : -1), [e.clientX - r.left, e.clientY - r.top]); }, { passive: false });
  window.addEventListener('keydown', (e) => {
    if (e.target.closest('input, select, textarea') || !$('#playview').hidden || !$('#modal').hidden) return;
    const k = e.key.toLowerCase();
    if ((e.ctrlKey || e.metaKey) && k === 'z') { e.preventDefault(); e.shiftKey ? redo() : undo(); }
    else if ((e.ctrlKey || e.metaKey) && k === 'y') { e.preventDefault(); redo(); }
    else if ((e.ctrlKey || e.metaKey) && k === 's') { e.preventDefault(); saveZip(); }
    else if (k === 'delete' || k === 'backspace') { if (S.sel) { e.preventDefault(); deleteSelected(); } }
    else if (k === ' ') { S.space = true; e.preventDefault(); }
    else if (k === 'escape') { S.pick = null; S.sel = null; refreshInspector(); refreshStatus(); requestRender(); }
    else if (k === '+' || k === '=') setZoom(S.zoom + 1);
    else if (k === '-') setZoom(S.zoom - 1);
    else if (!e.ctrlKey && !e.metaKey) { const t = { v: 'select', b: 'brush', r: 'rect', g: 'fill', e: 'eraser', c: 'collision', o: 'object', d: 'door', i: 'eyedropper' }[k]; if (t) setTool(t); }
  });
  window.addEventListener('keyup', (e) => { if (e.key === ' ') S.space = false; });
  window.addEventListener('beforeunload', () => { flush(); });
  document.addEventListener('visibilitychange', () => { if (document.hidden) flush(); });
  setTool('select');
}

boot().catch((e) => { console.error(e); curtain('The editor could not start: ' + e.message, { err: true }); });
