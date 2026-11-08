/* Netplay v2, the game side: drives today's engine (platform/rollback.c)
 * from a protocol-v2 session (web/net_session.js).
 *
 * The model is docs/design/NETPLAY-V2.md "Model v2.1":
 *
 *   - nobody waits for anybody.  The room seals every frame at a deadline;
 *     this file feeds the sealed stream into the engine as authoritative
 *     (PortRbSealInput), predicts the rest ("repeat last", the engine's
 *     own), and never stalls (PortRbSetStallGate(0)).  If the sealed stream
 *     overtakes this instance it fast-forwards (PortRbReplayTo).
 *   - corrections inside the engine's ring are the engine's shallow
 *     rollback -- local, optional, nothing on the wire knows about it.
 *     Anything else -- a peer that fell too far behind, a desync, a join --
 *     is repaired by a world sync: the room names a donor, the donor sends
 *     the pages of its state at a checkpoint that differ from ours, and we
 *     apply them at that frame and re-run our inputs since
 *     (PortRbApplySnapshotAt).
 *   - every `hashEvery` frames past inputStart, once the frame is sealed and
 *     simulated, the state at its start is hashed (the game's own desync
 *     quantity: gRngVal and every Kirby's position) and reported; the room
 *     is the referee.
 *
 * Same-build only, like the ring: a snapshot holds host function pointers
 * until netsim's portable snapshots (M2) land, and the room's version gate
 * (simVersion) is what keeps builds apart.  When netsim's PortNet* API
 * lands, engineRb below is replaced by an adapter over it; the protocol
 * does not change.
 *
 * opts: { Module, net, log?, getKeys?, onStatus?(text), onCorrection?(info),
 *         want: 'play'|'watch', depth? }
 */
(function (root, factory) {
    'use strict';
    if (typeof module === 'object' && module.exports)
        module.exports = factory(require('./rb_boot.js'));
    else
        root.createKatamNetGame = factory(root.createKatamWorldSession);
}(typeof self !== 'undefined' ? self : this, function (World) {
    'use strict';

    var EV_PLAYERS = 1, EV_SLOT = 2, EV_NETPLAY = 3, EV_STORY = 4, SLOT_AI = 0xFF;
    var ENGINE_SEATS = 4;            /* PORT_RB_PLAYERS: today's columns */
    var PAGE = 4096;
    var KEEP_CHECKPOINTS = 8;

    /* The game's own desync quantity, read out of a snapshot (EWRAM at 0,
     * IWRAM at 0x40000): gRngVal and every Kirby's x and y. */
    function simHash(snap) {
        var dv = new DataView(snap.buffer, snap.byteOffset, snap.byteLength);
        var h = 0x811C9DC5;
        function mix(v) {
            for (var i = 0; i < 4; i++) {
                h ^= (v >>> (8 * i)) & 0xFF;
                h = Math.imul(h, 0x01000193) >>> 0;
            }
        }
        mix(dv.getUint32(0x40000 + 0x68D8, true));
        for (var k = 0; k < 4; k++) {
            mix(dv.getUint32(0x20EE0 + 424 * k + 64, true));
            mix(dv.getUint32(0x20EE0 + 424 * k + 68, true));
        }
        return ('0000000' + h.toString(16)).slice(-8);
    }

    function pageHashes(snap) {
        var n = Math.ceil(snap.length / PAGE);
        var out = new Uint32Array(n);
        for (var p = 0; p < n; p++) {
            var h = 0x811C9DC5, end = Math.min(snap.length, (p + 1) * PAGE);
            for (var i = p * PAGE; i < end; i += 4) {
                h ^= snap[i] | (snap[i + 1] << 8) | (snap[i + 2] << 16) | (snap[i + 3] << 24);
                h = Math.imul(h, 0x01000193);
            }
            out[p] = h >>> 0;
        }
        return out;
    }

    function b64(u8) {
        if (typeof Buffer !== 'undefined') return Buffer.from(u8.buffer, u8.byteOffset, u8.byteLength).toString('base64');
        var s = '';
        for (var i = 0; i < u8.length; i += 0x8000)
            s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
        return btoa(s);
    }

    function unb64(s) {
        if (typeof Buffer !== 'undefined') return new Uint8Array(Buffer.from(s, 'base64'));
        var bin = atob(s), u = new Uint8Array(bin.length);
        for (var i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i);
        return u;
    }

    function pipe(u8, Stream) {
        var cs = new Stream('deflate-raw');
        var w = cs.writable.getWriter();
        w.write(u8);
        w.close();
        return new Response(cs.readable).arrayBuffer().then(function (b) { return new Uint8Array(b); });
    }
    function deflate(u8) { return pipe(u8, CompressionStream); }
    function inflate(u8) { return pipe(u8, DecompressionStream); }

    /* Blob layout, aligned with netsim's draft (KSNP header, then the
     * compressed body); ver 0 = today's same-build ring snapshot, deflate.
     *   u32 'KSNP' u16 ver u16 kind(0 full, 2 pages) u32 frame u32 baseFrame
     *   u32 rawSize u32 simLo u32 simHi u32 hashLo u32 hashHi    (36 bytes)
     * body (deflate-raw): u32 pageSize u32 nPages u32 count, count x u32
     * index, then the pages. */
    var MAGIC = 0x504E534B;

    function buildBlob(snap, frame, h, theirHashes) {
        var n = Math.ceil(snap.length / PAGE);
        var idx = [];
        if (theirHashes) {
            var mine = pageHashes(snap);
            for (var p = 0; p < n; p++) if (mine[p] !== theirHashes[p]) idx.push(p);
        } else {
            for (var q = 0; q < n; q++) idx.push(q);
        }
        var bodyLen = 12 + 4 * idx.length;
        for (var k = 0; k < idx.length; k++)
            bodyLen += Math.min(PAGE, snap.length - idx[k] * PAGE);
        var body = new Uint8Array(bodyLen);
        var dv = new DataView(body.buffer);
        dv.setUint32(0, PAGE, true);
        dv.setUint32(4, n, true);
        dv.setUint32(8, idx.length, true);
        var o = 12;
        for (k = 0; k < idx.length; k++, o += 4) dv.setUint32(o, idx[k], true);
        for (k = 0; k < idx.length; k++) {
            var s = snap.subarray(idx[k] * PAGE, Math.min(snap.length, (idx[k] + 1) * PAGE));
            body.set(s, o);
            o += s.length;
        }
        return deflate(body).then(function (z) {
            var out = new Uint8Array(36 + z.length);
            var hv = new DataView(out.buffer);
            hv.setUint32(0, MAGIC, true);
            hv.setUint16(4, 0, true);
            hv.setUint16(6, theirHashes ? 2 : 0, true);
            hv.setUint32(8, frame >>> 0, true);
            hv.setUint32(12, 0, true);
            hv.setUint32(16, snap.length, true);
            hv.setUint32(28, parseInt(h, 16) >>> 0, true);
            out.set(z, 36);
            return { blob: out, pages: idx.length };
        });
    }

    /* -> Promise<{snap, pages}> rebuilt onto `base` (or zeros). */
    function applyBlob(blob, base, size) {
        var hv = new DataView(blob.buffer, blob.byteOffset, blob.byteLength);
        if (blob.length < 36 || hv.getUint32(0, true) !== MAGIC)
            return Promise.reject(new Error('not a KSNP blob'));
        if (hv.getUint32(16, true) !== size)
            return Promise.reject(new Error('snapshot size differs (another build?)'));
        return inflate(blob.subarray(36)).then(function (body) {
            var dv = new DataView(body.buffer, body.byteOffset, body.byteLength);
            var ps = dv.getUint32(0, true), count = dv.getUint32(8, true);
            var snap = base ? base.slice() : new Uint8Array(size);
            var o = 12 + 4 * count;
            for (var k = 0; k < count; k++) {
                var p = dv.getUint32(12 + 4 * k, true);
                var len = Math.min(ps, size - p * ps);
                snap.set(body.subarray(o, o + len), p * ps);
                o += len;
            }
            return { snap: snap, pages: count };
        });
    }

    function createKatamNetGame(opts) {
        var Module = opts.Module;
        var net = opts.net;
        var log = opts.log || function (t) { console.log(t); };
        var status = opts.onStatus || function () {};
        var getKeys = opts.getKeys || function () {
            return Module._PortCurrentKeys ? Module._PortCurrentKeys() : 0;
        };
        var DEPTH = opts.depth || 32;   /* PORT_RB_MAX_FRAMES: ~535 ms of round trip */

        var st = {
            phase: 'idle',          /* idle -> booting -> live              */
            seat: -1, kirby: -1, focus: -1, liveFrom: Infinity,
            frontier: 0, inputStart: 0, hashEvery: 30, nextCheck: 0,
            hashes: 0, syncs: 0, syncPages: 0, syncBytes: 0, corrections: [],
            ffwd: 0, sent: 0, sealedApplied: 0, lastView: 0,
            delay: opts.delay || 2, hold: 0, held: 0, lastStamped: -1,
            clock: null, clockAt: 0, clockRtt: 0, lastLate: 0, lastLateAt: 0,
        };
        var queue = [];             /* sealed frames awaiting the engine     */
        var evQueue = [];           /* session events awaiting scheduling    */
        var copies = new Map();     /* checkpoint frame -> snapshot bytes    */
        var hashOf = new Map();
        var snapPtr = 0, snapSize = 0;
        var begun = null;           /* resolves when the engine is primed    */
        var mySeatEvents = [];      /* {f, kind, slot?}                      */

        function engineFrame() { return Module._PortFrameNumber() >>> 0; }
        function catching() { return !!Module._PortRbCatchingUp(); }

        /* --- session -> engine ------------------------------------------------ */

        function schedule(ev) {
            var f = ev.f >>> 0;
            if (st.phase === 'live' && f <= engineFrame() && !catching()) {
                log('[net-game] event ' + ev.kind + '@' + f + ' arrived at ' + engineFrame() +
                    ' -- applying next frame; a sync will reconcile');
                f = engineFrame() + 1;
            }
            if (ev.kind === 'slot' && ev.slot < ENGINE_SEATS) {
                var peer = ev.seat < 0 || ev.seat >= ENGINE_SEATS ? -1 : ev.seat;
                Module._PortRbAssignSlot(f, ev.slot, peer);
                if (ev.seat === st.seat && st.seat >= 0) mySeatEvents.push({ f: f, slot: ev.slot });
                else mySeatEvents = mySeatEvents.filter(function (x) { return x.slot !== ev.slot || x.f > f; });
            } else if (ev.kind === 'view') {
                if (Module._PortSimViewUseSession && Module._PortInputLogScheduleView)
                    Module._PortInputLogScheduleView(f, ev.w, ev.h);
            } else if (ev.kind === 'seat' && ev.seat === st.seat) {
                st.liveFrom = f;
            } else if (ev.kind === 'unseat' && ev.seat === st.seat) {
                st.liveFrom = Infinity;
            }
        }

        function applyFrames(r) {
            var seats = Math.min(ENGINE_SEATS, r.frames.length ? r.frames[0].length : 0);
            for (var i = 0; i < r.frames.length; i++) {
                var f = r.firstFrame + i, v = r.frames[i];
                for (var s = 0; s < seats; s++)
                    Module._PortRbSealInput(s, f, v[s]);
            }
            st.frontier = r.firstFrame + r.frames.length;
            st.sealedApplied += r.frames.length;
        }

        function pump() {
            var i;
            for (i = 0; i < evQueue.length; i++) schedule(evQueue[i]);
            evQueue.length = 0;
            for (i = 0; i < queue.length; i++) applyFrames(queue[i]);
            queue.length = 0;
        }

        /* --- start: the boot every peer runs locally, then the session --------- */

        function begin(params, events, frames) {
            var F0 = params.inputStart;
            if (!World || World.F0 !== F0)
                log('[net-game] warning: room inputStart ' + F0 + ' vs boot F0 ' + (World && World.F0));
            st.inputStart = F0;
            st.hashEvery = params.hashEvery;
            st.nextCheck = F0 + params.hashEvery;
            /* The spawn window follows the session's `view` events from here
             * (platform/view.c), not this window -- before frame 0 runs, so
             * no local request ever reached the input log. */
            if (Module._PortSimViewUseSession) Module._PortSimViewUseSession();
            else if (Module._PortSimViewPinNative) Module._PortSimViewPinNative();
            if (!Module._PortRbInit(DEPTH, ENGINE_SEATS))
                throw new Error('PortRbInit failed');
            if (Module._PortRbSetStallGate) Module._PortRbSetStallGate(0);
            Module._PortRbSetSelf(st.seat >= 0 && st.seat < ENGINE_SEATS ? st.seat : -1);
            /* The boot: a single-player world (slots 1-3 the AI's), the
             * tutorial skipped, four Kirbys and the network branch at F0 --
             * the same timeline web/rb_boot.js schedules, so the same world.
             * Every slot goes to the AI at F0 first; the session's own slot
             * events at F0 (scheduled after, so applied after) seat the
             * founders. */
            for (var k = 1; k < ENGINE_SEATS; k++) Module._PortRbScheduleEvent(0, EV_SLOT, k, SLOT_AI);
            Module._PortRbScheduleEvent(0, EV_PLAYERS, 1, 0);
            Module._PortRbScheduleEvent(0, EV_STORY, 3, 0);
            Module._PortRbScheduleEvent(F0, EV_PLAYERS, 4, 0);
            Module._PortRbScheduleEvent(F0, EV_NETPLAY, 0, 0);
            for (k = 0; k < ENGINE_SEATS; k++) Module._PortRbScheduleEvent(F0, EV_SLOT, k, SLOT_AI);
            var bm = World.bootMask;
            for (var f = 0; f < F0; f++) {
                var m = bm(f);
                Module._PortRbConfirmInput(0, f, m);
                if (st.seat > 0 && st.seat < ENGINE_SEATS) Module._PortRbConfirmInput(st.seat, f, m);
            }
            st.phase = 'live';
            for (var e = 0; e < events.length; e++) schedule(events[e]);
            for (var i = 0; i < frames.length; i++) applyFrames(frames[i]);
            pump();
            snapSize = Module._PortRbSnapshotBytes ? Module._PortRbSnapshotBytes() >>> 0 : 0;
            if (snapSize) snapPtr = Module._malloc(snapSize);
            Module._PortRbReplayTo(Math.max(F0, st.frontier));
            status('replaying to frame ' + Math.max(F0, st.frontier));
        }

        /* --- per presented frame ------------------------------------------------ */

        function tick() {
            if (st.phase !== 'live')
                return;
            pump();
            if (catching())
                return;
            var now = engineFrame();
            /* Overtaken by the sealed stream: run to it (and a little past,
             * by the round trip), never wait. */
            var target = clockTarget();
            if (st.frontier > now + 1 || (target !== null && now < target - 3)) {
                var to = Math.max(st.frontier + 1, target === null ? 0 : Math.round(target));
                Module._PortRbReplayTo(to);
                st.ffwd++;
                return;
            }
            if (target !== null && now > target + 2 && st.hold === 0)
                st.hold = Math.min(6, Math.round(now - target - 1));
            /* The camera: our Kirby once our slot event has applied, else 0. */
            var want = 0;
            for (var i = 0; i < mySeatEvents.length; i++)
                if (mySeatEvents[i].f <= now) want = mySeatEvents[i].slot;
            if (now >= st.inputStart && want !== st.focus) {
                Module._PortRbNetPlay(want);
                st.focus = want;
                st.kirby = want;
            }
            /* Our input, stamped `delay` frames ahead so it reaches the room
             * before that frame's deadline (docs/design/NETPLAY-V2.md §2.3:
             * local policy, never agreed).  A raised delay fills the frames
             * it skips with the same sample; a lowered one drops a sample.
             * Either way our column is what the room seals, for everyone. */
            if (st.seat >= 0 && st.seat < ENGINE_SEATS && st.liveFrom !== Infinity) {
                var target = now + st.delay;
                var from = Math.max(st.lastStamped + 1, st.liveFrom, st.frontier, now);
                if (target >= from) {
                    var keys = getKeys() & 0x3FF, arr = [];
                    for (var f = from; f <= target && arr.length < 32; f++) {
                        Module._PortRbConfirmInput(st.seat, f, keys);
                        arr.push(keys);
                    }
                    if (net.sendInput(from, arr)) st.sent += arr.length;
                    st.lastStamped = from + arr.length - 1;
                }
            }
            checkpoints(now);
            /* A held frame: the host's pacer waits one more period before
             * the next one (browser: portFrameDue; harness: portNetHold). */
            if (st.hold > 0 && (now % 10) === 0) {
                st.hold--;
                st.held++;
                if (typeof Module.portFrameDue === 'number') Module.portFrameDue += 1000 / 59.7275;
                Module.portNetHold = (Module.portNetHold || 0) + 1;
            }
            if (Module._PortSimViewRequested && (now & 63) === 0) {
                var v = Module._PortSimViewRequested() >>> 0;
                if (v !== st.lastView) {
                    st.lastView = v;
                    net.view(v >>> 16, v & 0xFFFF);
                }
            }
        }

        function checkpoints(now) {
            if (!snapPtr) return;
            while (st.nextCheck < st.frontier + 1 && st.nextCheck < now) {
                var C = st.nextCheck;
                if (now - C > DEPTH - 2) {          /* fell out of the ring */
                    st.nextCheck += st.hashEvery;
                    continue;
                }
                if (!Module._PortRbSnapshotAt(C, snapPtr))
                    return;                          /* not final yet; next frame */
                var snap = Module.HEAPU8.slice(snapPtr, snapPtr + snapSize);
                var h = simHash(snap);
                copies.set(C, snap);
                hashOf.set(C, h);
                while (copies.size > KEEP_CHECKPOINTS) {
                    var oldest = copies.keys().next().value;
                    copies.delete(oldest);
                    hashOf.delete(oldest);
                }
                net.sendHash(C, h);
                st.hashes++;
                if (opts.onHash) opts.onHash(C, h);
                st.nextCheck += st.hashEvery;
            }
        }

        /* --- world sync ----------------------------------------------------------- */

        net.on('sync_req', function (m) {
            var mine = copies.get(m.f);
            net.send(mine ? { t: 'sync_have', id: m.id, f: m.f, ps: PAGE,
                              hashes: b64(new Uint8Array(pageHashes(mine).buffer)) }
                          : { t: 'sync_have', id: m.id, f: m.f });
        });

        net.on('snap_req', function (m) {
            var mine = copies.get(m.f);
            if (!mine) return net.send({ t: 'snap_fail', id: m.id, why: 'checkpoint ' + m.f + ' no longer held' });
            var theirs = null;
            if (m.hashes && m.ps === PAGE) {
                var u = unb64(m.hashes);
                theirs = new Uint32Array(u.buffer, u.byteOffset, u.byteLength >> 2);
            }
            buildBlob(mine, m.f, hashOf.get(m.f), theirs).then(function (b) {
                net.sendSnapshot(m.id, m.f, b.blob, hashOf.get(m.f), theirs ? 'pages' : 'full');
            }, function (e) {
                net.send({ t: 'snap_fail', id: m.id, why: String(e.message || e) });
            });
        });

        net.on('snapshot', function (meta, blob) {
            if (!snapPtr) return net.send({ t: 'sync_done', id: meta.id, ok: false, why: 'no engine' });
            applyBlob(blob, copies.get(meta.f), snapSize).then(function (r) {
                var h = simHash(r.snap);
                if (meta.h && h !== meta.h)
                    throw new Error('rebuilt state hashes ' + h + ', donor said ' + meta.h);
                Module.HEAPU8.set(r.snap, snapPtr);
                if (!Module._PortRbApplySnapshotAt(meta.f, snapPtr))
                    throw new Error('engine refused the snapshot');
                /* Our copies after the sync frame describe a past that no
                 * longer happened. */
                copies.forEach(function (_, f) { if (f > meta.f) { copies.delete(f); hashOf.delete(f); } });
                copies.set(meta.f, r.snap);
                hashOf.set(meta.f, h);
                if (st.nextCheck <= meta.f) st.nextCheck = meta.f + st.hashEvery;
                st.syncs++;
                st.syncPages += r.pages;
                st.syncBytes += blob.length;
                var info = { frame: meta.f, kind: 'sync', pages: r.pages, bytes: blob.length,
                             reason: meta.reason, at: engineFrame() };
                st.corrections.push(info);
                if (st.corrections.length > 32) st.corrections.shift();
                net.send({ t: 'sync_done', id: meta.id, ok: true, pages: r.pages });
                correction(info);
            }).catch(function (e) {
                log('[net-game] sync failed: ' + e.message);
                net.send({ t: 'sync_done', id: meta.id, ok: false, why: String(e.message) });
            });
        });

        /* The presentation seam: "a correction happened at frame F".  The
         * renderer's interpolation (branch smooth) blends camera and sprites
         * across it instead of snapping. */
        function correction(info) {
            if (opts.onCorrection) opts.onCorrection(info);
            if (Module.portNetCorrection) {
                try { Module.portNetCorrection(info); } catch (e) { /* presentation only */ }
            }
            if (typeof window !== 'undefined' && window.dispatchEvent && typeof CustomEvent !== 'undefined')
                window.dispatchEvent(new CustomEvent('katam-net-correction', { detail: info }));
        }

        /* --- wiring ---------------------------------------------------------------- */

        net.on('frames', function (r) { queue.push(r); });
        /* Adaptive input delay: the room counts our inputs that missed their
         * deadline; one step a second at most, up on lateness, down after
         * five quiet seconds, never below what the round trip needs. */
        net.on('stats', function (m) {
            var t = Date.now();
            var floor = Math.max(1, Math.ceil(((net.rtt || 0) / 2 - (net.params ? net.params.deadlineMs : 100) + 20) / 16.74));
            if (m.late > st.lastLate) {
                st.lastLate = m.late;
                st.lastLateAt = t;
                if (st.delay < 12) st.delay++;
            } else if (t - st.lastLateAt > 5000 && st.delay > floor && st.delay > 1) {
                st.delay--;
                st.lastLateAt = t;
            }
            if (st.delay < floor) st.delay = floor;
            if (typeof m.clock === 'number') {
                st.clock = m.clock;
                st.clockAt = t;
                st.clockRtt = net.rtt || 0;
            }
        });

        /* Time sync against the room's frame clock (netplay/session.mjs,
         * clockFrame).  Our present should lead the room's clock by our
         * one-way latency, less the input delay, plus a frame of margin:
         * then our stamped inputs reach the room before their frames are
         * due.  Behind that: fast-forward.  Ahead of it: hold a frame now
         * and then (rate dilation, at most 6 a second). */
        function clockTarget() {
            if (st.clock === null) return null;
            var P = 1000 / 59.7275;
            var est = st.clock + (st.clockRtt / 2 + (Date.now() - st.clockAt)) / P;
            return est + Math.ceil(st.clockRtt / 2 / P) - st.delay + 2;
        }
        net.on('event', function (ev) { evQueue.push(ev); });
        net.on('roster', function () {
            if (net.seat !== st.seat && st.phase !== 'live') st.seat = net.seat;
            else if (net.seat !== st.seat && st.phase === 'live') {
                /* Seated mid-session (SIT after the archive). */
                st.seat = net.seat;
                if (st.seat >= 0 && st.seat < ENGINE_SEATS) Module._PortRbSetSelf(st.seat);
            }
        });

        var archive = null;
        var maxSeq = 0;
        net.on('archive_begin', function (m) { archive = { begin: m, frames: [] }; });
        net.on('archive', function (r) { if (archive) archive.frames.push(r); });
        net.on('event', function (ev) { if (ev.seq > maxSeq) maxSeq = ev.seq; });

        begun = new Promise(function (resolve, reject) {
            function go(params, events, frames) {
                try {
                    st.seat = net.seat;
                    for (var i = 0; i < events.length; i++) if (events[i].seq > maxSeq) maxSeq = events[i].seq;
                    begin(params, events, frames);
                    resolve();
                } catch (e) { reject(e); }
            }
            net.on('start', function (m) { if (st.phase === 'idle') go(m.params, m.events, []); });
            net.on('archive_end', function () {
                if (!archive) return;
                var a = archive;
                archive = null;
                if (st.phase === 'idle') {
                    go(net.params, a.begin.events, a.frames);
                } else {
                    /* A reconnect while live: sealing the archive again is
                     * idempotent; only events we never saw are new. */
                    for (var i = 0; i < a.begin.events.length; i++)
                        if (a.begin.events[i].seq > maxSeq) {
                            maxSeq = a.begin.events[i].seq;
                            evQueue.push(a.begin.events[i]);
                        }
                    for (i = 0; i < a.frames.length; i++) queue.push(a.frames[i]);
                }
                if (opts.want === 'play') net.sit();
            });
            /* Late, or back after a lost connection: the archive is the way
             * in (log replay from the boot today; netsim's snapshot join
             * replaces the replay, not the message). */
            net.on('welcome', function (m) {
                if (m.phase === 'playing') net.requestArchive();
            });
            /* Created after the welcome (the wasm took longer to start than
             * the socket): ask now. */
            if (net.mid >= 0 && net.phase === 'playing') net.requestArchive();
        });

        var statsPtr = 0;
        function engineStats() {
            if (!Module._PortRbGetStats) return null;
            if (!statsPtr) statsPtr = Module._malloc(32);
            Module._PortRbGetStats(statsPtr);
            var u = new Uint32Array(Module.HEAPU8.buffer, statsPtr, 8);
            return { frame: u[0], confirmed: u[1], rollbacks: u[2], resimFrames: u[3], deepest: u[4],
                     lateDrops: u[5], predictions: u[6], mispredictions: u[7] };
        }

        return {
            state: st, ready: begun, tick: tick, idle: pump, engineStats: engineStats,
            copies: copies, simHash: simHash,
            corrupt: function (addr, value) { Module.HEAPU8[addr] = value; },
        };
    }

    createKatamNetGame.simHash = simHash;
    createKatamNetGame.pageHashes = pageHashes;
    createKatamNetGame.buildBlob = buildBlob;
    createKatamNetGame.applyBlob = applyBlob;
    return createKatamNetGame;
}));
