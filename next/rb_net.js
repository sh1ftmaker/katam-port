/* Path B: the rollback session driver.  docs/NETPLAY.md §2's second
 * architecture -- no SIO, no MultiSio, no game lobby.  The game runs its
 * ordinary single-player world; the rollback engine (platform/rollback.c)
 * injects every seat's buttons through the game's own network-input branch;
 * this file moves those buttons over a netplay/ relay room and runs the
 * join/leave choreography.
 *
 * What travels: one 7-byte MSG_INPUT per frame per player, each relayed to
 * the other players and appended to the room's history.  A seat change is a
 * JSON `assign` relayed to everyone including its sender, so every
 * participant schedules the same event at the same frame -- the append-only
 * timeline discipline from platform/port/rollback.h.  A joiner asks for
 * `history`, confirms the whole session into a fresh timeline, replays it
 * with the picture off (PortRbReplayTo -- measured at ~12 us a frame under
 * node), then takes a seat a safe margin past the live frame.
 *
 * The discipline that cannot be delegated to this file: every participant
 * must reach the SAME game state at the SAME frame before the session
 * starts -- same boot inputs, same (empty) save.  The founders do that by
 * running one scripted boot; a joiner does it by running that same boot and
 * then replaying the history.  Activation (PortRbInit + PortRbNetPlay) is
 * part of the timeline for this purpose and happens at the agreed frame on
 * every instance, founders and joiners alike.
 */
(function (root, factory) {
    'use strict';
    if (typeof module === 'object' && module.exports)
        module.exports = factory();
    else
        root.createKatamRbSession = factory();
}(typeof self !== 'undefined' ? self : this, function () {
    'use strict';

    var MSG_INPUT = 0x02;
    var MSG_LOG = 0x03;

    /* A departed peer's seat goes to the AI only after this long -- a
     * socket blip shorter than this costs nothing but a stall at the
     * rollback window.  PING_MS keeps an idle socket alive through quiet
     * menus (both relays ignore unknown JSON). */
    var LOSS_GRACE_MS = 5000;
    var PING_MS = 20000;

    /* opts: { Module, url, socket?, log? } */
    function createKatamRbSession(opts) {
        var Module = opts.Module;
        var log = opts.log || function (t) { console.log(t); };

        var st = {
            slot: -1,               /* room slot == rollback peer id         */
            connected: false,
            online: [false, false, false, false],
            active: false,          /* engine initialised and driving        */
            seated: false,
            latest: 0,              /* highest frame the room reported       */
            newest: 0,              /* highest input frame heard live        */
            peerNewest: [0, 0, 0, 0],   /* highest input frame per peer      */
            peerKeys: [0, 0, 0, 0],     /* ... and the keys it carried       */
            lastSent: 0,            /* highest frame we put on the wire      */
            sent: 0, confirmed: 0, assigns: 0, reconnects: 0,
            closed: false,
        };
        var joinedResolvers = [];

        var rxInputs = [];          /* tagged records awaiting confirm       */
        var rxAssigns = [];
        var historyWait = null;     /* {resolve} while a history fetch runs  */
        var historyRecords = [];

        /* The link outlives the socket, exactly as web/mp_net.js does for
         * Path A -- but Path B can promise more on the far side of a
         * reconnect, because the room stores every input ever relayed.  The
         * stall gate (PortRbShouldStall) freezes this instance within the
         * rollback window of the moment the socket died, so our unheard
         * inputs are a bounded resend, and everything the peers did
         * meanwhile is in the room's history to refetch.  A reconnect is a
         * pause, not a desync. */
        var sock = null;
        var attempts = 0;
        var lossTimers = [null, null, null, null];

        function connect() {
            sock = opts.socket || new WebSocket(opts.url);
            sock.binaryType = 'arraybuffer';
            sock.onopen = function () {
                st.connected = true;
                if (attempts)
                    log('[rb-net] socket reopened');
            };
            sock.onclose = function () {
                st.connected = false;
                if (st.closed)
                    return;
                if (opts.socket) {      /* a handed-in socket: no way to
                                         * mint another one */
                    log('[rb-net] socket closed');
                    return;
                }
                var delay = Math.min(4000, 250 << Math.min(attempts, 4));
                attempts++;
                st.reconnects++;
                log('[rb-net] socket lost -- reconnecting in ' + delay +
                    ' ms (the game holds at the rollback window meanwhile)');
                setTimeout(function () {
                    if (!st.closed)
                        connect();
                }, delay);
            };
            sock.onerror = function () { log('[rb-net] socket error'); };
            sock.onmessage = onMessage;
        }

        /* After a reconnect: send the inputs we simulated that the room
         * never heard -- bounded by the stall gate to a window's worth --
         * then reconfirm the whole session from the room's history, which
         * fills whatever the peers did while we were gone.  ConfirmInput
         * ignores everything already known, so the refetch costs one pass. */
        function resync() {
            if (st.seated) {
                var now = Module._PortFrameNumber() >>> 0;
                var n = 0;
                for (var f = st.lastSent + 1; f <= now; f++, n++)
                    sendInput(f, Module._PortRbInputAt(st.slot, f) & 0x3FF);
                if (n)
                    log('[rb-net] re-sent ' + n + ' unheard input(s)');
            }
            historyRecords = [];
            historyWait = { resolve: function (h) {
                for (var i = 0; i < h.records.length; i++) {
                    var r = h.records[i];
                    Module._PortRbConfirmInput(r.slot, r.frame, r.keys);
                }
                if (h.latest > st.newest)
                    st.newest = h.latest;
                log('[rb-net] resynchronised (' + h.records.length +
                    ' session inputs reconfirmed)');
            } };
            sock.send(JSON.stringify({ type: 'history' }));
        }

        function onMessage(e) {
            if (typeof e.data === 'string') {
                var m;
                try { m = JSON.parse(e.data); } catch (err) { return; }
                if (m.type === 'joined') {
                    var was = st.slot;
                    st.slot = m.slot;
                    for (var i = 0; i < 4; i++)
                        st.online[i] = !!(m.online && m.online[i]);
                    log('[rb-net] joined room as peer ' + m.slot);
                    if (was >= 0 && m.slot !== was) {
                        /* The seat went to someone else while we were away.
                         * This engine cannot change peer id mid-session;
                         * dropping back in fresh is the reload path. */
                        log('[rb-net] seat lost across the reconnect (' + was +
                            ' -> ' + m.slot + ') -- reload the page to drop ' +
                            'back into the world');
                        st.closed = true;
                        try { sock.close(); } catch (e2) { /* dead */ }
                        return;
                    }
                    attempts = 0;
                    if (was >= 0 && st.active)
                        resync();
                    while (joinedResolvers.length)
                        joinedResolvers.shift()(m.slot);
                } else if (m.type === 'peer') {
                    st.online[m.slot] = m.online;
                    log('[rb-net] peer ' + m.slot + (m.online ? ' online' : ' offline'));
                    if (!m.online) {
                        /* Not a leave yet -- a grace window first, so a
                         * reconnecting peer keeps their Kirby.  Their
                         * absence shows as a stall at the rollback window,
                         * which is the price of never diverging. */
                        if (lossTimers[m.slot] === null && st.active) {
                            (function (peer) {
                                lossTimers[peer] = setTimeout(function () {
                                    lossTimers[peer] = null;
                                    if (!st.online[peer])
                                        handlePeerLoss(peer);
                                }, LOSS_GRACE_MS);
                            }(m.slot));
                        }
                    } else if (lossTimers[m.slot] !== null) {
                        clearTimeout(lossTimers[m.slot]);
                        lossTimers[m.slot] = null;
                        log('[rb-net] peer ' + m.slot + ' is back -- seat kept');
                    }
                } else if (m.type === 'assign') {
                    rxAssigns.push(m);
                } else if (m.type === 'history-done') {
                    if (historyWait) {
                        var w = historyWait;
                        historyWait = null;
                        w.resolve({ records: historyRecords, latest: m.latest });
                    }
                }
                return;
            }
            var b = new Uint8Array(e.data);
            if (b.length === 8 && b[0] === MSG_INPUT) {
                var dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
                var rec = { slot: dv.getUint8(1),
                            frame: dv.getUint32(2, true),
                            keys: dv.getUint16(6, true) };
                rxInputs.push(rec);
                if (rec.frame > st.newest)
                    st.newest = rec.frame;
                if (rec.frame >= st.peerNewest[rec.slot]) {
                    st.peerNewest[rec.slot] = rec.frame;
                    st.peerKeys[rec.slot] = rec.keys;
                }
            } else if (b[0] === MSG_LOG && historyWait) {
                var dv2 = new DataView(b.buffer, b.byteOffset, b.byteLength);
                var count = dv2.getUint16(1, true);
                for (var j = 0; j < count; j++) {
                    var o = 3 + 7 * j;
                    historyRecords.push({ slot: dv2.getUint8(o),
                                          frame: dv2.getUint32(o + 1, true),
                                          keys: dv2.getUint16(o + 5, true) });
                }
            }
        };

        function sendInput(frame, keys) {
            if (sock.readyState !== 1)
                return;
            var buf = new ArrayBuffer(7);
            var dv = new DataView(buf);
            dv.setUint8(0, MSG_INPUT);
            dv.setUint32(1, frame >>> 0, true);
            dv.setUint16(5, keys & 0xFFFF, true);
            sock.send(buf);
            st.sent++;
            if (frame > st.lastSent)
                st.lastSent = frame;
        }

        function applyAssign(m) {
            var now = Module._PortFrameNumber();
            /* The seal rides with the seat change: the announcer's last
             * word on what the departed peer held, applied with authority
             * (PortRbSealInput) so every survivor's timeline agrees at the
             * exact frames where their views of the dead stream differ. */
            if (m.seal && typeof m.seal.player === 'number') {
                var to = m.frame >>> 0;
                var from = m.seal.from >>> 0;
                if (to - from <= 3600) {
                    for (var f = from; f < to; f++)
                        Module._PortRbSealInput(m.seal.player, f,
                                                m.seal.keys & 0x3FF);
                    log('[rb-net] sealed peer ' + m.seal.player + ' from ' +
                        from + ' to ' + to);
                }
            }
            if (m.frame <= now + 2)
                log('[rb-net] WARNING: assign for slot ' + m.slot +
                    ' at frame ' + m.frame + ' arrived at ' + now +
                    ' -- too late to apply consistently');
            Module._PortRbAssignSlot(m.frame, m.slot, m.peer);
            st.assigns++;
            log('[rb-net] slot ' + m.slot + ' -> ' +
                (m.peer < 0 ? 'AI' : 'peer ' + m.peer) + ' at frame ' + m.frame);
        }

        /* A departed peer's Kirby goes to the AI, announced once, by the
         * lowest-numbered seated survivor so exactly one announcement wins.
         * The announcement carries the seal: the stream officially ends at
         * the announcer's last-heard frame, held to the seat change. */
        var handlePeerLoss = function (peer) {
            if (!st.active || sock.readyState !== 1)
                return;
            var slot = Module._PortRbPeerSlot(peer);
            if (slot < 0)
                return;
            var lowest = -1;
            for (var s = 0; s < 4; s++) {
                var p = Module._PortRbSlotPeer(s);
                if (p >= 0 && p !== peer && st.online[p] && (lowest < 0))
                    lowest = p;
            }
            if (lowest === st.slot)
                sock.send(JSON.stringify({ type: 'assign',
                    frame: (Module._PortRbSuggestEventFrame() >>> 0) + 30,
                    slot: slot, peer: -1,
                    seal: { player: peer, from: st.peerNewest[peer] + 1,
                            keys: st.peerKeys[peer] } }));
        };

        /* Everything that arrived, into the engine.  Called from frame()
         * each presented frame -- and from the host's stall loop
         * (Module.portNetIdle), because during a stall frames are exactly
         * what is not happening, and these queues are what end it. */
        function pump() {
            var i;
            for (i = 0; i < rxAssigns.length; i++)
                applyAssign(rxAssigns[i]);
            rxAssigns.length = 0;
            if (rxInputs.length) {
                for (i = 0; i < rxInputs.length; i++) {
                    var r = rxInputs[i];
                    Module._PortRbConfirmInput(r.slot, r.frame, r.keys);
                }
                st.confirmed += rxInputs.length;
                rxInputs.length = 0;
            }
        }

        connect();

        var pinger = setInterval(function () {
            if (sock && sock.readyState === 1)
                sock.send('{"type":"ping"}');
        }, PING_MS);

        return {
            state: st,
            get socket() { return sock; },

            /* Resolves with this instance's room slot (its peer id). */
            whenJoined: function () {
                return new Promise(function (res) {
                    if (st.slot >= 0) res(st.slot);
                    else joinedResolvers.push(res);
                });
            },

            /* The room's stored history, engine untouched -- the raw
             * material for web/rb_boot.js, which owns the orchestration.
             * Stored assigns arrive as ordinary control messages and land in
             * the assign queue; frame() schedules them once the engine is
             * active. */
            fetchHistory: function () {
                return new Promise(function (resolve) {
                    var ask = function () {
                        sock.send(JSON.stringify({ type: 'history' }));
                    };
                    historyRecords = [];
                    historyWait = { resolve: resolve };
                    if (sock.readyState === 1)
                        ask();
                    else
                        sock.addEventListener('open', ask, { once: true });
                });
            },

            /* The engine was initialised by the caller; start pumping. */
            activate: function () { st.active = true; },

            /* Founders, at the agreed activation frame.  `players` seats
             * 0..players-1 are taken by peers 0..players-1 (PortRbInit's
             * identity map); the rest are the AI's. */
            start: function (depth, players) {
                if (!Module._PortRbInit(depth, players))
                    return 0;
                Module._PortRbSetSelf(st.slot);
                if (!Module._PortRbNetPlay(st.slot))
                    return 0;
                st.active = true;
                st.seated = st.slot < players;
                return 1;
            },

            /* A joiner, at that same frame, with the game held there by the
             * host (do not resolve rAF while this promise is pending).
             * Resolves once the engine is replaying; the caller resumes the
             * frame loop and the catch-up runs at simulation speed. */
            /* `players` must be exactly what the founders passed to start():
             * PortRbInit's identity slot map is part of the shared initial
             * condition, and a joiner that inits a different count maps the
             * AI slots to empty peer columns -- measured as a joiner whose
             * AI Kirbys stand still while everyone else's wander. */
            join: function (depth, players) {
                return new Promise(function (resolve, reject) {
                    var ask = function () {
                        sock.send(JSON.stringify({ type: 'history' }));
                    };
                    historyRecords = [];
                    historyWait = { resolve: function (h) {
                        if (!Module._PortRbInit(depth, players))
                            return reject(new Error('PortRbInit failed'));
                        Module._PortRbSetSelf(st.slot);
                        Module._PortRbNetPlay(st.slot);
                        st.active = true;
                        var i;
                        for (i = 0; i < rxAssigns.length; i++)
                            applyAssign(rxAssigns[i]);
                        rxAssigns.length = 0;
                        for (i = 0; i < h.records.length; i++) {
                            var r = h.records[i];
                            Module._PortRbConfirmInput(r.slot, r.frame, r.keys);
                        }
                        st.confirmed += h.records.length;
                        st.latest = h.latest;
                        if (h.latest > Module._PortFrameNumber())
                            Module._PortRbReplayTo(h.latest);
                        log('[rb-net] joining: ' + h.records.length +
                            ' inputs replayed to frame ' + h.latest);
                        resolve(h);
                    } };
                    if (sock.readyState === 1)
                        ask();
                    else
                        sock.addEventListener('open', ask, { once: true });
                });
            },

            /* Ask for a Kirby: the lowest AI-driven slot, a safe margin past
             * the live frame.  Everyone, this instance included, applies the
             * seat change when the relayed announcement comes back. */
            requestSeat: function () {
                var slot = Module._PortRbVacantSlot();
                if (slot < 0)
                    return -1;
                var frame = (Module._PortRbSuggestEventFrame() >>> 0) + 30;
                sock.send(JSON.stringify({ type: 'assign', frame: frame,
                                           slot: slot, peer: st.slot }));
                return slot;
            },

            /* Once per host frame, from portPresent: apply what arrived,
             * record and publish the local buttons.  `keys` is what the
             * player is holding for the frame about to run. */
            frame: function (keys) {
                pump();
                if (!st.active)
                    return;
                st.seated = Module._PortRbPeerSlot(st.slot) >= 0;
                if (st.seated) {
                    /* The engine owns the frame number: stamping the wire
                     * with anything derived host-side skews every edge by
                     * the phase difference.  See PortRbSetLocalInput. */
                    var f = Module._PortRbSetLocalInput(keys & 0x3FF) >>> 0;
                    if (f)
                        sendInput(f, keys & 0x3FF);
                }
            },

            /* The host's stall-loop pump; see pump(). */
            idle: pump,

            close: function () {
                st.closed = true;
                clearInterval(pinger);
                for (var i = 0; i < 4; i++)
                    if (lossTimers[i] !== null) {
                        clearTimeout(lossTimers[i]);
                        lossTimers[i] = null;
                    }
                try { sock.close(); } catch (e) { /* already dead */ }
            },
        };
    }

    return createKatamRbSession;
}));
