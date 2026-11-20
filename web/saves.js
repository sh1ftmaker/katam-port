/* Save slots and import backups.  docs/features/saves.md.
 *
 * The live save is the one record per ROM the save block in web/shell.html
 * keeps (key: the ROM's identity).  This file adds, in the same IndexedDB
 * store and so in this browser only:
 *
 *   slot/<rom>/<name>      a named copy ("profile") the player made
 *   backup/<rom>/<time>    the save an import (a file, a slot, a backup)
 *                          was about to replace -- the last MAX_BACKUPS kept
 *
 * Loading a slot or a backup goes through the same import path as a .sav
 * file, so it too is backed up first and refused during an online session.
 * The dialog opens from SETTINGS > GAME > SAVE SLOTS (platform/menu_poc.c,
 * the 'save.slots' action) and from the page's own Save data bar.
 */
(function () {
  'use strict';
  var shell = window.Module && Module._shell;
  if (!shell || !shell.save || !shell.idb || !shell.idb.list) return;

  var MAX_BACKUPS = 5;
  var MAX_NAME = 24;
  var STORE = shell.idb.saveStore;

  function reason(e) { return (e && e.message) || String(e || 'unknown error'); }
  function online() { return !!(Module._PortLnActive && Module._PortLnActive()); }

  // An all-0x00 or all-0xFF region is a cartridge nobody has saved on: not
  // worth a backup.
  function blank(b) {
    if (!b || !b.length) return true;
    var f = b[0];
    if (f !== 0 && f !== 0xff) return false;
    for (var i = 1; i < b.length; i++) if (b[i] !== f) return false;
    return true;
  }

  function same(a, b) {
    if (!a || !b || a.length !== b.length) return false;
    for (var i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
  }

  function cleanName(n) {
    return String(n || '').replace(/[\u0000-\u001f\/]/g, ' ').replace(/\s+/g, ' ')
      .trim().slice(0, MAX_NAME);
  }

  function pad(t) { var s = String(t); while (s.length < 15) s = '0' + s; return s; }

  function slotKey(rom, name) { return 'slot/' + rom + '/' + name; }
  function backupKey(rom, t)  { return 'backup/' + rom + '/' + pad(t); }

  function list(kind) {
    var rom = shell.save.key();
    if (!rom) return Promise.resolve([]);
    return shell.idb.list(STORE, kind + '/' + rom + '/').then(function (recs) {
      // Newest first; backups by key, whose time part is unique (lastT).
      return (recs || []).sort(function (a, b) {
        return (b.saved || 0) - (a.saved || 0) || (a.id < b.id ? 1 : a.id > b.id ? -1 : 0);
      });
    });
  }

  function record(id, bytes, extra) {
    var r = { id: id, bytes: new Uint8Array(bytes), size: bytes.length, saved: Date.now(),
              rom: shell.save.label() };
    for (var k in extra) r[k] = extra[k];
    return r;
  }

  // The save block's applyImport calls this with the bytes it is about to
  // overwrite.  Resolves once they are stored (or there was nothing worth
  // keeping); rejects to cancel the import.
  var lastT = 0;
  shell.saveBackup = function (old, incoming) {
    var rom = shell.save.key();
    if (!rom || blank(old)) return Promise.resolve();
    return list('backup').then(function (recs) {
      if (recs.length && same(recs[0].bytes, old)) return null;      // already kept
      var t = lastT = Math.max(Date.now(), lastT + 1);
      return shell.idb.put(STORE, backupKey(rom, t),
        record(backupKey(rom, t), old, { saved: t, reason: 'before ' + (incoming || 'an import') }))
        .then(function () {
          return Promise.all(recs.slice(MAX_BACKUPS - 1).map(function (r) {
            return shell.idb.del(STORE, r.id);
          }));
        });
    }).then(function () { refresh(); });
  };

  //--------------------------------------------------------------------
  // The API the dialog (and tools/saves_test.mjs) use
  //--------------------------------------------------------------------

  function saveToSlot(name) {
    name = cleanName(name);
    var rom = shell.save.key();
    if (!rom) return Promise.reject(new Error('load a ROM first'));
    if (!name) return Promise.reject(new Error('the slot needs a name'));
    if (online()) return Promise.reject(new Error('not during an online game'));
    var bytes = shell.save.current();
    if (!bytes || blank(bytes)) return Promise.reject(new Error('there is no save to keep yet'));
    return shell.idb.put(STORE, slotKey(rom, name), record(slotKey(rom, name), bytes, { name: name }));
  }

  function loadRecord(rec, what) {
    if (online()) { shell.note('Leave the online session first.', 'warn'); return; }
    shell.save.load(new Uint8Array(rec.bytes), what);
  }

  shell.saveSlots = {
    list: list,
    saveTo: saveToSlot,
    load: loadRecord,
    del: function (rec) { return shell.idb.del(STORE, rec.id); },
    open: openDialog,
    blank: blank,
    cleanName: cleanName
  };

  //--------------------------------------------------------------------
  // The dialog
  //--------------------------------------------------------------------

  var dlg = null, body = null;

  function css() {
    var st = document.createElement('style');
    st.textContent =
      '#saveslotsdlg{position:fixed;inset:0;z-index:70;display:flex;align-items:center;' +
      'justify-content:center;background:#000a;font:14px system-ui,sans-serif;color:#eee}' +
      '#saveslotsdlg[hidden]{display:none}' +
      '#saveslotsdlg .ssbox{background:#1b1d23;border:1px solid #444;border-radius:12px;' +
      'width:min(520px,calc(100% - 32px));max-height:calc(100% - 32px);overflow:auto;padding:14px 16px}' +
      '#saveslotsdlg h2{font-size:17px;margin:0 0 4px}#saveslotsdlg h3{font-size:14px;margin:14px 0 6px;color:#ffb3dc}' +
      '#saveslotsdlg p{margin:4px 0;color:#bbb;font-size:13px}' +
      '#saveslotsdlg .ssrow{display:flex;flex-wrap:wrap;gap:6px;align-items:center;padding:6px 0;border-top:1px solid #2c2f36}' +
      '#saveslotsdlg .ssname{flex:1 1 140px;min-width:0;overflow-wrap:anywhere}' +
      '#saveslotsdlg .sswhen{color:#999;font-size:12px}' +
      '#saveslotsdlg button,#saveslotsdlg input{font:inherit;border-radius:6px;border:1px solid #555;' +
      'background:#2a2d35;color:#fff;padding:5px 10px}' +
      '#saveslotsdlg input{flex:1 1 160px;min-width:0}#saveslotsdlg button{cursor:pointer}' +
      '#saveslotsdlg button:disabled{opacity:.5;cursor:default}' +
      '#saveslotsdlg .sshead{display:flex;justify-content:space-between;align-items:center;gap:8px}';
    document.head.appendChild(st);
  }

  function when(t) {
    try { return new Date(t).toLocaleString(); } catch (e) { return ''; }
  }

  function el(tag, text, attrs) {
    var e = document.createElement(tag);
    if (text != null) e.textContent = text;
    for (var k in attrs || {}) e.setAttribute(k, attrs[k]);
    return e;
  }

  function button(text, fn, disabled) {
    var b = el('button', text, { type: 'button' });
    b.disabled = !!disabled;
    b.addEventListener('click', fn);
    return b;
  }

  function filename(rec, kind) {
    var tag = kind === 'slot' ? cleanName(rec.name).replace(/[^A-Za-z0-9_-]+/g, '_')
                              : 'backup-' + new Date(rec.saved).toISOString().replace(/[:.]/g, '-');
    return shell.save.base() + '-' + tag + '.sav';
  }

  function afterLoad() {
    // The save block's own note says a reset is needed; offer it here.
    var row = el('div', null, { 'class': 'ssrow' });
    row.appendChild(el('span', 'Loaded. Restart the game to play from it?', { 'class': 'ssname' }));
    row.appendChild(button('Restart now', function () {
      var go = function () { location.reload(); };
      if (shell.saveSettle) shell.saveSettle(go); else go();
    }));
    body.insertBefore(row, body.firstChild);
  }

  function rows(kind, recs, box) {
    if (!recs.length) {
      box.appendChild(el('p', kind === 'slot' ? 'No slots yet.' : 'No backups yet — one is made ' +
        'automatically whenever an import replaces your save.'));
      return;
    }
    recs.forEach(function (rec) {
      var row = el('div', null, { 'class': 'ssrow' });
      var name = el('span', null, { 'class': 'ssname' });
      name.appendChild(document.createTextNode(kind === 'slot' ? rec.name : (rec.reason || 'backup')));
      name.appendChild(el('div', when(rec.saved), { 'class': 'sswhen' }));
      row.appendChild(name);
      row.appendChild(button(kind === 'slot' ? 'Load' : 'Restore', function () {
        if (!confirm('Replace the current save with "' + (rec.name || 'this backup') +
                     '"? (The current save is backed up first.)')) return;
        loadRecord(rec, kind === 'slot' ? 'slot "' + rec.name + '"' : 'a backup');
        setTimeout(function () { refresh(); afterLoad(); }, 300);
      }, online()));
      row.appendChild(button('Export', function () {
        shell.save.download(new Uint8Array(rec.bytes), filename(rec, kind));
      }));
      row.appendChild(button('Delete', function () {
        if (!confirm('Delete "' + (rec.name || 'this backup') + '" from this browser?')) return;
        shell.saveSlots.del(rec).then(refresh, function (e) { shell.note('could not delete: ' + reason(e), 'err'); });
      }));
      box.appendChild(row);
    });
  }

  function refresh() {
    if (!dlg || dlg.hidden) return;
    body.textContent = '';
    var rom = shell.save.key();
    if (!rom) {
      body.appendChild(el('p', 'Load a ROM first: slots belong to a particular game.'));
      return;
    }
    body.appendChild(el('p', 'Kept in this browser only, per ROM (' + shell.save.label() + '). ' +
      'Export anything you want to keep elsewhere.'));
    if (online()) body.appendChild(el('p', 'During an online game slots can be exported but not loaded or saved.'));

    var add = el('div', null, { 'class': 'ssrow' });
    var input = el('input', null, { type: 'text', maxlength: String(MAX_NAME),
                                     placeholder: 'Slot name, e.g. "100% run"', 'aria-label': 'Slot name' });
    var save = button('Save current to slot', function () {
      var n = cleanName(input.value);
      if (!n) { input.focus(); return; }
      list('slot').then(function (recs) {
        var exists = recs.some(function (r) { return r.name === n; });
        if (exists && !confirm('Overwrite the slot "' + n + '"?')) return;
        return saveToSlot(n).then(function () {
          shell.note('Saved to slot "' + n + '".', 'ok');
          refresh();
        });
      })['catch'](function (e) { shell.note('could not save the slot: ' + reason(e), 'err'); });
    }, online());
    input.addEventListener('keydown', function (e) { if (e.key === 'Enter') save.click(); });
    add.appendChild(input);
    add.appendChild(save);

    var slotsBox = el('div');
    var backBox = el('div');
    body.appendChild(el('h3', 'Slots'));
    body.appendChild(add);
    body.appendChild(slotsBox);
    body.appendChild(el('h3', 'Automatic backups (last ' + MAX_BACKUPS + ')'));
    body.appendChild(backBox);
    Promise.all([list('slot'), list('backup')]).then(function (r) {
      rows('slot', r[0], slotsBox);
      rows('backup', r[1], backBox);
    }, function (e) {
      slotsBox.appendChild(el('p', 'Browser storage is unavailable (' + reason(e) + ').'));
    });
  }

  function close() {
    if (dlg) dlg.hidden = true;
  }

  function openDialog() {
    if (!dlg) {
      css();
      dlg = el('div', null, { id: 'saveslotsdlg', role: 'dialog', 'aria-modal': 'true',
                              'aria-labelledby': 'saveslotstitle' });
      var box = el('div', null, { 'class': 'ssbox' });
      var head = el('div', null, { 'class': 'sshead' });
      head.appendChild(el('h2', 'Save slots & backups', { id: 'saveslotstitle' }));
      head.appendChild(button('Close', close));
      box.appendChild(head);
      body = el('div');
      box.appendChild(body);
      dlg.appendChild(box);
      dlg.addEventListener('click', function (e) { if (e.target === dlg) close(); });
      // The game's key handler is on the window: keys typed here are the
      // dialog's, not the game's (Escape closes).
      ['keydown', 'keyup'].forEach(function (t) {
        dlg.addEventListener(t, function (e) {
          if (t === 'keydown' && e.key === 'Escape') close();
          e.stopPropagation();
        });
      });
      document.body.appendChild(dlg);
    }
    dlg.hidden = false;
    refresh();
    var first = dlg.querySelector('input,button');
    if (first) first.focus();
  }

  var pageBtn = document.getElementById('saveslots');
  if (pageBtn) pageBtn.addEventListener('click', openDialog);
})();
