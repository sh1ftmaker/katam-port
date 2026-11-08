import test from 'node:test';
import assert from 'node:assert/strict';
import { makeRoom, Clock } from './harness.mjs';
import { electDonor, sessionViewOf, PROTO_VERSION, SessionCore } from '../session.mjs';
import { encodeChunk } from '../wire.mjs';

const S = 2200;    // inputStart

function lobby(n, params = {}) {
    const room = makeRoom({ hashEvery: 30, ...params });
    const cs = [];
    for (let i = 0; i < n; i++) cs.push(room.client('p' + i, { want: 'play' }));
    return { room, cs };
}

function started(n, params) {
    const { room, cs } = lobby(n, params);
    cs[0].sendJson({ t: 'start' });
    return { room, cs };
}

test('version gate: proto, sim, rom, content, seats -- with the field named', () => {
    const room = makeRoom();
    const a = room.client('host');
    assert.equal(a.welcome.role, 'spectator');
    for (const [k, v] of [['sim', 'other'], ['rom', 'x'], ['content', 'mod'], ['proto', 1]]) {
        const b = room.client('b' + k, { [k]: v });
        assert.equal(b.error.code, 'version', k);
        assert.equal(b.error.field, k);
        assert.equal(b.open, false);
    }
    const c = room.client('few', { caps: { seats: 2, sync: true } });
    assert.equal(c.error.field, 'seats');
});

test('lobby: seats, ready, roster, host, lock, approval, kick', () => {
    const { room, cs } = lobby(2);
    const [h, b] = cs;
    assert.equal(h.roster.host, h.mid);
    assert.deepEqual(h.roster.members.map((m) => m.seat), [0, 1]);
    b.sendJson({ t: 'ready', ready: true });
    assert.equal(h.roster.members[1].ready, true);
    // non-host cannot lock or kick
    b.sendJson({ t: 'lock', on: true });
    assert.equal(h.roster.locked, false);
    h.sendJson({ t: 'lock', on: true });
    assert.equal(h.roster.locked, true);
    const x = room.client('stranger');
    assert.equal(x.error.code, 'locked');
    // a known identity may come back while locked
    b.disconnect();
    const b2 = room.client('p1', { want: 'play' });
    assert.ok(!b2.error);
    assert.equal(b2.mid, b.mid);
    h.sendJson({ t: 'lock', on: false });
    h.sendJson({ t: 'approval', on: true });
    const y = room.client('asker');
    assert.equal(y.welcome.role, 'pending');
    assert.equal(h.last('join_req').mid, y.mid);
    y.sendJson({ t: 'sit' });                     // ignored while pending
    assert.equal(h.roster.members.find((m) => m.mid === y.mid).seat, -1);
    h.sendJson({ t: 'approve', mid: y.mid });
    assert.equal(y.last('welcome').role, 'spectator');
    const z = room.client('denied');
    h.sendJson({ t: 'deny', mid: z.mid });
    assert.equal(z.error.code, 'denied');
    // kick: banned for the room's life
    h.sendJson({ t: 'kick', mid: b2.mid });
    assert.equal(b2.error.code, 'kicked');
    assert.ok(!h.roster.members.some((m) => m.mid === b2.mid));
    h.sendJson({ t: 'approval', on: false });
    const b3 = room.client('p1');
    assert.equal(b3.error.code, 'kicked');
});

test('lobby: a full room watches; seats free on leave; host passes on after grace', () => {
    const { room, cs } = lobby(5);
    assert.equal(cs[4].last('notice').code, 'seats-full');
    assert.equal(cs[4].me.role, 'spectator');
    cs[1].disconnect();
    assert.equal(cs[0].roster.members.find((m) => m.mid === cs[1].mid)?.seat ?? -1, -1);
    cs[4].sendJson({ t: 'sit' });
    assert.equal(cs[4].me.seat, 1);
    cs[0].disconnect();                            // the host
    room.run(5000);
    assert.equal(cs[2].roster.host, cs[0].mid, 'grace: host kept while away');
    room.run(6000);
    assert.equal(cs[2].roster.host, cs[2].mid, 'longest-seated connected member');
});

test('start: seat, slot and view events at inputStart; everyone streams', () => {
    const { room, cs } = started(3);
    const st = cs[2].last('start');
    assert.equal(st.frame, S);
    const kinds = st.events.map((e) => `${e.kind}@${e.f}`);
    assert.deepEqual(kinds, ['seat@2200', 'slot@2200', 'seat@2200', 'slot@2200', 'seat@2200', 'slot@2200', 'view@2200']);
    assert.deepEqual(st.events.filter((e) => e.kind === 'slot').map((e) => [e.slot, e.seat]), [[0, 0], [1, 1], [2, 2]]);
});

test('deadline sequencer: seal on all-arrived, else at the deadline with repeat-last; late inputs dropped', () => {
    const { room, cs } = started(2, { deadlineMs: 100 });
    const [a, b] = cs;
    a.input(S, [1]);
    assert.equal(a.sealed.size, 0, 'waits for b');
    b.input(S, [2]);
    assert.deepEqual([...a.sealed.get(S)], [1, 2, 0, 0]);
    assert.deepEqual([...b.sealed.get(S)], [1, 2, 0, 0]);
    // b goes quiet: S+1 is due one frame after S (the session clock) and
    // seals deadlineMs after that; b repeats 2
    a.input(S + 1, [5]);
    room.run(110);
    assert.ok(!a.sealed.has(S + 1));
    room.run(10);
    assert.deepEqual([...a.sealed.get(S + 1)], [5, 2, 0, 0]);
    // b's late input for S+1 is discarded; it does not rewrite history
    b.input(S + 1, [9]);
    assert.deepEqual([...b.sealed.get(S + 1)], [5, 2, 0, 0]);
    assert.equal(room.core.stats.lateInputs, 1);
    // ... but its newest value is what fills next time ("repeat last")
    a.input(S + 2, [5]);
    room.run(105);
    assert.deepEqual([...a.sealed.get(S + 2)], [5, 9, 0, 0]);
    // nothing sealed later than the deadline past its due time
    assert.ok(room.core.stats.sealLatencyMax <= 105);
    // the clock, not the first sender, decides: a far-ahead stamp does not
    // shorten the deadline of the frames before it
    a.input(S + 3, [5]);
    a.input(S + 40, [5]);
    assert.ok(!a.sealed.has(S + 3) || room.clock.t >= room.core._dueAt(S + 3) + 100);
    // both clients hold identical sealed streams
    for (const [f, v] of a.sealed) assert.deepEqual([...b.sealed.get(f)], [...v]);
});

test('watermark: no event is ever stamped below a watermark already sent', () => {
    const { room, cs } = started(2);
    const [a, b] = cs;
    let f = S;
    for (let i = 0; i < 50; i++, f++) { a.input(f, [0]); b.input(f, [0]); }
    const wm = a.wm;
    assert.ok(wm >= f + 30 - 1);
    const c = room.client('late');
    c.sendJson({ t: 'archive' });
    c.sendJson({ t: 'sit' });
    const evs = a.events.filter((e) => e.seq > 5);
    assert.ok(evs.length >= 2);
    for (const e of evs) assert.ok(e.f >= wm, `${e.kind}@${e.f} < wm ${wm}`);
});

test('a disconnected seat never delays the others; Kirby to the AI; reconnect reclaims seat and Kirby', () => {
    const { room, cs } = started(2, { dropToAiMs: 1500, reconnectGraceMs: 60000 });
    const [a, b] = cs;
    a.input(S, [1]); b.input(S, [1]);
    b.disconnect();
    a.input(S + 1, [1]);
    assert.ok(a.sealed.has(S + 1), 'sealed at once: b is not expected');
    assert.equal(a.sealed.get(S + 1)[1], 0, 'a gone seat reads neutral');
    room.run(1600);
    const ev = a.events.filter((e) => e.kind === 'slot' && e.seat === -1);
    assert.equal(ev.length, 1);
    assert.equal(ev[0].slot, 1);
    assert.ok(a.events.some((e) => e.kind === 'unseat' && e.seat === 1));
    // back within grace: archive, sit, same seat and Kirby
    const b2 = room.client('p1');
    assert.equal(b2.mid, b.mid);
    b2.sendJson({ t: 'archive' });
    b2.sendJson({ t: 'sit' });
    const back = a.events.filter((e) => e.kind === 'slot' && e.seat === 1);
    assert.equal(back.at(-1).slot, 1);
    assert.equal(b2.me.seat, 1);
});

test('late joiner: archive reproduces the sealed stream bit for bit, then the live stream continues', () => {
    const { room, cs } = started(2);
    const [a, b] = cs;
    let f = S;
    const rnd = (i) => (i * 2654435761 >>> 22) & 0x3FF;
    for (let i = 0; i < 9000; i++, f++) {
        a.input(f, [i % 97 < 10 ? rnd(i) : 3]);
        if (i % 3) b.input(f, [rnd(i + 1) & 0x30]);
        else room.run(101, 101);
    }
    const j = room.client('joiner');
    j.sendJson({ t: 'archive' });
    assert.equal(j.last('archive_end').to, f);
    for (let k = 0; k < 20; k++, f++) { a.input(f, [k]); b.input(f, [k]); }
    assert.equal(j.sealed.size, a.sealed.size);
    for (const [fr, v] of a.sealed) assert.deepEqual([...j.sealed.get(fr)], [...v], `frame ${fr}`);
    // archive is compact: change records only
    assert.ok(room.core.archive.length < 9000 * 2);
});

test('hash referee: majority wins, minority is resynced from an elected donor', () => {
    const { room, cs } = started(3);
    const [a, b, c] = cs;
    let f = S;
    for (; f < S + 61; f++) for (const x of cs) x.input(f, [0]);
    a.sendJson({ t: 'hash', f: S + 30, h: 'aaaa' });
    b.sendJson({ t: 'hash', f: S + 30, h: 'aaaa' });
    c.sendJson({ t: 'hash', f: S + 30, h: 'cccc' });
    const d = a.last('desync');
    assert.equal(d.f, S + 30);
    assert.equal(d.ref, 'aaaa');
    const req = c.last('sync_req');
    assert.ok(req, 'minority asked to sync');
    assert.ok([a.mid, b.mid].includes(req.donor));
    assert.equal(req.f, S + 30);
    assert.equal(req.reason, 'mismatch');
    assert.equal(room.core.stats.desyncs, 1);
});

test('hash referee: two players disagreeing -- the tie goes to the longer clean history', () => {
    const { room, cs } = started(2);
    const [a, b] = cs;
    for (let f = S; f < S + 121; f++) { a.input(f, [0]); b.input(f, [0]); }
    a.sendJson({ t: 'hash', f: S + 30, h: '1' }); b.sendJson({ t: 'hash', f: S + 30, h: '1' });
    a.sendJson({ t: 'hash', f: S + 60, h: '2' });
    room.run(1600);                               // b never reports 60: a alone is clean
    a.sendJson({ t: 'hash', f: S + 90, h: '3' }); b.sendJson({ t: 'hash', f: S + 90, h: 'X' });
    assert.equal(a.last('desync').ref, '3');
    assert.equal(b.last('sync_req').donor, a.mid);
});

test('donor election policies', () => {
    const m = (mid, clean, rtt, seatedAt) => ({ mid, cleanStreak: clean, rtt, seatedAt });
    const c = [m(1, 5, 80, 10), m(2, 9, 120, 20), m(3, 9, 40, 30)];
    assert.equal(electDonor(c, 'majority').mid, 3);
    assert.equal(electDonor(c, 'rtt').mid, 3);
    assert.equal(electDonor([m(1, 9, 80, 1), m(2, 1, 20, 2)], 'rtt').mid, 2);
    assert.equal(electDonor([m(1, 9, 80, 1), m(2, 1, 20, 2)], 'majority').mid, 1);
    assert.equal(electDonor(c, 'host', 1).mid, 1);
    assert.equal(electDonor([], 'majority'), null);
});

test('sync relay: have -> snap_req -> snap + chunks relayed -> done; bytes counted', () => {
    const { room, cs } = started(2);
    const [a, b] = cs;
    for (let f = S; f < S + 31; f++) { a.input(f, [0]); b.input(f, [0]); }
    a.sendJson({ t: 'hash', f: S + 30, h: 'ok' });
    b.sendJson({ t: 'hash', f: S + 30, h: 'bad' });
    // tie broken by clean history: both 0 -> rtt -> seatedAt; give a an rtt
    const tgt = [a, b].find((x) => x.last('sync_req'));
    const donor = tgt === a ? b : a;
    const req = tgt.last('sync_req');
    tgt.sendJson({ t: 'sync_have', id: req.id, f: req.f, ps: 4096, hashes: 'AAAA' });
    const sr = donor.last('snap_req');
    assert.equal(sr.to, tgt.mid);
    assert.equal(sr.hashes, 'AAAA');
    const payload = new Uint8Array(40000).fill(7);
    donor.sendJson({ t: 'snap', id: sr.id, f: sr.f, size: payload.length, count: 3, h: 'ok', kind: 'pages' });
    for (let i = 0; i < 3; i++)
        room.core.message(donor.id, encodeChunk(sr.id, i, 3, payload.subarray(i * 16384, (i + 1) * 16384)));
    assert.equal(tgt.last('snap').count, 3);
    assert.equal(tgt.chunks.length, 3);
    // a chunk from someone else for this transfer is ignored
    room.core.message(tgt.id, encodeChunk(sr.id, 0, 3, payload.subarray(0, 10)));
    assert.equal(tgt.chunks.length, 3);
    tgt.sendJson({ t: 'sync_done', id: req.id, ok: true });
    assert.equal(room.core.stats.syncs, 1);
    assert.ok(room.core.stats.syncBytes > 40000);
});

test('sync triggers: join, lag, periodic, ask; timeout cancels; rate floor', () => {
    const { room, cs } = started(2, { syncEveryMs: 4000, lagSyncFrames: 60, minSyncGapMs: 2000,
                                      joinSyncDelayMs: 1000, syncTimeoutMs: 3000 });
    const [a, b] = cs;
    let f = S;
    const seated = [a, b];
    const play = (n) => { for (let i = 0; i < n; i++, f++) for (const x of seated) x.input(f, [0]); };
    const hash = (x, fr) => x.sendJson({ t: 'hash', f: fr, h: 'h' + fr });
    play(31); hash(a, S + 30); hash(b, S + 30);
    // join: a late spectator sits, does not report, gets a sync after the delay
    const j = room.client('joiner');
    j.sendJson({ t: 'archive' }); j.sendJson({ t: 'sit' });
    seated.push(j);
    room.run(500);
    assert.ok(!j.last('sync_req'));
    room.run(700);
    assert.equal(j.last('sync_req').reason, 'join');
    // timeout: nobody answers -> cancel to both
    room.run(3100);
    assert.equal(j.last('sync_cancel').why, 'timeout');
    // lag: b stops reporting while a goes on
    for (let k = 2; k <= 5; k++) { play(30); hash(a, S + 30 * k); hash(j, S + 30 * k); }
    room.run(3500);   // judged without b after hashWaitMs; b is first busy donating to a's periodic sync, which times out
    assert.equal(b.last('sync_req')?.reason, 'lag');
    // periodic: within syncEveryMs, a non-donor gets one -- never the donor
    room.run(4500);
    const periodic = [a, b, j].filter((x) => x.all('sync_req').some((r) => r.reason === 'periodic'));
    assert.ok(periodic.length >= 1);
    for (const x of periodic)
        for (const r of x.all('sync_req').filter((q) => q.reason === 'periodic'))
            assert.notEqual(r.donor, x.mid);
});

test('session view: max of seated requests, rounded to 32 px cells, clamped; event in play', () => {
    assert.deepEqual(sessionViewOf([{ w: 300, h: 170 }, { w: 400, h: 100 }, null]), { w: 416, h: 192 });
    assert.deepEqual(sessionViewOf([{ w: 5000, h: 5000 }]), { w: 1024, h: 512 });
    const { room, cs } = started(2);
    cs[1].sendJson({ t: 'view', w: 480, h: 270 });
    const v = cs[0].events.filter((e) => e.kind === 'view').at(-1);
    assert.deepEqual([v.w, v.h], [480, 288]);
    assert.ok(v.f >= S + 30);
});

test('scale: 100 seats -- sealed at the deadline when one is slow, identical streams, compact frames', () => {
    const room = makeRoom({ seats: 100, deadlineMs: 50, hashEvery: 30 });
    const cs = [];
    for (let i = 0; i < 100; i++) cs.push(room.client('p' + i, { want: 'play', caps: { seats: 128, sync: true } }));
    room.run(300);                                // rosters coalesce in big rooms
    assert.equal(room.core.params.seats, 100);
    assert.equal(room.core.params.kirbys, 4);
    assert.ok(cs.every((c) => c.me.seat >= 0));
    cs[0].sendJson({ t: 'start' });
    const before = cs[0].bin.reduce((n, b) => n + b.length, 0);
    for (let f = S; f < S + 600; f++) {
        for (let i = 0; i < 99; i++) cs[i].input(f, [(f >> 4) + i & 0x3FF]);
        if (f % 10 === 0) cs[99].input(f, [1]);     // seat 99: one frame in ten
        room.run(17, 17);
    }
    assert.ok(cs[0].sealed.size >= 590);
    for (const [f, v] of cs[0].sealed) assert.deepEqual([...cs[57].sealed.get(f)], [...v]);
    const perFrame = (cs[0].bin.reduce((n, b) => n + b.length, 0) - before) / cs[0].sealed.size;
    assert.ok(perFrame < 13 + 99 * 2 / 16 + 20, `bytes/frame ${perFrame}`);
    assert.ok(room.core.stats.sealLatencyMax <= 50 + 17);
});

test('persistence: export/import resumes the archive; seats come back as AI', () => {
    const { room, cs } = started(2);
    for (let f = S; f < S + 100; f++) { cs[0].input(f, [f & 3]); cs[1].input(f, [1]); }
    const blob = JSON.parse(JSON.stringify(room.core.exportState()));
    const core = new SessionCore({ send: () => {}, close: () => {} });
    core.importState(blob);
    assert.equal(core.frontier, S + 100);
    assert.equal(core.phase, 'playing');
    assert.equal(core.archive.length, room.core.archive.length);
    assert.ok(core.events.filter((e) => e.kind === 'slot' && e.seat === -1).length === 2);
});

test('a chronically late seat stops being waited for, and is waited for again once on time', () => {
    const { room, cs } = started(2, { deadlineMs: 100, excuseAfter: 20 });
    const [a, b] = cs;
    let f = S;
    // b is 30 frames behind: every one of its inputs is late
    for (let i = 0; i < 40; i++, f++) { a.input(f, [1]); room.run(17, 17); b.input(f - 30, [2]); }
    const lat0 = room.core.stats.sealLatencyMax;
    assert.ok(lat0 >= 100, 'sealed at the deadline while b was still expected');
    // excused now: a's frames seal on arrival
    const before = a.sealed.size;
    a.input(f, [1]);
    assert.equal(a.sealed.size, before + 1, 'no deadline wait');
    assert.equal(a.sealed.get(f)[1], 2, 'b repeats its newest input');
    f++;
    // b catches up (fast-forwards) and sends on time: waited for again
    // after rejoinAfter (10) on-time frames
    for (let i = 0; i < 10; i++, f++) { b.input(f, [3]); a.input(f, [1]); }
    assert.equal(a.sealed.get(f - 1)[1], 3);
    a.input(f, [1]);
    assert.ok(!a.sealed.has(f), 'waiting for b again');
});
