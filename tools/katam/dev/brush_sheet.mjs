// brush_sheet.mjs -- look at a tile set's terrain brushes the way
// docs/design/BRUSH-CLASSIFICATION.md asks: every learned style's pieces on a
// contact sheet, laid out by shape role, and a test shape painted with it.
//
//   node tools/katam/katam.mjs decompile --rom ROM --pack PACK -o VAN     (once; PACK from
//       build/lp64/katam --res-extract PACK ROM)
//   node tools/katam/dev/brush_sheet.mjs VAN tileset:vanilla/0 OUT [--rules FILE|none]
//
// OUT gets
//   styles.json      every style the editor would offer (kind, name, cells, and
//                    per piece: token, uses, dominant role and its share)
//   sheet-K-N.png    style N of kind K: one row per role (FILL, TOP, CORNER TL, ...),
//                    most used first, numbered as in styles.json (red: the role
//                    holds for under 60% of its uses); magenta = see-through
//   paint-K-N.png    a test shape (floor, step, notch, ledge, pillar, overhang)
//                    painted by the brush, edge art included, on a plain sky
//   test-K-N.png     the topology test level (lib/brushtest.mjs: every ground shape and
//                    slope join, or the kind's runs and pools) painted by the brush;
//                    yellow frame: the brush's own piece of another shape, red: another
//                    brush's piece; styles.json "test" has the sums and the missing shapes
//   rim.png          the edge art the tile set draws in the air against ground
//   zoom.png         with --zoom solid-1:10,11,17 (or rim:2,5): those pieces, numbered as
//                    on the sheet, in place in the original, 5x5 cells around, three places each
// --rules: the hand-checked preset (default: the one in
// assets/schema/authoring/autotile/ for that tile set; `none` to see what is
// learned with no preset).  The pictures are the player's ROM's art: OUT must
// be outside the repository, and the script refuses otherwise.
import fs from 'node:fs'; import path from 'node:path'; import zlib from 'node:zlib'; import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const LIB = path.join(ROOT, 'tools/katam/lib');
const imp = (n) => import(path.join(LIB, n + '.mjs'));
const [{ nodeIO }, png, { indexVocab }, { loadRegistry }, project, grid, tiles, gfx, auto, brushtest] = await Promise.all(['io', 'png', 'vocab', 'registry', 'project', 'grid', 'tiles', 'gfx', 'autotile', 'brushtest'].map(imp));
png.setZlib({ deflate: (b) => new Uint8Array(zlib.deflateSync(b)), inflate: (b) => new Uint8Array(zlib.inflateSync(b)) });

const pos = [], opt = {};
for (let a = process.argv.slice(2); a.length;) { const v = a.shift(); if (v.startsWith('--')) opt[v.slice(2)] = a.shift(); else pos.push(v); }
const [VAN, TS, OUT] = pos;
if (!VAN || !TS || !OUT) { console.error('usage: brush_sheet.mjs VANILLA_PROJECT tileset:vanilla/N OUT_DIR [--rules FILE|none]'); process.exit(2); }
const out = path.resolve(OUT);
if (out === ROOT || out.startsWith(ROOT + path.sep)) { console.error(`refusing: ${out} is inside the repository (the sheets are ROM art)`); process.exit(2); }
fs.mkdirSync(out, { recursive: true });

const io = await nodeIO();
const vocab = indexVocab(JSON.parse(fs.readFileSync(path.join(ROOT, 'assets/schema/authoring/vocab.json'), 'utf8')));
const registry = loadRegistry(io, path.join(ROOT, 'assets/schema/semantics'));
const van = project.loadProject(io, path.resolve(VAN));
const bind = new project.Bindings(vocab, [van]);
const ctx = project.makeContext({ vocab, registry, bindings: bind });
const colClass = ctx.vocab.collision.map((c) => auto.classOfBits(c.bits));

// the rooms of the original that use the tile set, as the editor reads them (web/editor/editor.js dict())
const rooms = []; let look = null;
const counts = new Map();
for (const [id, d] of van.docs) {
  const doc = d.doc;
  if (!id.startsWith('room:') || doc.tileset !== TS) continue;
  const W = doc.size.width, H = doc.size.height;
  const art = grid.decodeGrid(doc.art, W, H).map((s) => tiles.metaToken(tiles.metaValues(s)));
  const col = grid.decodeGrid(doc.collision, W, H).map((v) => ctx.collisionValue(v));
  rooms.push({ W, H, art, cls: col.map((v) => colClass[v]), col, id: doc.bind && doc.bind.roomId });
  for (let i = 0; i < art.length; i++) { const e = counts.get(art[i]) || { n: 0, cols: new Map() }; e.n++; e.cols.set(col[i], (e.cols.get(col[i]) || 0) + 1); counts.set(art[i], e); }
  look = look || doc;
}
if (!rooms.length) { console.error(`no room uses ${TS}`); process.exit(1); }
// the blank piece: the most used piece whose usual collision is air (as the editor's dict())
const usual = (e) => [...e.cols].sort((a, b) => b[1] - a[1])[0][0];
const airPiece = [...counts].sort((a, b) => b[1].n - a[1].n).find(([, e]) => colClass[usual(e)] === 'air');
const empty = airPiece ? airPiece[0] : '000.0 000.0 000.0 000.0';

const docOf = (id) => { const r = bind.resolve(id); return r ? bind.docs.get(r) : null; };
const ts = docOf(look.tileset), pal = docOf(look.palette);
const rgb5 = (hex) => { const c = gfx.bgr555(hex); return [c & 31, (c >> 5) & 31, (c >> 10) & 31].map((v) => (v << 3) | (v >> 2)); };
const T = gfx.pngToTiles(io.readBytes(`${ts.base}/${ts.doc.image}`), ts.doc.tileCount, ts.doc.image);
const banks = pal.doc.banks.map((b) => b.map(rgb5));
const pixCache = new Map();
/** a piece's 16x16 RGBA, alpha 0 where it is see-through (as the editor's metaImage) */
function pixels(tok) {
  let im = pixCache.get(tok);
  if (im) return im;
  im = new Uint8Array(1024);
  const vals = tiles.metaValues(tok);
  for (let q = 0; q < 4; q++) {
    const ox = (q & 1) * 8, oy = (q >> 1) * 8, v = vals[q], t = v & 0x3ff, hf = v & 0x400, vf = v & 0x800, bk = banks[v >> 12];
    for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++) {
      const sx = hf ? 7 - x : x, sy = vf ? 7 - y : y, by = T[t * 32 + sy * 4 + (sx >> 1)], ci = sx & 1 ? by >> 4 : by & 15;
      if (!ci) continue;
      const c = (bk && bk[ci]) || [16, 14, 30], o = ((oy + y) * 16 + ox + x) * 4;
      im[o] = c[0]; im[o + 1] = c[1]; im[o + 2] = c[2]; im[o + 3] = 255;
    }
  }
  pixCache.set(tok, im);
  return im;
}
const opaque = (t) => { const p = pixels(t); for (let i = 3; i < p.length; i += 4) if (p[i] < 255) return false; return true; };

let rules = null;
if (opt.rules && opt.rules !== 'none') rules = JSON.parse(fs.readFileSync(opt.rules, 'utf8'));
else if (!opt.rules) {
  const dir = path.join(ROOT, 'assets/schema/authoring/autotile');
  for (const f of fs.readdirSync(dir)) { const r = JSON.parse(fs.readFileSync(path.join(dir, f), 'utf8')); if (r.tileset === TS) { rules = r; opt.rules = path.relative(ROOT, path.join(dir, f)); } }
}
const model = auto.learn(rooms, empty, { opaque, pixels, rules });

// ---- shape roles: what a piece is, from the terrain around where the original puts it
// (shape bits: NEAR order UL U UR L R DL D DR; a set bit is the same terrain)
function roleOf(shape, slope) {
  if (slope) return 'SLOPE';
  const u = shape & 2, l = shape & 8, r = shape & 16, d = shape & 64;
  const open = [!u, !d, !l, !r], n = open.filter(Boolean).length;
  if (n === 0) {
    const miss = [[1, 'UL'], [4, 'UR'], [32, 'DL'], [128, 'DR']].filter(([b]) => !(shape & b)).map(([, s]) => s);
    return miss.length === 0 ? 'FILL' : miss.length === 1 ? 'NOTCH ' + miss[0] : 'NOTCHES';
  }
  if (n === 1) return !u ? 'TOP' : !d ? 'CEILING' : !l ? 'WALL L' : 'WALL R';
  if (n === 2) {
    if (!u && !d) return 'ONE HIGH';
    if (!l && !r) return 'ONE WIDE';
    return 'CORNER ' + (!u ? 'T' : 'B') + (!l ? 'L' : 'R');
  }
  return n === 3 ? 'END' : 'SINGLE';
}
const ROLES = ['FILL', 'TOP', 'CORNER TL', 'CORNER TR', 'WALL L', 'WALL R', 'NOTCH UL', 'NOTCH UR', 'NOTCH DL', 'NOTCH DR', 'NOTCHES', 'CEILING', 'CORNER BL', 'CORNER BR', 'ONE HIGH', 'ONE WIDE', 'END', 'SINGLE', 'SLOPE'];
const SHIFT = 11, MASK = (1 << SHIFT) - 1;   // a source cell: room << 11 | index (autotile.mjs SRC_SHIFT)

// ---- a 3x5 font for the labels
const FONT = {};
'0:7b6f,1:2c97,2:73e7,3:73cf,4:5bc9,5:79cf,6:79ef,7:7249,8:7bef,9:7bcf,A:2bed,B:6bae,C:7927,D:6b6e,E:79e7,F:79e4,G:796f,H:5bed,I:7497,J:124f,K:5bad,L:4927,M:5fed,N:7b6d,O:7b6f,P:7be4,Q:7b7b,R:7bad,S:79cf,T:7492,U:5b6f,V:5b6a,W:5bfd,X:5aad,Y:5a92,Z:72a7, :0,-:1c0'
  .split(',').forEach((p) => { const [c, h] = p.split(':'); FONT[c] = parseInt(h, 16); });

function canvas(w, h, bg) {
  const im = new Uint8Array(w * h * 4);
  for (let i = 0; i < w * h; i++) im.set(bg, i * 4);
  return { w, h, im };
}
function text(C, x, y, s, k = 2, c = [20, 20, 20, 255]) {
  for (const ch of String(s).toUpperCase()) {
    const g = FONT[ch] || 0;
    for (let b = 0; b < 15; b++) if (g & (1 << (14 - b))) {
      const px = x + (b % 3) * k, py = y + ((b / 3) | 0) * k;
      for (let yy = 0; yy < k; yy++) for (let xx = 0; xx < k; xx++) { const X = px + xx, Y = py + yy; if (X < C.w && Y < C.h) C.im.set(c, (Y * C.w + X) * 4); }
    }
    x += 4 * k;
  }
}
function blit(C, tok, x, y, k, bg) {   // bg null: see-through drawn magenta
  const p = pixels(tok);
  for (let v = 0; v < 16; v++) for (let u = 0; u < 16; u++) {
    const s = (v * 16 + u) * 4, c = p[s + 3] ? [p[s], p[s + 1], p[s + 2], 255] : bg || [255, 0, 255, 255];
    for (let yy = 0; yy < k; yy++) for (let xx = 0; xx < k; xx++) { const X = x + u * k + xx, Y = y + v * k + yy; if (X < C.w && Y < C.h) C.im.set(c, (Y * C.w + X) * 4); }
  }
}
const save = (C, f) => fs.writeFileSync(path.join(out, f), png.encodeRGBA(C.w, C.h, C.im));

// ---- the test shape: floor, a step up, a one-cell notch, a floating ledge with
// an underside, a one-wide pillar, an overhang (a concave corner underneath),
// a 1:1 hill (/ \) and a gentle one (a b up, c d down: autotile.mjs SLOPES)
const SHAPE = [
  '........................................',
  '........................................',
  '...................######...............',
  '...................######...............',
  '........................................',
  '.......................#................',
  '..######...............#................',
  '..######..........###..#................',
  '..######..........#....#../###\\.ab##cd..',
  '##############.#########################',
  '########################################',
  '########################################',
];
const SLOPE_V = { '/': 11, '\\': 12, a: 5, b: 6, c: 7, d: 8 };
const solidV = colClass.indexOf('solid'), airV = colClass.indexOf('air');
// the collision value each kind of brush paints: the one the original uses most (as the editor's colRep)
const colRep = {};
for (const [v, e] of Object.entries(registry.collisionEnum())) {
  const c = colClass[v];
  if (c && (colRep[c] === undefined || (e.cells || 0) > colRep[c][1])) colRep[c] = [Number(v), e.cells || 0];
}
const colOf = (kind) => (colRep[kind] ? colRep[kind][0] : kind === 'air' ? airV : solidV);
/** The topology test level (lib/brushtest.mjs) painted with a style: pieces that are not the
 *  style's own for that shape framed, yellow ('near': its own piece of another shape) or red
 *  ('borrowed': another brush's), with the sums on a strip above. */
function testLevel(style) {
  const base = model.kinds.solid && model.kinds.solid.styles[0];
  const T = brushtest.paintTest(model, style, { colOf, classOf: (v) => colClass[v], base: base === style ? null : base, salt: 7 });
  if (!T) return null;
  const k = 2, PS = 16 * k, top = 28, C = canvas(T.W * PS, T.H * PS + top, SKY);
  for (let i = 0; i < T.W * T.H; i++) {
    const x = (i % T.W) * PS, y = ((i / T.W) | 0) * PS + top;
    if (T.art[i] !== model.empty) blit(C, T.art[i], x, y, k, SKY);
    const c = T.how[i] === 'near' ? [255, 210, 0, 255] : T.how[i] === 'borrowed' ? [230, 0, 0, 255] : null;
    if (c) for (let t = 0; t < PS; t++) for (const [u, v] of [[t, 0], [t, 1], [t, PS - 1], [t, PS - 2], [0, t], [1, t], [PS - 1, t], [PS - 2, t]]) C.im.set(c, ((y + v) * C.w + x + u) * 4);
  }
  for (let i = 0; i < C.w * top; i++) C.im.set([235, 235, 235, 255], i * 4);
  const r = T.report;
  text(C, 6, 6, `${style.name}  shapes ${r.shapes.own} of ${r.shapes.level} own` + (style.kind === 'solid' ? `  slopes ${r.slopes.own} of ${r.slopes.level}` : '') + `  cells ${r.cells.own} own ${r.cells.near} near ${r.cells.borrowed} borrowed`, 2);
  return { C, report: { ...r, shapes: { ...r.shapes, missing: r.shapes.missing.map(brushtest.blobLabel) } } };
}
const SKY = [150, 200, 240, 255];
function paint(style) {
  const H = SHAPE.length, W = SHAPE[0].length, art = new Array(W * H).fill(model.empty), cls = new Array(W * H).fill('air'), col = new Array(W * H).fill(airV);
  const want = new Map(), cells = [];
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = y * W + x, ch = SHAPE[y][x]; cells.push(i);
    if (ch === '.' || (ch !== '#' && style.kind !== 'solid')) continue;
    col[i] = ch === '#' ? solidV : SLOPE_V[ch]; cls[i] = ch === '#' ? style.kind : colClass[col[i]]; want.set(i, style);
  }
  auto.retile(model, { W, H, art, cls, col }, cells, want, 7);
  const k = 3, C = canvas(W * 16 * k, H * 16 * k, SKY);
  for (let i = 0; i < W * H; i++) if (art[i] !== model.empty) blit(C, art[i], (i % W) * 16 * k, ((i / W) | 0) * 16 * k, k, SKY);
  return C;
}

const report = { tileset: TS, rules: opt.rules || null, rooms: rooms.map((R) => R.id), empty, styles: [] };
const k = 3, PW = 16 * k, CW = PW + 6, CH = PW + 16, PER = 16, LABEL = 120;
for (const [kind, K] of Object.entries(model.kinds)) {
  K.styles.forEach((s, n) => {
    const per = new Map();
    for (const id of s.cells) {
      const R = model.rooms[id >> SHIFT], i = id & MASK, t = R.art[i];
      let e = per.get(t); if (!e) per.set(t, e = { tok: t, n: 0, roles: new Map() });
      e.n++; const ro = roleOf(R.shape[i], R.slope[i]); e.roles.set(ro, (e.roles.get(ro) || 0) + 1);
    }
    const pieces = [...per.values()].sort((a, b) => b.n - a.n || (a.tok < b.tok ? -1 : 1)).map((e, ix) => {
      const [role, rn] = [...e.roles].sort((a, b) => b[1] - a[1])[0];
      return { ix, tok: e.tok, uses: e.n, role, share: +(rn / e.n).toFixed(2) };
    });
    const test = testLevel(s);
    report.styles.push({ kind, id: s.id, name: s.name, cells: s.cells.length, test: test ? test.report : null, pieces });
    if (test) save(test.C, `test-${kind}-${n + 1}.png`);
    const byRole = ROLES.map((ro) => [ro, pieces.filter((p) => p.role === ro)]).filter(([, l]) => l.length);
    const rows = byRole.reduce((a, [, l]) => a + Math.ceil(l.length / PER), 0);
    const C = canvas(LABEL + PER * CW + 8, 30 + rows * CH + 8, [235, 235, 235, 255]);
    text(C, 8, 8, `${s.name}  ${pieces.length} PIECES ${s.cells.length} CELLS`, 3);
    let y = 30;
    for (const [ro, l] of byRole) {
      text(C, 8, y + 16, ro, 2);
      l.forEach((p, j) => {
        if (j && j % PER === 0) y += CH;
        const x = LABEL + (j % PER) * CW;
        blit(C, p.tok, x, y, k, null);
        text(C, x, y + PW + 3, p.ix, 2, p.share < 0.6 ? [190, 0, 0, 255] : [20, 20, 20, 255]);
      });
      y += CH;
    }
    save(C, `sheet-${kind}-${n + 1}.png`);
    save(paint(s), `paint-${kind}-${n + 1}.png`);
  });
}
// --zoom solid-1:10,11,17 -- those pieces of that style where the original puts them:
// the 5x5 cells around up to three places each (framed red), with the room id
if (opt.zoom) {
  const [which, list] = opt.zoom.split(':'), [kind, n] = [which.replace(/-\d+$/, ''), +which.replace(/^.*-/, '')];
  const rimZoom = which === 'rim';   // --zoom rim:2,5 -- edge art pieces, numbered as on rim.png
  const s = rimZoom ? { cells: model.rooms.flatMap((R, r) => R.art.map((t, i) => (R.cls[i] === 'air' && model.rim.has(t) ? (r << SHIFT) | i : -1)).filter((id) => id >= 0)) } : model.kinds[kind] && model.kinds[kind].styles[n - 1];
  const rep = rimZoom ? { pieces: [...model.rim].map((tok) => ({ tok })) } : report.styles.find((r) => s && r.id === s.id);
  if (!s || !rep) { console.error(`no style ${which}`); process.exit(1); }
  const idOf = new Map(rooms.map((R) => [R.art, R.id]));
  const want = list.split(',').map(Number), CS = 5 * 16 * 2, Z = canvas(want.length ? 3 * (CS + 10) + 70 : 1, want.length * (CS + 26) + 8, [235, 235, 235, 255]);
  want.forEach((ix, row) => {
    const p = rep.pieces[ix];
    if (!p) return;
    const seen = new Set(), at = [];
    for (const id of s.cells) {
      const r = id >> SHIFT, R = model.rooms[r], i = id & MASK;
      if (R.art[i] !== p.tok || seen.has(r)) continue;
      seen.add(r); at.push([R, i]);
      if (at.length === 3) break;
    }
    const y0 = 8 + row * (CS + 26);
    text(Z, 8, y0 + CS / 2 - 5, ix, 3);
    at.forEach(([R, i], c) => {
      const cx = i % R.W, cy = (i / R.W) | 0, x0 = 70 + c * (CS + 10);
      for (let b = -2; b <= 2; b++) for (let a = -2; a <= 2; a++) {
        const X = cx + a, Y = cy + b, px = x0 + (a + 2) * 32, py = y0 + (b + 2) * 32;
        if (X < 0 || Y < 0 || X >= R.W || Y >= R.H) continue;
        blit(Z, R.art[Y * R.W + X], px, py, 2, SKY);
      }
      for (let t = 0; t < 32; t++) for (const [u, v] of [[t, 0], [t, 31], [0, t], [31, t]]) Z.im.set([220, 0, 0, 255], ((y0 + 64 + v) * Z.w + x0 + 64 + u) * 4);
      text(Z, x0, y0 + CS + 4, `ROOM ${idOf.get(R.art)} ${cx} ${cy}`, 2);
    });
  });
  save(Z, 'zoom.png');
}
const rim = [...model.rim];
const RC = canvas(Math.max(1, Math.min(rim.length, PER)) * CW + 16, Math.max(1, Math.ceil(rim.length / PER)) * CH + 16, [235, 235, 235, 255]);
rim.forEach((t, j) => { const x = 8 + (j % PER) * CW, y = 8 + ((j / PER) | 0) * CH; blit(RC, t, x, y, k, null); text(RC, x, y + PW + 3, j, 2); });
save(RC, 'rim.png');
report.rim = rim;
fs.writeFileSync(path.join(out, 'styles.json'), JSON.stringify(report, null, 1) + '\n');
console.log(`${TS}: ${rooms.length} rooms (${report.rooms.join(' ')}), rules ${report.rules || 'none'}`);
for (const s of report.styles) console.log(`  ${s.id.padEnd(16)} ${s.name.padEnd(16)} cells ${String(s.cells).padStart(5)}  pieces ${String(s.pieces.length).padStart(3)}` + (s.test ? `  test level: shapes ${s.test.shapes.own}/${s.test.shapes.level}` + (s.kind === 'solid' ? `, slopes ${s.test.slopes.own}/${s.test.slopes.level}` : '') + `, cells ${s.test.cells.own} own ${s.test.cells.near} near ${s.test.cells.borrowed} borrowed` : ''));
console.log(`  rim (edge art) ${rim.length} pieces -> ${out}`);
