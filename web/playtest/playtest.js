// The room playtest page (no longer published: /editor/ replaces it): the game page (web/shell.html,
// unchanged) plus this script, which `make dist-playtest` inserts right
// before the game's loader.
//
// What it does, all in this browser and never uploaded:
//   - takes the player's ROM the way the game page does (the shell's own
//     picker / drag-drop / remembered copy), checks it is the supported dump;
//   - decompiles the vanilla game from it into memory (tools/katam/lib: the
//     same compiler as the CLI), so references, validation and the preview
//     use the player's own resources;
//   - loads a project: one of the committed examples, or the player's room /
//     project files (JSON, a .zip, pasted or dropped);
//   - validates it (errors with file, field and hint), renders a preview,
//     compiles the room bundle (katam-native/1);
//   - hands the bundle to the game (PortResSetRoomBundle) and boots straight
//     into the room (PortPlaytestBoot: the regression boot, fast-forwarded,
//     then a warp to the chosen arrival);
//   - "Reload room" recompiles and re-links mid-game (PortResReloadRooms)
//     and warps back in: seconds per iteration.
// The playtest never reads or writes the player's saved game (blank,
// unpersisted save memory, as netplay sessions do).
(function () {
  'use strict';
  var WARP_FRAME = 1300, SETTLE = 90;
  var BASE = (document.currentScript && document.currentScript.src) ? document.currentScript.src.replace(/[^/]*$/, '') : './';
  var LIB = BASE + (window.KATAM_PT_LIB || 'katam/lib/');
  var lib = function (m) { return import(LIB + m + '.mjs'); };

  var S = {
    data: null,          // data.json: vocab, registry, schemas, manifest, rom tables, examples
    L: null,             // the loaded library modules
    io: null,            // memoryIO: /schema, /semantics, /vanilla, /project
    vocab: null, registry: null, vanillaLayer: null,
    romBytes: null, started: false, booted: false,
    source: 'example:meadow', room: null, arrival: null, arrivals: [],
    lastBundle: null, lastRoom: null, lastArrival: null,
  };

  // ------------------------------------------------------------------ UI

  var css = document.createElement('link');
  css.rel = 'stylesheet';
  css.href = BASE + 'playtest.css' + (document.currentScript && document.currentScript.dataset.css ? '?v=' + document.currentScript.dataset.css : '');
  document.head.appendChild(css);

  var panel = document.createElement('div');
  panel.id = 'pt';
  panel.innerHTML =
    '<div class="pt-head"><b>Room playtest</b> <span class="pt-dim">build <span id="pt-build">dev</span></span>' +
    ' <a class="pt-dim" href="../">game page</a></div>' +
    '<div id="pt-status" class="pt-status">Load your ROM to start (it stays in this browser).</div>' +
    '<div class="pt-row"><label class="pt-l" for="pt-source">Project</label>' +
    '<select id="pt-source"><option value="example:meadow">Example: Sky Garden — meadow (room 983)</option>' +
    '<option value="example:tower">Example: Sky Garden — tower (room 984)</option>' +
    '<option value="user" disabled>Your files (load, paste or drop below)</option></select></div>' +
    '<div class="pt-row"><label class="pt-l" for="pt-room">Room</label><select id="pt-room"></select>' +
    '<label class="pt-l" for="pt-arrive">Arrive</label><select id="pt-arrive"></select>' +
    '<span id="pt-xy" hidden><input id="pt-x" type="number" min="0" max="255" value="3" aria-label="cell x">' +
    '<input id="pt-y" type="number" min="0" max="255" value="5" aria-label="cell y"></span></div>' +
    '<div class="pt-row pt-btns"><button id="pt-play" type="button" class="pt-primary">Play room</button>' +
    '<button id="pt-reload" type="button" disabled title="Recompile and warp back in, without rebooting">Reload room</button>' +
    '<button id="pt-restart" type="button" title="Reboot the game into the room">Restart</button>' +
    '<label class="pt-file">Load files…<input id="pt-files" type="file" multiple accept=".json,.zip,application/json,application/zip"></label>' +
    '<button id="pt-pastebtn" type="button">Paste room JSON</button></div>' +
    '<div id="pt-drop" class="pt-drop">Drop room / project files (.room.json, project.json, or a .zip) here</div>' +
    '<div id="pt-paste" hidden><textarea id="pt-text" rows="8" spellcheck="false" placeholder="{ &quot;format&quot;: &quot;katam-room/1&quot;, ... }"></textarea>' +
    '<button id="pt-pasteok" type="button">Use this room</button></div>' +
    '<div id="pt-errors"></div>' +
    '<div class="pt-preview"><div class="pt-row"><span class="pt-l">Preview</span>' +
    '<label><input type="checkbox" class="pt-ov" value="collision"> collision</label>' +
    '<label><input type="checkbox" class="pt-ov" value="objects" checked> objects</label>' +
    '<label><input type="checkbox" class="pt-ov" value="doors" checked> doors</label>' +
    '<label><input type="checkbox" class="pt-ov" value="grid"> grid</label></div>' +
    '<canvas id="pt-canvas" width="240" height="160"></canvas></div>' +
    '<details class="pt-help"><summary>How this works</summary><p>' +
    'Your ROM never leaves this browser. The page decompiles the game from it in memory, compiles the room ' +
    'you pick with the same compiler as <code>katam compile</code>, and boots the game straight into it ' +
    '(the intro is fast-forwarded). New rooms are numbered after the original game\'s ids: 983 and up. ' +
    '<b>Reload room</b> recompiles your files and warps you back in without a reboot; <b>Restart</b> reboots. ' +
    'Arrive picks a door that leads into the room (or a cell: Kirby is placed at x&#183;16&#8722;8, y&#183;16&#8722;7 px). ' +
    'Playtests run on a blank save that is never stored: your saved game is not touched. ' +
    'The settings menu (&#9881;, F1) works as on the game page. Format: ' +
    '<a href="https://github.com/sh1ftmaker/katam-port/blob/main/docs/design/authoring/rooms.md">rooms.md</a>.</p></details>';

  function $(id) { return document.getElementById(id); }
  // A drawer of its own over the left of the picture: the game page has no
  // panel to sit in (the picture has the window; the game's own menu has
  // the rest).  Collapses to a tab, so the room can be played full size.
  var tab = document.createElement('button');
  tab.id = 'pt-tab';
  tab.type = 'button';
  function setOpen(open) {
    document.body.classList.toggle('pt-closed', !open);
    tab.textContent = open ? '\u25c0 Hide' : 'Playtest \u25b6';
    tab.setAttribute('aria-expanded', open ? 'true' : 'false');
  }
  tab.addEventListener('click', function () { setOpen(document.body.classList.contains('pt-closed')); });

  function mount() {
    document.body.appendChild(panel);
    document.body.appendChild(tab);
    setOpen(true);
    var bl = document.getElementById('buildlabel');
    if (bl) $('pt-build').textContent = bl.textContent;
    document.title = 'KATAM room playtest';
    var cur = document.querySelector('#curtain h2');
    if (cur) cur.insertAdjacentHTML('afterend', '<p><b>Room playtest:</b> after your ROM loads, the game boots straight into the room picked below.</p>');
    wire();
  }

  function status(t, cls) { var e = $('pt-status'); e.textContent = t; e.className = 'pt-status' + (cls ? ' pt-' + cls : ''); }
  function esc(s) { return String(s).replace(/[&<>"]/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]; }); }

  function showFindings(res) {
    var box = $('pt-errors');
    var rows = [];
    (res.errors || []).forEach(function (e) { rows.push(['error', e]); });
    (res.warnings || []).forEach(function (e) { rows.push(['warning', e]); });
    if (!rows.length) { box.innerHTML = ''; return; }
    box.innerHTML = '<div class="pt-sum">' + (res.errors || []).length + ' error(s), ' + (res.warnings || []).length + ' warning(s)</div>' +
      rows.map(function (r) {
        var e = r[1];
        return '<div class="pt-f pt-' + r[0] + '"><span class="pt-sev">' + r[0] + '</span> <code>' + esc(e.file || '') + '</code>' +
          (e.where ? ' <code class="pt-where">' + esc(e.where) + '</code>' : '') + (e.code ? ' <span class="pt-dim">[' + esc(e.code) + ']</span>' : '') +
          '<div>' + esc(e.message) + '</div>' + (e.hint ? '<div class="pt-hint">hint: ' + esc(e.hint) + '</div>' : '') + '</div>';
      }).join('');
  }

  // ---------------------------------------------------------- data + library

  var dataReady = fetch(BASE + (window.KATAM_PT_DATA || 'data.json')).then(function (r) {
    if (!r.ok) throw new Error('data.json: ' + r.status);
    return r.json();
  }).then(function (d) { S.data = d; return d; });

  function loadLibrary() {
    if (S.L) return Promise.resolve(S.L);
    return Promise.all(['io', 'png', 'zlib', 'rom', 'vocab', 'registry', 'pipeline', 'project', 'validate', 'render', 'native', 'rompack', 'playtest'].map(lib))
      .then(function (m) {
        var L = { io: m[0], png: m[1], zlib: m[2], rom: m[3], vocab: m[4], registry: m[5], pipeline: m[6], project: m[7], validate: m[8], render: m[9], native: m[10], rompack: m[11], playtest: m[12] };
        L.png.setZlib({ deflate: L.zlib.deflate, inflate: L.zlib.inflate });
        S.L = L;
        S.io = L.io.memoryIO();
        var d = S.data;
        Object.keys(d.schemas).forEach(function (n) { S.io.writeText('/schema/' + n, JSON.stringify(d.schemas[n])); });
        Object.keys(d.semantics).forEach(function (n) { S.io.writeText('/semantics/' + n, JSON.stringify(d.semantics[n])); });
        S.vocab = L.vocab.indexVocab(d.vocab);
        S.registry = L.registry.loadRegistry(S.io, '/semantics');
        return L;
      });
  }

  function sha1(bytes) {
    return crypto.subtle.digest('SHA-1', bytes).then(function (h) {
      return Array.from(new Uint8Array(h), function (b) { return b.toString(16).padStart(2, '0'); }).join('');
    });
  }

  // The player's game, decompiled into /vanilla (once per page).
  function buildVanilla() {
    if (S.vanillaLayer) return Promise.resolve();
    var L = S.L, d = S.data;
    status('Decompiling the game from your ROM (in this browser)…');
    return new Promise(function (r) { setTimeout(r, 30); }).then(function () {
      var t0 = performance.now();
      var romTables = L.rom.parseRomTables(d.romTables);
      var pack = L.rompack.packFromRom(S.romBytes, romTables, d.manifest);
      L.pipeline.decompileVanilla({ io: S.io, romBytes: S.romBytes, romTables: romTables, pack: pack, vocab: S.vocab, registry: S.registry, outDir: '/vanilla' });
      S.vanillaLayer = L.project.loadProject(S.io, '/vanilla');
      console.log('[playtest] vanilla project decompiled in ' + Math.round(performance.now() - t0) + ' ms');
    });
  }

  // ------------------------------------------------------------- projects

  function clearProject() { S.io.list('/project').forEach(function (p) { S.io.files.delete(p); }); }

  function putExample() {
    clearProject();
    var ex = S.data.examples;
    Object.keys(ex).forEach(function (p) { S.io.writeText('/project/' + p.replace(/^sky_garden\//, ''), ex[p]); });
  }

  // Files from the player: a zip, project.json, room files (placed by id).
  function putFiles(files) {
    var L = S.L;
    clearProject();
    var rooms = 0, other = 0, hasProject = false;
    files.forEach(function (f) {
      var name = f.name.replace(/\\/g, '/');
      if (/(^|\/)project\.json$/.test(name)) { S.io.writeText('/project/project.json', f.text); hasProject = true; return; }
      var doc = null;
      try { doc = JSON.parse(f.text); } catch (e) { S.io.writeText('/project/rooms/unparsed/' + name.split('/').pop().replace(/\.json$/, '') + '.room.json', f.text); rooms++; return; }
      if (doc && doc.id && L.project.parseId(doc.id)) { S.io.writeText('/project/' + L.project.pathOf(doc.id), f.text); rooms++; }
      else { S.io.writeText('/project/rooms/misc/' + (name.split('/').pop().replace(/\.(room\.)?json$/, '') || 'room') + '.room.json', f.text); other++; }
    });
    if (!hasProject) S.io.writeText('/project/project.json', JSON.stringify({ format: 'katam-project/1', id: 'playtest', name: 'Playtest', base: 'vanilla' }, null, 2));
    return rooms + other;
  }

  function readZip(buf) {
    // central directory -> {name, text}; stored or deflate entries
    var b = new Uint8Array(buf), dv = new DataView(buf), out = [];
    var e = b.length - 22;
    while (e >= 0 && dv.getUint32(e, true) !== 0x06054b50) e--;
    if (e < 0) throw new Error('not a zip file');
    var n = dv.getUint16(e + 10, true), p = dv.getUint32(e + 16, true);
    for (var i = 0; i < n; i++) {
      var method = dv.getUint16(p + 10, true), csize = dv.getUint32(p + 20, true);
      var nlen = dv.getUint16(p + 28, true), xlen = dv.getUint16(p + 30, true), clen = dv.getUint16(p + 32, true);
      var local = dv.getUint32(p + 42, true);
      var name = new TextDecoder().decode(b.subarray(p + 46, p + 46 + nlen));
      p += 46 + nlen + xlen + clen;
      if (name.endsWith('/') || !/\.json$/i.test(name)) continue;
      var start = local + 30 + dv.getUint16(local + 26, true) + dv.getUint16(local + 28, true);
      var raw = b.subarray(start, start + csize), data;
      if (method === 0) data = raw;
      else if (method === 8) data = S.L.zlib.inflate(concat([Uint8Array.of(0x78, 0x01), raw]));
      else throw new Error(name + ': compression method ' + method + ' (use stored or deflate)');
      out.push({ name: name, text: new TextDecoder().decode(data) });
    }
    return out;
  }
  function concat(parts) { var n = 0, o = 0; parts.forEach(function (x) { n += x.length; }); var r = new Uint8Array(n); parts.forEach(function (x) { r.set(x, o); o += x.length; }); return r; }

  function takeFiles(list) {
    return loadLibrary().then(function () {
      return Promise.all(Array.from(list).map(function (f) {
        return /\.zip$/i.test(f.name)
          ? f.arrayBuffer().then(readZip)
          : f.text().then(function (t) { return [{ name: f.webkitRelativePath || f.name, text: t }]; });
      }));
    }).then(function (groups) {
      var files = [].concat.apply([], groups);
      if (!files.length) { status('No JSON files in what was loaded.', 'err'); return; }
      putFiles(files);
      useUserSource();
    }).catch(function (e) { status('Could not read the files: ' + e.message, 'err'); });
  }

  function useUserSource() {
    var o = $('pt-source').querySelector('option[value=user]');
    o.disabled = false;
    $('pt-source').value = 'user';
    S.source = 'user';
    try { sessionStorage.setItem('katam-pt-files', JSON.stringify(S.io.list('/project').map(function (p) { return [p, S.io.readText(p)]; }))); } catch (e) { /* too big: restart loses them */ }
    S.room = null;
    refresh();
  }

  // ----------------------------------------------------- compile / validate

  function stack() { return [S.vanillaLayer, S.L.project.loadProject(S.io, '/project')]; }

  function compileNow() {
    var L = S.L;
    var res = L.validate.validatePath({ io: S.io, target: '/project', bases: ['/vanilla'], vocab: S.vocab, registry: S.registry, schemaDir: '/schema' });
    var compiled = L.pipeline.compileStack({ io: S.io, layers: stack(), vocab: S.vocab, registry: S.registry });
    compiled.errors.forEach(function (e) { res.errors.push({ severity: 'error', code: 'compile', file: e.file, where: e.where, message: e.message, hint: e.hint }); });
    showFindings(res);
    var b = L.pipeline.bundleOf(compiled);
    return { res: res, compiled: compiled, bundle: b };
  }

  function fillRooms(c) {
    var sel = $('pt-room'), keep = S.room;
    var rooms = c.bundle.rooms.map(function (r) { return [r.id, r.roomId]; });
    sel.innerHTML = rooms.map(function (r) { return '<option value="' + r[1] + '">' + esc(r[0]) + ' (' + r[1] + ')</option>'; }).join('');
    if (!rooms.length) return;
    if (keep === null || !rooms.some(function (r) { return r[1] === keep; })) {
      var want = S.source === 'example:tower' ? 'room:sky_garden/tower' : S.source === 'example:meadow' ? 'room:sky_garden/meadow' : null;
      var w = rooms.filter(function (r) { return r[0] === want; })[0];
      keep = (w || rooms[0])[1];
    }
    sel.value = String(keep);
    S.room = keep;
    fillArrivals(c);
  }

  function fillArrivals(c) {
    var L = S.L, b = c.compiled.bindings, sel = $('pt-arrive'), opts = [];
    b.docs.forEach(function (d, id) {
      if (!/^room:/.test(id)) return;
      (d.doc.doors || []).forEach(function (dr, i) {
        var to = b.resolve(dr.to), n = to ? b.num.get(to) : Number((/(\d+)$/.exec(dr.to || '') || [])[1]);
        if (n === S.room && dr.arrive) opts.push({ x: dr.arrive.x, y: dr.arrive.y, label: 'door ' + i + ' of ' + id + ' → (' + dr.arrive.x + ', ' + dr.arrive.y + ')' });
      });
    });
    if (!opts.length) { var a = L.playtest.chooseArrival(c.compiled, S.room); opts.push({ x: a.x, y: a.y, label: a.how + ' (' + a.x + ', ' + a.y + ')' }); }
    S.arrivals = opts;
    sel.innerHTML = opts.map(function (o, i) { return '<option value="' + i + '">' + esc(o.label) + '</option>'; }).join('') + '<option value="xy">cell x, y…</option>';
    var prev = S.arrival && opts.findIndex(function (o) { return o.x === S.arrival.x && o.y === S.arrival.y; });
    sel.value = prev >= 0 ? String(prev) : '0';
    pickArrival();
  }

  function pickArrival() {
    var v = $('pt-arrive').value;
    $('pt-xy').hidden = v !== 'xy';
    S.arrival = v === 'xy' ? { x: Number($('pt-x').value) | 0, y: Number($('pt-y').value) | 0 } : S.arrivals[Number(v)] || S.arrivals[0];
  }

  function preview(c) {
    var L = S.L, cv = $('pt-canvas');
    var ov = Array.from(document.querySelectorAll('.pt-ov')).filter(function (e) { return e.checked; }).map(function (e) { return e.value; });
    var id = c.compiled.bindings.byNum.room.get(S.room);
    if (!id) return;
    try {
      var img = L.render.renderRoom({ io: S.io, layers: stack(), vocab: S.vocab, registry: S.registry, roomId: id, overlay: ov, scale: 1 });
      cv.width = img.w; cv.height = img.h;
      cv.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(img.rgba.buffer, img.rgba.byteOffset, img.rgba.length), img.w, img.h), 0, 0);
    } catch (e) { console.warn('[playtest] preview:', e); }
  }

  var last = null;
  function refresh() {
    if (!S.romBytes || !S.vanillaLayer) return null;
    if (S.source !== 'user') putExample();
    var t0 = performance.now();
    var c = compileNow();
    last = c;
    fillRooms(c);
    preview(c);
    var ms = Math.round(performance.now() - t0);
    if (c.res.errors.length) status('The project has errors: fix them and press Play / Reload. (' + ms + ' ms)', 'err');
    else if (!c.bundle.rooms.length) status('The project defines no rooms to play.', 'err');
    else status((S.booted ? 'Playing room ' : 'Ready: room ') + S.room + ' (' + c.bundle.bytes.length + '-byte bundle, compiled in ' + ms + ' ms).', 'ok');
    $('pt-reload').disabled = !S.booted;
    return c;
  }

  function writeHeap(bytes) {
    var p = Module._malloc(bytes.length);
    Module.HEAPU8.set(bytes, p);
    return p;
  }

  // ------------------------------------------------------------ the boot

  var resolvePlay = null;
  // Chained onto the promise the C side awaits before its first frame (the
  // netplay world mode does the same): the ROM is mapped by then.
  function beforeStart() {
    S.started = true;
    // A playtest never touches the player's saved game.
    Module.portSramRestore = function () { return 0; };
    Module.portSramDirty = function () {};
    var size = (window.shell && shell.rom && shell.rom.size) || 16 * 1024 * 1024;
    S.romBytes = Module.HEAPU8.slice(0x08000000, 0x08000000 + size);
    return Promise.all([dataReady, loadLibrary()]).then(function () { return sha1(S.romBytes); }).then(function (h) {
      if (h !== S.L.rom.VANILLA_SHA1) {
        status('This ROM (sha1 ' + h.slice(0, 12) + '…) is not the supported dump (Kirby & The Amazing Mirror, USA): the playtest needs it. The game runs without a room.', 'err');
        return;
      }
      return buildVanilla().then(function () {
        var c = refresh();
        restoreSelection();
        c = refresh();
        if (c && !c.res.errors.length && c.bundle.rooms.length) return boot(c);
        status('Fix the errors, then press Play room.', 'err');
        return new Promise(function (r) { resolvePlay = r; });
      });
    }).catch(function (e) {
      console.error(e);
      status('Playtest failed to start: ' + e.message + ' — the game runs without a room.', 'err');
    });
  }

  function boot(c) {
    var p = writeHeap(c.bundle.bytes);
    Module._PortResSetRoomBundle(p, c.bundle.bytes.length);
    Module._free(p);
    Module._PortPlaytestBoot(S.room, S.arrival.x, S.arrival.y);
    Module.portTurbo = function () { return Module._PortFrameNumber() < WARP_FRAME + SETTLE; };
    S.booted = true;
    S.lastRoom = S.room;
    status('Booting into room ' + S.room + ' (intro fast-forwarded)…', 'ok');
    $('pt-reload').disabled = false;
    var poll = setInterval(function () {
      if (Module._PortFrameNumber() >= WARP_FRAME + SETTLE) { clearInterval(poll); status('Playing room ' + S.room + '. Edit your files and press Reload room.', 'ok'); }
    }, 250);
  }

  function reloadRoom() {
    if (!S.booted) return;
    var c = refresh();
    if (!c || c.res.errors.length) return;
    var p = writeHeap(c.bundle.bytes);
    var rc = Module._PortResReloadRooms(p, c.bundle.bytes.length);
    Module._free(p);
    if (rc !== 0) { status('The game refused the reloaded bundle (see the log); Restart instead.', 'err'); return; }
    Module._PortWarp(0, S.room, S.arrival.x, S.arrival.y);
    status('Reloaded: warping into room ' + S.room + '.', 'ok');
  }

  function saveSelection() {
    try { sessionStorage.setItem('katam-pt-sel', JSON.stringify({ source: S.source, room: S.room, arrival: S.arrival })); } catch (e) { /* ignore */ }
  }
  function restoreSelection() {
    try {
      var s = JSON.parse(sessionStorage.getItem('katam-pt-sel') || 'null');
      if (!s) return;
      if (s.source === 'user') {
        var files = JSON.parse(sessionStorage.getItem('katam-pt-files') || 'null');
        if (!files) return;
        clearProject();
        files.forEach(function (f) { S.io.writeText(f[0], f[1]); });
        $('pt-source').querySelector('option[value=user]').disabled = false;
      }
      S.source = s.source; $('pt-source').value = s.source;
      S.room = s.room; S.arrival = s.arrival;
    } catch (e) { /* ignore */ }
  }

  function wire() {
    $('pt-source').addEventListener('change', function () { S.source = this.value; S.room = null; S.arrival = null; refresh(); });
    $('pt-room').addEventListener('change', function () { S.room = Number(this.value); S.arrival = null; if (last) { fillArrivals(last); preview(last); } });
    $('pt-arrive').addEventListener('change', pickArrival);
    $('pt-x').addEventListener('change', pickArrival);
    $('pt-y').addEventListener('change', pickArrival);
    Array.from(document.querySelectorAll('.pt-ov')).forEach(function (e) { e.addEventListener('change', function () { if (last) preview(last); }); });
    $('pt-play').addEventListener('click', function () {
      saveSelection();
      if (!S.started) { status('Load your ROM first (the button on the game screen, or drop the .gba file on the page).'); return; }
      if (resolvePlay) {
        var c = refresh();
        if (c && !c.res.errors.length && c.bundle.rooms.length) { var r = resolvePlay; resolvePlay = null; boot(c); r(); }
        return;
      }
      if (S.booted) { if (S.room === S.lastRoom) reloadRoom(); else location.reload(); }
    });
    $('pt-reload').addEventListener('click', reloadRoom);
    $('pt-restart').addEventListener('click', function () { saveSelection(); location.reload(); });
    $('pt-files').addEventListener('change', function () { if (this.files.length) takeFiles(this.files); this.value = ''; });
    $('pt-pastebtn').addEventListener('click', function () { $('pt-paste').hidden = !$('pt-paste').hidden; });
    $('pt-pasteok').addEventListener('click', function () {
      var t = $('pt-text').value;
      loadLibrary().then(function () { putFiles([{ name: 'pasted.room.json', text: t }]); useUserSource(); });
    });
    var drop = $('pt-drop');
    ['dragenter', 'dragover'].forEach(function (ev) { drop.addEventListener(ev, function (e) { e.preventDefault(); e.stopPropagation(); drop.classList.add('pt-over'); }); });
    drop.addEventListener('dragleave', function () { drop.classList.remove('pt-over'); });
    drop.addEventListener('drop', function (e) {
      e.preventDefault(); e.stopPropagation(); drop.classList.remove('pt-over');
      document.body.classList.remove('dragging');
      if (e.dataTransfer.files.length) takeFiles(e.dataTransfer.files);
    });
    // The page-wide drop is the shell's ROM loader; once a ROM is in, room
    // files dropped anywhere come here instead.
    window.addEventListener('drop', function (e) {
      if (!e.dataTransfer || !e.dataTransfer.files.length) return;
      var f = Array.from(e.dataTransfer.files);
      if (f.every(function (x) { return /\.(json|zip)$/i.test(x.name); })) { e.stopImmediatePropagation(); e.preventDefault(); document.body.classList.remove('dragging'); takeFiles(f); }
    }, true);
  }

  if (typeof Module === 'undefined' || !Module.portRomReady) {
    console.error('[playtest] the game page did not set up Module.portRomReady');
    return;
  }
  Module.portRomReady = Module.portRomReady.then(beforeStart);
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', mount); else mount();
})();
