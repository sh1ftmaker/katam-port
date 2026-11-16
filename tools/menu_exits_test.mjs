// Every menu has a way out: the game's front-end menus, the SETTINGS
// sections and their NO / YES screens, the ONLINE screens, the PLAYER CARD
// (typing finished and cancelled), the PLAYERS page, and the pause menu's
// pages alone and in local multiplayer -- B goes back one level, Start
// closes the pause from any page or sub-page, Select turns the pause's
// pages; MAIN MENU leaves to the title without a reload, twice, with a new
// game between.  (The online side -- as host and guest, LEAVE / END GAME, a
// host leaving, REMOVE -- is tools/online_browser_test.mjs --cases exits.)
//
//   node tools/menu_exits_test.mjs --cdp 19371 --base http://127.0.0.1:18961/dist/
//        --rom-url http://127.0.0.1:18961/rom.gba --shots DIR
//        [--cases front,joinmsg,cards,pause,local,phone,wide,kirbys] [--relay http://127.0.0.1:18821]
//
// Needs a Chromium already listening on --cdp with its own profile (never the
// shared one), a static server for the dist and a ROM you own (served from
// outside the tree), and for `joinmsg` and `cards` a room server
// (netplay/dev-relay.mjs).  `cards`: an invite link's first screen (the
// visitor's player card: B to ONLINE, A joins), and SETTINGS > PLAYERS >
// PLAYER CARD on the pause page (the card kept, the Kirby wearing it).
// Screenshots are ROM-derived: write them outside the tree.  At the end it
// prints the exits table it walked.
import fs from 'fs';
import path from 'path';

const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf('--' + n); return i >= 0 ? argv[i + 1] : d; };
const CDP_PORT = +flag('cdp', 9363);
const BASE = flag('base', 'http://127.0.0.1:8963/dist/');
// --mod 4 | 16 | default: the module every page loads unless a case names its
// own (extra `mod`; null = none, the page's default).  4 (?mod=4, the 4-Kirby
// module) unless given: the 16-Kirby module is the page's default, so a suite
// meant for it says --mod 16 or default (tools/full_check.sh).
const MOD = flag('mod', '4');
const ROM_URL = flag('rom-url', 'http://127.0.0.1:8963/rom.gba');
const SHOTS = flag('shots', '');
const CASES = flag('cases', 'front,joinmsg,pause,local,phone,wide').split(',');
const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let pass = 0, fail = 0;
const ok = (name, c, extra) => {
    if (c) { pass++; console.log('  ok   ' + name + (extra ? '  (' + extra + ')' : '')); }
    else { fail++; console.log('  FAIL ' + name + (extra ? '  (' + extra + ')' : '')); }
    return c;
};
const T0 = Date.now();
const say = (t) => console.log(`[${((Date.now() - T0) / 1000).toFixed(0)}s] ${t}`);

class CDP {
    static async connect(url) {
        const ws = new WebSocket(url);
        await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
        return new CDP(ws);
    }
    constructor(ws) {
        this.ws = ws; this.id = 0; this.wait = new Map(); this.handlers = [];
        ws.onmessage = (e) => {
            const m = JSON.parse(e.data);
            if (m.id && this.wait.has(m.id)) {
                const { res, rej } = this.wait.get(m.id); this.wait.delete(m.id);
                m.error ? rej(new Error(JSON.stringify(m.error))) : res(m.result);
            } else for (const h of this.handlers) h(m);
        };
    }
    send(method, params = {}, sessionId) {
        const id = ++this.id;
        this.ws.send(JSON.stringify({ id, method, params, sessionId }));
        return new Promise((res, rej) => this.wait.set(id, { res, rej }));
    }
}

const ver = await (await fetch(`http://127.0.0.1:${CDP_PORT}/json/version`)).json();
const cdp = await CDP.connect(ver.webSocketDebuggerUrl);
const pages = {};
const logs = {};
cdp.handlers.push((m) => {
    const who = Object.keys(pages).find((k) => pages[k].sessionId === m.sessionId) || '?';
    if (m.method === 'Runtime.consoleAPICalled') {
        const t = m.params.args.map((a) => a.value ?? a.description ?? '').join(' ');
        (logs[who] = logs[who] || []).push(t);
        if (/\[devices\]|localmp:|exception|abort/i.test(t)) console.log(`    ${who}: ${t.slice(0, 200)}`);
    }
    if (m.method === 'Runtime.exceptionThrown')
        console.log(`    ${who} exception: ` + JSON.stringify(m.params.exceptionDetails).slice(0, 400));
});

// Fake gamepads, in place before the page's own scripts.
const PAD_HOOK = `(function(){
  var pads = [null, null, null, null, null];
  window.__pads = pads;
  function mk(i, id) {
    var b = [];
    for (var k = 0; k < 17; k++) b.push({ pressed: false, touched: false, value: 0 });
    return { index: i, id: id, connected: true, mapping: 'standard', buttons: b, axes: [0, 0, 0, 0],
             timestamp: performance.now() };
  }
  Object.defineProperty(Navigator.prototype, 'getGamepads', { configurable: true, value: function () { return pads.slice(); } });
  function fire(type, g) { var e = new Event(type); e.gamepad = g; window.dispatchEvent(e); }
  window.__padPlug = function (i, id) { pads[i] = mk(i, id); fire('gamepadconnected', pads[i]); };
  window.__padUnplug = function (i) { var g = pads[i]; if (!g) return; g.connected = false; pads[i] = null; fire('gamepaddisconnected', g); };
  window.__padSet = function (i, btns, axes) {
    var g = pads[i]; if (!g) return;
    g.buttons.forEach(function (b, k) { var on = btns.indexOf(k) >= 0; b.pressed = on; b.value = on ? 1 : 0; });
    if (axes) g.axes = axes;
    g.timestamp = performance.now();
  };
})();`;

async function openPage(name, { w = 960, h = 640, mobile = false, touch = false, extra = {}, early = '' } = {}) {
    const { browserContextId } = await cdp.send('Target.createBrowserContext');
    const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank', browserContextId, newWindow: true });
    const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
    const s = (m, p) => cdp.send(m, p, sessionId);
    const p = { name, s, targetId, sessionId,
        ev: async (expr) => {
            const r = await s('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
            if (r.exceptionDetails) throw new Error(expr.slice(0, 80) + ': ' + JSON.stringify(r.exceptionDetails).slice(0, 300));
            return r.result.value;
        },
        close: async () => { try { await cdp.send('Target.closeTarget', { targetId }); } catch (e) { /* gone */ }
                             try { await cdp.send('Target.disposeBrowserContext', { browserContextId }); } catch (e) { /* gone */ } },
    };
    pages[name] = p;
    await s('Page.enable'); await s('Runtime.enable');
    await s('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: mobile ? 2 : 1, mobile });
    if (touch) {
        await s('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 5 });
        await s('Emulation.setEmitTouchEventsForMouse', { enabled: true, configuration: 'mobile' });
    }
    await s('Page.addScriptToEvaluateOnNewDocument', { source: PAD_HOOK });
    if (early) await s('Page.addScriptToEvaluateOnNewDocument', { source: early });
    const u = new URL(BASE + 'index.html');
    u.searchParams.set('rom', ROM_URL);
    const ex = Object.assign(MOD === 'default' ? {} : { mod: MOD }, extra);
    for (const [k, v] of Object.entries(ex)) if (v !== null && v !== undefined) u.searchParams.set(k, v);
    await s('Page.navigate', { url: u.toString() });
    return p;
}

async function until(fn, ms, step = 100) {
    const t = Date.now();
    for (;;) { let v; try { v = await fn(); } catch (e) { v = false; } if (v) return v; if (Date.now() - t > ms) return false; await sleep(step); }
}
async function shot(p, name) {
    if (!SHOTS) return;
    const r = await p.s('Page.captureScreenshot', { format: 'png' });
    fs.mkdirSync(SHOTS, { recursive: true });
    fs.writeFileSync(path.join(SHOTS, name), Buffer.from(r.data, 'base64'));
    console.log('    shot ' + path.join(SHOTS, name));
}
// Player 1's keys: keyboard set A only (WASD, Z/X, Enter, Backspace), so the
// arrows -- keyboard set B, a device of its own -- stay out of it.
const KEYS = { A: ['KeyZ', 'z', 90], B: ['KeyX', 'x', 88], START: ['Enter', 'Enter', 13], SELECT: ['Backspace', 'Backspace', 8],
               UP: ['KeyW', 'w', 87], DOWN: ['KeyS', 's', 83], LEFT: ['KeyA', 'a', 65], RIGHT: ['KeyD', 'd', 68],
               NUMENTER: ['NumpadEnter', 'Enter', 13],
               /* keyboard set B's arrows: on the player card's NAME row the
                * letters type (WASD, X too), the arrows still move */
               ARROWDOWN: ['ArrowDown', 'ArrowDown', 40] };
async function press(p, k, holdMs = 90, gapMs = 160) {
    const [code, key, vk] = KEYS[k];
    await p.s('Input.dispatchKeyEvent', { type: 'keyDown', code, key, windowsVirtualKeyCode: vk });
    await sleep(holdMs);
    await p.s('Input.dispatchKeyEvent', { type: 'keyUp', code, key, windowsVirtualKeyCode: vk });
    await sleep(gapMs);
}
async function keyHold(p, k, down) {
    const [code, key, vk] = KEYS[k];
    await p.s('Input.dispatchKeyEvent', { type: down ? 'keyDown' : 'keyUp', code, key, windowsVirtualKeyCode: vk });
}
const PB = { A: 0, B: 2, SELECT: 8, START: 9, UP: 12, DOWN: 13, LEFT: 14, RIGHT: 15 };
async function padPress(p, i, btn, holdMs = 120, gapMs = 150) {
    await p.ev(`__padSet(${i}, [${PB[btn]}])`);
    await sleep(holdMs);
    await p.ev(`__padSet(${i}, [])`);
    await sleep(gapMs);
}
const frame = (p) => p.ev('Module._PortFrameNumber()');
const waitFrames = async (p, n) => { const f0 = await frame(p); await until(async () => (await frame(p)) >= f0 + n, 60000, 30); };
const human = (p, s) => p.ev(`Module._PortLmpSlotIsHuman(${s})`);
const humans = (p) => p.ev('[0,1,2,3].map(function(s){return Module._PortLmpSlotIsHuman(s)})');
const devList = (p) => p.ev('katamDevices.listDevices()');
const notices = (p) => p.ev('Array.from(document.querySelectorAll("#pnotices .pnotice")).map(function(e){return e.textContent})');
const hint = (p) => p.ev('(function(){var h=document.getElementById("pjoinhint");return h&&h.style.opacity==="1"?h.textContent:""})()');
const pagesModel = (p) => p.ev('Module._PortPocScreenModel()');
const screenLive = async (p) => (await frame(p)) - (await p.ev('Module._PortPocScreenLastDraw()')) < 5;

/* power-on to Game Select, FILE 1 (a fresh profile: an empty file) */
async function toGameSelect(p) {
    await until(() => p.ev('!!(window.Module && Module.portRuntimeUp && Module._PortFrameNumber() > 30)'), 60000);
    await until(async () => (await frame(p)) >= 400, 60000, 30);
    await press(p, 'START'); await waitFrames(p, 100);     // the intro -> the title
    await press(p, 'START'); await waitFrames(p, 100);     // the title -> file select
    await press(p, 'A'); await waitFrames(p, 100);         // FILE 1 -> Game Select
}

/* Game Select > START GAME > ONE PLAYER, then into ordinary play: player 1
 * warped to a level room, the intro's hold on the other Kirbys lifted (as
 * tools/netaudit/make_players_recordings.py does) -- unless `opening`: the
 * fresh file's story stage left as it is (the CPU KIRBYS case: the count
 * changes in the opening too, platform/wide.c OpeningHolds) */
async function startPlay(p, opening = false) {
    await press(p, 'A'); await waitFrames(p, 60);          // the pop-up
    await press(p, 'A'); await waitFrames(p, 60);          // ONE PLAYER
    await until(() => p.ev('Module._PortLmpInStory()'), 60000, 100);
    await waitFrames(p, 200);
    await p.ev('Module._PortWarp(0, 117, 44, 9)');
    await waitFrames(p, 120);
    if (opening) return;
    // gAIKirbyState (0x0203AD2C, u16) = NORMAL (300): the CPUs are partners
    await p.ev('(function(){var a=0x0203AD2C;Module.HEAPU8[a]=300&255;Module.HEAPU8[a+1]=300>>8;})()');
    await waitFrames(p, 60);
}

const XBOX = 'Xbox 360 Controller (XInput STANDARD GAMEPAD)';
const XONE = 'Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 0b13)';
const M = { BROWSE: 17, PASS: 18, LIGHTING: 19, EXPERIMENTAL: 20, TOP: 0, MP: 1, ONLINE: 2, HOST: 3, JOIN: 4, PLAYERS: 5, CARD: 6, PICK: 7,
            PAUSE_TOP: 9, DISPLAY: 10, SOUND: 11, SEC_PLAYERS: 12, SEC_ONLINE: 13, GAME: 14, ADVANCED: 15, CONFIRM: 16 };
/* the top level's sections, in order (LIGHTING is EXPERIMENTAL's first row) */
const SECTIONS = ['DISPLAY', 'SOUND', 'SEC_PLAYERS', 'SEC_ONLINE', 'GAME', 'ADVANCED', 'EXPERIMENTAL'];
const ouiScreen = (p) => p.ev('Module._PortOuiScreen()');
/* the pause screen (PortLnObserve works outside a session too): paused, its page */
const pauseState = (p) => p.ev(`(function(){var q=Module._malloc(64);Module._PortLnObserve(q);
    var r={paused:Module.HEAP32[q>>2],menu:Module.HEAP32[(q>>2)+2]};Module._free(q);return r})()`);
const MENU = { HELP: 1, WORLD: 2, AREA: 4, SETTINGS: 8 };
const TABLE = [];
const row = (screen, state, b, start, select) => TABLE.push({ screen, state, b, start, select });
async function step(p, k, n = 20) { await press(p, k); await waitFrames(p, n); }
const settingsWord = (p) => p.ev('Module._PortPocSettingsWord() >>> 0');
const volume = async (p) => ((await settingsWord(p)) >>> 10) & 127;
const lightOn = async (p) => (await p.ev('Module._PortPocSettingsExtWord() >>> 0')) & 1;
/* EXPERIMENTAL > CPU KIRBYS as saved: the Kirbys in all ("kirbys=N") */
const kirbysSetting = (p) => p.ev(`(function(){var m=/(?:^|\\n)kirbys=(\\d+)/.exec(localStorage.getItem('katam.settings')||'');return m?+m[1]:4})()`);

// --------------------------------------------------------------------------
/* Front end: SETTINGS, each section and the NO / YES screen; MULTIPLAYER,
 * ONLINE, HOST GAME, JOIN GAME (typing, a message), the PLAYER CARD (typing
 * finished and cancelled, the colour picker); the PLAYERS page and a card
 * from it.  B goes back one level everywhere; Start is A on these screens
 * (the game's own rule, sub_0813B484); Select does nothing here, as on
 * every screen of the game's front end. */
async function caseFront() {
    say('front: every front-end screen B goes back from');
    const p = await openPage('front');
    await toGameSelect(p);
    for (let i = 0; i < 4; i++) await step(p, 'DOWN', 8);
    await press(p, 'A');
    await until(() => screenLive(p), 10000);
    await waitFrames(p, 30);
    ok('Game Select > SETTINGS: the top level', (await pagesModel(p)) === M.TOP);
    await shot(p, 'front-settings-top.png');
    for (let i = 0; i < SECTIONS.length; i++) {
        await step(p, 'A');
        ok(`A opens ${SECTIONS[i]}`, (await pagesModel(p)) === M[SECTIONS[i]]);
        await shot(p, `front-section-${i}-${SECTIONS[i].toLowerCase()}.png`);
        await step(p, 'B');
        ok(`B from ${SECTIONS[i]}: SETTINGS`, (await pagesModel(p)) === M.TOP);
        await step(p, 'DOWN', 10);
    }
    row('Front SETTINGS > each section', 'rows', 'back to SETTINGS on its row', '= A', 'nothing (game rule)');
    // EXPERIMENTAL > LIGHTING: off by default; LIGHTING Right turns it on,
    // every row then works
    await step(p, 'UP', 10); await step(p, 'A');                 // (wrapped from DISPLAY) -> EXPERIMENTAL
    ok('EXPERIMENTAL section', (await pagesModel(p)) === M.EXPERIMENTAL);
    await shot(p, 'front-experimental.png');
    await step(p, 'A');                                          // its first row: LIGHTING
    ok('EXPERIMENTAL > LIGHTING section; lighting off by default', (await pagesModel(p)) === M.LIGHTING && (await lightOn(p)) === 0);
    await step(p, 'RIGHT', 10);
    ok('LIGHTING Right: on', (await lightOn(p)) === 1);
    await shot(p, 'front-lighting-on.png');
    await step(p, 'B');
    ok('B from LIGHTING: EXPERIMENTAL', (await pagesModel(p)) === M.EXPERIMENTAL);
    // CPU KIRBYS on the ordinary module: Right asks first (the page would
    // restart with the 16-Kirby module); B there keeps 4 Kirbys
    await step(p, 'DOWN', 10); await step(p, 'RIGHT', 10);
    ok('CPU KIRBYS Right asks first (NO / YES)', (await pagesModel(p)) === M.CONFIRM);
    await shot(p, 'front-kirbys-confirm.png');
    await step(p, 'B');
    ok('B on that NO / YES: EXPERIMENTAL, still 4 Kirbys', (await pagesModel(p)) === M.EXPERIMENTAL && (await kirbysSetting(p)) === 4);
    await step(p, 'B'); await step(p, 'DOWN', 10);               // SETTINGS on EXPERIMENTAL -> (wraps) DISPLAY
    // ADVANCED > DEFAULTS: NO / YES
    for (let i = 0; i < 5; i++) await step(p, 'DOWN', 8);      // DISPLAY -> ADVANCED
    await step(p, 'A');
    ok('ADVANCED is up', (await pagesModel(p)) === M.ADVANCED);
    await step(p, 'DOWN', 8); await step(p, 'DOWN', 8);        // DEFAULTS
    await step(p, 'A');
    ok('A on DEFAULTS asks (NO / YES)', (await pagesModel(p)) === M.CONFIRM);
    await shot(p, 'front-confirm-defaults.png');
    await step(p, 'B');
    ok('B on NO / YES: back to ADVANCED', (await pagesModel(p)) === M.ADVANCED);
    row('Front NO / YES (DEFAULTS)', 'asking', 'back (= NO)', '= A', 'nothing');
    // a setting changed, then DEFAULTS puts it back
    await step(p, 'B'); for (let i = 0; i < 4; i++) await step(p, 'UP', 8);   // SOUND
    await step(p, 'A'); const v0 = await volume(p);
    await step(p, 'LEFT', 10);
    ok('SOUND > VOLUME: Left turns it down', (await volume(p)) === Math.max(0, v0 - 10), `${v0} -> ${await volume(p)}`);
    await step(p, 'B'); for (let i = 0; i < 4; i++) await step(p, 'DOWN', 8);  // ADVANCED
    await step(p, 'A'); await step(p, 'DOWN', 8); await step(p, 'DOWN', 8);
    await step(p, 'A'); await step(p, 'DOWN', 8); await step(p, 'A', 30);      // YES
    ok('DEFAULTS > YES: the volume is the default again, back on ADVANCED', (await volume(p)) === 100 && (await pagesModel(p)) === M.ADVANCED,
       String(await volume(p)));
    ok('DEFAULTS > YES: lighting off again', (await lightOn(p)) === 0);
    // the gesture rows: FULLSCREEN by the keyboard (now) and by a pad (asks)
    await step(p, 'B'); await step(p, 'DOWN', 8); await step(p, 'DOWN', 8);   // EXPERIMENTAL, wraps to DISPLAY
    await step(p, 'A');
    for (let i = 0; i < 11; i++) await step(p, 'DOWN', 6);     // FULLSCREEN
    await shot(p, 'front-display-fullscreen-row.png');
    await p.ev(`__padPlug(0, ${JSON.stringify(XBOX)})`); await waitFrames(p, 10);
    await sleep(5500);                  // the last key's user activation runs out
    await padPress(p, 0, 'A'); await sleep(1600);
    ok('FULLSCREEN by a gamepad: the page asks for a click or tap', await p.ev('!document.getElementById("gesture").hidden'));
    await shot(p, 'front-gesture-prompt.png');
    await p.ev('document.getElementById("gesture").click()'); await sleep(200);
    ok('... and the click does it (the prompt goes)', await p.ev('document.getElementById("gesture").hidden'));
    await p.ev('document.fullscreenElement && document.exitFullscreen()'); await sleep(300);
    await press(p, 'A'); await sleep(600);
    ok('FULLSCREEN by the keyboard: no prompt', await p.ev('document.getElementById("gesture").hidden'));
    await p.ev('document.fullscreenElement && document.exitFullscreen()'); await sleep(300);
    await step(p, 'B'); await step(p, 'B', 40);
    ok('B on SETTINGS: back to Game Select', !(await screenLive(p)));
    row('Front SETTINGS', 'top level', 'Game Select', '= A', 'nothing');

    // MULTIPLAYER and the ONLINE screens
    for (let i = 0; i < 1; i++) await step(p, 'DOWN', 8);      // SETTINGS -> START GAME (wraps)
    await step(p, 'A', 60);                                     // the pop-up
    await step(p, 'DOWN'); await step(p, 'A', 80);             // MULTIPLAYER
    ok('MULTIPLAYER is up', (await pagesModel(p)) === M.MP && await screenLive(p));
    await step(p, 'DOWN'); await step(p, 'A', 30);             // ONLINE
    ok('ONLINE is up', (await pagesModel(p)) === M.ONLINE);
    await step(p, 'A', 30);                                     // HOST GAME
    ok('HOST GAME is up', (await pagesModel(p)) === M.HOST);
    await waitFrames(p, 60);
    await step(p, 'DOWN', 10);
    ok('HOST GAME: FREE ROAM is the START row\'s default', (await p.ev('Module._PortOuiRoomsMode()')) === 1);
    await step(p, 'B', 30);
    ok('B on HOST GAME: ONLINE', (await pagesModel(p)) === M.ONLINE);
    row('HOST GAME', 'waiting / start row', 'ONLINE (room closed)', '= A', 'nothing');
    await step(p, 'DOWN'); await step(p, 'A', 30);             // JOIN GAME
    ok('JOIN GAME is up', (await pagesModel(p)) === M.JOIN);
    await step(p, 'RIGHT', 8); await step(p, 'A', 8); await step(p, 'RIGHT', 8);   // two letters
    await step(p, 'B', 8);
    ok('JOIN typing: B steps back a letter (still JOIN)', (await pagesModel(p)) === M.JOIN && ((await ouiScreen(p)) >> 8) === 3);
    await step(p, 'B', 20);
    ok('JOIN typing: B at the first letter: ONLINE', (await pagesModel(p)) === M.ONLINE);
    row('JOIN GAME', 'typing the code', 'a letter back; at the first: ONLINE', '= A (next letter / join)', 'nothing');
    await step(p, 'B', 20);
    ok('B on ONLINE: MULTIPLAYER', (await pagesModel(p)) === M.MP);
    // PLAYER CARD: typing finished with Start, typing cancelled with B
    await step(p, 'DOWN', 8);                                  // PLAYER CARD
    await step(p, 'A', 30);
    ok('PLAYER CARD is up (on NAME)', (await pagesModel(p)) === M.CARD);
    const nameOf = () => p.ev('Module.ccall("PortCardNameJs","string",[],[])');
    const n0 = await nameOf();
    await padPress(p, 0, 'A'); await waitFrames(p, 10);       // typing (the pad: no keyboard letters)
    await padPress(p, 0, 'RIGHT'); await waitFrames(p, 10);
    ok('typing with the pad changes the name', (await nameOf()) !== n0, `${n0} -> ${await nameOf()}`);
    await shot(p, 'front-card-typing.png');
    for (let i = 0; i < 9 && (await p.ev('Module._PortCardTyping()')); i++) { await padPress(p, 0, 'B'); await waitFrames(p, 6); }
    ok('B back to the first letter and once more: cancelled, the name as it was', (await nameOf()) === n0
       && (await p.ev('Module._PortCardTyping()')) === 0 && (await pagesModel(p)) === M.CARD, JSON.stringify(await nameOf()));
    await padPress(p, 0, 'A'); await waitFrames(p, 10);
    await padPress(p, 0, 'RIGHT'); await waitFrames(p, 10);
    const n1 = await nameOf();
    await padPress(p, 0, 'START'); await waitFrames(p, 10);
    ok('Start finishes typing, the name kept', (await p.ev('Module._PortCardTyping()')) === 0 && (await nameOf()) === n1);
    row('PLAYER CARD', 'typing the name', 'a letter back; at the first: cancel (name as it was)', 'done (name kept)', 'nothing');
    await step(p, 'DOWN', 8); await step(p, 'A', 20);          // COLOR -> the picker
    ok('the colour picker is up', (await pagesModel(p)) === M.PICK);
    await step(p, 'B', 20);
    ok('B on the picker: the card', (await pagesModel(p)) === M.CARD);
    row('PLAYER CARD > COLOR', 'picking', 'the card', '= A (keeps, back to the card)', 'nothing');
    await step(p, 'B', 20);
    ok('B on the card: MULTIPLAYER', (await pagesModel(p)) === M.MP);
    await p.ev('Module.ccall("PortCardSetNameJs",null,["string"],[""])');
    await step(p, 'B', 60);
    ok('B on MULTIPLAYER: off the screen (the pop-up)', !(await screenLive(p)));
    row('MULTIPLAYER', 'LOCAL / ONLINE / PLAYER CARD', 'the Start Game pop-up', '= A', 'nothing');
    await p.close();
}

/* JOIN with a code nobody has: the message, then B. */
async function caseJoinMessage() {
    say('join message: NO SUCH GAME, then B');
    const p = await openPage('joinmsg', { extra: RELAY ? { netserver: RELAY } : {} });
    await toGameSelect(p);
    await step(p, 'A', 60); await step(p, 'DOWN'); await step(p, 'A', 80);
    await step(p, 'DOWN'); await step(p, 'A', 30); await step(p, 'DOWN'); await step(p, 'A', 30);
    for (let i = 0; i < 4; i++) { await press(p, 'LEFT', 60, 90); await press(p, 'A'); }   // ZZZZ
    const st = await until(async () => (await ouiScreen(p)) === 3 * 256 + 9, 15000);
    ok('NO SUCH GAME', !!st, String(await ouiScreen(p)));
    await shot(p, 'join-no-such-game.png');
    await step(p, 'B', 20);
    ok('B on the message: ONLINE', (await pagesModel(p)) === M.ONLINE);
    row('JOIN GAME', 'NO SUCH GAME / FULL / OTHER BUILD / OTHER ROM / FAILED / HOST LEFT / REMOVED', 'ONLINE', '= A (join again)', 'nothing');
    // BROWSE GAMES (the server browser): B back to ONLINE on its row
    await step(p, 'DOWN', 10); await step(p, 'A', 40);     // ONLINE row 2: BROWSE GAMES
    ok('BROWSE GAMES is up', ((await ouiScreen(p)) >> 8) === 4, String(await ouiScreen(p)));
    await shot(p, 'browse-list.png');
    await step(p, 'B', 20);
    ok('B on BROWSE GAMES: ONLINE', ((await ouiScreen(p)) >> 8) === 1 && (await pagesModel(p)) === M.ONLINE);
    row('BROWSE GAMES', 'the list (or its message)', 'ONLINE', '= A (join that game)', 'nothing');
    // HOST GAME's PASS row: typing finished with Start, cancelled with B
    for (let i = 0; i < 2; i++) await step(p, 'UP', 10);  // HOST GAME
    await step(p, 'A', 40);
    for (let i = 0; i < 3; i++) await step(p, 'DOWN', 10); // PASS (row 3)
    const pass = () => p.ev('Module.ccall("PortObrHostPassJs","string",[],[])');
    const p0 = await pass();
    // the pad (keyboard letters type the password: B is X there)
    await p.ev(`__padPlug(0, ${JSON.stringify(XBOX)})`); await waitFrames(p, 10);
    const pad = async (k) => { await padPress(p, 0, k); await waitFrames(p, 10); };
    await pad('A');                                        // typing
    ok('HOST GAME > PASS: A starts typing', (await p.ev('Module._PortObrTyping()')) !== 0);
    await pad('RIGHT');
    ok('... Right picks a letter', (await pass()) !== p0, JSON.stringify(await pass()));
    for (let i = 0; i < 9 && (await p.ev('Module._PortObrTyping()')); i++) await pad('B');
    ok('... B at the first letter cancels (the password as it was)', (await pass()) === p0 && (await pagesModel(p)) === M.HOST,
       JSON.stringify(await pass()) + ' model ' + (await pagesModel(p)));
    await pad('A'); await pad('RIGHT');
    const p1 = await pass();
    await pad('START');
    ok('... Start finishes typing, kept', (await p.ev('Module._PortObrTyping()')) === 0 && (await pass()) === p1 && (await pagesModel(p)) === M.HOST);
    row('HOST GAME > PASS / PASSWORD screen', 'typing a password', 'a letter back; at the first: cancel', 'done (kept)', 'nothing');
    await step(p, 'B', 30);
    ok('B on HOST GAME: ONLINE', (await pagesModel(p)) === M.ONLINE);
    row('PASSWORD (a locked game)', 'asking / WRONG / LOCKED', 'JOIN GAME (cancels)', '= A', 'nothing');
    await p.close();
}

/* An invite link (?join=CODE): once the ROM is in, the game goes by itself
 * to the visitor's player card -- JOIN on top with the code, their name and
 * colour below -- and nothing is joined until A on JOIN; B there is the
 * ONLINE screen (the card kept).  Then SETTINGS > PLAYERS > PLAYER CARD on
 * the pause page: the pauser's own card, B back to its row, the colour on
 * the Kirby once the pause closes, the card saved. */
async function caseCards() {
    say('cards: an invite link\'s first screen; PLAYER CARD in the pause');
    const extra = Object.assign({ join: 'BCDF' }, RELAY ? { netserver: RELAY } : {});
    /* (up as the screen slides in: its keys from the frame it is in) */
    const inviteUp = async (q) => (await q.ev('katamOnline.inviteCard()')) === 3 && (await pagesModel(q)) === M.CARD
        && (await q.ev('Module._PortCardSub()')) === 1 && (await screenLive(q)) && !!(await waitFrames(q, 12) || 1);
    const role = (q) => q.ev('katamOnline.role()');
    const p = await openPage('cards-invite', { extra });
    await until(() => p.ev('!!(window.Module && Module.portRuntimeUp && Module._PortFrameNumber() > 30)'), 60000);
    const f0 = await frame(p);
    const up = await until(() => inviteUp(p), 60000);
    ok('invite link: the first screen is the visitor\'s player card', !!up, `after ${(await frame(p)) - f0} frames`);
    ok('... nothing joined yet', (await role(p)) === null);
    await shot(p, 'cards-invite-card.png');
    await step(p, 'DOWN', 10);                                  // NAME
    await p.ev('Module.ccall("PortCardSetNameJs",null,["string"],["VISITOR"])');
    await step(p, 'UP', 10);                                    // JOIN
    await step(p, 'B', 30);
    ok('B on the invite\'s card: ONLINE, not joined, the name kept',
       (await pagesModel(p)) === M.ONLINE && (await role(p)) === null && (await p.ev('katamOnline.inviteCard()')) === 0
       && (await p.ev('Module.ccall("PortCardNameJs","string",[],[])')) === 'VISITOR',
       `model ${await pagesModel(p)} role ${await role(p)}`);
    ok('... and saved', /card0=0,[0-9,]*VISITOR/.test(await p.ev("localStorage.getItem('katam.settings')||''")));
    await step(p, 'B', 30);
    ok('... B again: MULTIPLAYER', (await pagesModel(p)) === M.MP);
    row('Invite link\'s PLAYER CARD', 'JOIN / NAME / COLOR', 'ONLINE (the invite put away)', '= A (JOIN on its row)', 'nothing');
    await p.close();
    const q = await openPage('cards-invite-join', { extra });
    ok('another visit: the card again', !!(await until(() => inviteUp(q), 60000)));
    await step(q, 'A', 30);
    ok('A on JOIN: JOIN GAME joins the code', (await pagesModel(q)) === M.JOIN && (await role(q)) === 'guest');
    const st = await until(async () => (await ouiScreen(q)) === 3 * 256 + 9, 15000);
    ok('... NO SUCH GAME (nobody hosts it)', !!st, String(await ouiScreen(q)));
    await shot(q, 'cards-invite-joined.png');
    await q.close();
    // the pause: SETTINGS > PLAYERS > PLAYER CARD
    const r = await openPage('cards-pause');
    await toGameSelect(r);
    await startPlay(r);
    const open = async () => {
        for (let i = 0; i < 5; i++) {
            await step(r, 'START', 70);
            if ((await pauseState(r)).paused === 1) return true;
            await waitFrames(r, 30);
        }
        return false;
    };
    const toSettings = async () => {
        for (let i = 0; i < 6 && (await pauseState(r)).menu !== MENU.SETTINGS; i++) await step(r, 'SELECT', 70);
        return (await pagesModel(r)) === M.PAUSE_TOP;
    };
    ok('pause, SETTINGS', (await open()) && (await toSettings()));
    await step(r, 'DOWN', 8); await step(r, 'DOWN', 8); await step(r, 'A');   // PLAYERS
    await step(r, 'UP', 10);                                    // (wraps) its last row: PLAYER CARD
    await step(r, 'A', 20);
    ok('PLAYERS > PLAYER CARD: the card, on NAME', (await pagesModel(r)) === M.CARD && (await r.ev('Module._PortCardSub()')) === 1);
    await r.ev('Module.ccall("PortCardSetNameJs",null,["string"],["PAUSER"])');
    await step(r, 'DOWN', 10); await step(r, 'RIGHT', 10);       // COLOR: the first spray
    await shot(r, 'cards-pause-card.png');
    await step(r, 'B', 20);
    ok('B on it: the PLAYERS section, still paused', (await pagesModel(r)) === M.SEC_PLAYERS && (await pauseState(r)).paused === 1);
    await step(r, 'START', 40);
    const painted = await until(async () => (await r.ev('Module._PortCardSlotPaintJs(0) >>> 0')) !== 0, 5000);
    ok('the pause closed: player 1 wears the card\'s colour', !!painted, String(await r.ev('Module._PortCardSlotPaintJs(0) >>> 0')));
    ok('... and the card is saved', /card0=1,0,[0-9,]*PAUSER/.test(await r.ev("localStorage.getItem('katam.settings')||''")),
       await r.ev("(localStorage.getItem('katam.settings')||'').split('\\n').filter(function(l){return /^card0/.test(l)}).join(' ')"));
    await shot(r, 'cards-pause-played.png');
    row('Pause SETTINGS > PLAYERS > PLAYER CARD', 'name / colour (the pauser\'s own)', 'the PLAYERS section', 'close the pause (kept)', 'next page');
    await r.close();
}

/* The pause menu, alone: every page, the SETTINGS sections, NO / YES, the
 * PLAYERS page and a card from it; MAIN MENU twice, a new game between. */
async function casePause() {
    say('pause: B, Start and Select on every page');
    const p = await openPage('pause');
    await toGameSelect(p);
    await startPlay(p);
    const toPage = async (want) => {
        for (let i = 0; i < 6; i++) { if ((await pauseState(p)).menu === want) return true; await step(p, 'SELECT', 70); }
        return (await pauseState(p)).menu === want;
    };
    /* Start until paused: a press the game does not see (a frame longer
     * than the key was held, on a loaded machine, or mid-transition) is
     * pressed again -- the exits are what is tested, not one key's timing */
    const open = async () => {
        for (let i = 0; i < 5; i++) {
            await step(p, 'START', 70);
            if ((await pauseState(p)).paused === 1) {
                if (i) console.log(`    (Start pressed ${i + 1} times before the game paused)`);
                return true;
            }
            await waitFrames(p, 30);
        }
        return false;
    };
    const closed = async () => { await waitFrames(p, 40); return (await pauseState(p)).paused === 0; };
    // the pause opens on SETTINGS (each time: not the page left last time)
    ok('Start pauses (on the SETTINGS page)', await open() && (await pauseState(p)).menu === MENU.SETTINGS && (await pagesModel(p)) === M.PAUSE_TOP);
    await shot(p, 'pause-settings-top.png');
    await step(p, 'B'); ok('SETTINGS top: B closes the pause', await closed());
    await open(); await step(p, 'START'); ok('SETTINGS top: Start closes the pause', await closed());
    await open();
    await step(p, 'SELECT', 70); ok('SETTINGS top: Select turns to the help page', (await pauseState(p)).menu === MENU.HELP);
    await shot(p, 'pause-help-alone.png');
    await step(p, 'B'); ok('help page: B closes the pause', await closed());
    await open(); ok('the next pause opens on SETTINGS again (not the page left)', (await pauseState(p)).menu === MENU.SETTINGS);
    await toPage(MENU.HELP); await step(p, 'START'); ok('help page: Start closes the pause', await closed());
    await open(); await toPage(MENU.HELP); await step(p, 'SELECT', 70);
    ok('help page: Select turns the page', (await pauseState(p)).menu !== MENU.HELP);
    row('Pause: help / area map / world map pages', 'alone', 'close the pause', 'close the pause', 'next page');
    ok('Select reaches SETTINGS', await toPage(MENU.SETTINGS) && (await pagesModel(p)) === M.PAUSE_TOP);
    row('Pause SETTINGS', 'top level', 'close the pause', 'close the pause', 'next page (help)');
    await toPage(MENU.SETTINGS);
    for (let i = 0; i < SECTIONS.length; i++) {
        await step(p, 'A');
        ok(`pause: A opens ${SECTIONS[i]}`, (await pagesModel(p)) === M[SECTIONS[i]]);
        await shot(p, `pause-section-${i}-${SECTIONS[i].toLowerCase()}.png`);
        await step(p, 'B');
        ok(`pause: B from ${SECTIONS[i]}: SETTINGS, still paused`, (await pagesModel(p)) === M.PAUSE_TOP && (await pauseState(p)).paused === 1);
        await step(p, 'DOWN', 10);
    }
    // the seventh row: MAIN MENU
    await step(p, 'A');
    ok('MAIN MENU asks (NO / YES)', (await pagesModel(p)) === M.CONFIRM);
    await shot(p, 'pause-main-menu-confirm.png');
    await step(p, 'B'); ok('B on NO / YES: SETTINGS', (await pagesModel(p)) === M.PAUSE_TOP);
    await step(p, 'A'); await step(p, 'START'); ok('NO / YES: Start closes the pause', await closed());
    await open(); await toPage(MENU.SETTINGS);
    ok('the pause comes back on SETTINGS\' top level', (await pagesModel(p)) === M.PAUSE_TOP);
    await step(p, 'UP', 10); await step(p, 'A');             // (the cursor starts at the top: UP wraps to MAIN MENU)
    await step(p, 'SELECT', 70); ok('NO / YES: Select turns the page (nothing done)', (await pauseState(p)).menu !== MENU.SETTINGS && (await pauseState(p)).paused === 1);
    row('Pause SETTINGS > NO / YES (MAIN MENU)', 'asking', 'SETTINGS (= NO)', 'close the pause', 'next page (= NO)');
    await toPage(MENU.SETTINGS);
    for (const [i, name] of [[0, 'DISPLAY'], [2, 'SEC_PLAYERS']]) {
        for (let k = 0; k < i; k++) await step(p, 'DOWN', 8);
        await step(p, 'A');
        await step(p, 'START'); ok(`${name}: Start closes the pause`, await closed());
        await open(); await toPage(MENU.SETTINGS);
        for (let k = 0; k < i; k++) await step(p, 'DOWN', 8);
        await step(p, 'A'); await step(p, 'SELECT', 70);
        ok(`${name}: Select turns the page`, (await pauseState(p)).menu !== MENU.SETTINGS);
        await toPage(MENU.SETTINGS);
    }
    row('Pause SETTINGS > a section', 'its rows', 'SETTINGS on its row', 'close the pause', 'next page');
    // PLAYERS page and a card
    await step(p, 'DOWN', 8); await step(p, 'DOWN', 8); await step(p, 'A');   // the PLAYERS section
    await step(p, 'A', 30);
    ok('the PLAYERS page', (await pagesModel(p)) === M.PLAYERS);
    await step(p, 'B'); ok('PLAYERS page: B back to the PLAYERS section', (await pagesModel(p)) === M.SEC_PLAYERS);
    await step(p, 'A', 30); await step(p, 'START'); ok('PLAYERS page: Start closes the pause', await closed());
    row('Pause PLAYERS page', 'choosing pads', 'the PLAYERS section', 'close the pause', 'next page');
    await open(); await toPage(MENU.SETTINGS);
    await step(p, 'DOWN', 8); await step(p, 'DOWN', 8); await step(p, 'A'); await step(p, 'A', 30);
    await step(p, 'DOWN', 8); await step(p, 'A', 30);         // P2: nothing chosen -> P2's card
    ok('a card from the PLAYERS page', (await pagesModel(p)) === M.CARD);
    await step(p, 'B'); ok('card: B back to PLAYERS', (await pagesModel(p)) === M.PLAYERS);
    await step(p, 'A', 30); await step(p, 'START'); ok('card: Start closes the pause', await closed());
    row('Pause PLAYERS > a card', 'name / colour', 'PLAYERS', 'close the pause (name kept)', 'next page');
    // MAIN MENU > YES, twice, a game between
    for (let round = 1; round <= 2; round++) {
        const loads0 = await p.ev('Module._PortLnHomeLoads()');
        await open(); await toPage(MENU.SETTINGS);
        await step(p, 'UP', 10); await step(p, 'A'); await step(p, 'DOWN', 8);
        await shot(p, `pause-main-menu-yes-${round}.png`);
        await step(p, 'A', 10);
        const home = await until(async () => (await p.ev('Module._PortLnHomeLoads()')) === loads0 + 1, 5000);
        ok(`MAIN MENU > YES (${round}): back to power-on, no reload`, !!home);
        await waitFrames(p, 60);
        ok(`... straight to the title (no logos)`, (await p.ev('Module._PortLnHomeExpressed()')) === round);
        ok('... not paused, no menu', (await pauseState(p)).paused === 0 && !(await screenLive(p)));
        await shot(p, `pause-main-menu-title-${round}.png`);
        await waitFrames(p, 60);
        await step(p, 'START', 120);                           // title -> file select
        await step(p, 'A', 120);                               // FILE 1 -> Game Select
        await startPlay(p);
        ok(`a new game runs after MAIN MENU (${round})`, (await p.ev('Module._PortLmpInStory()')) === 1);
    }
    row('Pause SETTINGS > MAIN MENU > YES', 'alone / local', 'n/a', 'n/a', 'n/a');
    await p.close();
}

/* Local multiplayer: player 2 (a pad) pauses; only the pauser's keys work;
 * MAIN MENU leaves for both, and the next game is one player again. */
/* SETTINGS > EXPERIMENTAL > CPU KIRBYS: on the ordinary module more than 3
 * asks (NO / YES) and YES reloads the page with the 16-Kirby module
 * (katam16.js); there a new game starts with that many Kirbys, ADD 4 /
 * REMOVE 4 in the pause change them in the running game, LOCAL multiplayer
 * is unavailable (ONLINE is its 8-player games), and back at 3 the next load
 * is the ordinary module with the same save.  An invite link on the 16-Kirby
 * module is followed (web/online.js: a game on the other module reloads). */
async function caseKirbys() {
    say('kirbys: EXPERIMENTAL > CPU KIRBYS, from the 4-Kirby module (?mod=4) to the 16-Kirby one');
    const p = await openPage('kirbys', { extra: { mod: '4' } });
    const now = () => p.ev('Module._PortPocKirbysNow()');
    const max = () => p.ev('Module._PortPocKirbysMax()');
    const toExperimental = async () => {            // Game Select -> SETTINGS > EXPERIMENTAL
        for (let i = 0; i < 4; i++) await step(p, 'DOWN', 8);
        await press(p, 'A'); await until(() => screenLive(p), 10000); await waitFrames(p, 30);
        await step(p, 'UP', 10); await step(p, 'A');
        return (await pagesModel(p)) === M.EXPERIMENTAL;
    };
    await toGameSelect(p);
    ok('the ordinary module: 4 Kirby slots', (await max()) === 4 && !(await p.ev('!!window.KATAM_KIRBYS16')));
    ok('SETTINGS > EXPERIMENTAL', await toExperimental());
    await step(p, 'DOWN', 8); await step(p, 'DOWN', 8);          // ADD 4
    await shot(p, 'kirbys-add4-ordinary.png');
    await step(p, 'A');
    ok('ADD 4 on the ordinary module asks first (NO / YES)', (await pagesModel(p)) === M.CONFIRM);
    await step(p, 'DOWN', 8);
    await shot(p, 'kirbys-confirm-yes.png');
    const f0 = await frame(p);
    await press(p, 'A');                                         // YES: the page reloads
    const reloaded = await until(async () => { try { return (await p.ev('!!window.KATAM_KIRBYS16 && !!(window.Module && Module.portRuntimeUp)')) && (await frame(p)) < f0; } catch (e) { return false; } }, 60000, 250);
    ok('YES: the page reloads with the 16-Kirby module, 8 Kirbys saved', !!reloaded && (await max()) === 16 && (await kirbysSetting(p)) === 8,
       `max ${await max().catch(() => '?')}, setting ${await kirbysSetting(p).catch(() => '?')}`);
    ok('... at the page\'s own address (?mod=4 gone)', !/[?&](mod|kirbys)=/.test(await p.ev('location.search')), await p.ev('location.search'));
    ok('... local drop-in there too', (await p.ev('Module._PortLmpCanRequest()')) === 1);
    await toGameSelect(p);
    ok('16-Kirby module: SETTINGS > EXPERIMENTAL', await toExperimental());
    await step(p, 'DOWN', 8); await step(p, 'DOWN', 8);          // ADD 4
    await step(p, 'A'); await step(p, 'A');                      // 12, 16: no NO / YES here
    ok('ADD 4 twice: 16 Kirbys, no question', (await kirbysSetting(p)) === 16 && (await pagesModel(p)) === M.EXPERIMENTAL);
    await step(p, 'UP', 8);
    await shot(p, 'kirbys-16-cpu-row.png');
    await step(p, 'B'); await step(p, 'B', 40);                  // Game Select
    // MULTIPLAYER: LOCAL available, ONLINE its 8-player games
    await step(p, 'UP', 8); for (let i = 0; i < 3; i++) await step(p, 'UP', 8);   // START GAME
    await press(p, 'A'); await waitFrames(p, 60); await press(p, 'DOWN'); await waitFrames(p, 20);
    await press(p, 'A'); await waitFrames(p, 80);                // the MULTIPLAYER screen
    ok('MULTIPLAYER screen up', (await pagesModel(p)) === M.MP);
    ok('... LOCAL available', (await p.ev('Module._PortMenuPocRowAvailable(0)')) === 1);
    await shot(p, 'kirbys-mp-local.png');
    await press(p, 'DOWN'); await waitFrames(p, 20);
    await shot(p, 'kirbys-mp-online-8.png');
    await press(p, 'A'); await waitFrames(p, 40);
    ok('... ONLINE: A opens it (8-player games)', (await pagesModel(p)) !== M.MP && ((await p.ev('Module._PortOuiScreen()')) >> 8) === 1);
    await press(p, 'B'); await waitFrames(p, 40);
    await press(p, 'B'); await waitFrames(p, 60); await press(p, 'B'); await waitFrames(p, 60);
    // a new game (a fresh file: the story's opening, not sealed NORMAL): you and 15 CPU Kirbys
    await startPlay(p, true);
    const story = await p.ev('Module.HEAPU16[0x0203AD2C >> 1]');
    ok(`a new game on the 16-Kirby module: 16 Kirbys (story stage ${story}, NORMAL is 300)`, (await now()) === 16, String(await now()));
    await waitFrames(p, 300);
    await shot(p, 'kirbys-16-play.png');
    ok('... still 16 after a warp and 300 frames', (await now()) === 16, String(await now()));
    // the pause: REMOVE 4 three times, live
    const paused = async () => { for (let i = 0; i < 5; i++) { await step(p, 'START', 70); if ((await pauseState(p)).paused === 1) return true; await waitFrames(p, 30); } return false; };
    const toSettings = async () => { for (let i = 0; i < 6 && (await pauseState(p)).menu !== MENU.SETTINGS; i++) await step(p, 'SELECT', 70); };
    ok('pause', await paused()); await toSettings();
    await step(p, 'UP', 10); await step(p, 'UP', 10); await step(p, 'A');   // (wraps past MAIN MENU) EXPERIMENTAL
    ok('pause: EXPERIMENTAL', (await pagesModel(p)) === M.EXPERIMENTAL);
    for (let i = 0; i < 3; i++) await step(p, 'DOWN', 8);        // REMOVE 4
    for (let i = 0; i < 3; i++) await step(p, 'A');
    ok('REMOVE 4 three times: 4 in all saved', (await kirbysSetting(p)) === 4);
    await shot(p, 'kirbys-pause-4.png');
    await step(p, 'START', 60);
    ok('... and in the game once unpaused: 4 Kirbys', !!(await until(async () => (await now()) === 4, 20000)), String(await now()));
    ok('pause again', await paused()); await toSettings();
    await step(p, 'UP', 10); await step(p, 'UP', 10); await step(p, 'A');
    await step(p, 'DOWN', 8); await step(p, 'DOWN', 8); await step(p, 'A');  // ADD 4
    await step(p, 'UP', 8); await step(p, 'RIGHT', 8);           // CPU KIRBYS 8 -> 9 in all
    ok('ADD 4 and CPU KIRBYS Right: 9 in all', (await kirbysSetting(p)) === 9);
    await step(p, 'START', 60);
    ok('... live: 9 Kirbys', !!(await until(async () => (await now()) === 9, 20000)), String(await now()));
    await waitFrames(p, 600);
    await shot(p, 'kirbys-9-play.png');
    ok('... still 9 600 frames on', (await now()) === 9);
    // back to 4 in all, and the save
    ok('pause', await paused()); await toSettings();
    await step(p, 'UP', 10); await step(p, 'UP', 10); await step(p, 'A');
    await step(p, 'DOWN', 8); await step(p, 'DOWN', 8); await step(p, 'DOWN', 8); await step(p, 'A'); await step(p, 'A');
    ok('REMOVE 4 twice: 4 in all', (await kirbysSetting(p)) === 4);
    await step(p, 'START', 60);
    await until(async () => (await now()) === 4, 20000);
    const sramSum = `(function(){var h=Module.HEAPU8,s=0;for(var i=0x09000000;i<0x09010000;i++)s=(s*31+h[i])>>>0;return s})()`;
    await p.ev('Module._shell.flushSave()'); await sleep(1500);
    const sum16 = await p.ev(sramSum);
    await p.s('Page.reload', {});
    await until(() => p.ev('!!(window.Module && Module.portRuntimeUp && Module._PortFrameNumber() > 30)'), 60000);
    ok('the next load at 4 in all: still the 16-Kirby module (the default), 4 Kirbys', (await p.ev('!!window.KATAM_KIRBYS16')) && (await max()) === 16
       && (await p.ev('window.KATAM_KIRBYS')) === 4);
    // ?mod=4: the 4-Kirby module, the same save memory
    {
        const u = new URL(await p.ev('location.href'));
        u.searchParams.set('mod', '4');
        await p.s('Page.navigate', { url: u.toString() });
        await sleep(500);
    }
    await until(() => p.ev('!!(window.Module && Module.portRuntimeUp && Module._PortFrameNumber() > 30)'), 60000);
    ok('?mod=4: the 4-Kirby module', !(await p.ev('!!window.KATAM_KIRBYS16')) && (await max()) === 4);
    await toGameSelect(p);
    ok('... with the same save memory as the 16-Kirby module left', (await p.ev(sramSum)) === sum16, `${sum16} vs ${await p.ev(sramSum)}`);
    await p.close();
    // an invite link on the 16-Kirby module
    const q = await openPage('kirbys-invite', { extra: { join: 'BCDF', mod: null },
        early: `try{if(!/kirbys=/.test(localStorage.getItem('katam.settings')||''))localStorage.setItem('katam.settings','schema=2\\nkirbys=16\\n')}catch(e){}` });
    await until(() => q.ev('!!(window.Module && Module.portRuntimeUp && Module._PortFrameNumber() > 120)'), 60000);
    // (the visitor's player card first: A on its JOIN row goes on)
    ok('invite link on the 16-Kirby module: the player card first', !!(await until(async () => (await q.ev('katamOnline.inviteCard()')) === 3 && (await screenLive(q)), 60000)));
    await waitFrames(q, 12); await step(q, 'A', 30);
    await sleep(3000);
    await shot(q, 'kirbys-invite-followed.png');
    ok('invite link on the 16-Kirby module: followed (no such game here)', (await q.ev('!!window.KATAM_KIRBYS16 && !!(window.katamOnline && window.katamOnline.role && window.katamOnline.role() === "guest")'))
       && (await q.ev('document.getElementById("curtain").className')).indexOf('is-invite-bad') < 0,
       await q.ev('document.getElementById("curtain").className'));
    await q.close();
}

/* The page's default: a fresh profile and no ?mod -- the 16-Kirby module at
 * 4 Kirbys, one module fetched; LOCAL works there; ONLINE > HOST 4 PLAYERS
 * reloads into the 4-Kirby module and HOST 8 PLAYERS back; ?mod=4 is the
 * 4-Kirby module. */
const pageLog = (p) => p.ev('Array.from(document.querySelectorAll("#log > div")).map(function(e){return e.textContent})');
const fetchedWasm = (p) => p.ev(`performance.getEntriesByType('resource').map(function(r){return r.name}).filter(function(n){return /katam(16)?\\.wasm/.test(n)}).map(function(n){return n.replace(/^.*\\/|\\?.*$/g,'')})`);
async function caseDefault() {
    say('default: a fresh profile, no ?mod');
    const p = await openPage('default', { extra: { mod: null } });
    await toGameSelect(p);
    ok('default: the 16-Kirby module, 4 Kirbys', (await p.ev('!!window.KATAM_KIRBYS16')) && (await p.ev('Module._PortPocKirbysMax()')) === 16
       && (await p.ev('window.KATAM_KIRBYS')) === 4);
    ok('default: one module downloaded (katam16.wasm)', JSON.stringify(await fetchedWasm(p)) === '["katam16.wasm"]', JSON.stringify(await fetchedWasm(p)));
    {
        const L = await pageLog(p);
        ok('default: the page log names the module (no EXPERIMENTAL)', L.some((t) => /^module: katam16, 4 Kirbys/.test(t))
           && !L.some((t) => /EXPERIMENTAL/.test(t)), L.filter((t) => /module|EXPERIMENTAL/.test(t)).join(' | '));
    }
    ok('default: online games here hold 8', (await p.ev('katamOnline.module()')) === '16' && (await p.ev('katamOnline.seats()')) === 8);
    // MULTIPLAYER > LOCAL: a two-player game, 4 Kirbys
    await step(p, 'A', 60); await step(p, 'DOWN'); await step(p, 'A', 80);
    ok('default: MULTIPLAYER, LOCAL available', (await pagesModel(p)) === M.MP && (await p.ev('Module._PortMenuPocRowAvailable(0)')) === 1);
    await shot(p, 'default-mp.png');
    // ONLINE (row 1): its fourth row reloads into the 4-Kirby module
    await step(p, 'DOWN'); await step(p, 'A', 30);
    ok('default: ONLINE is up', (await pagesModel(p)) === M.ONLINE);
    for (let i = 0; i < 3; i++) await step(p, 'DOWN', 8);
    await shot(p, 'default-online-host4.png');
    await step(p, 'B', 30); await step(p, 'UP', 8);             // MULTIPLAYER, LOCAL
    await press(p, 'A');
    const inPlay = await until(() => p.ev('Module._PortLmpInPlay()'), 60000, 200);
    await waitFrames(p, 60);
    ok('default: LOCAL starts a two-player game', !!inPlay && JSON.stringify(await humans(p)) === '[1,1,0,0]', JSON.stringify(await humans(p)));
    ok('default: ... with 4 Kirbys', (await p.ev('Module._PortPocKirbysNow()')) === 4);
    await keyHold(p, 'ARROWDOWN', true); await waitFrames(p, 4);
    const k2 = await p.ev('katamDevices.playerKeys(1)');
    await keyHold(p, 'ARROWDOWN', false);
    ok('default: player 2\'s keys (the arrows) reach player 2', (k2 & 128) === 128, String(k2));
    await waitFrames(p, 120);
    await shot(p, 'default-local-two.png');
    await p.close();

    // ONLINE > HOST 4 PLAYERS: the page again on the 4-Kirby module, and HOST 8 PLAYERS back
    const r = await openPage('default-hop', { extra: { mod: null } });
    await toGameSelect(r);
    const toOnlineRow3 = async () => {
        await step(r, 'A', 60); await step(r, 'DOWN'); await step(r, 'A', 80);
        await step(r, 'DOWN'); await step(r, 'A', 30);
        for (let i = 0; i < 3; i++) await step(r, 'DOWN', 8);
        return (await pagesModel(r)) === M.ONLINE;
    };
    ok('default: ONLINE, fourth row', await toOnlineRow3());
    await press(r, 'A');
    const hopped4 = await until(async () => { try { return /[?&]mod=4\b/.test(await r.ev('location.search')) && !(await r.ev('!!window.KATAM_KIRBYS16')) && (await r.ev('!!(Module.portRuntimeUp && Module._PortFrameNumber() > 30)')); } catch (e) { return false; } }, 60000, 250);
    ok('default: HOST 4 PLAYERS reloads into the 4-Kirby module (?mod=4)', !!hopped4, await r.ev('location.search').catch(() => '?'));
    ok('... where online games hold 4', (await r.ev('katamOnline.seats()')) === 4);
    await toGameSelect(r);
    ok('4-Kirby module: ONLINE, fourth row', await toOnlineRow3());
    await shot(r, 'mod4-online-host8.png');
    await press(r, 'A');
    const hopped16 = await until(async () => { try { return /[?&]mod=16\b/.test(await r.ev('location.search')) && (await r.ev('!!window.KATAM_KIRBYS16')) && (await r.ev('!!(Module.portRuntimeUp && Module._PortFrameNumber() > 30)')); } catch (e) { return false; } }, 60000, 250);
    ok('4-Kirby module: HOST 8 PLAYERS reloads into the 16-Kirby module', !!hopped16, await r.ev('location.search').catch(() => '?'));
    await r.close();

    // ?mod=4: the 4-Kirby module, its wasm only
    const q = await openPage('mod4', { extra: { mod: '4' } });
    await until(() => q.ev('!!(window.Module && Module.portRuntimeUp && Module._PortFrameNumber() > 30)'), 60000);
    ok('?mod=4: the 4-Kirby module, katam.wasm only', !(await q.ev('!!window.KATAM_KIRBYS16')) && (await q.ev('Module._PortPocKirbysMax()')) === 4
       && JSON.stringify(await fetchedWasm(q)) === '["katam.wasm"]', JSON.stringify(await fetchedWasm(q)));
    ok('?mod=4: the page log names it', (await pageLog(q)).some((t) => /^module: katam, the 4-Kirby module/.test(t)));
    await q.close();
}

async function caseLocal() {
    say('local: the pause belongs to whoever paused');
    const p = await openPage('local');
    await toGameSelect(p);
    await startPlay(p);
    await p.ev(`__padPlug(0, ${JSON.stringify(XONE)})`); await waitFrames(p, 10);
    await padPress(p, 0, 'START'); await waitFrames(p, 60);
    ok('player 2 joins by Start', (await p.ev('Module._PortLmpSlotIsHuman(1)')) === 1);
    await padPress(p, 0, 'START'); await waitFrames(p, 70);
    ok('player 2 pauses', (await pauseState(p)).paused === 1);
    await step(p, 'B', 40);
    ok('player 1\'s B does not close player 2\'s pause', (await pauseState(p)).paused === 1);
    await padPress(p, 0, 'B'); await waitFrames(p, 40);
    ok('player 2\'s B closes it', (await pauseState(p)).paused === 0);
    await padPress(p, 0, 'START'); await waitFrames(p, 70);
    for (let i = 0; i < 6 && (await pauseState(p)).menu !== MENU.SETTINGS; i++) { await padPress(p, 0, 'SELECT'); await waitFrames(p, 70); }
    ok('player 2: Select to SETTINGS', (await pagesModel(p)) === M.PAUSE_TOP);
    await padPress(p, 0, 'A'); await waitFrames(p, 20);
    ok('player 2: A opens DISPLAY', (await pagesModel(p)) === M.DISPLAY);
    await padPress(p, 0, 'B'); await waitFrames(p, 20);
    ok('player 2: B back', (await pagesModel(p)) === M.PAUSE_TOP);
    await padPress(p, 0, 'START'); await waitFrames(p, 40);
    ok('player 2: Start closes', (await pauseState(p)).paused === 0);
    row('Pause, local multiplayer', 'the pauser (P2)', 'as alone', 'as alone', 'as alone (any player\'s Select)');
    row('Pause, local multiplayer', 'the other player', 'nothing (the pauser\'s menu)', 'nothing', 'next page');
    await padPress(p, 0, 'START'); await waitFrames(p, 70);
    for (let i = 0; i < 6 && (await pauseState(p)).menu !== MENU.SETTINGS; i++) { await padPress(p, 0, 'SELECT'); await waitFrames(p, 70); }
    await padPress(p, 0, 'UP'); await waitFrames(p, 10);
    await padPress(p, 0, 'A'); await waitFrames(p, 10); await padPress(p, 0, 'DOWN'); await waitFrames(p, 8);
    await padPress(p, 0, 'A'); await waitFrames(p, 60);
    ok('local MAIN MENU > YES: back to power-on', (await p.ev('Module._PortLnHomeLoads()')) === 1);
    await waitFrames(p, 120);
    await shot(p, 'local-main-menu-title.png');
    await step(p, 'START', 120); await step(p, 'A', 120);
    await startPlay(p);
    await waitFrames(p, 60);
    // the device layer keeps who holds which pad, as on any way back to the
    // title: player 2's pad is player 2 again in the next game
    ok('a new game afterwards: player 2\'s pad is player 2 again (the devices kept, as at any title)',
       JSON.stringify(await p.ev('[0,1,2,3].map(function(s){return Module._PortLmpSlotIsHuman(s)})')) === '[1,1,0,0]',
       JSON.stringify(await p.ev('[0,1,2,3].map(function(s){return Module._PortLmpSlotIsHuman(s)})')));
    const k0 = await p.ev('Module._PortLmpHostKeys(1)');
    await p.ev('__padSet(0, [15])'); await waitFrames(p, 10);
    ok('... and its keys reach player 2 (nothing stuck)', (await p.ev('Module._PortLmpHostKeys(1)')) !== k0 || (await p.ev('Module._PortLmpHostKeys(1)')) === 16,
       String(await p.ev('Module._PortLmpHostKeys(1)')));
    await p.ev('__padSet(0, [])'); await waitFrames(p, 10);
    ok('... and released', (await p.ev('Module._PortLmpHostKeys(1)')) === 0);
    await p.close();
}

/* The new screens for the owner to look at, at a phone's size (portrait,
 * touch) and a desktop's (1440 x 800): SETTINGS and each section on Game
 * Select and in the pause menu, the NO / YES screen. */
async function caseLooks(tag, opts) {
    say(`looks: ${tag}`);
    const p = await openPage(tag, opts);
    await toGameSelect(p);
    for (let i = 0; i < 4; i++) await step(p, 'DOWN', 8);
    await press(p, 'A'); await until(() => screenLive(p), 10000); await waitFrames(p, 30);
    await shot(p, `${tag}-front-settings.png`);
    for (let i = 0; i < SECTIONS.length; i++) {
        await step(p, 'A'); await shot(p, `${tag}-front-${SECTIONS[i].toLowerCase()}.png`);
        if (SECTIONS[i] === 'EXPERIMENTAL') {
            await step(p, 'A'); await shot(p, `${tag}-front-lighting.png`);      // LIGHTING
            await step(p, 'RIGHT', 10); await shot(p, `${tag}-front-lighting-on.png`);
            for (let k = 0; k < 4; k++) await step(p, 'DOWN', 6);
            await shot(p, `${tag}-front-lighting-on-rows.png`);
            for (let k = 0; k < 4; k++) await step(p, 'UP', 6);
            await step(p, 'LEFT', 10);
            await step(p, 'B'); await step(p, 'DOWN', 6);
            await shot(p, `${tag}-front-experimental-kirbys.png`);
            await step(p, 'UP', 6);
        }
        await step(p, 'B'); await step(p, 'DOWN', 10);
    }
    await step(p, 'B', 40);
    for (let i = 0; i < 4; i++) await step(p, 'UP', 8);         // START GAME
    await startPlay(p);
    /* (a Start the game ignores -- mid-transition -- is pressed again) */
    for (let i = 0; i < 6 && (await pauseState(p)).paused !== 1; i++) { await waitFrames(p, 30); await step(p, 'START', 90); }
    for (let i = 0; i < 6 && (await pauseState(p)).menu !== MENU.SETTINGS; i++) await step(p, 'SELECT', 80);
    await shot(p, `${tag}-pause-settings.png`);
    for (let i = 0; i < SECTIONS.length; i++) {
        await step(p, 'A'); await shot(p, `${tag}-pause-${SECTIONS[i].toLowerCase()}.png`);
        await step(p, 'B'); await step(p, 'DOWN', 10);
    }
    await step(p, 'A'); await shot(p, `${tag}-pause-main-menu-confirm.png`);
    ok(`${tag}: screens shot`, (await pagesModel(p)) === M.CONFIRM);
    await p.close();
}

const RELAY = flag('relay', '');
for (const c of CASES) {
    try {
        if (c === 'front') await caseFront();
        else if (c === 'joinmsg') await caseJoinMessage();
        else if (c === 'cards') await caseCards();
        else if (c === 'pause') await casePause();
        else if (c === 'local') await caseLocal();
        else if (c === 'default') await caseDefault();
        else if (c === 'kirbys') await caseKirbys();
        else if (c === 'phone') await caseLooks('phone', { w: 390, h: 844, mobile: true, touch: true });
        else if (c === 'wide') await caseLooks('wide', { w: 1440, h: 800 });
    } catch (e) {
        ok(`case ${c} ran to the end`, false, e.stack || e.message);
    }
}
console.log('\nEXITS TABLE (from the run):');
for (const r of TABLE) console.log(`  ${r.screen} | ${r.state} | B: ${r.b} | Start: ${r.start} | Select: ${r.select}`);
console.log(`\n${pass} passed, ${fail} failed (${((Date.now() - T0) / 1000).toFixed(0)} s)`);
process.exit(fail ? 1 : 0);
