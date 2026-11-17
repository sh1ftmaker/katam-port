// TURN credentials (netplay/turn.mjs, SessionCore._turnRequest): who gets
// them, how often, what a browser is handed, and Cloudflare's API shape --
// against a mock fetch (no Cloudflare account is touched here).
import test from 'node:test';
import assert from 'node:assert/strict';
import { makeRoom, Clock } from './harness.mjs';
import { TURN_LIMITS } from '../session.mjs';
import { browserIceServers, cloudflareTurn, staticTurn, TURN_API, TURN_TTL_S } from '../turn.mjs';
import { startRelay } from '../dev-relay.mjs';

const CF_REPLY = {
    iceServers: [
        { urls: ['stun:stun.cloudflare.com:3478', 'stun:stun.cloudflare.com:53'] },
        { urls: ['turn:turn.cloudflare.com:3478?transport=udp', 'turn:turn.cloudflare.com:53?transport=udp',
                 'turn:turn.cloudflare.com:3478?transport=tcp', 'turn:turn.cloudflare.com:80?transport=tcp',
                 'turns:turn.cloudflare.com:5349?transport=tcp', 'turns:turn.cloudflare.com:443?transport=tcp'],
          username: 'user-1', credential: 'cred-1' },
    ],
};

function mockFetch(reply = CF_REPLY, status = 201) {
    const calls = [];
    const f = async (url, init) => {
        calls.push({ url, init });
        if (/\/revoke$/.test(url)) return { status: 204, json: async () => ({}) };
        return { status, json: async () => reply };
    };
    f.calls = calls;
    return f;
}

const settle = () => new Promise((r) => setTimeout(r, 0));
const creds = (c) => c.json.filter((m) => m.t === 'turn_creds');

test('browserIceServers: TURN urls only, never port 53, credentials kept', () => {
    const out = browserIceServers(CF_REPLY.iceServers);
    assert.equal(out.length, 1);
    assert.deepEqual(out[0].urls, ['turn:turn.cloudflare.com:3478?transport=udp', 'turn:turn.cloudflare.com:3478?transport=tcp',
                                   'turn:turn.cloudflare.com:80?transport=tcp', 'turns:turn.cloudflare.com:5349?transport=tcp',
                                   'turns:turn.cloudflare.com:443?transport=tcp']);
    assert.equal(out[0].username, 'user-1');
    assert.equal(out[0].credential, 'cred-1');
    assert.deepEqual(browserIceServers(null), []);
    assert.deepEqual(browserIceServers({ urls: 'turn:h:3478', username: 'a', credential: 'b' }),
                     [{ urls: ['turn:h:3478'], username: 'a', credential: 'b' }]);
});

test('cloudflareTurn: no secrets -> no provider; with them, the documented request', async () => {
    assert.equal(cloudflareTurn({}), null);
    assert.equal(cloudflareTurn({ TURN_KEY_ID: 'k' }), null);
    assert.equal(cloudflareTurn({ TURN_KEY_API_TOKEN: 't' }), null);
    const f = mockFetch();
    const p = cloudflareTurn({ TURN_KEY_ID: 'key-id', TURN_KEY_API_TOKEN: 'tok' }, { fetch: f });
    const r = await p.mint();
    assert.equal(f.calls[0].url, TURN_API + 'key-id/credentials/generate-ice-servers');
    assert.equal(f.calls[0].init.method, 'POST');
    assert.equal(f.calls[0].init.headers.Authorization, 'Bearer tok');
    assert.deepEqual(JSON.parse(f.calls[0].init.body), { ttl: TURN_TTL_S });
    assert.equal(r.ttl, TURN_TTL_S);
    assert.deepEqual(r.usernames, ['user-1']);
    assert.ok(r.iceServers[0].urls.every((u) => !/:53\?/.test(u)));
    await p.revoke(['user-1']);
    assert.equal(f.calls[1].url, TURN_API + 'key-id/credentials/user-1/revoke');
    assert.equal(f.calls[1].init.method, 'POST');
    // an error status: no servers
    const bad = cloudflareTurn({ TURN_KEY_ID: 'k', TURN_KEY_API_TOKEN: 't' }, { fetch: mockFetch({}, 401) });
    assert.equal(await bad.mint(), null);
});

test('room without TURN: welcome says so, a request gets none', async () => {
    const room = makeRoom();
    const h = room.client('host'), g = room.client('guest');
    assert.equal(h.welcome.turn, false);
    g.sendJson({ t: 'turn_req' });
    await settle();
    assert.deepEqual(creds(g).pop(), { t: 'turn_creds', iceServers: [], why: 'none' });
});

test('room with TURN: members only, not alone, limited, revoked on leave and kick', async () => {
    const f = mockFetch();
    const turn = cloudflareTurn({ TURN_KEY_ID: 'k', TURN_KEY_API_TOKEN: 't' }, { fetch: f });
    const room = makeRoom({}, new Clock(), { turn });
    const h = room.client('host');
    assert.equal(h.welcome.turn, true);
    // alone in the room: nothing to link to
    h.sendJson({ t: 'turn_req' });
    await settle();
    assert.equal(creds(h).pop().why, 'alone');
    // not a member yet (no hello): the room closes it, no credentials
    room.core.open('raw', { ip: '10.9.9.9' });
    room.core.message('raw', JSON.stringify({ t: 'turn_req' }));
    assert.equal(f.calls.length, 0);
    // a member who failed the version gate never gets there either
    const bad = room.client('bad', { rom: 'other' });
    assert.equal(bad.error.code, 'version');
    const g = room.client('guest');
    g.sendJson({ t: 'turn_req' });
    await settle();
    const c = creds(g).pop();
    assert.equal(c.ttl, TURN_TTL_S);
    assert.equal(c.iceServers[0].username, 'user-1');
    assert.equal(f.calls.length, 1);
    // per member: a burst, then refused
    for (let i = 1; i < TURN_LIMITS.memberBurst; i++) { g.sendJson({ t: 'turn_req' }); await settle(); }
    assert.equal(creds(g).filter((m) => m.iceServers.length).length, TURN_LIMITS.memberBurst);
    g.sendJson({ t: 'turn_req' });
    await settle();
    assert.equal(creds(g).pop().why, 'rate');
    // ... and again after the refill
    room.clock.advance(TURN_LIMITS.memberEverySec * 1000);
    g.sendJson({ t: 'turn_req' });
    await settle();
    assert.ok(creds(g).pop().iceServers.length);
    const minted = f.calls.length;
    // leaving revokes what that member was given
    g.sendJson({ t: 'leave' });
    await settle(); await settle();
    const revokes = f.calls.slice(minted).filter((x) => /\/revoke$/.test(x.url));
    assert.ok(revokes.length >= 1, 'revoked on leave');
    // a kicked member's too
    const k = room.client('kicked');
    k.sendJson({ t: 'turn_req' });
    await settle();
    assert.ok(creds(k).pop().iceServers.length);
    const before = f.calls.length;
    h.sendJson({ t: 'kick', mid: k.mid });
    await settle(); await settle();
    assert.ok(f.calls.slice(before).some((x) => /\/revoke$/.test(x.url)), 'revoked on kick');
});

test('room with TURN: per-address and per-room limits', async () => {
    const turn = staticTurn([{ urls: ['turn:127.0.0.1:3478'], username: 'u', credential: 'p' }]);
    const room = makeRoom({ maxSpectators: 64 }, new Clock(), { turn });
    room.client('host');
    const sameIp = [];
    for (let i = 0; i < TURN_LIMITS.ipBurst + 1; i++) sameIp.push(room.client('n' + i, { ip: '10.1.1.1' }));
    for (const c of sameIp) { c.sendJson({ t: 'turn_req' }); await settle(); }
    const got = sameIp.map((c) => creds(c).pop());
    assert.equal(got.filter((x) => x.iceServers.length).length, TURN_LIMITS.ipBurst);
    assert.equal(got.pop().why, 'rate');
    const others = [];
    for (let i = 0; i < TURN_LIMITS.roomBurst; i++) others.push(room.client('o' + i));
    for (const c of others) { c.sendJson({ t: 'turn_req' }); await settle(); }
    const n = others.filter((c) => creds(c).pop().iceServers.length).length;
    assert.equal(n, TURN_LIMITS.roomBurst - TURN_LIMITS.ipBurst);
});

test('dev relay: no TURN by default; TURN_ICE_SERVERS-style list when given', async () => {
    for (const [turnServers, want] of [[null, 0], [[{ urls: ['turn:127.0.0.1:3478?transport=udp'], username: 'u', credential: 'p' }], 1]]) {
        const relay = await startRelay(0, { quiet: true, turnServers });
        const base = `http://127.0.0.1:${relay.port}`;
        const r = await (await fetch(base + '/v2/rooms', { method: 'POST' })).json();
        const open = (name) => new Promise((res) => {
            const ws = new WebSocket(`ws://127.0.0.1:${relay.port}/v2/rooms/${r.code}/ws`);
            const got = [];
            ws.onmessage = (e) => { const m = JSON.parse(e.data); got.push(m); if (m.t === 'welcome') res({ ws, got, welcome: m }); };
            ws.onopen = () => ws.send(JSON.stringify({ t: 'hello', proto: 2, sim: 's', rom: 'r', content: 'c',
                                                         identity: 'identity-' + name + '-0123456789', name }));
        });
        const h = await open('host'), g = await open('guest');
        assert.equal(g.welcome.turn, !!want);
        g.ws.send(JSON.stringify({ t: 'turn_req' }));
        const c = await new Promise((res) => {
            const t = setInterval(() => { const m = g.got.find((x) => x.t === 'turn_creds'); if (m) { clearInterval(t); res(m); } }, 10);
        });
        assert.equal(c.iceServers.length, want);
        if (want) assert.equal(c.iceServers[0].username, 'u');
        else assert.equal(c.why, 'none');
        h.ws.close(); g.ws.close();
        relay.close();
    }
});
