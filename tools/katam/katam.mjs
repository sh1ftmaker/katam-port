#!/usr/bin/env node
// katam -- the authoring CLI (docs/design/AUTHORING.md).
//
//   katam vocab                                   regenerate assets/schema/authoring/vocab.json + schemas
//   katam decompile --rom R --pack P -o DIR       ROM + extracted pack -> vanilla authoring project
//   katam compile PROJECT [--base DIR]... -o OUT  authoring -> OUT/pack (game-loadable) + OUT/native
//   katam roundtrip --rom R --pack P --work DIR [--bin build/lp64/katam]
//                                                 the gate: decompile, compile, compare with the ROM
//   katam validate PATH [--base DIR]... [--json]  schema + cross-reference checks
//   katam render ROOM -o out.png [--project DIR] [--base DIR]... [--overlay collision,objects,doors,nav] [--scale N]
//   katam new-room ID -o FILE [--width W --height H]   scaffold a valid room to edit
//   katam describe room|collision|objects|music [filter]   reference text for people and models
//   katam reach ROOM...                           (designed, not implemented: see AUTHORING.md)
//   katam playtest ROOM                           (designed, not implemented: see AUTHORING.md)
//
// Node >= 18, no dependencies.  The library under lib/ is browser-safe; only
// this file touches node:fs, node:zlib, node:crypto and child_process.
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { nodeIO } from './lib/io.mjs';
import { setZlib } from './lib/png.mjs';
import { parseRomTables, VANILLA_SHA1 } from './lib/rom.mjs';
import { buildVocab, indexVocab } from './lib/vocab.mjs';
import { loadRegistry } from './lib/registry.mjs';
import { decompileVanilla, loadStack, compileStack, writeOutput, gate } from './lib/pipeline.mjs';
import { readPack } from './lib/pack.mjs';
import { writeJson } from './lib/json.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const R = (p) => path.join(ROOT, p);
setZlib({ deflate: (b) => new Uint8Array(zlib.deflateSync(b, { level: 9 })), inflate: (b) => new Uint8Array(zlib.inflateSync(b)) });
const sha256 = (x) => crypto.createHash('sha256').update(x).digest('hex');

function args(argv) {
  const o = { _: [], base: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith('--')) {
      const k = a.slice(2);
      const flag = ['json', 'quiet', 'strict', 'no-pack-diff', 'check'].includes(k);
      const v = flag ? true : argv[++i];
      if (k === 'base') o.base.push(v); else o[k] = v;
    } else if (a === '-o') o.o = argv[++i];
    else o._.push(a);
  }
  return o;
}

function env() {
  const vocabPath = R('assets/schema/authoring/vocab.json');
  const vocab = indexVocab(fs.existsSync(vocabPath) ? JSON.parse(fs.readFileSync(vocabPath, 'utf8')) : buildVocab((p) => fs.readFileSync(R(p), 'utf8')));
  return { vocab, romTables: parseRomTables(fs.readFileSync(R('game/generated/port/rom_tables.h'), 'utf8')) };
}

const readRom = (p) => {
  const b = new Uint8Array(fs.readFileSync(p));
  const h = crypto.createHash('sha1').update(b).digest('hex');
  if (h !== VANILLA_SHA1) die(`${p}: sha1 ${h} is not the supported ROM (${VANILLA_SHA1}, Kirby & The Amazing Mirror USA)`);
  return b;
};
function die(msg) { console.error(`katam: ${msg}`); process.exit(2); }

const manifest = () => {
  const t = fs.readFileSync(R('assets/manifest.json'));
  return { manifest: JSON.parse(t), manifestSha256: sha256(t) };
};

async function main() {
  const [cmd, ...rest] = process.argv.slice(2);
  const a = args(rest);
  const io = await nodeIO();
  const registry = loadRegistry(io, a.registry || R('assets/schema/semantics'));
  switch (cmd) {
    case 'vocab': {
      // --check: regenerate in memory and compare with the committed files (CI)
      const { writeSchemas } = await import('./lib/schemas.mjs');
      const { memoryIO } = await import('./lib/io.mjs');
      const v = buildVocab((p) => fs.readFileSync(R(p), 'utf8'));
      const dir = R('assets/schema/authoring');
      const target = a.check ? memoryIO() : io;
      target.writeText(`${dir}/vocab.json`, writeJson(v));
      const n = writeSchemas(target, dir, indexVocab(v), registry);
      if (a.check) {
        const stale = [...target.files.keys()].filter((p) => !fs.existsSync(p) || fs.readFileSync(p, 'utf8') !== target.files.get(p));
        if (stale.length) { console.log(`stale (run: node tools/katam/katam.mjs vocab): ${stale.map((p) => path.relative(ROOT, p)).join(', ')}`); return 1; }
        console.log(`vocab.json and ${n} schemas are up to date (registry: ${registry.sources.length} files)`);
        return 0;
      }
      console.log(`wrote vocab.json and ${n} schemas (registry: ${registry.sources.length} files)`);
      return 0;
    }
    case 'decompile': {
      if (!a.rom || !a.pack || !a.o) die('decompile --rom ROM --pack EXTRACTED_PACK -o DIR');
      const { vocab, romTables } = env();
      decompileVanilla({ io, romBytes: readRom(a.rom), romTables, packDir: a.pack, vocab, registry, outDir: a.o, log: console.log });
      return 0;
    }
    case 'compile': {
      if (!a._[0] || !a.o) die('compile PROJECT [--base DIR]... -o OUT');
      const { vocab } = env();
      const layers = loadStack(io, [...a.base, a._[0]]);
      const compiled = compileStack({ io, layers, vocab, registry });
      if (compiled.errors.length) { printErrors(compiled.errors, a.json); return 1; }
      const rep = writeOutput({ io, compiled, outDir: a.o, ...manifest() }, sha256);
      console.log(writeJson({ rooms: compiled.rooms.size, ...rep, allocated: compiled.bindings.allocated }));
      return 0;
    }
    case 'roundtrip': return roundtrip(a, io, registry);
    case 'validate': {
      const { validatePath } = await import('./lib/validate.mjs');
      const { vocab } = env();
      const res = validatePath({ io, target: path.resolve(a._[0] || '.'), bases: a.base.map((b) => path.resolve(b)), vocab, registry, schemaDir: R('assets/schema/authoring') });
      if (a.json) console.log(writeJson(res));
      else {
        for (const e of res.errors) console.log(fmtErr(e, 'error'));
        for (const w of res.warnings) console.log(fmtErr(w, 'warning'));
        console.log(`${res.files} file(s): ${res.errors.length} error(s), ${res.warnings.length} warning(s)`);
      }
      return res.errors.length ? 1 : 0;
    }
    case 'render': {
      const { renderRoom } = await import('./lib/render.mjs');
      const { encodeRGBA } = await import('./lib/png.mjs');
      if (!a._[0] || !a.o) die('render ROOM_ID_OR_FILE -o out.png [--project DIR] [--base DIR]... [--overlay collision,objects,doors,nav] [--scale N]');
      const { vocab } = env();
      const dirs = [...a.base, ...(a.project ? [a.project] : [])];
      let target = a._[0];
      if (fs.existsSync(target) && target.endsWith('.room.json')) {
        const root = projectRootOf(path.resolve(target));
        if (root && !dirs.includes(root)) dirs.push(root);
        target = JSON.parse(fs.readFileSync(target, 'utf8')).id;
      }
      const layers = loadStack(io, dirs.map((d) => path.resolve(d)));
      const img = renderRoom({ io, layers, vocab, registry, roomId: target, overlay: (a.overlay || '').split(',').filter(Boolean), scale: Number(a.scale || 1) });
      io.writeBytes(a.o, encodeRGBA(img.w, img.h, img.rgba));
      console.log(`${a.o}: ${img.w}x${img.h}${img.notes.length ? ' (' + img.notes.join('; ') + ')' : ''}`);
      return 0;
    }
    case 'new-room': {
      const { scaffoldRoom } = await import('./lib/scaffold.mjs');
      if (!a._[0] || !a.o) die('new-room ID -o FILE [--width W] [--height H]');
      io.writeText(a.o, writeJson(scaffoldRoom(a._[0], Number(a.width || 30), Number(a.height || 12))));
      console.log(`wrote ${a.o}`);
      return 0;
    }
    case 'describe': {
      const { describe } = await import('./lib/describe.mjs');
      const { loadSchemas } = await import('./lib/validate.mjs');
      const { vocab } = env();
      console.log(describe(a._[0], a._[1], { vocab, registry, schemas: loadSchemas(io, R('assets/schema/authoring')) }));
      return 0;
    }
    case 'reach':
    case 'playtest': {
      const { plan } = await import('./lib/plans.mjs');
      console.log(plan(cmd));
      return 3;
    }
    default:
      { const L = fs.readFileSync(fileURLToPath(import.meta.url), 'utf8').split('\n'); console.log(L.slice(1, L.findIndex((l, k) => k > 0 && !l.startsWith('//'))).map((l) => l.replace(/^\/\/ ?/, '')).join('\n')); }
      return cmd ? 2 : 0;
  }
}

function projectRootOf(p) {
  let d = path.dirname(p);
  while (d !== path.dirname(d)) { if (fs.existsSync(path.join(d, 'project.json'))) return d; d = path.dirname(d); }
  return null;
}

const fmtErr = (e, lvl) => `${lvl}: ${e.file}${e.where ? ` [${e.where}]` : ''}: ${e.message}${e.hint ? `\n    hint: ${e.hint}` : ''}`;
function printErrors(errs, json) {
  if (json) console.log(writeJson({ errors: errs }));
  else for (const e of errs) console.log(fmtErr(e, 'error'));
}

function roundtrip(a, io, registry) {
  if (!a.rom || !a.pack || !a.work) die('roundtrip --rom ROM --pack EXTRACTED_PACK --work DIR [--bin KATAM]');
  const { vocab, romTables } = env();
  const romBytes = readRom(a.rom);
  const work = path.resolve(a.work);
  fs.rmSync(work, { recursive: true, force: true });
  const proj = `${work}/vanilla`, out = `${work}/out`;
  const t0 = Date.now();
  decompileVanilla({ io, romBytes, romTables, packDir: a.pack, vocab, registry, outDir: proj, log: console.log });
  const t1 = Date.now();
  const layers = loadStack(io, [proj]);
  const compiled = compileStack({ io, layers, vocab, registry });
  if (compiled.errors.length) { printErrors(compiled.errors.slice(0, 20)); console.log(`${compiled.errors.length} compile errors`); return 1; }
  const rep = writeOutput({ io, compiled, outDir: out, ...manifest() }, sha256);
  const t2 = Date.now();
  const g = gate({ romBytes, romTables, compiled, pack: readPack(io, a.pack) });
  let ok = true;
  for (const k of ['rooms', 'tilesets', 'backgrounds', 'palettes', 'objDesc']) {
    const r = g[k];
    console.log(`gate ${k.padEnd(11)} ${String(r.ok).padStart(4)} identical, ${r.bad.length} differ`);
    for (const b of r.bad.slice(0, 5)) console.log(`   ${k} ${b.id}: ${(b.diffs || []).slice(0, 4).map((d) => `${d.blob} ${d.why}`).join('; ')}`);
    if (r.bad.length) ok = false;
  }
  console.log(`gate compressed streams re-encoded byte-identical: ${g.packed.same}/${g.packed.total}`);
  // the compiled pack, file for file against the extractor's
  if (rep.pack && !a['no-pack-diff']) {
    const d = spawnSync('diff', ['-r', a.pack, rep.pack], { encoding: 'utf8' });
    if (d.status === 0) console.log(`gate pack        compiled pack identical to --res-extract output (${fs.readFileSync(a.pack + '/pack.lock', 'utf8').trim().split('\n').length} files)`);
    else { ok = false; console.log('gate pack        compiled pack differs from the extracted pack:\n' + d.stdout.split('\n').slice(0, 12).join('\n')); }
  }
  if (a.bin && rep.pack) {
    const r = spawnSync(a.bin, ['--no-audio', '--res-check', rep.pack, a.rom], {
      encoding: 'utf8', env: { ...process.env, XDG_DATA_HOME: `${work}/xdg/data`, XDG_CONFIG_HOME: `${work}/xdg/config`, SDL_VIDEODRIVER: 'dummy', SDL_AUDIODRIVER: 'dummy' },
    });
    const lines = (r.stdout + r.stderr).split('\n').filter((l) => l.startsWith('[res]'));
    console.log(lines.map((l) => '  ' + l).join('\n'));
    console.log(`gate --res-check ${r.status === 0 ? 'OK: the game compiled the pack to bytes identical to the ROM' : 'FAILED (exit ' + r.status + ')'}`);
    if (r.status !== 0) ok = false;
  }
  console.log(`timing: decompile ${t1 - t0} ms, compile ${t2 - t1} ms`);
  console.log(ok ? 'roundtrip: OK' : 'roundtrip: FAILED');
  return ok ? 0 : 1;
}

main().then((c) => process.exit(c || 0), (e) => {
  if (e && e.where !== undefined) console.error(`katam: ${e.where ? `[${e.where}] ` : ''}${e.message}${e.hint ? `\n    hint: ${e.hint}` : ''}`);
  else console.error(e.stack || e);
  process.exit(1);
});
