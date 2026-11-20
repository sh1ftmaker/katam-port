// input_remap_test.mjs -- web/input_remap.js (SETTINGS > PLAYERS > CONTROLS)
// with web/input_devices.js, against fake keys, gamepads and storage.
//
//   node tools/input_remap_test.mjs        (well under a second; no wasm)
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const KID = require('../web/input_devices.js');
const KR = require('../web/input_remap.js');
const BT = KID.BUTTONS;
const ROW = KR.ROW;
const row = (id) => ROW.ACT0 + KR.ACTIONS.findIndex((a) => a.id === id);

function store() { const m = {}; return { m, getItem: (k) => (k in m ? m[k] : null), setItem: (k, v) => { m[k] = String(v); } }; }
function pad(index, id, mapping = 'standard') {
  return { index, id, mapping, connected: true, buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })), axes: [0, 0, 0, 0] };
}
function press(gp, ...idx) { gp.buttons.forEach((b) => { b.pressed = false; b.value = 0; }); for (const i of idx) { gp.buttons[i].pressed = true; gp.buttons[i].value = 1; } }
function rig(st = store(), actions = []) {
  const pads = [];
  const sink = { inStory: () => true, slotIsHuman: (s) => (s === 0 ? 1 : 0), request: () => 0, canRequest: () => false,
                 hostKeys: () => {}, slotJoinable: () => 1 };
  const devs = new KID({ sink, getGamepads: () => pads.slice() });
  const r = KR.install(devs, null, { storage: st, onAction: (n) => actions.push(n) });
  return { devs, r, pads, st, actions, step: (n = 1) => { let m = 0; for (let i = 0; i < n; i++) m = devs.poll(); return m; } };
}
let passed = 0;
function test(name, fn) { fn(); passed++; console.log('ok', name); }

test('the defaults are exactly input_devices.js (keys and padMask)', () => {
  const { r } = rig();
  let seed = 7;
  const rnd = () => { seed = (seed * 1103515245 + 12345) >>> 0; return seed / 4294967296; };
  for (const mapping of ['standard', '']) {
    for (const nb of [17, 8, 10]) {
      for (let n = 0; n < 400; n++) {
        const gp = pad(0, 'Generic USB Joystick', mapping);
        gp.buttons.length = nb;
        gp.buttons.forEach((b) => { if (rnd() < 0.2) { b.pressed = true; b.value = 1; } });
        gp.axes = [rnd() * 2 - 1, rnd() * 2 - 1, 0, 0];
        assert.equal(r.padMask(gp, {}) & 0x3FF, KID.padMask(gp, {}), `${mapping} ${nb}`);
      }
    }
  }
  for (const code of Object.keys(KID.KEYS_A)) assert.equal(r.keyMask('a', { [code]: 1 }), KID.KEYS_A[code], code);
  for (const code of Object.keys(KID.KEYS_B)) assert.equal(r.keyMask('b', { [code]: 1 }), KID.KEYS_B[code], code);
});

test('pad families: names and the Switch Pro\'s Nintendo layout', () => {
  assert.equal(KR.padFamily('Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e)'), 'xbox');
  assert.equal(KR.padFamily('DualSense Wireless Controller (Vendor: 054c Product: 0ce6)'), 'ps');
  assert.equal(KR.padFamily('Pro Controller (STANDARD GAMEPAD Vendor: 057e Product: 2009)'), 'switch');
  assert.equal(KR.padInputName('b0', 'ps', true), 'CROSS');
  assert.equal(KR.padInputName('a1+', 'xbox', true), 'LS DN');
  const { r } = rig();
  const gp = pad(0, 'Pro Controller (STANDARD GAMEPAD Vendor: 057e Product: 2009)');
  press(gp, 1); assert.equal(r.padMask(gp, {}), BT.A, 'Switch A (right) is GBA A');
  press(gp, 0); assert.equal(r.padMask(gp, {}), BT.B, 'Switch B (bottom) is GBA B');
});

test('press to bind a key: repeats ignored, the game sees nothing until let go', () => {
  const t = rig();
  t.step();
  t.devs.keyDown('KeyJ'); assert.equal(t.step(), BT.A);
  t.r.rowStep(row('A'), 1);                     // A on the row (J still held)
  assert.ok(t.r.capturing());
  assert.equal(t.r.rowValue(row('A')), 'PRESS A KEY');
  assert.equal(t.step(), 0, 'nothing while waiting');
  t.devs.keyDown('KeyJ', true);                // auto-repeat of the held J
  assert.ok(t.r.capturing());
  t.devs.keyDown('KeyP'); t.step();
  assert.ok(!t.r.capturing());
  assert.equal(t.r.rowValue(row('A')), 'P');
  t.devs.keyUp('KeyJ'); assert.equal(t.step(), 0, 'P still down: settling');
  t.devs.keyUp('KeyP'); t.step(2);
  t.devs.keyDown('KeyP'); assert.equal(t.step(), BT.A);
  t.devs.keyUp('KeyP'); t.devs.keyDown('KeyJ'); assert.equal(t.step(), 0, 'J is nothing now');
  t.devs.keyUp('KeyJ');
  t.r.rowStep(row('A'), -1);                    // Left: this action's default
  t.devs.keyDown('KeyZ'); assert.equal(t.step(), BT.A);
});

test('conflicts are shown; Escape and the clock cancel', () => {
  const t = rig();
  t.r.rowStep(row('B'), 1); t.devs.keyDown('KeyJ'); t.devs.keyUp('KeyJ'); t.step(2);
  assert.match(t.r.rowValue(row('B')), /^! J/);
  assert.equal(t.r.rowDesc(row('B')), 'ALSO ON A');
  t.r.rowStep(row('L'), 1); t.devs.keyDown('Escape'); t.step();
  assert.ok(!t.r.capturing()); assert.equal(t.r.rowValue(row('L')), 'Q');
  t.r.rowStep(row('L'), 1); t.step(KR.CAPTURE_FRAMES + 1);
  assert.ok(!t.r.capturing());
});

test('a pad: its own profile, saved per pad id, reset to default', () => {
  const st = store();
  const t = rig(st);
  const gp = pad(0, 'Xbox 360 Controller (XInput STANDARD GAMEPAD)');
  t.pads.push(gp); t.step();
  t.r.rowStep(ROW.DEVICE, 1); t.r.rowStep(ROW.DEVICE, 1);   // KEYBOARD > KEYS 2 > PAD 1
  assert.equal(t.r.rowValue(ROW.DEVICE), 'PAD 1 XBOX');
  press(gp, 0); t.step();                                    // A held as the row is pressed
  t.r.rowStep(row('START'), 1); t.step();
  press(gp, 0, 10); t.step();                                // LS (stick click): the new START
  assert.ok(!t.r.capturing());
  assert.equal(t.r.rowValue(row('START')), 'LS');
  assert.equal(t.step(), 0, 'held: settling');
  press(gp); t.step(); press(gp, 10);
  assert.equal(t.step(), BT.START);
  // another page load: the same
  const u = rig(st); const gp2 = pad(0, gp.id); u.pads.push(gp2); press(gp2, 10);
  assert.equal(u.step(), BT.START);
  press(gp2, 9); assert.equal(u.step(), 0, 'MENU is not START now');
  u.r.rowStep(ROW.DEVICE, -1);                               // KEYBOARD < PAD 1 (wraps)
  assert.equal(u.r.rowValue(ROW.DEVICE), 'PAD 1 XBOX');
  u.r.rowStep(ROW.RESET, 1);
  assert.equal(u.step(), BT.START, 'reset: MENU is START again');
  assert.equal(u.r.rowValue(row('START')), 'MENU');
});

test('FULLSCREEN / MUTE: once per press; never while binding', () => {
  const t = rig();
  t.devs.keyDown('KeyF'); t.step(3); t.devs.keyUp('KeyF'); t.step();
  assert.deepEqual(t.actions, ['fullscreen']);
  t.r.rowStep(row('MUTE'), 1); t.devs.keyDown('KeyN'); t.step(); t.devs.keyUp('KeyN'); t.step();
  t.devs.keyDown('KeyN'); t.step();
  assert.deepEqual(t.actions, ['fullscreen', 'mute']);
});

test('rumble: hurt and a new ability, only when on', () => {
  const t = rig();
  const fx = [];
  const gp = pad(0, 'Xbox'); gp.vibrationActuator = { type: 'dual-rumble', playEffect: (k, o) => fx.push(o.duration) };
  let hp = 6, ab = 0;
  const probe = (p) => (p === 0 ? hp | (ab << 8) : -1);
  const go = () => t.r.watch(probe, () => [gp], true);
  go(); hp = 5; go(); ab = 3; go(); go();
  assert.deepEqual(fx, [220, 110]);
  t.r.rowStep(ROW.RUMBLE, 1); assert.equal(t.r.rowValue(ROW.RUMBLE), 'OFF');
  hp = 4; go(); assert.equal(fx.length, 2);
  assert.equal(rig(t.st).r.rumbleOn, false, 'saved');
});

test('the menu bridge: queries and steps', () => {
  const t = rig();
  const M = { portUiQuery: (k) => (k === 'touch' ? '1' : ''), portUiAction: (n) => n === 'touch' };
  KR.hookMenu(M, t.r);
  assert.equal(M.portUiQuery('touch'), '1');
  assert.equal(M.portUiQuery('remap.v.' + row('START')), 'ENTER');
  const g = M.portUiQuery('remap.gen');
  assert.ok(M.portUiAction('remap.step', ROW.DEVICE | 256));
  assert.equal(M.portUiQuery('remap.v.0'), 'KEYS 2');
  assert.notEqual(M.portUiQuery('remap.gen'), g);
  assert.ok(M.portUiAction('touch'));
  assert.equal(KR.ROWS, 15, 'platform/controls_ui.c CR_ROWS');
});

console.log(`${passed} tests passed`);
