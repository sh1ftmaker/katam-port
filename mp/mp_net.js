/* The network transport: a relay room (netplay/) presented to the port as a
 * link cable, at the seam described in docs/MULTIPLAYER.md §2.
 *
 * One file for both hosts.  In a page it hangs createKatamNetTransport on
 * window and shell.html's katamMultiplayer does the attaching; under node the
 * headless tests require() it and hand it the module instance directly.
 * Node 22's global WebSocket and a browser's agree on everything this uses.
 *
 * The rules it implements are docs/NETPLAY.md §3, and each one is a way a
 * relay can silently break the game's own link protocol:
 *
 *   - every peer's words are an ordered stream, consumed exactly once, in
 *     order.  Sequence numbers detect a duplicated or dropped batch; a gap
 *     raises the link error bit rather than papering over it.
 *
 *   - a child treats slot 0's stream as the cable clock: no buffered parent
 *     word, no transfer.  That is what paces a child instance to the
 *     parent's transfer rate, and it is why this transport never has to know
 *     what a frame is.
 *
 *   - the parent's reading of its peers depends on which protocol is on the
 *     bus, and the transport can tell, because the whole GBA map lives in
 *     the module's linear memory: it reads SIOCNT out of the IO mirror.
 *
 *     During the lobby (serial-interrupt enable set -- sub_0803024C sets it
 *     and the MultiSio parent clears it), peers' words are *sampled
 *     registers*: take the next buffered word if one arrived, repeat the
 *     last if not, and never stall.  Stalling there was tried and is wrong
 *     in a way the lobby measures: a stalled transfer leaves SIOCNT's busy
 *     bit set across the frame, and the lobby's counter phase
 *     (multi_boot_util.c, sub_0803040C) reads any of bits 2-7 beyond SD as
 *     a broken cable and starts the whole handshake over.  On hardware busy
 *     clears ~228 cycles after the arm.
 *
 *     In play (interrupt enable clear on the parent), the bus carries
 *     MultiSio's framed packet stream, and a repeated word is poison: it
 *     shifts every later halfword of the packet by one and the checksum
 *     eats the whole thing.  But stalling instead is a different trap, and
 *     it was measured before it was understood: the parent's sixteen
 *     transfers a frame are chained by its own interrupt handler *within*
 *     the frame, and a child's reply to word N cannot cross a network
 *     inside the frame that sent word N -- strict word-for-word coupling
 *     collapses the chain to a one-transfer-per-frame equilibrium (measured
 *     at tx=1/frame, stalls=1/frame) and MultiSio never assembles a packet.
 *     On hardware the child replies between transfers; that causality is
 *     not preservable over a wire and MultiSio never needed it: packets are
 *     sync-framed and checksummed precisely so the stream survives noise.
 *     So a missing play-phase word is served as 0x0000 -- one corrupted,
 *     checksummed-away packet at worst, against the eight frames of input
 *     redundancy each packet carries -- and the pipeline stays at sixteen a
 *     frame, one frame of buffering apart, which is the §7 model from
 *     docs/MULTIPLAYER.md working as designed.
 *
 *   - a peer that has never sent a word reads as 0xFFFF -- an empty slot on
 *     the cable, which is what a unit that has not joined yet looks like on
 *     hardware, and what the lobby's recognition phase expects to see while
 *     it waits.
 *
 *   - words this unit puts on the bus are batched per burst -- a microtask
 *     flush picks up the frame's transfers after the wasm suspends -- so a
 *     frame costs one WebSocket message each way, whether it carried the
 *     lobby's one transfer or play's sixteen.
 *
 *   - the link outlives the socket.  A relay socket dies for reasons that
 *     have nothing to do with the session -- an idle middlebox timeout, a
 *     worker redeploy, a wifi blip -- so a lost socket reconnects with the
 *     same id (the room gives it its slot back), the game goes on seeing
 *     the cable as plugged in for a grace window while it does, and the
 *     word streams re-baseline on both sides: the room announces the
 *     leave/rejoin, which resets every peer's view of this stream, and a
 *     fresh stream's first batch is its baseline wherever its sequence
 *     starts.  A keep-alive ping holds the socket open through minutes of
 *     silent menus.  Lost words mid-stream -- once a fatal link error --
 *     are logged and skipped instead: the packet layer above is
 *     sync-framed and checksummed precisely so a stream survives noise.
 */
(function (root, factory) {
    'use strict';
    if (typeof module === 'object' && module.exports)
        module.exports = factory();
    else
        root.createKatamNetTransport = factory();
}(typeof self !== 'undefined' ? self : this, function () {
    'use strict';

    var MSG_WORDS = 0x01;
    var MAX_BATCH = 64;

    /* Resilience knobs.  PRIME is the jitter buffer: words a dried-out peer
     * stream must bank before play-phase consumption resumes (24 words is a
     * frame and a half).  It only engages once the stream has actually been
     * flowing (FLOWING words consumed) -- at session establishment the
     * queue is legitimately empty, and holding MultiSio's first sync words
     * back for 24 transfers reads to the parent as a child that never
     * connected.  LAG_CAP bounds the standing backlog before the stream is
     * dropped forward to PRIME and the packet layer resyncs; it sits above
     * the child's catch-up burst (32 a frame) and at the edge of MultiSio's
     * eight frames of input redundancy, past which the lag is fatal anyway.
     * GRACE_MS is how long a lost socket may spend reconnecting before the
     * game is told the cable came out. */
    var PRIME = 24;
    var FLOWING = 32;
    var LAG_CAP = 128;
    var PING_MS = 20000;
    var GRACE_MS = 15000;

    /* opts:
     *   Module   the emscripten module instance (required)
     *   url      ws:// or wss:// room URL (required unless socket given)
     *   socket   an already-constructed WebSocket-like, for tests
     *   log      function(text); defaults to console.log
     *
     * Returns { transport, attach, detach, state } -- `transport` is the
     * object for Module.portMp, `attach` wires it and opens the link. */
    function createKatamNetTransport(opts) {
        var Module = opts.Module;
        var log = opts.log || function (t) { console.log(t); };

        var st = {
            slot: -1,
            connected: false,
            error: 0,
            online: [false, false, false, false],
            seen: [false, false, false, false],
            queues: [[], [], [], []],       /* per-slot FIFO of peer words   */
            last: [0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF],   /* the sampled register */
            expected: [0, 0, 0, 0],         /* next stream index per peer    */
            priming: [0, 0, 0, 0],          /* words to bank before resuming */
            flow: [0, 0, 0, 0],             /* play words consumed this phase */
            outWords: [],
            outSeq: 0,
            flushArmed: false,
            sent: 0, received: 0, stalls: 0, phantoms: 0,
            holes: 0, dropped: 0, reconnects: 0,
            downSince: 0, closed: false,
        };

        var sock = null;
        var attempts = 0;

        /* Everything that is per-*socket* rather than per-session.  Streams
         * cannot cross a reconnect: the room announced us leaving, so every
         * peer reset its view of our stream to zero -- outSeq restarts to
         * match -- and whatever their streams carried while we were gone is
         * unknowable, so ours of theirs reset too and re-baseline on their
         * next batch. */
        function resetStreams() {
            st.outWords.length = 0;
            st.outSeq = 0;
            for (var i = 0; i < 4; i++) {
                st.seen[i] = false;
                st.queues[i].length = 0;
                st.last[i] = 0xFFFF;
                st.expected[i] = 0;
                st.priming[i] = 0;
                st.flow[i] = 0;
            }
        }

        function connect() {
            sock = opts.socket || new WebSocket(opts.url);
            sock.binaryType = 'arraybuffer';
            sock.onopen = function () {
                resetStreams();
                st.connected = true;
                st.downSince = 0;
                if (attempts)
                    log('[mp-net] socket reopened');
            };
            sock.onclose = function () {
                st.connected = false;
                if (st.closed)
                    return;
                if (!st.downSince)
                    st.downSince = Date.now();
                if (opts.socket) {          /* a handed-in socket: no way to
                                             * mint another one */
                    log('[mp-net] socket closed');
                    return;
                }
                var delay = Math.min(4000, 250 << Math.min(attempts, 4));
                attempts++;
                st.reconnects++;
                log('[mp-net] socket lost -- reconnecting in ' + delay + ' ms');
                setTimeout(function () {
                    if (!st.closed)
                        connect();
                }, delay);
            };
            sock.onerror = function () {
                log('[mp-net] socket error');
            };
            sock.onmessage = onMessage;
        }

        function onMessage(e) {
            if (typeof e.data === 'string') {
                var m;
                try { m = JSON.parse(e.data); } catch (err) { return; }
                if (m.type === 'joined') {
                    if (st.slot >= 0 && m.slot !== st.slot) {
                        /* The seat was taken while we were away.  The game
                         * cannot change slot mid-session, so this is the one
                         * reconnect outcome that really is a dead link. */
                        log('[mp-net] rejoined as slot ' + m.slot +
                            ' but was slot ' + st.slot + ' -- raising link error');
                        st.error = 1;
                        return;
                    }
                    attempts = 0;
                    st.slot = m.slot;
                    for (var i = 0; i < 4; i++)
                        st.online[i] = !!(m.online && m.online[i]);
                    log('[mp-net] joined as slot ' + m.slot);
                } else if (m.type === 'peer') {
                    st.online[m.slot] = m.online;
                    if (!m.online) {
                        /* The cable end came out.  Anything still buffered
                         * died with it, and a returning peer starts a fresh
                         * stream from zero. */
                        st.seen[m.slot] = false;
                        st.queues[m.slot].length = 0;
                        st.last[m.slot] = 0xFFFF;
                        st.expected[m.slot] = 0;
                        st.priming[m.slot] = 0;
                        st.flow[m.slot] = 0;
                    }
                    log('[mp-net] peer slot ' + m.slot +
                        (m.online ? ' joined' : ' left'));
                } else if (m.type === 'error') {
                    log('[mp-net] server: ' + m.reason);
                    st.error = 1;
                }
                return;
            }

            var b = new Uint8Array(e.data);
            if (b.length < 7 || b[0] !== MSG_WORDS)
                return;
            var dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
            var slot = dv.getUint8(1);
            var seq = dv.getUint32(2, true);
            var count = dv.getUint8(6);
            if (slot > 3 || slot === st.slot || b.length !== 7 + 2 * count)
                return;

            /* A fresh stream's first batch is its baseline: ours after a
             * reconnect (resetStreams), a peer's after their leave/rejoin
             * (the peer handler resets seen[]).  After that, a batch
             * replayed across a reconnect overlaps what was already
             * consumed: drop the overlap, keep the tail.  A batch from the
             * future means words were genuinely lost -- once a fatal link
             * error, now a logged hole: the packet layer above is
             * sync-framed and checksummed precisely so a stream survives
             * noise, so adopt the new position and let it. */
            if (!st.seen[slot])
                st.expected[slot] = seq;
            var skip = st.expected[slot] - seq;
            if (skip < 0) {
                st.holes -= skip;
                log('[mp-net] slot ' + slot + ': ' + (-skip) + ' word(s) lost at '
                    + st.expected[slot] + ' -- resyncing stream');
                st.priming[slot] = PRIME;
                skip = 0;
            }
            if (skip >= count)
                return;
            for (var w = skip; w < count; w++)
                st.queues[slot].push(dv.getUint16(7 + 2 * w, true));
            st.expected[slot] = seq + count;
            st.seen[slot] = true;
            st.received += count - skip;
        }

        connect();

        /* An idle socket dies at whatever timeout the quietest middlebox on
         * the path enforces, and the menus before MULTIPLAYER can sit
         * silent for minutes.  Both relays ignore JSON they do not
         * recognise, so a ping is free. */
        var pinger = setInterval(function () {
            if (sock && sock.readyState === 1)
                sock.send('{"type":"ping"}');
        }, PING_MS);

        function flush() {
            st.flushArmed = false;
            if (!st.outWords.length || sock.readyState !== 1)
                return;
            while (st.outWords.length) {
                var chunk = st.outWords.splice(0, MAX_BATCH);
                var buf = new ArrayBuffer(6 + 2 * chunk.length);
                var dv = new DataView(buf);
                dv.setUint8(0, MSG_WORDS);
                dv.setUint32(1, st.outSeq, true);
                dv.setUint8(5, chunk.length);
                for (var i = 0; i < chunk.length; i++)
                    dv.setUint16(6 + 2 * i, chunk[i], true);
                st.outSeq += chunk.length;
                st.sent += chunk.length;
                sock.send(buf);
            }
        }

        function armFlush() {
            if (st.flushArmed)
                return;
            st.flushArmed = true;
            /* Runs when the wasm suspends at the end of the frame, so the
             * whole burst of transfers goes as one message. */
            if (typeof queueMicrotask === 'function')
                queueMicrotask(flush);
            else
                setTimeout(flush, 0);
        }

        function onlineCount() {
            var n = 0;
            for (var i = 0; i < 4; i++)
                if (st.online[i])
                    n++;
            return n;
        }

        /* How many transfers could complete right now.  Only the clock
         * stream gates a transfer, so for a child this is slot 0's queue
         * depth -- what platform/sio.c consults to run catch-up transfers
         * when the host fell behind.  The parent is never gated. */
        function available() {
            return st.slot > 0 ? st.queues[0].length : 0;
        }

        var transport = {
            open: function (players) { return 1; },
            close: function () {
                st.closed = true;
                clearInterval(pinger);
                try { sock.close(); } catch (e) { /* already dead */ }
            },
            poll: function (ptr) {
                flush();                        /* backstop for the microtask */
                var h = Module.HEAPU8;
                /* A lost socket within its grace window still reads as a
                 * plugged-in cable: a child stalls on its silent clock and a
                 * parent's packets corrupt and are checksummed away, which
                 * is what a noisy cable does on hardware -- and nothing at
                 * all if the reconnect lands quickly enough. */
                var up = st.slot >= 0
                    && (st.connected
                        || (st.downSince
                            && Date.now() - st.downSince < GRACE_MS));
                h[ptr] = up ? 1 : 0;
                h[ptr + 1] = st.slot >= 0 ? st.slot : 0;
                h[ptr + 2] = onlineCount() || 1;
                h[ptr + 3] = st.error ? 1 : 0;
            },
            exchange: function (word, ptr) {
                /* Stall checks first, mutation after: a stalled exchange is
                 * retried with the same word, so consuming or sending
                 * anything on the stall path would double it. */
                var h = Module.HEAPU8;
                var s;

                /* A child is clocked by the parent's stream: nothing
                 * buffered from slot 0, no transfer. */
                if (st.slot !== 0 && st.queues[0].length === 0) {
                    st.stalls++;
                    return 0;
                }

                /* The SIOCNT read (out of the IO mirror in the module's own
                 * heap) is what distinguishes the lobby from play -- see the
                 * header comment for both policies. */
                var sioCnt = h[0x04000128] | (h[0x04000129] << 8);
                var lobbyStyle = (sioCnt & 0x4000) !== 0;

                if (lobbyStyle)
                    st.flow[0] = st.flow[1] = st.flow[2] = st.flow[3] = 0;

                for (s = 0; s < 4; s++) {
                    var w = 0xFFFF;
                    if (s !== st.slot && st.seen[s] && st.online[s]) {
                        var q = st.queues[s];
                        /* The standing backlog equals every phantom ever
                         * served -- each one was a transfer its real word
                         * missed, and the word still arrives and waits its
                         * turn -- so it only ever grows.  Past LAG_CAP the
                         * added input lag is worse than a moment of noise:
                         * drop forward to PRIME and let the packet layer
                         * find its sync word again. */
                        if (!lobbyStyle && q.length > LAG_CAP) {
                            st.dropped += q.length - PRIME;
                            q.splice(0, q.length - PRIME);
                        }
                        if (q.length
                            && (lobbyStyle || q.length >= st.priming[s])) {
                            st.priming[s] = 0;
                            if (!lobbyStyle)
                                st.flow[s]++;
                            w = st.last[s] = q.shift();
                        } else if (lobbyStyle) {
                            w = st.last[s];         /* sampled register      */
                        } else {
                            /* Dry mid-play: noise, not a repeat and not a
                             * stall (the header says why).  If the stream
                             * was flowing, make it bank PRIME words before
                             * resuming, so a latency spike costs one burst
                             * of checksummed-away packets and leaves a
                             * jitter buffer standing -- instead of the
                             * words trickling out one at a time and
                             * corrupting every packet for the whole of the
                             * spike.  A stream that never flowed is just
                             * MultiSio starting up; hold nothing back. */
                            if (!st.priming[s] && st.flow[s] >= FLOWING)
                                st.priming[s] = PRIME;
                            w = 0x0000;
                            st.phantoms++;
                        }
                    }
                    h[ptr + 2 * s] = w & 0xFF;
                    h[ptr + 2 * s + 1] = w >> 8;
                }
                st.outWords.push(word & 0xFFFF);
                armFlush();
                return 1;
            },
            pending: function () { return available(); },
        };

        return {
            transport: transport,
            state: st,
            socket: sock,
            attach: function (players) {
                Module.portMp = transport;
                return !!Module._PortMpUseJs(players || 2);
            },
            detach: function () {
                if (Module._PortMpDetach)
                    Module._PortMpDetach();
                transport.close();
            },
        };
    }

    return createKatamNetTransport;
}));
