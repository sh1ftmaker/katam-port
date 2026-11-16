// The public game list (listing.mjs): publish / list / expiry / limits / the
// name sanitiser, through DirectoryService as both servers drive it, and the
// dev relay serving the same over HTTP (with and without the list).
import test from 'node:test';
import assert from 'node:assert/strict';
import { seededRng } from '../codes.mjs';
import { DirectoryService } from '../directory.mjs';
import { sanitizeName, PublicList, LIST_TTL_MS, LIST_PER_IP } from '../listing.mjs';
import { startRelay } from '../dev-relay.mjs';
import { Clock } from './harness.mjs';

function svc(clock = new Clock(), list = {}) {
    return new DirectoryService({ now: clock.now, rng: seededRng(3), list });
}
function host(d, ip = '1.1.1.1') {
    const r = d.create(ip);
    assert.equal(r.status, 201);
    return { code: r.body.code, resume: r.body.resume, roomId: r.roomId };
}
const pub = (d, h, f = {}, ip = '1.1.1.1') =>
    d.publish({ code: h.code, resume: h.resume, name: 'KIRBY', n: 1, mode: 'together', locked: false, ...f }, ip);

test('sanitiser: the player card character set, at most 8, PLAYER when empty', () => {
    assert.equal(sanitizeName('kirby'), 'KIRBY');
    assert.equal(sanitizeName('  meta   knight  '), 'META KNI');
    assert.equal(sanitizeName('<script>'), 'SCRIPT');
    assert.equal(sanitizeName('A.B-C!9'), 'A.B-C!9');
    assert.equal(sanitizeName('ÄÖÜ‮\u0000'), 'PLAYER');
    assert.equal(sanitizeName(''), 'PLAYER');
    assert.equal(sanitizeName(null), 'PLAYER');
    assert.equal(sanitizeName(42), 'PLAYER');
    assert.equal(sanitizeName('ABCDEFG HIJ'), 'ABCDEFG');      // no trailing space
    assert.equal(sanitizeName('x'.repeat(10000)), 'XXXXXXXX');
});

test('a private room is never listed; a published one is, with exactly its public fields', () => {
    const d = svc();
    const a = host(d), b = host(d, '2.2.2.2');
    assert.deepEqual(d.publicList('9.9.9.9').body.games, []);
    assert.equal(pub(d, b, { name: 'dedede', n: 2, mode: 'roam', locked: true }, '2.2.2.2').status, 200);
    const g = d.publicList('9.9.9.9').body.games;
    assert.equal(g.length, 1);
    assert.deepEqual(Object.keys(g[0]).sort(), ['age', 'code', 'full', 'locked', 'max', 'mode', 'n', 'name']);
    assert.deepEqual({ ...g[0], age: 0 }, { code: b.code, name: 'DEDEDE', n: 2, max: 4, mode: 'roam',
                                            locked: true, full: false, age: 0 });
    assert.ok(!g.some((x) => x.code === a.code));
    // nothing else is kept: no IP, no token, no room id in the listing
    const stored = JSON.stringify([...d.pub.list.rooms.values()]);
    assert.ok(!stored.includes('2.2.2.2') && !stored.includes(b.resume));
    // and none of it is in what the Directory persists
    assert.ok(!JSON.stringify(d.dir.toJSON()).includes('DEDEDE'));
});

test('publishing needs the room and its resume token', () => {
    const d = svc();
    const a = host(d), b = host(d);
    assert.equal(pub(d, { code: a.code, resume: b.resume }).status, 403);   // someone else's token
    assert.equal(pub(d, { code: a.code, resume: 'x' }).status, 403);
    assert.equal(pub(d, { code: 'ZZZZ', resume: a.resume }).status, 403);
    assert.equal(d.publish(null, '1.1.1.1').status, 400);
    assert.equal(d.publish('nope', '1.1.1.1').status, 400);
    assert.equal(d.unpublish({ code: a.code, resume: b.resume }, '1.1.1.1').status, 403);
    assert.equal(d.publicList('1.1.1.1').body.games.length, 0);
});

test('heartbeats update in place; full shows FULL; values are clamped', () => {
    const c = new Clock();
    const d = svc(c);
    const a = host(d), b = host(d);
    pub(d, a);
    c.advance(1000);
    pub(d, b, { name: 'B' });
    c.advance(1000);
    pub(d, a, { n: 4 });
    let g = d.publicList('9').body.games;
    assert.deepEqual(g.map((x) => x.code), [b.code, a.code]);            // full ones last
    assert.equal(g[1].full, true);
    pub(d, a, { n: 3 });
    g = d.publicList('9').body.games;
    assert.deepEqual(g.map((x) => x.code), [a.code, b.code]);            // listing order kept
    pub(d, a, { n: 99, mode: 'weird', locked: 'yes' });
    g = d.publicList('9').body.games.find((x) => x.code === a.code);
    assert.equal(g.n, 4); assert.equal(g.mode, 'together'); assert.equal(g.locked, false);
    pub(d, a, { n: -3 });
    assert.equal(d.publicList('9').body.games.find((x) => x.code === a.code).n, 1);
});

test('an 8-player game: its own max, n up to it, its module; a bad max is 4', () => {
    const c = new Clock();
    const d = svc(c);
    const a = host(d), b = host(d);
    pub(d, a, { n: 5, max: 8, mod: '16' });
    c.advance(1000);
    pub(d, b, { n: 4 });
    let g = d.publicList('9').body.games;
    const ga = g.find((x) => x.code === a.code), gb = g.find((x) => x.code === b.code);
    assert.deepEqual({ ...ga, age: 0 }, { code: a.code, name: 'KIRBY', n: 5, max: 8, mode: 'together', locked: false, full: false, mod: '16', age: 0 });
    assert.equal(gb.full, true); assert.equal(gb.max, 4); assert.equal('mod' in gb, false);   // a 4-player game: as before
    assert.deepEqual(g.map((x) => x.code), [a.code, b.code]);            // 5/8 is open, 4/4 is full
    pub(d, a, { n: 99, max: 8, mod: '16' });
    ga.n = d.publicList('9').body.games.find((x) => x.code === a.code).n;
    assert.equal(ga.n, 8);
    assert.equal(d.publicList('9').body.games.find((x) => x.code === a.code).full, true);
    for (const max of [3, 17, 'x', -8, 2.5])
        { pub(d, a, { n: 6, max, mod: 'w8' }); const x = d.publicList('9').body.games.find((y) => y.code === a.code);
          assert.equal(x.max, 4); assert.equal(x.n, 4); assert.equal('mod' in x, false); }
    pub(d, a, { n: 12, max: 16 });
    assert.equal(d.publicList('9').body.games.find((x) => x.code === a.code).n, 12);
});

test('expiry: unheard for the TTL, the host ends it, or the room empties', () => {
    const c = new Clock();
    const d = svc(c);
    const a = host(d), b = host(d), e = host(d, '3.3.3.3');
    pub(d, a); pub(d, b); pub(d, e, {}, '3.3.3.3');
    assert.equal(d.publicList('9').body.games.length, 3);
    c.advance(LIST_TTL_MS - 1000);
    pub(d, a);                                     // a keeps beating
    c.advance(2000);
    let g = d.publicList('9').body.games.map((x) => x.code);
    assert.ok(g.includes(a.code) && !g.includes(b.code));
    assert.equal(d.unpublish({ code: a.code, resume: a.resume }, '1.1.1.1').body.removed, true);
    g = d.publicList('9').body.games.map((x) => x.code);
    assert.ok(!g.includes(a.code));
    pub(d, e, {}, '3.3.3.3');
    d.touch(e.roomId, 2);
    assert.ok(d.publicList('9').body.games.some((x) => x.code === e.code));
    d.touch(e.roomId, 0);                          // everyone gone: off at once
    assert.ok(!d.publicList('9').body.games.some((x) => x.code === e.code));
});

test('bounds: rooms per IP, the whole list, and what one GET returns', () => {
    const c = new Clock();
    const d = svc(c, { max: 6, shown: 4 });
    const mine = [];
    for (let i = 0; i <= LIST_PER_IP; i++) { mine.push(host(d, '5.5.5.5')); c.advance(61000); }
    for (let i = 0; i < LIST_PER_IP; i++) assert.equal(pub(d, mine[i], {}, '5.5.5.5').status, 200);
    const over = pub(d, mine[LIST_PER_IP], {}, '5.5.5.5');
    assert.equal(over.status, 409);
    assert.match(over.body.msg, /your network/);
    assert.equal(pub(d, mine[0], { n: 2 }, '5.5.5.5').status, 200);  // an update is not a new one
    const others = [];
    for (let i = 0; i < 4; i++) others.push(host(d, '6.6.6.' + i));
    for (let i = 0; i < 3; i++) assert.equal(pub(d, others[i], {}, '6.6.6.' + i).status, 200);
    const all = pub(d, others[3], {}, '6.6.6.3');
    assert.equal(all.status, 409);
    assert.match(all.body.msg, /full/);
    assert.equal(d.publicList('9').body.games.length, 4);              // shown cap
});

test('rate limits: listing and publishing per IP, with Retry-After', () => {
    const c = new Clock();
    const d = svc(c);
    let r;
    for (let i = 0; i < 20; i++) assert.equal(d.publicList('7.7.7.7').status, 200);
    r = d.publicList('7.7.7.7');
    assert.equal(r.status, 429);
    assert.ok(+r.headers['Retry-After'] >= 1);
    assert.equal(d.publicList('8.8.8.8').status, 200);                // another IP is fine
    c.advance(2000);
    assert.equal(d.publicList('7.7.7.7').status, 200);
    const a = host(d, '7.7.7.7');
    for (let i = 0; i < 10; i++) assert.equal(pub(d, a, {}, '7.7.7.7').status, 200);
    assert.equal(pub(d, a, {}, '7.7.7.7').status, 429);
    c.advance(10000);
    assert.equal(pub(d, a, {}, '7.7.7.7').status, 200);
});

test('the list is not persisted and a list instance holds no address', () => {
    const l = new PublicList({ rng: seededRng(1) });
    assert.equal(l.put('r1', 'BCDF', { name: 'X' }, '10.0.0.1'), 'ok');
    assert.ok(!JSON.stringify([...l.rooms.values()]).includes('10.0.0.1'));
});

async function http(port, method, path, body) {
    const r = await fetch(`http://127.0.0.1:${port}${path}`, {
        method, headers: body ? { 'Content-Type': 'application/json' } : undefined,
        body: body ? JSON.stringify(body) : undefined });
    let j = null;
    try { j = await r.json(); } catch (e) { /* no body */ }
    return { status: r.status, body: j };
}

test('dev relay: the same list over HTTP; and without it (--no-public) a 404', async () => {
    const relay = await startRelay(0, { quiet: true });
    try {
        const p = relay.port;
        assert.deepEqual((await http(p, 'GET', '/v2/public')).body.games, []);
        const h = (await http(p, 'POST', '/v2/rooms')).body;
        const r = await http(p, 'POST', '/v2/public', { code: h.code, resume: h.resume, name: 'waddle', n: 1, mode: 'roam', locked: true });
        assert.equal(r.status, 200);
        assert.equal(r.body.every, 25);
        const g = (await http(p, 'GET', '/v2/public')).body.games;
        assert.equal(g.length, 1);
        assert.equal(g[0].name, 'WADDLE'); assert.equal(g[0].locked, true); assert.equal(g[0].mode, 'roam');
        assert.equal((await http(p, 'POST', '/v2/public', { code: h.code, resume: 'f'.repeat(48) })).status, 403);
        assert.equal((await http(p, 'POST', '/v2/public/remove', { code: h.code, resume: h.resume })).status, 200);
        assert.deepEqual((await http(p, 'GET', '/v2/public')).body.games, []);
        // the old endpoints unchanged
        assert.equal((await http(p, 'GET', '/v2/rooms/' + h.code)).status, 200);
        assert.equal((await http(p, 'GET', '/v2/rooms/ZZZZ')).status, 404);
    } finally { relay.close(); }
    const old = await startRelay(0, { quiet: true, noPublic: true });
    try {
        const p = old.port;
        assert.equal((await http(p, 'GET', '/v2/public')).status, 404);
        const h = (await http(p, 'POST', '/v2/rooms')).body;
        assert.equal((await http(p, 'POST', '/v2/public', { code: h.code, resume: h.resume })).status, 404);
        assert.equal((await http(p, 'GET', '/v2/rooms/' + h.code)).status, 200);   // hosting and joining as before
    } finally { old.close(); }
});
