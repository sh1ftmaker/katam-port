/* input_devices.js -- input devices -> players for the web page: drop-in /
 * drop-out local multiplayer, the page's half.
 *
 * The same rules as the native core, platform/input_devices.c (read its
 * header comment); the changes listed under "Since the native core" below
 * were made here first and the native core now follows them too
 * (tests/input_devices_native_test.c):
 *
 *   - A device is keyboard set A, keyboard set B, the touch buttons, or a
 *     gamepad (the Gamepad API: the "standard" mapping, which is what XInput
 *     pads report, and a fallback for pads that report none).  Each has an
 *     id, a kind, a name, a short label (KEYBOARD, KEYS 2, TOUCH, PAD n) and
 *     a GBA key mask per frame.
 *   - A device nobody claimed (FOLLOW) drives player 1.  PARKED drives nobody
 *     but its Start joins; OFF is ignored.
 *   - Drop in: during play, Start on a gamepad (or keyboard set B) that is
 *     not what player 1 is playing with, or Start on a PARKED device -> a new
 *     player on the next free slot (the one it last had, else a person with
 *     no controller, else the lowest CPU the story is not holding).
 *   - Drop out: dropOut(player) (the PLAYERS page), Select held two seconds
 *     on a joined device, or a pad unplugged for GRACE_FRAMES.  Plugged back
 *     in within that, it carries on; later, it is PARKED and Start rejoins.
 *   - The assignment is the page's.  Only per-player masks and join/leave
 *     requests reach the game (both recorded by the C side).
 *
 * Since the native core (decisions reviewed when the page was wired):
 *
 *   - A join press only joins during play (sink.inStory()).  On the title
 *     and the menus a second pad's Start is player 1's, as it always was, so
 *     picking a pad up at the title never makes a player 2 by surprise.
 *   - A device player 1 has used in the last JOIN_QUIET frames is player 1's
 *     even if it was not the very last one: a keyboard-and-pad player's Start
 *     pauses, it does not join.
 *   - Once a second person is playing, the device player 1 was using is
 *     claimed by player 1 (unplugged and plugged back, it is still theirs),
 *     and the other unclaimed pads stop driving player 1: they wait, and only
 *     their Start does anything (join, or "four players already").  This is
 *     the fix for a fifth pad's Start pausing player 1's game.
 *   - Select held HOLD_LEAVE_FRAMES on a joined device leaves (the game has
 *     no use for Select in play).
 *   - Events for the page's notices (opts.onEvent) and a "free controller"
 *     test for its hint (waitingPads()).
 *   - Online (setOnline): every device drives this device's player, nothing
 *     joins and the host keys of players 2-4 are left to the session -- or,
 *     with { seats: true } (what the page uses), the local rules exactly,
 *     player p being this device's seat p (the session's Kirby for it is the
 *     host's choice: docs/MULTIPLAYER.md §14).
 *
 * No DOM or wasm dependency: the page gives it a `sink` (the game's exports)
 * and calls poll() once a frame; node tests give it fakes
 * (tools/input_devices_test.mjs).  Works as a classic <script> (sets
 * window.KatamInputDevices) and as a CommonJS module.
 *
 * Wiring: web/shell.html (the Input section) and docs/MULTIPLAYER.md §13.
 */
(function (root) {
  'use strict';

  var A = 1, B = 2, SELECT = 4, START = 8, RIGHT = 16, LEFT = 32, UP = 64, DOWN = 128,
      BTN_R = 256, BTN_L = 512;

  var KIND = { KEYS_A: 1, KEYS_B: 2, TOUCH: 3, PAD: 4 };
  var FOLLOW = -1, PARKED = -2, OFF = -3;
  var GRACE_FRAMES = 180;
  var PEND_FRAMES = 60;
  var JOIN_QUIET = 180;          /* frames since player 1 last used a device */
  var HOLD_LEAVE_FRAMES = 120;   /* Select held this long: leave              */

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

  function btn(b, i) { return !!(b[i] && (b[i].pressed || b[i].value > 0.5)); }

  /* A pad's buttons as a GBA mask.
   *
   * mapping "standard" (every XInput pad -- Xbox 360/One/Series and the many
   * pads that pretend to be one -- and the browsers' own maps of DualShock,
   * DualSense, Switch Pro ...): bottom/right face A, left/top B, shoulders and
   * triggers L/R, Back/View Select, Start/Menu Start, d-pad and left stick.
   *
   * No mapping (a DirectInput pad the browser does not know, Firefox on
   * Linux for many): buttons 0-3 the faces in the same pairs, 4-7 shoulders
   * and triggers, 8 Select, 9 Start (or 7 on a pad with only eight buttons,
   * where 6/7 are usually Select/Start), the d-pad from buttons 12-15 if
   * there are that many, from axes 0/1, from a hat as axes 6/7, or from a
   * hat as one axis (axis 9: -1 up, clockwise in steps of 2/7, 9/7 centred;
   * only once it has been seen centred, so a pad whose axis 9 is a trigger
   * or a stick is not read as a d-pad held down).  st: the pad's own memory
   * across frames (the device record). */
  function padMask(gp, st) {
    var m = 0, b = gp.buttons || [], ax = gp.axes || [];
    var on = function (i) { return btn(b, i); };
    var std = gp.mapping === 'standard';
    var nb = b.length;
    if (on(0) || on(1)) m |= A;
    if (on(2) || on(3)) m |= B;
    if (std || nb > 9) {
      if (on(4) || on(6)) m |= BTN_L;
      if (on(5) || on(7)) m |= BTN_R;
      if (on(8)) m |= SELECT;
      if (on(9)) m |= START;
    } else {
      if (on(4)) m |= BTN_L;
      if (on(5)) m |= BTN_R;
      if (on(6)) m |= SELECT;
      if (on(7)) m |= START;
    }
    if (on(12) || ax[1] < -0.5) m |= UP;
    if (on(13) || ax[1] > 0.5) m |= DOWN;
    if (on(14) || ax[0] < -0.5) m |= LEFT;
    if (on(15) || ax[0] > 0.5) m |= RIGHT;
    if (!std) {
      /* (axes 6/7 only once seen at rest at 0: a trigger resting at -1 there
       * is not a d-pad held up and left) */
      if (st && ax.length >= 8 && Math.abs(ax[6]) < 0.1 && Math.abs(ax[7]) < 0.1) st.hat67 = true;
      if (ax.length >= 8 && (!st || st.hat67)) {
        if (ax[7] < -0.5) m |= UP;
        if (ax[7] > 0.5) m |= DOWN;
        if (ax[6] < -0.5) m |= LEFT;
        if (ax[6] > 0.5) m |= RIGHT;
      }
      if (st && ax.length >= 10 && ax[9] > 1.2 && ax[9] < 1.35) st.hat = true;
      if (st && st.hat && ax[9] >= -1.05 && ax[9] <= 1.05) {
        /* a hat as one axis: 8 directions, -1 = up, steps of 2/7; > 1 = none */
        var h = Math.round((ax[9] + 1) * 3.5);
        var dirs = [UP, UP | RIGHT, RIGHT, DOWN | RIGHT, DOWN, DOWN | LEFT, LEFT, UP | LEFT];
        if (h >= 0 && h <= 7) m |= dirs[h];
      }
    }
    return m;
  }

  /* A pad's name as the browser gives it, without the vendor/product ids:
   * "Xbox 360 Controller (XInput STANDARD GAMEPAD)" -> "Xbox 360 Controller",
   * "054c-0ce6-Sony Interactive ..." -> "Sony Interactive ...". */
  function padName(id) {
    var s = String(id || '').replace(/\(.*?(vendor|product|STANDARD GAMEPAD)[^)]*\)/gi, '')
      .replace(/^[0-9a-f]{4}-[0-9a-f]{4}-/i, '').replace(/\s+/g, ' ').trim();
    return s || 'Gamepad';
  }

  /* A device the pad list should not count: too few buttons to play with
   * (the motion sensors some pads expose as a second "gamepad"). */
  function playable(gp) { return (gp.buttons || []).length >= 4; }

  /* opts.sink: { slotIsHuman(slot), request(slot, on) -> frame|0,
   *              canRequest(), hostKeys(player, mask), slotJoinable(slot)?,
   *              inStory()?, hudUp()? (the game's HUD is up: not paused) }
   * opts.getGamepads: () -> array (default navigator.getGamepads)
   * opts.log: (text) -> void
   * opts.onEvent: ({type, player, device, name, label, ...}) -> void */
  function KatamInputDevices(opts) {
    opts = opts || {};
    this.sink = opts.sink;
    this.getGamepads = opts.getGamepads || function () {
      return (typeof navigator !== 'undefined' && navigator.getGamepads) ? navigator.getGamepads() : [];
    };
    this.log = opts.log || function () {};
    this.onEvent = opts.onEvent || function () {};
    this.devs = [];
    this.nextId = 1;
    this.p1Dev = 0;
    this.tick = 0;
    this.grace = [0, 0, 0, 0];
    this.pend = [0, 0, 0, 0];
    this.pendOn = [0, 0, 0, 0];
    this.bound = [0, 0, 0, 0];
    this.out = [0, 0, 0, 0];
    this.held = Object.create(null);    /* KeyboardEvent.code -> 1 */
    this.touch = 0;
    this.padIds = Object.create(null);   /* gamepad slot index -> device id */
    this.online = false;
    this.onlineHooks = null;
    this.seatMode = false;               /* a session's seats (setOnline seats) */
    this.seatWrote = [0, 0, 0, 0];
    this.spareQuiet = 0;
    this.muted = false;                  /* the page's own menu has the input */
    this.remap = opts.remap || null;     /* web/input_remap.js: the bindings  */
    this.anyDown = false;
    this.wasMulti = false;
    this.kbA = this.addDevice(KIND.KEYS_A, 'keyboard-a', 'Keyboard');
    this.kbB = this.addDevice(KIND.KEYS_B, 'keyboard-b', 'Keyboard (arrows + numpad)');
    this.touchId = 0;                    /* added on the first touch */
  }

  var P = KatamInputDevices.prototype;

  P.find = function (id) {
    for (var i = 0; i < this.devs.length; i++) if (this.devs[i].id === id) return this.devs[i];
    return null;
  };

  P.emit = function (ev) {
    try { this.onEvent(ev); } catch (e) { this.log('onEvent threw: ' + e); }
  };

  P.label = function (d) {
    if (!d) return 'CPU';
    if (d.kind === KIND.KEYS_A) return 'KEYBOARD';
    if (d.kind === KIND.KEYS_B) return 'KEYS 2';
    if (d.kind === KIND.TOUCH) return 'TOUCH';
    return 'PAD ' + d.num;
  };

  P.addDevice = function (kind, key, name) {
    var i, d, n;
    if (key)
      for (i = 0; i < this.devs.length; i++) {
        d = this.devs[i];
        if (!d.present && d.kind === kind && d.key === key) {
          d.present = true; d.keys = d.prev = 0; d.name = name; d.hold = 0;
          if (d.player >= 1 && this.grace[d.player]) {
            this.grace[d.player] = 0;
            this.emit({ type: 'back', player: d.player, device: d.id, label: this.label(d), name: d.name });
          }
          this.log(d.name + ' is back (' + (d.player >= 0 ? 'still playing'
                   : d.player === PARKED ? 'parked: Start rejoins' : 'player 1') + ')');
          this.emit({ type: 'connected', device: d.id, label: this.label(d), name: d.name, again: true });
          return d.id;
        }
      }
    n = 0;
    if (kind === KIND.PAD) {
      /* the lowest pad number no known pad has */
      for (n = 1; ; n++) {
        var taken = false;
        for (i = 0; i < this.devs.length; i++)
          if (this.devs[i].kind === KIND.PAD && this.devs[i].num === n) taken = true;
        if (!taken) break;
      }
    }
    d = { id: this.nextId++, kind: kind, key: key || '', name: name || 'Device', player: FOLLOW,
          present: true, lastSlot: -1, swallow: false, keys: 0, prev: 0, num: n,
          lastUse: -1e9, hold: 0, touched: false };
    this.devs.push(d);
    this.log(d.name + ' connected (device ' + d.id + ')');
    if (kind === KIND.PAD) this.emit({ type: 'connected', device: d.id, label: this.label(d), name: d.name });
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
    d.present = false; d.keys = d.prev = 0; d.swallow = false; d.hold = 0;
    if (this.p1Dev === id) this.p1Dev = 0;
    if (d.player >= 1 && !this.hasDevice(d.player, false)) {
      this.grace[d.player] = GRACE_FRAMES;
      this.log(d.name + ' unplugged -- player ' + (d.player + 1) + "'s Kirby goes to the CPU in " +
               GRACE_FRAMES + ' frames');
      this.emit({ type: 'unplugged', player: d.player, device: d.id, label: this.label(d), name: d.name });
    } else {
      this.log(d.name + ' unplugged');
      this.emit({ type: 'disconnected', player: d.player, device: d.id, label: this.label(d), name: d.name });
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
    d.hold = 0;
    if (s >= 1) { d.lastSlot = s; this.bound[s] = 1; this.grace[s] = 0; }
    if (this.p1Dev === d.id) this.p1Dev = 0;
  };

  /* Anyone besides player 1 playing (or asked for)? */
  P.multi = function () {
    for (var s = 1; s < 4; s++)
      if (this.sink.slotIsHuman(s) || (this.pend[s] && this.pendOn[s]) || this.hasDevice(s, true)) return true;
    return false;
  };

  P.humans = function () {
    var n = 1;
    for (var s = 1; s < 4; s++) if (this.sink.slotIsHuman(s)) n++;
    return n;
  };

  P.join = function (id, why) {
    var d = this.find(id), s, waits;
    if (!d || !d.present || d.player === OFF || d.player >= 1) return false;
    if (this.online) {
      var hooks = this.onlineHooks;
      if (hooks && hooks.join) {
        var slot = hooks.join(d.id);
        if (slot >= 1 && slot <= 3) { this.claim(d, slot); return true; }
      }
      return false;
    }
    if (!this.sink.canRequest()) { this.log(d.name + ' cannot join now'); return false; }
    s = this.freeSlot(d);
    if (s < 0) {
      this.log(d.name + ' cannot join -- four players already');
      this.emit({ type: 'full', device: d.id, label: this.label(d), name: d.name });
      return false;
    }
    waits = !!(this.sink.slotJoinable && !this.sink.slotJoinable(s));
    if (!this.sink.slotIsHuman(s) && !this.ask(s, true)) {
      /* a session's host with every Kirby a person already */
      if (this.seatMode) this.emit({ type: 'full', device: d.id, label: this.label(d), name: d.name });
      return false;
    }
    this.claim(d, s);
    this.log(d.name + ' joins as player ' + (s + 1));
    this.emit({ type: 'join', player: s, device: d.id, label: this.label(d), name: d.name,
                waits: waits, why: why || 'start' });
    return true;
  };

  P.dropOut = function (player, why) {
    if (player < 1 || player > 3) return false;
    if (this.online) {
      var hooks = this.onlineHooks;
      if (!(hooks && hooks.leave && hooks.leave(player))) return false;
    }
    for (var i = 0; i < this.devs.length; i++)
      if (this.devs[i].player === player) { this.devs[i].player = PARKED; this.devs[i].hold = 0; }
    this.grace[player] = 0;
    this.bound[player] = 0;
    if (!this.online && this.sink.slotIsHuman(player)) this.ask(player, false);
    this.log('player ' + (player + 1) + ' drops out; the CPU takes the Kirby');
    this.emit({ type: 'leave', player: player, why: why || 'menu' });
    return true;
  };

  /* Move device `id` to player 0..3 (or FOLLOW / PARKED / OFF).  A player who
   * loses their only device to another player leaves (the CPU takes their
   * Kirby): that is what "give this pad to player 3" means on the PLAYERS
   * page.  Moving it to a CPU player asks for the join. */
  P.assign = function (id, player) {
    var d = this.find(id), from, wasHuman;
    if (!d || player < OFF || player > 3) return false;
    from = d.player;
    wasHuman = player >= 1 && !!this.sink.slotIsHuman(player);
    if (from === player) return true;
    if (player >= 1 && !this.sink.slotIsHuman(player) && !(this.pend[player] && this.pendOn[player])) {
      if (this.online || !this.ask(player, true)) return false;
    }
    if (player >= 0) this.claim(d, player);
    else {
      d.player = player;
      if (this.p1Dev === id && player !== FOLLOW) this.p1Dev = 0;
    }
    if (from >= 1 && from !== player && !this.hasDevice(from, true)) {
      this.bound[from] = 0;
      if (!this.online && this.sink.slotIsHuman(from)) this.ask(from, false);
      this.emit({ type: 'leave', player: from, why: 'moved' });
    }
    if (player >= 1 && from !== player && !wasHuman) {
      this.emit({ type: 'join', player: player, device: d.id, label: this.label(d), name: d.name,
                  waits: !!(this.sink.slotJoinable && !this.sink.slotJoinable(player)), why: 'menu' });
    }
    return true;
  };

  /* Is this press of Start a join?  (d's Start went down this frame.) */
  P.joinPress = function (d, pressed) {
    if (!(pressed & START) || this.muted) return false;
    if (d.kind === KIND.TOUCH) return false;
    if (d.player === PARKED) return true;
    if (d.player !== FOLLOW) return false;
    if (d.kind !== KIND.PAD && d.kind !== KIND.KEYS_B) return false;
    if (this.online) return !!(this.onlineHooks && this.onlineHooks.join) && this.p1Dev !== d.id;
    if (this.sink.inStory && !this.sink.inStory()) return false;
    /* With somebody else playing, every unclaimed pad is waiting to join. */
    if (this.multi()) return true;
    if (this.p1Dev === 0 || this.p1Dev === d.id) return false;
    return this.tick - d.lastUse > JOIN_QUIET;
  };

  /* --- the page's inputs ---------------------------------------------------- */
  /* repeat: the browser's auto-repeat (KeyboardEvent.repeat).  With a
   * remap (web/input_remap.js) the keys are its profiles', and while it
   * waits for a key to bind every fresh key is that. */
  P.keyDown = function (code, repeat) {
    var r = this.remap;
    if (r && r.capturing()) { if (!repeat) r.offer('kb', code); return true; }
    if (r ? !r.keyKnown(code) : (KEYS_A[code] === undefined && KEYS_B[code] === undefined)) return false;
    this.held[code] = 1;
    return true;
  };
  P.keyUp = function (code) {
    if (!this.held[code]) return false;
    delete this.held[code];
    return true;
  };
  P.releaseKeys = function () { this.held = Object.create(null); };
  P.setTouch = function (mask) {
    if (!this.touchId && mask) this.touchId = this.addDevice(KIND.TOUCH, 'touch', 'Touch buttons');
    this.touch = mask & 0x3FF;
  };

  /* Hot-plug: the gamepads the browser reports now, against the ones known.
   * A poll sees what gamepadconnected/disconnected say (the page also calls
   * scanPads from them), and a node test has no events. */
  P.scanPads = function () {
    var list, seen = Object.create(null), i, k, pads = [];
    try { list = this.getGamepads() || []; } catch (e) { list = []; }
    for (i = 0; i < list.length; i++) {
      var gp = list[i];
      if (!gp || gp.connected === false || !playable(gp)) continue;
      k = String(gp.index !== undefined ? gp.index : i);
      seen[k] = 1;
      var id = this.padIds[k], known = id && this.find(id);
      if (known && known.present && known.key !== (gp.id || ('pad ' + k))) {
        /* another pad in the same browser slot without a disconnect between */
        this.removeDevice(id);
        known = null;
      }
      if (!known || !known.present)
        id = this.padIds[k] = this.addDevice(KIND.PAD, gp.id || ('pad ' + k), padName(gp.id));
      if (this.remap) {
        var r = this.remap, pm = r.padMask(gp, this.find(id));
        if (r.capturing()) r.offerPad(gp);
        r.extras('pad:' + gp.id, pm);
        if (pm) this.anyDown = true;
        pads.push({ id: gp.id || ('pad ' + k), std: gp.mapping === 'standard' || (gp.buttons || []).length > 9,
                    label: 'PAD ' + this.find(id).num });
        this.setKeys(id, pm);
      } else this.setKeys(id, padMask(gp, this.find(id)));
    }
    if (this.remap) this.remap.setPads(pads);
    for (k in this.padIds)
      if (!seen[k]) { this.removeDevice(this.padIds[k]); delete this.padIds[k]; }
  };

  /* Once a frame: read everything, decide, hand the game players 2-4's masks.
   * Returns player 1's mask (for Module._PortSetKeys). */
  P.poll = function () {
    var a = 0, b = 0, code, r = this.remap, i;
    if (r) {
      a = r.keyMask('a', this.held);
      b = r.keyMask('b', this.held);
      r.extras('kb:a', a);
      r.extras('kb:b', b);
    } else
      for (code in this.held) {
        if (KEYS_B[code] !== undefined) b |= KEYS_B[code];
        else if (KEYS_A[code] !== undefined) a |= KEYS_A[code];
      }
    this.setKeys(this.kbA, a);
    this.setKeys(this.kbB, b);
    if (this.touchId) this.setKeys(this.touchId, this.touch);
    this.anyDown = !!(a || b || this.touch);
    this.scanPads();
    if (r) {
      /* binding a button: the game sees nothing until it is all let go */
      r.frame(this.anyDown);
      if (r.holding()) for (i = 0; i < this.devs.length; i++) this.devs[i].keys = 0;
    }
    return this.frame();
  };

  /* Online (an online session is on, web/online.js): every device's keys are
   * this device's player's (the session decides which Kirby that is), no
   * local joins, and players 2-4's host keys belong to the session.
   * hooks (for a session that carries more players per device):
   *   { split: true  -- keep the local assignment: localKeys(p) per player,
   *     join(devId) -> slot 1..3 or -1, leave(slot) -> bool }.
   *   { seats: true } -- the local rules, unchanged: during a session the
   *     sink's calls speak of this device's SEATS (platform/localmp.c's
   *     "device layer's door"), so player p is seat p, its keys go to
   *     hostKeys(p) as always, a join asks the host for a Kirby for that
   *     seat and a leave gives it back.  this.seatMode says so (the page's
   *     notices name the Kirby the host gave the seat). */
  P.setOnline = function (on, hooks) {
    on = !!on;
    if (hooks !== undefined) this.onlineHooks = hooks || null;
    var seats = on && !!(this.onlineHooks && this.onlineHooks.seats), s;
    if (seats !== this.seatMode) {
      /* what the sink's slot numbers mean changes: nothing is pending */
      this.seatMode = seats;
      for (s = 1; s < 4; s++) this.pend[s] = 0;
      /* leaving a session: for a moment the game still has the session's
       * people (the page puts this device's own game back at the next
       * frame), and none of them is a person here to give a controller to */
      if (!seats) this.spareQuiet = this.tick + 60;
      this.log(seats ? 'online: the local players are this device\'s seats' : 'online session over: local players again');
    }
    on = on && !seats;
    if (on === this.online) return;
    this.online = on;
    for (s = 1; s < 4; s++) this.pend[s] = 0;
    if (on) for (s = 1; s < 4; s++) this.sink.hostKeys(s, 0);
    this.log(on ? 'online: every device drives this device\'s player' : 'online session over: local players again');
  };

  P.frame = function () {
    var i, s, d, out = [0, 0, 0, 0], sink = this.sink, multi, pressed;
    this.tick++;
    for (i = 0; i < this.devs.length; i++) {
      d = this.devs[i];
      if (!d.present) continue;
      pressed = d.keys & ~d.prev;
      if (pressed) d.touched = true;
      if (this.joinPress(d, pressed)) {
        if (this.join(d.id)) d.swallow = true;
        else if (d.player === FOLLOW && (this.multi() || this.seatMode)) d.swallow = true;   /* a waiting pad's Start pauses nobody */
      }
    }
    multi = this.multi();
    /* Player 1's device: the last unclaimed one to press anything (with
     * somebody else playing, only a keyboard or the touch buttons). */
    for (i = 0; i < this.devs.length; i++) {
      d = this.devs[i];
      if (!d.present || d.player !== FOLLOW || !(d.keys & ~d.prev) || d.swallow) continue;
      if (multi && d.kind === KIND.PAD && d.id !== this.p1Dev) continue;
      this.p1Dev = d.id;
    }
    if (multi !== this.wasMulti) {
      this.wasMulti = multi;
      /* The first other person: the device player 1 was playing with is
       * theirs for good (unplugged and back, it is still player 1's). */
      if (multi && this.p1Dev && !this.online) {
        d = this.find(this.p1Dev);
        if (d && d.player === FOLLOW && d.kind === KIND.PAD) { this.claim(d, 0); this.p1Dev = d.id; }
      }
      this.emit({ type: multi ? 'multi' : 'single' });
    }
    if (!this.online) {
      for (s = 1; s < 4; s++) {
        if (!this.grace[s]) continue;
        if (this.hasDevice(s, false)) { this.grace[s] = 0; continue; }
        if (--this.grace[s] === 0) {
          for (i = 0; i < this.devs.length; i++) if (this.devs[i].player === s) this.devs[i].player = PARKED;
          this.bound[s] = 0;
          this.log('player ' + (s + 1) + "'s controller did not come back; the CPU takes the Kirby");
          if (sink.slotIsHuman(s)) this.ask(s, false);
          this.emit({ type: 'leave', player: s, why: 'unplugged' });
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
        } else if (human && !this.hasDevice(s, true) && !this.bound[s] && !this.seatMode &&
                   this.tick >= this.spareQuiet) {
          /* (not in a session: a seat there is only ever a person because a
           * controller of this device asked, or katamOnline.requestSlot did
           * -- whose keys are its caller's) */
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
            this.emit({ type: 'join', player: s, device: spare.id, label: this.label(spare), name: spare.name,
                        why: 'game' });
          }
        }
      }
    }
    var leaveNow = [];
    for (i = 0; i < this.devs.length; i++) {
      d = this.devs[i];
      if (!d.present) continue;
      var k = d.keys;
      if (d.swallow) {
        if (!(k & START)) d.swallow = false;
        k &= ~START;
      }
      d.prev = d.keys;
      /* Select held on a joined device: leave */
      if (d.player >= 1 && (k & SELECT) && !this.online && !this.muted) {
        if (++d.hold === HOLD_LEAVE_FRAMES) leaveNow.push(d.player);
      } else d.hold = 0;
      if (d.player === FOLLOW) {
        if (this.online && !(this.onlineHooks && this.onlineHooks.split)) out[0] |= k;
        else if (multi && d.kind === KIND.PAD && d.id !== this.p1Dev) continue;   /* waiting */
        else out[0] |= k;
        if (k) d.lastUse = this.tick;
      } else if (d.player >= 0 && d.player <= 3) {
        if (this.online && !(this.onlineHooks && this.onlineHooks.split)) out[0] |= k;
        else out[d.player] |= k;
      }
    }
    for (i = 0; i < leaveNow.length; i++) this.dropOut(leaveNow[i], 'select');
    if (this.muted) out = [0, 0, 0, 0];   /* the page's menu is open: the game sees nothing */
    this.out = out;
    if (this.online) return out[0];
    for (s = 1; s < 4; s++) {
      /* a session's seats: only those a controller here plays (and a last 0
       * when it stops) -- katamOnline.requestSlot's callers set their own */
      var mine = this.hasDevice(s, true);
      if (!this.seatMode || mine || this.seatWrote[s]) sink.hostKeys(s, out[s]);
      this.seatWrote[s] = mine;
    }
    return out[0];
  };

  /* --- the hint's side ---------------------------------------------------------- */
  /* Pads that would join on Start now (a free controller), for the page's
   * "PRESS START TO JOIN".  Pads that never pressed anything are not
   * counted, nor anything when four are playing or nothing can join. */
  P.waitingPads = function () {
    var outList = [], i, d;
    if (this.online || !this.sink.canRequest() || (this.sink.inStory && !this.sink.inStory())) return outList;
    /* not while the game is paused (or between rooms): the game's HUD is up */
    if (this.sink.hudUp && !this.sink.hudUp()) return outList;
    if (this.humans() >= 4) return outList;
    var multi = this.multi();
    for (i = 0; i < this.devs.length; i++) {
      d = this.devs[i];
      if (!d.present || d.kind !== KIND.PAD || !d.touched) continue;
      if (d.player === PARKED) outList.push(d.id);
      else if (d.player !== FOLLOW || d.id === this.p1Dev) continue;
      else if (multi) outList.push(d.id);
      /* one person: only a pad player 1 has never played with (one they
       * switch to and from is theirs, and nagging them would be wrong; its
       * Start still joins once it has been put down a while) */
      else if (this.p1Dev && d.lastUse < 0) outList.push(d.id);
    }
    return outList;
  };

  /* --- the menu's side ------------------------------------------------------- */
  P.listDevices = function () {
    var self = this;
    return this.devs.map(function (d) {
      return { id: d.id, kind: d.kind, player: d.player, present: d.present, keys: d.keys, name: d.name,
               label: self.label(d), num: d.num, touched: d.touched,
               waiting: d.present && (d.player === PARKED ||
                        (d.player === FOLLOW && d.kind === KIND.PAD && d.id !== self.p1Dev && self.multi())) };
    });
  };
  P.listPlayers = function () {
    var out = [], p, i;
    for (p = 0; p < 4; p++) {
      var o = { player: p, human: !!this.sink.slotIsHuman(p), pending: p >= 1 && !!this.pend[p],
                graceLeft: p >= 1 ? this.grace[p] : 0, devices: [], names: [], labels: [], main: 0 };
      for (i = 0; i < this.devs.length; i++) {
        var d = this.devs[i];
        if (!(d.player === p || (p === 0 && d.player === FOLLOW))) continue;
        if (!d.present && d.player !== p) continue;
        if (p === 0 && d.player === FOLLOW && this.multi() && d.kind === KIND.PAD && d.id !== this.p1Dev) continue;
        o.devices.push(d.id);
        o.names.push(d.name + (d.present ? '' : ' (unplugged)'));
        o.labels.push(this.label(d));
      }
      /* the one to show: player 1's last used, else a claimed one, else the first */
      if (p === 0 && this.p1Dev && o.devices.indexOf(this.p1Dev) >= 0) o.main = this.p1Dev;
      else for (i = 0; i < o.devices.length && !o.main; i++) if (this.find(o.devices[i]).player === p) o.main = o.devices[i];
      if (!o.main && o.devices.length) o.main = o.devices[0];
      out.push(o);
    }
    return out;
  };
  P.player1Device = function () { return this.p1Dev; };
  P.playerKeys = function (p) { return this.out[p] || 0; };
  P.localKeys = P.playerKeys;

  KatamInputDevices.KIND = KIND;
  KatamInputDevices.FOLLOW = FOLLOW;
  KatamInputDevices.PARKED = PARKED;
  KatamInputDevices.OFF = OFF;
  KatamInputDevices.GRACE_FRAMES = GRACE_FRAMES;
  KatamInputDevices.JOIN_QUIET = JOIN_QUIET;
  KatamInputDevices.HOLD_LEAVE_FRAMES = HOLD_LEAVE_FRAMES;
  KatamInputDevices.KEYS_A = KEYS_A;
  KatamInputDevices.KEYS_B = KEYS_B;
  KatamInputDevices.padMask = padMask;
  KatamInputDevices.padName = padName;
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
      slotJoinable: function (s) { return up() && Module._PortLmpSlotJoinable ? Module._PortLmpSlotJoinable(s) : 1; },
      inStory: function () { return up() && Module._PortLmpInStory ? Module._PortLmpInStory() : 1; },
      hudUp: function () { return up() && Module._PortGameHudUpJs ? Module._PortGameHudUpJs() : 1; }
    };
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = KatamInputDevices;
  else root.KatamInputDevices = KatamInputDevices;
})(typeof window !== 'undefined' ? window : this);
