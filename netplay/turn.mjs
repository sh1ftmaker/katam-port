// TURN credentials for room members: the middle tier between a direct
// WebRTC link and the room's WebSocket `signal` fallback (web/online.js).
//
// A TURN provider is { mint(info) -> Promise<{iceServers, ttl, usernames}|null>,
// revoke?(usernames) -> Promise }.  SessionCore (session.mjs) owns who may
// ask and how often; this file only knows where credentials come from:
//
//   cloudflareTurn(env)   Cloudflare Realtime TURN (turn.cloudflare.com).
//                         The worker's secrets TURN_KEY_ID and
//                         TURN_KEY_API_TOKEN; either missing -> null (no
//                         TURN: the room says so in its welcome, and the
//                         page behaves exactly as with STUN only).
//   staticTurn(list)      a fixed server list (the dev relay's
//                         TURN_ICE_SERVERS, a local TURN server in tests).
//
// Cloudflare's API (developers.cloudflare.com/realtime/turn/generate-credentials/):
//   POST https://rtc.live.cloudflare.com/v1/turn/keys/<TURN_KEY_ID>/credentials/generate-ice-servers
//        Authorization: Bearer <TURN_KEY_API_TOKEN>   {"ttl": <seconds>}
//     -> 201 {"iceServers": [{urls:[stun...]}, {urls:[turn..., turns...], username, credential}]}
//   POST .../credentials/<username>/revoke  -> 204
// A credential lasts at most 48 h; one that expires while in use is
// disconnected shortly after.  The page refreshes them before that
// (RTCPeerConnection.setConfiguration, which Cloudflare documents for it).

export const TURN_API = 'https://rtc.live.cloudflare.com/v1/turn/keys/';

// Three hours: a long play session; the page asks for fresh ones half an
// hour before they run out (web/online.js TURN_RENEW_BEFORE_MS).
export const TURN_TTL_S = 3 * 3600;
const TURN_TTL_MAX_S = 48 * 3600;

// What a browser should be given: TURN entries only (the page has its own
// STUN), at most a few, each url a turn:/turns: url not on port 53 --
// Cloudflare's docs: "The alternate port 53 is known to be blocked by web
// browsers, and the TURN URL will time out if used in browsers."
export function browserIceServers(list) {
    const out = [];
    for (const s of Array.isArray(list) ? list : list ? [list] : []) {
        if (!s || typeof s !== 'object') continue;
        const urls = (Array.isArray(s.urls) ? s.urls : [s.urls])
            .filter((u) => typeof u === 'string' && /^turns?:/i.test(u) && !/^turns?:[^?]*:53(\?|$)/i.test(u))
            .slice(0, 8);
        if (!urls.length) continue;
        const e = { urls };
        if (typeof s.username === 'string') e.username = s.username;
        if (typeof s.credential === 'string') e.credential = s.credential;
        out.push(e);
        if (out.length >= 4) break;
    }
    return out;
}

export function cloudflareTurn(env = {}, opts = {}) {
    const keyId = env.TURN_KEY_ID, token = env.TURN_KEY_API_TOKEN;
    if (!keyId || !token) return null;
    const doFetch = opts.fetch || ((...a) => fetch(...a));
    const ttl = Math.max(60, Math.min(TURN_TTL_MAX_S, opts.ttl || TURN_TTL_S));
    const log = opts.log || (() => {});
    const base = TURN_API + encodeURIComponent(keyId) + '/credentials/';
    const auth = { Authorization: 'Bearer ' + token };
    return {
        ttl,
        async mint() {
            const r = await doFetch(base + 'generate-ice-servers', {
                method: 'POST',
                headers: { ...auth, 'Content-Type': 'application/json' },
                body: JSON.stringify({ ttl }),
            });
            if (r.status !== 201 && r.status !== 200) {
                log(`[turn] generate-ice-servers: HTTP ${r.status}`);
                return null;
            }
            const j = await r.json();
            const iceServers = browserIceServers(j && j.iceServers);
            if (!iceServers.length) return null;
            return { iceServers, ttl, usernames: iceServers.map((s) => s.username).filter(Boolean) };
        },
        async revoke(usernames) {
            for (const u of new Set(usernames || [])) {
                try {
                    await doFetch(base + encodeURIComponent(u) + '/revoke', { method: 'POST', headers: auth });
                } catch (e) { log('[turn] revoke: ' + e.message); }
            }
        },
    };
}

export function staticTurn(list, ttl = TURN_TTL_S) {
    const iceServers = browserIceServers(list);
    if (!iceServers.length) return null;
    return { ttl, async mint() { return { iceServers, ttl, usernames: [] }; } };
}
