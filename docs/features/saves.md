# Save management and the installable page

## What it does

- **Export / import a save** (existed before): a plain 64 KiB SRAM image, the
  `.sav` any emulator reads. SETTINGS > GAME > EXPORT SAVE / IMPORT SAVE, or
  drop a `.sav` on the page.
- **Save slots ("profiles")**: SETTINGS > GAME > SAVE SLOTS opens a dialog
  where you name a copy of the current save, then load, export or delete it
  later. Slots are per ROM and live in this browser's IndexedDB.
- **Automatic backups**: every import (a file, a slot, a backup) first
  stores the save it is about to replace. The last 5 per ROM are kept and
  are listed in the same dialog (Restore / Export / Delete). A blank
  cartridge isn't backed up, and an identical save isn't backed up twice. If
  the backup can't be written, the import is cancelled.
- **Remembered ROM** (existed before): the visitor's ROM is kept in their
  own browser (IndexedDB `katam.rom`) so a returning visit starts without a
  picker. SETTINGS > GAME > FORGET ROM erases it. The ROM never leaves
  the browser.
- **Installable app (PWA)**: `manifest.webmanifest`, icons drawn by
  `tools/gen_icons.py` (original art, not from the ROM) and a service
  worker (`sw.js`) for offline play once the game has loaded online.

Online play: loading a slot or backup, or saving to a slot, is refused while
in an online session (the session owns save memory). Exporting still works.

## Service worker rules (web/sw.js)

- It only touches same-origin GETs for files directly in its own directory
  with a page-asset extension (`.js .wasm .webmanifest .png .svg`), plus the
  page itself. ROMs (`.gba`, `.bin`, `rom`, anything outside the directory)
  pass through untouched, so they are never cached. Pages in
  subdirectories (`/editor/`, `/next/`) are left alone.
- The page is network-first, so a reload while online is always the newest
  build. `?v=<hash>` files are cache-first, because their URL is their
  content. Everything unversioned is network-first.
- The cache name includes a version that `tools/stamp_build.py` stamps into
  `sw.js` (a hash of the stamped page plus the worker). When a new worker
  activates it deletes older caches and tells open pages its version.
  `web/pwa.js` shows "A new version of the game is out. Reload / Later"
  on a tab whose `<meta name="katam-sw">` differs. Reload waits for the
  save to be written first.
- Unstamped pages (`make`, not `make dist`) do not register a worker.
  `?nosw` unregisters it (an escape hatch).

## Where the code is

| File | What |
|---|---|
| `web/saves.js` | slots, backups (`shell.saveBackup`), the dialog |
| `web/pwa.js` | worker registration, the update bar |
| `web/sw.js` | the service worker |
| `web/manifest.webmanifest`, `web/icon.svg`, `tools/gen_icons.py` | manifest and icons (PNGs generated at `make dist`) |
| `web/shell.html` | hooks: `shell.idb.list`, `shell.save.{current,key,load,...}`, backup-before-import in `applyImport`, the `save.slots` action, manifest links |
| `platform/menu_poc.c`, `platform/ui/ui_menu.cpp` | the SAVE SLOTS menu item |
| `Makefile` (dist), `tools/stamp_build.py` | copying, icons, version stamping |
| `tools/saves_test.mjs` | node test (smoke suite `saves`) |
