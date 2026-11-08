// The runtime ("native") layer: GBA-shaped records and payloads, exactly as the
// game reads them.  This module is the ONLY place that knows those layouts.
//
// A native value is plain data: Uint8Array blobs whose pointer fields are left
// zero, plus `links` naming what each pointer field points at.  Placement (ROM
// addresses for identity, arena addresses for a mod) is a separate, later step,
// so two natives can be compared blob by blob without caring where either lives.
//
//   readRoomNative(rom, roomId)      ROM -> native        (decompile input, gate reference)
//   readTilesetNative(rom, idx)      gForegroundInfo[idx]
//   readBackgroundNative(rom, idx)   gRoomTiledBGs[idx]
//   compareNative(a, b)              blob-by-blob diff for the round-trip gate
//
// Layout facts and where they were established are in docs/ROOM-FORMAT.md,
// docs/naming/room-tables.md and docs/naming/doors.md; every terminator size
// below was measured over all 286 rooms (docs/design/authoring/rooms.md).
import { u8, u16, u32, put } from './bytes.mjs';
import { decompress } from './compress.mjs';

export const ROOM_PROPS_SIZE = 0x28;
export const NUM_ROOM_SLOTS = 983;
export const NUM_CONTENT = 287;
export const TILED_BG_SIZE = 0x20;
export const TEMPLATE_SIZE = 0x24;
export const CELL_PATCH_SIZE = 0x12;      // struct Unk_0888562C   (tag 1)
export const CELL_DOOR_SIZE = 0x0e;       // struct Unk_0888562C_3 (tag 3)
export const NAV_NODE_SIZE = 0x34;        // struct LevelInfo_1E8_14
export const COLLISION_CAP = 1950;        // sizeof gUnk_02024ED0[slot]
export const NAV_MAX_NODES = 32;          // LevelInfo_1E8_14.unk0[0x20]

// Terminators, measured on every vanilla list (all 286 rooms, one shape each).
export const OBJ_TERMINATOR = Uint8Array.from([0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]);
export const CELL_TERMINATOR = Uint8Array.from([0, 0, 0, 0, 0, 0, 0xff, 0xff]);
export const PROGRESS_TERMINATOR = Uint8Array.from([0, 0, 0xff, 0xff]);

/** RoomProps field table: [name, offset, size, signed] */
export const ROOM_PROPS_FIELDS = [
  ['songIdx', 0, 2, false], ['unk02', 2, 2, false], ['unk04', 4, 2, false],
  ['unkXmod_06', 6, 2, true], ['unkXmod_08', 8, 2, true], ['unk0A', 10, 2, false],
  ['unk0C', 12, 2, false], ['unk0E', 14, 2, true], ['unk10', 16, 2, true],
  ['priorityFlags', 18, 2, false], ['pixelDataIdx', 20, 2, false], ['paletteDataIdx', 22, 2, false],
  ['mapDataIdx', 24, 2, false], ['solidityMapIdx', 26, 2, false], ['objectList2Idx', 28, 2, false],
  ['backgroundIdx', 30, 2, false], ['objectListIdx', 32, 2, false], ['unk22', 34, 2, false],
  ['doorsIdx', 36, 2, false], ['unk26_0', 38, 1, false], ['unk26_1', 39, 1, false],
];

/** struct ObjectTemplate (include/data.h): [name, offset, size, signed] */
export const TEMPLATE_FIELDS = [
  ['spawnTable', 0, 1, false], ['unk1', 1, 1, false], ['unk2', 2, 1, false], ['unk3', 3, 1, false],
  ['unk4', 4, 1, false], ['unk5', 5, 1, false], ['x', 6, 2, true], ['y', 8, 2, true],
  ['unkA', 10, 1, false], ['unkB', 11, 1, false], ['type', 12, 1, false], ['unkD', 13, 1, false],
  ['subtype1', 14, 1, false], ['unkF', 15, 1, false], ['subtype2', 16, 1, false], ['unk11', 17, 1, false],
  ['unk12', 18, 2, false], ['unk14', 20, 2, false], ['unk16', 22, 2, false], ['unk18', 24, 2, false],
  ['unk1A', 26, 2, true], ['unk1C', 28, 2, true], ['unk1E', 30, 2, false], ['unk20', 32, 2, false],
  ['unk22', 34, 2, false],
];

export function getField(b, off, size, signed) {
  let v = size === 1 ? b[off] : size === 2 ? u16(b, off) : u32(b, off);
  if (signed) v = size === 1 ? (v << 24) >> 24 : size === 2 ? (v << 16) >> 16 : v | 0;
  return v;
}

export function setField(b, off, size, v) {
  put(b, off, size, v);
}

export function readFields(b, fields, base = 0) {
  const out = {};
  for (const [n, o, s, sg] of fields) out[n] = getField(b, base + o, s, sg);
  return out;
}

export function writeFields(fields, values, size) {
  const b = new Uint8Array(size);
  for (const [n, o, s] of fields) setField(b, o, s, values[n] ?? 0);
  return b;
}

/** Room ids that are real rooms (the other slots are 0xFFFF filler). */
export function vanillaRoomIds(rom) {
  const base = rom.table('gRoomProps');
  const out = [];
  for (let r = 0; r < NUM_ROOM_SLOTS; r++) if (rom.u16(base + r * ROOM_PROPS_SIZE + 0x20) < NUM_CONTENT) out.push(r);
  return out;
}

function copy(b) { return Uint8Array.from(b); }

function zeroed(b, offs) {
  const c = copy(b);
  for (const o of offs) put(c, o, 4, 0);
  return c;
}

function lz(rom, addr) {
  const d = decompress(rom.from(addr));
  return { data: d.data, packed: copy(rom.slice(addr, (d.consumed + 3) & ~3)) };
}

/** Walk a self-striding list.  strideAt(addr) -> stride (0 ends). Returns [records..., end]. */
function walk(rom, addr, tagOf, strideOf) {
  const recs = [];
  for (let n = 0; ; n++) {
    if (n > 4096) throw new Error(`list at 0x${addr.toString(16)} does not terminate`);
    const st = strideOf(addr);
    if (tagOf(addr) === 0 || st === 0) return { recs, end: addr };
    recs.push({ addr, stride: st });
    addr += st;
  }
}

/** Everything one room owns, as native blobs.  Shared resources (tileset,
 *  fg palette, background) are referenced by index, not copied. */
export function readRoomNative(rom, roomId) {
  const pAddr = rom.table('gRoomProps') + roomId * ROOM_PROPS_SIZE;
  const propsBytes = copy(rom.slice(pAddr, ROOM_PROPS_SIZE));
  const props = readFields(propsBytes, ROOM_PROPS_FIELDS);
  const n = { roomId, props, blobs: {} };
  const B = n.blobs;
  B.props = propsBytes;

  // Playfield layer (BG3): RoomTiledBG + LZ77 tilemap.  Its tileset/palette
  // fields are zero in every vanilla entry; the real ones come via
  // gForegroundInfo / gForegroundPalettes.
  const fg = rom.entry('gForegroundTilemaps', props.mapDataIdx);
  const fgHdr = rom.slice(fg, TILED_BG_SIZE);
  for (const o of [0x08, 0x10]) if (u32(fgHdr, o)) throw new Error(`room ${roomId}: fg layer has its own tileset/palette pointer (not seen in vanilla)`);
  B.fgHeader = zeroed(fgHdr, [0x18]);
  const fgTm = lz(rom, rom.ptr(fg + 0x18));
  B.fgTilemap = fgTm.data;
  B.fgTilemapPacked = fgTm.packed;
  n.widthTiles = u16(fgHdr, 0);
  n.heightTiles = u16(fgHdr, 2);

  // Extra layer (BG2): never used in vanilla (objectList2Idx == 0xFFFF everywhere).
  if (props.objectList2Idx !== 0xffff) throw new Error(`room ${roomId}: extra BG layer present (not seen in vanilla)`);

  // Collision: RL map + cell records (tag 1 patch, tag 3 door), sorted by (y, x).
  const sm = rom.entry('gSolidityMaps', props.solidityMapIdx);
  const col = lz(rom, rom.ptr(sm));
  B.collision = col.data;
  B.collisionPacked = col.packed;
  const cl = walk(rom, rom.u32(sm + 4), (a) => rom.u8(a), (a) => rom.u16(a + 4));
  B.cellRecords = copy(rom.slice(cl.recs.length ? cl.recs[0].addr : cl.end, cl.end - (cl.recs.length ? cl.recs[0].addr : cl.end)));
  B.cellTerminator = copy(rom.slice(cl.end, CELL_TERMINATOR.length));

  // Object list: LevelInfo_1E4 header, templates, derived type set.
  const ol = rom.entry('gLevelObjLists', props.objectListIdx);
  const olHdr = rom.slice(ol, 0x10);
  const tl = walk(rom, rom.ptr(ol), () => 1, (a) => rom.u8(a + 1));
  const t0 = tl.recs.length ? tl.recs[0].addr : tl.end;
  B.templates = copy(rom.slice(t0, tl.end - t0));
  B.objTerminator = copy(rom.slice(tl.end, OBJ_TERMINATOR.length));
  B.objHeader = zeroed(olHdr, [0, 4, 8]);
  B.objTypes = copy(rom.slice(u32(olHdr, 4), olHdr[0x0c]));
  B.objTypes2 = copy(rom.slice(u32(olHdr, 8), olHdr[0x0d]));

  // Progress list (gUnk_08D640A4): chests then door shadows, 8-byte entries.
  const pl = rom.entry('gUnk_08D640A4', props.doorsIdx);
  const plHdr = rom.slice(pl, 8);
  const pw = walk(rom, rom.u32(pl), (a) => rom.u8(a + 1), (a) => rom.u8(a + 1));
  const p0 = pw.recs.length ? pw.recs[0].addr : pw.end;
  B.progress = copy(rom.slice(p0, pw.end - p0));
  B.progressTerminator = copy(rom.slice(pw.end, PROGRESS_TERMINATOR.length));
  B.progressHeader = zeroed(plHdr, [0]);

  // Nav graph (gUnk_08D63C28): 4 per-player scripts (u16, 0xFFFF-terminated),
  // RL node map, node array.
  const nv = rom.entry('gUnk_08D63C28', props.unk22);
  const nvHdr = rom.slice(nv, 0x1c);
  B.navHeader = zeroed(nvHdr, [0, 4, 8, 12, 0x10, 0x14]);
  for (let i = 0; i < 4; i++) {
    let a = rom.ptr(nv + 4 * i), len = 0;
    while (rom.u16(a + len) !== 0xffff) { len += 2; if (len > 0x2000) throw new Error('nav script does not terminate'); }
    B[`navScript${i}`] = copy(rom.slice(a, len + 2));
  }
  const nm = lz(rom, rom.ptr(nv + 0x10));
  B.navMap = nm.data;
  B.navMapPacked = nm.packed;
  B.navNodes = copy(rom.slice(rom.ptr(nv + 0x14), nvHdr[0x18] * NAV_NODE_SIZE));

  n.refs = { tileset: props.pixelDataIdx, palette: props.paletteDataIdx, background: props.backgroundIdx };
  return n;
}

/** gForegroundInfo[idx]: LZ77 tileset for char block 2 + tile animations. */
export function readTilesetNative(rom, idx) {
  const fi = rom.entry('gForegroundInfo', idx);
  const hdr = rom.slice(fi, 0x10);
  const B = {};
  B.header = zeroed(hdr, [0, 4, 8]);
  const ts = lz(rom, u32(hdr, 0));
  B.tiles = ts.data;
  B.tilesPacked = ts.packed;
  const nAnims = u16(hdr, 0x0c);
  const anims = [];
  let gfxEnd = 0;
  for (let i = 0; i < nAnims; i++) {
    const a = u32(hdr, 8) + i * 0x14;
    const rec = rom.slice(a, 0x14);
    const nFrames = u16(rec, 0x10), nChunks = u16(rec, 0x12), copySize = u32(rec, 4), stride = u32(rec, 8);
    const frames = copy(rom.slice(u32(rec, 0x0c), nFrames * 8));
    for (let f = 0; f < nFrames; f++) {
      const end = u32(frames, f * 8) + (nChunks - 1) * stride + copySize;
      if (end > gfxEnd) gfxEnd = end;
    }
    anims.push({ record: zeroed(rec, [0x0c]), frames });
  }
  B.anims = anims;
  B.animGfx = nAnims ? copy(rom.slice(u32(hdr, 4), gfxEnd)) : new Uint8Array(0);
  return { idx, roomId: u16(hdr, 0x0e), blobs: B, animGfxPtr: u32(hdr, 4) };
}

/** gRoomTiledBGs[idx]: shared background layer (BG0): LZ77 tiles, raw palette, raw tilemap. */
export function readBackgroundNative(rom, idx) {
  const a = rom.entry('gRoomTiledBGs', idx);
  const hdr = rom.slice(a, TILED_BG_SIZE);
  const w = u16(hdr, 0), h = u16(hdr, 2);
  const B = {};
  B.header = zeroed(hdr, [0x08, 0x10, 0x18]);
  const ts = lz(rom, u32(hdr, 0x08));
  B.tiles = ts.data;
  B.tilesPacked = ts.packed;
  B.palette = copy(rom.slice(u32(hdr, 0x10), u16(hdr, 0x16) * 2));
  B.tilemap = copy(rom.slice(u32(hdr, 0x18), w * h * 2));
  return { idx, width: w, height: h, blobs: B };
}

/** Compare two natives blob by blob.  -> [{blob, a, b, at}] for every difference. */
export function compareBlobs(a, b, prefix = '') {
  const diffs = [];
  const keys = new Set([...Object.keys(a), ...Object.keys(b)]);
  for (const k of keys) {
    const x = a[k], y = b[k];
    const name = prefix + k;
    if (x instanceof Uint8Array || y instanceof Uint8Array) {
      if (!(x instanceof Uint8Array) || !(y instanceof Uint8Array)) { diffs.push({ blob: name, why: 'missing on one side' }); continue; }
      if (x.length !== y.length) { diffs.push({ blob: name, why: `length ${x.length} != ${y.length}` }); continue; }
      for (let i = 0; i < x.length; i++) if (x[i] !== y[i]) { diffs.push({ blob: name, why: `byte ${i}: 0x${x[i].toString(16)} != 0x${y[i].toString(16)}` }); break; }
    } else if (Array.isArray(x) || Array.isArray(y)) {
      if (!Array.isArray(x) || !Array.isArray(y) || x.length !== y.length) { diffs.push({ blob: name, why: 'array length' }); continue; }
      x.forEach((e, i) => diffs.push(...compareBlobs(e, y[i], `${name}[${i}].`)));
    } else if (x && typeof x === 'object') {
      diffs.push(...compareBlobs(x, y || {}, name + '.'));
    }
  }
  return diffs;
}

export { u8 };
