"""report.py -- AUDIT-REPORT.md and audit-report.json for tools/netaudit/matrix.py.

The measured columns (SAME/DIFFERS, first frame, ranges) come from the run;
the class and the proposed fix for each factor are the audit's reading of
those measurements, kept here next to the factor id so a rerun that changes
a verdict shows up against the text that explains it."""
import json
import time

# Class letters (the task's): a presentation/device state stored in sim
# memory; b the sim genuinely depends on the device -> session-pinned input;
# c state outside the image a session start must carry; d harmless but breaks
# a whole-image hash.
KB = {
    'baseline-tour': ('-', 'Control. Two processes, same build, same device: identical raw image on every frame.', ''),
    'baseline-menus': ('-', 'Control for the menus recording (menus on).', ''),
    'baseline-native': ('-', 'Control for the native factors (setarch -R).', ''),
    'audio-on-off': ('a', 'Only gSoundInfo (IWRAM 0x03000560: pcmSamplesPerVBlank/pcmFreq/divFreq from the device rate, '
                     'then the SoundChannel mixing state) and a few gMPlayTrack_0 bytes (channel allocation follows '
                     'channels the mixer ended). No gameplay symbol differs at the last frame.',
                     'audio.c:252 PortSampleRateSet: write fixed values (the GBA table rate) into soundInfo whatever the '
                     'device; drive the mixer from a fixed per-frame block (m4a_mixer.c PortMixerSetBlock with sNominal of a '
                     'fixed session rate, e.g. 48000/59.7275) and resample to the device after the image boundary.'),
    'audio-rate': ('a', '44.1 vs 48 kHz: gSoundInfo header from frame 0, channel phase (fw/ct) after.', 'as audio-on-off'),
    'audio-block': ('a', 'Same rate, different device callback size and jitter: ChooseBlockSize (audio.c:147) '
                    'steers the block per frame from the JS queue depth, so SoundChannel state in gSoundInfo advances '
                    'by a device-dependent amount from frame ~57.',
                    'audio.c:147 ChooseBlockSize: in a session, keep sBlock = the fixed nominal; absorb clock drift in '
                    'the output resampler or ring (outside the image).'),
    'audio-live': ('-', 'At a steady 60 Hz the queue never starves, so AudioTopUp never fires: same as not live.', ''),
    'audio-live-30hz': ('b', 'A running AudioContext at 30 Hz pacing starves the queue, so AudioTopUp '
                        '(host_web.c:422, called at :447) runs extra m4aSoundMain+m4aSoundVSync between frames. '
                        'SoundMain ends the VBlank window (m4a_mixer.c:1184 PortVBlankEnd), so the game\'s VBlank DMA '
                        'queue (gUnk_03002EC0) drains differently. VRAM, IO, OAM, palette and the task heaps follow, '
                        'from frame 2.',
                        'host_web.c:447: never call AudioTopUp while a shared session runs (today the guard is only '
                        'PortRbActive/PortNetActive; the local-MP-online design uses neither). Better: delete it and let '
                        'the output side repeat or stretch audio on starvation.'),
    'pace-30hz': ('-', 'Pacing alone (two frames per vsync) changes nothing.', ''),
    'pace-burst': ('-', 'Catch-up bursts alone change nothing.', ''),
    'render-off-all': ('-', 'PortSetRenderEnabled(0) skips composition only: identical image (the scanline loop, '
                       'HBlank DMA and the affine latches still run).', ''),
    'render-off-burst': ('-', 'Replayed-style frames with rendering off: identical.', ''),
    'pace-burst-audio': ('a', 'Bursts with audio on (not live): only the mixer block size and so gSoundInfo channel state '
                         'move (queue depth seen differently). Same mechanism as audio-block.', 'as audio-block'),
    'clock': ('-', 'performance.now/Date.now offset and rate: no effect.', ''),
    'random': ('-', 'Math.random: no effect.', ''),
    'malloc-shift': ('-', 'Shifted malloc addresses: no host pointer reaches the raw image (wasm, this recording).', ''),
    'heap-junk': ('-', 'Junk-filled free heap: no uninitialised read reaches the image.', ''),
    'menus-switch': ('b', 'PortMenuPoc() (menu_poc.c:166: KATAM_MENU_POC, else any `location` without ?menus=0) '
                     'changes Game Select, the Start Game pop-up and the pause cycle: a different game from frame 288. '
                     'Not recorded in recordings either.',
                     'Make it a session constant: the host sends it in the session start, and inputlog writes '
                     '`option menus N` (inputlog.c:349 next to option localmp) so replays carry it.'),
    'menus-param0': ('-', '?menus=0 and no location are the same (both off).', ''),
    'p2-input': ('b', 'PortLmpP2InputAvailable (localmp.c:130, sP2Input, set by the page from touch/gamepad detection) '
                 'dims LOCAL and swaps its description text (menu_poc.c:952-959): the touch-only device refuses the '
                 'LOCAL start, so the game runs one-player from frame ~1112.',
                 'menu_poc.c:959 MpUnavailable / :952 MpDesc: in a session, never read a device flag inside the sim; '
                 'the online start screen should be decided by session state (who is seated), not by this device\'s keys.'),
    'p2-input-tour': ('-', 'Without the menus the flag is never read.', ''),
    'settings-menus': ('a', 'Stored settings (renderer, zoom, aspect, scaling, volume, camera) are drawn into OBJ VRAM '
                       'tiles while a settings screen is up (menu_poc.c:865 RowValue -> :1014 DrawRowTiles). Gone again '
                       'once the screen closes: identical at the end. The in-game change itself (RowStep, '
                       'menu_poc.c:884-912) runs on every device and writes every device\'s localStorage.',
                       'menu_poc.c:865 RowValue: in a session, draw session values (or "--") instead of gPortSettings; '
                       'and gate RowStep\'s PortSettingsChanged (:912) to the device whose player pressed the button.'),
    'settings-tour': ('-', 'Settings never reach the image without the menus.', ''),
    'sram': ('c', 'A different stored save is loaded into SRAM at the first ReadSram and decides the file the game '
             'loads (gVisitedDoors etc. from frame 406): everything differs.',
             'Session start must carry SRAM (64 KiB) and the guest must not restore its own (sram.c SramRestoreOnce / '
             'sram_web.c PortSramLoad): give the session a save of its own; also stop guests writing the session save '
             'over theirs (PortSramMarkDirty).'),
    'view-follow': ('b', 'The sim view (spawn/despawn windows) follows each display: a phone at 21:9 zoomed out asks '
                    'for 746x320, a desktop for 480x320 (present.c:367-373). Objects spawn differently from frame ~408.',
                    'Session-pinned: PortSimViewUseSession() + the session\'s view events, as measured by view-pin-session.'),
    'view-pin-native': ('b', 'PortSimViewPinNative() holds 240x160 and ALSO makes PortSimViewLocked() true, which '
                        'localmp.c:139 SessionActive() treats as a netplay session: "localmp: refused", the game '
                        'starts with ONE human. Pinning this way is not sufficient; it breaks local MP.',
                        'localmp.c:139: drop PortSimViewLocked() from SessionActive (or pin through '
                        'PortSimViewUseSession, which does not lock).'),
    'view-pin-session': ('-', 'PortSimViewUseSession() with the session\'s view events: identical. This is the pin that works.', ''),
    'view-mode': ('b', 'The page\'s widescreen experiment (shell.html:2014 -> PortSetViewMode) sets gPortCamPadX/Y '
                  '(view.c:474) which the game\'s camera clamp reads (code_080023A4.c:1735-1746): gKirbys and the '
                  'camera differ from frame 1502.',
                  'view.c:474: zero gPortCamPadX/Y while a session runs (or derive them from the session view); hide '
                  'the view panel online.'),
    'join-tour-image': ('c', 'A device that receives only the image mid-play has sSetting = 0 (localmp.c:89), so '
                        'inputlog.c:704 never latches player 2\'s pad (PortLmpSetting() >= 2): player 2 freezes, '
                        'gKirbys differ from frame +21.',
                        'inputlog.c:704: latch when gPortSim->lmp.humans (in the image) is set, not on the host '
                        'setting; move sSetting into the sim block (lmp.flags) so PortLmpChoose/StartHumans use image state.'),
    'join-tour-image-lmp': ('c', 'Image plus the local-MP setting: identical (SRAM still differs, it is outside the image, '
                            'but nothing reads it here).', 'carry the setting (see join-tour-image)'),
    'join-tour-full': ('-', 'Image + setting + SRAM + DMA table + PPU latches: identical.', ''),
    'join-menus-image': ('c', 'Joining while the pause SETTINGS page is up: menu_poc.c host statics are missing '
                         '(sObjSave :1256, sOwner :146, sModel :965, sLblOff* :149, sDescShown :966). The page redraws '
                         'differently and, when it closes, PortPocPauseObjRestore has nothing to put back: sprite tiles '
                         'stay wrong to the end of the run.', 'see join-menus-full'),
    'join-menus-full': ('c', 'Even with SRAM, DMA, PPU and the setting carried, the menu_poc statics are missing: VRAM '
                        'OBJ tiles differ to the end (8.7 KB at the last frame), plus a few frames of EWRAM/IWRAM/IO.',
                        'menu_poc.c:1256 sObjSave: keep the saved OBJ tiles in the sim block (it has room) or in a fixed '
                        'GBA buffer; make sModel/sOwner/sDescShown/the decoded offsets derivable from game state (the '
                        'menu struct) instead of host statics; or refuse joins while a menu_poc screen is up.'),
    'native-gpu': ('d', 'Native only: the GPU renderer moves the C stack, and dma.c:311-312 mirrors the host source '
                   'address of a DmaFill (a stack local) into DMA3SAD (IO 0x0D4) and the DMA table, frames 0-390. '
                   'Nothing reads it; the raw IO hash sees it. The same can happen in wasm whenever the stack depth '
                   'of a DmaFill differs.',
                   'dma.c:311-312: mirror the GBA address (or 0 for a non-GBA source) instead of the host pointer.'),
    'native-window': ('-', 'Under replay the window size never reaches the sim (view requests are dropped while '
                      'replaying); live it does -- see view-follow.', ''),
    'native-ini': ('-', 'Zoom/aspect/smooth camera/pixel filter/volume/HUD from settings.ini: identical under replay.', ''),
    'native-audio': ('a', 'Native SDL audio device: same gSoundInfo effect as audio-on-off (plus the DMA stack mirror).',
                     'as audio-on-off'),
}

STATIC = [
    # (class, file:line, what, reaches)
    ('b', 'platform/web/host_web.c:422-447', 'AudioTopUp runs m4aSoundMain/m4aSoundVSync extra times when the AudioContext runs and the queue starves; SoundMain calls PortVBlankEnd (m4a_mixer.c:1184)', 'image + gameplay timing of VBlank DMA (measured: audio-live-30hz)'),
    ('a', 'platform/audio.c:147 ChooseBlockSize, :300', 'mixer block from PortAudioQueuedFrames/PortAudioUnderruns (JS queue depth)', 'gSoundInfo channel state (measured: audio-block)'),
    ('a', 'platform/audio.c:247-262 PortSampleRateSet', 'pcmSamplesPerVBlank/pcmFreq/divFreq from the device rate (or 1/1/0/1 with none)', 'gSoundInfo header (measured: audio-on-off, audio-rate)'),
    ('b', 'platform/menu_poc.c:166 PortMenuPoc', 'KATAM_MENU_POC / location.search ?menus', 'game flow (measured: menus-switch); not recorded'),
    ('b', 'platform/localmp.c:130, menu_poc.c:952-959', 'PortLmpP2InputAvailable (touch/gamepad detection) gates LOCAL and its text', 'game flow (measured: p2-input)'),
    ('b', 'platform/ui/present.c:364-373 -> view.c:660', 'sim view requested from window size, DPR, aspect, zoom, renderer, PortLmpMinZoom (KATAM_LOCAL_MP_MINZOOM)', 'spawn windows (measured: view-follow); fine under PortSimViewUseSession'),
    ('b', 'platform/localmp.c:139 SessionActive', 'PortSimViewLocked() counts as a session: pinning the view natively refuses local MP', 'humans = 1 (measured: view-pin-native)'),
    ('b', 'platform/view.c:474-475, 495', 'gPortCamPadX/Y and sLegacyFloor*/sLegacyNoCull from PortSetViewMode (page view panel)', 'camera clamp code_080023A4.c:1735-1746 (measured: view-mode)'),
    ('b', 'platform/rando.c:230-231', 'gPortRandoFlags/Seed from localStorage on the page (shell.html:2111) or option rando', 'object lists (not measured; recorded as option rando)'),
    ('b', 'game/src/pause_settings.c:194', 'the pause SETTINGS page reads gPauseMenus[gLocalPlayerId] keys: only the device-local player 1 can change a value (player 2 can open it and quit, not change). Online, gLocalPlayerId must be the same constant on every device', 'menu control flow (observed in localmp_menus: player 2\'s Down/Right do nothing)'),
    ('b', 'platform/netsim.c:417, rollback.c:996/1605, seats.c', 'GAME_REPIN_FOCUS writes the device\'s own seat into gLocalPlayerId every frame (existing netplay paths)', 'image + ~500 game branches; must not be used by the local-MP-online design'),
    ('b', 'platform/sram.c SramRestoreOnce -> sram_web.c:19 PortSramLoad', 'the device\'s stored save copied into SRAM at first access', 'which file the game loads (measured: sram)'),
    ('b', 'platform/res*.c, host_web.c:169-170', 'resource packs / mods / room bundles (Res_SetRoomBundleMemory, PortResReloadRooms) can differ per device and be reloaded mid-session', 'game data (not measured)'),
    ('b', 'platform/playtest.c:32', 'PortPlaytestBoot overrides keys in UpdateKeyInput', 'inputs (not measured; page-only)'),
    ('c', 'platform/localmp.c:89 sSetting (PortLmpChoose :111)', 'the menu\'s LOCAL choice kept in host .bss; read by PortLmpStartHumans and inputlog.c:704', 'player 2\'s pad never latched after a join (measured: join-tour-image)'),
    ('c', 'platform/menu_poc.c:146, 149, 718, 965-966, 1256', 'sOwner, sLbl/HdrOff*, sGsDesc*, sModel, sDescShown, sObjSave: menu screen state in host statics', 'VRAM/OAM/cursor (measured: join-menus-full)'),
    ('c', 'platform/view.c:608-609, inputlog.c:45 sViews', 'sim-view pin flags and the view event list', 'sim.view recomputed each latch from the list: a joiner needs the session\'s events'),
    ('c', 'platform/dma.c:52 sChannels', 'armed/repeating DMA channels (host table); netsim/rollback save it outside the map', 'measured harmless at the join points tested (join-tour-image-lmp SAME without it)'),
    ('c', 'platform/audio.c, m4a_mixer.c statics', 'sRate/sBlock/sTarget/sQueueAvg, mixer ring', 'how far gSoundInfo advances (class a above)'),
    ('d', 'platform/dma.c:311-312 PortDmaSet', 'host source/destination address mirrored into IO DMAxSAD/DAD (a stack address for DmaFill)', 'raw IO hash only (measured: native-gpu)'),
    ('d', 'platform/ppu.c:1029-1030', 'affine latches advanced past line 160 by the view height', 'host PPU latches only (dead at the frame top)'),
    ('out', 'platform/menu_poc.c:884-912 RowStep -> present.c:196 PortSettingsChanged', 'the in-game SETTINGS screens write gPortSettings, apply volume/renderer and save localStorage on EVERY device (measured: storage writes at frames 571, 601, 1751 on each device)', 'device settings, not the image'),
    ('out', 'platform/sram.c WriteSram -> PortSramMarkDirty', 'every device writes the session\'s save over its own stored save (PortSimSaveOwner: every peer owns it in a session)', 'device storage'),
    ('ok', 'time', 'PortHostMicros/performance.now/emscripten_get_now: pacing and netsim stats only (measured: clock)', ''),
    ('ok', 'random', 'no Math.random/rand in the sim (measured: random)', ''),
    ('ok', 'render', 'PortSetRenderEnabled/PORT_NO_RENDER, gPortRlEnabled/gPortPpuCompose, rl_build.c (reads only), gPortOamMin/Max (render list only) (measured: render-off-*, native-gpu)', ''),
    ('ok', 'host pointers', 'code IDs everywhere; no malloc address reaches the image (measured: malloc-shift, heap-junk)', ''),
    ('ok', 'env diagnostics', 'PORT_* and KATAM_* trace/stat switches (DMA, state, spawnstat, camlog, RL debug, localmp trace/doors) default off and only log', ''),
]

CARRY = [
    ('SRAM (64 KiB)', 'the save the session plays; guests must not load their own (measured: sram)'),
    ('the local-MP setting (sSetting / humans chosen)', 'until it lives in the sim block (measured: join-tour-image)'),
    ('the menus switch (PortMenuPoc)', 'a session constant, also for replays (measured: menus-switch)'),
    ('the session view event list', 'PortSimViewUseSession + every `view` event so far; never the device\'s display (measured: view-follow, view-pin-session)'),
    ('the rando seed/flags (option rando)', 'host statics read by the sim (rando.c:230)'),
    ('resource pack / mods / room bundles', 'identical on every device, hashed into the session id'),
    ('menu_poc screen state (sObjSave, sModel, sOwner, ...)', 'or refuse a join while a menu screen is up (measured: join-menus-full)'),
    ('DMA channel table + PPU latches', 'not needed at the join points measured, but saved by rollback; carry them as rollback does'),
    ('gLocalPlayerId = 0 on every device', 'the shared camera is player 1\'s; never repin focus per device'),
    ('audio: nothing, IF the mixer is made device-independent', 'else every device needs the same rate, block schedule and no AudioTopUp'),
]


def fmt_first(c, col):
    v = c.get(col, {})
    return '-' if not v or v.get('first') is None else '%d (%d fr)' % (v['first'], v['frames'])


def write(ev, F, meta, secs, root):
    rows = []
    for x in F:
        cls, expl, fix = KB.get(x['id'], ('?', '', ''))
        if x['verdict'] == 'SAME' and cls not in ('-',):
            expl = '(verdict changed since the text was written) ' + expl
        d = x.get('diffs', {})
        first = x['cmp']['image']['first']
        fd = d.get(first) or d.get(str(first)) or {}
        last = max((int(k) for k in d), default=None)
        ld = d.get(last) or d.get(str(last)) or {}
        rows.append({
            'id': x['id'], 'group': x['group'], 'title': x['title'], 'verdict': x['verdict'],
            'first_frame': first, 'from': x.get('from'),
            'columns': {k: v for k, v in x['cmp'].items() if isinstance(v, dict)},
            'frames_compared': x['cmp']['frames'],
            'first_ranges': fd.get('ranges', [])[:12], 'first_symbols': fd.get('by_symbol', []),
            'last_frame': last, 'last_symbols': ld.get('by_symbol', []), 'last_gameplay': ld.get('gameplay', []),
            'class': cls, 'explanation': expl, 'fix': fix,
            'runs': [x['base_run'], x['var_run']], 'rc': x.get('rc'),
        })
    out = {'generated': time.strftime('%Y-%m-%d %H:%M:%S'), 'seconds': round(secs),
           'factors': rows, 'static': [dict(zip(('class', 'where', 'what', 'reaches'), s)) for s in STATIC],
           'carry': [dict(zip(('what', 'why'), c)) for c in CARRY],
           'live_view_probe': meta.get('live_view_probe'),
           'device_meta': {k: {kk: v.get(kk) for kk in ('audioStats', 'sramWrites', 'storageWrites')}
                           for k, v in meta.items() if isinstance(v, dict) and 'audioStats' in v}}
    (ev / 'audit-report.json').write_text(json.dumps(out, indent=1, default=str))

    L = []
    L.append('# Online multiplayer determinism audit (local-MP-over-network design)\n')
    L.append('Generated %s by `python3 tools/netaudit/matrix.py` in %d s. Each row: one device-side factor '
             'flipped against a baseline device, same wasm build (node) unless the id says native, same '
             'recording. Compared: the RAW whole image at the top of every frame (EWRAM, IWRAM, IO, palette, '
             'VRAM, OAM, sim block; no masks, no canonicalisation). SRAM, IO with the DMA address mirrors zeroed, '
             'and the host DMA/PPU state are extra columns, not part of the image.\n' % (out['generated'], secs))
    L.append('Recordings: `tests/recordings/localmp_tour.txt` (menus off) and `tests/recordings/localmp_menus.txt` '
             '(menus on). Classes: **a** presentation/device state stored in sim memory; **b** the sim depends on '
             'the device, must become a session input; **c** state outside the image a session start must carry; '
             '**d** harmless but breaks a whole-image hash.\n')
    L.append('## Matrix\n')
    L.append('| factor | verdict | first frame (frames differing) | differing at first frame | at the last frame | gameplay reached | class |')
    L.append('|---|---|---|---|---|---|---|')
    for r in rows:
        fs = ', '.join('%s %d' % (s, n) for s, n in r['first_symbols'][:4]) or '-'
        ls = ', '.join('%s %d' % (s, n) for s, n in r['last_symbols'][:4]) or ('-' if r['verdict'] != 'SAME' else '')
        gp = ', '.join(r['last_gameplay']) or ('no' if r['verdict'] == 'DIFFERS' else '')
        ff = '-' if r['first_frame'] is None else '%d (%d)' % (r['first_frame'], r['columns']['image']['frames'])
        if r['from']:
            ff += ' [from %d]' % r['from']
        L.append('| `%s` %s | **%s** | %s | %s | %s | %s | %s |' % (r['id'], r['title'], r['verdict'], ff, fs, ls, gp, r['class']))
    L.append('\nPer-column first differing frames are in audit-report.json (`columns`).\n')
    L.append('## Findings and proposed fixes\n')
    for r in rows:
        if r['verdict'] == 'SAME' and r['class'] == '-':
            continue
        L.append('### `%s` -- %s (%s, class %s)\n' % (r['id'], r['verdict'], r['title'], r['class']))
        L.append(r['explanation'] + '\n')
        if r['first_ranges']:
            L.append('First differing frame %s; largest ranges:\n' % r['first_frame'])
            for g in r['first_ranges'][:6]:
                L.append('- `%s` %s %s: %d bytes (`%s` vs `%s`)' % (g['part'], g['addr'] or '', g['name'], g['bytes'], g['a'], g['b']))
            L.append('')
        if r['fix']:
            L.append('**Fix:** ' + r['fix'] + '\n')
    L.append('## Static audit: where simulation code reads the device or writes outside the image\n')
    L.append('| class | where | what | reaches |')
    L.append('|---|---|---|---|')
    for c, w, what, reach in STATIC:
        L.append('| %s | `%s` | %s | %s |' % (c, w, what, reach))
    L.append('\n## What a session start must carry besides the image\n')
    for w, why in CARRY:
        L.append('- **%s** -- %s' % (w, why))
    pr = meta.get('live_view_probe') or {}
    if pr:
        L.append('\n## Live probe: the display becomes a simulation input\n')
        L.append('Native, GPU renderer, `--gpu-view auto`, 200 frames recorded live (not replayed): the `view` '
                 'events each window produced -- ' + '; '.join('%s: %s' % (k, ', '.join(v) if v else 'none')
                                                         for k, v in pr.items())
                 + '. Under replay these requests are dropped, which is why `native-window` is SAME; '
                 '`view-follow` replays the difference such a request makes.\n')
    L.append('\n## Device telemetry\n')
    L.append('SRAM writes (frames) and localStorage writes per device run are in audit-report.json '
             '(`device_meta`). In the menus recording every device writes `katam.settings` when the '
             'in-game SETTINGS screens change a value (frames 571, 601 front end; 1751 pause page).\n')
    (ev / 'AUDIT-REPORT.md').write_text('\n'.join(L) + '\n')
