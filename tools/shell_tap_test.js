// Taps the real page in a real browser.
//
// tools/shell_test.js checks the shell's logic against stubs, which is fast
// and catches the things that are decidable from the source.  It cannot
// answer the question that actually matters for a control on a phone: does a
// finger landing on this element produce the thing it is supposed to produce.
// That depends on the browser's own gesture handling, the compatibility mouse
// events, and every listener the page installed -- none of which a stub has.
//
// So this drives headless Chrome over the DevTools protocol, dispatches real
// touch sequences at real coordinates, and watches for the outcome.  Chrome is
// not Safari, but the rule that broke this page -- preventDefault() on
// touchstart suppresses the synthesised click -- is in the touch-events spec
// and Chrome implements it, so the class of bug does reproduce here.
//
//   make shell-tap-test        (builds web/katam.html first)
//   node tools/shell_tap_test.js [--keep] [--head]
//
// Needs google-chrome and python3 on PATH.  No ROM: every case here is about
// the curtain and the crash panel, which is what a visitor sees *before* a
// ROM, and the crash panel is opened synthetically.

const { spawn, spawnSync } = require('child_process');
const fs   = require('fs');
const path = require('path');
const os   = require('os');

const ROOT   = path.resolve(__dirname, '..');
const WEBDIR = path.join(ROOT, 'web');
const PAGE   = 'katam.html';
const KEEP   = process.argv.includes('--keep');
const HEAD   = process.argv.includes('--head');

let pass = 0, fail = 0;
function ok(name, cond, extra) {
  if (cond) { pass++; console.log('  ok   ' + name); }
  else { fail++; console.log('  FAIL ' + name + (extra ? '  (' + extra + ')' : '')); }
}

function chromeBinary() {
  for (const c of ['google-chrome', 'chromium', 'chromium-browser', 'google-chrome-stable']) {
    const r = spawnSync('which', [c], { encoding: 'utf8' });
    if (r.status === 0) return r.stdout.trim();
  }
  return null;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

//====================================================================
// A minimal CDP client on node's built-in WebSocket
//====================================================================

class CDP {
  constructor(ws) {
    this.ws = ws;
    this.id = 0;
    this.pending = new Map();
    this.handlers = new Map();
    ws.addEventListener('message', (ev) => {
      const msg = JSON.parse(ev.data);
      if (msg.id !== undefined) {
        const p = this.pending.get(msg.id);
        if (!p) return;
        this.pending.delete(msg.id);
        if (msg.error) p.reject(new Error(msg.error.message));
        else p.resolve(msg.result);
      } else {
        const hs = this.handlers.get(msg.method) || [];
        for (const h of hs) h(msg.params);
      }
    });
  }
  static async attach(url) {
    const ws = new WebSocket(url);
    await new Promise((res, rej) => {
      ws.addEventListener('open', res, { once: true });
      ws.addEventListener('error', rej, { once: true });
    });
    return new CDP(ws);
  }
  send(method, params) {
    const id = ++this.id;
    this.ws.send(JSON.stringify({ id, method, params: params || {} }));
    return new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
  }
  on(method, fn) {
    if (!this.handlers.has(method)) this.handlers.set(method, []);
    this.handlers.get(method).push(fn);
  }
  async eval(expr) {
    const r = await this.send('Runtime.evaluate', {
      expression: expr, returnByValue: true, awaitPromise: true,
      userGesture: false,          // never fake activation: it hides the bug
    });
    if (r.exceptionDetails)
      throw new Error(r.exceptionDetails.exception
        ? r.exceptionDetails.exception.description : 'eval threw');
    return r.result.value;
  }
}

//====================================================================
// Touch, as a finger does it
//====================================================================

async function tap(cdp, sel) {
  const box = await cdp.eval(`(function () {
    var el = document.querySelector(${JSON.stringify(sel)});
    if (!el) return null;
    var r = el.getBoundingClientRect();
    if (!r.width || !r.height) return null;
    return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
  })()`);
  if (!box) throw new Error('no box for ' + sel);

  const pt = [{ x: box.x, y: box.y, radiusX: 12, radiusY: 12, force: 1, id: 1 }];
  await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: pt });
  await sleep(30);
  await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
  await sleep(120);
  return box;
}

//====================================================================
main().catch((e) => { console.error(e); process.exit(2); });

async function main() {
  const chrome = chromeBinary();
  if (!chrome) { console.error('no chrome on PATH -- skipping'); process.exit(0); }
  if (!fs.existsSync(path.join(WEBDIR, PAGE))) {
    console.error('no web/' + PAGE + ' -- run `make web/katam.html` first');
    process.exit(2);
  }

  // A server, because file:// gives the page an opaque origin and half the
  // storage APIs it touches on startup then throw.
  // -u: the "Serving HTTP on ... port N" line is how the port is learned, and
  // a buffered stdout never delivers it.
  const server = spawn('python3', ['-u', '-m', 'http.server', '0', '--bind', '127.0.0.1'],
                       { cwd: WEBDIR, stdio: ['ignore', 'pipe', 'pipe'] });
  const port = await new Promise((res, rej) => {
    let buf = '';
    const grab = (d) => {
      buf += d.toString();
      const m = buf.match(/port (\d+)/);
      if (m) res(Number(m[1]));
    };
    server.stdout.on('data', grab);
    server.stderr.on('data', grab);
    setTimeout(() => rej(new Error('server did not start: ' + buf)), 8000);
  });

  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'katam-tap-'));
  const args = [
    '--remote-debugging-port=0',
    '--user-data-dir=' + profile,
    '--no-first-run', '--no-default-browser-check',
    '--disable-gpu', '--hide-scrollbars',
    // A phone, so the shell takes its touch layout and the touch listeners
    // are the ones under test.
    '--window-size=390,844',
    'http://127.0.0.1:' + port + '/' + PAGE,
  ];
  if (!HEAD) args.unshift('--headless=new');

  const browser = spawn(chrome, args, { stdio: ['ignore', 'pipe', 'pipe'] });
  const wsUrl = await new Promise((res, rej) => {
    let buf = '';
    browser.stderr.on('data', (d) => {
      buf += d.toString();
      const m = buf.match(/ws:\/\/[^\s]+/);
      if (m) res(m[0]);
    });
    setTimeout(() => rej(new Error('chrome did not start: ' + buf)), 20000);
  });

  const cleanup = () => {
    try { browser.kill('SIGKILL'); } catch (e) {}
    try { server.kill('SIGKILL'); } catch (e) {}
    if (!KEEP) try { fs.rmSync(profile, { recursive: true, force: true }); } catch (e) {}
  };
  process.on('exit', cleanup);

  const bro = await CDP.attach(wsUrl);
  const targets = await bro.send('Target.getTargets');
  const page = targets.targetInfos.find((t) => t.type === 'page' && /katam/.test(t.url));
  if (!page) throw new Error('no page target');
  const { sessionId } = await bro.send('Target.attachToTarget',
                                       { targetId: page.targetId, flatten: true });

  // Route everything through the page session.
  const rawSend = bro.ws.send.bind(bro.ws);
  bro.ws.send = (s) => {
    const o = JSON.parse(s);
    if (!/^Target\./.test(o.method)) o.sessionId = sessionId;
    rawSend(JSON.stringify(o));
  };

  await bro.send('Page.enable');
  await bro.send('Runtime.enable');
  await bro.send('Log.enable');

  // Touch, and a device pixel ratio, so the shell's mobile paths are live.
  await bro.send('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 5 });
  await bro.send('Emulation.setDeviceMetricsOverride', {
    width: 390, height: 844, deviceScaleFactor: 3, mobile: true,
  });

  // The signal that the ROM picker actually opened. Intercepting it also
  // stops a real dialog appearing, which headless cannot dismiss.
  let fileChooserEvents = 0;
  bro.on('Page.fileChooserOpened', () => { fileChooserEvents++; });
  await bro.send('Page.setInterceptFileChooserDialog', { enabled: true });

  const pageErrors = [];
  bro.on('Runtime.exceptionThrown', (p) => {
    pageErrors.push(p.exceptionDetails.exception
      ? p.exceptionDetails.exception.description
      : p.exceptionDetails.text);
  });

  // Give the wasm time to instantiate; the curtain is up either way.
  for (let i = 0; i < 60; i++) {
    const ready = await bro.eval(
      "!!document.getElementById('curtain') && document.readyState === 'complete'");
    if (ready) break;
    await sleep(250);
  }
  await sleep(1500);

  //------------------------------------------------------------------
  console.log('the curtain, on a fresh load');
  //------------------------------------------------------------------
  ok('the curtain is showing',
     (await bro.eval("!document.getElementById('curtain').classList.contains('hidden')")) === true);

  const labelVisible = await bro.eval(`(function () {
    var l = document.querySelector('#curtain label.filebtn');
    if (!l) return 'missing';
    var r = l.getBoundingClientRect();
    return r.width > 0 && r.height > 0 ? 'visible' : 'zero-size';
  })()`);
  ok('"Choose ROM file…" is on screen', labelVisible === 'visible', labelVisible);

  // What the label points at, and whether that target can be activated at
  // all. Safari and Chrome both refuse to open the picker for an input that
  // is display:none or visibility:hidden.
  const inputState = await bro.eval(`(function () {
    var i = document.getElementById('romfile');
    if (!i) return { missing: true };
    var cs = getComputedStyle(i);
    return {
      display: cs.display, visibility: cs.visibility,
      inSheet: !!i.closest('#chrome'),
      sheetVis: (function () {
        var c = document.getElementById('chrome');
        return c ? getComputedStyle(c).visibility : null;
      })(),
    };
  })()`);
  console.log('       #romfile: ' + JSON.stringify(inputState));

  // The general form of the bug, checked for every label on the page rather
  // than just this one: a label the player can see, pointing at a control
  // that is not rendered. Browsers decline to open a file picker for an
  // input in a `display: none` or `visibility: hidden` subtree, so such a
  // label is dead however cleanly the tap reaches it -- and it fails only
  // in the layout that puts the target in a hidden subtree, which for this
  // page is the phone one.
  const deadLabels = await bro.eval(`(function () {
    var out = [];
    var labels = document.querySelectorAll('label[for]');
    for (var n = 0; n < labels.length; n++) {
      var l = labels[n];
      var lr = l.getBoundingClientRect();
      if (!lr.width || !lr.height) continue;            // not on screen: fine
      var t = document.getElementById(l.htmlFor);
      if (!t) { out.push({ label: l.textContent.trim(), why: 'no such target' }); continue; }
      // offsetParent is null for a display:none subtree; position:fixed is
      // the other way it can be null, hence the second test.
      var cs = getComputedStyle(t);
      var rendered = t.getClientRects().length > 0;
      if (!rendered || cs.visibility === 'hidden')
        out.push({ label: l.textContent.trim(), target: l.htmlFor,
                   display: cs.display, visibility: cs.visibility, rendered: rendered });
    }
    return out;
  })()`);
  ok('every visible label points at a rendered control',
     deadLabels.length === 0, JSON.stringify(deadLabels));

  // Instrument the click path so a failure says *where* it stopped.
  await bro.eval(`(function () {
    window.__saw = { labelClick: 0, inputClick: 0, touchDefaultPrevented: null };
    var l = document.querySelector('#curtain label.filebtn');
    l.addEventListener('click', function () { window.__saw.labelClick++; });
    document.getElementById('romfile')
      .addEventListener('click', function () { window.__saw.inputClick++; });
    l.addEventListener('touchstart', function (e) {
      // Read after the page's own listeners have had the event.
      setTimeout(function () { window.__saw.touchDefaultPrevented = e.defaultPrevented; }, 0);
    });
  })()`);

  await tap(bro, '#curtain label.filebtn');
  await sleep(400);
  const saw = await bro.eval('window.__saw');
  console.log('       after the tap: ' + JSON.stringify(saw) +
              '  fileChooserOpened=' + fileChooserEvents);

  ok('the tap is not swallowed by the touch handler',
     saw.touchDefaultPrevented === false, 'defaultPrevented=' + saw.touchDefaultPrevented);
  ok('the label receives a click', saw.labelClick > 0);
  ok('the label activates #romfile', saw.inputClick > 0);
  ok('the ROM picker opens', fileChooserEvents > 0, 'fileChooserOpened=' + fileChooserEvents);

  //------------------------------------------------------------------
  console.log('the sheet, whose labels point at inputs that no longer live in it');
  //------------------------------------------------------------------
  const before = fileChooserEvents;
  await tap(bro, '#menubtn');
  await sleep(500);
  const sheetOpen = await bro.eval("document.body.classList.contains('sheet-open')");
  ok('the menu button opens the sheet', sheetOpen === true);

  if (sheetOpen) {
    await tap(bro, '#chrome label[for="romfile"]');
    await sleep(400);
    ok('"Load ROM…" still opens the picker', fileChooserEvents > before,
       'fileChooserOpened went ' + before + ' -> ' + fileChooserEvents);

    const mid = fileChooserEvents;
    await tap(bro, '#chrome label[for="savefile"]');
    await sleep(400);
    ok('"Import save…" still opens the picker', fileChooserEvents > mid,
       'fileChooserOpened went ' + mid + ' -> ' + fileChooserEvents);

    await tap(bro, '#sheetclose');
    await sleep(500);
  }

  //------------------------------------------------------------------
  console.log('the crash panel');
  //------------------------------------------------------------------
  await bro.eval(`(function () {
    // Reach the panel the way a crash does, rather than by adding the class:
    // this exercises the real handler, including the report it builds.
    window.dispatchEvent(new ErrorEvent('error', {
      error: new Error('synthetic, from shell_tap_test'),
      message: 'synthetic, from shell_tap_test',
    }));
  })()`);
  await sleep(400);

  ok('the crash panel is up',
     (await bro.eval("document.body.classList.contains('crashed')")) === true);
  ok('the report has text',
     (await bro.eval("document.getElementById('crashreport').textContent.length")) > 40);

  await bro.eval(`(function () {
    window.__crash = { copyClick: 0, touchPrevented: null };
    var b = document.getElementById('crashcopy');
    b.addEventListener('click', function () { window.__crash.copyClick++; });
    b.addEventListener('touchstart', function (e) {
      setTimeout(function () { window.__crash.touchPrevented = e.defaultPrevented; }, 0);
    });
  })()`);

  await tap(bro, '#crashcopy');
  await sleep(300);
  const crashSaw = await bro.eval('window.__crash');
  console.log('       after the tap: ' + JSON.stringify(crashSaw));
  ok('the Copy report tap is not swallowed',
     crashSaw.touchPrevented === false, 'defaultPrevented=' + crashSaw.touchPrevented);
  ok('Copy report receives the click', crashSaw.copyClick > 0);

  const btnText = await bro.eval("document.getElementById('crashcopy').textContent");
  console.log('       button now reads: ' + JSON.stringify(btnText));
  ok('the button reports an outcome', /Copied|Copy failed/.test(btnText), btnText);

  // Selectability, which is the other half of the original report.
  const selectable = await bro.eval(`(function () {
    var cs = getComputedStyle(document.getElementById('crashreport'));
    return { userSelect: cs.webkitUserSelect || cs.userSelect,
             callout: cs.webkitTouchCallout || null };
  })()`);
  console.log('       #crashreport: ' + JSON.stringify(selectable));
  ok('the report is selectable', selectable.userSelect === 'text', selectable.userSelect);

  //------------------------------------------------------------------
  console.log('page errors');
  //------------------------------------------------------------------
  const realErrors = pageErrors.filter((e) => !/synthetic, from shell_tap_test/.test(e));
  ok('no unexpected exceptions', realErrors.length === 0, realErrors.join(' | ').slice(0, 300));

  console.log('\n' + pass + ' passed, ' + fail + ' failed');
  cleanup();
  process.exit(fail ? 1 : 0);
}
