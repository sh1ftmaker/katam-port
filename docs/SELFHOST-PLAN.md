# Self-hosted room server: plan

Status: **plan only, nothing built** (written 2026-11-17). It comes from reading
`netplay/` and `web/online.js`; none of it has been prototyped.

## Why

Online play needs a room server for joining: it swaps the WebRTC connection
details between browsers, checks the room code and password, gates on build
and ROM, and holds the kick list and the public list. Today that is one
Cloudflare worker (`katam-netplay`). A game in progress on direct links does
not pass traffic through it, but nobody can join, rejoin or re-establish a
broken link without it. The page also uses Cloudflare's STUN server to set up
direct links. A self-hosted server removes that dependency for anyone who
wants to run their own.

## What exists today

- **Shared room logic.** Rooms, codes, passwords, the version gate, kick list,
  public list and TURN hand-out are in files both servers use
  (`session.mjs`, `codes.mjs`, `directory.mjs`, `listing.mjs`, `turn.mjs`).
  `server.mjs` (the worker) and `dev-relay.mjs` (plain Node, for tests) are
  thin wrappers around them.
- **A page switch.** `?netserver=<base>` points the game at another room
  server (`web/online.js`, `web/online_browse.js`; the default is
  `KatamNet.DEFAULT_BASE` in `web/net_session.js`).
- **STUN.** The page always uses `stun:stun.cloudflare.com:3478`; it cannot be
  changed without editing the page.

## The main obstacle: HTTPS

The game page is served over HTTPS (GitHub Pages), and browsers refuse to let
an HTTPS page open a plain `ws://` or `http://` connection (except to
`localhost`). So a self-hosted server needs a real certificate, or the page
must be served from the same machine. This shapes the plan.

## Steps

1. **A proper server entry point.** Split `netplay/selfhost.mjs` out of
   `dev-relay.mjs`:
   - config by environment variables (port, public list on or off, optional
     TURN servers, STUN address);
   - a health-check path;
   - clean shutdown.

   The test-only parts (the v1 rooms, test hooks) stay in `dev-relay.mjs`.
2. **Serve the game from the same server.** The self-host server also serves
   the built page (`build/dist`), so one address gives the game and the rooms.
   A page served this way uses its own origin as the room server by default,
   so no `?netserver` is needed.
3. **HTTPS, three documented ways, easiest first.**
   - A tunnel (Cloudflare tunnel or similar): one command, a public address
     with a certificate. Still depends on the tunnel provider.
   - A reverse proxy such as Caddy on a home server or VPS with a domain;
     certificates are fetched automatically.
   - Plain HTTP on a LAN or VPN (Tailscale, a LAN party). Browsers allow
     insecure origins only for `localhost`, so this needs the VPN's own
     certificate feature or a browser flag. Confirm what works before
     documenting it as supported.
4. **Configurable STUN.** The server tells the page which STUN (and optional
   TURN) servers to use, in the room's `welcome`, defaulting to a public one.
   An optional bundled STUN/TURN (coturn in the Docker setup) covers the fully
   independent case.
5. **Hardening the worker gets for free.**
   - the client address behind a proxy (the rate limits key on it; the worker
     reads `CF-Connecting-IP`);
   - connection and message-size caps;
   - room and memory limits;
   - an idle-room sweep.

   On Cloudflare the platform absorbs floods; a home server will not.
6. **Persistence, minimal.** The worker saves room state so a restart does not
   lose rooms. Start with memory only (a restart ends open rooms; games in
   progress on direct links keep running), and add a small state file later if
   wanted.
7. **Invite links carry the server.** A host on a self-hosted server makes
   invite links that point at that server, so friends need no setup.
8. **Packaging.** One `npm start`, a Dockerfile, a compose file with the
   optional proxy and TURN, and a short doc page.
9. **Tests.** Run the existing browser online tests
   (`tools/online_browser_test.mjs`, which already run against
   `dev-relay.mjs`) against the new entry point, plus one test for the
   same-origin default and one for the server-supplied STUN list.

## Open decisions

- **Fallback in the official page.** Should the public site try a second room
  server by itself when Cloudflare is down, or is self-hosting only something
  a person opts into?
- **Public list.** Do self-hosted servers have their own public room list, or
  codes and invite links only?
- **ROM stays local.** The self-hosted page would still ask each visitor for
  their own ROM; the server never holds it. Recommended: keep it that way.

## Size

Steps 1, 2, 4, 7 and 9 are roughly one agent session and give a working "run
this on a machine with a domain" server. Steps 3, 5 and 8 are a second session
of mostly documentation, packaging and limits. The fully independent version
with bundled TURN is the slowest to verify, because it needs testing on real
restrictive networks.

## Not covered: no server at all

A "manual connect" mode (host and guest paste a connection blob to each other
over chat) would need no room server. It does not exist, is clunky for eight
people, and players behind home routers would still need some STUN server.
