import test from 'node:test';
import assert from 'node:assert/strict';
import {
    CODE_ALPHABET, CODE_SPACE, normalizeCode, isValidCode, explainCode, codeFromIndex,
    CodeDirectory, RateLimiter, seededRng, isBlocked,
} from '../codes.mjs';
import { DirectoryService } from '../directory.mjs';
import { Clock } from './harness.mjs';

test('alphabet: 19 consonants, no vowels/Y/L, 130321 codes', () => {
    assert.equal(CODE_ALPHABET.length, 19);
    assert.ok(!/[AEIOUYL0-9]/.test(CODE_ALPHABET));
    assert.equal(CODE_SPACE, 130321);
});

test('normalise: case, spacing, look-alikes, pasted links', () => {
    assert.equal(normalizeCode('bcdf'), 'BCDF');
    assert.equal(normalizeCode(' b-c d.f '), 'BCDF');
    assert.equal(normalizeCode('8CDF'), 'BCDF');       // 8 -> B
    assert.equal(normalizeCode('2X5G'), 'ZXSG');       // 2 -> Z, 5 -> S
    assert.equal(normalizeCode('ruvw'), 'RVVW');       // U -> V
    assert.equal(normalizeCode('https://x.y/play/?room=qrst&x=1'), 'QRST');
    assert.equal(normalizeCode('ABCD'), null);         // vowel
    assert.equal(normalizeCode('BCD'), null);
    assert.equal(normalizeCode('BCDFG'), null);
    assert.match(explainCode('BC'), /2 more letters/);
    assert.match(explainCode('BCDA'), /never contain A/);
    assert.equal(explainCode('bcdf'), null);
});

test('blocklist: never valid, never allocated', () => {
    assert.ok(isBlocked('FVCK'));
    assert.equal(normalizeCode('fuck'), null);         // U->V, then blocked
    assert.ok(!isValidCode('XKKK'));
    let blocked = 0;
    for (let i = 0; i < CODE_SPACE; i++) if (isBlocked(codeFromIndex(i))) blocked++;
    assert.ok(blocked < 400, `blocked ${blocked}`);   // 269: 0.2% of the space
});

test('directory: random allocation, unique, no collisions over many rooms', () => {
    const clock = new Clock();
    const d = new CodeDirectory({ now: clock.now, rng: seededRng(3) });
    const seen = new Set();
    for (let i = 0; i < 5000; i++) {
        const a = d.allocate();
        assert.ok(isValidCode(a.code));
        assert.ok(!seen.has(a.code));
        seen.add(a.code);
        assert.equal(a.roomId.length, 32);
        assert.equal(a.resumeToken.length, 48);
    }
    assert.equal(d.size, 5000);
    // consecutive codes are not sequential
    const arr = [...seen].slice(0, 10);
    assert.ok(new Set(arr.map((c) => c[0])).size > 3);
});

test('directory: a code is reused only after its room is empty and expired', () => {
    const clock = new Clock();
    const d = new CodeDirectory({ now: clock.now, rng: seededRng(5), idleMs: 20 * 60e3 });
    const a = d.allocate();
    d.touch(a.roomId, 2);
    clock.advance(3600e3);                     // in use for an hour: still live
    assert.equal(d.resolve(a.code).roomId, a.roomId);
    d.touch(a.roomId, 0);                       // everyone left
    clock.advance(19 * 60e3);
    assert.ok(d.resolve(a.code), 'still within the idle grace');
    d.touch(a.roomId, 1);                       // someone came back
    d.touch(a.roomId, 0);
    clock.advance(19 * 60e3);
    assert.ok(d.resolve(a.code));
    clock.advance(2 * 60e3);
    assert.equal(d.resolve(a.code), null, 'expired');
    d.sweep();
    assert.equal(d.size, 0);
    // never-joined rooms expire too
    const b = d.allocate();
    clock.advance(21 * 60e3);
    assert.equal(d.resolve(b.code), null);
});

test('directory: exhaustion falls back to a scan and reclaims expired codes', () => {
    const clock = new Clock();
    const d = new CodeDirectory({ now: clock.now, rng: seededRng(9), idleMs: 1000 });
    // Fill all but a few codes, live.
    let n = 0;
    for (;;) { const a = d.allocate({ persist: false }); if (!a) break; d.touch(a.roomId, 1); n++; }
    assert.ok(n > 130000, `allocated ${n}`);
    assert.equal(d.allocate(), null);
    // Empty one room; after expiry its code is the one handed out.
    const [code, e] = d.codes.entries().next().value;
    d.touch(e.roomId, 0);
    clock.advance(1001);
    const again = d.allocate({ persist: false });
    assert.equal(again.code, code);
    assert.notEqual(again.roomId, e.roomId);
});

test('directory: resume token rebinds a persisted room to a fresh code, never the old one by guess', () => {
    const clock = new Clock();
    const d = new CodeDirectory({ now: clock.now, rng: seededRng(11), idleMs: 1000 });
    const a = d.allocate();
    d.touch(a.roomId, 1); d.touch(a.roomId, 0);
    clock.advance(5000);
    assert.equal(d.resolve(a.code), null);
    const r = d.resumeRoom(a.resumeToken);
    assert.equal(r.roomId, a.roomId);
    assert.ok(isValidCode(r.code));
    assert.equal(d.resolve(r.code).roomId, a.roomId);
    assert.equal(d.resumeRoom('x'.repeat(48)), null);
    clock.advance(8 * 24 * 3600e3);
    d.sweep();
    assert.equal(d.resumeRoom(a.resumeToken), null, 'expired after 7 days idle');
    // persistence roundtrip
    const d2 = new CodeDirectory({ now: clock.now });
    const b = d.allocate();
    d2.load(JSON.parse(JSON.stringify(d.toJSON())));
    assert.equal(d2.resolve(b.code).roomId, b.roomId);
});

test('rate limiter: token bucket with refill', () => {
    const clock = new Clock();
    const l = new RateLimiter({ capacity: 3, refillPerSec: 1, now: clock.now });
    assert.ok(l.take('a') && l.take('a') && l.take('a'));
    assert.ok(!l.take('a'));
    assert.ok(l.take('b'), 'per key');
    assert.equal(l.retryAfter('a'), 1);
    clock.advance(1000);
    assert.ok(l.take('a'));
    assert.ok(!l.take('a'));
    clock.advance(10000);
    l.sweep();
    assert.equal(l.buckets.has('a'), false);
});

test('edge: brute force is bounded per IP; misses cost more than hits', () => {
    const clock = new Clock();
    const svc = new DirectoryService({ now: clock.now, rng: seededRng(2) });
    const c = svc.create('1.1.1.1');
    assert.equal(c.status, 201);
    // Guessing from one IP: 6 misses, then 429 with Retry-After.
    let ok404 = 0, limited = 0;
    for (let i = 0; i < 50; i++) {
        const r = svc.lookup(codeFromIndex(i * 7 + 1), '6.6.6.6');
        if (r.status === 404) ok404++;
        if (r.status === 429) { limited++; assert.ok(Number(r.headers['Retry-After']) >= 1); }
    }
    assert.equal(ok404, 6);
    assert.equal(limited, 44);
    // Another IP with the right code is unaffected; a real player retrying is fine.
    for (let i = 0; i < 20; i++)
        assert.equal(svc.lookup(c.body.code.toLowerCase(), '2.2.2.2').status, 200);
    // Over an hour a guesser gets ~6 + 3600*0.05 = 186 misses.
    let misses = 0;
    for (let s = 0; s < 3600; s++) {
        clock.advance(1000);
        const r = svc.lookup(codeFromIndex((s * 13) % 130321), '6.6.6.6');
        if (r.status === 404) misses++;
    }
    assert.ok(misses <= 190, `misses ${misses}`);
    // creates are limited too
    let made = 0;
    for (let i = 0; i < 20; i++) if (svc.create('3.3.3.3').status === 201) made++;
    assert.equal(made, 6);
});
