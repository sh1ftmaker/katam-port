// Protocol-v2 load test: N synthetic clients in one room, no game.
//
//   node tools/load_test.mjs [--url http://127.0.0.1:PORT] [--clients 16]
//        [--seconds 20] [--slow 1] [--slow-ms 300] [--lag 0] [--deadline 100]
//        [--flush 0] [--json out.json]
//
// Without --url it starts an in-process dev relay (and fakes one IP per
// client with X-Forwarded-For, so the edge limits see N players, not one).
// Each client sends one INPUT per 60 Hz frame (a held direction that
// changes every ~23 frames, the measured mean run), receives the sealed
// stream, and records:
//   - seal latency: time from sending frame f to receiving f sealed;
//   - bytes up / down per second;
//   - frames it fell behind the sealed frontier.
// --slow K makes the last K clients add --slow-ms of one-way delay to
// everything they send and receive (a bad link); --lag K makes the last K
// run at 30 Hz instead of 60 (a slow device).  PASS: every client's
// sealed stream is byte-identical, and the fast clients' frame rate and
// seal latency stay bounded by the deadline whatever the slow ones do.

import WebSocket from 'ws';
import { encodeInput, FrameDecoder, T_FRAMES } from '../wire.mjs';
import { PROTO_VERSION } from '../session.mjs';
import { startRelay } from '../dev-relay.mjs';
import { createHash } from 'crypto';

const args = Object.fromEntries(process.argv.slice(2).reduce((a, x, i, all) => {
    if (x.startsWith('--')) a.push([x.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : '1']);
    return a;
}, []));
const N = +(args.clients || 16);
const SECONDS = +(args.seconds || 20);
const SLOW = +(args.slow || 0);
const SLOW_MS = +(args['slow-ms'] || 300);
const LAG = +(args.lag || 0);
const DEADLINE = +(args.deadline || 100);
const FLUSH = +(args.flush || 0);

let relay = null;
let base = args.url;
if (!base) {
    relay = await startRelay(0, { quiet: true, ipHeader: true });
    base = `http://127.0.0.1:${relay.port}`;
}
const wsBase = base.replace(/^http/, 'ws');

const create = await fetch(`${base}/v2/rooms`, { method: 'POST', headers: { 'X-Forwarded-For': '10.9.0.1' } });
const { code } = await create.json();
if (!code) throw new Error('create failed: ' + create.status);

function pct(a, p) { if (!a.length) return 0; const s = a.slice().sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(p * s.length))]; }

class Client {
    constructor(i) {
        this.i = i; this.slow = i >= N - SLOW; this.lagging = i >= N - LAG && !this.slow;
        this.delay = this.slow ? SLOW_MS : 0;
        this.dec = new FrameDecoder(); this.frameHashes = []; this.sealedN = 0;
        this.sentAt = new Map(); this.lat = []; this.up = 0; this.down = 0;
        this.frame = 0; this.frontier = 0; this.pings = 0; this.behindMax = 0; this.keys = 0; this.started = false;
        this.identity = 'load-' + i + '-' + Math.random().toString(36).slice(2) + 'xxxxxxxxxxxx';
    }
    connect() {
        return new Promise((res, rej) => {
            this.ws = new WebSocket(`${wsBase}/v2/rooms/${code}/ws`, { headers: { 'X-Forwarded-For': `10.8.${this.i >> 8}.${this.i & 255}` } });
            this.ws.binaryType = 'nodebuffer';
            this.ws.on('open', () => {
                this.tx(JSON.stringify({ t: 'hello', proto: PROTO_VERSION, sim: 'load', rom: 'load', content: 'vanilla',
                    identity: this.identity, name: 'bot' + this.i, platform: 'node', want: 'play',
                    caps: { seats: 1024, sync: false },
                    create: this.i === 0 ? { seats: N, deadlineMs: DEADLINE, flushMs: FLUSH, syncEveryMs: 0 } : undefined }));
            });
            this.ws.on('message', (d, bin) => setTimeout(() => this.rx(d, bin), this.delay));
            this.ws.on('error', rej);
            this.onWelcome = res;
        });
    }
    tx(data) {
        this.up += data.length;
        if (this.delay) setTimeout(() => this.ws.readyState === 1 && this.ws.send(data), this.delay);
        else this.ws.send(data);
    }
    rx(d, bin) {
        this.down += d.length;
        if (!bin) {
            const m = JSON.parse(d.toString());
            if (this.started) { this.byType = this.byType || {}; this.byType[m.t] = (this.byType[m.t] || 0) + d.length; }
            if (m.t === 'welcome') { this.mid = m.mid; this.onWelcome(); }
            if (m.t === 'start') { this.frame = m.frame; this.started = true; this.t0 = Date.now(); }
            if (m.t === 'roster') this.roster = m;
            if (m.t === 'pong') this.rtt = Date.now() - m.c;
            if (m.t === 'stats') this.roomStats = m;
            if (m.t === 'error') console.error(`client ${this.i}: error`, m);
            return;
        }
        const u = new Uint8Array(d.buffer, d.byteOffset, d.byteLength);
        if (this.started) { this.byType = this.byType || {}; this.byType.bin = (this.byType.bin || 0) + d.length; }
        if (u[0] !== T_FRAMES) return;
        const r = this.dec.decode(u);
        const now = Date.now();
        r.frames.forEach((f, k) => {
            const fr = r.firstFrame + k;
            let h = 0x811C9DC5;
            for (let q = 0; q < f.length; q++) h = Math.imul(h ^ f[q], 0x01000193);
            this.frameHashes.push(h >>> 0); this.sealedN++;
            const s = this.sentAt.get(fr);
            if (s !== undefined) { this.lat.push(now - s); this.sentAt.delete(fr); }
        });
        this.frontier = r.firstFrame + r.frames.length;
    }
    step() {
        if (!this.started) return;
        // Not waiting for anybody: if the sealed stream is ahead of us, we
        // fast-forward (a real client resims; here we just jump).
        // Lead the sealed frontier by the round trip, so our inputs arrive
        // before their frames seal (what a real client's input delay does).
        const lead = Math.ceil((this.rtt || 0) / 16.74) + 2;
        let skipped = 0;
        if (this.frontier + lead > this.frame + 4) {
            this.behindMax = Math.max(this.behindMax, this.frontier + lead - this.frame);
            skipped = Math.min(31, this.frontier + lead - this.frame);
        }
        if (++this.pings % 60 === 0) this.tx(JSON.stringify({ t: 'ping', c: Date.now(), rtt: this.rtt }));
        if (Math.random() < 1 / 23) this.keys = (Math.random() * 1024) & 0x3F0;
        // A jump forward still stamps the frames it skips (as web/net_game.js
        // does): a hole would make everyone wait for the deadline.
        const n = skipped + 1;
        const now = Date.now();
        for (let k = 0; k < n; k++) this.sentAt.set(this.frame + k, now);
        this.tx(encodeInput(this.frame, new Array(n).fill(this.keys)));
        this.frame += n - 1;
        this.frame++;
    }
}

const clients = [];
for (let i = 0; i < N; i++) { const c = new Client(i); clients.push(c); await c.connect(); }
await new Promise((r) => setTimeout(r, 300));
const seated = clients[0].roster.members.filter((m) => m.seat >= 0).length;
clients[0].tx(JSON.stringify({ t: 'start' }));
await new Promise((r) => setTimeout(r, 200));

const T0 = Date.now();
let ticks = 0;
const loopStart = performance.now();
await new Promise((resolve) => {
    const iv = setInterval(() => {
        const due = Math.floor((performance.now() - loopStart) / (1000 / 60));
        while (ticks < due) {
            for (const c of clients) if (!c.lagging || ticks % 2 === 0) c.step();
            ticks++;
        }
        if (Date.now() - T0 >= SECONDS * 1000) { clearInterval(iv); resolve(); }
    }, 4);
});
await new Promise((r) => setTimeout(r, SLOW_MS * 2 + DEADLINE + 300));

const secs = SECONDS;   // the played span; the tail wait only drains
const fast = clients.filter((c) => !c.slow && !c.lagging);
// The room keeps sealing while seats are connected (nobody waits), and a
// slow link sees the stream later: compare the common prefix.
const counts = clients.map((c) => c.sealedN);
const common = Math.min(...counts);
const hashes = new Set(clients.map((c) => createHash('sha1').update(new Uint32Array(c.frameHashes.slice(0, common))).digest('hex')));
const res = {
    clients: N, seated, code, seconds: +secs.toFixed(1), deadlineMs: DEADLINE, flushMs: FLUSH,
    slow: SLOW, slowMs: SLOW_MS, lagging: LAG,
    sealedFrames: common, framesPerSec: +(common / secs).toFixed(1),
    identicalStreams: hashes.size === 1 && common > 0,
    fastFramesSentPerSec: +(Math.min(...fast.map((c) => c.sentAt.size + c.lat.length)) / secs).toFixed(1),
    sealLatencyMs: { p50: pct(fast.flatMap((c) => c.lat), 0.5), p90: pct(fast.flatMap((c) => c.lat), 0.9), p95: pct(fast.flatMap((c) => c.lat), 0.95), p99: pct(fast.flatMap((c) => c.lat), 0.99),
                     max: Math.max(...fast.flatMap((c) => c.lat)) },
    slowSealLatencyMs: SLOW || LAG ? pct(clients.filter((c) => c.slow || c.lagging).flatMap((c) => c.lat), 0.5) : null,
    downBytesPerSecPerClient: Math.round(clients.reduce((n, c) => n + Object.values(c.byType || {}).reduce((a, b) => a + b, 0), 0) / N / secs),
    joinBurstBytesPerClient: Math.round(clients.reduce((n, c) => n + c.down - Object.values(c.byType || {}).reduce((a, b) => a + b, 0), 0) / N),
    upBytesPerSecPerClient: Math.round(clients.reduce((n, c) => n + c.up, 0) / N / secs),
    roomFanoutMsgsPerSec: relay ? Math.round([...relay.v2.rooms.values()][0].core.stats.framesMsgs * N / secs) : null,
    remoteDeadlineSeals: clients[0].roomStats ? `${clients[0].roomStats.deadlineSeals}/${clients[0].roomStats.sealed}` : null,
    deadlineSealsPct: relay ? +(100 * [...relay.v2.rooms.values()][0].core.stats.deadlineSeals / [...relay.v2.rooms.values()][0].core.stats.sealed).toFixed(1) : null,
    lateInputs: relay ? [...relay.v2.rooms.values()][0].core.stats.lateInputs : null,
    slowBehindMaxFrames: Math.max(0, ...clients.filter((c) => c.slow || c.lagging).map((c) => c.behindMax)),
};
res.downByTypePerSec = Object.fromEntries(Object.entries(clients[0].byType || {}).map(([k, v]) => [k, Math.round(v / secs)]));
console.log(JSON.stringify(res, null, 1));
if (args.json) (await import('fs')).writeFileSync(args.json, JSON.stringify(res, null, 1));
for (const c of clients) c.ws.close();
if (relay) relay.close();
const pass = res.identicalStreams && res.sealLatencyMs.p95 <= DEADLINE + 60 + (FLUSH || 0);
console.log(pass ? 'LOAD TEST PASSED' : 'LOAD TEST FAILED');
process.exit(pass ? 0 : 1);
