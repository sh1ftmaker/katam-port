// input_devices_test.mjs -- web/input_devices.js against fake gamepads.
//
//   node tools/input_devices_test.mjs
//
// No browser and no wasm: navigator.getGamepads is a stand-in that returns
// objects shaped like the Gamepad API's (standard mapping), and the game is a
// fake sink that applies join/leave requests at the next frame, the way the
// latch does.  The rules under test are the ones in platform/input_devices.c.
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const KID = require('../web/input_devices.js');
const BT = KID.BUTTONS;

// --- fakes -----------------------------------------------------------------
function fakeGame() {
  const g = {
    slots: 1, queue: [], frame: 0, requests: [], keys: [0, 0, 0, 0], canRequest: true,
    joinable: [1, 1, 1, 1], story: true,
    sink: {
      inStory: () => g.story,
      slotIsHuman: (s) => (s === 0 ? 1 : (g.slots >> s) & 1),
      request: (s, on) => { g.requests.push([g.frame + 1, s, on]); g.queue.push([s, on]); return g.frame + 1; },
      canRequest: () => g.canRequest,
      hostKeys: (p, m) => { g.keys[p] = m; },
      slotJoinable: (s) => g.joinable[s],
    },
    latch() {            // the next frame's latch applies what was asked
      g.frame++;
      for (const [s, on] of g.queue) g.slots = on ? g.slots | (1 << s) : g.slots & ~(1 << s);
      g.queue = [];
    },
  };
  return g;
}

function pad(index, id) {
  return { index, id, connected: true, buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })), axes: [0, 0, 0, 0] };
}
function press(gp, ...idx) { gp.buttons.forEach((b) => { b.pressed = false; b.value = 0; }); for (const i of idx) { gp.buttons[i].pressed = true; gp.buttons[i].value = 1; } }
const START = 9, A_BTN = 0, RIGHT_BTN = 15;

function rig() {
  const g = fakeGame();
  const pads = [];
  const log = [];
  const events = [];
  const core = new KID({ sink: g.sink, getGamepads: () => pads.slice(), log: (t) => log.push(t),
                         onEvent: (e) => events.push(e) });
  const step = (n = 1) => { let p1 = 0; for (let i = 0; i < n; i++) { p1 = core.poll(); g.latch(); } return p1; };
  return { g, pads, core, step, log, events };
}
const byName = (core, name) => core.listDevices().find((d) => d.name === name);
let passed = 0;
function test(name, fn) { fn(); passed++; console.log('ok', name); }

// --- tests ------------------------------------------------------------------
test('default: every unclaimed device drives player 1', () => {
  const { pads, core, step, g } = rig();
  pads.push(pad(0, 'Xbox 360 Controller (XInput STANDARD GAMEPAD)'));
  core.keyDown('KeyD');
  press(pads[0], A_BTN);
  assert.equal(step(), BT.RIGHT | BT.A);
  core.keyDown('ArrowLeft');                  // set B, unclaimed: player 1's too
  assert.equal(step() & BT.LEFT, BT.LEFT);
  assert.deepEqual(g.keys.slice(1), [0, 0, 0]);
  assert.equal(g.requests.length, 0);
});

test("a lone pad's first Start is player 1's, not a join", () => {
  const { pads, step, g } = rig();
  pads.push(pad(0, 'pad A'));
  press(pads[0], START);
  assert.equal(step() & BT.START, BT.START);
  assert.equal(g.requests.length, 0);
});

test('drop in: Start on a second pad, set B Start; four is the limit', () => {
  const { pads, core, step, g } = rig();
  core.keyDown('KeyJ'); step(); core.keyUp('KeyJ'); step();          // player 1 is on the keyboard
  pads.push(pad(0, 'Xbox One Controller'));
  step();
  press(pads[0], START);
  const p1 = step();
  assert.equal(p1 & BT.START, 0, 'the join press does not reach player 1');
  assert.deepEqual(g.requests.at(-1).slice(1), [1, 1]);
  assert.equal(g.slots & 2, 2);
  assert.equal(g.keys[1] & BT.START, 0, 'held Start is swallowed until released');
  press(pads[0], RIGHT_BTN);
  step();
  assert.equal(g.keys[1], BT.RIGHT, 'pad drives player 2');
  pads.push(pad(1, 'DualSense Wireless Controller'));
  step(); press(pads[1], START); step(); press(pads[1]); step();
  assert.equal(g.slots & 4, 4, 'player 3');
  core.keyDown('NumpadEnter'); step(); core.keyUp('NumpadEnter'); step();
  assert.equal(g.slots & 8, 8, 'keyboard set B is player 4');
  pads.push(pad(2, 'Fifth Pad'));
  step(); press(pads[2], START);
  assert.equal(step() & BT.START, 0, "a fifth pad's Start does not pause player 1's game");
  press(pads[2], RIGHT_BTN);
  assert.equal(step() & BT.RIGHT, 0, 'nor does its d-pad move player 1: it waits');
  press(pads[2]); step();
  assert.equal(byName(core, 'Fifth Pad').player, KID.FOLLOW);
  assert.ok(core.listPlayers().every((p) => p.human));
  assert.deepEqual(core.listPlayers().map((p) => p.names.join('+')),
    ['Keyboard', 'Xbox One Controller', 'DualSense Wireless Controller', 'Keyboard (arrows + numpad)']);
  assert.deepEqual(core.waitingPads(), [], 'no join hint with four playing');
});

test('drop out from the menu (a middle player) and rejoin on the same slot', () => {
  const { pads, core, step, g } = rig();
  core.keyDown('KeyJ'); step(); core.keyUp('KeyJ');
  pads.push(pad(0, 'P2 pad'), pad(1, 'P3 pad'));
  step();
  press(pads[0], START); step(); press(pads[0]); step();
  press(pads[1], START); step(); press(pads[1]); step();
  assert.equal(g.slots, 0b0111);
  core.dropOut(1);
  step();
  assert.equal(g.slots, 0b0101, 'player 2 left, player 3 stays');
  press(pads[0], A_BTN);
  const p1 = step();
  assert.equal(p1 & BT.A, 0, 'a parked pad drives nobody');
  assert.equal(g.keys[1], 0);
  press(pads[0], START); step(); press(pads[0]); step();
  assert.equal(g.slots, 0b0111);
  assert.equal(byName(core, 'P2 pad').player, 1, 'back on its own slot');
});

test('unplug: back within the grace period carries on; later it is parked', () => {
  const { pads, core, step, g } = rig();
  core.keyDown('KeyJ'); step(); core.keyUp('KeyJ');
  pads.push(pad(0, 'Wireless Controller'));
  step(); press(pads[0], START); step(); press(pads[0]); step();
  assert.equal(g.slots & 2, 2);
  const gp = pads.pop();
  step(100);
  assert.equal(g.slots & 2, 2, 'still player 2 in the grace period');
  assert.equal(core.listPlayers()[1].graceLeft > 0, true);
  pads.push(gp);
  step(KID.GRACE_FRAMES);
  assert.equal(g.slots & 2, 2, 'came back in time');
  assert.equal(byName(core, 'Wireless Controller').player, 1);
  pads.pop();
  step(KID.GRACE_FRAMES + 2);
  assert.equal(g.slots & 2, 0, 'the CPU took the Kirby');
  pads.push(gp); press(gp);
  step();
  assert.equal(byName(core, 'Wireless Controller').player, KID.PARKED);
  press(gp, START); step(); press(gp); step();
  assert.equal(g.slots & 2, 2, 'Start rejoins');
});

test('reassign, list, and a slot the story holds is skipped', () => {
  const { pads, core, step, g } = rig();
  core.keyDown('KeyJ'); step(); core.keyUp('KeyJ');
  g.joinable[1] = 0;                           // the intro has slot 1 in the hub
  pads.push(pad(0, 'Pad'));
  step(); press(pads[0], START); step(); press(pads[0]); step();
  assert.equal(byName(core, 'Pad').player, 2, 'joined on slot 2, not the held slot 1');
  const kbB = byName(core, 'Keyboard (arrows + numpad)').id;
  assert.ok(core.assign(kbB, 3));
  step();
  assert.equal(g.slots & 8, 8, 'assigning to a CPU slot asks for the join');
  assert.ok(core.assign(kbB, KID.OFF));
  core.keyDown('ArrowUp');
  assert.equal(step() & BT.UP, 0, 'an OFF device drives nobody');
  assert.equal(g.slots & 8, 0, 'taking their only device away: player 4 leaves (the CPU plays)');
  assert.equal(core.listPlayers()[3].names.length, 0);
});

test("MULTIPLAYER > LOCAL: a person with no controller gets the spare pad", () => {
  const { pads, core, step, g } = rig();
  core.keyDown('KeyJ'); step(); core.keyUp('KeyJ');
  pads.push(pad(0, 'Spare'));
  step();
  g.slots = 0b0011;                            // the game started with two humans
  step();
  assert.equal(byName(core, 'Spare').player, 1);
  press(pads[0], A_BTN); step();
  assert.equal(g.keys[1], BT.A);
});

test('a join cannot be asked for in a replay or online', () => {
  const { pads, step, g, core } = rig();
  g.canRequest = false;
  core.keyDown('KeyJ'); step(); core.keyUp('KeyJ');
  pads.push(pad(0, 'Pad'));
  step(); press(pads[0], START); step();
  assert.equal(g.requests.length, 0);
  assert.equal(g.slots, 1);
});

test('touch is a device that drives player 1 and never joins', () => {
  const { core, step, g } = rig();
  core.setTouch(BT.START);
  assert.equal(step() & BT.START, BT.START);
  assert.equal(g.requests.length, 0);
});

test('on the title and in the menus a second pad\'s Start is player 1\'s', () => {
  const { pads, core, step, g } = rig();
  g.story = false;
  core.keyDown('KeyJ'); step(); core.keyUp('KeyJ'); step();
  pads.push(pad(0, 'Pad'));
  step(); press(pads[0], START);
  assert.equal(step() & BT.START, BT.START);
  assert.equal(g.requests.length, 0);
  core.keyDown('NumpadEnter');
  assert.equal(step() & BT.START, BT.START, 'Numpad Enter too');
  assert.deepEqual(core.waitingPads(), [], 'no join hint outside play');
});

test('a pad player 1 used a moment ago is still player 1\'s: its Start pauses', () => {
  const { pads, core, step, g } = rig();
  pads.push(pad(0, 'Pad'));
  step(); press(pads[0], A_BTN); step(); press(pads[0]); step();
  core.keyDown('KeyD'); step(); core.keyUp('KeyD'); step();     // the keyboard is the last one used
  press(pads[0], START);
  assert.equal(step() & BT.START, BT.START, 'a pause, not a join');
  assert.equal(g.requests.length, 0);
  press(pads[0]); step();
  core.keyDown('KeyD'); step(); core.keyUp('KeyD'); step(KID.JOIN_QUIET + 5);   // keyboard again, pad idle
  press(pads[0], START);
  step();
  assert.equal(g.slots & 2, 2, 'a while later the same press joins');
});

test("player 1's pad is theirs once someone joins: unplugged and back, still player 1", () => {
  const { pads, core, step, g } = rig();
  pads.push(pad(0, 'P1 pad'), pad(1, 'P2 pad'));
  step(); press(pads[0], A_BTN); step(); press(pads[0]); step();
  press(pads[1], START); step(); press(pads[1]); step();
  assert.equal(g.slots & 2, 2);
  assert.equal(byName(core, 'P1 pad').player, 0, 'claimed by player 1');
  const gp = pads.shift();
  step(10);
  pads.unshift(gp);
  step();
  press(gp, START);
  assert.equal(step() & BT.START, BT.START, "player 1's Start pauses, it does not join");
  assert.equal(g.slots, 0b0011);
});

test('Select held two seconds leaves; the pad is parked and Start rejoins', () => {
  const { pads, core, step, g, events } = rig();
  core.keyDown('KeyJ'); step(); core.keyUp('KeyJ');
  pads.push(pad(0, 'Pad'));
  step(); press(pads[0], START); step(); press(pads[0]); step();
  assert.equal(g.slots & 2, 2);
  press(pads[0], 8);
  step(KID.HOLD_LEAVE_FRAMES - 5);
  assert.equal(g.slots & 2, 2, 'not yet');
  step(10);
  assert.equal(g.slots & 2, 0, 'left');
  assert.equal(byName(core, 'Pad').player, KID.PARKED);
  assert.ok(events.some((e) => e.type === 'leave' && e.player === 1 && e.why === 'select'));
  press(pads[0]); step();
  assert.deepEqual(core.waitingPads(), [byName(core, 'Pad').id], 'the hint shows the parked pad');
  press(pads[0], START); step(); press(pads[0]); step();
  assert.equal(g.slots & 2, 2);
});

test('the PLAYERS page: moving a pad from player 2 to player 3 is a leave and a join', () => {
  const { pads, core, step, g } = rig();
  core.keyDown('KeyJ'); step(); core.keyUp('KeyJ');
  pads.push(pad(0, 'Pad'));
  step(); press(pads[0], START); step(); press(pads[0]); step();
  assert.equal(g.slots, 0b0011);
  assert.ok(core.assign(byName(core, 'Pad').id, 2));
  step(2);
  assert.equal(g.slots, 0b0101, 'player 2 left, player 3 joined');
  press(pads[0], A_BTN); step();
  assert.equal(g.keys[2], BT.A);
});

test('events: join, unplugged, back, leave', () => {
  const { pads, core, step, events } = rig();
  core.keyDown('KeyJ'); step(); core.keyUp('KeyJ');
  pads.push(pad(0, 'Pad'));
  step(); press(pads[0], START); step(); press(pads[0]); step();
  const gp = pads.pop(); step(5); pads.push(gp); step();
  pads.pop(); step(KID.GRACE_FRAMES + 2);
  const types = events.filter((e) => e.player === 1).map((e) => e.type);
  assert.deepEqual(types, ['join', 'unplugged', 'back', 'unplugged', 'leave']);
  assert.equal(events.find((e) => e.type === 'join').label, 'PAD 1');
});

test('online: every device drives this device\'s player, nobody joins, host keys untouched', () => {
  const { pads, core, step, g } = rig();
  core.keyDown('KeyJ'); step(); core.keyUp('KeyJ');
  pads.push(pad(0, 'Pad'));
  step(); press(pads[0], START); step(); press(pads[0]); step();
  assert.equal(g.slots & 2, 2);
  core.setOnline(true);
  g.keys = [0, 7, 7, 7];
  press(pads[0], RIGHT_BTN);
  assert.equal(step() & BT.RIGHT, BT.RIGHT, 'a joined pad drives this device\'s player online');
  assert.deepEqual(g.keys, [0, 7, 7, 7], 'players 2-4 are the session\'s');
  pads.push(pad(1, 'Pad B'));
  step(); press(pads[1], START);
  assert.equal(step() & BT.START, BT.START);
  assert.equal(g.requests.length, 1);
});

test('the page menu open: the game sees nothing, nothing joins', () => {
  const { pads, core, step, g } = rig();
  core.keyDown('KeyJ'); step(); core.keyUp('KeyJ');
  pads.push(pad(0, 'Pad'));
  step();
  core.muted = true;
  press(pads[0], START);
  assert.equal(step(), 0);
  assert.equal(g.requests.length, 0);
});

test('a pad without the standard mapping: hat axes, Start on 9 or 7, motion sensors ignored', () => {
  const gp = { index: 0, id: 'Generic USB Joystick', mapping: '', connected: true,
               buttons: Array.from({ length: 12 }, () => ({ pressed: false, value: 0 })), axes: [0, 0, 0, 0, 0, 0, 0, 0] };
  gp.axes[6] = 1; gp.axes[7] = -1; gp.buttons[9].pressed = true;
  assert.equal(KID.padMask(gp), BT.RIGHT | BT.UP | BT.START);
  const eight = { mapping: '', buttons: Array.from({ length: 8 }, () => ({ pressed: false, value: 0 })), axes: [0, 0] };
  eight.buttons[7].pressed = true; eight.buttons[6].pressed = true;
  assert.equal(KID.padMask(eight), BT.START | BT.SELECT);
  const hat = { mapping: '', buttons: Array.from({ length: 12 }, () => ({ pressed: false, value: 0 })),
                axes: [0, 0, 0, 0, 0, 0, 0, 0, 0, -1 / 7] };    // a hat as one axis: down-right
  const st = {};
  assert.equal(KID.padMask(hat, st), 0, 'not a hat until it has been seen centred');
  hat.axes[9] = 9 / 7;                                              // centred
  assert.equal(KID.padMask(hat, st), 0);
  hat.axes[9] = -1 / 7;
  assert.equal(KID.padMask(hat, st), BT.DOWN | BT.RIGHT);
  hat.axes[9] = -1;
  assert.equal(KID.padMask(hat, st), BT.UP);
  assert.equal(KID.padName('Xbox 360 Controller (XInput STANDARD GAMEPAD)'), 'Xbox 360 Controller');
  assert.equal(KID.padName('054c-0ce6-Sony Interactive Entertainment Wireless Controller'),
    'Sony Interactive Entertainment Wireless Controller');
  const { pads, core, step } = rig();
  pads.push({ index: 0, id: 'Motion Sensors', connected: true, buttons: [], axes: [0, 0, 0] });
  step();
  assert.equal(core.listDevices().filter((d) => d.kind === KID.KIND.PAD).length, 0);
});

test('two identical pads: numbered, and each comes back as itself', () => {
  const { pads, core, step } = rig();
  pads.push(pad(0, 'Xbox 360 Controller (XInput STANDARD GAMEPAD)'), pad(1, 'Xbox 360 Controller (XInput STANDARD GAMEPAD)'));
  step();
  const list = core.listDevices().filter((d) => d.kind === KID.KIND.PAD);
  assert.deepEqual(list.map((d) => d.label), ['PAD 1', 'PAD 2']);
  pads.pop(); step(); pads.push(pad(1, 'Xbox 360 Controller (XInput STANDARD GAMEPAD)')); step();
  assert.deepEqual(core.listDevices().filter((d) => d.kind === KID.KIND.PAD).map((d) => d.label), ['PAD 1', 'PAD 2']);
});

console.log(`${passed} tests passed`);
