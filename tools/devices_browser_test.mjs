// Drop-in / drop-out local multiplayer and the PLAYERS page, end to end in a
// real browser: build/dist in a headless Chromium, fake gamepads.
//
//   node tools/devices_browser_test.mjs --cdp 9363 --base http://127.0.0.1:8963/dist/
//        --rom-url http://127.0.0.1:8963/rom.gba --shots DIR [--cases frontend,local,solo,touch]
//
// Needs a Chromium already listening on --cdp with its own profile (never the
// shared one) and a static server for the dist and a ROM you own (served
// from outside the tree).  Screenshots are ROM-derived: write them outside
// the tree.
//
// The gamepads are stand-ins: navigator.getGamepads returns objects shaped
// like the Gamepad API's ("standard" mapping, as XInput pads report), and
// plugging / unplugging fires gamepadconnected / gamepaddisconnected, before
// the page's scripts run.  Everything else is the real page: keyboard
// events through CDP, touches through CDP, the game's own menus.
//
// Cases:
//   frontend  Game Select > SETTINGS: the PLAYERS row and page, a choice
//             shown and cancelled, B back.
//   local     play: players 2, 3 and 4 join by Start (notices, the join
//             hint), all four move, the middle one leaves by holding Select
//             and rejoins, a pad unplugged and back in the grace period,
//             unplugged for good (the CPU takes it) and rejoined, a fifth
//             Start ("4 players already", player 1 not paused), the pause
//             menu's SETTINGS > PLAYERS page: drop out, add a waiting pad,
//             move a pad from one player to another; B back to SETTINGS.
//   solo      one person with a keyboard and a pad: both drive player 1,
//             the pad's Start pauses, nobody joins.
//   touch     a phone-shaped viewport, touch only: touch drives player 1,
//             its Start never joins.
//   hudplaces where the party HUD's cards go: in the bar beside a 3:2
//             picture on a wide window (opaque), over the picture on a
//             phone in landscape whose touch pads fill those bars (the
//             keep-outs), name tags on.
//   picker    the player card's colour by parts (platform/player_card.c),
//             at phone size (844x390, touch) and 1440x800: Start Game >
//             MULTIPLAYER > PLAYER CARD, A on COLOR (the game's own colour
//             becomes its spray, feet with the body), the grid on BODY (the
//             Kirby wears the cell under the cursor, A keeps), FEET stepped
//             off BODY, its grid (B puts it back, then a cell kept), back to
//             BODY with Right, B out; saved as card0parts; an old one-colour
//             card reads as it did (OWN, the same paint); then play in it.
import fs from 'fs';
import path from 'path';

const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf('--' + n); return i >= 0 ? argv[i + 1] : d; };
const CDP_PORT = +flag('cdp', 9363);
const BASE = flag('base', 'http://127.0.0.1:8963/dist/');
const ROM_URL = flag('rom-url', 'http://127.0.0.1:8963/rom.gba');
const SHOTS = flag('shots', '');
const CASES = flag('cases', 'gl,lmp,frontend,local,solo,touch,hudplaces,picker').split(',');
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
    for (const [k, v] of Object.entries(extra)) u.searchParams.set(k, v);
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
 * tools/netaudit/make_players_recordings.py does) */
async function startPlay(p) {
    await press(p, 'A'); await waitFrames(p, 60);          // the pop-up
    await press(p, 'A'); await waitFrames(p, 60);          // ONE PLAYER
    await until(() => p.ev('Module._PortLmpInStory()'), 60000, 100);
    await waitFrames(p, 200);
    await p.ev('Module._PortWarp(0, 117, 44, 9)');
    await waitFrames(p, 120);
    // gAIKirbyState (0x0203AD2C, u16) = NORMAL (300): the CPUs are partners
    await p.ev('(function(){var a=0x0203AD2C;Module.HEAPU8[a]=300&255;Module.HEAPU8[a+1]=300>>8;})()');
    await waitFrames(p, 60);
}

/* Game Select's SETTINGS (the fifth row), its PLAYERS section (the third
 * row) and in it the PLAYERS row (the first) */
async function toSettingsPlayers(p) {
    for (let i = 0; i < 4; i++) { await press(p, 'DOWN'); await waitFrames(p, 8); }
    await press(p, 'A');
    await until(() => screenLive(p), 10000);
    await waitFrames(p, 30);
    await press(p, 'DOWN'); await waitFrames(p, 10);       // SOUND
    await press(p, 'DOWN'); await waitFrames(p, 10);       // PLAYERS
    await press(p, 'A'); await waitFrames(p, 20);          // the PLAYERS section, on PLAYERS
}
const SEC_PLAYERS = 12, TOP = 0, PAUSE_TOP = 9;

/* The pause menu's SETTINGS page: Start, then Select until the page is up */
async function toPauseSettings(p) {
    await press(p, 'START'); await waitFrames(p, 60);
    for (let i = 0; i < 5; i++) {
        if (await screenLive(p)) return true;
        await press(p, 'SELECT'); await waitFrames(p, 70);
    }
    return screenLive(p);
}

/* On the PLAYERS page, cursor on row `row` (from row `from`), choose device
 * `devId` (0 = CPU / ANY) with Right presses, A to confirm. */
async function choose(p, from, row, devId, confirm = true) {
    for (let r = from; r < row; r++) { await press(p, 'DOWN'); await waitFrames(p, 8); }
    for (let r = from; r > row; r--) { await press(p, 'UP'); await waitFrames(p, 8); }
    const st = await p.ev(`(function(){
        var d = katamDevices.listDevices().filter(function(x){return x.present && x.player !== ${-3}}).map(function(x){return x.id});
        var pl = katamDevices.listPlayers()[${row}];
        var ids = [0].concat(d);
        var cur;
        if (${row} >= 1 && !pl.human) cur = 0;
        else if (${row} === 0 && !katamDevices.listDevices().some(function(x){return x.player === 0 && x.present})) cur = 0;
        else cur = ids.indexOf(pl.main);
        return { ids: ids, cur: cur };
    })()`);
    const target = st.ids.indexOf(devId);
    const n = st.ids.length;
    const rights = ((target - Math.max(st.cur, 0)) % n + n) % n;
    for (let k = 0; k < rights; k++) { await press(p, 'RIGHT', 70, 110); await waitFrames(p, 4); }
    await waitFrames(p, 10);
    if (confirm) { await press(p, 'A'); await waitFrames(p, 30); }
    return rights;
}

const XBOX = 'Xbox 360 Controller (XInput STANDARD GAMEPAD)';
const XONE = 'Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 0b13)';
const DS = 'DualSense Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 0ce6)';

// --------------------------------------------------------------------------
async function caseFrontend() {
    say('frontend: Game Select > SETTINGS > PLAYERS');
    const p = await openPage('front');
    await toGameSelect(p);
    await p.ev(`__padPlug(0, ${JSON.stringify(XBOX)})`);
    await waitFrames(p, 10);
    await toSettingsPlayers(p);
    ok('the settings screen is up (SETTINGS > PLAYERS)', (await pagesModel(p)) === SEC_PLAYERS);
    await shot(p, 'front-01-settings-players-row.png');
    await press(p, 'A'); await waitFrames(p, 20);
    ok('A on PLAYERS opens the PLAYERS page', (await pagesModel(p)) === 5 && await p.ev('Module._PortPuiActive()') === 1);
    await shot(p, 'front-02-players-page.png');
    const pad = (await devList(p)).find((d) => d.label === 'PAD 1');
    ok('the pad is listed', !!pad, pad && pad.name);
    await choose(p, 0, 1, pad.id, false);
    await shot(p, 'front-03-players-choose-pad.png');
    await press(p, 'UP'); await waitFrames(p, 10);         // the cursor leaves the row: the choice is forgotten
    await press(p, 'DOWN'); await waitFrames(p, 10);
    await press(p, 'A'); await waitFrames(p, 20);          // nothing chosen: P2's card
    ok('nothing chosen, nothing joined', (await human(p, 1)) === 0);
    ok('A on P2 with nothing chosen opens player 2\'s card', (await pagesModel(p)) === 6);
    // the NAME row is a row like the others until A starts typing: D (Right)
    // types nothing, it is the game's (web/shell.html cardKeys)
    await press(p, 'RIGHT'); await waitFrames(p, 10);
    ok('NAME row, not typing: D is the game\'s Right, not a letter',
       (await p.ev('Module.ccall("PortCardSeatProfiles","string",[],[])')).split('\n')[1].split('\t')[0] === ''
       && (await p.ev('Module._PortCardTyping()')) === 0);
    await press(p, 'A'); await waitFrames(p, 10);          // Z: A on NAME starts typing
    ok('A on the NAME row starts typing', (await p.ev('Module._PortCardTyping()')) === 1);
    for (const ch of 'ANNA') {
        await p.s('Input.dispatchKeyEvent', { type: 'keyDown', code: 'Key' + ch, key: ch.toLowerCase(), text: ch.toLowerCase(), windowsVirtualKeyCode: ch.charCodeAt(0) });
        await p.s('Input.dispatchKeyEvent', { type: 'keyUp', code: 'Key' + ch, key: ch.toLowerCase(), windowsVirtualKeyCode: ch.charCodeAt(0) });
        await waitFrames(p, 4);
    }
    await waitFrames(p, 10);
    await shot(p, 'front-03a-p2-card-name.png');
    const prof = await p.ev('Module.ccall("PortCardSeatProfiles","string",[],[])');
    ok('typed letters name player 2\'s card', prof.split('\n')[1].split('\t')[0] === 'ANNA', JSON.stringify(prof));
    await press(p, 'START'); await waitFrames(p, 10);      // Enter: done typing
    ok('Enter ends typing, the name kept', (await p.ev('Module._PortCardTyping()')) === 0
       && (await p.ev('Module.ccall("PortCardSeatProfiles","string",[],[])')).split('\n')[1].split('\t')[0] === 'ANNA');
    await shot(p, 'front-03b-p2-card-name-done.png');
    await press(p, 'B'); await waitFrames(p, 20);          // X: the game's B again
    ok('B (X) on the card after typing: back to PLAYERS', (await pagesModel(p)) === 5);
    await press(p, 'B'); await waitFrames(p, 20);
    ok('B goes back to the PLAYERS section', (await pagesModel(p)) === SEC_PLAYERS && await p.ev('Module._PortPuiActive()') === 0);
    await press(p, 'B'); await waitFrames(p, 20);
    ok('B goes back to SETTINGS', (await pagesModel(p)) === TOP);
    await press(p, 'B'); await waitFrames(p, 40);
    ok('B again leaves SETTINGS', !(await screenLive(p)));
    await shot(p, 'front-04-back-on-game-select.png');
    await p.close();
}

async function caseLocal() {
    say('local: drop in / drop out with four pads');
    const p = await openPage('local');
    await toGameSelect(p);
    await startPlay(p);
    ok('in story play', await p.ev('Module._PortLmpInStory()') === 1);
    await shot(p, 'local-00-one-player.png');
    // three pads arrive; nobody pressed anything yet
    await p.ev(`__padPlug(0, ${JSON.stringify(XBOX)}); __padPlug(1, ${JSON.stringify(XONE)}); __padPlug(2, ${JSON.stringify(DS)})`);
    await waitFrames(p, 10);
    ok('three pads seen', (await devList(p)).filter((d) => d.kind === 4 && d.present).length === 3);
    await press(p, 'RIGHT', 300); // player 1 walks (keyboard)
    // player 2: Start on pad 1
    await padPress(p, 0, 'START');
    await waitFrames(p, 6);
    const n2 = await notices(p);
    ok('player 2 joined by Start', (await human(p, 1)) === 1, JSON.stringify(n2));
    ok('notice: PLAYER 2 JOINED', n2.some((t) => /player 2 joined/i.test(t)));
    await shot(p, 'local-01-player2-joined.png');
    ok('the game is not paused by the join press', !(await screenLive(p)));
    // pad 2 presses A: waiting -> the hint
    await padPress(p, 1, 'A');
    await waitFrames(p, 10);
    const h1 = await hint(p);
    ok('join hint for a free pad', /PAD 2.*start to join/i.test(h1), h1);
    await shot(p, 'local-02-join-hint.png');
    ok("a waiting pad's A does not move player 1", (await p.ev('katamDevices.playerKeys(0)')) === 0);
    await padPress(p, 1, 'START');
    await waitFrames(p, 6);
    ok('player 3 joined', (await human(p, 2)) === 1);
    await padPress(p, 2, 'START');
    await waitFrames(p, 6);
    ok('player 4 joined', (await human(p, 3)) === 1);
    ok('four people', JSON.stringify(await humans(p)) === '[1,1,1,1]');
    await sleep(300);
    await shot(p, 'local-03-player4-joined.png');
    await waitFrames(p, 200);
    ok('the hint went away', (await hint(p)) === '');
    // everybody plays: each pad its own way, player 1 on the keyboard
    await keyHold(p, 'RIGHT', true);
    await p.ev('__padSet(0, [15]); __padSet(1, [14]); __padSet(2, [0])');
    await waitFrames(p, 20);
    const pk = await p.ev('[0,1,2,3].map(function(i){return katamDevices.playerKeys(i)})');
    ok('each player has their own keys', pk[0] === 16 && pk[1] === 16 && pk[2] === 32 && pk[3] === 1, JSON.stringify(pk));
    await keyHold(p, 'RIGHT', false);
    await p.ev('__padSet(0, []); __padSet(1, []); __padSet(2, [])');
    // spread out for the picture: all walk left, players 4, 3, 2 stop in turn
    await p.ev('__padSet(0, [14]); __padSet(1, [14]); __padSet(2, [14])');
    await keyHold(p, 'LEFT', true);
    await waitFrames(p, 25); await p.ev('__padSet(2, [0])');
    await waitFrames(p, 25); await p.ev('__padSet(1, [])');
    await waitFrames(p, 25); await p.ev('__padSet(0, [0])');
    await waitFrames(p, 20);
    await keyHold(p, 'LEFT', false);
    await shot(p, 'local-04-four-kirbys.png');
    await p.ev('__padSet(0, []); __padSet(1, []); __padSet(2, [])');
    await waitFrames(p, 30);
    // the middle player (3, pad 2) leaves by holding Select, and rejoins
    await p.ev('__padSet(1, [8])');
    await sleep(2600);
    await p.ev('__padSet(1, [])');
    await waitFrames(p, 6);
    ok('player 3 left by holding Select', (await human(p, 2)) === 0 && (await human(p, 3)) === 1);
    const n3 = await notices(p);
    ok('notice: PLAYER 3 LEFT', n3.some((t) => /player 3 left/i.test(t)), JSON.stringify(n3));
    await shot(p, 'local-05-player3-left.png');
    await padPress(p, 1, 'START');
    await waitFrames(p, 6);
    ok('player 3 rejoined on the same Kirby', (await human(p, 2)) === 1);
    // pad 1 (player 2) unplugged and back within the grace period
    await p.ev('__padUnplug(0)');
    await waitFrames(p, 10);
    const n4 = await notices(p);
    ok('notice: controller unplugged', n4.some((t) => /player 2: controller unplugged/i.test(t)), JSON.stringify(n4));
    await shot(p, 'local-06-unplugged.png');
    await waitFrames(p, 60);
    await p.ev(`__padPlug(0, ${JSON.stringify(XBOX)})`);
    await waitFrames(p, 10);
    ok('back in time: still player 2', (await human(p, 1)) === 1 && (await devList(p)).find((d) => d.label === 'PAD 1').player === 1);
    ok('notice: reconnected', (await notices(p)).some((t) => /player 2 reconnected/i.test(t)));
    // unplugged for good: the CPU takes the Kirby; plugged back, Start rejoins
    await p.ev('__padUnplug(0)');
    await waitFrames(p, 200);
    ok('gone too long: player 2 is the CPU again', (await human(p, 1)) === 0);
    ok('notice: left (unplugged)', (await notices(p)).some((t) => /player 2 left/i.test(t)));
    await p.ev(`__padPlug(0, ${JSON.stringify(XBOX)})`);
    await waitFrames(p, 10);
    ok('plugged back: parked', (await devList(p)).find((d) => d.label === 'PAD 1').player === -2);
    await padPress(p, 0, 'START');
    await waitFrames(p, 6);
    ok('Start rejoins as player 2', (await human(p, 1)) === 1);
    // a fifth controller (keyboard set B) presses Start: four already
    await press(p, 'NUMENTER');
    await waitFrames(p, 6);
    ok('notice: 4 players already', (await notices(p)).some((t) => /4 players already/i.test(t)));
    ok("the fifth Start does not pause player 1's game", !(await screenLive(p)));
    await shot(p, 'local-07-four-already.png');
    // a fifth pad too
    await p.ev(`__padPlug(3, 'Generic USB Gamepad')`);
    await waitFrames(p, 10);
    await padPress(p, 3, 'START');
    await waitFrames(p, 10);
    ok("a fifth pad's Start does not pause player 1's game", !(await screenLive(p)));
    await padPress(p, 3, 'RIGHT');
    ok("nor does its d-pad drive player 1", (await p.ev('katamDevices.playerKeys(0)')) === 0);

    // the pause menu's SETTINGS > PLAYERS
    ok('the pause SETTINGS page is up', await toPauseSettings(p));
    ok('... on its top level', (await pagesModel(p)) === PAUSE_TOP);
    await press(p, 'DOWN'); await waitFrames(p, 10);        // SOUND
    await press(p, 'DOWN'); await waitFrames(p, 10);        // PLAYERS
    await press(p, 'A'); await waitFrames(p, 20);
    ok('A opens the PLAYERS section', (await pagesModel(p)) === SEC_PLAYERS);
    // NAME TAGS (the third row): OFF / ON, Right turns them on
    await press(p, 'DOWN'); await waitFrames(p, 10);        // PARTY HUD
    await press(p, 'DOWN'); await waitFrames(p, 10);        // NAME TAGS
    {
        const was = await p.ev('Module._PortCardTagsOn()');
        await press(p, 'RIGHT'); await waitFrames(p, 10);
        ok('pause SETTINGS > NAME TAGS: Right turns them on', was === 0 && (await p.ev('Module._PortCardTagsOn()')) === 1);
        await shot(p, 'local-08b-pause-name-tags-row.png');
        await press(p, 'LEFT'); await waitFrames(p, 10);
        ok('... and Left off again', (await p.ev('Module._PortCardTagsOn()')) === 0);
    }
    // PARTY HUD: OFF / PLAYERS / ALL, Right steps it
    await press(p, 'UP'); await waitFrames(p, 10);          // PARTY HUD
    {
        const was = await p.ev('Module._PortPartyHudSettingJs()');
        await press(p, 'RIGHT'); await waitFrames(p, 10);
        const now = await p.ev('Module._PortPartyHudSettingJs()');
        ok('pause SETTINGS > PARTY HUD: Right steps the setting', now === (was + 1) % 3, `${was} -> ${now}`);
        await shot(p, 'local-08a-pause-party-hud-row.png');
        await press(p, 'LEFT'); await waitFrames(p, 10);
        ok('... and Left steps it back', (await p.ev('Module._PortPartyHudSettingJs()')) === was);
    }
    await press(p, 'UP'); await waitFrames(p, 10);          // PLAYERS
    await shot(p, 'local-08-pause-settings-players-row.png');
    await press(p, 'A'); await waitFrames(p, 20);
    ok('the pause PLAYERS page is up', (await pagesModel(p)) === 5);
    await shot(p, 'local-09-pause-players.png');
    // P2 -> CPU (drop out)
    await choose(p, 0, 1, 0);
    await waitFrames(p, 10);
    ok('PLAYERS page: player 2 dropped out', (await human(p, 1)) === 0);
    // P2 <- PAD 1, now waiting (parked): add it back as a player
    const pad1 = (await devList(p)).find((d) => d.label === 'PAD 1').id;
    await choose(p, 1, 1, pad1, false);
    await shot(p, 'local-10-pause-players-choose.png');
    await press(p, 'A'); await waitFrames(p, 30);
    ok('PLAYERS page: the waiting pad is player 2 again', (await human(p, 1)) === 1);
    // P4 <- PAD 2 (player 3's): player 3 leaves, the pad is player 4's
    const pad2 = (await devList(p)).find((d) => d.label === 'PAD 2').id;
    await choose(p, 1, 3, pad2);
    await waitFrames(p, 10);
    const dl = await devList(p);
    ok('PLAYERS page: PAD 2 moved to player 4, player 3 is the CPU',
       dl.find((d) => d.id === pad2).player === 3 && (await human(p, 2)) === 0 && (await human(p, 3)) === 1,
       JSON.stringify(await humans(p)));
    await shot(p, 'local-11-pause-players-moved.png');
    // A on a row with nothing chosen: that player's card (cursor on P4)
    await press(p, 'A'); await waitFrames(p, 20);
    ok('pause PLAYERS: A on P4 opens player 4\'s card', (await pagesModel(p)) === 6 && await screenLive(p));
    await shot(p, 'local-11a-pause-p4-card.png');
    await press(p, 'ARROWDOWN'); await waitFrames(p, 8);    // COLOR (WASD type letters on NAME)
    await press(p, 'RIGHT'); await waitFrames(p, 10);       // the first spray (PINK)
    await press(p, 'RIGHT'); await waitFrames(p, 10);       // YELLOW
    await shot(p, 'local-11b-pause-p4-card-yellow.png');
    await press(p, 'B'); await waitFrames(p, 30);
    ok('B on the card: back to PLAYERS on P4', (await pagesModel(p)) === 5);
    await press(p, 'B'); await waitFrames(p, 30);
    ok('B on the pause PLAYERS page: back to the PLAYERS section, still paused', (await pagesModel(p)) === SEC_PLAYERS && await screenLive(p));
    await press(p, 'B'); await waitFrames(p, 30);
    ok('B again: SETTINGS, still paused', (await pagesModel(p)) === PAUSE_TOP && await screenLive(p));
    await shot(p, 'local-12-pause-back-to-settings.png');
    await press(p, 'START'); await waitFrames(p, 60);
    ok('Start: back to the game', !(await screenLive(p)));
    // the card's colour went on when the card closed (a recorded paint)
    await waitFrames(p, 30);
    ok("player 4 wears the card's colour (YELLOW)", (await p.ev('Module._PortCardSlotPaintJs(3) >>> 0')) === 0x01000001,
       (await p.ev('Module._PortCardSlotPaintJs(3) >>> 0')).toString(16));
    // names on the party HUD's cards (presentation)
    await p.ev(`(function(){function set(c,v){var b=Module._malloc(64);for(var i=0;i<v.length;i++)Module.HEAPU8[b+i]=v.charCodeAt(i);Module.HEAPU8[b+v.length]=0;Module._PortCardSetJs(c,b);Module._free(b);}
        set(1,'1,6,0,0,0,ANNA');set(3,'1,1,0,0,0,DOT.');})()`);
    await waitFrames(p, 60);
    ok('party HUD: cards up for the other players', (await p.ev('Module._PortPartyHudCardsJs()')) >= 2, String(await p.ev('Module._PortPartyHudCardsJs()')));
    ok('party HUD names: P2 ANNA, P4 DOT.', (await p.ev('Module.ccall("PortPlayerName","string",["number"],[1])')) === 'ANNA'
       && (await p.ev('Module.ccall("PortPlayerName","string",["number"],[3])')) === 'DOT.');
    await shot(p, 'local-13-party-hud-names.png');
    // compact cards: a few seconds after the last change, tag + health only
    {
        const t0 = await frame(p);
        const full0 = await p.ev('Module._PortPartyHudFullJs()');
        await until(async () => (await p.ev('Module._PortPartyHudFullJs()')) === 0, 15000);
        const full1 = await p.ev('Module._PortPartyHudFullJs()');
        ok('party HUD: the cards close to tag + health when nothing changes', full1 === 0,
           `${full0} open, then ${full1} after ${(await frame(p)) - t0} frames`);
        await shot(p, 'local-14-party-hud-compact.png');
    }
    // name tags over the Kirbys (SETTINGS > NAME TAGS; the same switch)
    await p.ev('Module._PortCardSetTagsOn(1)');
    await waitFrames(p, 20);
    ok('name tags: one over each person\'s Kirby in the room', (await p.ev('Module._PortPartyHudTagsJs()')) >= 2,
       String(await p.ev('Module._PortPartyHudTagsJs()')));
    await shot(p, 'local-15-name-tags.png');
    await p.ev('Module._PortCardSetTagsOn(0)');
    await waitFrames(p, 5);
    ok('name tags off: none', (await p.ev('Module._PortPartyHudTagsJs()')) === 0);
    await p.close();
}

async function caseSolo() {
    say('solo: one person, a keyboard and a pad');
    const p = await openPage('solo');
    await toGameSelect(p);
    await startPlay(p);
    await p.ev(`__padPlug(0, ${JSON.stringify(XBOX)})`);
    await waitFrames(p, 10);
    await p.ev('__padSet(0, [0])');
    await waitFrames(p, 4);
    ok('the pad drives player 1', ((await p.ev('Module._PortCurrentKeys()')) & 1) === 1);
    await p.ev('__padSet(0, [])');
    await keyHold(p, 'RIGHT', true);
    await waitFrames(p, 4);
    ok('the keyboard drives player 1', ((await p.ev('Module._PortCurrentKeys()')) & 16) === 16);
    await keyHold(p, 'RIGHT', false);
    await waitFrames(p, 4);
    await p.ev('__padSet(0, [9])');
    await waitFrames(p, 3);
    const k = await p.ev('Module._PortCurrentKeys()');
    await p.ev('__padSet(0, [])');
    await waitFrames(p, 60);
    ok("the pad's Start is player 1's (it was just used): a pause, nobody joins",
       (k & 8) === 8 && JSON.stringify(await humans(p)) === '[1,0,0,0]', 'keys ' + k);
    await shot(p, 'solo-01-pad-start-paused.png');
    await press(p, 'START'); await waitFrames(p, 60);
    ok('no join hint for one person', (await hint(p)) === '');
    await p.close();
}

async function caseTouch() {
    say('touch: a phone, touch only');
    const p = await openPage('touch', { w: 390, h: 844, mobile: true, touch: true });
    await until(() => p.ev('!!(window.Module && Module.portRuntimeUp && Module._PortFrameNumber() > 30)'), 60000);
    await until(async () => (await frame(p)) >= 400, 60000, 30);
    const rect = async (id) => p.ev(`(function(){var r=document.getElementById('${id}').getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2,w:r.width}})()`);
    const tap = async (id, holdMs = 120) => {
        const r = await rect(id);
        await p.s('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: r.x, y: r.y, id: 1 }] });
        await sleep(holdMs);
        await p.s('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
        await sleep(150);
    };
    const st = await rect('tb-start');
    ok('the touch buttons are on screen', st.w > 0, JSON.stringify(st));
    await tap('tb-start'); await waitFrames(p, 100);
    await tap('tb-start'); await waitFrames(p, 100);
    await tap('tb-a'); await waitFrames(p, 100);           // Game Select
    await tap('tb-a'); await waitFrames(p, 60);
    await tap('tb-a'); await waitFrames(p, 60);            // ONE PLAYER
    await until(() => p.ev('Module._PortLmpInStory()'), 60000, 100);
    await waitFrames(p, 200);
    await p.ev('Module._PortWarp(0, 117, 44, 9)');
    await waitFrames(p, 120);
    await p.ev('(function(){var a=0x0203AD2C;Module.HEAPU8[a]=300&255;Module.HEAPU8[a+1]=300>>8;})()');
    await waitFrames(p, 60);
    const r = await rect('tb-a');
    await p.s('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: r.x, y: r.y, id: 1 }] });
    await waitFrames(p, 4);
    ok('touch drives player 1', ((await p.ev('Module._PortCurrentKeys()')) & 1) === 1);
    await shot(p, 'touch-01-phone-play.png');
    await p.s('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
    await waitFrames(p, 10);
    const rs = await rect('tb-start');
    await p.s('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: rs.x, y: rs.y, id: 1 }] });
    await waitFrames(p, 3);
    const k = await p.ev('Module._PortCurrentKeys()');
    await p.s('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
    await waitFrames(p, 60);
    ok("touch's Start is player 1's (a pause), never a join", (k & 8) === 8 && JSON.stringify(await humans(p)) === '[1,0,0,0]', 'keys ' + k);
    const td = (await devList(p)).find((d) => d.kind === 3);
    ok('touch is a device that drives player 1', td && td.player === -1);
    ok('no join hint on a phone', (await hint(p)) === '');
    await shot(p, 'touch-02-phone-paused.png');
    await p.close();
}

// The renderer the page really uses: WebGL2 (Module.portGl), not the 2D
// fallback a Chromium without GL gives.
async function caseGl() {
    const p = await openPage('gl');
    await until(() => p.ev('!!(window.Module && Module.portRuntimeUp && Module._PortFrameNumber() > 30)'), 60000);
    ok('the page runs on WebGL2 (Module.portGl)', await p.ev('!!Module.portGl'));
    const wasm = await p.ev(`fetch(performance.getEntriesByType('resource').map(function(r){return r.name}).filter(function(n){return /katam\.wasm/.test(n)})[0]).then(function(r){return r.arrayBuffer()}).then(function(b){return crypto.subtle.digest('SHA-256', b)}).then(function(h){return Array.from(new Uint8Array(h)).map(function(x){return x.toString(16).padStart(2,'0')}).join('')})`);
    const mine = (await import('crypto')).createHash('sha256').update(fs.readFileSync(path.join(ROOT, 'build/dist/katam.wasm'))).digest('hex');
    ok('the served katam.wasm is this build', wasm === mine, wasm.slice(0, 12) + ' vs ' + mine.slice(0, 12));
    await p.close();
}

// Local multiplayer from the game's own menu and from a recording.
async function caseLmp() {
    say('lmp: START GAME > MULTIPLAYER > LOCAL, keyboard only');
    for (const withPad of [false, true]) {
        const p = await openPage(withPad ? 'lmp-pad' : 'lmp-kb');
        await toGameSelect(p);
        if (withPad) { await p.ev(`__padPlug(0, ${JSON.stringify(XBOX)})`); await waitFrames(p, 5); }
        await press(p, 'A'); await waitFrames(p, 60);      // the pop-up
        await press(p, 'DOWN'); await waitFrames(p, 20);   // MULTIPLAYER
        await press(p, 'A'); await waitFrames(p, 80);      // the MULTIPLAYER screen, LOCAL
        await press(p, 'A');
        const inPlay = await until(() => p.ev('Module._PortLmpInPlay()'), 60000, 200);
        await waitFrames(p, 60);
        const tag = withPad ? 'with a pad' : 'keyboard only';
        ok(`LOCAL starts a two-player game (${tag})`, !!inPlay && (await human(p, 1)) === 1, JSON.stringify(await humans(p)));
        const d = (await devList(p)).find((x) => x.player === 1);
        ok(`player 2 has a controller (${tag})`, !!d && d.label === (withPad ? 'PAD 1' : 'KEYS 2'), d && d.label);
        if (withPad) { await p.ev('__padSet(0, [15])'); await waitFrames(p, 4); }
        else await keyHold(p, 'RIGHT', false);
        if (withPad) ok('the pad drives player 2', (await p.ev('katamDevices.playerKeys(1)')) === 16);
        await shot(p, `lmp-${withPad ? 'pad' : 'kb'}-two-players.png`);
        await p.close();
    }
    await caseReplay();
}

async function caseReplay() {
    for (const menus of ['0', '']) {
        say(`lmp: a page replay of localmp_tour.txt (option localmp 2)${menus ? ', ?menus=0' : ', menus on'}`);
        const text = fs.readFileSync(path.join(ROOT, 'tests/recordings/localmp_tour.txt'), 'utf8');
        const early = `(function(){var T=${JSON.stringify(text)};var t=setInterval(function(){var M=window.Module;
          if(!M||!M.portRuntimeUp||!M._PortInputLogLoad)return;clearInterval(t);
          var n=T.length,ptr=M._malloc(n+1);for(var i=0;i<n;i++)M.HEAPU8[ptr+i]=T.charCodeAt(i);M.HEAPU8[ptr+n]=0;
          window.__replayLoadedAt=M._PortFrameNumber();window.__replay=M._PortInputLogLoad(ptr,n);},1);})();`;
        const p = await openPage('lmp-replay' + menus, { extra: menus ? { menus } : {}, early });
        await until(() => p.ev('window.__replay !== undefined'), 60000, 50);
        const at = await p.ev('window.__replayLoadedAt');
        await until(async () => (await frame(p)) >= 1500, 120000, 200);
        const two = await p.ev('Module._PortLmpInPlay()');
        ok(`replay of option localmp 2 plays two players${menus ? ' (?menus=0)' : ' (menus on)'}`, two === 1,
           `loaded at frame ${at}, humans ${await p.ev('Module._PortLmpSlotIsHuman ? JSON.stringify([0,1,2,3].map(function(s){return Module._PortLmpSlotIsHuman(s)})) : "(no export)"')}`);
        await shot(p, `lmp-replay${menus ? '-menus0' : '-menus1'}.png`);
        await p.close();
    }
}

// Where the cards go: two windows, three pads joined in each.
async function caseHudPlaces() {
    for (const v of [{ tag: 'wide', w: 1440, h: 640, settings: 'aspect=1\nnametags=1\n' },
                     { tag: 'phone', w: 844, h: 390, mobile: true, touch: true, settings: 'nametags=1\n' }]) {
        say(`hudplaces: ${v.tag} ${v.w}x${v.h}`);
        const early = `try{localStorage.setItem('katam.settings',${JSON.stringify(v.settings)})}catch(e){}`;
        const p = await openPage('hud-' + v.tag, { w: v.w, h: v.h, mobile: !!v.mobile, touch: !!v.touch, early });
        await toGameSelect(p);
        await startPlay(p);
        // player 2 wears a custom brown (no spray is): the party HUD's little
        // Kirby must be that brown too, not the nearest spray
        await p.ev(`(function(){var v='2,0,25,75,55,BRUNO',b=Module._malloc(64);for(var i=0;i<v.length;i++)Module.HEAPU8[b+i]=v.charCodeAt(i);Module.HEAPU8[b+v.length]=0;Module._PortCardSetJs(1,b);Module._free(b);})()`);
        await p.ev(`__padPlug(0, ${JSON.stringify(XBOX)}); __padPlug(1, ${JSON.stringify(XONE)}); __padPlug(2, ${JSON.stringify(DS)})`);
        await waitFrames(p, 10);
        if (!v.touch) await press(p, 'RIGHT', 200);
        for (let i = 0; i < 3; i++) { await padPress(p, i, 'START'); await waitFrames(p, 10); }
        await p.ev('__padSet(0, [15]); __padSet(1, [14])');
        await waitFrames(p, 40);
        await p.ev('__padSet(0, []); __padSet(1, [])');
        await waitFrames(p, 30);
        const at = await p.ev('[0,1,2,3,4,5].map(function(i){return Module._PortPartyHudAtJs(i)})');
        await shot(p, `hud-${v.tag}-open.png`);
        ok(`${v.tag}: player 2 wears the card's custom colour`, ((await p.ev('Module._PortCardSlotPaintJs(1) >>> 0')) >>> 24) === 2,
           (await p.ev('Module._PortCardSlotPaintJs(1) >>> 0')).toString(16));
        if (v.tag === 'wide') {
            ok('wide window, 3:2 picture: the cards sit in the bar beside it', at[4] === 1, JSON.stringify(at));
        } else {
            // the touch pads take the bars: the cards stay over the picture,
            // pushed clear of every keep-out
            const keep = await p.ev(`(function(){var c=Module.portGlCanvas,cr=c.getBoundingClientRect(),sx=c.width/cr.width,sy=c.height/cr.height,o=[];
              ['dpad','tb-a','tb-b','tb-l','tb-r','tb-select','tb-start'].forEach(function(id){var e=document.getElementById(id);if(!e)return;var r=e.getBoundingClientRect();
              if(r.width>0&&r.height>0)o.push([(r.left-cr.left)*sx,(r.top-cr.top)*sy,r.width*sx,r.height*sy])});return o})()`);
            const hit = keep.some((k) => at[0] < k[0] + k[2] && k[0] < at[0] + at[2] && at[1] < k[1] + k[3] && k[1] < at[1] + at[3]);
            ok('phone: the cards are clear of the touch controls', at[2] > 0 && !hit, JSON.stringify(at) + ' vs ' + keep.length + ' controls');
        }
        ok(`${v.tag}: name tags over the players' Kirbys`, (await p.ev('Module._PortPartyHudTagsJs()')) >= 2);
        await until(async () => (await p.ev('Module._PortPartyHudFullJs()')) === 0, 15000);
        await waitFrames(p, 5);
        await shot(p, `hud-${v.tag}-quiet.png`);
        const drawn = await p.ev('Module._PortPartyHudAtJs(5)');
        if (v.tag === 'wide')
            ok('wide: in the bar the cards stay whole (they cost the picture nothing)', drawn === 3, String(drawn));
        else
            ok('phone: over the picture they close to tag + health after a few quiet seconds', drawn === 0, String(drawn));
        await p.close();
    }
}

// --------------------------------------------------------------------------
async function casePicker() {
    const prof0 = async (p) => (await p.ev('Module.ccall("PortCardSeatProfiles","string",[],[])')).split('\n')[0].split('\t');
    const edit = (p, w) => p.ev(`Module._PortCardEditJs(${w}) >>> 0`);
    const presetRgb = { OLIVE: 0x808020 };
    // the card's cursor down to COLOR: past NAME (the row that takes text)
    const toColorRow = async (p) => {
        await waitFrames(p, 30);
        for (let n = 0; n < 6 && !(await p.ev('Module._PortCardWantsText()')); n++) { await press(p, 'DOWN'); await waitFrames(p, 10); }
        await press(p, 'DOWN'); await waitFrames(p, 10);
        return !(await p.ev('Module._PortCardWantsText()'));
    };
    // the grid's cursor to `cell` (8 a row): Up/Down to its row, Left/Right to its column
    const gridTo = async (p, cell) => {
        for (let n = 0; n < 12 && Math.floor((await edit(p, 3)) / 8) !== Math.floor(cell / 8); n++) { await press(p, 'DOWN', 60, 90); await waitFrames(p, 3); }
        for (let n = 0; n < 12 && (await edit(p, 3)) % 8 !== cell % 8; n++) { await press(p, 'RIGHT', 60, 90); await waitFrames(p, 3); }
        return (await edit(p, 3)) === cell;
    };
    for (const v of [{ tag: 'phone', w: 844, h: 390, mobile: true, touch: true }, { tag: 'wide', w: 1440, h: 800 }]) {
        say(`picker (${v.tag}): the player card's colour by parts`);
        const p = await openPage('pick-' + v.tag, v);
        await toGameSelect(p);
        await press(p, 'A'); await waitFrames(p, 60);          // START GAME: the pop-up
        await press(p, 'DOWN'); await waitFrames(p, 10);
        await press(p, 'A'); await waitFrames(p, 60);          // MULTIPLAYER
        await until(() => screenLive(p), 10000);
        await press(p, 'DOWN'); await waitFrames(p, 8);
        await press(p, 'DOWN'); await waitFrames(p, 8);
        await press(p, 'A'); await waitFrames(p, 30);          // PLAYER CARD
        ok(`${v.tag}: MULTIPLAYER > PLAYER CARD opens the card`, (await pagesModel(p)) === 6);
        await toColorRow(p);
        await shot(p, `picker-${v.tag}-01-card.png`);
        await press(p, 'A'); await waitFrames(p, 30);
        ok(`${v.tag}: A on COLOR opens the picker (BODY / FEET)`, (await pagesModel(p)) === 7);
        ok(`${v.tag}: the game's colour came in as its spray (PINK), feet with the body`,
           (await edit(p, 0)) === 0x01000000 && (await edit(p, 1)) === 0, (await edit(p, 0)).toString(16));
        await shot(p, `picker-${v.tag}-02-pick.png`);
        // BODY's grid: the cursor on PINK (cell 8), two Rights: CORAL
        await press(p, 'A'); await waitFrames(p, 10);
        ok(`${v.tag}: A on BODY opens the grid on its cell`, (await edit(p, 2)) === 1 && (await edit(p, 3)) === 8);
        await press(p, 'RIGHT'); await waitFrames(p, 6);
        await press(p, 'RIGHT'); await waitFrames(p, 6);
        ok(`${v.tag}: the Kirby wears the cell under the cursor (CORAL)`, (await edit(p, 0)) === 0x02FF8A80,
           (await edit(p, 0)).toString(16));
        ok(`${v.tag}: ... not yet kept`, (await prof0(p))[1] === '01000000', (await prof0(p)).join('|'));
        await shot(p, `picker-${v.tag}-03-grid-body.png`);
        await press(p, 'DOWN'); await waitFrames(p, 6);        // row 3 of the grid: OLIVE
        await press(p, 'A'); await waitFrames(p, 10);
        ok(`${v.tag}: A keeps it: body OLIVE, the grid closed`, (await edit(p, 2)) === 0
           && (await prof0(p))[1] === (0x02000000 + presetRgb.OLIVE).toString(16).toUpperCase().padStart(8, '0'),
           (await prof0(p)).join('|'));
        // FEET: Right steps off BODY onto the first preset (RED)
        await press(p, 'DOWN'); await waitFrames(p, 8);
        await press(p, 'RIGHT'); await waitFrames(p, 8);
        const red = await edit(p, 1);
        ok(`${v.tag}: Right on FEET: feet of their own (RED)`, red >= 0x8000 && (await prof0(p))[2] === red.toString(16).toUpperCase(),
           (await prof0(p)).join('|'));
        await press(p, 'A'); await waitFrames(p, 10);          // the grid on RED (cell 0)
        await press(p, 'LEFT'); await waitFrames(p, 6);        // GRAPE, wrapped
        ok(`${v.tag}: the feet follow the cursor (GRAPE)`, (await edit(p, 3)) === 7 && (await edit(p, 1)) !== red);
        await shot(p, `picker-${v.tag}-04-grid-feet.png`);
        await press(p, 'B'); await waitFrames(p, 10);
        ok(`${v.tag}: B puts the feet back (RED)`, (await edit(p, 2)) === 0 && (await edit(p, 1)) === red);
        await press(p, 'A'); await waitFrames(p, 10);
        for (let i = 0; i < 3; i++) { await press(p, 'UP'); await waitFrames(p, 5); }   // row 1 of the grid (wraps)
        for (let i = 0; i < 4; i++) { await press(p, 'RIGHT'); await waitFrames(p, 5); }
        await press(p, 'A'); await waitFrames(p, 10);          // cell 12: CREAM
        const cream = await edit(p, 1);
        ok(`${v.tag}: feet CREAM kept`, (await edit(p, 3)) === 12 && cream !== red && (await prof0(p))[2] === cream.toString(16).toUpperCase(),
           (await prof0(p)).join('|'));
        await shot(p, `picker-${v.tag}-05-feet-kept.png`);
        const store = await p.ev('JSON.stringify(Object.keys(localStorage).map(function(k){return localStorage.getItem(k)}))');
        ok(`${v.tag}: saved with the settings (card0parts=19,13)`, /card0parts=19,13/.test(store));
        // Right on FEET until BODY again (13 -> 33 -> 0)
        for (let i = 0; i < 20; i++) { await press(p, 'RIGHT', 60, 90); await waitFrames(p, 3); }
        ok(`${v.tag}: FEET back to BODY`, (await edit(p, 1)) === 0 && (await prof0(p)).length === 2, (await prof0(p)).join('|'));
        for (let i = 0; i < 12; i++) { await press(p, 'LEFT', 60, 90); await waitFrames(p, 3); }   // and CREAM again
        ok(`${v.tag}: Left from BODY steps onto the presets from the end`, (await edit(p, 1)) !== 0);
        await press(p, 'B'); await waitFrames(p, 20);
        ok(`${v.tag}: B: back to the card`, (await pagesModel(p)) === 6);
        await press(p, 'B'); await waitFrames(p, 20);
        ok(`${v.tag}: B: the card closed`, (await p.ev('Module._PortCardSub()')) === 0);
        if (v.tag === 'wide') {
            // an old one-colour card (as the last build saved it) reads as it did
            await p.ev(`(function(){var v='2,0,200,80,90,OLD',b=Module._malloc(64);for(var i=0;i<v.length;i++)Module.HEAPU8[b+i]=v.charCodeAt(i);Module.HEAPU8[b+v.length]=0;Module._PortCardSetJs(0,b);Module._free(b);})()`);
            const old = await prof0(p);
            ok('an old card: one colour, no feet', old.length === 2 && /^02/.test(old[1]), old.join('|'));
            await press(p, 'A'); await waitFrames(p, 30);
            await toColorRow(p);
            await press(p, 'A'); await waitFrames(p, 30);
            ok('an old card opens on its own colour (BODY OWN), unchanged', (await pagesModel(p)) === 7
               && ((await edit(p, 0)) >>> 0).toString(16).toUpperCase().padStart(8, '0') === old[1]);
            await shot(p, `picker-${v.tag}-06-old-card.png`);
            // body ORANGE from the grid, feet TEAL: then play in it
            await press(p, 'A'); await waitFrames(p, 8);
            await gridTo(p, 1);
            await press(p, 'A'); await waitFrames(p, 8);
            await press(p, 'DOWN'); await waitFrames(p, 8);
            await press(p, 'A'); await waitFrames(p, 8);
            await gridTo(p, 20);
            await press(p, 'A'); await waitFrames(p, 8);
            ok('ORANGE body (the spray) and TEAL feet', (await edit(p, 0)) === 0x0100000A && (await edit(p, 1)) === 0xC622,
               (await edit(p, 0)).toString(16) + ' ' + (await edit(p, 1)).toString(16));
            await shot(p, `picker-${v.tag}-07-orange-teal.png`);
            await press(p, 'B'); await waitFrames(p, 20);
            await press(p, 'B'); await waitFrames(p, 20);
            await press(p, 'B'); await waitFrames(p, 40);       // MULTIPLAYER -> Game Select
            await startPlay(p);
            await until(async () => ((await p.ev('Module._PortCardSlotFeetJs(0) >>> 0')) === 0xC622), 10000);
            ok('in play, player 1 wears it: the spray and the feet (a recorded paint)',
               (await p.ev('Module._PortCardSlotPaintJs(0) >>> 0')) === 0x0100000A && (await p.ev('Module._PortCardSlotFeetJs(0) >>> 0')) === 0xC622);
            await waitFrames(p, 60);
            await shot(p, `picker-${v.tag}-08-play.png`);
        }
        await p.close();
    }
}

for (const c of CASES) {
    try {
        if (c === 'picker') await casePicker();
        if (c === 'gl') await caseGl();
        else if (c === 'lmp') await caseLmp();
        else if (c === 'replay') await caseReplay();
        if (c === 'frontend') await caseFrontend();
        else if (c === 'local') await caseLocal();
        else if (c === 'solo') await caseSolo();
        else if (c === 'touch') await caseTouch();
        else if (c === 'hudplaces') await caseHudPlaces();
    } catch (e) {
        ok(`case ${c} ran to the end`, false, e.message);
    }
}
console.log(`\n${pass} passed, ${fail} failed (${((Date.now() - T0) / 1000).toFixed(0)} s)`);
process.exit(fail ? 1 : 0);
