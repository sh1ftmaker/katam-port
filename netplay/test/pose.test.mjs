// Co-op poses: the POSE_UP/POSE_DOWN relay (session.mjs _pose) and its codec.
import test from 'node:test';
import assert from 'node:assert/strict';
import { makeRoom } from './harness.mjs';
import { POSE_RATE, POSE_BURST, FEATURES } from '../session.mjs';
import {
    encodePoseUp, poseUpOk, poseDown, decodePoseDown, decodePoseBody, T_POSE_UP, POSE_KEY,
} from '../wire.mjs';

const COOP = { sim: 'coop/1', want: 'play' };
const pose = (x, size = 36) => { const b = new Uint8Array(size); b.fill(x & 0xFF); return b; };
const up = (seq, n = 2, size = 36, flags = 0) =>
    encodePoseUp(flags, Array.from({ length: n }, (_, i) => ({ seq: seq + i, bytes: pose(seq + i, size) })));

function coop(n, params) {
    const room = makeRoom(params);
    const cs = [];
    for (let i = 0; i < n; i++) cs.push(room.client('c' + i, COOP));
    return { room, cs };
}

test('pose codec: roundtrip, size checks, mid relay', () => {
    const u = up(65535, 2, 36, POSE_KEY);
    assert.equal(u.length, 4 + 2 * 38);
    assert.ok(poseUpOk(u));
    const d = decodePoseDown(poseDown(513, u));
    assert.equal(d.from, 513);
    assert.equal(d.flags, POSE_KEY);
    assert.equal(d.size, 36);
    assert.deepEqual(d.poses.map((p) => p.seq), [65535, 0]);      // wraps
    assert.deepEqual([...d.poses[1].bytes], [...pose(0)]);
    assert.deepEqual(decodePoseBody(u.subarray(1)).poses[0].seq, 65535);
    assert.ok(!poseUpOk(u.subarray(0, u.length - 1)));
    const bad = u.slice(); bad[2] = 5;                               // n > 4
    assert.ok(!poseUpOk(bad));
    assert.ok(!poseUpOk(up(1, 1, 7)));                               // size < 8
    assert.ok(!poseUpOk(up(1, 1, 65)));                              // size > 64
    assert.ok(poseUpOk(up(1, 4, 64)) && poseUpOk(up(1, 1, 8)));
    assert.equal(decodePoseBody(new Uint8Array([0, 1, 36])), null);
});

test('welcome lists the pose feature', () => {
    const { cs } = coop(1);
    assert.deepEqual(cs[0].welcome.features, [...FEATURES]);
    assert.ok(cs[0].welcome.features.includes('pose'));
});

test('a pose goes to every other connected member, not back to the sender', () => {
    const { room, cs } = coop(3);
    const [a, b, c] = cs;
    room.run(100);
    const u = up(10);
    a.bytes(u);
    for (const x of [b, c]) {
        assert.equal(x.poses.length, 1);
        assert.equal(x.poses[0].from, a.mid);
        assert.deepEqual(x.poses[0].poses.map((p) => p.seq), [10, 11]);
    }
    assert.equal(a.poses, undefined);
    // verbatim: POSE_DOWN = 0x41, mid, then the sender's bytes from offset 1
    const raw = b.bin.at(-1);
    assert.deepEqual([...raw.subarray(3)], [...u.subarray(1)]);
    // a disconnected member gets nothing; the rest still do
    c.disconnect();
    a.bytes(up(12));
    assert.equal(b.poses.length, 2);
    assert.equal(room.core.stats.poses, 2);
});

test('wrong sizes are dropped, not fatal', () => {
    const { cs, room } = coop(2);
    const [a, b] = cs;
    a.bytes(up(1).subarray(0, 20));
    a.bytes(new Uint8Array([T_POSE_UP]));
    a.bytes(up(1, 1, 100));
    assert.equal(a.open, true);
    assert.equal(a.error, undefined);
    assert.equal(b.poses, undefined);
    assert.equal(room.core.stats.poseDrops, 3);
    a.bytes(up(5));
    assert.equal(b.poses.length, 1);
    // ...while an unknown binary type is still fatal, as before
    a.bytes(new Uint8Array([0x7F, 0, 0]));
    assert.equal(a.error.code, 'malformed');
});

test('per-member rate cap: a burst, then the refill rate', () => {
    const { room, cs } = coop(3);
    const [a, b, c] = cs;
    for (let i = 0; i < POSE_BURST + 10; i++) a.bytes(up(i));
    assert.equal(b.poses.length, POSE_BURST);
    // another member has its own bucket
    c.bytes(up(1));
    assert.equal(b.poses.length, POSE_BURST + 1);
    // 1 s at 60 msg/s: the cap lets POSE_RATE through
    let got = b.poses.length;
    for (let i = 0; i < 60; i++) { room.clock.advance(1000 / 60); a.bytes(up(i)); }
    const passed = b.poses.length - got;
    assert.ok(passed >= POSE_RATE - 1 && passed <= POSE_RATE + 1, `passed ${passed}`);
    // 30 msg/s, the client's rate, is never capped
    for (let i = 0; i < 2 * 60; i++) room.clock.advance(1000);   // refill
    got = b.poses.length;
    for (let i = 0; i < 300; i++) { room.clock.advance(1000 / 30); a.bytes(up(i)); }
    assert.equal(b.poses.length - got, 300);
});

test('pending members neither send nor receive poses', () => {
    const { room, cs } = coop(2);
    const [h, b] = cs;
    h.sendJson({ t: 'approval', on: true });
    const p = room.client('late', COOP);
    assert.equal(p.welcome.role, 'pending');
    h.bytes(up(1));
    assert.equal(p.poses, undefined);
    assert.equal(b.poses.length, 1);
    p.bytes(up(1));
    assert.equal(h.poses, undefined);
    assert.equal(b.poses.length, 1);
    h.sendJson({ t: 'approve', mid: p.mid });
    h.bytes(up(3));
    assert.equal(p.poses.length, 1);
});

test('a co-op room stays in the lobby: START is ignored, INPUT does nothing', () => {
    const { room, cs } = coop(2);
    const [a, b] = cs;
    a.sendJson({ t: 'start' });
    assert.equal(room.core.phase, 'lobby');
    assert.equal(a.last('start'), null);
    a.input(2200, [1, 2, 3]);
    room.run(500);
    assert.equal(room.core.phase, 'lobby');
    assert.equal(b.sealed.size, 0);
    assert.deepEqual(a.roster.members.map((m) => m.seat), [0, 1]);
    // ...and a lockstep client cannot walk into it (the sim gate)
    const l = room.client('lockstep', { want: 'play' });
    assert.equal(l.error.code, 'version');
    assert.equal(l.error.field, 'sim');
});

test('poses do not disturb a lockstep session (INPUT/FRAMES unaffected)', () => {
    const room = makeRoom({ hashEvery: 30 });
    const a = room.client('a', { want: 'play' }), b = room.client('b', { want: 'play' });
    a.sendJson({ t: 'start' });
    assert.equal(room.core.phase, 'playing');
    for (let f = 2200; f < 2230; f++) { a.input(f, [1]); b.input(f, [2]); a.bytes(up(f)); }
    room.run(200);
    assert.equal(b.sealed.size, 30);
    assert.equal(b.sealed.get(2229)[0], 1);
    assert.equal(b.poses.length, 20);   // the burst cap; relayed in play as well
});
