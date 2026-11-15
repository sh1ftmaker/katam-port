// Online play end to end in a real browser (web/online.js): two or three
// headless Chromium pages on build/dist, a room server, the game's own menus.
//
//   node tools/online_browser_test.mjs --cdp 9351 --base http://127.0.0.1:8941/dist/
//        --rom-url http://127.0.0.1:8941/rom.gba --relay http://127.0.0.1:8797
//        --shots DIR [--play 130] [--lag 120 --jitter 30 --loss 2]
//        [--cases menu,invite,drop,relay,errors,three] [--live]
//
// Needs: a Chromium already listening on --cdp with its own profile (never
// the shared one; on this machine `--headless=new --ignore-gpu-blocklist
// --use-angle=gl-egl` gives WebGL2 -- SwiftShader flags give none, and the
// page then falls back to its 2D canvas; every page is checked for
// Module.portGl), ports nobody else uses (check that the served katam.wasm
// is your build's), a static server for the dist and for a ROM you own (served
// from outside the tree), and a room server: netplay/dev-relay.mjs locally,
// or --live for the deployed worker (normal client use: one room, joined).
// Screenshots are ROM-derived: write them outside the tree.
//
// What it checks (from window.katamOnline.stats() and the engine's own
// hashes, compared here frame by frame across the two pages):
//   menu    host through Start Game > MULTIPLAYER > ONLINE > HOST GAME, guest
//           through JOIN GAME typing the code with the pad; two minutes of
//           scripted play with artificial latency, doors led by each side;
//           both progressing, data channels open, every compared frame
//           identical (repairs reported), guest's own-input override rate.
//   drop    the guest's page goes away; the host keeps playing; a new guest
//           joins with the same code (by invite link) and is identical again.
//   invite  a guest joining by ?join=CODE (the drop case's rejoin is one).
//   relay   ?netrelay=1: the same over the room's signal path.
//   errors  JOIN with a code nobody has: NO SUCH GAME.
//   three   host + two guests (one through JOIN GAME, one by invite link):
//           the HOST screen counts them (PLAYERS 3/4), play with latency,
//           every frame of both guests compared with the host's; a guest
//           drops (its page closes) and its Kirby goes to the CPU; a local
//           join on the host (a second player on the host's keyboard: seat
//           1, katamOnline.requestSlot); the dropped guest rejoins by invite
//           and gets a Kirby (now four players); a local join on a guest is
//           refused (full) with a notice; the host's second player leaves
//           and a pad on that guest joins by Start (the device module's
//           seat path) and gets that Kirby, drives it, and leaves by holding
//           Select.  Player cards: every device's names and colours (a
//           guest's custom colour too) the same on all three pages, through
//           joins, the drop, the rejoin and the pad's player; the party HUD
//           up.  Then leaving without a reload: guest B leaves from its menu
//           (back to power-on), joins again and is identical; the host ends
//           the game, guest A gets a "Back to the title" button; the host
//           hosts again through the game's menus and guest A joins again.
//           Scripted local players' keys go in through
//           Module._PortLmpSetHostKeys(seat, ...); the pad through the page.
//   rooms   FREE ROAM (rooms mode) with two and three pages (--rooms-pages
//           2,3): each page on its own player's room, the canonical frames
//           identical, the digest equal, a phone call, the pause menu.
import fs from 'fs';
import path from 'path';
import { human, ROOMS } from './netproof/scenario.mjs';

const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf('--' + n); return i >= 0 ? argv[i + 1] : d; };
const has = (n) => argv.includes('--' + n);
const CDP_PORT = +flag('cdp', 9351);
const BASE = flag('base', 'http://127.0.0.1:8941/dist/');
const ROM_URL = flag('rom-url', 'http://127.0.0.1:8941/rom.gba');
const LIVE = has('live');
const RELAY = LIVE ? null : flag('relay', 'http://127.0.0.1:8797');
const SHOTS = flag('shots', '');
const PLAY_S = +flag('play', 130);
const LAG = +flag('lag', 120), JITTER = +flag('jitter', 30), LOSS = +flag('loss', 2);
const CASES = flag('cases', 'errors,menu,drop,relay,three').split(',');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let pass = 0, fail = 0;
const results = {};
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
const logs = {};
cdp.handlers.push((m) => {
    const who = Object.keys(pages).find((k) => pages[k].sessionId === m.sessionId) || '?';
    if (m.method === 'Runtime.consoleAPICalled') {
        const t = m.params.args.map((a) => a.value ?? a.description ?? '').join(' ');
        (logs[who] = logs[who] || []).push(t);
        if (/\[online\]|error|exception|abort/i.test(t) && !/dbus|favicon/i.test(t)) console.log(`    ${who}: ${t.slice(0, 220)}`);
    }
    if (m.method === 'Runtime.exceptionThrown')
        console.log(`    ${who} exception: ` + JSON.stringify(m.params.exceptionDetails).slice(0, 400));
});
const pages = {};

function pageUrl(extra) {
    const u = new URL(BASE + 'index.html');
    u.searchParams.set('rom', ROM_URL);
    if (RELAY) u.searchParams.set('netserver', RELAY);
    for (const [k, v] of Object.entries(extra || {})) u.searchParams.set(k, v);
    return u.toString();
}

async function openPage(name, extra, w = 960, h = 640) {
    const { browserContextId } = await cdp.send('Target.createBrowserContext');
    const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank', browserContextId, newWindow: true });
    const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
    const s = (m, p) => cdp.send(m, p, sessionId);
    const p = { name, s, targetId, sessionId, ctx: browserContextId,
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
    await s('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile: false });
    await s('Page.addScriptToEvaluateOnNewDocument', { source: PAGE_HOOK });
    await s('Page.addScriptToEvaluateOnNewDocument', { source: PAD_HOOK });
    if (has('noaudio')) await s('Page.addScriptToEvaluateOnNewDocument', { source: NOAUDIO_HOOK });
    await s('Page.navigate', { url: pageUrl(extra) });
    return p;
}

// --noaudio: no audio device at all in either page (as the node rig runs):
// the shell's Module gets a silent transport before the module opens one.
const NOAUDIO_HOOK = `(function(){
  var M;
  Object.defineProperty(window, 'Module', { configurable: true, get: function () { return M; },
    set: function (v) { M = v; if (v && typeof v === 'object' && !v.portAudio)
      v.portAudio = { rate: 0, depth: 0, headless: true, push: function () {} }; } });
})();`;

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

// In every page: a scripted pad (window.__script(frame) -> mask, applied
// through the page's own key path once per presented frame) and a log of
// what the page's session reports, for the test to read.
const PAGE_HOOK = `(function(){
  var t = setInterval(function(){
    var M = window.Module;
    if (!M || !M.portRuntimeUp || !M._PortSetKeys || !M.portPresent || M.__hooked) return;
    clearInterval(t);
    M.__hooked = true;
    var present = M.portPresent;
    M.portPresent = function (a, b, c) {
      if (present) present(a, b, c);
      if (window.__script) {
        try { M._PortSetKeys(window.__script(M._PortFrameNumber()) & 0x3FF); } catch (e) { console.log('script: ' + e); window.__script = null; }
      }
      if (window.__seats) {
        try { var k = window.__seats(M._PortFrameNumber()); for (var s = 1; s < 4; s++) M._PortLmpSetHostKeys(s, (k[s] || 0) & 0x3FF); }
        catch (e) { console.log('seats: ' + e); window.__seats = null; }
      }
    };
  }, 20);
})();`;

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
const KEYS = { A: ['KeyZ', 'z', 90], B: ['KeyX', 'x', 88], START: ['Enter', 'Enter', 13], UP: ['ArrowUp', 'ArrowUp', 38],
               DOWN: ['ArrowDown', 'ArrowDown', 40], F1: ['F1', 'F1', 112], SELECT: ['Backspace', 'Backspace', 8], LEFT: ['ArrowLeft', 'ArrowLeft', 37], RIGHT: ['ArrowRight', 'ArrowRight', 39] };
async function press(p, k, holdMs = 90, gapMs = 160) {
    const [code, key, vk] = KEYS[k];
    await p.s('Input.dispatchKeyEvent', { type: 'keyDown', code, key, windowsVirtualKeyCode: vk });
    await sleep(holdMs);
    await p.s('Input.dispatchKeyEvent', { type: 'keyUp', code, key, windowsVirtualKeyCode: vk });
    await sleep(gapMs);
}
const frame = (p) => p.ev('Module._PortFrameNumber()');
const stats = (p) => p.ev('window.katamOnline ? katamOnline.stats() : null');
const screen = (p) => p.ev('Module._PortOuiScreen ? Module._PortOuiScreen() : -1');
const waitFrames = async (p, n) => { const f0 = await frame(p); await until(async () => (await frame(p)) >= f0 + n, 60000, 50); };

/* power-on to Game Select, FILE 1 (a fresh profile: an empty file) */
/* every page must run the WebGL2 path (the developer menu, the HUD): a page
 * that fell back to the 2D canvas is not the page players get */
const glSeen = new Set();
async function glCheck(p) {
    if (glSeen.has(p)) return;
    glSeen.add(p);
    await until(() => p.ev('!!(window.Module && Module.portRuntimeUp)'), 60000);
    ok(`${p.name}: WebGL2 renderer (Module.portGl)`, !!(await p.ev('!!Module.portGl')));
}
async function toGameSelect(p) {
    await until(() => p.ev('!!(window.Module && Module.portRuntimeUp && Module._PortFrameNumber() > 30)'), 60000);
    await glCheck(p);
    await until(async () => (await frame(p)) >= 400, 60000, 30);
    await press(p, 'START'); await waitFrames(p, 100);     // the intro -> the title
    await press(p, 'START'); await waitFrames(p, 100);     // the title -> file select
    p.fsPrint = await fsPrint(p);                          // what FILE SELECT looks like (to find it again)
    await press(p, 'A'); await waitFrames(p, 100);         // FILE 1 -> Game Select
}
/* a screen's fingerprint: the first BG palette lines and DISPCNT */
// (DISPCNT and BG0-2CNT: BG3CNT is off on FILE SELECT and keeps whatever the
// screen before set -- the intro's on the way in, nothing after leaving an
// online game, which goes from the logo straight to the title)
const fsPrint = (p) => p.ev('Array.from(Module.HEAPU8.subarray(0x05000000, 0x05000040)).join(",") + "|" + Array.from(Module.HEAPU8.subarray(0x04000000, 0x0400000E)).join(",") + "|" + Array.from(Module.HEAPU8.subarray(0x05000200, 0x05000240)).join(",")');

/* Game Select > Start Game > MULTIPLAYER > ONLINE */
async function toOnline(p) {
    await press(p, 'A'); await waitFrames(p, 60);          // START GAME: the pop-up
    await press(p, 'DOWN'); await waitFrames(p, 20);       // MULTIPLAYER
    await press(p, 'A'); await waitFrames(p, 80);          // the MULTIPLAYER screen
    await press(p, 'DOWN'); await waitFrames(p, 20);       // ONLINE
    await press(p, 'A'); await waitFrames(p, 40);          // the ONLINE screen
}

const ALPHA = 'BCDFGHJKMNPQRSTVWXZ';
const KEY = { A: 1, B: 2, SELECT: 4, START: 8, RIGHT: 16, LEFT: 32, UP: 64, DOWN: 128, R: 256, L: 512 };

/* JOIN GAME's CODE row: Left/Right pick the letter, A the next one; A on
 * the last letter joins */
async function typeCode(p, code) {
    for (const ch of code) {
        const i = ALPHA.indexOf(ch);
        if (i <= ALPHA.length / 2) for (let k = 0; k < i; k++) await press(p, 'RIGHT', 60, 90);
        else for (let k = 0; k < ALPHA.length - i; k++) await press(p, 'LEFT', 60, 90);
        await press(p, 'A');
    }
}

/* --- the play script: keys per session frame for each player, and the
 * host's warps (sealed events), all relative to the session's first frame */
const M_ = { A: 1, B: 2, s: 4, S: 8, R: 16, L: 32, U: 64, D: 128, r: 256, l: 512 };
function parseKeys(script, keys, warps, player, shift) {
    let f = 0;
    const mask = (x) => [...x].reduce((m, ch) => m | M_[ch], 0);
    for (const tok of script.split(/\s+/).filter(Boolean)) {
        if (tok.startsWith('@')) f = +tok.slice(1) + shift;
        else if (tok.startsWith('warp:')) { const [, r, x, y] = tok.split(':').map(Number); warps.push([f, player, r, x, y]); f += 1; }
        else if (tok.startsWith('h') && tok.includes(':')) { const [n, k] = tok.slice(1).split(':'); keys.push([f, mask(k), +n]); f += +n + 6; }
        else { keys.push([f, mask(tok), 6]); f += 12; }
    }
}
function buildPlay(frames, seed, doorsAt) {
    const k1 = new Array(frames).fill(0), k2 = new Array(frames).fill(0), warps = [];
    const h1 = human(seed * 2 + 1), h2 = human(seed * 2 + 2);
    for (let f = 0; f < frames; f++) { k1[f] = h1(f) & ~(KEY.START | KEY.R); k2[f] = h2(f) & ~(KEY.START | KEY.R); }
    // the rig's door segment (tools/netproof/scenario.mjs A: doors in two
    // areas, led by each player): taps of Up in front of doors after warps
    const d1 = [], d2 = [], shift = doorsAt - 1300;
    parseKeys(`@1300 warp:117:44:9 @1340 h8:R U U U U @1440 h140:R @1590 h140:L
               @1950 warp:203:60:5 @2060 h30:L U U U U`, d1, warps, 0, shift);
    parseKeys(`@1750 h30:L U U U U @1990 h10:R U U U U @2150 h120:L`, d2, warps, 1, shift);
    for (let f = doorsAt; f < doorsAt + 1100 && f < frames; f++) { k1[f] = 0; k2[f] = 0; }
    for (const [f, m, n] of d1) for (let i = 0; i < n && f + i < frames; i++) k1[f + i] = m;
    for (const [f, m, n] of d2) for (let i = 0; i < n && f + i < frames; i++) k2[f + i] = m;
    // then a warp every 40 s through level rooms, as the rig's free play
    for (let g = doorsAt + 1200, i = 0; g < frames; g += 2400, i++) {
        const r = ROOMS[(seed * 7 + i * 5) % ROOMS.length];
        warps.push([g, 0, r[0], r[1], r[2]]);
    }
    return { k1, k2, warps };
}
async function setScript(p, keys, base, warps) {
    await p.ev(`(function(){
        var K = ${JSON.stringify(keys)}, B = ${base}, W = ${JSON.stringify(warps || [])}, wi = 0;
        window.__script = function (f) {
            var r = f - B;
            while (wi < W.length && W[wi][0] <= r) { var w = W[wi++]; Module._PortLnEvent(1, w[1], w[2], w[3], w[4]); }
            return r >= 0 && r < K.length ? K[r] : 0;
        };
    })()`);
}

/* every hashed frame final on the guest against the host's hash of it */
function makeComparer(host, guest) {
    const st = { compared: 0, equal: 0, mismatched: 0, first: null, from: 0, diffs: [] };
    st.run = async () => {
        const gs = await stats(guest);
        if (!gs || !gs.session) return st;
        const to = gs.frame;
        if (!st.from) st.from = Math.max(gs.frame - 600, 0);
        const g = await guest.ev(`katamOnline.hashes(${st.from}, ${to})`);
        const h = await host.ev(`katamOnline.hashes(${st.from}, ${to})`);
        const hm = new Map(h.map((x) => [x[0], x]));
        let last = st.from;
        for (const x of g) {
            if (!x[3]) continue;            // not final (yet, or before a repair's floor)
            const y = hm.get(x[0]);
            last = x[0] + 1;
            if (!y) continue;
            st.compared++;
            if (x[1] === y[1] && x[2] === y[2]) st.equal++;
            else {
                st.mismatched++;
                if (st.first === null) st.first = x[0];
                if (st.diffs.length < 6) {
                    const a = await guest.ev(`katamOnline.regions(${x[0]})`), b = await host.ev(`katamOnline.regions(${x[0]})`);
                    if (a && b) st.diffs.push({ f: x[0], regions: Object.keys(a.rg).filter((k) => a.rg[k] !== b.rg[k]) });
                }
            }
        }
        st.from = Math.max(st.from, last);
        return st;
    };
    st.reset = () => { st.from = 0; };
    return st;
}

async function snapStats(name, p) { const s = await stats(p); console.log(`    ${name}: ` + JSON.stringify(s)); return s; }

async function hostToWaiting(p, tag) {
    await toGameSelect(p);
    await toOnline(p);
    await shot(p, `${tag}-01-online.png`);
    await press(p, 'A');                    // HOST GAME
    const code = await until(async () => { const s = await stats(p); return s && s.code && s.phase === 'hosting' && (await screen(p)) === 2 * 256 + 2 ? s.code : null; }, 20000);
    await waitFrames(p, 20);
    await shot(p, `${tag}-02-host-waiting.png`);
    return code;
}

/* HOST GAME's START row: FREE ROAM is the default; Left / Right pick the
 * other mode (rooms 1 = FREE ROAM, 0 = TOGETHER) */
async function pickMode(p, rooms) {
    if ((await p.ev('Module._PortOuiRoomsMode()')) !== rooms) { await press(p, 'RIGHT'); await waitFrames(p, 20); }
    return (await p.ev('Module._PortOuiRoomsMode()')) === rooms;
}
async function startHost(p, tag, rooms = 0) {
    await press(p, 'DOWN');                 // START
    await waitFrames(p, 10);
    ok(`${tag}: START row set to ${rooms ? 'FREE ROAM' : 'TOGETHER'}`, await pickMode(p, rooms));
    await shot(p, `${tag}-04-host-start-row.png`);
    await press(p, 'A');
    return until(async () => { const s = await stats(p); return s && s.session; }, 20000);
}

/* both sides playing; collect while `secs` of play run */
async function playFor(host, guest, secs, cmp, tag, shotsAt = []) {
    const t0 = Date.now();
    const f0 = { h: await frame(host), g: await frame(guest) };
    let shotIdx = 0;
    while (Date.now() - t0 < secs * 1000) {
        await sleep(5000);
        await cmp.run();
        const el = (Date.now() - t0) / 1000;
        if (shotIdx < shotsAt.length && el >= shotsAt[shotIdx]) {
            await shot(host, `${tag}-play-host-${shotIdx}.png`);
            await shot(guest, `${tag}-play-guest-${shotIdx}.png`);
            shotIdx++;
        }
        const hs = await stats(host), gs = await stats(guest);
        say(`${tag}: host f${hs.frame} guest f${gs.frame} lead ${gs.lead} rtt ${gs.rttMs} path ${gs.path}/${hs.path} rewinds ${gs.rewinds} repairs ${gs.repairs} mism ${gs.mismatches} cmp ${cmp.equal}/${cmp.compared}`);
    }
    return { dh: (await frame(host)) - f0.h, dg: (await frame(guest)) - f0.g };
}

function hostArrivals(fromIdx) {
    const out = { 1: 0, 2: 0 }, L = logs.host || [];
    for (let i = fromIdx; i < L.length; i++) { const m = /localmp: f\d+ player (\d) arrives in room/.exec(L[i]); if (m) out[m[1]]++; }
    return out;
}
async function audioState(p) {
    return p.ev(`(function(){var a=Module.portAudio;return a&&a.ctx?a.ctx.state+' '+a.ctx.sampleRate+'Hz':'none';})()`);
}

const knobs = { netlag: LAG, netjitter: JITTER, netloss: LOSS };

// --- errors: a code nobody has --------------------------------------------------
if (CASES.includes('errors')) {
    say('# errors: JOIN with a code nobody has');
    const g = await openPage('guest');
    await toGameSelect(g);
    await toOnline(g);
    await press(g, 'DOWN');
    await press(g, 'A');                   // JOIN GAME
    await waitFrames(g, 20);
    await shot(g, 'err-01-join-entry.png');
    await typeCode(g, 'ZZZZ');
    const st = await until(async () => { const s = await screen(g); return s === 3 * 256 + 9 ? s : null; }, 15000);
    await waitFrames(g, 10);
    await shot(g, 'err-02-no-such-game.png');
    ok('JOIN with an unknown code says NO SUCH GAME', !!st, 'screen ' + (await screen(g)));
    await press(g, 'UP');                  // the CODE row
    await press(g, 'RIGHT');               // editing again clears it
    await waitFrames(g, 10);
    ok('editing the code clears the message', (await screen(g)) === 3 * 256 + 0);
    await g.close();
}

// --- menu: host and guest through the game's menus, 2+ minutes of play -----------
if (CASES.includes('menu')) {
    say('# menu: host through HOST GAME, guest through JOIN GAME; play with ' + LAG + ' ms one way, jitter ' + JITTER + ', loss ' + LOSS + '%');
    const host = await openPage('host', knobs);
    const code = await hostToWaiting(host, 'm');
    ok('host made a room and shows its code', !!code, code);
    const guest = await openPage('guest', knobs);
    await toGameSelect(guest);
    await toOnline(guest);
    await press(guest, 'DOWN');
    await press(guest, 'A');               // JOIN GAME
    await waitFrames(guest, 20);
    await shot(guest, 'm-03-join-entry.png');
    await typeCode(guest, code.slice(0, 3));
    await shot(guest, 'm-03b-join-typing.png');
    await typeCode(guest, code.slice(3));
    await sleep(300);
    await shot(guest, 'm-05-join-connecting.png');
    const waiting = await until(async () => (await screen(guest)) === 3 * 256 + 7, 20000);
    await waitFrames(guest, 10);
    await shot(guest, 'm-06-join-waiting.png');
    ok('guest connected and waits for the host', !!waiting);
    const ready = await until(async () => (await screen(host)) === 2 * 256 + 4, 10000);
    await waitFrames(host, 10);
    await shot(host, 'm-03-host-p2-ready.png');
    ok('host sees player 2 ready', !!ready);
    const s0 = await stats(guest);
    ok('data channels open (WebRTC)', s0.dcOpen && s0.path === 'p2p', 'path ' + s0.path);
    ok('session started', !!(await startHost(host, 'm')));
    const joined = await until(async () => { const s = await stats(guest); return s && s.session && s.joined && s.frame > 0; }, 30000);
    ok('guest took the host\'s image and plays', !!joined);
    const hs0 = await stats(host);
    const base = hs0.startFrame;
    // let the new game's start run (its intro), then the script
    await sleep(2000);
    await shot(host, 'm-07-host-ingame-start.png');
    await shot(guest, 'm-07-guest-ingame-start.png');
    const frames = Math.ceil((PLAY_S + 60) * 60);
    const doorsAt = 900;
    const play = buildPlay(frames, 1, doorsAt);
    await setScript(host, play.k1, base, play.warps);
    await setScript(guest, play.k2, base, []);
    const cmp = makeComparer(host, guest);
    const logIdx = (logs.host || []).length;
    if (has('diag')) {
        // a full image of one frame on both sides, diffed by snapshot part
        await sleep(8000);
        for (let rep = 0; rep < 3; rep++) {
            const D = ((await frame(host)) + 400) & ~15;
            await host.ev(`Module._PortLnDumpAt(${D})`); await guest.ev(`Module._PortLnDumpAt(${D})`);
            await until(async () => (await stats(guest)).verifiedUpTo > D + 2 || (await stats(guest)).sealedUpTo > D + 60, 30000);
            const getDump = `(function(){var r=Module._PortLnDumpReady();if(!r)return null;var p=Module._PortLnDumpPtr(),n=Module._PortLnImageBytes();
                var b=Module.HEAPU8.subarray(p,p+n),s='';for(var i=0;i<n;i+=0x8000)s+=String.fromCharCode.apply(null,b.subarray(i,i+0x8000));
                var parts=[],q=Module._malloc(16);for(var k=0;Module._PortLnSnapPart(k,q);k++)parts.push([Module.HEAPU32[q>>2],Module.HEAPU32[(q>>2)+1],Module.HEAPU32[(q>>2)+2]]);Module._free(q);
                return {f:r-1,b64:btoa(s),parts:parts};})()`;
            const a1 = await host.ev(getDump), b1 = await guest.ev(getDump);
            if (!a1 || !b1) { console.log('    diag: no dump', !!a1, !!b1); continue; }
            const A = Buffer.from(a1.b64, 'base64'), B = Buffer.from(b1.b64, 'base64');
            console.log(`    diag: frame ${a1.f}/${b1.f}, ${A.length} bytes`);
            const hdr = 64;
            for (const [base, off, size] of a1.parts) {
                const d = [];
                for (let i = 0; i < size; i++) if (A[hdr + off + i] !== B[hdr + off + i]) d.push(i);
                if (d.length) console.log(`      part base ${base.toString(16)} size ${size}: ${d.length} bytes differ, first ` +
                    d.slice(0, 24).map((i) => (base + i).toString(16) + '=' + A[hdr + off + i].toString(16) + '/' + B[hdr + off + i].toString(16)).join(' '));
            }
            const tail = a1.parts.reduce((m, x) => Math.max(m, x[1] + x[2]), 0) + hdr;
            let dt = 0; for (let i = tail; i < A.length; i++) if (A[i] !== B[i]) dt++;
            console.log(`      after the snapshot (sram, host state): ${dt} bytes differ`);
        }
    }
    const audio = { host: await audioState(host), guest: await audioState(guest) };
    const pr = await playFor(host, guest, PLAY_S, cmp, 'm', [20, 45, 90]);
    await cmp.run();
    const hs = await snapStats('host', host), gs = await snapStats('guest', guest);
    results.menu = { code, host: hs, guest: gs, compare: { ...cmp, run: undefined, reset: undefined }, progress: pr, audio, arrivals: hostArrivals(logIdx) };
    ok('host progressed', pr.dh > PLAY_S * 50, pr.dh + ' frames');
    ok('guest progressed', pr.dg > PLAY_S * 50, pr.dg + ' frames');
    ok('data channels still the path', gs.path === 'p2p' && hs.path === 'p2p', gs.path + '/' + hs.path);
    ok('frames compared across the two pages, all identical', cmp.compared > PLAY_S * 2 && cmp.mismatched === 0,
       `${cmp.equal}/${cmp.compared} equal; guest-detected mismatches ${gs.mismatches}, repairs ${gs.repairs}`);
    ok('no repair pending at the end', !gs.repairWanted);
    ok('doors: a room change led by each player', results.menu.arrivals[1] > 0 && results.menu.arrivals[2] > 0, JSON.stringify(results.menu.arrivals));
    console.log(`    guest own-input overridden ${gs.ownOverriddenPct}%, rewinds ${gs.rewinds} (${gs.rewindsPerMin}/min), lead ${gs.lead}, rtt ${gs.rttMs} ms, holds ${gs.holds} skips ${gs.skips} bursts ${gs.bursts}; audio ${JSON.stringify(audio)}`);
    pages.menuHost = host; pages.menuGuest = guest;
    // the page's online readout (the developer menu's Online section, the
    // diagnostics): the numbers; there is no gear for players any more
    ok('no gear button on the page', !(await guest.ev('!!document.getElementById("uigear")')));
    const txt = await guest.ev("Module.portUiQuery ? Module.portUiQuery('online') : ''");
    ok('menu Online section has the numbers', /Path: direct/.test(txt) && /Rewinds/.test(txt), txt.replace(/\n/g, ' | '));

    // --- drop: the guest goes away; the host plays on; a new guest by invite ---
    if (CASES.includes('drop')) {
        say('# drop: the guest page closes; the host keeps playing; a guest rejoins by invite link');
        await guest.close();
        delete pages.guest;
        const gone = await until(async () => { const s = await stats(host); return s && !s.peer; }, 15000);
        ok('host notices player 2 left', !!gone);
        const hf = await frame(host);
        await sleep(4000);
        await shot(host, 'd-01-host-p2-left.png');
        ok('host keeps playing alone', (await frame(host)) - hf > 200, ((await frame(host)) - hf) + ' frames in 4 s');
        const inv = await host.ev('katamOnline.invite()');
        const u = new URL(inv);
        const guest2 = await openPage('guest', Object.assign({ join: u.searchParams.get('join') }, knobs));
        await sleep(1500);
        await shot(guest2, 'd-02-invite-banner.png');
        const back = await until(async () => { const s = await stats(guest2); return s && s.session && s.joined; }, 60000);
    await glCheck(guest2);
        ok('a guest rejoins with the same code (invite link)', !!back);
        await sleep(1500);
        await shot(guest2, 'd-03-rejoined-guest.png');
        await shot(host, 'd-03-rejoined-host.png');
        // the "getting the game" banner must not outlive the join, and the
        // guest must not start far further ahead than it ends up (it used to
        // take about 15 s to come down, one frame a second)
        await sleep(3500);
        const early = await stats(guest2);
        ok('invite banner is gone once the guest is in the game', !early.banner, String(early.banner));
        const g2 = await stats(guest2);
        const rest = buildPlay(4000, 2, 100000);
        await setScript(guest2, rest.k2, g2.frame, []);
        await setScript(host, rest.k1, await frame(host), []);
        const cmp2 = makeComparer(host, guest2);
        await playFor(host, guest2, 40, cmp2, 'd');
        await cmp2.run();
        const gs2 = await snapStats('guest2', guest2);
        results.drop = { guest: gs2, compare: { ...cmp2, run: undefined, reset: undefined } };
        ok('rejoined guest identical to the host', cmp2.compared > 40 && cmp2.mismatched === 0, `${cmp2.equal}/${cmp2.compared}; repairs ${gs2.repairs}`);
        ok('guest lead settled within 5 s of joining', Math.abs(early.lead - gs2.lead) <= 4, `lead ${early.lead} five seconds in, ${gs2.lead} at the end`);
        // the host goes away: the guest is told
        await host.close();
        delete pages.host;
        const told = await until(async () => { const s = await stats(guest2); return s && s.phase === 'hostleft' && s.banner; }, 20000);
        await shot(guest2, 'd-04-guest-host-left.png');
        ok('guest is told the host left', !!told, told ? (await stats(guest2)).banner : '');
        await guest2.close();
    } else {
        await host.close(); await guest.close();
    }
}

// --- relay: the same over the room's signal path ------------------------------
if (CASES.includes('relay')) {
    say('# relay: ?netrelay=1 on both');
    const k = Object.assign({ netrelay: '1' }, knobs);
    const host = await openPage('host', k);
    const code = await hostToWaiting(host, 'r');
    ok('relay: host room', !!code, code);
    const guest = await openPage('guest', Object.assign({ join: code }, k));
    const ready = await until(async () => (await screen(host)) === 2 * 256 + 4, 30000);
    await glCheck(guest);
    ok('relay: player 2 ready', !!ready);
    ok('relay: session started', !!(await startHost(host, 'r')));
    const joined = await until(async () => { const s = await stats(guest); return s && s.session && s.joined; }, 30000);
    ok('relay: guest joined', !!joined);
    const base = (await stats(host)).startFrame;
    const play = buildPlay(60 * 80, 3, 100000);
    await setScript(host, play.k1, base, play.warps.slice(0, 1));
    await setScript(guest, play.k2, base, []);
    const cmp = makeComparer(host, guest);
    await playFor(host, guest, 45, cmp, 'r', [20]);
    await cmp.run();
    const hs = await snapStats('host', host), gs = await snapStats('guest', guest);
    results.relay = { host: hs, guest: gs, compare: { ...cmp, run: undefined, reset: undefined } };
    ok('relay: path is the relay on both', gs.path === 'relay' && hs.path === 'relay' && !gs.dcOpen);
    ok('relay: identical', cmp.compared > 40 && cmp.mismatched === 0, `${cmp.equal}/${cmp.compared}; repairs ${gs.repairs}`);
    await host.close(); await guest.close();
}

// --- three: host + two guests, drop and rejoin, local joins on host and guest ----
async function seatScript(p, seat, seed) {
    // a human-like second player on this page's seat `seat` (its keys through PortLmpSetHostKeys)
    await p.ev(`(function(){
        var seed = ${seed}, a = seed >>> 0;
        function rnd() { a = (a + 0x6D2B79F5) >>> 0; var t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }
        var until = 0, dir = 0, jumpUntil = 0, last = -1, held = 0;
        var prev = window.__seats;
        window.__seatOn = window.__seatOn || {};
        window.__seatOn[${seat}] = true;
        window.__seats = function (f) {
            var out = prev ? prev(f) : [0, 0, 0, 0];
            if (!window.__seatOn[${seat}]) { out[${seat}] = 0; return out; }
            if (f !== last) {
                last = f;
                if (f >= until) { var r = rnd(); dir = r < 0.5 ? 16 : r < 0.75 ? 32 : 0; until = f + 6 + Math.floor(rnd() * 40); }
                if (f >= jumpUntil && rnd() < 0.03) jumpUntil = f + 4 + Math.floor(rnd() * 24);
                held = dir | (f < jumpUntil ? 1 : 0);
            }
            out[${seat}] = held;
            return out;
        };
    })()`);
}
async function slotsOf(p) { return p.ev('katamOnline.slots()'); }
const screenLive = async (p) => (await frame(p)) - (await p.ev('Module._PortPocScreenLastDraw()')) < 5;
/* the player cards (platform/player_card.c): set card c from a settings value
 * "MODE,SPRAY,HUE,SAT,VAL,NAME"; read every slot's name and colour as this
 * page has them */
async function setCard(p, c, v) {
    await until(() => p.ev('!!(window.Module && Module.portRuntimeUp && Module._PortCardSetJs)'), 60000);
    await p.ev(`(function(){var v=${JSON.stringify(v)},b=Module._malloc(64);for(var i=0;i<v.length;i++)Module.HEAPU8[b+i]=v.charCodeAt(i);Module.HEAPU8[b+v.length]=0;Module._PortCardSetJs(${c},b);Module._free(b);})()`);
}
async function cardsOf(p) {
    return p.ev(`({ names: [0,1,2,3].map(function(s){return Module.ccall('PortPlayerName','string',['number'],[s])}),
                    paint: [0,1,2,3].map(function(s){return (Module._PortCardSlotPaintJs(s)>>>0).toString(16)}) })`);
}
const cardsStr = (c) => c.names.map((n, i) => `${n || '-'}:${c.paint[i]}`).join(' ');
/* every page has the same names and colours, and they are `want` (by slot:
 * [name, paint] or null = a CPU: no name, the game's own colour) */
async function cardsCheck(tag, pagesList, want) {
    const all = [];
    for (const p of pagesList) all.push(await cardsOf(p));
    const same = all.every((c) => cardsStr(c) === cardsStr(all[0]));
    const exp = want.map((w) => (w ? `${w[0]}:${w[1]}` : '-:0')).join(' ');
    const match = cardsStr(all[0]).split(' ').every((x, i) => {
        const w = exp.split(' ')[i];
        return w.endsWith(':custom') ? x.startsWith(w.split(':')[0] + ':2') : x === w;
    });
    ok(`${tag}: every page has the same names and colours`, same, all.map(cardsStr).join(' / '));
    ok(`${tag}: names and colours as the cards say`, match, `${cardsStr(all[0])} vs ${exp}`);
    return all[0];
}
const slotsStr = (sl) => sl.map((x, i) => `P${i + 1}:` + (x ? `${x.dev}.${x.seat}` : 'cpu')).join(' ');
async function playMany(host, guests, secs, cmps, tag, shotsAt = []) {
    const t0 = Date.now();
    let shotIdx = 0;
    while (Date.now() - t0 < secs * 1000) {
        await sleep(5000);
        for (const c of cmps) await c.run();
        const el = (Date.now() - t0) / 1000;
        if (shotIdx < shotsAt.length && el >= shotsAt[shotIdx]) {
            await shot(host, `${tag}-host-${shotIdx}.png`);
            for (const [i, g] of guests.entries()) await shot(g, `${tag}-guest${i + 1}-${shotIdx}.png`);
            shotIdx++;
        }
        const hs = await stats(host);
        const gl = [];
        for (const g of guests) { const s = await stats(g); gl.push(`f${s.frame} lead ${s.lead} rew ${s.rewinds} rep ${s.repairs} mism ${s.mismatches}`); }
        say(`${tag}: host f${hs.frame} ${slotsStr(hs.slots)} | ` + gl.join(' | ') + ' | cmp ' + cmps.map((c) => `${c.equal}/${c.compared}`).join(' '));
    }
}
if (CASES.includes('three')) {
    say('# three: host + two guests (JOIN GAME and invite link), drop and rejoin, local joins');
    const R = results.three = { steps: [] };
    const host = await openPage('host', knobs);
    // the player cards: each device's own (seat 0), the host's and guest A's
    // second players (seat 1)
    await setCard(host, 0, '1,2,0,0,0,HOSTY'); await setCard(host, 1, '1,9,0,0,0,COUCH');
    const code = await hostToWaiting(host, 't');
    ok('three: host room', !!code, code);
    // guest A through the menus
    const ga = await openPage('guestA', knobs);
    await setCard(ga, 0, '1,6,0,0,0,ANNA'); await setCard(ga, 1, '1,10,0,0,0,PADDY');
    await toGameSelect(ga);
    await toOnline(ga);
    await press(ga, 'DOWN');
    await press(ga, 'A');
    await waitFrames(ga, 20);
    await typeCode(ga, code);
    const aw = await until(async () => (await screen(ga)) === 3 * 256 + 7, 25000);
    ok('three: guest A (JOIN GAME) connected and waits', !!aw);
    // guest B by invite link
    const gb = await openPage('guestB', Object.assign({ join: code }, knobs));
    await setCard(gb, 0, '2,0,120,90,80,BOB');
    const bw = await until(async () => { const s = await stats(gb); return s && s.phase === 'waiting'; }, 40000);
    await glCheck(gb);
    ok('three: guest B (invite link) connected and waits', !!bw);
    await sleep(1500);
    await press(host, 'DOWN');              // the START row: PLAYERS 3/4
    await waitFrames(host, 20);
    ok('three: FREE ROAM is the default mode', (await host.ev('Module._PortOuiRoomsMode()')) === 1);
    ok('three: START row set to TOGETHER', await pickMode(host, 0));
    await press(host, 'UP'); await waitFrames(host, 10);    // (not selected: PLAYERS n/4)
    await press(host, 'DOWN'); await waitFrames(host, 10);
    await shot(host, 't-03-host-players-3of4.png');
    const hsPre = await stats(host);
    ok('three: host counts two guests ready', hsPre.peers && hsPre.peers.filter((p) => p.ready).length === 2, JSON.stringify(hsPre.peers.map((p) => [p.dev, p.ready, p.path])));
    await press(host, 'A');
    const started = await until(async () => { const s = await stats(host); return s && s.session; }, 20000);
    ok('three: session started', !!started);
    const inA = await until(async () => { const s = await stats(ga); return s && s.session && s.joined; }, 30000);
    const inB = await until(async () => { const s = await stats(gb); return s && s.session && s.joined; }, 30000);
    ok('three: both guests took the host\'s image', !!inA && !!inB);
    await sleep(2500);
    const sl0 = await slotsOf(host);
    ok('three: three Kirbys are people (host, guest A, guest B)', sl0[0] && sl0[1] && sl0[2] && !sl0[3] && sl0[1].dev !== sl0[2].dev, slotsStr(sl0));
    await shot(host, 't-04-host-ingame.png'); await shot(ga, 't-04-guestA-ingame.png'); await shot(gb, 't-04-guestB-ingame.png');
    // a guest's colour reaches every device; every name too
    const devOf = (sl, d) => sl.findIndex((x) => x && x.dev === d && x.seat === 0);
    const cw = [null, null, null, null];
    cw[0] = ['HOSTY', '1000002'];
    cw[devOf(sl0, (await stats(ga)).self)] = ['ANNA', '1000006'];
    cw[devOf(sl0, (await stats(gb)).self)] = ['BOB', 'custom'];
    await cardsCheck('three: cards', [host, ga, gb], cw);
    const base = (await stats(host)).startFrame;
    const frames = 60 * 200;
    const play = buildPlay(frames, 4, 900);
    await setScript(host, play.k1, base, play.warps);
    await setScript(ga, play.k2, base, []);
    const pb = buildPlay(frames, 5, 100000);
    await setScript(gb, pb.k2, base, []);
    const cA = makeComparer(host, ga), cB = makeComparer(host, gb);
    await playMany(host, [ga, gb], 60, [cA, cB], 't-play', [15, 40]);
    ok('three: the party HUD is up with cards on every page', (await host.ev('Module._PortPartyHudCardsJs()')) >= 1
       && (await ga.ev('Module._PortPartyHudCardsJs()')) >= 1 && (await gb.ev('Module._PortPartyHudCardsJs()')) >= 1);
    await cA.run(); await cB.run();
    ok('three: guest A identical to the host', cA.compared > 50 && cA.mismatched === 0, `${cA.equal}/${cA.compared}`);
    ok('three: guest B identical to the host', cB.compared > 50 && cB.mismatched === 0, `${cB.equal}/${cB.compared}`);
    R.steps.push({ step: 'play3', host: await stats(host), a: await stats(ga), b: await stats(gb), cA: cA.equal + '/' + cA.compared, cB: cB.equal + '/' + cB.compared });
    // guest B drops
    const bSlot = (await slotsOf(host)).findIndex((x) => x && x.dev === (R.steps[0].b.self));
    await gb.close(); delete pages.guestB;
    const cpu = await until(async () => { const sl = await slotsOf(host); return bSlot > 0 && !sl[bSlot] ? sl : null; }, 20000);
    ok('three: guest B\'s Kirby goes to the CPU when its page goes', !!cpu, cpu ? slotsStr(cpu) : 'slot ' + bSlot);
    const bGone = await until(async () => { const c = await cardsOf(host); return !c.names[bSlot] && c.paint[bSlot] === '0' ? c : null; }, 10000);
    ok('three: its name and colour go with it (the game\'s own colour again)', !!bGone, bGone ? cardsStr(bGone) : cardsStr(await cardsOf(host)));
    await sleep(800);
    await shot(host, 't-05-host-guestB-left.png'); await shot(ga, 't-05-guestA-guestB-left.png');
    const noteA = (await stats(ga)).banner;
    ok('three: guest A is told', /left/i.test(noteA || ''), String(noteA));
    // a local join on the host: a second player on the host's keyboard
    await seatScript(host, 1, 77);
    const hj = await host.ev('katamOnline.requestSlot(1, true)');
    const hjs = await until(async () => { const sl = await slotsOf(host); return sl.some((x) => x && x.dev === 0 && x.seat === 1) ? sl : null; }, 10000);
    ok('three: local join on the host gets a Kirby', hj && !!hjs, hjs ? slotsStr(hjs) : '');
    await sleep(700);
    await shot(host, 't-06-host-local-join.png');
    // guest B rejoins by invite
    const gb2 = await openPage('guestB', Object.assign({ join: code }, knobs));
    await setCard(gb2, 0, '2,0,120,90,80,BOB');
    const inB2 = await until(async () => { const s = await stats(gb2); return s && s.session && s.joined; }, 60000);
    await glCheck(gb2);
    ok('three: guest B rejoins with the same code (invite) and plays', !!inB2);
    await sleep(2500);
    const sl4 = await slotsOf(host);
    ok('three: four Kirbys are people now', sl4.every(Boolean), slotsStr(sl4));
    await shot(gb2, 't-07-guestB-rejoined.png'); await shot(host, 't-07-host-four.png');
    await setScript(gb2, buildPlay(frames, 6, 100000).k2, (await frame(gb2)), []);
    const cB2 = makeComparer(host, gb2);
    // a local join on guest A: refused, the game is full
    await seatScript(ga, 1, 99);
    await ga.ev('katamOnline.requestSlot(1, true)');
    const refused = await until(async () => { const s = await stats(ga); return s.seatFullNotes > 0 ? s : null; }, 15000);
    await sleep(300);
    await shot(ga, 't-08-guestA-full.png');
    ok('three: a local join on a guest is refused when four play, with a notice', !!refused, refused ? refused.banner : '');
    // the host's second player leaves; guest A asks again and gets that Kirby
    await host.ev('window.__seatOn[1] = false; katamOnline.requestSlot(1, false)');
    const freed = await until(async () => { const sl = await slotsOf(host); return !sl.some((x) => x && x.dev === 0 && x.seat === 1) ? sl : null; }, 10000);
    ok('three: the host\'s second player leaves', !!freed, freed ? slotsStr(freed) : '');
    const aDev = (await stats(ga)).self;
    // ... by the device path: a pad on guest A, Start (as offline: the
    // device module, its seat 1, the host's choice of Kirby)
    await ga.ev('window.__seatOn[1] = false; window.__seats = null');
    await ga.ev(`__padPlug(0, ${JSON.stringify('Xbox 360 Controller (XInput STANDARD GAMEPAD)')})`);
    await press(ga, 'RIGHT'); await waitFrames(ga, 10);   // player 1 played with the keyboard just now
    await sleep(3200);                                     // ... and the pad was put down a while
    await shot(ga, 't-08b-guestA-pad-hint.png');
    await ga.ev('__padSet(0, [9])'); await sleep(150); await ga.ev('__padSet(0, [])');
    const gj = await until(async () => { const sl = await slotsOf(host); return sl.some((x) => x && x.dev === aDev && x.seat === 1) ? sl : null; }, 15000);
    ok('three: a pad on guest A joins by Start and gets the free Kirby', !!gj, gj ? slotsStr(gj) : '');
    // (said when the host's answer lands: once, by the device notices)
    await until(() => ga.ev('document.querySelectorAll("#pnotices .pnotice").length > 0'), 5000);
    const gaNotes = await ga.ev('Array.from(document.querySelectorAll("#pnotices .pnotice")).map(function(e){return e.textContent}).join(" | ")');
    ok('three: guest A is told (notice)', /joining|joined/i.test(gaNotes) || /Your player/i.test((await stats(ga)).banner || ''), gaNotes + ' / ' + (await stats(ga)).banner);
    {
        await sleep(400);
        const notes = await ga.ev('Array.from(document.querySelectorAll("#pnotices .pnotice")).map(function(e){return e.textContent})');
        const ban = (await stats(ga)).banner || '';
        const n = notes.filter((t) => /joining|joined/i.test(t)).length + (/joined|Your player/i.test(ban) ? 1 : 0);
        ok('three: one notice for that join, not two', n === 1, JSON.stringify(notes) + ' / banner: ' + ban);
        await shot(ga, 't-08c-guestA-one-notice.png');
    }
    await ga.ev('__padSet(0, [15])'); await waitFrames(ga, 6);
    ok('three: the pad drives guest A\'s seat 1 (its host keys)', (await ga.ev('Module._PortLmpHostKeys(1)')) === 16);
    await ga.ev('__padSet(0, [])');
    await sleep(800);
    await shot(ga, 't-09-guestA-two-players.png'); await shot(host, 't-09-host.png');
    const padSlot = gj ? gj.findIndex((x) => x && x.dev === aDev && x.seat === 1) : -1;
    const bDev2 = (await stats(gb2)).self, bSlot2 = devOf(await slotsOf(host), bDev2);
    const cw2 = [['HOSTY', '1000002'], null, null, null];
    cw2[devOf(await slotsOf(host), aDev)] = ['ANNA', '1000006'];
    if (padSlot > 0) cw2[padSlot] = ['PADDY', '100000a'];
    if (bSlot2 > 0) cw2[bSlot2] = ['BOB', 'custom'];
    await sleep(1500);
    await cardsCheck('three: four players\' cards', [host, ga, gb2], cw2);
    // the PLAYERS page in a session (pause > SETTINGS > PLAYERS): who plays
    // each Kirby, read only, drawn the same on every device (the comparers
    // below cover these frames)
    await host.ev('window.__scriptSaved = window.__script; window.__script = null; Module._PortSetKeys(0)');
    await waitFrames(host, 10);
    await press(host, 'START'); await waitFrames(host, 60);
    let pageUp = false;
    for (let i = 0; i < 5 && !pageUp; i++) {
        if (await screenLive(host)) pageUp = true;
        else { await press(host, 'SELECT'); await waitFrames(host, 70); }
    }
    ok('session: the pause SETTINGS page is up', pageUp);
    await press(host, 'DOWN'); await waitFrames(host, 10);        // SOUND
    await press(host, 'DOWN'); await waitFrames(host, 10);        // PLAYERS
    await press(host, 'A'); await waitFrames(host, 20);           // the PLAYERS section
    await press(host, 'DOWN'); await waitFrames(host, 10);        // PARTY HUD
    await press(host, 'DOWN'); await waitFrames(host, 10);        // NAME TAGS
    await press(host, 'RIGHT'); await waitFrames(host, 30);       // the edit: the host's own device
    await sleep(800);
    await shot(host, 't-09b-host-pause-name-tags.png'); await shot(ga, 't-09b-guestA-pause-name-tags.png');
    ok('session: NAME TAGS on the pause page goes to the host\'s own setting', (await host.ev('Module._PortCardTagsOn()')) === 1
       && (await ga.ev('Module._PortCardTagsOn()')) === 0, `host ${await host.ev('Module._PortCardTagsOn()')}, guest A ${await ga.ev('Module._PortCardTagsOn()')}`);
    await press(host, 'LEFT'); await waitFrames(host, 30);
    await sleep(800);
    ok('session: ... and off again', (await host.ev('Module._PortCardTagsOn()')) === 0);
    await press(host, 'UP'); await waitFrames(host, 10);          // PARTY HUD
    await shot(host, 't-09c-host-pause-party-hud.png');
    await press(host, 'UP'); await waitFrames(host, 10);          // PLAYERS
    await press(host, 'A'); await waitFrames(host, 40);
    await sleep(1500);
    const models = [await host.ev('Module._PortPocScreenModel()'), await ga.ev('Module._PortPocScreenModel()'), await gb2.ev('Module._PortPocScreenModel()')];
    ok('session: PLAYERS opens the read-only page on every device', models.every((m) => m === 8), JSON.stringify(models));
    await shot(host, 't-09d-host-session-players.png'); await shot(ga, 't-09d-guestA-session-players.png'); await shot(gb2, 't-09d-guestB-session-players.png');
    await press(host, 'B'); await waitFrames(host, 30);
    ok('session: B goes back to the PLAYERS section', (await host.ev('Module._PortPocScreenModel()')) === 12);
    await press(host, 'B'); await waitFrames(host, 30);
    ok('session: B again: SETTINGS', (await host.ev('Module._PortPocScreenModel()')) === 9);
    await press(host, 'START'); await waitFrames(host, 60);
    await host.ev('window.__script = window.__scriptSaved');
    await playMany(host, [ga, gb2], 40, [cA, cB2], 't-play4', [20]);
    // the pad's player leaves by holding Select (two seconds): the CPU, the game's colour
    await ga.ev('__padSet(0, [8])'); await sleep(3000); await ga.ev('__padSet(0, [])');
    const padGone = await until(async () => { const sl = await slotsOf(host); return padSlot > 0 && !sl[padSlot] ? sl : null; }, 15000);
    ok('three: the pad\'s player leaves by holding Select', !!padGone, padGone ? slotsStr(padGone) : slotsStr(await slotsOf(host)));
    const padCol = await until(async () => { const c = await cardsOf(ga); return padSlot > 0 && c.paint[padSlot] === '0' && !c.names[padSlot] ? c : null; }, 10000);
    ok('three: ... and its colour and name go (on guest A)', !!padCol, cardsStr(await cardsOf(ga)));
    await cA.run(); await cB2.run();
    ok('three: after the churn, guest A identical', cA.mismatched === 0, `${cA.equal}/${cA.compared}`);
    ok('three: after the churn, rejoined guest B identical', cB2.compared > 30 && cB2.mismatched === 0, `${cB2.equal}/${cB2.compared}`);
    const hsEnd = await snapStats('host', host), aEnd = await snapStats('guestA', ga), bEnd = await snapStats('guestB', gb2);
    R.end = { host: hsEnd, a: aEnd, b: bEnd, cA: cA.equal + '/' + cA.compared, cB2: cB2.equal + '/' + cB2.compared };
    ok('three: host never rewound', !hsEnd.hostRewinds && !hsEnd.rewinds, `${hsEnd.hostRewinds}/${hsEnd.rewinds}`);
    const txt = await host.ev("Module.portUiQuery ? Module.portUiQuery('online') : ''");
    ok('three: the menu\'s Online section lists each guest', /Dev \d/.test(txt) && (txt.match(/Dev \d/g) || []).length >= 2, txt.replace(/\n/g, ' | '));
    R.menuText = txt;

    // --- leaving without reloading (web/online.js leave, PortLnGoHome) -------
    say('three: leaving, joining again and hosting again, no reload');
    const bDevL = (await stats(gb2)).self, bSlotL = devOf(await slotsOf(host), bDevL);
    await gb2.ev('window.__script = null; Module._PortSetKeys(0); katamOnline.leave()');
    const bOut = await until(async () => { const s = await stats(gb2); return s && !s.session && !s.role ? s : null; }, 10000);
    ok('leave: guest B leaves from its menu (no session, no role)', !!bOut);
    const homed = await until(() => gb2.ev('Module._PortLnHomeLoads()'), 5000);
    ok('leave: guest B\'s game went back to power-on', homed === 1, String(homed));
    // ... and straight to the title, not through the boot logos again
    const bf0 = await frame(gb2);
    await until(async () => (await frame(gb2)) >= bf0 + 45, 20000, 20);
    await shot(gb2, 't-10a-guestB-left-title.png');
    ok('leave: guest B is at the title at once (no boot logos)', (await gb2.ev('Module._PortLnHomeExpressed()')) === 1,
       String(await gb2.ev('Module._PortLnHomeExpressed()')));
    const bCpu = await until(async () => { const sl = await slotsOf(host); return bSlotL > 0 && !sl[bSlotL] ? sl : null; }, 15000);
    ok('leave: the host hands guest B\'s Kirby to the CPU', !!bCpu, bCpu ? slotsStr(bCpu) : slotsStr(await slotsOf(host)));
    await waitFrames(gb2, 420);
    await shot(gb2, 't-10-guestB-left-boots-again.png');
    ok('leave: guest B runs on its own (frames advance, no session)', !(await stats(gb2)).session && (await frame(gb2)) > 0);
    await gb2.ev(`katamOnline.join(${JSON.stringify(code)})`);
    const inB3 = await until(async () => { const s = await stats(gb2); return s && s.session && s.joined; }, 60000);
    ok('leave: guest B joins again without reloading', !!inB3);
    await sleep(2500);
    const cB3 = makeComparer(host, gb2);
    await playMany(host, [ga, gb2], 20, [cA, cB3], 't-rejoin', [10]);
    await cB3.run();
    ok('leave: guest B identical after joining again', cB3.compared > 20 && cB3.mismatched === 0, `${cB3.equal}/${cB3.compared}`);
    // the host ends the game: each guest is told and goes back the same way
    // (the scripted pads stop: from here on the menus are driven by hand)
    for (const p of [host, ga, gb2]) await p.ev('window.__script = null; window.__seats = null; Module._PortSetKeys(0)');
    await host.ev('katamOnline.leave()');
    const hOut = await until(async () => { const s = await stats(host); return s && !s.session && (await host.ev('Module._PortLnHomeLoads()')) === 1 ? s : null; }, 10000);
    ok('leave: the host ends the game (back to power-on)', !!hOut);
    const hf0 = await frame(host);
    ok('leave: the host goes straight to the title (no boot logos)',
       !!(await until(async () => (await host.ev('Module._PortLnHomeExpressed()')) === 1, 5000)));
    const aLeft = await until(async () => { const s = await stats(ga); return s && s.phase === 'hostleft' ? s : null; }, 15000);
    ok('leave: guest A is told the host left', !!aLeft, aLeft ? aLeft.banner : '');
    await sleep(300);
    await shot(ga, 't-11-guestA-host-left.png');
    const btn = await ga.ev('(function(){var b=document.getElementById("lnleave");return !!(b && b.style.display!=="none")})()');
    ok('leave: guest A gets a "Back to the title" button', btn);
    // the host's game is at the title (no logos, no intro): Start until
    // FILE SELECT, watched frame by frame (its picture is compared with the
    // one taken on the way in, mid-fade, so every frame is looked at)
    await until(async () => (await frame(host)) >= hf0 + 120, 60000, 30);
    await shot(host, 't2-00-host-boots-again.png');
    let atFs = false;
    for (let i = 0; i < 4 && !atFs; i++) {
        await press(host, 'START');
        atFs = !!(await until(async () => (await fsPrint(host)) === host.fsPrint, 3000, 2));
    }
    ok('again: the host\'s game is back at FILE SELECT, from the title', atFs);
    await waitFrames(host, 150);                               // (it takes input once it has faded in)
    await press(host, 'A'); await waitFrames(host, 200);       // FILE 1 -> Game Select
    await toOnline(host);
    await shot(host, 't2-01-online.png');
    ok('again: the host is on the ONLINE screen', (await host.ev('Module._PortPocScreenModel()')) === 2 && await screenLive(host),
       String(await host.ev('Module._PortPocScreenModel()')));
    await press(host, 'A');                                    // HOST GAME
    const code2 = await until(async () => { const s = await stats(host); return s && s.code && s.phase === 'hosting' && (await screen(host)) === 2 * 256 + 2 ? s.code : null; }, 20000);
    await waitFrames(host, 20);
    await shot(host, 't2-02-host-waiting.png');
    await ga.ev('document.getElementById("lnleave").click()');
    const aOut = await until(async () => { const s = await stats(ga); return s && !s.session && !s.role && (await ga.ev('Module._PortLnHomeLoads()')) === 1 ? s : null; }, 10000);
    ok('leave: the button takes guest A back to the title', !!aOut);
    {
        const af0 = await frame(ga);
        await until(async () => (await frame(ga)) >= af0 + 45, 20000, 20);
        await shot(ga, 't-11a-guestA-title-at-once.png');
        ok('leave: guest A is at the title at once (no boot logos)', (await ga.ev('Module._PortLnHomeExpressed()')) === 1);
    }
    await until(async () => { const s = await stats(gb2); return s && s.phase === 'hostleft'; }, 15000);
    await gb2.ev('katamOnline.leave()');
    await waitFrames(ga, 420);
    await shot(ga, 't-12-guestA-title-again.png');
    ok('again: the host makes a new room from the menus', !!code2 && code2 !== code, code2);
    await ga.ev(`katamOnline.join(${JSON.stringify(code2)})`);
    const aw2 = await until(async () => { const s = await stats(ga); return s && s.phase === 'waiting'; }, 40000);
    ok('again: guest A connects and waits', !!aw2);
    await sleep(1500);
    const st2 = await startHost(host, 't2');
    ok('again: the session starts', !!st2);
    const inA2 = await until(async () => { const s = await stats(ga); return s && s.session && s.joined; }, 30000);
    ok('again: guest A takes the new image and plays', !!inA2);
    await sleep(2500);
    const slA = await slotsOf(host);
    const cw3 = [['HOSTY', '1000002'], null, null, null];
    cw3[devOf(slA, (await stats(ga)).self)] = ['ANNA', '1000006'];
    await cardsCheck('again: cards', [host, ga], cw3);
    const b3 = (await stats(host)).startFrame;
    const play3 = buildPlay(60 * 60, 8, 900);
    await setScript(host, play3.k1, b3, play3.warps);
    await setScript(ga, play3.k2, b3, []);
    const cA2 = makeComparer(host, ga);
    await playMany(host, [ga], 25, [cA2], 't-again', [12]);
    await cA2.run();
    ok('again: guest A identical to the host', cA2.compared > 30 && cA2.mismatched === 0, `${cA2.equal}/${cA2.compared}`);
    await host.close(); await ga.close(); await gb2.close();
    delete pages.host; delete pages.guestA; delete pages.guestB;
}

// --- exits: the pause menus in an online game, per-device settings, REMOVE ----------
//   Three pages, TOGETHER.  The room code in session state and on the help
//   page; B / Start / Select as the pauser and as the others; per-device
//   settings (the owner's four cases: a guest's ZOOM and CAMERA edits are
//   its own, the host's page then shows the host's values and each press
//   changes the host by one step; an edit rewound on the guest is applied
//   once); the host REMOVEs guest B (its title, the CPU, refused when it
//   comes back, a new guest still welcome); guest A's LEAVE GAME; the host's
//   END GAME, and the remaining guest's pad takes it to the title.
const SIM_LNSET = 0x02040014;
const lnset = (p) => p.ev(`Array.from(Module.HEAPU8.subarray(${SIM_LNSET}, ${SIM_LNSET + 12}))`);
const pauseOf = (p) => p.ev(`(function(){var q=Module._malloc(64);Module._PortLnObserve(q);
    var r={paused:Module.HEAP32[q>>2],pauser:Module.HEAP32[(q>>2)+1],menu:Module.HEAP32[(q>>2)+2]};Module._free(q);return r})()`);
const modelOf = (p) => p.ev('Module._PortPocScreenModel()');
const word = (p) => p.ev('Module._PortPocSettingsWord() >>> 0');
const shownWord = (p) => p.ev('Module._PortPocShownWord() >>> 0');
const zoomIdx = (w) => (w >>> 4) & 7, camOf = (w) => (w >>> 7) & 1;
if (CASES.includes('exits')) {
    say('# exits: the pause menus online, per-device settings, REMOVE, LEAVE / END GAME');
    const table = [];
    const step = async (p, k, n = 20) => { await press(p, k); await waitFrames(p, n); };
    /* Start until the game is paused (a Start right after the pause closed,
     * or mid-door, is the game's to ignore) */
    const pauseNow = async (p) => {
        for (let i = 0; i < 6; i++) {
            if ((await pauseOf(p)).paused === 1) return true;
            await waitFrames(p, 60);
            await step(p, 'START', 90);
        }
        return (await pauseOf(p)).paused === 1;
    };
    const toSettings = async (p) => {
        for (let i = 0; i < 5 && (await pauseOf(p)).menu !== 8; i++) await step(p, 'SELECT', 80);
        return (await pauseOf(p)).menu === 8;
    };
    const host = await openPage('host', knobs);
    const code = await hostToWaiting(host, 'x');
    ok('exits: host room', !!code, code);
    const ga = await openPage('guestA', Object.assign({ join: code }, knobs));
    ok('exits: guest A waits', !!(await until(async () => { const st = await stats(ga); return st && st.phase === 'waiting'; }, 40000)));
    const gb = await openPage('guestB', Object.assign({ join: code }, knobs));
    ok('exits: guest B waits', !!(await until(async () => { const st = await stats(gb); return st && st.phase === 'waiting'; }, 40000)));
    await glCheck(ga); await glCheck(gb);
    await sleep(1500);
    ok('exits: session started (TOGETHER)', !!(await startHost(host, 'x', 0)));
    for (const g of [ga, gb]) ok(`exits: ${g.name} took the image`, !!(await until(async () => { const st = await stats(g); return st && st.session && st.joined; }, 30000)));
    await sleep(3000);
    const all = [host, ga, gb];
    // the room code: session state, the same everywhere
    const sets = await Promise.all(all.map(lnset));
    const codeIn = (b) => String.fromCharCode(b[6], b[7], b[8], b[9]);
    ok('exits: the room code is session state on every page', sets.every((b) => b[0] === 2 && b[5] === 1 && codeIn(b) === code), JSON.stringify(sets.map(codeIn)));
    const cmpA = makeComparer(host, ga), cmpB = makeComparer(host, gb);
    // --- guest A pauses: the help page with the code
    await pauseNow(ga);
    const pz = await Promise.all(all.map(pauseOf));
    ok('exits: guest A pauses the game for everyone', pz.every((x) => x.paused === 1), JSON.stringify(pz));
    ok('exits: the help page shows the room line on every page', (await Promise.all(all.map((p) => p.ev('Module._PortPocHelpLineShown()')))).every((v) => v === 1));
    await shot(ga, 'x-01-guestA-help-code.png'); await shot(host, 'x-01-host-help-code.png');
    await step(host, 'B', 30);
    ok('exits: the host\'s B does not close guest A\'s pause', (await pauseOf(host)).paused === 1);
    await step(host, 'SELECT', 80);
    ok('exits: the host\'s Select turns the page (any player\'s, as the game has it)', (await pauseOf(host)).menu !== 1);
    for (let i = 0; i < 5 && (await pauseOf(ga)).menu !== 8; i++) await step(ga, 'SELECT', 80);
    ok('exits: guest A on SETTINGS', (await modelOf(ga)) === 9 && (await modelOf(host)) === 9);
    table.push(['Pause, online (TOGETHER)', 'the other players', 'nothing', 'nothing', 'next page']);
    // --- SETTINGS > ONLINE on guest A: the code and the list, read only
    for (let i = 0; i < 3; i++) await step(ga, 'DOWN', 10);
    await step(ga, 'A', 30);
    ok('exits: ONLINE section (guest A)', (await modelOf(ga)) === 13);
    await waitFrames(ga, 30);
    await shot(ga, 'x-02-guestA-online-list.png'); await shot(host, 'x-02-host-online-list.png');
    await step(ga, 'DOWN', 10); await step(ga, 'DOWN', 10); await step(ga, 'DOWN', 10);   // a player row
    await step(ga, 'A', 30);
    ok('exits: a guest cannot REMOVE (stays on the list)', (await modelOf(ga)) === 13);
    await step(ga, 'B', 30);
    ok('exits: B: SETTINGS', (await modelOf(ga)) === 9);
    // --- the owner's cases: guest A's ZOOM and CAMERA are its own
    const h0 = await word(host), a0 = await word(ga);
    for (let i = 0; i < 3; i++) await step(ga, 'UP', 10);         // (B left the cursor on ONLINE)
    await step(ga, 'A', 30);                                       // DISPLAY
    await step(ga, 'DOWN', 10); await step(ga, 'DOWN', 10);        // ZOOM
    await step(ga, 'RIGHT', 30); await sleep(1500);
    let h1 = await word(host), a1 = await word(ga);
    ok('case 1: guest A\'s ZOOM Right: guest A one step, the host unchanged',
       zoomIdx(a1) === Math.min(6, zoomIdx(a0) + 1) && zoomIdx(h1) === zoomIdx(h0), `A ${zoomIdx(a0)}->${zoomIdx(a1)}, host ${zoomIdx(h0)}->${zoomIdx(h1)}`);
    ok('case 1: every page shows guest A\'s value', (await Promise.all(all.map(shownWord))).every((w) => zoomIdx(w) === zoomIdx(a1)));
    await shot(ga, 'x-03-guestA-zoom.png'); await shot(host, 'x-03-host-sees-guestA-zoom.png');
    // an edit rewound on the guest is still applied once
    await ga.ev('Module._PortLnForceRewind(40)'); await sleep(1500);
    ok('exits: a rewind over guest A\'s edit does not apply it again', zoomIdx(await word(ga)) === zoomIdx(a1), `${zoomIdx(await word(ga))}`);
    await step(ga, 'DOWN', 10); await step(ga, 'DOWN', 10);       // CAMERA
    await step(ga, 'RIGHT', 30); await sleep(1500);
    ok('case 3: guest A\'s CAMERA Right: guest A toggled, the host not', camOf(await word(ga)) !== camOf(a0) && camOf(await word(host)) === camOf(h0));
    await step(ga, 'START', 60);
    ok('exits: guest A\'s Start (in a section) closes the pause', (await pauseOf(host)).paused === 0);
    table.push(['Pause, online', 'the pauser, in a section', 'SETTINGS', 'close the pause', 'next page']);
    // --- the host pauses: its own values; each press one step
    ok('exits: the host pauses', await pauseNow(host) && await toSettings(host));
    await step(host, 'A', 30); await step(host, 'DOWN', 10); await step(host, 'DOWN', 10);   // DISPLAY > ZOOM
    await sleep(800);
    const sh = await shownWord(host), hz = zoomIdx(await word(host));
    ok('case 2: the host\'s page shows the host\'s own ZOOM', zoomIdx(sh) === hz && zoomIdx(sh) !== zoomIdx(await word(ga)), `shown ${zoomIdx(sh)}, host ${hz}, A ${zoomIdx(await word(ga))}`);
    await shot(host, 'x-04-host-own-zoom.png');
    await step(host, 'RIGHT', 30); await sleep(800);
    ok('case 2: Right: the host one step, guest A unchanged', zoomIdx(await word(host)) === Math.min(6, hz + 1) && zoomIdx(await word(ga)) === zoomIdx(a1));
    await step(host, 'DOWN', 10); await step(host, 'DOWN', 10);   // CAMERA
    for (let i = 0; i < 3; i++) {
        const c0 = camOf(await word(host));
        await step(host, 'RIGHT', 30); await sleep(800);
        ok(`case 4: CAMERA Right #${i + 1} toggles the host's own`, camOf(await word(host)) !== c0 && camOf(await shownWord(host)) === camOf(await word(host)));
    }
    await step(host, 'LEFT', 30); await sleep(500);                // (back to where it was, odd count)
    await step(host, 'UP', 10); await step(host, 'UP', 10); await step(host, 'LEFT', 30);   // ZOOM back
    await step(host, 'B', 30);
    // --- REMOVE guest B (the host's list; B is guest 2)
    for (let i = 0; i < 3; i++) await step(host, 'DOWN', 10);      // ONLINE
    await step(host, 'A', 30);
    for (let i = 0; i < 4; i++) await step(host, 'DOWN', 10);      // row 4: guest 2's Kirby
    await step(host, 'A', 30);
    ok('exits: the host\'s A on a guest\'s row asks (NO / YES)', (await modelOf(host)) === 16);
    await shot(host, 'x-05-host-remove-confirm.png'); await shot(ga, 'x-05-guestA-sees-remove-confirm.png');
    await step(host, 'B', 30);
    ok('exits: B on REMOVE: back to the list', (await modelOf(host)) === 13);
    await step(host, 'A', 30); await step(host, 'DOWN', 10);
    const bDev = (await stats(gb)).self, bSlot = (await slotsOf(host)).findIndex((x) => x && x.dev === bDev);
    await step(host, 'A', 30);                                       // YES
    const bOut = await until(async () => { const s2 = await stats(gb); return s2 && !s2.session && (await gb.ev('Module._PortLnHomeLoads()')) === 1 ? s2 : null; }, 15000);
    ok('REMOVE: guest B is out, its game back at the title', !!bOut);
    await sleep(500);
    await shot(gb, 'x-06-guestB-removed.png');
    ok('REMOVE: guest B says REMOVED BY THE HOST', /REMOVED BY THE HOST/.test((await stats(gb)).banner || ''), (await stats(gb)).banner);
    const bCpu = await until(async () => { const sl = await slotsOf(host); return bSlot > 0 && !sl[bSlot]; }, 15000);
    ok('REMOVE: guest B\'s Kirby is the CPU', !!bCpu, String(bSlot));
    await step(host, 'START', 60);
    ok('exits: the host\'s Start closes the pause after REMOVE', (await pauseOf(host)).paused === 0);
    await sleep(2000);
    await cmpA.run();
    ok('REMOVE: guest A still identical to the host', cmpA.mismatched === 0 && cmpA.compared > 5, `${cmpA.equal}/${cmpA.compared}`);
    await waitFrames(gb, 200);
    await gb.ev(`katamOnline.join(${JSON.stringify(code)})`);
    const refused = await until(async () => ((await screen(gb)) & 255) === 15, 20000);   /* REMOVED (from the title: no JOIN screen) */
    ok('REMOVE: guest B\'s rejoin with the code is refused (REMOVED)', !!refused, String(await screen(gb)));
    await waitFrames(gb, 10);
    await shot(gb, 'x-07-guestB-rejoin-refused.png');
    const gc = await openPage('guestC', Object.assign({ join: code }, knobs));
    ok('REMOVE: a new guest can still join', !!(await until(async () => { const st = await stats(gc); return st && st.session && st.joined; }, 60000)));
    await sleep(2500);
    // --- guest A: LEAVE GAME from its pause
    ok('exits: guest A pauses again', await pauseNow(ga) && await toSettings(ga));
    await step(ga, 'UP', 10); await step(ga, 'A', 30);              // MAIN MENU (the top level's last row)
    ok('exits: a guest\'s MAIN MENU asks', (await modelOf(ga)) === 16);
    await shot(ga, 'x-08-guestA-leave-confirm.png');
    await step(ga, 'DOWN', 10); await step(ga, 'A', 30);
    const aOut = await until(async () => { const s2 = await stats(ga); return s2 && !s2.session && (await ga.ev('Module._PortLnHomeLoads()')) === 1; }, 15000);
    ok('LEAVE GAME: guest A leaves, its game at the title', !!aOut);
    await sleep(1500);
    ok('LEAVE GAME: the others are still paused (guest A was the pauser)', (await pauseOf(host)).paused === 1);
    await step(host, 'START', 60);
    ok('LEAVE GAME: the host can close the pause the leaver left open', (await pauseOf(host)).paused === 0);
    table.push(['Pause, online', 'the pauser left / was removed', 'n/a', 'the host closes it', 'next page']);
    // --- the host: END GAME
    ok('exits: the host pauses again', await pauseNow(host) && await toSettings(host));
    await step(host, 'UP', 10); await step(host, 'A', 30);
    await shot(host, 'x-09-host-end-confirm.png');
    await step(host, 'DOWN', 10); await step(host, 'A', 30);
    ok('END GAME: the host goes to the title', !!(await until(async () => (await host.ev('Module._PortLnHomeLoads()')) === 1, 15000)));
    ok('END GAME: guest C is told', !!(await until(async () => { const s2 = await stats(gc); return s2 && s2.phase === 'hostleft'; }, 15000)));
    await sleep(500);
    await shot(gc, 'x-10-guestC-host-left.png');
    await step(gc, 'A', 5); await sleep(1500);
    ok('END GAME: guest C\'s A (the pad or keys) takes it to the title', (await gc.ev('Module._PortLnHomeLoads()')) === 1 && !(await stats(gc)).session);
    table.push(['Online, the host left', 'guest, game stopped', 'title', 'title', '(A too) title']);
    console.log('  exits table (online):'); for (const r of table) console.log('    ' + r.join(' | '));
    for (const p of [host, ga, gb, gc]) await p.close();
    delete pages.host; delete pages.guestA; delete pages.guestB; delete pages.guestC;
}

// --- rooms: FREE ROAM (rooms mode), two and three pages --------------------------
//   The host picks FREE ROAM on HOST GAME's START row (Right), then each
//   player is warped to a room of their own: every page must show its own
//   player in that player's room (Module._PortLnObserve: whose screen, which
//   room) with the WebGL2 renderer; every guest's canonical shadow identical
//   to the host's frames (the hashes it verifies), its own game equal to the
//   shadow by the gameplay digest; a phone call (R) by the host brings
//   everyone to the host's room; the pause menu opened from each page in turn.
async function screenOf(p) {
    return p.ev(`(function(){var q=Module._malloc(64);Module._PortLnObserve(q);var lp=Module.HEAP32[(q>>2)+13],room=Module.HEAP32[(q>>2)+12],paused=Module.HEAP32[q>>2];
        Module._PortLnKirby(lp,q);var kr=Module.HEAP32[(q>>2)+2];var rooms=[];for(var i=0;i<4;i++){Module._PortLnKirby(i,q);rooms.push(Module.HEAP32[(q>>2)+2]);}
        Module._free(q);return {lp:lp,room:room,kroom:kr,rooms:rooms,paused:paused};})()`);
}
for (const nPages of (flag('rooms-pages', '2,3')).split(',').map(Number)) {
    if (!CASES.includes('rooms')) break;
    const tag = 'fr' + nPages;
    say(`# rooms: FREE ROAM with ${nPages} pages`);
    const R = results[tag] = {};
    const host = await openPage('host', knobs);
    const code = await hostToWaiting(host, tag);
    ok(`${tag}: host room`, !!code, code);
    const guests = [];
    for (let i = 1; i < nPages; i++) {
        const g = await openPage('guest' + i, Object.assign({ join: code }, knobs));
        const w = await until(async () => { const st = await stats(g); return st && st.phase === 'waiting'; }, 40000);
        await glCheck(g);
        ok(`${tag}: guest ${i} (invite link) waits`, !!w);
        guests.push(g);
    }
    await sleep(1500);
    await press(host, 'DOWN');              // the START row
    await waitFrames(host, 10);
    await pickMode(host, 1);                // FREE ROAM (the default; TOGETHER if a test left it so)
    await shot(host, `${tag}-01-host-free-roam.png`);
    ok(`${tag}: HOST GAME offers FREE ROAM`, (await host.ev('Module._PortOuiRoomsMode()')) === 1);
    await press(host, 'A');
    ok(`${tag}: session started`, !!(await until(async () => { const st = await stats(host); return st && st.session; }, 20000)));
    for (const g of guests) ok(`${tag}: ${g.name} took the host's image`, !!(await until(async () => { const st = await stats(g); return st && st.session && st.joined; }, 30000)));
    await sleep(2500);
    const hs0 = await stats(host);
    ok(`${tag}: rooms mode is on for every page`, hs0.rooms === 1 && (await Promise.all(guests.map(stats))).every((x) => x.rooms === 1));
    // the script: each player to a room of their own, then a phone call by
    // the host (R), the others answer (R 40 frames later)
    const spots = [[117, 44, 9], [203, 60, 5], [101, 3, 7], [104, 2, 9]];
    const r0 = (await frame(host)) + 60, CALL = 900;
    const warps = [];
    for (let k = 0; k < nPages; k++) warps.push([0, k, spots[k][0], spots[k][1], spots[k][2]]);
    const keysFor = (who) => { const K = new Array(3000).fill(0);
        const at = who === 0 ? CALL : CALL + 40; for (let i = 0; i < 6; i++) K[at + i] = KEY.R; return K; };
    await setScript(host, keysFor(0), r0, warps);
    for (const g of guests) await setScript(g, keysFor(1), r0, []);
    const cmps = guests.map((g) => makeComparer(host, g));
    await until(async () => (await frame(host)) >= r0 + 420, 30000);
    for (const c of cmps) await c.run();
    // apart: each page on its own player, in that player's room
    const all = [host, ...guests];
    const apart = [];
    for (let i = 0; i < all.length; i++) {
        const sc = await screenOf(all[i]);
        apart.push(sc);
        await shot(all[i], `${tag}-02-apart-${all[i].name}.png`);
        ok(`${tag}: ${all[i].name} shows its own player (${i + 1}P) in that player's room`, sc.lp === i && sc.room === sc.kroom && sc.room === spots[i][0],
           JSON.stringify(sc));
    }
    R.apart = apart;
    ok(`${tag}: the players are in ${nPages} different rooms`, new Set(apart[0].rooms.slice(0, nPages)).size === nPages, JSON.stringify(apart[0].rooms));
    // the phone: the host calls, the others answer
    await until(async () => (await frame(host)) >= r0 + CALL + 600, 40000);
    const met = [];
    for (let i = 0; i < all.length; i++) {
        const sc = await screenOf(all[i]);
        met.push(sc);
        await shot(all[i], `${tag}-03-after-call-${all[i].name}.png`);
    }
    R.afterCall = met;
    ok(`${tag}: the host's call brought everyone to the host's room`, met[0].rooms.slice(0, nPages).every((r) => r === met[0].rooms[0]), JSON.stringify(met[0].rooms));
    ok(`${tag}: each page still shows its own player`, met.every((sc, i) => sc.lp === i && sc.room === sc.kroom), JSON.stringify(met.map((x) => [x.lp, x.room, x.kroom])));
    // the pause menu, opened from each page in turn (START, then START again)
    R.pause = [];
    for (let i = 0; i < all.length; i++) {
        const p = all[i], f0 = await frame(p), K = new Array(400).fill(0);
        for (let j = 0; j < 6; j++) { K[20 + j] = KEY.START; }
        await setScript(p, K, f0, []);
        const paused = await until(async () => (await screenOf(host)).paused === 1, 10000);
        await sleep(1500);
        const shotsNow = [];
        for (const q of all) { await shot(q, `${tag}-04-pause-by-${p.name}-on-${q.name}.png`); shotsNow.push(await screenOf(q)); }
        R.pause.push({ by: p.name, paused: !!paused, screens: shotsNow });
        ok(`${tag}: ${p.name} pauses the game for everyone`, !!paused && shotsNow.every((x) => x.paused === 1));
        const f1 = await frame(p), K2 = new Array(400).fill(0);
        for (let j = 0; j < 6; j++) K2[10 + j] = KEY.START;
        await setScript(p, K2, f1, []);
        let resumed = await until(async () => (await screenOf(host)).paused === 0, 6000);
        if (!resumed) {
            const f2 = await frame(p), K3 = new Array(400).fill(0);
            for (let j = 0; j < 6; j++) K3[10 + j] = KEY.B;
            await setScript(p, K3, f2, []);
            resumed = await until(async () => (await screenOf(host)).paused === 0, 8000);
        }
        ok(`${tag}: ${p.name} closes the pause menu`, !!resumed);
        await sleep(800);
    }
    for (const c of cmps) await c.run();
    const gsts = [];
    for (let i = 0; i < guests.length; i++) {
        const st = await snapStats(guests[i].name, guests[i]);
        gsts.push(st);
        ok(`${tag}: ${guests[i].name}'s canonical frames identical to the host's`, cmps[i].compared > 20 && cmps[i].mismatched === 0, `${cmps[i].equal}/${cmps[i].compared}; repairs ${st.repairs}`);
        ok(`${tag}: ${guests[i].name}'s own game equal to the canonical one (digest)`, st.digCompared > 500 && st.digMismatch === 0 && !st.persRepairs,
           `${st.digEqual}/${st.digCompared}, rebuilt ${st.persRepairs}, shadow ${st.canonUsPerFrame} us/frame`);
    }
    R.guests = gsts; R.compare = cmps.map((c) => ({ ...c, run: undefined, reset: undefined }));
    R.wasm = await host.ev(`fetch('katam.wasm').then(r=>r.arrayBuffer()).then(b=>crypto.subtle.digest('SHA-1',b)).then(h=>Array.from(new Uint8Array(h)).map(x=>x.toString(16).padStart(2,'0')).join(''))`);
    if (nPages === 2) await roomsMore(tag, R, host, guests[0], code);
    for (const p of all) { try { await p.close(); } catch (e) { /* gone */ } delete pages[p.name]; }
}

/* FREE ROAM, two pages, after the main checks: the waiting hint at a door to
 * the hub (room 390, cell 8,9: OBJ_DOOR_TO_HUB) on each page while the other
 * player is elsewhere (localmp.c PortLmpWaitingFor, drawn by the party HUD);
 * then the host leaves, hosts FREE ROAM again from its own menus and the
 * guest joins again: rooms mode on, each page on its own player, digest equal. */
async function waitingFor(p) {
    return p.ev('(function(){var b=Module._malloc(4);Module.HEAP32[b>>2]=-1;var k=Module._PortLmpWaitingFor(b),w=Module.HEAP32[b>>2];Module._free(b);return [k,w];})()');
}
async function roomsMore(tag, R, host, guest, code) {
    const HUB = [390, 8, 9];
    R.hint = [];
    for (const [who, other, page] of [[0, 1, host], [1, 0, guest]]) {
        const f0 = (await frame(host)) + 60;
        await setScript(host, new Array(600).fill(0), f0, [[0, who, HUB[0], HUB[1], HUB[2]], [1, other, 203, 60, 5]]);
        await setScript(guest, new Array(600).fill(0), f0, []);
        await until(async () => (await frame(page)) >= f0 + 90, 20000);
        let seen = null;
        for (let i = 0; i < 20 && !seen; i++) { const w = await waitingFor(page); if (w[0] === 1) seen = w; else await waitFrames(page, 6); }
        await sleep(300);
        await shot(page, `${tag}-05-hint-${page.name}.png`);
        const sc = await screenOf(page);
        R.hint.push({ page: page.name, seen, screen: sc });
        ok(`${tag}: ${page.name} at the door to the hub, the other player elsewhere: "THE DOOR WAITS FOR ${other + 1}P"`,
           !!seen && seen[1] === other && sc.kroom === HUB[0], JSON.stringify({ seen, rooms: sc.rooms }));
    }
    // leave, host FREE ROAM again, join again
    for (const p of [host, guest]) await p.ev('window.__script = null; window.__seats = null; Module._PortSetKeys(0)');
    await host.ev('katamOnline.leave()');
    const hOut = await until(async () => { const s = await stats(host); return s && !s.session ? s : null; }, 10000);
    ok(`${tag}: the host ends the game`, !!hOut);
    const left = await until(async () => { const s = await stats(guest); return s && s.phase === 'hostleft' ? s : null; }, 15000);
    ok(`${tag}: the guest is told the host left`, !!left);
    const hf0 = await frame(host);
    await until(async () => (await frame(host)) >= hf0 + 300, 60000, 30);
    let atFs = false;
    for (let i = 0; i < 6 && !atFs; i++) { await press(host, 'START'); await waitFrames(host, 100); atFs = (await fsPrint(host)) === host.fsPrint; }
    ok(`${tag}: again: the host is back at FILE SELECT`, atFs);
    await waitFrames(host, 150);
    await press(host, 'A'); await waitFrames(host, 200);
    await toOnline(host);
    await press(host, 'A');
    const code2 = await until(async () => { const s = await stats(host); return s && s.code && s.phase === 'hosting' && (await screen(host)) === 2 * 256 + 2 ? s.code : null; }, 20000);
    ok(`${tag}: again: the host makes a new room`, !!code2 && code2 !== code, code2);
    await guest.ev('document.getElementById("lnleave").click()');
    await until(async () => { const s = await stats(guest); return s && !s.session && !s.role; }, 10000);
    await waitFrames(guest, 200);
    await guest.ev(`katamOnline.join(${JSON.stringify(code2)})`);
    ok(`${tag}: again: the guest waits in the new room`, !!(await until(async () => { const s = await stats(guest); return s && s.phase === 'waiting'; }, 40000)));
    await sleep(1500);
    await press(host, 'DOWN'); await waitFrames(host, 10);
    ok(`${tag}: again: HOST GAME still offers FREE ROAM`, (await host.ev('Module._PortOuiRoomsMode()')) === 1 || (await (async () => { await press(host, 'RIGHT'); await waitFrames(host, 20); return (await host.ev('Module._PortOuiRoomsMode()')) === 1; })()));
    await shot(host, `${tag}-06-again-free-roam.png`);
    await press(host, 'A');
    ok(`${tag}: again: session started`, !!(await until(async () => { const st = await stats(host); return st && st.session; }, 20000)));
    ok(`${tag}: again: the guest took the image`, !!(await until(async () => { const st = await stats(guest); return st && st.session && st.joined; }, 30000)));
    await sleep(2500);
    ok(`${tag}: again: rooms mode on for both`, (await stats(host)).rooms === 1 && (await stats(guest)).rooms === 1);
    const r0 = (await frame(host)) + 60;
    await setScript(host, new Array(900).fill(0), r0, [[0, 0, 117, 44, 9], [0, 1, 203, 60, 5]]);
    await setScript(guest, new Array(900).fill(0), r0, []);
    const cmp = makeComparer(host, guest);
    await until(async () => (await frame(host)) >= r0 + 600, 30000);
    await cmp.run();
    const scs = [await screenOf(host), await screenOf(guest)];
    await shot(host, `${tag}-07-again-host.png`); await shot(guest, `${tag}-07-again-guest.png`);
    ok(`${tag}: again: each page on its own player in that player's room`, scs[0].lp === 0 && scs[1].lp === 1 && scs[0].room === 117 && scs[1].room === 203,
       JSON.stringify(scs.map((x) => [x.lp, x.room, x.kroom])));
    const st = await snapStats(guest.name, guest);
    ok(`${tag}: again: canonical identical, digest equal`, cmp.compared > 10 && cmp.mismatched === 0 && st.digMismatch === 0 && !st.persRepairs,
       `${cmp.equal}/${cmp.compared}; digest ${st.digEqual}/${st.digCompared}, rebuilt ${st.persRepairs}`);
    R.again = { code2, screens: scs, compare: { equal: cmp.equal, compared: cmp.compared } };
}

if (SHOTS) fs.writeFileSync(path.join(SHOTS, 'online-test-results.json'), JSON.stringify(results, null, 1));
console.log(`\n${pass} ok, ${fail} failed (${((Date.now() - T0) / 1000).toFixed(0)} s)`);
for (const p of Object.values(pages)) { try { await p.close(); } catch (e) { /* gone */ } }
process.exit(fail ? 1 : 0);
