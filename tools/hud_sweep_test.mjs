// hud_sweep_test.mjs -- the party HUD readable on every device: a sweep of
// windows, pixel densities and display settings over the real page, failing
// on a card (or the hint line) that is unreadable or misplaced.
//
//   node tools/hud_sweep_test.mjs --cdp 19361 --base http://127.0.0.1:18951/dist/
//        --rom-url http://127.0.0.1:18951/rom.gba --relay http://127.0.0.1:18811
//        --shots DIR [--scenarios local,together,freeroam,flat] [--quick] [--json FILE]
//        [--before http://127.0.0.1:18951/before/]   (also shoot an older build at the same moments)
//        [--windows phoneP,laptop2x] [--settings fit,strip]   (a subset)
//        [--older]   (--base is an older build: no settings hook, no per-card
//                     getters -- its whole card stack is measured, at its
//                     own settings; for before/after numbers)
//
// Needs, like tools/online_browser_test.mjs: a Chromium of your own on --cdp
// with WebGL2 (`--headless=new --ignore-gpu-blocklist --use-angle=gl-egl`),
// a static server for the dist and a ROM you own (outside the tree), and for
// the online scenarios a room server (netplay/dev-relay.mjs).  Screenshots
// are ROM-derived: write them outside the tree.
//
// Scenarios (each puts other players on the HUD, then sweeps the windows):
//   local     one page, two players on it (a pad joins), TOGETHER.
//   together  online, host + guest, TOGETHER, both pages swept.
//   freeroam  online FREE ROAM, the players in different rooms: the other
//             player's card is AWAY on both pages.
//   flat      ?renderer=2d (the page without WebGL2): the cards blended into
//             the 240x160 picture; the page's one notice about it.
// Windows: phone portrait 390x844 and landscape 844x390 at DPR 3 (touch, the
// page's pads up), a laptop 1440x800 at DPR 2, 1920x1080, an ultrawide
// 3440x1440, and 1280x720 at DPR 1, 2 and 3.  Settings on each: every PIXELS
// mode, every SCREEN shape, the HUD's original strip, the Classic renderer.
//
// Each sample asserts, from the exported getters (party_hud.c, present.c)
// and from the screenshot's pixels:
//   placed   every card's plate wholly inside the canvas and the visible
//            window, inside the picture unless it sits in a bar beside or
//            above it, and under no visible page element (DOM boxes of
//            everything painted over the canvas: pads, buttons, notices).
//   size     its lettering at least 1.5 page pixels per HUD pixel (a 6-row
//            glyph >= 9 CSS px), and over the play field the cards together
//            under 6% of the picture's area (they must not block play).
//   contrast WCAG 2.x thresholds, measured on the screenshot: the lettering
//            against its plate >= 4.5:1 (1.4.3, text), and the card against
//            what surrounds it >= 3:1 (1.4.11, the boundary of a component):
//            the better of plate-vs-surroundings and edge-vs-surroundings.
//            A dark opaque plate with a light edge passes on any background;
//            a see-through card (the old design) fails on whichever
//            background matches it.
import fs from 'fs';
import path from 'path';
import crypto from 'crypto';

const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf('--' + n); return i >= 0 ? argv[i + 1] : d; };
const has = (n) => argv.includes('--' + n);
const CDP_PORT = +flag('cdp', 19361);
const BASE = flag('base', 'http://127.0.0.1:18951/dist/');
const BEFORE = flag('before', '');
const ROM_URL = flag('rom-url', 'http://127.0.0.1:18951/rom.gba');
const RELAY = flag('relay', 'http://127.0.0.1:18811');
const SHOTS = flag('shots', '');
const QUICK = has('quick');
const SCEN = flag('scenarios', 'local,together,freeroam,flat').split(',');
const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let pass = 0, fail = 0;
const failures = [];
const ok = (name, c, extra) => {
    if (c) pass++;
    else { fail++; failures.push(name + (extra ? '  (' + extra + ')' : '')); console.log('  FAIL ' + name + (extra ? '  (' + extra + ')' : '')); }
    return c;
};
const okLoud = (name, c, extra) => { if (c) console.log('  ok   ' + name + (extra ? '  (' + extra + ')' : '')); return ok(name, c, extra); };
const T0 = Date.now();
const say = (t) => console.log(`[${((Date.now() - T0) / 1000).toFixed(0)}s] ${t}`);
const results = [];

class CDP {
    static async connect(port) {
        const ver = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
        const ws = new WebSocket(ver.webSocketDebuggerUrl);
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
const cdp = await CDP.connect(CDP_PORT);

const PAD_HOOK = `(function(){var pads=[null,null,null,null];function mk(i,id){var b=[];for(var k=0;k<17;k++)b.push({pressed:false,touched:false,value:0});return{index:i,id:id,connected:true,mapping:'standard',buttons:b,axes:[0,0,0,0],timestamp:performance.now()}}
Object.defineProperty(Navigator.prototype,'getGamepads',{configurable:true,value:function(){return pads.slice()}});
window.__padPlug=function(i,id){pads[i]=mk(i,id);var e=new Event('gamepadconnected');e.gamepad=pads[i];window.dispatchEvent(e)};
window.__padSet=function(i,bs){var g=pads[i];if(!g)return;g.buttons.forEach(function(b,k){var on=bs.indexOf(k)>=0;b.pressed=on;b.value=on?1:0});g.timestamp=performance.now()}})();`;
const SCRIPT_HOOK = `(function(){var t=setInterval(function(){var M=window.Module;if(!M||!M.portRuntimeUp||!M._PortSetKeys||!M.portPresent||M.__hooked)return;clearInterval(t);M.__hooked=true;var present=M.portPresent;M.portPresent=function(a,b,c){if(present)present(a,b,c);if(window.__script){try{M._PortSetKeys(window.__script(M._PortFrameNumber())&0x3FF)}catch(e){window.__script=null}}}},20)})();`;

async function openPage(name, base, extra = {}) {
    const { browserContextId } = await cdp.send('Target.createBrowserContext');
    const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank', browserContextId, newWindow: true });
    const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
    const s = (m, p) => cdp.send(m, p, sessionId);
    const logs = [];
    cdp.handlers.push((m) => {
        if (m.sessionId !== sessionId) return;
        if (m.method === 'Runtime.consoleAPICalled') logs.push(m.params.args.map((a) => a.value ?? a.description ?? '').join(' '));
    });
    const p = { name, s, logs,
        ev: async (expr) => {
            const r = await s('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
            if (r.exceptionDetails) throw new Error(expr.slice(0, 80) + ': ' + JSON.stringify(r.exceptionDetails).slice(0, 300));
            return r.result.value;
        },
        close: async () => { try { await cdp.send('Target.closeTarget', { targetId }); } catch (e) { /* gone */ }
                             try { await cdp.send('Target.disposeBrowserContext', { browserContextId }); } catch (e) { /* gone */ } },
    };
    await s('Page.enable'); await s('Runtime.enable');
    await s('Emulation.setDeviceMetricsOverride', { width: 960, height: 640, deviceScaleFactor: 1, mobile: false });
    await s('Page.addScriptToEvaluateOnNewDocument', { source: PAD_HOOK });
    await s('Page.addScriptToEvaluateOnNewDocument', { source: SCRIPT_HOOK });
    const u = new URL(base + 'index.html');
    u.searchParams.set('rom', ROM_URL);
    u.searchParams.set('netserver', RELAY);
    for (const [k, v] of Object.entries(extra)) u.searchParams.set(k, v);
    await s('Page.navigate', { url: u.toString() });
    return p;
}
async function until(fn, ms, step = 100) {
    const t = Date.now();
    for (;;) { let v; try { v = await fn(); } catch (e) { v = false; } if (v) return v; if (Date.now() - t > ms) return false; await sleep(step); }
}
const KEYS = { A: ['KeyZ', 'z', 90], B: ['KeyX', 'x', 88], START: ['Enter', 'Enter', 13], DOWN: ['KeyS', 's', 83], RIGHT: ['KeyD', 'd', 68] };
async function press(p, k, holdMs = 90, gapMs = 160) {
    const [code, key, vk] = KEYS[k];
    await p.s('Input.dispatchKeyEvent', { type: 'keyDown', code, key, windowsVirtualKeyCode: vk });
    await sleep(holdMs);
    await p.s('Input.dispatchKeyEvent', { type: 'keyUp', code, key, windowsVirtualKeyCode: vk });
    await sleep(gapMs);
}
const frame = (p) => p.ev('Module._PortFrameNumber()');
const waitFrames = async (p, n) => { const f0 = await frame(p); await until(async () => (await frame(p)) >= f0 + n, 60000, 30); };
const stats = (p) => p.ev('window.katamOnline ? katamOnline.stats() : null');
const ouiScreen = (p) => p.ev('Module._PortOuiScreen ? Module._PortOuiScreen() : -1');

async function toGameSelect(p) {
    await until(() => p.ev('!!(window.Module && Module.portRuntimeUp && Module._PortFrameNumber() > 30)'), 60000);
    await until(async () => (await frame(p)) >= 400, 60000, 30);
    await press(p, 'START'); await waitFrames(p, 100);
    await press(p, 'START'); await waitFrames(p, 100);
    await press(p, 'A'); await waitFrames(p, 100);
}
async function wasmIsThisBuild(p, distDir) {
    const wasm = await p.ev(`fetch(performance.getEntriesByType('resource').map(function(r){return r.name}).filter(function(n){return /katam\\.wasm/.test(n)})[0]).then(function(r){return r.arrayBuffer()}).then(function(b){return crypto.subtle.digest('SHA-256', b)}).then(function(h){return Array.from(new Uint8Array(h)).map(function(x){return x.toString(16).padStart(2,'0')}).join('')})`);
    const mine = crypto.createHash('sha256').update(fs.readFileSync(path.join(distDir, 'katam.wasm'))).digest('hex');
    return [wasm === mine, wasm.slice(0, 12) + ' vs ' + mine.slice(0, 12)];
}

// --- the windows and the settings ----------------------------------------------------
const WINDOWS = [
    { tag: 'phoneP', w: 390, h: 844, dpr: 3, mobile: true },
    { tag: 'phoneL', w: 844, h: 390, dpr: 3, mobile: true },
    { tag: 'laptop2x', w: 1440, h: 800, dpr: 2 },
    { tag: 'fhd', w: 1920, h: 1080, dpr: 1 },
    { tag: 'ultrawide', w: 3440, h: 1440, dpr: 1 },
    { tag: 'hd1x', w: 1280, h: 720, dpr: 1 },
    { tag: 'hd2x', w: 1280, h: 720, dpr: 2 },
    { tag: 'hd3x', w: 1280, h: 720, dpr: 3 },
];
const BASE_SET = 'renderer=0\nscaling=1\naspect=0\nhud=0\nhudscale=0\nzoom=1.000\npartyhud=2\n';
const SETTINGS = [
    { tag: 'fit', s: '' },
    { tag: 'sharp', s: 'scaling=0\n' },
    { tag: 'smooth', s: 'scaling=2\n' },
    ...['3x2', '4x3', '16x10', '16x9', '21x9', 'fill'].map((t, i) => ({ tag: 'a' + t, s: `aspect=${i + 1}\n` })),
    { tag: 'strip', s: 'hud=1\naspect=5\n' },
    { tag: 'classic', s: 'renderer=1\n' },
];
const WSEL = flag('windows', '');
const windows = WSEL ? WINDOWS.filter((w) => WSEL.split(',').includes(w.tag)) : QUICK ? WINDOWS.filter((w) => ['phoneP', 'laptop2x', 'ultrawide'].includes(w.tag)) : WINDOWS;
const SSEL = flag('settings', '');
const settings = SSEL ? SETTINGS.filter((x) => SSEL.split(',').includes(x.tag)) : QUICK ? SETTINGS.filter((s) => ['fit', 'sharp', 'a3x2', 'strip', 'classic'].includes(s.tag)) : SETTINGS;

async function setWindow(p, W) {
    await p.s('Emulation.setDeviceMetricsOverride', { width: W.w, height: W.h, deviceScaleFactor: W.dpr, mobile: !!W.mobile });
    await p.s('Emulation.setTouchEmulationEnabled', { enabled: !!W.mobile, maxTouchPoints: W.mobile ? 5 : 1 });
    await p.s('Emulation.setEmitTouchEventsForMouse', { enabled: !!W.mobile, configuration: 'mobile' });
    // the page's touch pads: on for the phones, off for the rest (as a
    // person would have them; they are keep-outs for the cards)
    await p.ev(`(function(){var t=Module._shell&&Module._shell.touch;if(t&&t.on()!==${!!W.mobile})t.toggle()})()`);
    await sleep(800);       // the page re-measures its pads (keep-outs) twice a second
}
async function setSettings(p, text) {
    if (!(await p.ev('!!Module._PortSettingsSetJs'))) return false;     // an older build: as it is
    await p.ev(`(function(){var t=${JSON.stringify(BASE_SET + text)},n=t.length,b=Module._malloc(n+1);for(var i=0;i<n;i++)Module.HEAPU8[b+i]=t.charCodeAt(i);Module.HEAPU8[b+n]=0;Module._PortSettingsSetJs(b);Module._free(b);})()`);
}

// --- one sample: getters, DOM, screenshot pixels --------------------------------------------
// In the page: the geometry (framebuffer -> CSS -> screenshot pixels), the
// cards and the hint from the getters, and the page elements over each.
const GEOM = `(function(){
  var M=Module, c=M.portGlCanvas || document.getElementById('screen'), cr=c.getBoundingClientRect(), dpr=window.devicePixelRatio||1;
  var sx=c.width/cr.width, sy=c.height/cr.height, gl=!!M.portGl;
  var boxes=[];
  if (M._PortPartyHudCardJs) {
    for (var i=0;i<3;i++){ var w=M._PortPartyHudCardJs(i,2); if(w>0) boxes.push({kind:'card',i:i,x:M._PortPartyHudCardJs(i,0),y:M._PortPartyHudCardJs(i,1),w:w,h:M._PortPartyHudCardJs(i,3),
        slot:M._PortPartyHudCardJs(i,4),state:M._PortPartyHudCardJs(i,5),whole:M._PortPartyHudCardJs(i,6)}); }
    var hw=M._PortPartyHudCardJs(3,2); if(hw>0) boxes.push({kind:'hint',x:M._PortPartyHudCardJs(3,0),y:M._PortPartyHudCardJs(3,1),w:hw,h:M._PortPartyHudCardJs(3,3)});
  } else if (M._PortPartyHudAtJs && M._PortPartyHudAtJs(2)>0) {
    boxes.push({kind:'stack',x:M._PortPartyHudAtJs(0),y:M._PortPartyHudAtJs(1),w:M._PortPartyHudAtJs(2),h:M._PortPartyHudAtJs(3),whole:M._PortPartyHudAtJs(5)});
  }
  var inBand = M._PortPartyHudAtJs ? M._PortPartyHudAtJs(4) : 0;
  var scale = M._PortPartyHudCardJs ? M._PortPartyHudCardJs(3,4)/100 : 0;
  var dst = M._PortShowDstJs ? [0,1,2,3].map(function(k){return M._PortShowDstJs(k)}) : null;
  if (!gl) {
    // the 2D canvas: the cards are in the 240x160 picture's pixels, the
    // canvas is that picture scaled by a whole number
    var f=c.width/240;
    boxes.forEach(function(b){b.x*=f;b.y*=f;b.w*=f;b.h*=f;});
    dst=[0,0,c.width,c.height]; scale=f;
  }
  // framebuffer px -> CSS px
  boxes.forEach(function(b){ b.cx=cr.left+b.x/sx; b.cy=cr.top+b.y/sy; b.cw=b.w/sx; b.ch=b.h/sy; });
  // page elements painted over each box
  function painted(el){
    var cs=getComputedStyle(el); if(cs.display==='none'||cs.visibility==='hidden')return false;
    for(var e=el;e&&e!==document.body;e=e.parentElement){var o=parseFloat(getComputedStyle(e).opacity);if(o<0.05)return false;}
    var bg=cs.backgroundColor, hasBg=bg&&!/rgba\\(.*,\\s*0\\)$/.test(bg)&&bg!=='transparent';
    var bw=parseFloat(cs.borderTopWidth)+parseFloat(cs.borderLeftWidth);
    var txt=Array.from(el.childNodes).some(function(n){return n.nodeType===3&&n.textContent.trim()});
    return hasBg||bw>0||txt||/^(IMG|SVG|CANVAS|VIDEO|BUTTON|INPUT)$/i.test(el.tagName)||cs.backgroundImage!=='none';
  }
  var all=Array.from(document.body.querySelectorAll('*')).filter(function(el){return el!==c&&!el.contains(c)&&el.tagName!=='SCRIPT'&&el.tagName!=='STYLE'});
  boxes.forEach(function(b){
    b.over=[];
    all.forEach(function(el){var r=el.getBoundingClientRect(); if(r.width<1||r.height<1)return;
      if(r.left<b.cx+b.cw-0.5&&b.cx+0.5<r.right&&r.top<b.cy+b.ch-0.5&&b.cy+0.5<r.bottom&&painted(el)) b.over.push((el.id?'#'+el.id:el.tagName.toLowerCase()+(el.className&&typeof el.className==='string'?'.'+el.className.split(' ')[0]:'')));});
  });
  return {cr:{l:cr.left,t:cr.top,w:cr.width,h:cr.height}, fb:[c.width,c.height], sx:sx, sy:sy, dpr:dpr, gl:gl, dst:dst, inBand:inBand, scale:scale, boxes:boxes,
          vw:innerWidth, vh:innerHeight, keep:M._PortPartyHudKeepJs?[0,1,2,3,4,5,6,7,8,9,10,11].map(function(s){return [0,1,2,3].map(function(k){return M._PortPartyHudKeepJs(s,k)})}).filter(function(r){return r[2]>0}):[], cards:M._PortPartyHudCardsJs?M._PortPartyHudCardsJs():0, hint:M._PortPartyHudHintJs?UTF8ToStringSafe(M._PortPartyHudHintJs()):''};
  function UTF8ToStringSafe(p){var s='';for(var i=0;i<64;i++){var ch=M.HEAPU8[p+i];if(!ch)break;s+=String.fromCharCode(ch)}return s}
})()`;

// Pixels: relative luminance and WCAG contrast.
const lin = (v) => { v /= 255; return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); };
const lum = (r, g, b) => 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
const contrast = (a, b) => (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05);

// In the page: decode the screenshot, and for a box (screenshot pixels) the
// colours inside, its edge ring and the ring just outside it.
const ANALYSE = `async function(b64, boxes){
  var blob=await (await fetch('data:image/png;base64,'+b64)).blob(), bm=await createImageBitmap(blob);
  var oc=new OffscreenCanvas(bm.width,bm.height), g=oc.getContext('2d'); g.drawImage(bm,0,0);
  return boxes.map(function(b){
    var pad=Math.max(2,Math.round(b.unit*1.5));
    var x0=Math.max(0,Math.floor(b.x)-pad), y0=Math.max(0,Math.floor(b.y)-pad), x1=Math.min(bm.width,Math.ceil(b.x+b.w)+pad), y1=Math.min(bm.height,Math.ceil(b.y+b.h)+pad);
    if (x1<=x0||y1<=y0) return null;
    var d=g.getImageData(x0,y0,x1-x0,y1-y0).data, W=x1-x0;
    var inside={}, edge={}, ring={}, nIn=0, nEdge=0, nRing=0;
    var u=b.unit;
    for(var y=y0;y<y1;y++)for(var x=x0;x<x1;x++){
      var i=((y-y0)*W+(x-x0))*4, key=(d[i]>>2)+','+(d[i+1]>>2)+','+(d[i+2]>>2);
      var inX=x>=b.x&&x<b.x+b.w, inY=y>=b.y&&y<b.y+b.h;
      if(inX&&inY){
        var ex=x<b.x+u||x>=b.x+b.w-u||y<b.y+u||y>=b.y+b.h-u;
        var corner=(x<b.x+u||x>=b.x+b.w-u)&&(y<b.y+u||y>=b.y+b.h-u);
        if(corner) continue;
        if(ex){edge[key]=(edge[key]||0)+1;nEdge++;} else {inside[key]=(inside[key]||0)+1;nIn++;}
      } else if (x<b.x-1||x>=b.x+b.w+1||y<b.y-1||y>=b.y+b.h+1) { ring[key]=(ring[key]||0)+1;nRing++; }
    }
    function top(h){return Object.keys(h).map(function(k){return [k.split(',').map(function(v){return (+v<<2)+2}),h[k]]}).sort(function(a,b){return b[1]-a[1]})}
    return {inside:top(inside).slice(0,24), nIn:nIn, edge:top(edge).slice(0,6), nEdge:nEdge, ring:top(ring).slice(0,40), nRing:nRing};
  });
}`;

function judge(a) {
    // plate: the commonest colour inside; ink: the colour of >= 2% coverage
    // that stands out most from it.
    if (!a || !a.inside.length) return null;
    const L = (c) => lum(c[0], c[1], c[2]);
    const plate = a.inside[0][0], Lp = L(plate);
    let ink = 1;
    for (const [c, n] of a.inside) if (n >= a.nIn * 0.02) ink = Math.max(ink, contrast(L(c), Lp));
    const edgeC = a.edge.length ? a.edge[0][0] : plate, Le = L(edgeC);
    // the surroundings: luminance median of the ring
    const ringL = [];
    for (const [c, n] of a.ring) ringL.push([L(c), n]);
    ringL.sort((x, y) => x[0] - y[0]);
    let acc = 0, med = 0;
    for (const [l, n] of ringL) { acc += n; if (acc >= a.nRing / 2) { med = l; break; } }
    const plateCover = a.inside[0][1] / Math.max(1, a.nIn);
    const bound = Math.max(contrast(Lp, med), contrast(Le, med));
    return { plate, plateCover: +plateCover.toFixed(2), ink: +ink.toFixed(2), bound: +bound.toFixed(2),
             plateVsRing: +contrast(Lp, med).toFixed(2), edgeVsRing: +contrast(Le, med).toFixed(2) };
}

async function sample(p, label, opts = {}) {
    // the geometry before and after the screenshot must agree (a card that
    // moved in between -- a keep-out update, a card opening -- is sampled
    // again)
    let g, shot;
    for (let tries = 0; tries < 4; tries++) {
        g = await p.ev(GEOM);
        shot = await p.s('Page.captureScreenshot', { format: 'png' });
        const g2 = await p.ev(GEOM);
        const key = (x) => JSON.stringify(x.boxes.map((b) => [b.x, b.y, b.w, b.h, b.state]));
        if (key(g) === key(g2)) break;
        await sleep(300);
    }
    const file = SHOTS ? path.join(SHOTS, label.replace(/[^\w.-]+/g, '_') + '.png') : '';
    if (file && opts.keep) { fs.mkdirSync(SHOTS, { recursive: true }); fs.writeFileSync(file, Buffer.from(shot.data, 'base64')); }
    // CSS px -> screenshot px (the screenshot is at the device's density)
    const k = g.dpr, unitCss = g.scale > 0 ? g.scale / g.sx : (g.dst ? g.dst[3] / 160 / g.sx : 1);
    const boxes = g.boxes.map((b) => ({ x: b.cx * k, y: b.cy * k, w: b.cw * k, h: b.ch * k, unit: Math.max(1, unitCss * k) }));
    const an = boxes.length ? await p.s('Runtime.callFunctionOn', { functionDeclaration: ANALYSE, arguments: [{ value: shot.data }, { value: boxes }],
        executionContextId: undefined, objectId: (await p.s('Runtime.evaluate', { expression: 'window' })).result.objectId, awaitPromise: true, returnByValue: true }) : null;
    const res = an ? an.result.value : [];
    const out = { label, geom: { keep: g.keep, fb: g.fb, dst: g.dst, inBand: g.inBand, scale: g.scale, dpr: g.dpr, gl: g.gl, cards: g.cards, hint: g.hint }, boxes: [] };
    g.boxes.forEach((b, i) => out.boxes.push(Object.assign({ kind: b.kind, i: b.i, state: b.state, whole: b.whole, fbRect: [b.x, b.y, b.w, b.h], over: b.over }, judge(res[i]) || {})));
    if (opts.assert === false) { results.push(out); return out; }
    // --- the assertions
    const cssUnit = g.scale / g.sx;     // CSS px per HUD px
    for (const b of g.boxes) {
        const name = `${label} ${b.kind}${b.i !== undefined ? b.i : ''}`;
        const inCanvas = b.x >= 0 && b.y >= 0 && b.x + b.w <= g.fb[0] && b.y + b.h <= g.fb[1];
        const inWindow = b.cx >= -0.5 && b.cy >= -0.5 && b.cx + b.cw <= g.vw + 0.5 && b.cy + b.ch <= g.vh + 0.5;
        const d = g.dst, inPic = d && b.x >= d[0] - 1 && b.y >= d[1] - 1 && b.x + b.w <= d[0] + d[2] + 1 && b.y + b.h <= d[1] + d[3] + 1;
        ok(`${name}: inside the canvas and the window`, inCanvas && inWindow, JSON.stringify([b.x, b.y, b.w, b.h]) + ' fb ' + g.fb + ' css ' + [b.cx, b.cy, b.cw, b.ch].map(Math.round) + ' win ' + g.vw + 'x' + g.vh);
        ok(`${name}: inside the picture (or in the bar beside it)`, inPic || (b.kind !== 'hint' && g.inBand), JSON.stringify([b.x, b.y, b.w, b.h]) + ' dst ' + JSON.stringify(d));
        ok(`${name}: under no page element`, b.over.length === 0, b.over.join(','));
        if (g.gl && b.kind !== 'stack')
            ok(`${name}: big enough to read (>= 1.5 CSS px per HUD px)`, cssUnit >= 1.5 - 1e-6, cssUnit.toFixed(2));
    }
    if (g.gl && !g.inBand && g.dst) {
        const area = g.boxes.filter((b) => b.kind === 'card').reduce((s, b) => s + b.w * b.h, 0);
        ok(`${label}: over the play field the cards take under 6% of the picture`, area / (g.dst[2] * g.dst[3]) < 0.06,
           (100 * area / (g.dst[2] * g.dst[3])).toFixed(1) + '%');
    }
    out.boxes.forEach((b) => {
        if (b.ink === undefined) return;
        const name = `${label} ${b.kind}${b.i !== undefined ? b.i : ''}`;
        ok(`${name}: lettering against its plate >= 4.5:1`, b.ink >= 4.5, `ink ${b.ink}, plate ${b.plate} cover ${b.plateCover}`);
        ok(`${name}: the card against its surroundings >= 3:1`, b.bound >= 3, `plate/ring ${b.plateVsRing}, edge/ring ${b.edgeVsRing}`);
    });
    results.push(out);
    return out;
}

async function sweep(p, tag, { keepEvery = 1, expectCards = 1, expectAway = false } = {}) {
    let n = 0;
    for (const W of windows) {
        await setWindow(p, W);
        const older = !(await p.ev('!!Module._PortSettingsSetJs'));
        for (const S of (older ? settings.slice(0, 1) : settings)) {
            await setSettings(p, S.s);
            await waitFrames(p, 12);
            await sleep(120);
            const keep = (n++ % keepEvery) === 0 || S.tag === 'fit';
            const r = await sample(p, `${tag}-${W.tag}-${S.tag}`, { keep });
            ok(`${tag}-${W.tag}-${S.tag}: ${expectCards} card(s) up`, r.geom.cards >= expectCards, `cards ${r.geom.cards}`);
            if (expectAway) ok(`${tag}-${W.tag}-${S.tag}: the other player's card says AWAY`,
                               r.boxes.some((b) => b.kind === 'card' && b.state === 3), JSON.stringify(r.boxes.map((b) => b.state)));
        }
    }
    await setSettings(p, '');
    say(`${tag}: ${n} samples, ${fail} failures so far`);
}

// Before / after: the same windows at the same moment on an older build.
async function beforeShots(tag, mk) {
    if (!BEFORE) return;
    const p = await mk(BEFORE);
    for (const W of windows.filter((w) => ['phoneP', 'phoneL', 'laptop2x', 'ultrawide'].includes(w.tag))) {
        await setWindow(p, W);
        await waitFrames(p, 20); await sleep(150);
        await sample(p, `BEFORE-${tag}-${W.tag}`, { keep: true, assert: false });
    }
    await p.close();
}

// --- scenarios ---------------------------------------------------------------------------
const XBOX = 'Xbox 360 Controller (XInput STANDARD GAMEPAD)';
async function localTwo(base) {
    const p = await openPage('local', base);
    await toGameSelect(p);
    await press(p, 'A'); await waitFrames(p, 60);
    await press(p, 'A'); await waitFrames(p, 60);
    await until(() => p.ev('Module._PortLmpInStory()'), 60000, 100);
    await waitFrames(p, 200);
    await p.ev('Module._PortWarp(0, 117, 44, 9)');
    await waitFrames(p, 120);
    await p.ev('(function(){var a=0x0203AD2C;Module.HEAPU8[a]=300&255;Module.HEAPU8[a+1]=300>>8;})()');
    await p.ev(`__padPlug(0, ${JSON.stringify(XBOX)})`); await waitFrames(p, 10);
    await p.ev('__padSet(0,[9])'); await sleep(150); await p.ev('__padSet(0,[])');
    await waitFrames(p, 60);
    return p;
}
async function online(base, freeRoam) {
    const host = await openPage('host', base);
    await toGameSelect(host);
    await press(host, 'A'); await waitFrames(host, 60);
    await press(host, 'DOWN'); await waitFrames(host, 20);
    await press(host, 'A'); await waitFrames(host, 80);
    await press(host, 'DOWN'); await waitFrames(host, 20);
    await press(host, 'A'); await waitFrames(host, 40);
    await press(host, 'A');
    const code = await until(async () => { const s = await stats(host); return s && s.code && s.phase === 'hosting' && (await ouiScreen(host)) === 2 * 256 + 2 ? s.code : null; }, 20000);
    const guest = await openPage('guest', base, { join: code });
    await until(async () => { const st = await stats(guest); return st && st.phase === 'waiting'; }, 40000);
    await sleep(1500);
    await press(host, 'DOWN'); await waitFrames(host, 10);
    if (freeRoam) { await press(host, 'RIGHT'); await waitFrames(host, 20); }
    await press(host, 'A');
    await until(async () => { const st = await stats(host); return st && st.session; }, 20000);
    await until(async () => { const st = await stats(guest); return st && st.session && st.joined; }, 30000);
    await until(() => host.ev('Module._PortLmpInStory()'), 60000, 200);
    await waitFrames(host, 300);
    await host.ev('Module._PortLnEvent(4, 0x0203AD2C, 300 & 255, 0, 0); Module._PortLnEvent(4, 0x0203AD2D, 300 >> 8, 0, 0)');
    await host.ev('Module._PortLnEvent(1, 0, 117, 44, 9)');
    await waitFrames(host, 5);
    await host.ev(freeRoam ? 'Module._PortLnEvent(1, 1, 203, 60, 5)' : 'Module._PortLnEvent(1, 1, 117, 46, 9)');
    await waitFrames(host, 240);
    return { host, guest, code };
}

for (const sc of SCEN) {
    try {
        if (sc === 'local') {
            say('# local: two players on one page');
            const p = await localTwo(BASE);
            if (BASE === flag('base', '') && !has('older')) {
                const [same, h] = await wasmIsThisBuild(p, path.join(ROOT, 'build/dist'));
                okLoud('the served katam.wasm is this build', same, h);
            }
            okLoud('local: the page runs on WebGL2', await p.ev('!!Module.portGl'));
            await sweep(p, 'local', { keepEvery: 4 });
            await p.close();
            await beforeShots('local', localTwo);
        } else if (sc === 'together' || sc === 'freeroam') {
            const fr = sc === 'freeroam';
            say(`# ${sc}: online host + guest`);
            const o = await online(BASE, fr);
            okLoud(`${sc}: both pages run on WebGL2`, (await o.host.ev('!!Module.portGl')) && (await o.guest.ev('!!Module.portGl')));
            if (fr) {
                const rooms = await o.host.ev('(function(){var q=Module._malloc(64),r=[];for(var i=0;i<2;i++){Module._PortLnKirby(i,q);r.push(Module.HEAP32[(q>>2)+2])}Module._free(q);return r})()');
                okLoud('freeroam: the two players are in different rooms', rooms[0] !== rooms[1], JSON.stringify(rooms));
            }
            await sweep(o.host, sc + '-host', { keepEvery: 4, expectAway: fr });
            await sweep(o.guest, sc + '-guest', { keepEvery: 4, expectAway: fr });
            await o.host.close(); await o.guest.close();
            if (BEFORE) {
                const b = await online(BEFORE, fr);
                for (const [who, p] of [['host', b.host], ['guest', b.guest]])
                    for (const W of windows.filter((w) => ['phoneP', 'phoneL', 'laptop2x', 'ultrawide'].includes(w.tag))) {
                        await setWindow(p, W); await waitFrames(p, 20); await sleep(150);
                        await sample(p, `BEFORE-${sc}-${who}-${W.tag}`, { keep: true, assert: false });
                    }
                await b.host.close(); await b.guest.close();
            }
        } else if (sc === 'flat') {
            say('# flat: ?renderer=2d');
            const p = await openPage('flat', BASE, { renderer: '2d' });
            await until(() => p.ev('!!(window.Module && Module.portRuntimeUp && Module._PortFrameNumber() > 30)'), 60000);
            const note = await until(() => p.ev('Array.from(document.querySelectorAll("#pnotices .pnotice")).map(function(e){return e.textContent}).join("|")'), 8000);
            okLoud('flat: the page says plainly that the display is the simple one', /Simple display/.test(note || ''), note);
            const st = await p.ev('katamStatus()');
            okLoud('flat: the status names the 2D fallback and why', /RENDERER: 2D FALLBACK - ASKED FOR BY \?RENDERER=2D/.test(st), st.replace(/\n/g, ' | '));
            okLoud('flat: display options reported unavailable', !(await p.ev('Module.portGl')));
            await toGameSelect(p);
            await press(p, 'A'); await waitFrames(p, 60);
            await press(p, 'A'); await waitFrames(p, 60);
            await until(() => p.ev('Module._PortLmpInStory()'), 60000, 100);
            await waitFrames(p, 200);
            await p.ev('Module._PortWarp(0, 117, 44, 9)'); await waitFrames(p, 120);
            await p.ev(`__padPlug(0, ${JSON.stringify(XBOX)})`); await waitFrames(p, 10);
            await p.ev('__padSet(0,[9])'); await sleep(150); await p.ev('__padSet(0,[])');
            await waitFrames(p, 60);
            for (const W of windows) {
                await setWindow(p, W); await waitFrames(p, 15); await sleep(150);
                const r = await sample(p, `flat-${W.tag}`, { keep: true });
                ok(`flat-${W.tag}: the party HUD is drawn into the 2D picture`, r.geom.cards >= 1 && r.boxes.some((b) => b.kind === 'card'), `cards ${r.geom.cards}`);
            }
            await p.close();
        }
    } catch (e) {
        ok(`scenario ${sc} ran to the end`, false, e.stack || e.message);
    }
}
if (flag('json', '')) fs.writeFileSync(flag('json', ''), JSON.stringify({ when: new Date().toISOString(), results, failures }, null, 1));
const byKind = {};
for (const f of failures) { const k = f.replace(/^\S+ (card\d|hint|stack)?:?/, '').replace(/\s+\(.*$/, '').trim(); byKind[k] = (byKind[k] || 0) + 1; }
if (fail) console.log('failures by check:', JSON.stringify(byKind, null, 1));
console.log(`\n${pass} passed, ${fail} failed (${results.length} samples, ${((Date.now() - T0) / 1000).toFixed(0)} s)`);
process.exit(fail ? 1 : 0);
