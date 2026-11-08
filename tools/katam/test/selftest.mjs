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
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
setZlib({ deflate: (b) => new Uint8Array(zlib.deflateSync(b)), inflate: (b) => new Uint8Array(zlib.inflateSync(b)) });
let seed = 1;
const rnd = (n) => { seed = (seed * 1103515245 + 12345) >>> 0; return seed % n; };
let n = 0;
const t = (name, f) => { f(); n++; };

t('compression round trips', () => {
  for (let k = 0; k < 50; k++) {
    const len = rnd(3000);
    const d = Uint8Array.from({ length: len }, () => (rnd(4) ? rnd(3) : rnd(256)));
    assert.deepEqual(decompress(encodeLZ77(d)).data, d);
    assert.deepEqual(decompress(encodeRL(d)).data, d);
  }
});
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
console.log(`katam selftest: ${n} checks passed`);
