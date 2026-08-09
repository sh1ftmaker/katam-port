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
            sent: 0, confirmed: 0, assigns: 0,
        };

        var rxInputs = [];          /* tagged records awaiting confirm       */
        var rxAssigns = [];
        var historyWait = null;     /* {resolve} while a history fetch runs  */
        var historyRecords = [];

        var sock = opts.socket || new WebSocket(opts.url);
        sock.binaryType = 'arraybuffer';
        sock.onopen = function () { st.connected = true; };
        sock.onclose = function () { st.connected = false; log('[rb-net] socket closed'); };
        sock.onerror = function () { st.connected = false; log('[rb-net] socket error'); };

        sock.onmessage = function (e) {
            if (typeof e.data === 'string') {
                var m;
                try { m = JSON.parse(e.data); } catch (err) { return; }
                if (m.type === 'joined') {
                    st.slot = m.slot;
                    for (var i = 0; i < 4; i++)
                        st.online[i] = !!(m.online && m.online[i]);
                    log('[rb-net] joined room as peer ' + m.slot);
                } else if (m.type === 'peer') {
                    st.online[m.slot] = m.online;
                    log('[rb-net] peer ' + m.slot + (m.online ? ' online' : ' offline'));
                    if (!m.online && handlePeerLoss)
                        handlePeerLoss(m.slot);
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
                rxInputs.push({ slot: dv.getUint8(1),
                                frame: dv.getUint32(2, true),
                                keys: dv.getUint16(6, true) });
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
        }

        function applyAssign(m) {
            var now = Module._PortFrameNumber();
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
         * lowest-numbered seated survivor so exactly one announcement wins. */
        var handlePeerLoss = function (peer) {
            if (!st.active)
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
                    frame: Module._PortRbSuggestEventFrame() >>> 0,
                    slot: slot, peer: -1 }));
        };

        return {
            state: st,
            socket: sock,

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

            close: function () {
                try { sock.close(); } catch (e) { /* already dead */ }
            },
        };
    }

    return createKatamRbSession;
}));
