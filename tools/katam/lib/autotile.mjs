// Terrain brushes: paint "ground here" and get the edges, corners and a varied
// fill, in the room's own tileset.
//
// Nothing is hand-authored per tileset.  The rules are learned from the rooms
// of the original game that use the tileset:
//
//   - a cell's SHAPE is which of its neighbours are the same kind of terrain
//     (4 sides; when all 4 are, also the 4 diagonals: inner corners);
//   - for every terrain kind (ground, one-way platform, ladder, hazard, water)
//     and shape, which 16x16 pieces the original puts there, and how often;
//   - which pieces sit next to which (left-right and above-below pairs);
//   - a STYLE is a family of pieces that the original uses together (grass
//     ground vs. stone blocks): the connected groups of the "sits next to"
//     graph, cut at its weak links;
//   - which scenery piece the original puts in the air cell above (or below)
//     a given ground piece: the grass blades over a floor.
//
// Painting picks, for each cell, a piece of the chosen style that the original
// uses for that shape and that agrees with the neighbours already placed.
// Where several fit (a fill usually has many), the choice is random but
// seeded by the cell, so repainting does not reshuffle the room.
//
// Browser-safe, no dependencies.  The editor (web/editor/) is the user.

export const KINDS = ['solid', 'platform', 'slope', 'hazard', 'ladder', 'water', 'breakable', 'door', 'other'];

/** vocab.json collision "bits" -> the terrain kind the editor shows. */
export function classOfBits(bits) {
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

/** Kinds a brush exists for, and what counts as "the same terrain" next door. */
export const BRUSH_KINDS = {
  solid: { label: 'Ground', same: (c) => c === 'solid' || c === 'slope' || c === 'breakable', edge: true },
  platform: { label: 'Platform', same: (c) => c === 'platform', edge: false },
  ladder: { label: 'Ladder', same: (c) => c === 'ladder', edge: false },
  hazard: { label: 'Hazard', same: (c) => c === 'hazard', edge: false },
  water: { label: 'Water', same: (c) => c === 'water', edge: true },
};

const N = 1, E = 2, S = 4, W = 8, NE = 16, SE = 32, SW = 64, NW = 128;
const bump = (m, k, n = 1) => m.set(k, (m.get(k) || 0) + n);
const sub = (m, k) => { let v = m.get(k); if (!v) m.set(k, v = new Map()); return v; };
const popcount = (v) => { let c = 0; while (v) { c += v & 1; v >>= 1; } return c; };

/** The shape of cell (x, y): bits for neighbours of the same terrain.  Outside
 *  the room counts as the same terrain when `edge` (ground runs off the map). */
export function shapeOf(W_, H_, x, y, isSame, edge) {
  const at = (a, b) => (a < 0 || b < 0 || a >= W_ || b >= H_ ? edge : isSame(b * W_ + a));
  let m = (at(x, y - 1) ? N : 0) | (at(x + 1, y) ? E : 0) | (at(x, y + 1) ? S : 0) | (at(x - 1, y) ? W : 0);
  if (m === 15) m |= (at(x + 1, y - 1) ? NE : 0) | (at(x + 1, y + 1) ? SE : 0) | (at(x - 1, y + 1) ? SW : 0) | (at(x - 1, y - 1) ? NW : 0);
  return m;
}

/**
 * Learn a tileset's terrain from rooms: [{ W, H, art: [token], cls: [kind] }].
 * `empty` is the tileset's blank piece.
 */
export function learn(rooms, empty) {
  const model = { empty, kinds: {}, right: new Map(), down: new Map(), above: new Map(), below: new Map(), styleOf: new Map(), rim: new Set() };
  const count = new Map();                                  // token -> uses as terrain of its main kind
  const kindCount = new Map();                              // token -> Map(kind -> n)
  for (const kind of Object.keys(BRUSH_KINDS)) model.kinds[kind] = { shapes: new Map(), styles: [] };

  const hidden = (t) => t === empty || /^(...[.hvx]f ?){4}$/.test(t);
  for (const R of rooms) {
    const { W: w, H: h, art, cls } = R;
    for (const [kind, K] of Object.entries(BRUSH_KINDS)) {
      const isSame = (i) => K.same(cls[i]);
      const shapes = model.kinds[kind].shapes;
      for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
        const i = y * w + x;
        if (cls[i] !== kind) continue;
        const t = art[i];
        if (hidden(t)) continue;          // the blank / flat-black pieces the original hides deep inside walls
        bump(sub(shapes, shapeOf(w, h, x, y, isSame, K.edge)), t);
        bump(sub(kindCount, t), kind);
        if (x + 1 < w && K.same(cls[i + 1])) bump(sub(model.right, t), art[i + 1]);
        if (y + 1 < h && K.same(cls[i + w])) bump(sub(model.down, t), art[i + w]);
        if (y > 0 && cls[i - w] === 'air') bump(sub(model.above, t), art[i - w]);
        if (y + 1 < h && cls[i + w] === 'air') bump(sub(model.below, t), art[i + w]);
      }
    }
  }
  // a piece belongs to the kind it is mostly used as
  const mainKind = new Map();
  for (const [t, m] of kindCount) { let best = null, bn = 0, all = 0; for (const [k, n] of m) { all += n; if (n > bn) { best = k; bn = n; } } mainKind.set(t, best); count.set(t, bn); }
  for (const [kind, K] of Object.entries(model.kinds)) {
    for (const [shape, m] of K.shapes) { for (const t of [...m.keys()]) if (mainKind.get(t) !== kind) m.delete(t); if (!m.size) K.shapes.delete(shape); }
  }

  // styles: families of pieces used together.  Union pieces joined by a
  // strong "sits next to" link; weak links (a rare junction of two
  // materials) do not merge families.
  for (const [kind, K] of Object.entries(model.kinds)) {
    const toks = [...mainKind].filter(([, k]) => k === kind).map(([t]) => t);
    const parent = new Map(toks.map((t) => [t, t]));
    const find = (t) => { while (parent.get(t) !== t) { parent.set(t, parent.get(parent.get(t))); t = parent.get(t); } return t; };
    const link = (a, b, n) => {
      if (a === b || !parent.has(a) || !parent.has(b)) return;
      const need = Math.max(2, 0.08 * Math.min(count.get(a), count.get(b)));
      if (n >= need) parent.set(find(a), find(b));
    };
    for (const rel of [model.right, model.down]) for (const [a, m] of rel) for (const [b, n] of m) link(a, b, n + ((rel.get(b) || new Map()).get(a) || 0));
    const groups = new Map();
    for (const t of toks) { const r = find(t); if (!groups.has(r)) groups.set(r, []); groups.get(r).push(t); }
    const styles = [...groups.values()].map((list) => ({ tokens: new Set(list), n: list.reduce((a, t) => a + count.get(t), 0) })).sort((a, b) => b.n - a.n);
    // a style is worth a brush when it can at least fill and cap: enough uses, and some shape variety
    K.styles = styles.filter((s, i) => {
      const shapes = new Set();
      for (const [shape, m] of K.shapes) for (const t of m.keys()) if (s.tokens.has(t)) { shapes.add(shape & 15); break; }
      s.shapes = shapes.size;
      return s.n >= 12 && (kind === 'solid' ? shapes.size >= 3 : true) && i < 12;
    });
    K.styles.forEach((s, i) => { s.id = `${kind}:${i}`; s.kind = kind; s.index = i; for (const t of s.tokens) model.styleOf.set(t, s); });
  }

  // the scenery the original puts over / under terrain pieces (never the blank piece)
  for (const rel of [model.above, model.below]) for (const m of rel.values()) for (const t of m.keys()) if (t !== empty && !mainKind.has(t)) model.rim.add(t);
  model.count = count;
  return model;
}

// a small, fast, seedable hash -> [0, 1)
function rnd(x, y, salt) {
  let h = (x * 374761393 + y * 668265263 + salt * 2147483647) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177); h ^= h >>> 16;
  return (h >>> 0) / 4294967296;
}

/** Candidates for a shape within a style: the exact shape, else the nearest one the style has. */
function candidates(K, style, shape) {
  const pick = (sh) => { const m = K.shapes.get(sh); if (!m) return null; const out = []; for (const [t, n] of m) if (style.tokens.has(t)) out.push([t, n]); return out.length ? out : null; };
  const c = pick(shape);
  if (c) return c;
  // nearest shape by differing sides (ties: more sides closed, which reads as fill)
  let best = null, bd = 99;
  for (const sh of K.shapes.keys()) {
    const d = popcount((sh ^ shape) & 15) * 4 + popcount((sh ^ shape) >> 4);
    if (d < bd) { const p = pick(sh); if (p) { best = p; bd = d; } }
  }
  return best;
}

/**
 * Re-pick the pieces of `cells` (indices) in a grid:
 *   grid = { W, H, art: [token], cls: [kind] }           (cls already says what each cell is)
 *   want = Map(index -> style) for cells just painted; other terrain cells keep
 *          the style of the piece they have (pieces of no style are left alone).
 * Returns the indices whose art changed.  Also tends the scenery in the air
 * cells directly above and below the touched terrain.
 */
export function retile(model, grid, cells, want = new Map(), salt = 0) {
  const { W: w, H: h, art, cls } = grid;
  const changed = new Set();
  const region = [...new Set(cells)].filter((i) => i >= 0 && i < w * h).sort((a, b) => a - b);
  const styleAt = (i) => want.get(i) || (BRUSH_KINDS[cls[i]] ? model.styleOf.get(art[i]) : null) || null;
  const inRegion = new Set(region);
  const set = (i, t) => { if (art[i] !== t) { art[i] = t; changed.add(i); } };

  for (let pass = 0; pass < 2; pass++) {
    const done = new Set();
    for (const i of region) {
      const style = styleAt(i);
      if (!style || style.kind !== cls[i]) continue;
      const K = model.kinds[style.kind], B = BRUSH_KINDS[style.kind];
      const x = i % w, y = (i / w) | 0;
      const shape = shapeOf(w, h, x, y, (j) => B.same(cls[j]), B.edge);
      const cand = candidates(K, style, shape);
      if (!cand) continue;
      // neighbours that are settled: outside the region, or already placed this pass (all of them on pass 2)
      const settled = (j) => !inRegion.has(j) || done.has(j) || pass > 0;
      const pair = (rel, a, b) => ((rel.get(a) || new Map()).get(b) || 0);
      let best = -1e9;
      const scored = cand.map(([t, n]) => {
        let s = Math.log(1 + n);
        const nb = (j, rel, first) => {
          if (!B.same(cls[j]) || !settled(j)) return;
          const p = first ? pair(rel, art[j], t) : pair(rel, t, art[j]);
          s += p ? 1.5 * Math.log(1 + p) : -5;
        };
        if (x > 0) nb(i - 1, model.right, true);
        if (y > 0) nb(i - w, model.down, true);
        if (x + 1 < w) nb(i + 1, model.right, false);
        if (y + 1 < h) nb(i + w, model.down, false);
        if (s > best) best = s;
        return [t, s];
      });
      // among the near-best, a weighted, seeded draw: the varied fill
      const pool = scored.filter(([, s]) => s >= best - 1.2);
      let total = 0;
      for (const p of pool) { p[1] = Math.exp(p[1] - best); total += p[1]; }
      let r = rnd(x, y, salt) * total, pickTok = pool[0][0];
      for (const [t, wgt] of pool) { r -= wgt; if (r <= 0) { pickTok = t; break; } }
      set(i, pickTok);
      done.add(i);
    }
  }

  // scenery over and under: only blank cells and cells that hold such scenery already
  const air = new Set();
  for (const i of region) for (const j of [i - w, i + w, i]) if (j >= 0 && j < w * h && cls[j] === 'air') air.add(j);
  const top = (m) => { let b = null, bn = 0, all = 0; if (m) for (const [t, n] of m) { all += n; if (n > bn) { b = t; bn = n; } } return b && bn >= 0.5 * all ? b : null; };
  for (const j of air) {
    if (art[j] !== model.empty && !model.rim.has(art[j])) continue;
    const dn = j + w < w * h && BRUSH_KINDS[cls[j + w]] ? j + w : -1, up = j - w >= 0 && BRUSH_KINDS[cls[j - w]] ? j - w : -1;
    let t = null;
    if (dn >= 0) t = top(model.above.get(art[dn]));
    if (!t && up >= 0) t = top(model.below.get(art[up]));
    set(j, t || model.empty);
  }
  return [...changed];
}

/** A small sample of a style (cols x rows of pieces), for a swatch. */
export function sample(model, style, cols = 4, rows = 3) {
  const B = BRUSH_KINDS[style.kind];
  const w = cols + 2, h = rows + 2, art = new Array(w * h).fill(model.empty), cls = new Array(w * h).fill('air');
  const want = new Map(), cells = [];
  const y0 = style.kind === 'solid' || style.kind === 'water' ? 2 : style.kind === 'ladder' ? 1 : 2;
  for (let y = y0; y < h; y++) for (let x = 1; x < w - 1; x++) {
    if (style.kind === 'platform' || style.kind === 'hazard') { if (y !== y0) continue; }
    if (style.kind === 'ladder' && x !== (w >> 1)) continue;
    const i = y * w + x; cls[i] = style.kind; want.set(i, style); cells.push(i);
  }
  // the sample's bottom row runs off the picture like ground does
  retile(model, { W: w, H: h + (B.edge ? 0 : 0), art, cls }, cells, want, 7);
  return { W: w, H: h, art };
}
