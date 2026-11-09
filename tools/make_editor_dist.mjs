#!/usr/bin/env node
// make_editor_dist.mjs DIST OUT -- the level editor (/editor/) from the
// stamped game page in DIST (make dist).
//
//   OUT/index.html, editor.js, editor.css   the editor (web/editor/)
//   OUT/katam-<id>/lib/   tools/katam/lib (the authoring compiler), content-
//                         addressed so a new build never meets a cached module
//   OUT/data.json         vocab, semantics registry, schemas, manifest, ROM
//                         table addresses, the committed example project --
//                         all committed text; no game data
//   OUT/play/             the play frame: DIST/index.html + web/editor/play.js
//                         (one <script> ahead of the game's loader; the shell
//                         itself is not changed), katam.js, katam.wasm
//
// Nothing ROM-derived is written: the page decompiles the player's own ROM
// in their browser.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const [dist, out] = process.argv.slice(2).map((p) => path.resolve(p));
if (!dist || !out) { console.error('usage: make_editor_dist.mjs DIST OUT'); process.exit(2); }
const sha = (b) => crypto.createHash('sha256').update(b).digest('hex').slice(0, 12);
const R = (p) => path.join(ROOT, p);

fs.rmSync(out, { recursive: true, force: true });
fs.mkdirSync(`${out}/play`, { recursive: true });

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
for (const e of fs.readdirSync(ex, { withFileTypes: true })) if (e.isDirectory()) walk(`${ex}/${e.name}`);   // every example project, keyed <project>/<path>
const dataText = JSON.stringify({
  format: 'katam-editor-data/1',
  vocab: JSON.parse(fs.readFileSync(R('assets/schema/authoring/vocab.json'), 'utf8')),
  schemas: Object.fromEntries(Object.entries(dirJson(R('assets/schema/authoring'))).filter(([f]) => f.endsWith('.schema.json'))),
  semantics: dirJson(R('assets/schema/semantics')),
  autotile: fs.existsSync(R('assets/schema/authoring/autotile')) ? dirJson(R('assets/schema/authoring/autotile')) : {},
  manifest: JSON.parse(fs.readFileSync(R('assets/manifest.json'), 'utf8')),
  romTables: fs.readFileSync(R('game/generated/port/rom_tables.h'), 'utf8'),
  examples,
});
fs.writeFileSync(`${out}/data.json`, dataText);

// the game page, for the play frame
let game = fs.readFileSync(`${dist}/index.html`, 'utf8');
const label = (/<meta name="katam-build" content="([^"]*)"/.exec(game) || [])[1] || 'dev';
for (const f of ['katam.js', 'katam.wasm', 'mp_net.js', 'rb_net.js', 'rb_boot.js', 'net_session.js', 'net_game.js', 'lobby.js', 'robots.txt'])
  if (fs.existsSync(`${dist}/${f}`)) fs.copyFileSync(`${dist}/${f}`, `${out}/play/${f}`);
fs.copyFileSync(R('web/editor/play.js'), `${out}/play/play.js`);
const shim = game.search(/<script>Module\.locateFile=/);
if (shim < 0) { console.error('make_editor_dist: no locateFile shim in index.html (run make dist first)'); process.exit(1); }
game = game.slice(0, shim) + `<script src="play.js?v=${sha(fs.readFileSync(`${out}/play/play.js`))}"></script>` + game.slice(shim);
game = game.replace(/(<meta name="katam-build" content=")([^"]*)"/, '$1$2 editor-play"');
fs.writeFileSync(`${out}/play/index.html`, game);

// the editor page
for (const f of ['editor.js', 'editor.css']) fs.copyFileSync(R(`web/editor/${f}`), `${out}/${f}`);
if (fs.existsSync(`${dist}/robots.txt`)) fs.copyFileSync(`${dist}/robots.txt`, `${out}/robots.txt`);
const html = fs.readFileSync(R('web/editor/index.html'), 'utf8')
  .replaceAll('@BUILD@', label)
  .replaceAll('@LIB@', `katam-${libId}/lib/`)
  .replaceAll('@DATA@', `data.json?v=${sha(dataText)}`)
  .replaceAll('@JS@', sha(fs.readFileSync(`${out}/editor.js`)))
  .replaceAll('@CSS@', sha(fs.readFileSync(`${out}/editor.css`)));
fs.writeFileSync(`${out}/index.html`, html);
console.log(`  EDITOR ${out} (lib katam-${libId}, ${libFiles.length} modules, data.json ${(dataText.length / 1024).toFixed(0)} KiB) -- ${label}`);
