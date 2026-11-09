// The level editor's play frame (/editor/play/): the game page (web/shell.html,
// unchanged) plus this script, which `make dist-editor` inserts right before
// the game's loader.  The editor (web/editor/editor.js) shows this page in a
// frame and posts it the compiled rooms:
//
//   editor -> frame   { katamEd: 1, type: 'play', bundle: Uint8Array, room, x, y }
//                     { katamEd: 1, type: 'pause' }              (hidden: stand still)
//   frame  -> editor  { katamPlay: 1, type: 'ready' }            (can take a play)
//                     { katamPlay: 1, type: 'status', text, done }
//
// The first play boots the game into the room (PortResSetRoomBundle +
// PortPlaytestBoot: the regression boot, fast-forwarded, then a warp); later
// ones re-link the rooms in the running game and warp back in.  A playtest
// never reads or writes the player's saved game.
(function () {
  'use strict';
  var WARP_FRAME = 1300, SETTLE = 90;
  if (typeof Module === 'undefined' || !Module.portRomReady || window.parent === window) return;
  var booted = false, first = null, takeFirst = null;
  var firstPlay = new Promise(function (r) { takeFirst = r; });
  function tell(m) { m.katamPlay = 1; window.parent.postMessage(m, location.origin); }
  function say(text, done) { tell({ type: 'status', text: text, done: !!done }); }

  // Back in the editor the frame is hidden, not thrown away, so the next
  // play is a warp and not a boot.  While hidden the game must stand still
  // and stay silent: its frame pacer waits on requestAnimationFrame with a
  // setTimeout guard (platform/web/host_web.c), so both are held here.
  var paused = false, held = [];
  var raf = window.requestAnimationFrame.bind(window), sto = window.setTimeout.bind(window);
  window.requestAnimationFrame = function (cb) { if (paused) { held.push(function () { raf(cb); }); return 0; } return raf(cb); };
  window.setTimeout = function (cb, ms) {
    if (paused && typeof cb === 'function') { var a = Array.prototype.slice.call(arguments, 2); held.push(function () { sto.apply(null, [cb, ms].concat(a)); }); return 0; }
    return sto.apply(null, arguments);
  };
  function setPaused(p) {
    if (p === paused) return;
    paused = p;
    var ctx = Module.portAudio && Module.portAudio.ctx;
    try { if (ctx) { if (p) ctx.suspend(); else ctx.resume(); } } catch (e) { /* no audio yet */ }
    if (!p) { var h = held; held = []; h.forEach(function (f) { f(); }); }
  }

  function writeHeap(bytes) { var p = Module._malloc(bytes.length); Module.HEAPU8.set(bytes, p); return p; }
  function settle(room) {
    var poll = setInterval(function () {
      if (Module._PortFrameNumber() >= WARP_FRAME + SETTLE) { clearInterval(poll); say('Playing room ' + room + '.', true); }
    }, 200);
  }
  function boot(m) {
    var p = writeHeap(m.bundle);
    Module._PortResSetRoomBundle(p, m.bundle.length);
    Module._free(p);
    Module._PortPlaytestBoot(m.room, m.x, m.y);
    Module.portTurbo = function () { return Module._PortFrameNumber() < WARP_FRAME + SETTLE; };
    booted = true;
    say('Skipping the intro…');
    settle(m.room);
  }
  function reload(m) {
    if (Module._PortFrameNumber() < WARP_FRAME + SETTLE) { sto(function () { reload(m); }, 200); return; }
    var p = writeHeap(m.bundle);
    var rc = Module._PortResReloadRooms(p, m.bundle.length);
    Module._free(p);
    if (rc !== 0) { say('The game could not take the changed rooms: press Restart.'); return; }
    Module._PortWarp(0, m.room, m.x, m.y);
    say('Playing room ' + m.room + '.', true);
  }

  window.addEventListener('message', function (e) {
    if (e.origin !== location.origin || !e.data || e.data.katamEd !== 1) return;
    if (e.data.type === 'pause') { setPaused(true); return; }
    if (e.data.type !== 'play') return;
    setPaused(false);
    if (booted) reload(e.data); else if (takeFirst) { takeFirst(e.data); takeFirst = null; } else first = e.data;
  });
  // Chained onto the promise the C side awaits before its first frame: the
  // ROM is mapped by then, and the game waits here for the rooms.
  Module.portRomReady = Module.portRomReady.then(function () {
    Module.portSramRestore = function () { return 0; };
    Module.portSramDirty = function () {};
    return firstPlay.then(function (m) { boot(first || m); });
  }).catch(function (e) { console.error(e); say('The playtest could not start: ' + e.message); });
  function ready() { document.title = 'KATAM playtest'; tell({ type: 'ready' }); }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', ready); else ready();
})();
