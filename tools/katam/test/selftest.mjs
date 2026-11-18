// ROM-free self-test of the authoring library (make authoring-check).
import zlib from 'node:zlib';
import assert from 'node:assert/strict';
import { decompress, encodeLZ77, encodeRL } from '../lib/compress.mjs';
import { encodeGrid, decodeGrid } from '../lib/grid.mjs';
import { tokenOf, valueOf } from '../lib/tiles.mjs';
import { setZlib } from '../lib/png.mjs';
import { tilesToPng, pngToTiles, colorOf, bgr555 } from '../lib/gfx.mjs';
import { scaffoldRoom } from '../lib/scaffold.mjs';
import { compileRoom, decompileRoom } from '../lib/room.mjs';
import { Bindings, makeContext } from '../lib/project.mjs';
import { Registry } from '../lib/registry.mjs';
import { buildVocab, indexVocab } from '../lib/vocab.mjs';
import { LEVELS, BLOBS } from '../lib/brushtest.mjs';
import { blobOf, learn, retile } from '../lib/autotile.mjs';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
setZlib({ deflate: (b) => new Uint8Array(zlib.deflateSync(b)), inflate: (b) => new Uint8Array(zlib.inflateSync(b)) });
let seed = 1;
const rnd = (n) => { seed = (seed * 1103515245 + 12345) >>> 0; return seed % n; };
let n = 0;
const t = (name, f) => { f(); n++; };

t('brush test levels: rooms, and every ground shape', () => {
  for (const [kind, rows] of Object.entries(LEVELS)) {
    const W = rows[0].length, H = rows.length;
    assert.ok(W >= 15 && H >= 13 && W * H <= 1950, `${kind}: ${W} x ${H} is no room`);
    for (const r of rows) { assert.equal(r.length, W, kind); assert.match(r, /^[.#\/\\abcd=H~^]+$/, kind); }
  }
  assert.equal(BLOBS.length, 47);
  const rows = LEVELS.solid, W = rows[0].length, H = rows.length, seen = new Set();
  const g = (x, y) => (x < 0 || y < 0 || x >= W || y >= H ? true : '#/\\abcd'.includes(rows[y][x]));
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (rows[y][x] === '#') {
    let m = 0;
    [[-1, -1], [0, -1], [1, -1], [-1, 0], [1, 0], [-1, 1], [0, 1], [1, 1]].forEach(([dx, dy], k) => { if (g(x + dx, y + dy)) m |= 1 << k; });
    seen.add(blobOf(m));
  }
  assert.deepEqual([...seen].sort((a, b) => a - b), BLOBS);
});

t('brush presets: take, and no wrong edges when cut square', () => {
  // three materials in synthetic rooms, a piece per role: a floor that never shows a wall or
  // an underside (a), and floating blocks (b, c) that the learner keeps apart
  const room = (rows, m) => {
    const H = rows.length, W = rows[0].length, art = [], cls = [], g = (x, y) => x < 0 || y < 0 || x >= W || y >= H || rows[y][x] === '#';
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      cls.push(g(x, y) ? 'solid' : 'air');
      const r = !g(x, y) ? -1 : !g(x, y - 1) ? 0 : !g(x, y + 1) ? 1 : !g(x - 1, y) ? 2 : !g(x + 1, y) ? 3 : 4 + ((x + y) & 1);
      art.push(r < 0 ? EMPTY : tok(m, r));
    }
    return { W, H, art, cls };
  };
  const EMPTY = '000.0 000.0 000.0 000.0', tok = (m, r) => Array(4).fill(`${m}${r}.0`).join(' '), air = '.'.repeat(30);
  const floor = [...Array(9).fill(air), '#####...........#####.........', ...Array(4).fill('#'.repeat(30))];
  const blocks = [air, '..####.....######.....###.....', '..####.....######.....###.....', '..####.....######.............', '...............###....#####...',
    '..######.......###....#####...', '..######..............#####...', air, '....#####........######.......', '....#####........######.......', '....#####........######.......', air, air, air];
  const rooms = [room(floor, 'a'), room(floor, 'a'), room(blocks, 'b'), room(blocks, 'c')];
  const names = (m) => m.kinds.solid.styles.map((s) => [...s.tokens].map((t) => t.slice(0, 2)).sort().join(''));
  assert.deepEqual(names(learn(rooms, EMPTY)), ['a0a4a5', 'b0b1b2b3b4b5', 'c0c1c2c3c4c5']);
  const took = learn(rooms, EMPTY, { rules: { styles: [{ piece: tok('b', 4), take: [0, 1, 2, 3, 4, 5].map((r) => tok('c', r)) }] } });
  assert.deepEqual(names(took), ['b0b1b2b3b4b5c0c1c2c3c4c5', 'a0a4a5']);   // c moved into b; c, left empty, is gone
  // the floor material on its own, painted as floating blocks and a pillar: never a top piece with ground above it
  const m = learn(rooms, EMPTY, { rules: { styles: [{ piece: tok('a', 4), borrow: false }] } }), st = m.kinds.solid.styles[0];
  const paint = ['...............', '..####...#.....', '..####...#.....', '..####...#.....', '.........#.....', ...Array(8).fill('#'.repeat(15))];
  const W = 15, grid = { W, H: paint.length, art: Array(W * paint.length).fill(EMPTY), cls: [...paint.join('')].map((c) => (c === '#' ? 'solid' : 'air')) };
  const want = new Map(grid.cls.flatMap((c, i) => (c === 'solid' ? [[i, st]] : [])));
  retile(m, grid, [...want.keys()], want, 7);
  grid.cls.forEach((c, i) => { if (c === 'solid' && i >= W && grid.cls[i - W] === 'solid') assert.notEqual(grid.art[i], tok('a', 0), `top piece under ground at ${i % W},${(i / W) | 0}`); });
});

t('compression round trips', () => {
  for (let k = 0; k < 50; k++) {
    const len = rnd(3000);
    const d = Uint8Array.from({ length: len }, () => (rnd(4) ? rnd(3) : rnd(256)));
    assert.deepEqual(decompress(encodeLZ77(d)).data, d);
    assert.deepEqual(decompress(encodeRL(d)).data, d);
  }
});
{ // browser zlib (lib/zlib.mjs): stored deflate, and inflate of node zlib output
  const Z = await import('../lib/zlib.mjs');
  for (let k = 0; k < 30; k++) {
    const d = Uint8Array.from({ length: rnd(200000) }, () => (rnd(3) ? rnd(4) : rnd(256)));
    assert.deepEqual(new Uint8Array(zlib.inflateSync(Z.deflate(d))), d);
    assert.deepEqual(Z.inflate(new Uint8Array(zlib.deflateSync(d, { level: 1 + (k % 9) }))), d);
    assert.deepEqual(Z.inflate(Z.deflate(d)), d);
  }
  n++;
}
t('tile tokens', () => { for (let v = 0; v < 65536; v += 7) assert.equal(valueOf(tokenOf(v)), v); });
t('colours', () => { for (let c = 0; c < 32768; c += 3) assert.equal(bgr555(colorOf(c)), c); });
t('grid', () => {
  const vals = Array.from({ length: 200 }, () => rnd(150));
  const g = encodeGrid(vals, 20, 10);
  assert.deepEqual(decodeGrid(g, 20, 10), vals);
});
t('png tiles', () => {
  const tiles = Uint8Array.from({ length: 32 * 70 }, () => rnd(256));
  assert.deepEqual(pngToTiles(tilesToPng(tiles), 70), tiles);
});
t('scaffold compiles and survives decompile -> compile', () => {
  const vocab = indexVocab(buildVocab((p) => fs.readFileSync(path.join(ROOT, p), 'utf8')));
  const doc = scaffoldRoom('room:test/a', 20, 10);
  const layers = [{ docs: new Map([['room:test/a', { doc }]]) }];
  const ctx = makeContext({ vocab, registry: new Registry(), bindings: new Bindings(vocab, layers), external: true });
  const c = compileRoom(doc, ctx);
  const nat = { roomId: c.roomId, props: c.props, blobs: c.blobs, widthTiles: 40, heightTiles: 20 };
  const again = compileRoom({ ...decompileRoom(nat, ctx), id: 'room:test/a' }, ctx);
  for (const k of Object.keys(c.blobs)) assert.deepEqual(again.blobs[k], c.blobs[k], k);
});
{ // new rooms are numbered after the original ids (983+); the bundle links them
  const { nodeIO } = await import('../lib/io.mjs');
  const { loadStack, compileStack, bundleOf } = await import('../lib/pipeline.mjs');
  const { readBundle, BUNDLE_TABLES } = await import('../lib/native.mjs');
  const { loadRegistry } = await import('../lib/registry.mjs');
  const io = await nodeIO();
  const vocab = indexVocab(buildVocab((p) => fs.readFileSync(path.join(ROOT, p), 'utf8')));
  const registry = loadRegistry(io, path.join(ROOT, 'assets/schema/semantics'));
  const layers = loadStack(io, [path.join(ROOT, 'examples/authoring/sky_garden')]);
  const compiled = compileStack({ io, layers, vocab, registry, external: true });
  assert.deepEqual(compiled.errors, []);
  assert.deepEqual(compiled.bindings.allocated, [['room:sky_garden/meadow', 983], ['room:sky_garden/tower', 984]]);
  const b = bundleOf(compiled);
  const r = readBundle(b.bytes);
  const at = (table, idx) => r.entries.find((e) => e.table === table && e.index === idx);
  for (const id of [983, 984]) for (const tb of ['gRoomProps', 'gUnk_08D6CD0C', 'gUnk_0893CB44']) assert.ok(at(tb, id), `${tb}[${id}]`);
  for (const c of [287, 288]) for (const tb of ['gForegroundTilemaps', 'gSolidityMaps', 'gLevelObjLists', 'gUnk_08D640A4', 'gUnk_08D63C28']) assert.ok(at(tb, c), `${tb}[${c}]`);
  for (const e of r.entries) { assert.ok(BUNDLE_TABLES[e.table]); if (e.inline) assert.equal(r.blobs[e.blob].length, BUNDLE_TABLES[e.table][1]); }
  for (const x of r.relocs) { assert.ok(x.target < r.blobs.length); assert.ok(x.at + 4 <= r.blobs[x.blob].length); }
  // deterministic: the same project links to the same bytes
  assert.deepEqual(bundleOf(compileStack({ io, layers: loadStack(io, [path.join(ROOT, 'examples/authoring/sky_garden')]), vocab, registry, external: true })).bytes, b.bytes);
  n++;
}
console.log(`katam selftest: ${n} checks passed`);
