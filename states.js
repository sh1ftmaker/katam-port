// Save states, rewind and fast-forward / slow motion: the page's half.
// The engine is platform/states.c; docs/features/states.md says how to use it.
//
//   Shift+F1..F8  save to slot 1..8        F1..F8  load slot 1..8
//   (slot 9 from the menu; F9 / F10 are web/capture.js's)
//   hold `        rewind                   hold Tab  fast-forward
//   hold Shift+Tab  slow motion
//   touch: the small row of buttons that comes with the touch pads
//   menu:  SETTINGS > GAME > SAVE STATES
//
// Slots are kept in this browser (IndexedDB "katam-states"), compressed by
// the engine; the speed and rewind choices in localStorage.  Every action is
// refused by the engine while an online room (or any session) is running,
// and the speed hook below reports 1 then.
(function () {
  'use strict';
  if (typeof window === 'undefined') return;

  var DB = 'katam-states', STORE = 'slots', PREFS = 'katam.states';
  var devMenu = new URLSearchParams(location.search).get('dev') === '1';
  var held = { rewind: false, fast: false, slow: false };
  var have = 0, inited = null;

  function M() { return window.Module; }
  function ready() {
    var m = M();
    // rom-ready (web/shell.html): the module is running the game.
    return !!(m && m._PortStSetPrefs && m.HEAPU8 && document.body &&
              document.body.classList.contains('rom-ready'));
  }
  function allowed() { return ready() && M()._PortStAllowed() === 1; }

  // --- storage -------------------------------------------------------------
  var dbp = null;
  function db() {
    if (dbp) return dbp;
    dbp = new Promise(function (res, rej) {
      var r = indexedDB.open(DB, 1);
      r.onupgradeneeded = function () { r.result.createObjectStore(STORE); };
      r.onsuccess = function () { res(r.result); };
      r.onerror = function () { rej(r.error); };
    });
    return dbp;
  }
  function tx(mode, fn) {
    return db().then(function (d) {
      return new Promise(function (res, rej) {
        var t = d.transaction(STORE, mode), out;
        out = fn(t.objectStore(STORE));
        t.oncomplete = function () { res(out && out.result); };
        t.onerror = function () { rej(t.error); };
      });
    });
  }
  function loadPrefs() {
    var p = { rewind: 1, fast: 3, slow: 2, slot: 1 };
    try {
      var s = JSON.parse(localStorage.getItem(PREFS) || '{}');
      for (var k in p) if (typeof s[k] === 'number') p[k] = s[k];
    } catch (e) { /* defaults */ }
    return p;
  }
  function savePrefs() {
    var m = M();
    try {
      localStorage.setItem(PREFS, JSON.stringify({
        rewind: m._PortStRewindOn(), fast: m._PortStFast(),
        slow: m._PortStSlow(), slot: m._PortStSlot() }));
    } catch (e) { /* not kept */ }
  }

  // --- the engine's callbacks (EM_JS in platform/states.c) -------------------
  function hook(m) {
    m.portStatesNote = note;
    m.portStatesPrefs = savePrefs;
    m.portStatesStore = function (slot, bytes) {
      tx('readwrite', function (s) { return s.put({ data: bytes, t: Date.now() }, slot); })
        .then(function () { have |= 1 << (slot - 1); M()._PortStSetHave(have); })
        .catch(function () { note('COULD NOT KEEP STATE ' + slot); });
    };
    m.portStatesFetch = function (slot) {
      tx('readonly', function (s) { return s.get(slot); }).then(function (v) {
        var mm = M();
        if (!v || !v.data) { note('STATE ' + slot + ' IS EMPTY'); return; }
        var p = mm._malloc(v.data.length);
        if (!p) { note('NOT ENOUGH MEMORY TO LOAD'); return; }
        mm.HEAPU8.set(v.data, p);
        mm._PortStLoadBlob(slot, p, v.data.length);   // owns p now
      }).catch(function () { note('COULD NOT READ STATE ' + slot); });
    };
    m.portStSpeed = function () {
      if (!held.fast && !held.slow) return 1;
      if (!allowed()) return 1;
      if (held.slow) return 1 / M()._PortStSlow();
      return M()._PortStFast();
    };
  }

  function init() {
    var m = M(), p = loadPrefs();
    if (inited === m) return;
    inited = m;
    hook(m);
    m._PortStSetPrefs(1, p.rewind, p.fast, p.slow, p.slot);
    tx('readonly', function (s) { return s.getAllKeys(); }).then(function (keys) {
      have = 0;
      (keys || []).forEach(function (k) { if (k >= 1 && k <= 9) have |= 1 << (k - 1); });
      M()._PortStSetHave(have);
    }).catch(function () { /* no storage: saving says so */ });
  }
  // The module starts once a ROM is in (either module: same exports).
  setInterval(function () {
    if (ready() && inited !== M()) init();
    var r = document.getElementById('stpads');      // no touch row online
    if (r) r.style.display = (ready() && !allowed()) ? 'none' : '';
  }, 400);

  // --- a notice over the picture ---------------------------------------------
  var toast = null, toastT = 0;
  function note(msg) {
    if (!toast) {
      toast = document.createElement('div');
      toast.id = 'stnote';
      toast.setAttribute('aria-live', 'polite');
      toast.style.cssText = 'position:fixed;left:50%;bottom:max(14px,env(safe-area-inset-bottom));' +
        'transform:translateX(-50%);z-index:40;pointer-events:none;padding:5px 12px;border-radius:8px;' +
        'background:rgba(16,18,24,.82);color:#fff;font:600 13px/1.3 system-ui,sans-serif;' +
        'letter-spacing:.06em;transition:opacity .25s;opacity:0';
      document.body.appendChild(toast);
    }
    toast.textContent = msg;
    toast.style.opacity = '1';
    clearTimeout(toastT);
    toastT = setTimeout(function () { toast.style.opacity = '0'; }, 1600);
  }

  // --- keys ------------------------------------------------------------------
  function typing(e) {
    var t = e.target;
    return t && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName));
  }
  function setRewind(on) {
    held.rewind = on;
    if (ready()) M()._PortStSetRewindHeld(on ? 1 : 0);
  }
  window.addEventListener('keydown', function (e) {
    if (!ready() || typing(e) || e.ctrlKey || e.metaKey || e.altKey) return;
    var f = /^F([1-8])$/.exec(e.code);      // F9 / F10: capture.js
    if (f && !(devMenu && e.code === 'F1')) {
      e.preventDefault(); e.stopPropagation();
      if (e.repeat) return;
      if (e.shiftKey) M()._PortStRequestSave(+f[1]);
      else M()._PortStRequestLoad(+f[1]);
      return;
    }
    if (e.code === 'Tab' && !devMenu) {
      e.preventDefault(); e.stopPropagation();
      if (!allowed()) { if (!e.repeat) note('NOT DURING AN ONLINE GAME'); return; }
      held.slow = e.shiftKey; held.fast = !e.shiftKey;
      return;
    }
    if (e.code === 'Backquote') {
      e.preventDefault(); e.stopPropagation();
      if (!allowed()) { if (!e.repeat) note('NOT DURING AN ONLINE GAME'); return; }
      if (!e.repeat && !M()._PortStRewindOn()) note('REWIND IS OFF - SETTINGS > GAME');
      setRewind(true);
    }
  }, true);
  window.addEventListener('keyup', function (e) {
    if (e.code === 'Tab' && !devMenu) { held.fast = held.slow = false; e.stopPropagation(); }
    else if (e.code === 'Backquote') { setRewind(false); e.stopPropagation(); }
    else if (/^F[1-8]$/.test(e.code)) e.stopPropagation();
  }, true);
  window.addEventListener('blur', function () {
    held.fast = held.slow = false;
    setRewind(false);
  });

  // --- touch: a row of four with the touch pads -------------------------------
  function touchRow() {
    var layer = document.getElementById('touch');
    if (!layer || document.getElementById('stpads')) return;
    var css = document.createElement('style');
    css.textContent =
      '#stpads{position:absolute;left:50%;translate:-50% 0;display:flex;gap:8px;pointer-events:none}' +
      '#stpads .stb{pointer-events:auto;touch-action:none;min-width:44px;height:30px;padding:0 10px;' +
      'border-radius:999px;display:flex;align-items:center;justify-content:center;font:600 11px system-ui,sans-serif;' +
      'letter-spacing:.06em;color:#b3bcc9;background:rgba(24,27,33,.78);border:1px solid rgba(255,255,255,.14);' +
      'box-shadow:0 0 0 1px rgba(0,0,0,.38);user-select:none;-webkit-user-select:none}' +
      '#stpads .stb.down{color:#fff;background:rgba(72,128,202,.55)}' +
      '@media (orientation:portrait){#stpads{bottom:calc(var(--row-2, 120px) + var(--sh-h, 30px) + 12px)}}' +
      '@media (orientation:landscape){#stpads{top:calc(var(--safe-t, 0px) + var(--gut, 10px))}}';
    document.head.appendChild(css);
    var row = document.createElement('div');
    row.id = 'stpads';
    [['rw', '◀◀'], ['save', 'SAVE'], ['load', 'LOAD'], ['ff', '▶▶']].forEach(function (b) {
      var el = document.createElement('div');
      el.className = 'stb';
      el.textContent = b[1];
      el.setAttribute('data-st', b[0]);
      function down(e) {
        e.preventDefault(); e.stopPropagation();
        if (!ready()) return;
        try { el.setPointerCapture(e.pointerId); } catch (err) { /* fine */ }
        el.classList.add('down');
        if (!allowed()) { note('NOT DURING AN ONLINE GAME'); return; }
        if (b[0] === 'rw') setRewind(true);
        else if (b[0] === 'ff') held.fast = true;
        else if (b[0] === 'save') M()._PortStRequestSave(M()._PortStSlot());
        else M()._PortStRequestLoad(M()._PortStSlot());
      }
      function up(e) {
        el.classList.remove('down');
        if (b[0] === 'rw') setRewind(false);
        else if (b[0] === 'ff') held.fast = false;
        if (e) e.stopPropagation();
      }
      el.addEventListener('pointerdown', down, { passive: false });
      el.addEventListener('pointerup', up);
      el.addEventListener('pointercancel', up);
      el.addEventListener('lostpointercapture', function () { up(null); });
      row.appendChild(el);
    });
    layer.appendChild(row);
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', touchRow);
  else touchRow();
})();
