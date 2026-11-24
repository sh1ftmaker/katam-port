/* input_remap.js -- SETTINGS > PLAYERS > CONTROLS: the buttons, rebound.
 *
 * Which key or pad button is which GBA button, per device, saved in this
 * browser (localStorage "katam.remap"), plus two of the page's own actions
 * (FULLSCREEN, MUTE) and gamepad rumble.  docs/features/remap.md.
 *
 *   - A profile is { ACTION: [input, ...] } for every action in ACTIONS.
 *     Keyboard inputs are KeyboardEvent.code; pad inputs are 'b<n>' (a
 *     button), 'a<n>+' / 'a<n>-' (an axis past half way) and 'dU' 'dD' 'dL'
 *     'dR' (the d-pad as web/input_devices.js reads it: d-pad buttons, left
 *     stick, a hat).
 *   - Keyboard set A (KEYBOARD) and set B (KEYS 2) have a profile each;
 *     every gamepad has its own, keyed by the browser's id for it, so two
 *     different pads keep different layouts.  A device with no saved
 *     profile uses its defaults: the keyboard's are input_devices.js's
 *     KEYS_A / KEYS_B; a pad's depend on its family (Xbox, PlayStation,
 *     Switch Pro, other) -- the Switch Pro's GBA A is its A, GBA B its B
 *     (Nintendo's layout); the others keep bottom/right = A, left/top = B.
 *     The defaults give exactly input_devices.js's padMask.
 *   - Press to bind: bind(action) waits for the next new press on the
 *     selected device (Escape or CAPTURE_FRAMES cancels) and makes it the
 *     action's only input.  Until every key and button is let go after
 *     that, nothing reaches the game (the press must not also be a menu
 *     press).  An input on two actions is a conflict: shown, not refused.
 *   - Rumble (gamepad vibrationActuator / hapticActuators): a strong pulse
 *     when a player's Kirby loses health, a light one when it gets an
 *     ability; on by default, SETTINGS > PLAYERS > CONTROLS > RUMBLE.
 *
 * Only which inputs make a GBA key mask changes: the masks reach the game
 * as before (recorded, sent online as before), so nothing here can desync
 * or cheat.  The menu's rows are dimmed during an online game (they are
 * drawn into game memory; platform/controls_ui.c).
 *
 * No DOM or wasm dependency beyond what is passed in; node tests:
 * tools/input_remap_test.mjs.  Classic <script> (window.KatamRemap) or
 * CommonJS.
 */
(function (root) {
  'use strict';

  var KID = root.KatamInputDevices ||
    (typeof require === 'function' ? require('./input_devices.js') : null);
  var BT = KID.BUTTONS;
  var FULLSCREEN = 1 << 10, MUTE = 1 << 11;
  var ACTIONS = [
    { id: 'A', bit: BT.A }, { id: 'B', bit: BT.B }, { id: 'L', bit: BT.L }, { id: 'R', bit: BT.R },
    { id: 'START', bit: BT.START }, { id: 'SELECT', bit: BT.SELECT },
    { id: 'UP', bit: BT.UP }, { id: 'DOWN', bit: BT.DOWN }, { id: 'LEFT', bit: BT.LEFT },
    { id: 'RIGHT', bit: BT.RIGHT },
    { id: 'FULLSCREEN', bit: FULLSCREEN, extra: true }, { id: 'MUTE', bit: MUTE, extra: true }
  ];
  var CAPTURE_FRAMES = 360;          /* six seconds to press something        */
  var STORE = 'katam.remap';

  /* The menu's rows (platform/controls_ui.c keeps the same numbers). */
  var ROW_DEVICE = 0, ROW_ACT0 = 1, ROW_RUMBLE = ROW_ACT0 + ACTIONS.length, ROW_RESET = ROW_RUMBLE + 1;
  var ROWS = ROW_RESET + 1;

  function invert(table) {
    var p = {}, code;
    ACTIONS.forEach(function (a) { p[a.id] = []; });
    for (code in table) ACTIONS.forEach(function (a) { if (table[code] === a.bit) p[a.id].push(code); });
    return p;
  }
  function clone(p) {
    var o = {};
    ACTIONS.forEach(function (a) { o[a.id] = (p && p[a.id] ? p[a.id] : []).slice(); });
    return o;
  }

  /* --- families and defaults ----------------------------------------------- */
  function padFamily(id) {
    var s = String(id || '');
    if (/xbox|xinput|045e/i.test(s)) return 'xbox';
    if (/playstation|dualshock|dualsense|wireless controller|054c|sony/i.test(s)) return 'ps';
    if (/pro controller|switch|nintendo|057e/i.test(s)) return 'switch';
    return 'pad';
  }
  var FAMILY_NAME = { xbox: 'XBOX', ps: 'PLAYSTATION', switch: 'SWITCH PRO', pad: 'GAMEPAD' };

  /* std: the browser's "standard" mapping, or more than 9 buttons (what
   * padMask treats the same way). */
  function padDefaults(family, std) {
    var p = clone(null);
    if (family === 'switch' && std) { p.A = ['b1', 'b3']; p.B = ['b0', 'b2']; }
    else { p.A = ['b0', 'b1']; p.B = ['b2', 'b3']; }
    if (std) { p.L = ['b4', 'b6']; p.R = ['b5', 'b7']; p.SELECT = ['b8']; p.START = ['b9']; }
    else { p.L = ['b4']; p.R = ['b5']; p.SELECT = ['b6']; p.START = ['b7']; }
    p.UP = ['dU']; p.DOWN = ['dD']; p.LEFT = ['dL']; p.RIGHT = ['dR'];
    return p;
  }
  function kbDefaults(set) {
    var p = invert(set === 'b' ? KID.KEYS_B : KID.KEYS_A);
    if (set !== 'b') p.FULLSCREEN = ['KeyF'];
    return p;
  }

  /* --- names, as the menu's font can draw them (A-Z 0-9 space - . : / + !) -- */
  var KEYNAME = {
    ArrowUp: 'ARROW UP', ArrowDown: 'ARROW DN', ArrowLeft: 'ARROW L', ArrowRight: 'ARROW R',
    Enter: 'ENTER', NumpadEnter: 'NUM ENTER', Backspace: 'BKSP', ShiftRight: 'R SHIFT',
    ShiftLeft: 'L SHIFT', ControlLeft: 'L CTRL', ControlRight: 'R CTRL', AltLeft: 'L ALT',
    AltRight: 'R ALT', Space: 'SPACE', Tab: 'TAB', Period: '.', Comma: 'COMMA', Slash: '/',
    Semicolon: 'SEMI', Quote: 'QUOTE', Minus: '-', Equal: 'EQUAL', BracketLeft: 'L BRKT',
    BracketRight: 'R BRKT', Backslash: 'BSLASH', Backquote: 'BQUOTE', NumpadAdd: 'NUM +',
    NumpadSubtract: 'NUM -', NumpadMultiply: 'NUM MUL', NumpadDivide: 'NUM /', NumpadDecimal: 'NUM .'
  };
  function keyName(code) {
    var m;
    if (KEYNAME[code]) return KEYNAME[code];
    if ((m = /^Key([A-Z])$/.exec(code))) return m[1];
    if ((m = /^Digit(\d)$/.exec(code))) return m[1];
    if ((m = /^Numpad(\d)$/.exec(code))) return 'NUM ' + m[1];
    return String(code).toUpperCase().replace(/[^A-Z0-9 .:\/+!-]/g, '').slice(0, 9) || '?';
  }
  var BTNNAME = {
    xbox: ['A', 'B', 'X', 'Y', 'LB', 'RB', 'LT', 'RT', 'VIEW', 'MENU', 'LS', 'RS', 'DPAD UP', 'DPAD DN', 'DPAD L', 'DPAD R', 'XBOX'],
    ps: ['CROSS', 'CIRCLE', 'SQUARE', 'TRIANGLE', 'L1', 'R1', 'L2', 'R2', 'SHARE', 'OPTIONS', 'L3', 'R3', 'DPAD UP', 'DPAD DN', 'DPAD L', 'DPAD R', 'PS', 'TOUCHPAD'],
    switch: ['B', 'A', 'Y', 'X', 'L', 'R', 'ZL', 'ZR', 'MINUS', 'PLUS', 'LS', 'RS', 'DPAD UP', 'DPAD DN', 'DPAD L', 'DPAD R', 'HOME', 'CAPTURE'],
    pad: ['BTN 0', 'BTN 1', 'BTN 2', 'BTN 3', 'LB', 'RB', 'LT', 'RT', 'SELECT', 'START', 'LS', 'RS', 'DPAD UP', 'DPAD DN', 'DPAD L', 'DPAD R', 'HOME']
  };
  function padInputName(inp, family, std) {
    var m;
    if (inp === 'dU') return 'DPAD UP';
    if (inp === 'dD') return 'DPAD DN';
    if (inp === 'dL') return 'DPAD L';
    if (inp === 'dR') return 'DPAD R';
    if ((m = /^b(\d+)$/.exec(inp))) {
      var n = +m[1], t = BTNNAME[family] || BTNNAME.pad;
      return std && t[n] ? t[n] : 'BTN ' + n;
    }
    if ((m = /^a(\d+)([+-])$/.exec(inp))) {
      var ax = +m[1], pos = m[2] === '+';
      if (std && ax < 4) return (ax < 2 ? 'LS ' : 'RS ') + (ax % 2 ? (pos ? 'DN' : 'UP') : (pos ? 'R' : 'L'));
      return 'AXIS ' + ax + m[2];
    }
    return '?';
  }

  /* --- one pad's inputs ------------------------------------------------------ */
  function btnOn(b, i) { return !!(b[i] && (b[i].pressed || b[i].value > 0.5)); }
  function inputOn(gp, inp, dirs) {
    var m;
    if (inp.charAt(0) === 'd')
      return !!(dirs & ({ dU: BT.UP, dD: BT.DOWN, dL: BT.LEFT, dR: BT.RIGHT })[inp]);
    if ((m = /^b(\d+)$/.exec(inp))) return btnOn(gp.buttons || [], +m[1]);
    if ((m = /^a(\d+)([+-])$/.exec(inp))) {
      var v = (gp.axes || [])[+m[1]];
      return typeof v === 'number' && (m[2] === '+' ? v > 0.5 : v < -0.5);
    }
    return false;
  }
  /* Every input pressed right now (for press-to-bind): buttons, and axes
   * well past centre (0.6: a stick resting a little off is not a press). */
  function padPressed(gp) {
    var out = [], b = gp.buttons || [], ax = gp.axes || [], i;
    for (i = 0; i < b.length; i++) if (btnOn(b, i)) out.push('b' + i);
    for (i = 0; i < ax.length; i++) {
      if (ax[i] > 0.6 && ax[i] <= 1.0) out.push('a' + i + '+');
      else if (ax[i] < -0.6) out.push('a' + i + '-');
    }
    return out;
  }

  /* opts.storage: { getItem, setItem } (default localStorage, failing
   * quietly); opts.onAction(name) for FULLSCREEN / MUTE; opts.log. */
  function KatamRemap(opts) {
    opts = opts || {};
    this.storage = opts.storage !== undefined ? opts.storage : (function () {
      try { return root.localStorage || null; } catch (e) { return null; }
    })();
    this.onAction = opts.onAction || function () {};
    this.log = opts.log || function () {};
    this.kb = { a: null, b: null };        /* saved profiles (null: defaults)    */
    this.pads = {};                        /* gp.id -> saved profile             */
    this.rumbleOn = true;
    this.gen = 1;                          /* bumps on every change (the menu)    */
    this.sel = 0;                          /* the device the menu edits           */
    this.cap = null;                       /* press to bind, while waiting        */
    this.settle = false;                   /* after a bind: all let go first      */
    this.notice = '';                      /* the last bind's conflict            */
    this.extraWas = {};                    /* source -> FULLSCREEN|MUTE held       */
    this.padInfo = [];                     /* the pads seen last scan            */
    this.load();
  }
  var P = KatamRemap.prototype;

  P.load = function () {
    var j = null;
    try { j = this.storage && JSON.parse(this.storage.getItem(STORE) || 'null'); } catch (e) { j = null; }
    if (!j || typeof j !== 'object') return;
    var self = this, ok = function (p) {
      if (!p || typeof p !== 'object') return null;
      var o = clone(null);
      ACTIONS.forEach(function (a) {
        if (Array.isArray(p[a.id])) o[a.id] = p[a.id].filter(function (s) { return typeof s === 'string'; }).slice(0, 4);
      });
      return o;
    };
    if (j.kb) { this.kb.a = ok(j.kb.a); this.kb.b = ok(j.kb.b); }
    if (j.pads && typeof j.pads === 'object')
      Object.keys(j.pads).forEach(function (k) { var p = ok(j.pads[k]); if (p) self.pads[k] = p; });
    if (j.rumble === 0 || j.rumble === false) this.rumbleOn = false;
  };
  P.save = function () {
    this.gen++;
    if (!this.storage) return;
    try {
      this.storage.setItem(STORE, JSON.stringify({ v: 1, kb: this.kb, pads: this.pads, rumble: this.rumbleOn ? 1 : 0 }));
    } catch (e) { /* private mode: this visit only */ }
  };

  /* --- the profiles ---------------------------------------------------------- */
  P.kbProfile = function (set) { return this.kb[set] || kbDefaults(set); };
  P.padProfile = function (gp) {
    var std = gp.mapping === 'standard' || (gp.buttons || []).length > 9;
    return this.pads[gp.id] || padDefaults(padFamily(gp.id), std);
  };

  /* KeyboardEvent.codes held -> set's mask (FULLSCREEN / MUTE bits too). */
  P.keyMask = function (set, held) {
    var p = this.kbProfile(set), m = 0;
    ACTIONS.forEach(function (a) {
      var l = p[a.id];
      for (var i = 0; i < l.length; i++) if (held[l[i]]) { m |= a.bit; break; }
    });
    return m;
  };
  /* Is code on either keyboard profile (the page then keeps the key)? */
  P.keyKnown = function (code) {
    var self = this;
    return ['a', 'b'].some(function (s) {
      var p = self.kbProfile(s);
      return ACTIONS.some(function (a) { return p[a.id].indexOf(code) >= 0; });
    });
  };
  /* A pad's mask by its profile (FULLSCREEN / MUTE bits too); st: its
   * device record (padMask's memory of the hats). */
  P.padMask = function (gp, st) {
    var p = this.padProfile(gp), dirs = KID.padMask(gp, st) & (BT.UP | BT.DOWN | BT.LEFT | BT.RIGHT), m = 0;
    ACTIONS.forEach(function (a) {
      var l = p[a.id];
      for (var i = 0; i < l.length; i++) if (inputOn(gp, l[i], dirs)) { m |= a.bit; break; }
    });
    return m;
  };

  /* FULLSCREEN / MUTE: a new press from source src runs it. */
  P.extras = function (src, mask) {
    var x = mask & (FULLSCREEN | MUTE), was = this.extraWas[src] || 0, now = x & ~was;
    this.extraWas[src] = x;
    if (this.cap || this.settle) return;
    if (now & FULLSCREEN) this.onAction('fullscreen');
    if (now & MUTE) this.onAction('mute');
  };

  /* --- the devices the menu can edit ---------------------------------------- */
  P.setPads = function (list) {          /* [{ id, std, label }] present pads */
    var was = this.devices()[this.sel], key = was && was.key;
    this.padInfo = list.slice();
    var d = this.devices(), i;
    this.sel = 0;
    for (i = 0; i < d.length; i++) if (d[i].key === key) this.sel = i;
    if (JSON.stringify(list) !== this.padSig) { this.padSig = JSON.stringify(list); this.gen++; }
  };
  P.devices = function () {
    var out = [{ key: 'kb:a', kind: 'kb', set: 'a', name: 'KEYBOARD' }, { key: 'kb:b', kind: 'kb', set: 'b', name: 'KEYS 2' }];
    this.padInfo.forEach(function (p) {
      var f = padFamily(p.id);
      out.push({ key: 'pad:' + p.id, kind: 'pad', id: p.id, std: p.std, family: f,
                 name: (p.label || 'PAD') + ' ' + FAMILY_NAME[f] });
    });
    return out;
  };
  P.current = function () { var d = this.devices(); return d[this.sel] || d[0]; };
  P.profileOf = function (d) {
    if (d.kind === 'kb') return this.kbProfile(d.set);
    return this.pads[d.id] || padDefaults(d.family, d.std);
  };
  P.inputName = function (d, inp) {
    return d.kind === 'kb' ? keyName(inp) : padInputName(inp, d.family, d.std);
  };
  P.setProfile = function (d, p) {
    if (d.kind === 'kb') this.kb[d.set] = p; else if (p) this.pads[d.id] = p; else delete this.pads[d.id];
    this.save();
  };

  /* The other actions an input of action ai is also on. */
  P.conflicts = function (d, ai) {
    var p = this.profileOf(d), mine = p[ACTIONS[ai].id], out = [];
    ACTIONS.forEach(function (a, j) {
      if (j !== ai && p[a.id].some(function (s) { return mine.indexOf(s) >= 0; })) out.push(a.id);
    });
    return out;
  };

  /* --- press to bind ---------------------------------------------------------- */
  P.bind = function (ai, baseline) {
    var d = this.current();
    this.cap = { ai: ai, key: d.key, left: CAPTURE_FRAMES, base: baseline || {} };
    this.notice = '';
    this.gen++;
  };
  P.cancel = function () {
    if (!this.cap) return;
    this.cap = null;
    this.settle = true;
    this.gen++;
  };
  P.capturing = function () { return !!this.cap; };
  /* An input pressed during a capture: from device key dk ('kb:a' for any
   * key -- either set's profile takes it, the menu says which). */
  P.offer = function (dk, inp) {
    var c = this.cap;
    if (!c) return false;
    var d = this.current();
    if (d.key !== c.key) { this.cancel(); return false; }
    if (d.kind === 'kb' ? dk !== 'kb' : dk !== d.key) return false;
    if (inp === 'Escape') { this.cancel(); return true; }
    var p = clone(this.profileOf(d)), id = ACTIONS[c.ai].id;
    p[id] = [inp];
    this.cap = null;
    this.settle = true;
    this.setProfile(d, p);
    var cf = this.conflicts(d, c.ai);
    this.notice = cf.length ? 'ALSO ON ' + cf.join(' ') : '';
    this.log('bound ' + id + ' to ' + this.inputName(d, inp) + ' on ' + d.name + (cf.length ? ' (also ' + cf.join(', ') + ')' : ''));
    return true;
  };
  /* Once a frame during a capture: a pad's new presses (not held when the
   * capture began).  key: 'pad:' + gp.id. */
  P.offerPad = function (gp) {
    var c = this.cap;
    if (!c) return;
    var key = 'pad:' + gp.id, pressed = padPressed(gp), base = c.base[key];
    if (!base) base = c.base[key] = { init: false, held: {} };
    if (!base.init) {                     /* the first look: what is already down */
      base.init = true;
      pressed.forEach(function (s) { base.held[s] = 1; });
      return;
    }
    Object.keys(base.held).forEach(function (s) { if (pressed.indexOf(s) < 0) delete base.held[s]; });
    for (var i = 0; i < pressed.length; i++)
      if (!base.held[pressed[i]] && this.offer(key, pressed[i])) return;
  };
  /* Once a frame: the capture's clock, and the settle after it. anyDown:
   * any key or button down on any device. */
  P.frame = function (anyDown) {
    if (this.cap && --this.cap.left <= 0) { this.log('bind: nothing pressed'); this.cancel(); }
    if (!this.cap && this.settle && !anyDown) { this.settle = false; this.gen++; }
  };
  /* While this is true the game gets no buttons. */
  P.holding = function () { return !!this.cap || this.settle; };

  /* --- the menu's rows (platform/controls_ui.c) ------------------------------- */
  P.rowValue = function (row) {
    var d = this.current(), self = this;
    if (row === ROW_DEVICE) return d.name;
    if (row === ROW_RUMBLE) return this.rumbleOn ? 'ON' : 'OFF';
    if (row === ROW_RESET) return '';
    var ai = row - ROW_ACT0;
    if (ai < 0 || ai >= ACTIONS.length) return '';
    if (this.cap && this.cap.ai === ai) return d.kind === 'kb' ? 'PRESS A KEY' : 'PRESS A BUTTON';
    var l = this.profileOf(d)[ACTIONS[ai].id];
    if (!l.length) return '-';
    var t = l.slice(0, 2).map(function (s) { return self.inputName(d, s); }).join(' / ');
    if (l.length > 2) t += ' +' + (l.length - 2);
    if (this.conflicts(d, ai).length) t = '! ' + t;
    return t;
  };
  P.rowDesc = function (row) {
    var d = this.current();
    if (row === ROW_DEVICE) return 'LEFT RIGHT: WHICH CONTROLLER';
    if (row === ROW_RUMBLE) return 'PAD SHAKES ON HITS AND ABILITIES';
    if (row === ROW_RESET) return 'THIS CONTROLLER TO DEFAULTS';
    var ai = row - ROW_ACT0;
    if (ai < 0 || ai >= ACTIONS.length) return '';
    if (this.cap && this.cap.ai === ai) return d.kind === 'kb' ? 'ESCAPE OR WAIT: CANCEL' : 'WAIT A MOMENT TO CANCEL';
    var cf = this.conflicts(d, ai);
    if (cf.length) return ('ALSO ON ' + cf.join(' ')).slice(0, 30);
    return ACTIONS[ai].extra ? 'A: PRESS THE NEW BUTTON' : 'A: REBIND  LEFT: DEFAULT';
  };
  /* A row's Left (dir -1) / Right or A (+1).  baseline: what bind() should
   * treat as already held. */
  P.rowStep = function (row, dir) {
    var d = this.current(), n = this.devices().length;
    if (this.cap) { this.cancel(); return; }
    if (row === ROW_DEVICE) { this.sel = (this.sel + (dir < 0 ? n - 1 : 1)) % n; this.gen++; return; }
    if (row === ROW_RUMBLE) { this.rumbleOn = !this.rumbleOn; this.save(); return; }
    if (row === ROW_RESET) { this.setProfile(d, null); return; }
    var ai = row - ROW_ACT0;
    if (ai < 0 || ai >= ACTIONS.length) return;
    if (dir < 0) {                        /* this action back to its default */
      var def = d.kind === 'kb' ? kbDefaults(d.set) : padDefaults(d.family, d.std);
      var p = clone(this.profileOf(d));
      p[ACTIONS[ai].id] = def[ACTIONS[ai].id].slice();
      this.setProfile(d, p);
      return;
    }
    this.bind(ai);
  };
  P.query = function (key) {
    var m;
    if (key === 'remap.on') return '1';
    if (key === 'remap.gen') return String(this.gen);
    if ((m = /^remap\.v\.(\d+)$/.exec(key))) return this.rowValue(+m[1]);
    if ((m = /^remap\.d\.(\d+)$/.exec(key))) return this.rowDesc(+m[1]);
    return null;
  };

  /* --- rumble ----------------------------------------------------------------- */
  function vibrate(gp, strong, ms) {
    try {
      var va = gp.vibrationActuator;
      if (va && va.playEffect) {
        va.playEffect(va.type === 'vibration' ? 'vibration' : 'dual-rumble',
                      { duration: ms, strongMagnitude: strong, weakMagnitude: Math.min(1, strong + 0.3) });
        return true;
      }
      if (gp.hapticActuators && gp.hapticActuators[0] && gp.hapticActuators[0].pulse) {
        gp.hapticActuators[0].pulse(strong, ms);
        return true;
      }
    } catch (e) { /* not this pad */ }
    return false;
  }
  /* Once a frame: probe(p) -> -1 (no Kirby) or hp | ability << 8, for
   * players 0..3; padsOf(p) -> the Gamepad objects player p is playing
   * with; live: the game's HUD is up (in play, not paused). */
  P.watch = function (probe, padsOf, live) {
    var p, v;
    if (!this.seen) this.seen = [-1, -1, -1, -1];
    for (p = 0; p < 4; p++) {
      v = live ? probe(p) : -1;
      var was = this.seen[p];
      this.seen[p] = v;
      if (!this.rumbleOn || v < 0 || was < 0) continue;
      var hp = v & 255, hp0 = was & 255, ab = (v >> 8) & 255, ab0 = (was >> 8) & 255, kind = 0;
      if (hp < hp0 && hp0 > 0) kind = 2;                /* hurt               */
      else if (ab && ab !== ab0) kind = 1;              /* a new ability      */
      if (!kind) continue;
      var pads = padsOf(p) || [];
      for (var i = 0; i < pads.length; i++)
        vibrate(pads[i], kind === 2 ? 0.9 : 0.25, kind === 2 ? 220 : 110);
    }
  };

  /* --- the page: hook the devices and the menu ------------------------------- */
  /* devs: the KatamInputDevices instance; Module: the game's.  Returns the
   * KatamRemap. */
  KatamRemap.install = function (devs, Module, opts) {
    var r = new KatamRemap(opts);
    devs.remap = r;
    return r;
  };
  /* The Gamepad objects player p plays with (devs: KatamInputDevices). */
  KatamRemap.padsOf = function (devs, p) {
    var list, out = [], k;
    try { list = devs.getGamepads() || []; } catch (e) { return out; }
    for (k in devs.padIds) {
      var d = devs.find(devs.padIds[k]);
      if (!d || !d.present || !(d.player === p || (p === 0 && d.player === KID.FOLLOW))) continue;
      for (var i = 0; i < list.length; i++)
        if (list[i] && String(list[i].index !== undefined ? list[i].index : i) === k) out.push(list[i]);
    }
    return out;
  };
  /* Module.portUiQuery / portUiAction (web/shell.html): answer 'remap.*'. */
  KatamRemap.hookMenu = function (Module, r) {
    var q = Module.portUiQuery, a = Module.portUiAction;
    Module.portUiQuery = function (key) {
      if (String(key).indexOf('remap.') === 0) { var v = r.query(key); return v === null ? '' : v; }
      return q ? q(key) : '';
    };
    Module.portUiAction = function (name, arg) {
      if (name === 'remap.step') { r.rowStep(arg & 255, (arg >> 8) & 1 ? 1 : -1); return true; }
      return a ? a(name, arg) : false;
    };
  };

  KatamRemap.ACTIONS = ACTIONS;
  KatamRemap.ROWS = ROWS;
  KatamRemap.ROW = { DEVICE: ROW_DEVICE, ACT0: ROW_ACT0, RUMBLE: ROW_RUMBLE, RESET: ROW_RESET };
  KatamRemap.FULLSCREEN = FULLSCREEN;
  KatamRemap.MUTE = MUTE;
  KatamRemap.CAPTURE_FRAMES = CAPTURE_FRAMES;
  KatamRemap.padFamily = padFamily;
  KatamRemap.padDefaults = padDefaults;
  KatamRemap.kbDefaults = kbDefaults;
  KatamRemap.keyName = keyName;
  KatamRemap.padInputName = padInputName;

  if (typeof module !== 'undefined' && module.exports) module.exports = KatamRemap;
  else root.KatamRemap = KatamRemap;
})(typeof window !== 'undefined' ? window : this);
