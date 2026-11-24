/* KATAM service worker: offline play for the installed page.
 *
 * docs/features/saves.md is the write-up.  The rules, in order of importance:
 *
 *  1. The ROM never goes near this cache.  Only GETs to this origin, for a
 *     file directly in this worker's own directory, with a page-asset
 *     extension (.js .wasm .webmanifest .png .svg), or the page itself, are
 *     ever stored.  A ROM picked from disk is never fetched at all; one
 *     loaded from a URL (?rom=, the test rig's /rom.gba) is a different path
 *     or extension and passes straight through.  The ROM a visitor chooses
 *     to keep lives in the page's IndexedDB, in their browser only.
 *  2. Never a stale build.  The page itself is network-first: online, every
 *     load gets the current index.html, which names its scripts and wasm with
 *     content-hash URLs (?v=..., tools/stamp_build.py), so a fresh page can
 *     never pick up an old payload.  Those ?v= files are cache-first (their
 *     URL *is* their content); anything unversioned is network-first.  The
 *     cache name carries this worker's VERSION (stamped by stamp_build.py),
 *     and activating a new worker deletes every older KATAM cache.
 *  3. Subdirectory pages (/editor/, /next/...) are not ours: a request
 *     whose path goes below this directory passes through untouched.
 */
'use strict';

var VERSION = 'd32f44aa99a5';
var PREFIX = 'katam-page-';
var CACHE = PREFIX + VERSION;

var BASE = new URL('./', self.location.href);           // this worker's directory
var ASSET = /\.(?:js|wasm|webmanifest|png|svg)$/i;
var PAGE = /^(?:|index\.html|katam\.html)$/;
// The heavy second module is fetched (and then cached) only by pages that use it.
var PRECACHE_SKIP = /^katam16\./;

// The file name relative to this directory, or null when the request is not
// one this worker may touch at all.
function ownName(url) {
  if (url.origin !== BASE.origin) return null;
  if (url.pathname.indexOf(BASE.pathname) !== 0) return null;
  var rest = url.pathname.slice(BASE.pathname.length);
  if (rest.indexOf('/') >= 0) return null;                 // a subdirectory's page
  if (rest === 'sw.js') return null;
  if (PAGE.test(rest) || ASSET.test(rest)) return rest;
  return null;
}

function cacheable(resp) {
  return resp && resp.ok && resp.status === 200 && resp.type === 'basic';
}

// One copy per file: storing katam.js?v=new drops katam.js?v=old.
function putVersioned(cache, req, resp) {
  var url = new URL(req.url);
  return cache.keys().then(function (keys) {
    return Promise.all(keys.filter(function (k) {
      var u = new URL(k.url);
      return u.pathname === url.pathname && u.search !== url.search;
    }).map(function (k) { return cache['delete'](k); }));
  }).then(function () { return cache.put(req, resp); });
}

// The versioned files a page names: <script src="x.js?v=id"> and the
// wasm shim's  p==="katam.wasm"?p+"?v=id".
function namedAssets(html) {
  var out = [], m;
  var re = /([A-Za-z0-9_.-]+\.js)\?v=([0-9a-f]{6,})/g;
  while ((m = re.exec(html))) out.push(m[1] + '?v=' + m[2]);
  var wre = /"([A-Za-z0-9_.-]+\.wasm)"\?p\+"\?v=([0-9a-f]{6,})"/g;
  while ((m = wre.exec(html))) out.push(m[1] + '?v=' + m[2]);
  return out.filter(function (n, i) {
    return out.indexOf(n) === i && !PRECACHE_SKIP.test(n);
  });
}

self.addEventListener('install', function (event) {
  event.waitUntil(caches.open(CACHE).then(function (cache) {
    var pageUrl = new URL('./', BASE).href;
    return fetch(pageUrl, { cache: 'reload' }).then(function (resp) {
      if (!cacheable(resp)) return;
      var copy = resp.clone();
      return resp.text().then(function (html) {
        var names = namedAssets(html).concat(['manifest.webmanifest', 'icon-192.png', 'icon-512.png']);
        return cache.put(pageUrl, copy).then(function () {
          // Each on its own: one missing icon must not stop the install.
          return Promise.all(names.map(function (n) {
            var u = new URL(n, BASE).href;
            return fetch(u, { cache: 'reload' }).then(function (r) {
              if (cacheable(r)) return cache.put(u, r);
            })['catch'](function () {});
          }));
        });
      });
    })['catch'](function () { /* offline install: runtime caching fills it */ });
  }).then(function () {
    // Activating at once is safe: the page is network-first and every
    // payload URL is content-addressed, so a page that is already open keeps
    // working, and the page decides whether to offer a reload (web/pwa.js).
    return self.skipWaiting();
  }));
});

self.addEventListener('activate', function (event) {
  event.waitUntil(caches.keys().then(function (names) {
    return Promise.all(names.filter(function (n) {
      return n.indexOf(PREFIX) === 0 && n !== CACHE;
    }).map(function (n) { return caches['delete'](n); }));
  }).then(function () { return self.clients.claim(); }).then(function () {
    return self.clients.matchAll({ type: 'window' });
  }).then(function (list) {
    list.forEach(function (c) { c.postMessage({ type: 'katam-sw', version: VERSION }); });
  }));
});

self.addEventListener('message', function (event) {
  if (event.data && event.data.type === 'katam-sw-version' && event.source) {
    event.source.postMessage({ type: 'katam-sw', version: VERSION });
  }
});

self.addEventListener('fetch', function (event) {
  var req = event.request;
  if (req.method !== 'GET') return;
  var url = new URL(req.url);
  var name = ownName(url);
  if (name === null) return;                               // not ours: the network, untouched

  if (PAGE.test(name)) {
    if (req.mode !== 'navigate' && name === '') return;
    // The page: always the network's when there is one.  Stored under the
    // directory URL whatever the query (?dev=1, ?room=...): the page reads
    // its own query string.
    var key = new URL('./', BASE).href;
    event.respondWith(fetch(req).then(function (resp) {
      if (cacheable(resp)) {
        var copy = resp.clone();
        caches.open(CACHE).then(function (c) { return c.put(key, copy); });
      }
      return resp;
    })['catch'](function () {
      return caches.open(CACHE).then(function (c) { return c.match(key); }).then(function (hit) {
        return hit || Response.error();
      });
    }));
    return;
  }

  if (url.searchParams.has('v')) {
    // Content-addressed: the cached copy is the right one by definition.
    event.respondWith(caches.open(CACHE).then(function (c) {
      return c.match(req).then(function (hit) {
        if (hit) return hit;
        return fetch(req).then(function (resp) {
          if (cacheable(resp)) putVersioned(c, req, resp.clone());
          return resp;
        });
      });
    }));
    return;
  }

  // Unversioned (a dev build, the manifest, the icons): network first.
  event.respondWith(fetch(req).then(function (resp) {
    if (cacheable(resp)) {
      var copy = resp.clone();
      caches.open(CACHE).then(function (c) { return c.put(req, copy); });
    }
    return resp;
  })['catch'](function () {
    return caches.open(CACHE).then(function (c) { return c.match(req); }).then(function (hit) {
      return hit || Response.error();
    });
  }));
});

if (typeof module !== 'undefined') module.exports = { ownName: ownName, namedAssets: namedAssets };
