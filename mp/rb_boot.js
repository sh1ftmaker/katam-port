/* Boot-to-world, and inhabiting a Kirby that is already there.
 *
 * The one problem Path B leaves the page is the shared initial condition:
 * every participant must reach the same state at the same frame before the
 * timeline means anything.  A snapshot cannot cross the wire -- it holds
 * host function pointers (docs/MULTIPLAYER.md §11) -- so the host's world
 * state travels the only way it can: as the inputs that produced it.
 *
 * The trick that makes it seamless is that the *boot is inputs too*.  The
 * session's timeline starts at power-on: frames 0..F0 of player 0's column
 * are a fixed menu script (title -> FILE 1 -> ONE PLAYER -> the world),
 * baked into this file rather than fetched, because every copy of the page
 * ships it.  The engine's own catch-up replays it with the picture off at
 * ~80x, so "boot to world" is under a second of wall clock for everyone --
 * the host on founding, a joiner on arrival.  A joiner simply keeps
 * replaying past F0 into the room's stored history, and arrives bit-exact
 * in the host's present, however long the session has run.  Then it asks
 * for a seat: the lowest AI-driven slot, taken over in place -- the Kirby
 * keeps its position, ability and everything else; the AI just stops
 * steering it (platform/port/rollback.h, "arbitrary joining and leaving").
 *
 * Determinism discipline, all of it load-bearing:
 *
 *   - save memory must be blank and stay unpersisted -- the boot script's
 *     menu timings assume FILE 1 is empty, and a netplay session must never
 *     eat the player's real save.  The caller isolates SRAM before the game
 *     touches it.
 *   - the engine initialises at frame 0, before the first frame runs --
 *     the caller invokes begin() with the game held at the starting line
 *     (in the page: before resolving portRomReady).
 *   - the world is created single-player.  EV_PLAYERS holds the game's
 *     player count at 1 through the boot -- so the world spawns one human
 *     and three AI Kirbys, the Kirbys a joiner will inhabit -- and raises
 *     it to 4 at F0.  EV_NETPLAY turns the network-input branch on at F0.
 *     Both are timeline events, so every instance and every replayer flips
 *     them at the same frame.
 *   - gUnk_0203AD3C (camera, menus, "which Kirby is you") stays 0 until
 *     this instance actually takes a seat: it is sim-neutral in the world
 *     but unproven in the menus, so nobody's boot runs with a nonzero one.
 */
(function (root, factory) {
    'use strict';
    if (typeof module === 'object' && module.exports)
        module.exports = factory();
    else
        root.createKatamWorldSession = factory();
}(typeof self !== 'undefined' ? self : this, function () {
    'use strict';

    /* The session constants.  Change any of these and running sessions
     * split into incompatible worlds; the room name should carry a version
     * when that starts to matter. */
    var F0 = 2200;                  /* activation: the world is up by here   */
    var DEPTH = 16;                 /* rollback window, frames               */
    var EV_PLAYERS = 1, EV_NETPLAY = 3;

    /* The proven boot script (tools/netplay_rb_test.mjs, §3a item 6 of
     * docs/NETPLAY.md): mash A through the title and file select, then the
     * timed presses through GAME SELECT into ONE PLAYER, idle from 900. */
    var PRESSES = [[480, 1], [500, 0], [560, 1], [580, 0], [720, 1], [740, 0],
                   [810, 0x80], [825, 0], [850, 1], [870, 0]];
    function bootMask(f) {
        if (f >= 300 && f < 440)
            return ((f - 300) % 8) < 4 ? 1 : 0;
        var mask = 0;
        for (var i = 0; i < PRESSES.length; i++)
            if (f >= PRESSES[i][0])
                mask = PRESSES[i][1];
        return f < 900 ? mask : 0;
    }

    /* opts: { Module, driver (a createKatamRbSession), log?,
     *         getKeys?: () => current local button mask,
     *         onStatus?: (text) => {} } */
    function createKatamWorldSession(opts) {
        var Module = opts.Module;
        var driver = opts.driver;
        var log = opts.log || function (t) { console.log(t); };
        var status = opts.onStatus || log;
        var getKeys = opts.getKeys || function () {
            return Module._PortCurrentKeys ? Module._PortCurrentKeys() : 0;
        };

        var st = {
            phase: 'connecting',    /* -> syncing -> seating -> playing      */
            slot: -1,
            role: null,             /* 'host' | 'joiner'                     */
        };

        function confirmBoot(column) {
            for (var f = 0; f < F0; f++)
                Module._PortRbConfirmInput(column, f, bootMask(f));
        }

        function caughtUp() {
            return !Module._PortRbCatchingUp();
        }

        /* Call with the game held at frame 0 (nothing simulated yet).
         * Resolves once the engine is primed and replaying; the caller then
         * lets the game run and calls tick() once per presented frame. */
        function begin() {
            return driver.whenJoined().then(function (slot) {
                st.slot = slot;
                status('[world] room slot ' + slot + ' -- fetching session');
                return driver.fetchHistory().then(function (h) {
                    st.role = h.latest > 0 ? 'joiner' : 'host';
                    if (!Module._PortRbInit(DEPTH, 1))
                        throw new Error('PortRbInit failed');
                    Module._PortRbSetSelf(slot);
                    /* Single-player world through the boot; four seats and
                     * the network-input branch from F0.  Events, so every
                     * replayer flips them at the same frames. */
                    Module._PortRbScheduleEvent(0, EV_PLAYERS, 1, 0);
                    Module._PortRbScheduleEvent(F0, EV_PLAYERS, 4, 0);
                    Module._PortRbScheduleEvent(F0, EV_NETPLAY, 0, 0);
                    confirmBoot(0);
                    if (slot !== 0)
                        confirmBoot(slot);  /* drives this instance's menus  */
                    driver.activate();
                    driver.frame(0);        /* schedule any stored assigns   */
                    for (var i = 0; i < h.records.length; i++) {
                        var r = h.records[i];
                        Module._PortRbConfirmInput(r.slot, r.frame, r.keys);
                    }
                    var target = Math.max(F0, h.latest);
                    Module._PortRbReplayTo(target);
                    st.phase = 'syncing';
                    status('[world] ' + st.role + ': replaying to frame ' +
                           target + ' (' + h.records.length + ' session inputs)');
                });
            });
        }

        /* Once per presented frame, from the host's portPresent.  Runs the
         * whole life cycle: close the gap to the live session, take a seat,
         * then just relay buttons. */
        function tick() {
            if (st.phase === 'connecting')
                return;
            if (st.phase === 'syncing') {
                if (!caughtUp())
                    return;
                /* The room moved on while we replayed.  Every input that
                 * arrived live is queued; apply it and replay again until
                 * the gap is inside the rollback window.  Two or three
                 * rounds -- each replay outruns the live session by two
                 * orders of magnitude. */
                driver.frame(0);
                var newest = driver.state.newest >>> 0;
                var now = Module._PortFrameNumber() >>> 0;
                if (newest > now + 12) {
                    Module._PortRbReplayTo(newest);
                    return;
                }
                st.phase = 'seating';
                if (st.role === 'host') {
                    /* Seated by construction: Init's map gives slot 0 to
                     * peer 0.  Take the camera and go. */
                    Module._PortRbNetPlay(0);
                    st.phase = 'playing';
                    status('[world] hosting -- world is live');
                } else {
                    var seat = driver.requestSeat();
                    status(seat >= 0
                        ? '[world] synced to the live session -- taking over Kirby ' + seat
                        : '[world] synced, but no free Kirby to inhabit');
                }
                return;
            }
            if (st.phase === 'seating') {
                driver.frame(getKeys());
                if (driver.state.seated) {
                    Module._PortRbNetPlay(driver.state.slot >= 0
                        ? Module._PortRbPeerSlot(st.slot) : 0);
                    st.phase = 'playing';
                    status('[world] seated -- playing');
                }
                return;
            }
            driver.frame(getKeys());
        }

        return { state: st, begin: begin, tick: tick, bootMask: bootMask,
                 F0: F0 };
    }

    return createKatamWorldSession;
}));
