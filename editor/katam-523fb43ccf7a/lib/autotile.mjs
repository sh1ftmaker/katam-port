// Terrain brushes: paint "ground here" and get the edges, corners and fill,
// in the room's own tileset.
//
// Mario-Maker-style editors do this with tile sets drawn for it: one piece
// per neighbour shape (16 or 47 of them) and a lookup.  This game's tile sets
// were not drawn that way: its rooms are hand-built from hundreds of pieces
// per set, a floor is three or four different rows deep, its fill is a
// pattern several pieces wide, and one set holds several materials (grass
// ground, stone blocks, clouds).  A per-shape lookup over that mixes
// materials and breaks the patterns.
//
// So the brushes work BY EXAMPLE, from the rooms of the original game that
// use the tile set ("image analogies": the original's collision is to its
// art as your collision is to the art wanted):
//
//   - a STYLE is a material: a community of pieces the original puts next
//     to each other (label propagation over the "sits next to" graph, so a
//     rare junction between two materials does not merge them);
//   - each terrain cell is drawn by COPYING the piece of a cell of the
//     original, of that style, chosen so that
//       1. its surroundings have the same shape as yours (same terrain or
//          not, two cells out in every direction: that tells a top row from
//          the row under it from deep fill, and an inner from an outer
//          corner),
//       2. the pieces already placed next to yours are the pieces next to
//          it in the original (edges line up, patterns continue),
//       3. where it can, it is the NEIGHBOUR of the cell your neighbour was
//          copied from: whole runs of the original come across intact, and
//          the fill varies the way the original's does;
//   - ties are broken by a seeded draw, so repainting does not reshuffle;
//   - the scenery the original puts over or under a ground piece (grass
//     blades over a floor) follows the piece.
//
// Browser-safe, no dependencies.  The editor (web/editor/) is the user.

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

// the 24 cells around a cell: the ring next to it first (bits 0-7), then the ring two out
const NEAR = [[-1, -1], [0, -1], [1, -1], [-1, 0], [1, 0], [-1, 1], [0, 1], [1, 1]];
const FAR = [];
for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) if (Math.max(Math.abs(dx), Math.abs(dy)) === 2) FAR.push([dx, dy]);
const AROUND = NEAR.concat(FAR);
const SIDE = [0, 1, 0, 1, 1, 0, 1, 0];          // NEAR[i] shares an edge (not just a corner)
const SRC_SHIFT = 11, SRC_MASK = (1 << SRC_SHIFT) - 1;   // a source cell: room << 11 | index (a room has at most 1950 cells)

const bump = (m, k, n = 1) => m.set(k, (m.get(k) || 0) + n);
const sub = (m, k) => { let v = m.get(k); if (!v) m.set(k, v = new Map()); return v; };
const popcount = (v) => { v -= (v >>> 1) & 0x55555555; v = (v & 0x33333333) + ((v >>> 2) & 0x33333333); return (((v + (v >>> 4)) & 0x0f0f0f0f) * 0x01010101) >>> 24; };

/** Which of the 24 cells around (x, y) are the same terrain: the cell's shape. */
function shapeBits(w, h, x, y, isSame, edge) {
  let m = 0;
  for (let k = 0; k < 24; k++) {
    const a = x + AROUND[k][0], b = y + AROUND[k][1];
    if (a < 0 || b < 0 || a >= w || b >= h ? edge : isSame(b * w + a)) m |= 1 << k;
  }
  return m;
}

/** How far the same terrain runs up, right, down and left of a cell (0-4 each), packed. */
function depthOf(w, h, x, y, isSame, edge) {
  let out = 0;
  for (let d = 0; d < 4; d++) {
    const dx = [0, 1, 0, -1][d], dy = [-1, 0, 1, 0][d];
    let n = 0;
    for (; n < 4; n++) { const a = x + dx * (n + 1), b = y + dy * (n + 1); if (!(a < 0 || b < 0 || a >= w || b >= h ? edge : isSame(b * w + a))) break; }
    out |= n << (d * 4);
  }
  return out;
}
const depthDiff = (a, b) => Math.abs((a & 15) - (b & 15)) + Math.abs(((a >> 4) & 15) - ((b >> 4) & 15)) + Math.abs(((a >> 8) & 15) - ((b >> 8) & 15)) + Math.abs(((a >> 12) & 15) - ((b >> 12) & 15));

/**
 * Learn a tileset's terrain from rooms: [{ W, H, art: [token], cls: [kind] }].
 * `empty` is the tileset's blank piece; `opaque(token)` says a piece has no
 * see-through pixel (the caller has the pixels; this module does not).
 */
export function learn(rooms, empty, { opaque = () => false } = {}) {
  // the blank and flat-black pieces the original leaves where the camera never looks
  const hidden = (t) => t === empty || /^(...[.hvx]f ?){4}$/.test(t);
  const model = { empty, rooms: [], kinds: {}, styleOf: new Map(), rim: new Set() };
  const kindCount = new Map(), graph = new Map(), airUse = new Map();
  for (const kind of Object.keys(BRUSH_KINDS)) model.kinds[kind] = { styles: [] };

  for (const R of rooms) {
    const { W: w, H: h, art, cls } = R;
    if (w * h > SRC_MASK) continue;
    const shape = new Int32Array(w * h), depth = new Uint16Array(w * h), ok = new Uint8Array(w * h);
    for (let i = 0; i < w * h; i++) {
      const K = BRUSH_KINDS[cls[i]];
      if (!K) continue;
      const x = i % w, y = (i / w) | 0, t = art[i];
      shape[i] = shapeBits(w, h, x, y, (j) => K.same(cls[j]), K.edge);
      depth[i] = depthOf(w, h, x, y, (j) => K.same(cls[j]), K.edge);
      if (hidden(t)) continue;
      ok[i] = 1;
      bump(sub(kindCount, t), cls[i]);
      // "sits next to": right and below, among cells of the same terrain
      for (const j of [x + 1 < w ? i + 1 : -1, y + 1 < h ? i + w : -1]) if (j >= 0 && K.same(cls[j]) && !hidden(art[j])) { bump(sub(graph, t), art[j]); bump(sub(graph, art[j]), t); }   // a piece next to itself votes for staying what it is
    }
    // air pieces and how often they sit against terrain: the art a ground needs beside it
    for (let i = 0; i < w * h; i++) {
      if (cls[i] !== 'air' || art[i] === empty) continue;
      const x = i % w, y = (i / w) | 0;
      let near = 0;
      for (const [dx, dy] of NEAR) { const a = x + dx, b = y + dy; if (a >= 0 && b >= 0 && a < w && b < h && BRUSH_KINDS[cls[b * w + a]]) { near = 1; break; } }
      const e = airUse.get(art[i]) || [0, 0]; e[0]++; e[1] += near; airUse.set(art[i], e);
    }
    model.rooms.push({ W: w, H: h, art, cls, shape, depth, ok, style: new Array(w * h).fill(null) });
  }
  const mainKind = new Map(), count = new Map();
  for (const [t, m] of kindCount) { let best = null, bn = 0; for (const [k, n] of m) if (n > bn) { best = k; bn = n; } mainKind.set(t, best); count.set(t, bn); }

  // styles: label propagation.  Every piece takes the label most of its
  // neighbours (by how often they touch) carry, until nothing changes.
  for (const kind of Object.keys(BRUSH_KINDS)) {
    const toks = [...mainKind].filter(([, k]) => k === kind).map(([t]) => t).sort((a, b) => count.get(b) - count.get(a) || (a < b ? -1 : 1));
    const label = new Map(toks.map((t) => [t, t]));
    for (let it = 0; it < 30; it++) {
      let moved = 0;
      for (const t of toks) {
        const votes = new Map();
        for (const [u, n] of graph.get(t) || []) if (label.has(u)) bump(votes, label.get(u), n);
        let best = label.get(t), bn = votes.get(best) || 0;
        for (const [l, n] of votes) if (n > bn || (n === bn && count.get(l) > count.get(best))) { best = l; bn = n; }
        if (best !== label.get(t)) { label.set(t, best); moved++; }
      }
      if (!moved) break;
    }
    // A material is more than a set of pieces: the grass on top of cobble and
    // the grass on top of a cliff are the same pieces.  So styles are given to
    // CELLS: each cell of the original takes the label most common around it
    // (two cells out, what lies under it counting double), and a strip of
    // grass joins the body it sits on.
    const tally = new Map(), size = new Map();
    for (const l of label.values()) bump(size, l);
    const block = (l) => size.get(l) <= 2;          // a piece stacked whole (a stone block): not part of anything's body
    model.rooms.forEach((R) => {
      const K = BRUSH_KINDS[kind], mine = new Array(R.W * R.H).fill(null);
      for (let i = 0; i < R.W * R.H; i++) {
        if (!R.ok[i] || R.cls[i] !== kind || !label.has(R.art[i])) continue;
        const x = i % R.W, y = (i / R.W) | 0, votes = new Map();
        for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) {
          const a = x + dx, b = y + dy;
          if (a < 0 || b < 0 || a >= R.W || b >= R.H) continue;
          const j = b * R.W + a;
          if (R.ok[j] && K.same(R.cls[j]) && label.has(R.art[j]) && !block(label.get(R.art[j]))) bump(votes, label.get(R.art[j]), dy > 0 ? 2 : 1);
        }
        let best = label.get(R.art[i]), bn = 0;
        if (!block(best)) for (const [l, n] of votes) if (n > bn || (n === bn && l === label.get(R.art[i]))) { best = l; bn = n; }
        mine[i] = best; bump(tally, best);
      }
      R['_' + kind] = mine;
    });
    const styles = [...tally].sort((a, b) => b[1] - a[1]).map(([l, n]) => ({ kind, key: l, tokens: new Set(), n, cells: [], byShape: new Map() }));
    const byKey = new Map(styles.map((s) => [s.key, s]));
    model.rooms.forEach((R, r) => {
      const mine = R['_' + kind]; delete R['_' + kind];
      for (let i = 0; i < mine.length; i++) {
        if (!mine[i]) continue;
        const s = byKey.get(mine[i]), id = (r << SRC_SHIFT) | i, key = R.shape[i] & 255;
        R.style[i] = s; s.tokens.add(R.art[i]); s.cells.push(id);
        let l = s.byShape.get(key); if (!l) s.byShape.set(key, l = []); l.push(id);
      }
    });
    model.kinds[kind].styles = styles;
  }
  // a style is worth a brush when the original uses it enough; a material with a
  // surface and a body is "Ground", a piece only ever stacked whole is a "Block"
  for (const [kind, K] of Object.entries(model.kinds)) {
    const most = K.styles.length ? K.styles[0].cells.length : 0;
    // a ground needs a body: styles that are all surface (strips of grass) or rare are not brushes
    const deep = (s) => s.cells.filter((id) => (model.rooms[id >> SRC_SHIFT].depth[id & SRC_MASK] & 15) >= 2).length;
    K.styles = K.styles.filter((s) => (s.tokens.size <= 2 ? s.cells.length >= 12 : s.cells.length >= Math.max(kind === 'solid' ? 16 : 8, 0.06 * most) && (kind !== 'solid' || (deep(s) >= 0.25 * s.cells.length && s.byShape.size >= 10)))).slice(0, 6);   // ...and one seen in too few shapes cannot follow a brush
    // how usual each piece is for its shape, within the style: the original's one-off
    // details (a tuft of grass deep inside a wall) should stay rare in what is painted
    for (const s of K.styles) {
      s.usual = new Map();
      for (const [key, list] of s.byShape) {
        const m = new Map(); let top = 0;
        for (const id of list) { const t = model.rooms[id >> SRC_SHIFT].art[id & SRC_MASK]; bump(m, t); top = Math.max(top, m.get(t)); }
        for (const [t, n] of m) m.set(t, n / top);
        s.usual.set(key, m);
      }
    }
    const names = new Map();
    K.styles.forEach((s) => {
      s.label = kind === 'solid' && s.tokens.size <= 2 ? 'Block' : BRUSH_KINDS[kind].label;
      const n = (names.get(s.label) || 0) + 1; names.set(s.label, n);
      s.index = n - 1; s.id = `${kind}:${s.label}:${n}`; s.name = `${s.label} ${n}`;
    });
  }
  // a piece seen on its own (a room loaded from a file) reads as the style that uses it most
  const uses = new Map();
  for (const R of model.rooms) for (let i = 0; i < R.style.length; i++) {
    const st = R.style[i];
    if (st && st.id) bump(sub(uses, R.art[i]), st); else { R.style[i] = null; if (st) R.ok[i] = R.ok[i] && 0; }
  }
  for (const [t, m] of uses) { let best = null, bn = 0; for (const [st, n] of m) if (n > bn) { best = st; bn = n; } model.styleOf.set(t, best); }
  // EDGE ART: the pieces the original draws in the air cells against terrain (grass
  // blades over a floor, the lip of a ledge, the underside of a ceiling), as opposed to
  // scenery that merely stands near it.  The brush owns these and nothing else in the air.
  // A piece that is mostly terrain itself, or a full opaque square (a back wall), is not an edge.
  for (const [t, [all, near]] of airUse) if (near >= 2 && near >= 0.7 * all && (count.get(t) || 0) < near && !opaque(t)) model.rim.add(t);
  return model;
}

// a small, fast, seedable hash -> [0, 1)
function rnd(x, y, salt) {
  let h = (x * 374761393 + y * 668265263 + salt * 2147483647) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177); h ^= h >>> 16;
  return (h >>> 0) / 4294967296;
}

const W_NEAR = 6, W_FAR = 1, W_DEPTH = 1.5, W_ODD = 5, W_SIDE = 5, W_CORNER = 2, COHERENT = 1.6, MAX_TRY = 72;

/**
 * Re-pick the pieces of `cells` (indices) in a grid:
 *   grid = { W, H, art: [token], cls: [kind], src?: Int32Array }
 *   want = Map(index -> style) for cells just painted; other terrain cells keep
 *          the style of the piece they have (pieces of no style are left alone).
 * `grid.src`, when given (one entry per cell, -1 = unknown), remembers which
 * cell of the original each cell was copied from, between calls.
 * Returns the indices whose art changed.  Also tends the scenery in the air
 * cells directly above and below the touched terrain.
 */
export function retile(model, grid, cells, want = new Map(), salt = 0) {
  const { W: w, H: h, art, cls } = grid;
  const src = grid.src && grid.src.length === w * h ? grid.src : new Int32Array(w * h).fill(-1);
  const changed = new Set();
  const region = [...new Set(cells)].filter((i) => i >= 0 && i < w * h).sort((a, b) => a - b);
  const inRegion = new Uint8Array(w * h);
  for (const i of region) inRegion[i] = 1;
  const styleAt = (i) => want.get(i) || (!BRUSH_KINDS[cls[i]] ? null : src[i] >= 0 && model.rooms[src[i] >> SRC_SHIFT].art[src[i] & SRC_MASK] === art[i] ? model.rooms[src[i] >> SRC_SHIFT].style[src[i] & SRC_MASK] : model.styleOf.get(art[i])) || null;
  const set = (i, t) => { if (art[i] !== t) { art[i] = t; changed.add(i); } };
  const todo = region.filter((i) => { const s = styleAt(i); return s && s.kind === cls[i]; });
  for (const i of todo) if (want.has(i)) src[i] = -1;

  for (let pass = 0; pass < 3; pass++) {
    const done = new Uint8Array(w * h);
    let moved = 0;
    for (const i of todo) {
      const style = styleAt(i), B = BRUSH_KINDS[style.kind];
      const x = i % w, y = (i / w) | 0;
      const shape = shapeBits(w, h, x, y, (j) => B.same(cls[j]), B.edge), depth = depthOf(w, h, x, y, (j) => B.same(cls[j]), B.edge);
      // the neighbours whose pieces are settled: all of them after the first pass
      const nb = [];
      for (let k = 0; k < 8; k++) {
        const a = x + NEAR[k][0], b = y + NEAR[k][1];
        if (a < 0 || b < 0 || a >= w || b >= h) continue;
        const j = b * w + a;
        if (!B.same(cls[j]) || (pass === 0 && inRegion[j] && !done[j] && want.has(j))) continue;
        nb.push(k, j);
      }
      // candidates: the cell beside the one each neighbour was copied from, then cells of the same shape
      const cand = new Map();
      for (let n = 0; n < nb.length; n += 2) {
        const k = nb[n], s = src[nb[n + 1]];
        if (s < 0) continue;
        const R = model.rooms[s >> SRC_SHIFT], si = s & SRC_MASK, sx = si % R.W - NEAR[k][0], sy = ((si / R.W) | 0) - NEAR[k][1];
        if (sx < 0 || sy < 0 || sx >= R.W || sy >= R.H) continue;
        const c = sy * R.W + sx;
        if (!R.ok[c] || R.style[c] !== style) continue;
        const id = (s & ~SRC_MASK) | c;
        cand.set(id, (cand.get(id) || 0) + 1);
      }
      const list = style.byShape.get(shape & 255) || style.cells;
      const step = Math.max(1, Math.floor(list.length / MAX_TRY)), off = Math.floor(rnd(x, y, salt + 3) * step);
      for (let q = off; q < list.length; q += step) if (!cand.has(list[q])) cand.set(list[q], 0);

      let best = -1, bestScore = 1e9;
      for (const [id, votes] of cand) {
        const R = model.rooms[id >> SRC_SHIFT], si = id & SRC_MASK, d = R.shape[si] ^ shape;
        const usual = (style.usual.get(R.shape[si] & 255) || new Map()).get(R.art[si]) || 0;
        let score = W_NEAR * popcount(d & 255) + W_FAR * popcount(d >>> 8) + W_DEPTH * depthDiff(R.depth[si], depth) + W_ODD * (1 - Math.sqrt(usual)) - COHERENT * Math.min(votes, 3) + rnd(x * 31 + (id & 1023), y * 17 + (id >> 10), salt) * 0.9;
        const sx = si % R.W, sy = (si / R.W) | 0;
        for (let n = 0; n < nb.length && score < bestScore; n += 2) {
          const k = nb[n], a = sx + NEAR[k][0], b = sy + NEAR[k][1];
          const there = a < 0 || b < 0 || a >= R.W || b >= R.H ? null : R.art[b * R.W + a];
          if (there !== art[nb[n + 1]]) score += SIDE[k] ? W_SIDE : W_CORNER;
        }
        if (score < bestScore) { bestScore = score; best = id; }
      }
      done[i] = 1;
      if (best < 0) continue;
      const t = model.rooms[best >> SRC_SHIFT].art[best & SRC_MASK];
      if (src[i] !== best || art[i] !== t) moved++;
      src[i] = best; set(i, t);
    }
    if (!moved) break;
  }

  // The air around the terrain.  The original's ground does not stop at its
  // collision: its art runs on into the air cells beside it.  Each air cell
  // next to terrain takes what the original has in the same place relative to
  // the cells the terrain was copied from (what is under an air cell counting
  // most).  Only blank cells and cells that already hold edge art are touched:
  // scenery placed by hand stays.
  const air = new Set();
  for (const i of todo.length ? region : []) {
    const x = i % w, y = (i / w) | 0;
    for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) { const a = x + dx, b = y + dy; if (a >= 0 && b >= 0 && a < w && b < h && cls[b * w + a] === 'air') air.add(b * w + a); }
  }
  for (const i of region) if (cls[i] === 'air') air.add(i);
  for (const j of air) {
    src[j] = -1;
    if (art[j] !== model.empty && !model.rim.has(art[j])) continue;
    const x = j % w, y = (j / w) | 0, votes = new Map();
    const around = (W_, H_, cl, px, py) => { let m = 0; for (let k = 0; k < 8; k++) { const a = px + NEAR[k][0], b = py + NEAR[k][1]; if (a >= 0 && b >= 0 && a < W_ && b < H_ && BRUSH_KINDS[cl[b * W_ + a]]) m |= 1 << k; } return m; };
    const here = around(w, h, cls, x, y);
    let terrain = 0, known = 0;
    for (let k = 0; k < 8; k++) {
      const a = x + NEAR[k][0], b = y + NEAR[k][1];
      if (a < 0 || b < 0 || a >= w || b >= h || !BRUSH_KINDS[cls[b * w + a]]) continue;
      terrain++;
      const s = src[b * w + a];
      if (s < 0) continue;
      known++;
      const R = model.rooms[s >> SRC_SHIFT], si = s & SRC_MASK, sx = si % R.W - NEAR[k][0], sy = ((si / R.W) | 0) - NEAR[k][1];
      if (sx < 0 || sy < 0 || sx >= R.W || sy >= R.H || R.cls[sy * R.W + sx] !== 'air') continue;
      if (around(R.W, R.H, R.cls, sx, sy) !== here) continue;      // the original's cell sits against other terrain too: its art is about that
      const t = R.art[sy * R.W + sx];
      bump(votes, model.rim.has(t) ? t : model.empty, !SIDE[k] ? 1 : NEAR[k][1] === 1 ? 4 : 3);
    }
    if (!terrain) { set(j, model.empty); continue; }
    if (!known || !votes.size) continue;        // terrain the brush did not place: leave its edges alone
    let best = model.empty, bn = 0;
    for (const [t, n] of votes) if (n > bn) { best = t; bn = n; }
    set(j, best);
  }
  return [...changed];
}

/** A small sample of a style (cols x rows of pieces), for a swatch. */
export function sample(model, style, cols = 4, rows = 3) {
  const w = cols + 2, h = rows + 2, art = new Array(w * h).fill(model.empty), cls = new Array(w * h).fill('air');
  const want = new Map(), cells = [];
  const body = style.kind === 'solid' || style.kind === 'water';
  for (let y = body ? 2 : 2; y < h; y++) for (let x = 1; x < w - 1; x++) {
    if (!body && style.kind !== 'ladder' && y !== 2) continue;
    if (style.kind === 'ladder' && x !== (w >> 1)) continue;
    const i = y * w + x; cls[i] = style.kind; want.set(i, style); cells.push(i);
  }
  retile(model, { W: w, H: h, art, cls }, cells, want, 7);
  return { W: w, H: h, art };
}
