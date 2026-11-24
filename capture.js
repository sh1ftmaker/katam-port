// Screenshots and short clips of the game picture (docs/features/display.md).
//
//   F9                       a PNG of the picture, now
//   F10                      start / stop a clip (WebM where the browser has
//                            it, else MP4), at most CLIP_MAX_S seconds
//   SETTINGS > DISPLAY       SCREENSHOT / RECORD CLIP: the same, but taken as
//                            the menu closes, so the menu is not in it
//
// The page's own business: it reads the canvas the frame was just drawn
// into (katamCapture.frame is called by web/shell.html's present(),
// in the same task as the draw, which is what lets a WebGL canvas without
// preserveDrawingBuffer be read at all), crops to the picture
// (Module.portPicRect, platform/ui/present.c) and downloads a file.  Nothing
// reaches the game, so it is allowed online too.
(function () {
  'use strict';

  var CLIP_MAX_S = 20;
  var CLIP_MAX_SIDE = 960;            // the clip's longer side, at most
  var MENU_QUIET = 20;                // frames without the SETTINGS screen = closed
  var MENU_WAIT_MS = 30000;           // give up waiting and take it anyway

  var shotWant = 0;                   // 0 none, 1 now, 2 when the menu closes
  var shotSince = 0;
  var clipWant = 0, clipSince = 0;    // 2: start when the menu closes
  var rec = null;                     // { mr, canvas, ctx, chunks, t0, timer, dest, type }
  var badge = null, badgeT = 0;

  function M() { return window.Module || {}; }

  function source() {
    var m = M();
    var c = m.portGlCanvas || document.getElementById('screen');
    if (!c || !c.width || !c.height) return null;
    var r = m.portGlCanvas && m.portPicRect;
    var x = 0, y = 0, w = c.width, h = c.height;
    if (r && r[2] > 0 && r[3] > 0) {
      x = Math.max(0, r[0]); y = Math.max(0, r[1]);
      w = Math.min(r[2], c.width - x); h = Math.min(r[3], c.height - y);
    }
    if (w <= 0 || h <= 0) return null;
    return { c: c, x: x, y: y, w: w, h: h };
  }

  function menuUp() {
    var m = M();
    if (!m._PortPocScreenLastDraw || !m._PortFrameNumber) return false;
    var d = (m._PortFrameNumber() >>> 0) - (m._PortPocScreenLastDraw() >>> 0);
    return d >= 0 && d < MENU_QUIET;
  }

  function stamp() {
    var d = new Date(), p = function (n) { return (n < 10 ? '0' : '') + n; };
    return d.getFullYear() + p(d.getMonth() + 1) + p(d.getDate()) + '-' +
           p(d.getHours()) + p(d.getMinutes()) + p(d.getSeconds());
  }

  function download(blob, name) {
    var url = URL.createObjectURL(blob);
    var a = document.createElement('a');
    a.href = url; a.download = name; a.style.display = 'none';
    document.body.appendChild(a);
    a.click();
    setTimeout(function () { URL.revokeObjectURL(url); a.remove(); }, 15000);
  }

  function say(text, ms) {
    if (!badge) {
      badge = document.createElement('div');
      badge.setAttribute('role', 'status');
      badge.style.cssText = 'position:fixed;left:50%;top:calc(env(safe-area-inset-top,0px) + 8px);' +
        'transform:translateX(-50%);z-index:9999;padding:4px 10px;border-radius:6px;' +
        'background:rgba(0,0,0,.72);color:#fff;font:600 13px system-ui,sans-serif;' +
        'pointer-events:none;';
      document.body.appendChild(badge);
    }
    badge.textContent = text;
    badge.hidden = false;
    clearTimeout(badgeT);
    if (ms) badgeT = setTimeout(function () { badge.hidden = true; }, ms);
  }

  function grab() {
    var s = source();
    if (!s) { say('No picture to save yet', 2000); return; }
    var out = document.createElement('canvas');
    out.width = s.w; out.height = s.h;
    var ctx = out.getContext('2d', { alpha: false });
    ctx.imageSmoothingEnabled = false;
    ctx.drawImage(s.c, s.x, s.y, s.w, s.h, 0, 0, s.w, s.h);
    out.toBlob(function (b) {
      if (!b) { say('Screenshot failed', 2500); return; }
      download(b, 'katam-' + stamp() + '.png');
      say('Screenshot saved', 1500);
    }, 'image/png');
  }

  function clipType() {
    if (!window.MediaRecorder) return null;
    var k = ['video/webm;codecs=vp9,opus', 'video/webm;codecs=vp8,opus', 'video/webm',
             'video/mp4;codecs=avc1,mp4a', 'video/mp4'];
    for (var i = 0; i < k.length; i++)
      if (!MediaRecorder.isTypeSupported || MediaRecorder.isTypeSupported(k[i])) return k[i];
    return null;
  }

  function clipStart() {
    var s = source(), type = clipType();
    if (!s) { say('No picture to record yet', 2000); return; }
    var cv = document.createElement('canvas');
    if (!type || !cv.captureStream) { say('This browser cannot record clips', 3000); return; }
    var k = Math.min(1, CLIP_MAX_SIDE / Math.max(s.w, s.h));
    cv.width = Math.max(2, Math.round(s.w * k) & ~1);
    cv.height = Math.max(2, Math.round(s.h * k) & ~1);
    var ctx = cv.getContext('2d', { alpha: false });
    var stream = cv.captureStream(30), dest = null, pa = M().portAudio;
    // The game's sound too, from the volume stage (audio_out_web.c), if it
    // is playing: a track from a suspended context stalls the recorder.
    try {
      if (pa && pa.ctx && pa.out && pa.ctx.state === 'running' && pa.ctx.createMediaStreamDestination) {
        dest = pa.ctx.createMediaStreamDestination();
        pa.out.connect(dest);
        dest.stream.getAudioTracks().forEach(function (t) { stream.addTrack(t); });
      }
    } catch (e) { dest = null; }
    var mr;
    try { mr = new MediaRecorder(stream, { mimeType: type, videoBitsPerSecond: 4000000 }); }
    catch (e) { say('Recording refused: ' + (e && e.message || e), 3000); return; }
    var r = { mr: mr, canvas: cv, ctx: ctx, chunks: [], t0: performance.now(), dest: dest, type: type };
    mr.ondataavailable = function (e) { if (e.data && e.data.size) r.chunks.push(e.data); };
    mr.onstop = function () {
      if (r.dest && pa && pa.out) { try { pa.out.disconnect(r.dest); } catch (e) {} }
      stream.getTracks().forEach(function (t) { t.stop(); });
      var blob = new Blob(r.chunks, { type: type.split(';')[0] });
      if (blob.size) {
        download(blob, 'katam-' + stamp() + (type.indexOf('mp4') >= 0 ? '.mp4' : '.webm'));
        say('Clip saved', 1500);
      } else say('Clip was empty', 2500);
    };
    mr.start(1000);
    r.timer = setInterval(function () {
      var t = (performance.now() - r.t0) / 1000;
      if (t >= CLIP_MAX_S) clipStop();
      else say('REC ' + Math.floor(t) + 's  (F10 stops)', 0);
    }, 250);
    rec = r;
    say('REC', 0);
  }

  function clipStop() {
    var r = rec;
    if (!r) return;
    rec = null;
    clearInterval(r.timer);
    try { r.mr.stop(); } catch (e) {}
  }

  // Called after every frame the game presents (web/shell.html present()).
  function onFrame() {
    var now = performance.now();
    if (shotWant && (shotWant === 1 || !menuUp() || now - shotSince > MENU_WAIT_MS)) {
      shotWant = 0;
      grab();
    }
    if (clipWant && !rec && (clipWant === 1 || !menuUp() || now - clipSince > MENU_WAIT_MS)) {
      clipWant = 0;
      clipStart();
    }
    if (rec) {
      var s = source();
      if (s) rec.ctx.drawImage(s.c, s.x, s.y, s.w, s.h, 0, 0, rec.canvas.width, rec.canvas.height);
    }
  }

  // fromMenu: wait for the SETTINGS screen to go before taking it.
  function shot(fromMenu) {
    shotWant = fromMenu ? 2 : 1;
    shotSince = performance.now();
    if (fromMenu) say('Screenshot when the menu closes', 2500);
  }
  function clip(fromMenu) {
    if (rec || clipWant) { clipWant = 0; clipStop(); return; }
    clipWant = fromMenu ? 2 : 1;
    clipSince = performance.now();
    if (fromMenu) say('Recording starts when the menu closes', 2500);
  }

  window.addEventListener('keydown', function (e) {
    if (e.repeat || e.ctrlKey || e.altKey || e.metaKey) return;
    if (e.code !== 'F9' && e.code !== 'F10') return;
    if (!document.body.classList.contains('rom-ready')) return;
    e.preventDefault();
    e.stopPropagation();
    if (e.code === 'F9') shot(false); else clip(false);
  }, true);

  window.katamCapture = {
    frame: onFrame, shot: shot, clip: clip,
    recording: function () { return !!rec || !!clipWant; },
  };
})();
