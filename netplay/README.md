# netplay/

The relay between two (to four) instances of the port, in two wearable forms
over one shared brain:

| file | what |
|---|---|
| `protocol.mjs` | the wire protocol and the room logic -- everything with a decision in it |
| `dev-relay.mjs` | that room on a plain node WebSocket server, for development and the headless tests |
| `server.mjs` | that room on Cloudflare (partyserver / Durable Objects), for the world |

The client side is `web/mp_net.js`, which turns the relay into a
`PortMpTransport` at the seam described in docs/MULTIPLAYER.md §2.  The
protocol's shape -- ordered per-transfer streams with counts and sequence
numbers, not fixed-size frames -- is derived in docs/NETPLAY.md §3, from
`platform/sio.c`.

Local:

    npm install
    npm run relay             # ws://127.0.0.1:8787/<room>

Cloudflare (manual, never CI):

    npx wrangler dev          # local worker emulation
    npx wrangler deploy       # -> wss://katam-netplay.<account>.workers.dev/parties/game-room/<room>
