// tools/saves_test.mjs -- the save slots, import backups and offline worker
// (docs/features/saves.md), in node, without a browser or a ROM.  < 1 s.
//
//   node tools/saves_test.mjs
import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let fails = 0;
const ok = (c, msg) => { if (!c) { fails++; console.log('FAIL ' + msg); } };

// --- web/sw.js: what it may touch ------------------------------------------
{
  const self = { location: { href: 'https://h.example/katam-port/sw.js' }, addEventListener() {} };
  const ctx = { self, module: { exports: {} }, URL, console };
  vm.runInNewContext(fs.readFileSync(path.join(ROOT, 'web/sw.js'), 'utf8'), ctx);
  const { ownName, namedAssets } = ctx.module.exports;
  const N = (u) => ownName(new URL(u));
  const B = 'https://h.example/katam-port/';
  ok(N(B) === '', 'the page itself');
  ok(N(B + 'index.html?dev=1') === 'index.html', 'the page with a query');
  ok(N(B + 'katam.wasm?v=abc123') === 'katam.wasm', 'a versioned wasm');
  ok(N(B + 'manifest.webmanifest') === 'manifest.webmanifest', 'the manifest');
  for (const rom of ['rom.gba', 'baserom.gba', 'game.bin', 'x.sav', 'kirby.zip', 'rom', 'rom.GBA'])
    ok(N(B + rom) === null, 'a ROM-like file is never ours: ' + rom);
  ok(N('https://h.example/rom.gba') === null, 'outside the directory');
  ok(N('https://other.example/katam-port/katam.js') === null, 'another origin');
  ok(N(B + 'editor/') === null && N(B + 'next/katam.js') === null, 'a subdirectory page');
  ok(N(B + 'sw.js') === null, 'the worker itself');
  const html = '<script src="katam.js?v=0123456789ab"></script><script src=mp_net.js?v=aaaaaaaaaaaa>' +
    '<script>Module.locateFile=function(p){return p==="katam.wasm"?p+"?v=bbbbbbbbbbbb":' +
    'p==="katam16.wasm"?p+"?v=cccccccccccc":p;};</script>';
  const names = namedAssets(html);
  ok(names.includes('katam.js?v=0123456789ab') && names.includes('mp_net.js?v=aaaaaaaaaaaa') &&
     names.includes('katam.wasm?v=bbbbbbbbbbbb'), 'precache list: ' + names.join(' '));
  ok(!names.some((n) => n.startsWith('katam16')), 'the 16-Kirby module is not precached');
  ok(/'__KATAM_SW_VERSION__'/.test(fs.readFileSync(path.join(ROOT, 'web/sw.js'), 'utf8')),
     'the version placeholder stamp_build.py replaces');
}

// --- web/saves.js: slots and backups against an in-memory store -----------
{
  const db = new Map();
  const notes = [];
  let live = new Uint8Array(16).fill(7);
  let imported = null;
  const shell = {
    note: (t, k) => notes.push([k, t]), log() {},
    idb: {
      saveStore: 'save',
      get: async (s, k) => db.get(k),
      put: async (s, k, r) => { db.set(k, structuredClone(r)); },
      del: async (s, k) => { db.delete(k); },
      list: async (s, prefix) => [...db.entries()].filter(([k]) => k.startsWith(prefix)).map(([, v]) => structuredClone(v)),
    },
  };
  shell.save = {
    key: () => 'AGKE-0-1000000-ff', label: () => 'KIRBY [AGKE]', base: () => 'katam',
    current: () => new Uint8Array(live),
    // what web/shell.html's applyImport does: back up, then overwrite
    load: (bytes, name) => shell.saveBackup(new Uint8Array(live), name).then(() => { live = bytes; imported = name; }),
    download() {},
  };
  const Module = { _shell: shell };
  const ctx = { window: { Module }, Module, document: { getElementById: () => null }, console, setTimeout };
  vm.runInNewContext(fs.readFileSync(path.join(ROOT, 'web/saves.js'), 'utf8'), ctx);
  const S = shell.saveSlots;
  ok(!!S, 'saves.js installs shell.saveSlots');

  await S.saveTo('  first / run  ');
  let slots = await S.list('slot');
  ok(slots.length === 1 && slots[0].name === 'first run', 'slot saved with a cleaned name: ' + JSON.stringify(slots[0]?.name));
  ok(slots[0].bytes[0] === 7, 'slot holds the live bytes');

  // Six imports of different saves: five backups kept, newest first.
  for (let i = 1; i <= 6; i++) {
    await new Promise((r) => setTimeout(r, 2));
    await shell.save.load(new Uint8Array(16).fill(10 + i), 'file' + i);
  }
  let backs = await S.list('backup');
  ok(backs.length === 5, 'five backups kept, not ' + backs.length);
  ok(backs[0].bytes[0] === 15 && backs[0].reason === 'before file6', 'newest backup first: ' + backs[0]?.bytes[0]);
  ok(live[0] === 16 && imported === 'file6', 'the import itself happened');

  // A blank cartridge is not worth a backup; an identical one is not kept twice.
  live = new Uint8Array(16);
  await shell.save.load(new Uint8Array(16).fill(1), 'x');
  ok((await S.list('backup'))[0].bytes[0] === 15, 'a blank save is not backed up');
  await shell.save.load(new Uint8Array(16).fill(1), 'y');
  await shell.save.load(new Uint8Array(16).fill(2), 'z');
  backs = await S.list('backup');
  ok(backs.length === 5 && backs[0].bytes[0] === 1 && backs[1].bytes[0] === 15, 'capped, and an identical save is not kept twice: ' + backs.map((b) => b.bytes[0]));

  // Loading a slot is an import (and so is backed up).
  slots = await S.list('slot');
  S.load(slots[0], 'slot');
  await new Promise((r) => setTimeout(r, 10));
  ok(live[0] === 7, 'slot loaded into save memory');
  ok((await S.list('backup'))[0].bytes[0] === 2, 'the save the slot replaced was backed up');

  // Records are per ROM, and slots never collide with the live save's key.
  ok(db.has('AGKE-0-1000000-ff') === false && [...db.keys()].every((k) => /^(slot|backup)\//.test(k)),
     'only prefixed keys written');
  let threw = false;
  try { await S.saveTo('   '); } catch (e) { threw = true; }
  ok(threw, 'an empty slot name is refused');
  await S.del(slots[0]);
  ok((await S.list('slot')).length === 0, 'slot deleted');
}

console.log(fails ? `saves_test: ${fails} failed` : 'saves_test: ok');
process.exit(fails ? 1 : 0);
