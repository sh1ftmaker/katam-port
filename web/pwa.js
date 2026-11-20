/* The installable page: registers web/sw.js and offers a reload when a newer
 * build has been published.  docs/features/saves.md.
 *
 * The worker is network-first for the page, so an ordinary reload is always
 * the current build; this file's only job is the tab (or installed app) that
 * stays open across a publish.  A newly activated worker tells every open
 * page its version (sw.js, activate); a page stamped with a different one
 * (<meta name="katam-sw">, tools/stamp_build.py) is older than the publish
 * and shows a small bar: Reload / Later.  Only activations are listened to,
 * never the worker that was already running at load: a page fetched fresh
 * under last week's worker is the newer of the two, not the stale one.
 *
 * Off on a dev build (no stamp), on file:, and with ?nosw (which also
 * unregisters a worker already installed for this directory).
 */
(function () {
  'use strict';
  var sw = navigator.serviceWorker;
  if (!sw || !window.isSecureContext) return;

  var meta = document.querySelector('meta[name="katam-sw"]');
  var mine = meta ? meta.getAttribute('content') : '';
  var nosw = /[?&]nosw(?:[=&]|$)/.test(location.search);

  if (nosw || !mine) {
    // ?nosw is the escape hatch; an unstamped page (make, not make dist)
    // does not install one either, and drops one a published page left.
    sw.getRegistration('./').then(function (r) {
      if (r && new URL(r.scope).pathname === new URL('./', location.href).pathname) r.unregister();
    })['catch'](function () {});
    return;
  }

  var bar = null, dismissed = '';

  function settleThenReload() {
    var shell = window.Module && Module._shell;
    var go = function () { location.reload(); };
    if (shell && shell.saveSettle) { try { shell.saveSettle(go); return; } catch (e) { /* below */ } }
    go();
  }

  function offer(version) {
    if (!version || version === mine || version === dismissed || bar) return;
    bar = document.createElement('div');
    bar.id = 'pwaupdate';
    bar.setAttribute('role', 'status');
    bar.style.cssText = 'position:fixed;left:50%;bottom:max(12px,env(safe-area-inset-bottom));' +
      'transform:translateX(-50%);z-index:60;display:flex;gap:10px;align-items:center;' +
      'background:#22252c;color:#eee;border:1px solid #444;border-radius:10px;' +
      'padding:8px 12px;font:14px system-ui,sans-serif;box-shadow:0 4px 18px #0008;max-width:calc(100% - 32px)';
    var text = document.createElement('span');
    text.textContent = 'A new version of the game is out.';
    var go = document.createElement('button');
    go.type = 'button';
    go.textContent = 'Reload';
    var later = document.createElement('button');
    later.type = 'button';
    later.textContent = 'Later';
    go.onclick = function () { go.disabled = true; settleThenReload(); };
    later.onclick = function () { dismissed = version; bar.remove(); bar = null; };
    [go, later].forEach(function (b) {
      b.style.cssText = 'font:inherit;padding:4px 10px;border-radius:6px;border:1px solid #666;' +
        'background:#333;color:#fff;cursor:pointer';
    });
    bar.appendChild(text);
    bar.appendChild(go);
    bar.appendChild(later);
    document.body.appendChild(bar);
  }

  sw.addEventListener('message', function (e) {
    if (e.data && e.data.type === 'katam-sw') offer(e.data.version);
  });
  if (sw.startMessages) sw.startMessages();

  window.addEventListener('load', function () {
    sw.register('sw.js', { scope: './' }).then(function (reg) {
      // An open tab checks for a publish now and then, and whenever it is
      // brought back to the front.
      setInterval(function () { reg.update()['catch'](function () {}); }, 30 * 60 * 1000);
      document.addEventListener('visibilitychange', function () {
        if (!document.hidden) reg.update()['catch'](function () {});
      });
    })['catch'](function (e) {
      if (window.Module && Module._shell && Module._shell.log) {
        Module._shell.log('offline support is unavailable (' + ((e && e.message) || e) + ')');
      }
    });
  });
})();
