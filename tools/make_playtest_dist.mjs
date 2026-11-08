#!/usr/bin/env node
// make_playtest_dist.mjs DIST OUT -- the room playtest page (/next/playtest/)
// from the stamped game page in DIST (make dist).
//
//   OUT/index.html        DIST/index.html + web/playtest/playtest.{js,css}
//                         (one <script> ahead of the game's loader; the shell
//                         itself is not changed)
//   OUT/katam.js, .wasm   the game, as built
//   OUT/katam-<id>/lib/   tools/katam/lib (the authoring compiler), content-
//                         addressed so a new build never meets a cached module
//   OUT/data.json         vocab, semantics registry, schemas, manifest, ROM
//                         table addresses, the committed example project --
//                         all committed text; no game data
//
// Nothing ROM-derived is written: the page decompiles the player's own ROM
// in their browser.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const [dist, out] = process.argv.slice(2).map((p) => path.resolve(p));
if (!dist || !out) { console.error('usage: make_playtest_dist.mjs DIST OUT'); process.exit(2); }
const sha = (b) => crypto.createHash('sha256').update(b).digest('hex').slice(0, 12);
const R = (p) => path.join(ROOT, p);

fs.rmSync(out, { recursive: true, force: true });
fs.mkdirSync(out, { recursive: true });
for (const f of ['katam.js', 'katam.wasm', 'mp_net.js', 'rb_net.js', 'rb_boot.js', 'robots.txt'])
  if (fs.existsSync(`${dist}/${f}`)) fs.copyFileSync(`${dist}/${f}`, `${out}/${f}`);

// the compiler library, content-addressed
const libDir = R('tools/katam/lib');
const libFiles = fs.readdirSync(libDir).filter((f) => f.endsWith('.mjs')).sort();
const libId = sha(Buffer.concat(libFiles.map((f) => fs.readFileSync(`${libDir}/${f}`))));
fs.mkdirSync(`${out}/katam-${libId}/lib`, { recursive: true });
for (const f of libFiles) fs.copyFileSync(`${libDir}/${f}`, `${out}/katam-${libId}/lib/${f}`);

// data.json
const dirJson = (d) => Object.fromEntries(fs.readdirSync(d).filter((f) => f.endsWith('.json')).sort().map((f) => [f, JSON.parse(fs.readFileSync(`${d}/${f}`, 'utf8'))]));
const examples = {};
const ex = R('examples/authoring');
const walk = (d) => { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = `${d}/${e.name}`; if (e.isDirectory()) walk(p); else if (p.endsWith('.json')) examples[path.relative(ex, p)] = fs.readFileSync(p, 'utf8'); } };
walk(`${ex}/sky_garden`);
const data = {
  format: 'katam-playtest-data/1',
  vocab: JSON.parse(fs.readFileSync(R('assets/schema/authoring/vocab.json'), 'utf8')),
  schemas: Object.fromEntries(Object.entries(dirJson(R('assets/schema/authoring'))).filter(([f]) => f.endsWith('.schema.json'))),
  semantics: dirJson(R('assets/schema/semantics')),
  manifest: JSON.parse(fs.readFileSync(R('assets/manifest.json'), 'utf8')),
  romTables: fs.readFileSync(R('game/generated/port/rom_tables.h'), 'utf8'),
  examples,
};
const dataText = JSON.stringify(data);
fs.writeFileSync(`${out}/data.json`, dataText);

// the page scripts, content-addressed by query
for (const f of ['playtest.js', 'playtest.css']) fs.copyFileSync(R(`web/playtest/${f}`), `${out}/${f}`);
const jsId = sha(fs.readFileSync(`${out}/playtest.js`)), cssId = sha(fs.readFileSync(`${out}/playtest.css`));
let html = fs.readFileSync(`${dist}/index.html`, 'utf8');
const shim = html.search(/<script>Module\.locateFile=/);
if (shim < 0) { console.error('make_playtest_dist: no locateFile shim in index.html (run make dist first)'); process.exit(1); }
const tag = `<script>window.KATAM_PT_LIB="katam-${libId}/lib/";window.KATAM_PT_DATA="data.json?v=${sha(dataText)}";</script>`
  + `<script src="playtest.js?v=${jsId}" data-css="${cssId}"></script>`;
html = html.slice(0, shim) + tag + html.slice(shim);
html = html.replace(/<title>[^<]*<\/title>/, '<title>KATAM room playtest</title>');
html = html.replace(/(<meta name="katam-build" content=")([^"]*)"/, '$1$2 playtest"');
fs.writeFileSync(`${out}/index.html`, html);
const label = (/<meta name="katam-build" content="([^"]*)"/.exec(html) || [])[1];
console.log(`  PLAYTEST ${out} (lib katam-${libId}, ${libFiles.length} modules, data.json ${(dataText.length / 1024).toFixed(0)} KiB) -- ${label}`);
