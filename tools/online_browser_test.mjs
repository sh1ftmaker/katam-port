// Online two-player play end to end in a real browser (web/online.js): two
// headless Chromium pages on build/dist, a room server, the game's own menus.
//
//   node tools/online_browser_test.mjs --cdp 9351 --base http://127.0.0.1:8941/dist/
//        --rom-url http://127.0.0.1:8941/rom.gba --relay http://127.0.0.1:8797
//        --shots DIR [--play 130] [--lag 120 --jitter 30 --loss 2]
//        [--cases menu,invite,drop,relay,errors] [--live]
//
// Needs: a Chromium already listening on --cdp with its own profile (never
// the shared one), a static server for the dist and for a ROM you own (served
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
const CASES = flag('cases', 'errors,menu,drop,relay').split(',');
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
               DOWN: ['ArrowDown', 'ArrowDown', 40], F1: ['F1', 'F1', 112], LEFT: ['ArrowLeft', 'ArrowLeft', 37], RIGHT: ['ArrowRight', 'ArrowRight', 39] };
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
async function toGameSelect(p) {
    await until(() => p.ev('!!(window.Module && Module.portRuntimeUp && Module._PortFrameNumber() > 30)'), 60000);
    await until(async () => (await frame(p)) >= 400, 60000, 30);
    await press(p, 'START'); await waitFrames(p, 100);     // the intro -> the title
    await press(p, 'START'); await waitFrames(p, 100);     // the title -> file select
    await press(p, 'A'); await waitFrames(p, 100);         // FILE 1 -> Game Select
}

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

async function startHost(p, tag) {
    await press(p, 'DOWN');                 // START
    await waitFrames(p, 10);
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
    // the game menu's Game page has an Online section (F1 opens the menu)
    const click = async (p, x, y) => { for (const type of ['mousePressed', 'mouseReleased'])
        await p.s('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1 }); };
    await click(guest, 930, 28);            // the gear
    await sleep(1000);
    await shot(guest, 'm-08-guest-imgui-online.png');
    const txt = await guest.ev("Module.portUiQuery ? Module.portUiQuery('online') : ''");
    ok('menu Online section has the numbers', /Path: direct/.test(txt) && /Rewinds/.test(txt), txt.replace(/\n/g, ' | '));
    await guest.ev('Module._PortUiSetOpen && Module._PortUiSetOpen(0)');

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
        ok('a guest rejoins with the same code (invite link)', !!back);
        await sleep(1500);
        await shot(guest2, 'd-03-rejoined-guest.png');
        await shot(host, 'd-03-rejoined-host.png');
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

if (SHOTS) fs.writeFileSync(path.join(SHOTS, 'online-test-results.json'), JSON.stringify(results, null, 1));
console.log(`\n${pass} ok, ${fail} failed (${((Date.now() - T0) / 1000).toFixed(0)} s)`);
for (const p of Object.values(pages)) { try { await p.close(); } catch (e) { /* gone */ } }
process.exit(fail ? 1 : 0);
