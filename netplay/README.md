# netplay/

> **Protocol v2 online rooms: shelved 2026-11-08, hidden behind `?online=1`.**
> They are still deployed and tested. How it is set up, how to run it, and
> why it desyncs and rubberbands: [docs/NETPLAY-HANDOFF.md](../docs/NETPLAY-HANDOFF.md).
> The v1 link-cable / Path B rooms in this directory are unaffected.

The rooms that katam-port instances meet in, in two wearable forms over
shared, socket-free logic:

| file | what |
|---|---|
| `codes.mjs` | 4-letter room codes, the code directory, resume tokens, rate limits |
| `directory.mjs` | the directory's HTTP face: create / look up / resume, with the edge limits |
| `wire.mjs` | protocol-v2 binary codecs: INPUT, FRAMES/ARCHIVE, CHUNK |
| `session.mjs` | **SessionCore**, the protocol-v2 room: lobby, seats, deadline sequencer, events and watermark, archive, hash referee, donor election, world-sync relay |
| `protocol.mjs` | protocol v1 (link-cable words, Path B inputs) and its RoomCore |
| `dev-relay.mjs` | all of it on a plain node server, for development and the tests |
| `server.mjs` | all of it on Cloudflare: `Directory` + `SessionRoom` DOs (v2), `GameRoom` (v1, partyserver) |
| `test/` | unit tests (`npm test`) |
| `tools/load_test.mjs` | N synthetic clients in one room: latency, bandwidth, fan-out |

The design is docs/design/NETPLAY-V2.md -- §8 (codes, lobby, versioning),
§12 (model v2.1: no lockstep, deadline-sealed inputs, periodic world sync),
§13 (scaling past 4 players).  Clients: `web/net_session.js` (protocol),
`web/net_game.js` (drives the engine), `web/lobby.js` (the page's UI).

## Protocol v2 in one screen

HTTP: `POST /v2/rooms` -> `{code, resume}`; `GET /v2/rooms/CODE`;
`POST /v2/resume {token}`; WebSocket `/v2/rooms/CODE/ws`.

Control (JSON, `t` = type), client -> room: `hello {proto, sim, rom,
content, identity, name, platform, view, caps:{seats, sync}, want, create?}`,
`sit`, `stand`, `leave`, `ready`, `name`, `view {w,h}`, `archive`,
`hash {f,h}`, `syncme`, `sync_have`, `snap`, `snap_fail`, `sync_done`,
`ping`, `signal {to,data}` (WebRTC seam), and host-only `start`, `lock`,
`approval`, `approve`, `deny`, `kick`, `settings`.  Room -> client:
`welcome`, `roster`, `error {code, field?, room?, yours?}`, `start`,
`ev {seq, f, kind: seat|unseat|slot|view, ...}`, `archive_begin/_end`,
`stats {clock, frontier, late, ...}`, `desync`, `sync_req`, `snap_req`,
`snap`, `sync_cancel`, `join_req`, `notice`, `pong`, `signal`.

Binary (little-endian): INPUT `0x20 u32 first, u8 n, n x u16` (one seat's
own keys); FRAMES/ARCHIVE `0x21/0x22 u8 flags, u16 seats, u32 first, u16 n,
u32 watermark, n x {seat bitmask, u16 per changed seat}`; CHUNK `0x30 u32
id, u16 index, u16 count, <= 16 KiB`.  The sealed record the engine gets is
`u32 frame; u16 keys[seats]` (agreed with netsim: `PortNetSealFrame`).

## Local

    npm install
    npm test                         # unit tests (codes, wire, SessionCore)
    npm run relay                    # http/ws://127.0.0.1:8787: v2 under /v2/, v1 rooms by path
    npm run load -- --clients 16 --slow 1      # synthetic clients

Game end to end (needs `build/katam-node.js` and your ROM):

    node tools/netplay_v2_test.mjs build/katam-node.js baserom.gba            # in-process relay
    node tools/netplay_v2_test.mjs build/katam-node.js baserom.gba --url http://127.0.0.1:8787

In a browser: serve `web/` (or `build/dist`), open
`?online=1&relay=http://127.0.0.1:8787`, Create game, and open the invite
link in a second tab (each tab is its own player).

## Cloudflare (manual, never CI)

    npx wrangler dev                 # local emulation; the same URLs on :8787
    npx wrangler deploy              # -> https://katam-netplay.<account>.workers.dev

The first deploy after this change applies migration `v2` (new SQLite DO
classes `SessionRoom` and `Directory`); `GameRoom` (v1) is untouched, so pages
already in the wild keep working.  The page's default server is
`KatamNet.DEFAULT_BASE` in `web/net_session.js`.
