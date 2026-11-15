/* input_devices.js -- input devices -> players for the web page: drop-in /
 * drop-out local multiplayer, the page's half.
 *
 * The same rules as the native core, platform/input_devices.c (read its
 * header comment; this file follows it line for line where it can):
 *
 *   - A device is keyboard set A, keyboard set B, the touch buttons, or a
 *     gamepad (the Gamepad API, "standard" mapping).  Each has an id, a kind,
 *     a name and a GBA key mask per frame.
 *   - A device nobody claimed (FOLLOW) drives player 1.  PARKED drives nobody
 *     but its Start joins; OFF is ignored.
 *   - Drop in: Start on a FOLLOW gamepad that is not the one player 1 is
 *     using, keyboard set B's Start, or Start on a PARKED device -> a new
 *     player on the next free slot (the one it last had, else a person with
 *     no controller, else the lowest CPU the story is not holding).
 *   - Drop out: dropOut(player) (the menu), or a pad unplugged for
 *     GRACE_FRAMES.  Plugged back in within that, it carries on; later, it
 *     is PARKED and Start rejoins.
 *   - The assignment is the page's.  Only per-player masks and join/leave
 *     requests reach the game (both recorded by the C side).
 *
 * No DOM or wasm dependency: the page gives it a `sink` (the game's exports)
 * and calls poll() once a frame; node tests give it fakes
 * (tools/input_devices_test.mjs).  Works as a classic <script> (sets
 * window.KatamInputDevices) and as a CommonJS module.
 *
 * Wiring it into web/shell.html is described in docs/MULTIPLAYER.md §13.
 */
(function (root) {
  'use strict';

  var A = 1, B = 2, SELECT = 4, START = 8, RIGHT = 16, LEFT = 32, UP = 64, DOWN = 128,
      BTN_R = 256, BTN_L = 512;

  var KIND = { KEYS_A: 1, KEYS_B: 2, TOUCH: 3, PAD: 4 };
  var FOLLOW = -1, PARKED = -2, OFF = -3;
  var GRACE_FRAMES = 180;
  var PEND_FRAMES = 60;

  /* Keyboard set B: the arrows, the numpad, and . , M / ; ' for a keyboard
   * without one (KeyboardEvent.code).  Set A is every other key of the
   * page's KEYMAP: WASD, J/Z, K/X, Q, E, Enter, Backspace, right Shift. */
  var KEYS_B = {
    ArrowUp: UP, ArrowDown: DOWN, ArrowLeft: LEFT, ArrowRight: RIGHT,
    Numpad0: A, Numpad2: A, Numpad1: B, NumpadDecimal: B,
    Numpad4: BTN_L, Numpad5: BTN_R, NumpadEnter: START, NumpadAdd: SELECT,
    Period: A, Comma: B, KeyM: BTN_L, Slash: BTN_R, Semicolon: START, Quote: SELECT
  };
  var KEYS_A = {
    KeyW: UP, KeyS: DOWN, KeyA: LEFT, KeyD: RIGHT,
    KeyJ: A, KeyZ: A, KeyK: B, KeyX: B, KeyQ: BTN_L, KeyE: BTN_R,
    Enter: START, Backspace: SELECT, ShiftRight: SELECT
  };

  /* The standard mapping, as natively: bottom/right face A, left/top B,
   * shoulders and triggers L/R, Back/Select, Start, d-pad and left stick. */
  function padMask(gp) {
    var m = 0, b = gp.buttons || [], ax = gp.axes || [];
    function on(i) { return !!(b[i] && (b[i].pressed || b[i].value > 0.5)); }
    if (on(0) || on(1)) m |= A;
    if (on(2) || on(3)) m |= B;
    if (on(4) || on(6)) m |= BTN_L;
    if (on(5) || on(7)) m |= BTN_R;
    if (on(8)) m |= SELECT;
    if (on(9)) m |= START;
    if (on(12) || ax[1] < -0.5) m |= UP;
    if (on(13) || ax[1] > 0.5) m |= DOWN;
    if (on(14) || ax[0] < -0.5) m |= LEFT;
    if (on(15) || ax[0] > 0.5) m |= RIGHT;
    return m;
  }

  /* opts.sink: { slotIsHuman(slot), request(slot, on) -> frame|0,
   *              canRequest(), hostKeys(player, mask), slotJoinable(slot)? }
   * opts.getGamepads: () -> array (default navigator.getGamepads)
   * opts.log: (text) -> void */
  function KatamInputDevices(opts) {
    opts = opts || {};
    this.sink = opts.sink;
    this.getGamepads = opts.getGamepads || function () {
      return (typeof navigator !== 'undefined' && navigator.getGamepads) ? navigator.getGamepads() : [];
    };
    this.log = opts.log || function () {};
    this.devs = [];
    this.nextId = 1;
    this.p1Dev = 0;
    this.grace = [0, 0, 0, 0];
    this.pend = [0, 0, 0, 0];
    this.pendOn = [0, 0, 0, 0];
    this.bound = [0, 0, 0, 0];
    this.out = [0, 0, 0, 0];
    this.held = Object.create(null);    /* KeyboardEvent.code -> 1 */
    this.touch = 0;
    this.padIds = Object.create(null);   /* gamepad slot index -> device id */
    this.kbA = this.addDevice(KIND.KEYS_A, 'keyboard-a', 'Keyboard');
    this.kbB = this.addDevice(KIND.KEYS_B, 'keyboard-b', 'Keyboard (arrows + numpad)');
    this.touchId = 0;                    /* added on the first touch */
  }

  var P = KatamInputDevices.prototype;

  P.find = function (id) {
    for (var i = 0; i < this.devs.length; i++) if (this.devs[i].id === id) return this.devs[i];
    return null;
  };

  P.addDevice = function (kind, key, name) {
    var i, d;
    if (key)
      for (i = 0; i < this.devs.length; i++) {
        d = this.devs[i];
        if (!d.present && d.kind === kind && d.key === key) {
          d.present = true; d.keys = d.prev = 0; d.name = name;
          if (d.player >= 1) this.grace[d.player] = 0;
          this.log(d.name + ' is back (' + (d.player >= 0 ? 'still playing'
                   : d.player === PARKED ? 'parked: Start rejoins' : 'player 1') + ')');
          return d.id;
        }
      }
    d = { id: this.nextId++, kind: kind, key: key || '', name: name || 'Device', player: FOLLOW,
          present: true, lastSlot: -1, swallow: false, keys: 0, prev: 0 };
    this.devs.push(d);
    this.log(d.name + ' connected (device ' + d.id + ')');
    return d.id;
  };

  P.hasDevice = function (p, absentToo) {
    for (var i = 0; i < this.devs.length; i++) {
      var d = this.devs[i];
      if (d.player === p && (d.present || absentToo)) return true;
    }
    return false;
  };

  P.removeDevice = function (id) {
    var d = this.find(id);
    if (!d || !d.present) return;
    d.present = false; d.keys = d.prev = 0; d.swallow = false;
    if (this.p1Dev === id) this.p1Dev = 0;
    if (d.player >= 1 && !this.hasDevice(d.player, false)) {
      this.grace[d.player] = GRACE_FRAMES;
      this.log(d.name + ' unplugged -- player ' + (d.player + 1) + "'s Kirby goes to the CPU in " +
               GRACE_FRAMES + ' frames');
    } else {
      this.log(d.name + ' unplugged');
    }
  };

  P.setKeys = function (id, mask) {
    var d = this.find(id);
    if (d && d.present) d.keys = mask & 0x3FF;
  };

  P.ask = function (slot, on) {
    if (!this.sink.canRequest()) return false;
    if (!this.sink.request(slot, on ? 1 : 0)) return false;
    this.pend[slot] = PEND_FRAMES;
    this.pendOn[slot] = on ? 1 : 0;
    return true;
  };

  P.freeSlot = function (d) {
    var s, sink = this.sink, self = this;
    function leaving(t) { return self.pend[t] && !self.pendOn[t]; }
    if (d.lastSlot >= 1 && !this.hasDevice(d.lastSlot, true) && !leaving(d.lastSlot)) return d.lastSlot;
    for (s = 1; s < 4; s++)
      if (!this.hasDevice(s, true) && sink.slotIsHuman(s) && !leaving(s)) return s;
    for (s = 1; s < 4; s++)
      if (!this.hasDevice(s, true) && !sink.slotIsHuman(s) && (!sink.slotJoinable || sink.slotJoinable(s)))
        return s;
    for (s = 1; s < 4; s++)
      if (!this.hasDevice(s, true) && !sink.slotIsHuman(s)) return s;
    return -1;
  };

  P.claim = function (d, s) {
    d.player = s;
    if (s >= 1) { d.lastSlot = s; this.bound[s] = 1; this.grace[s] = 0; }
    if (this.p1Dev === d.id) this.p1Dev = 0;
  };

  P.join = function (id) {
    var d = this.find(id), s;
    if (!d || !d.present || d.player === OFF || d.player >= 1) return false;
    if (!this.sink.canRequest()) { this.log(d.name + ' cannot join now'); return false; }
    s = this.freeSlot(d);
    if (s < 0) { this.log(d.name + ' cannot join -- four players already'); return false; }
    if (!this.sink.slotIsHuman(s) && !this.ask(s, true)) return false;
    this.claim(d, s);
    this.log(d.name + ' joins as player ' + (s + 1));
    return true;
  };

  P.dropOut = function (player) {
    if (player < 1 || player > 3) return false;
    for (var i = 0; i < this.devs.length; i++)
      if (this.devs[i].player === player) this.devs[i].player = PARKED;
    this.grace[player] = 0;
    this.bound[player] = 0;
    if (this.sink.slotIsHuman(player)) this.ask(player, false);
    this.log('player ' + (player + 1) + ' drops out; the CPU takes the Kirby');
    return true;
  };

  P.assign = function (id, player) {
    var d = this.find(id);
    if (!d || player < OFF || player > 3) return false;
    if (player >= 1 && !this.sink.slotIsHuman(player) && !this.ask(player, true)) return false;
    if (player >= 0) this.claim(d, player);
    else {
      d.player = player;
      if (this.p1Dev === id && player !== FOLLOW) this.p1Dev = 0;
    }
    return true;
  };

  P.joinPress = function (d, pressed) {
    if (!(pressed & START)) return false;
    if (d.player === PARKED) return d.kind !== KIND.TOUCH;
    if (d.player !== FOLLOW) return false;
    if (d.kind === KIND.KEYS_B) return true;
    return d.kind === KIND.PAD && this.p1Dev !== 0 && this.p1Dev !== d.id;
  };

  /* --- the page's inputs ---------------------------------------------------- */
  P.keyDown = function (code) {
    if (KEYS_A[code] === undefined && KEYS_B[code] === undefined) return false;
    this.held[code] = 1;
    return true;
  };
  P.keyUp = function (code) {
    if (!this.held[code]) return false;
    delete this.held[code];
    return true;
  };
  P.setTouch = function (mask) {
    if (!this.touchId && mask) this.touchId = this.addDevice(KIND.TOUCH, 'touch', 'Touch buttons');
    this.touch = mask & 0x3FF;
  };

  /* Hot-plug: the gamepads the browser reports now, against the ones known.
   * gamepadconnected/disconnected events need not be wired: a poll sees the
   * same thing, and a node test has no events. */
  P.scanPads = function () {
    var list = this.getGamepads() || [], seen = Object.create(null), i, k;
    for (i = 0; i < list.length; i++) {
      var gp = list[i];
      if (!gp || gp.connected === false) continue;
      k = String(gp.index !== undefined ? gp.index : i);
      seen[k] = 1;
      var id = this.padIds[k];
      if (!id || !this.find(id) || !this.find(id).present)
        id = this.padIds[k] = this.addDevice(KIND.PAD, gp.id || ('pad ' + k), gp.id || 'Gamepad');
      this.setKeys(id, padMask(gp));
    }
    for (k in this.padIds)
      if (!seen[k]) { this.removeDevice(this.padIds[k]); delete this.padIds[k]; }
  };

  /* Once a frame: read everything, decide, hand the game players 2-4's masks.
   * Returns player 1's mask (for Module._PortSetKeys). */
  P.poll = function () {
    var a = 0, b = 0, code;
    for (code in this.held) {
      if (KEYS_B[code] !== undefined) b |= KEYS_B[code];
      else if (KEYS_A[code] !== undefined) a |= KEYS_A[code];
    }
    this.setKeys(this.kbA, a);
    this.setKeys(this.kbB, b);
    if (this.touchId) this.setKeys(this.touchId, this.touch);
    this.scanPads();
    return this.frame();
  };

  P.frame = function () {
    var i, s, d, out = [0, 0, 0, 0], sink = this.sink;
    for (i = 0; i < this.devs.length; i++) {
      d = this.devs[i];
      if (!d.present) continue;
      if (this.joinPress(d, d.keys & ~d.prev) && this.join(d.id)) d.swallow = true;
    }
    for (i = 0; i < this.devs.length; i++) {
      d = this.devs[i];
      if (d.present && d.player === FOLLOW && (d.keys & ~d.prev)) this.p1Dev = d.id;
    }
    for (s = 1; s < 4; s++) {
      if (!this.grace[s]) continue;
      if (this.hasDevice(s, false)) { this.grace[s] = 0; continue; }
      if (--this.grace[s] === 0) {
        for (i = 0; i < this.devs.length; i++) if (this.devs[i].player === s) this.devs[i].player = PARKED;
        this.bound[s] = 0;
        this.log('player ' + (s + 1) + "'s controller did not come back; the CPU takes the Kirby");
        if (sink.slotIsHuman(s)) this.ask(s, false);
      }
    }
    for (s = 1; s < 4; s++) {
      var human = !!sink.slotIsHuman(s);
      if (!human) this.bound[s] = this.hasDevice(s, true) ? 1 : 0;
      if (this.pend[s]) {
        if (human === !!this.pendOn[s] || --this.pend[s] === 0) this.pend[s] = 0;
        continue;
      }
      if (this.hasDevice(s, true) && !human) {
        if (sink.canRequest()) this.ask(s, true);
      } else if (human && !this.hasDevice(s, true) && !this.bound[s]) {
        var spare = null;
        for (i = 0; i < this.devs.length && !spare; i++) {
          d = this.devs[i];
          if (d.present && d.player === FOLLOW && d.kind === KIND.PAD && d.id !== this.p1Dev) spare = d;
        }
        for (i = 0; i < this.devs.length && !spare; i++) {
          d = this.devs[i];
          if (d.present && d.player === FOLLOW && d.kind === KIND.KEYS_B) spare = d;
        }
        if (spare) {
          this.claim(spare, s);
          this.log('player ' + (s + 1) + ' is a person with no controller; ' + spare.name + ' is theirs');
        }
      }
    }
    for (i = 0; i < this.devs.length; i++) {
      d = this.devs[i];
      if (!d.present) continue;
      var k = d.keys;
      if (d.swallow) {
        if (!(k & START)) d.swallow = false;
        k &= ~START;
      }
      d.prev = d.keys;
      if (d.player === FOLLOW) out[0] |= k;
      else if (d.player >= 0 && d.player <= 3) out[d.player] |= k;
    }
    this.out = out;
    for (s = 1; s < 4; s++) sink.hostKeys(s, out[s]);
    return out[0];
  };

  /* --- the menu's side ------------------------------------------------------- */
  P.listDevices = function () {
    return this.devs.map(function (d) {
      return { id: d.id, kind: d.kind, player: d.player, present: d.present, keys: d.keys, name: d.name };
    });
  };
  P.listPlayers = function () {
    var out = [], p, i;
    for (p = 0; p < 4; p++) {
      var o = { player: p, human: !!this.sink.slotIsHuman(p), pending: p >= 1 && !!this.pend[p],
                graceLeft: p >= 1 ? this.grace[p] : 0, devices: [], names: [] };
      for (i = 0; i < this.devs.length; i++) {
        var d = this.devs[i];
        if (!(d.player === p || (p === 0 && d.player === FOLLOW))) continue;
        if (!d.present && d.player !== p) continue;
        o.devices.push(d.id);
        o.names.push(d.name + (d.present ? '' : ' (unplugged)'));
      }
      out.push(o);
    }
    return out;
  };
  P.player1Device = function () { return this.p1Dev; };
  P.playerKeys = function (p) { return this.out[p] || 0; };

  KatamInputDevices.KIND = KIND;
  KatamInputDevices.FOLLOW = FOLLOW;
  KatamInputDevices.PARKED = PARKED;
  KatamInputDevices.OFF = OFF;
  KatamInputDevices.GRACE_FRAMES = GRACE_FRAMES;
  KatamInputDevices.KEYS_A = KEYS_A;
  KatamInputDevices.KEYS_B = KEYS_B;
  KatamInputDevices.padMask = padMask;
  KatamInputDevices.BUTTONS = { A: A, B: B, SELECT: SELECT, START: START, RIGHT: RIGHT, LEFT: LEFT,
                                UP: UP, DOWN: DOWN, R: BTN_R, L: BTN_L };

  /* The game's exports as a sink (Module = the emscripten module). */
  KatamInputDevices.moduleSink = function (Module) {
    function up() { return !!Module.portRuntimeUp; }
    return {
      slotIsHuman: function (s) { return up() && Module._PortLmpSlotIsHuman ? Module._PortLmpSlotIsHuman(s) : (s === 0 ? 1 : 0); },
      request: function (s, on) { return up() && Module._PortLmpRequestHuman ? Module._PortLmpRequestHuman(s, on) : 0; },
      canRequest: function () { return up() && Module._PortLmpCanRequest ? Module._PortLmpCanRequest() : 0; },
      hostKeys: function (p, m) { if (up() && Module._PortLmpSetHostKeys) Module._PortLmpSetHostKeys(p, m); },
      slotJoinable: function (s) { return up() && Module._PortLmpSlotJoinable ? Module._PortLmpSlotJoinable(s) : 1; }
    };
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = KatamInputDevices;
  else root.KatamInputDevices = KatamInputDevices;
})(typeof window !== 'undefined' ? window : this);
