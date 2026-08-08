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
 *   - a peer that has never sent a word reads as 0xFFFF -- an empty slot on
 *     the cable, which is what a unit that has not joined yet looks like on
 *     hardware.  A peer that HAS been heard from and has nothing buffered is
 *     merely late, and the answer to late is a stall, never 0xFFFF: the
 *     recognition counters in the game's lobby reset on any change, so one
 *     synthesised word costs thirty settled frames.
 *
 *   - words this unit puts on the bus are batched per burst -- a microtask
 *     flush picks up the frame's transfers after the wasm suspends -- so a
 *     frame costs one WebSocket message each way, whether it carried the
 *     lobby's one transfer or play's sixteen.
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
            expected: [0, 0, 0, 0],         /* next stream index per peer    */
            outWords: [],
            outSeq: 0,
            flushArmed: false,
            sent: 0, received: 0, stalls: 0,
        };

        var sock = opts.socket || new WebSocket(opts.url);
        sock.binaryType = 'arraybuffer';

        sock.onopen = function () { st.connected = true; };
        sock.onclose = function () {
            st.connected = false;
            log('[mp-net] socket closed');
        };
        sock.onerror = function () {
            st.connected = false;
            log('[mp-net] socket error');
        };
        sock.onmessage = function (e) {
            if (typeof e.data === 'string') {
                var m;
                try { m = JSON.parse(e.data); } catch (err) { return; }
                if (m.type === 'joined') {
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
                        st.expected[m.slot] = 0;
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

            /* A batch replayed across a reconnect overlaps what was already
             * consumed: drop the overlap, keep the tail.  A batch from the
             * future means words were lost, and lost words are a dead
             * session, not a quiet one -- raise the error bit the game
             * already knows how to report. */
            var skip = st.expected[slot] - seq;
            if (skip < 0) {
                log('[mp-net] slot ' + slot + ': gap at ' + st.expected[slot] +
                    ' (got ' + seq + ') -- raising link error');
                st.error = 1;
                return;
            }
            if (skip >= count)
                return;
            for (var w = skip; w < count; w++)
                st.queues[slot].push(dv.getUint16(7 + 2 * w, true));
            st.expected[slot] = seq + count;
            st.seen[slot] = true;
            st.received += count - skip;
        };

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

        /* How many transfers could complete right now.  A child is clocked
         * by slot 0's stream; every heard-from peer also has to have a word
         * ready or the transfer would stall anyway. */
        function available() {
            var n = Infinity;
            for (var s = 0; s < 4; s++) {
                if (s === st.slot)
                    continue;
                if (s === 0 || (st.seen[s] && st.online[s]))
                    n = Math.min(n, st.queues[s].length);
            }
            return n === Infinity ? 0 : n;
        }

        var transport = {
            open: function (players) { return 1; },
            close: function () {
                try { sock.close(); } catch (e) { /* already dead */ }
            },
            poll: function (ptr) {
                flush();                        /* backstop for the microtask */
                var h = Module.HEAPU8;
                h[ptr] = (st.connected && st.slot >= 0) ? 1 : 0;
                h[ptr + 1] = st.slot >= 0 ? st.slot : 0;
                h[ptr + 2] = onlineCount() || 1;
                h[ptr + 3] = st.error ? 1 : 0;
            },
            exchange: function (word, ptr) {
                /* Availability first, mutation after: a stalled exchange is
                 * retried with the same word, so consuming or sending
                 * anything on the stall path would double it. */
                var s;
                for (s = 0; s < 4; s++) {
                    if (s === st.slot)
                        continue;
                    if (st.queues[s].length)
                        continue;
                    if ((st.slot !== 0 && s === 0) ||
                        (st.seen[s] && st.online[s])) {
                        st.stalls++;
                        return 0;
                    }
                }

                var h = Module.HEAPU8;
                for (s = 0; s < 4; s++) {
                    var w = (s !== st.slot && st.queues[s].length)
                        ? st.queues[s].shift() : 0xFFFF;
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
