// The Durable Object spike from docs/SERVER-RENDER.md §5 step 1: does the
// real module instantiate inside workerd, can it run seat rounds at 60 Hz,
// and what does a round cost there.  Nothing about rooms or players.
//
//   npx wrangler dev -c world/wrangler.spike.jsonc     (from netplay/)
//   curl -X POST --data-binary @rom.gba http://127.0.0.1:8787/rom
//   curl http://127.0.0.1:8787/run?rounds=300      -- as fast as it goes
//   curl http://127.0.0.1:8787/tick?seconds=10     -- paced at 60 Hz
//
// Local wrangler dev does not enforce the 128 MB limit; only a deploy does.

import { DurableObject } from 'cloudflare:workers';
import wasm from '../../build/katam-server.wasm';
import { startWorld } from './katam_host.mjs';

const PERIOD = 1000 / 59.7275;

async function deflateRaw(bytes) {
    const cs = new CompressionStream('deflate-raw');
    const w = cs.writable.getWriter();
    w.write(bytes); w.close();
    return new Uint8Array(await new Response(cs.readable).arrayBuffer());
}

export class WorldSpike extends DurableObject {
    constructor(ctx, env) {
        super(ctx, env);
        this.rom = null;
        this.world = null;
        this.bytes = [0, 0, 0, 0];
        this.frames = 0;
        this.pendingPackets = [];
    }

    async fetch(req) {
        const url = new URL(req.url);
        if (url.pathname === '/memory') {
            const pages = Number(url.searchParams.get('mb') || 192) * 16;
            try { const m = new WebAssembly.Memory({ initial: pages }); new Uint8Array(m.buffer)[0] = 1; return Response.json({ ok: true, bytes: m.buffer.byteLength }); }
            catch (e) { return Response.json({ ok: false, message: String(e) }); }
        }
        if (url.pathname === '/timers') {
            const t = Date.now(); let n = 0;
            for (; n < 100; n++) await new Promise((r) => setTimeout(r, 0));
            return Response.json({ ok: true, timers: n, ms: Date.now() - t });
        }
        if (req.method === 'POST' && url.pathname === '/rom') {
            this.rom = new Uint8Array(await req.arrayBuffer());
            this.world = null;
            return Response.json({ rom: this.rom.length });
        }
        if (!this.rom) return Response.json({ error: 'POST the ROM to /rom first' }, { status: 400 });
        if (!this.world) try {
            /* The codec wants a synchronous deflate; CompressionStream is
             * async.  The spike compresses each seat's delta bytes after the
             * round instead, which measures the same work. */
            const t = performance.now();
            this.world = await startWorld({
                wasm, rom: this.rom,
                deflate: (b) => { this.pendingPackets.push(b.slice()); return new Uint8Array(0); },
                onFrame: (seat) => { this.frames++; },
                log: (l) => console.log(l),
            });
            this.world.bootMs = performance.now() - t;
        } catch (e) {
            return Response.json({ error: 'world did not come up', message: String(e && e.message || e), stack: String(e && e.stack || '') }, { status: 500 });
        }
        const w = this.world;
        if (url.pathname === '/run') {
            const rounds = Number(url.searchParams.get('rounds') || 300);
            const t = performance.now();
            let deflateMs = 0, out = 0;
            for (let i = 0; i < rounds; i++) {
                w.setInput(0, (i % 90) < 45 ? 0x50 : 0);
                if (!w.tick()) await new Promise((r) => setTimeout(r, 0));
                await new Promise((r) => setTimeout(r, 0));   /* let the round finish */
                const td = performance.now();
                for (const b of this.pendingPackets) out += (await deflateRaw(b)).length;
                deflateMs += performance.now() - td;
                this.pendingPackets.length = 0;
            }
            const ms = performance.now() - t;
            return Response.json({
                bootMs: w.bootMs, rounds, msPerRound: ms / rounds,
                encodeMsPerRound: w.stats().presentMs / w.stats().rounds,
                deflateMsPerRound: deflateMs / rounds,
                bytesPerRound: out / rounds, frames: this.frames,
            });
        }
        if (url.pathname === '/tick') {
            const seconds = Number(url.searchParams.get('seconds') || 5);
            const want = Math.round(seconds * 1000 / PERIOD);
            const t = performance.now();
            let done = 0, late = 0, next = t;
            await new Promise((resolve) => {
                const timer = setInterval(() => {
                    const now = performance.now();
                    if (now > next + PERIOD) late++;
                    next += PERIOD;
                    w.setInput(0, (done % 90) < 45 ? 0x50 : 0);
                    w.tick();
                    this.pendingPackets.length = 0;
                    if (++done >= want) { clearInterval(timer); resolve(); }
                }, PERIOD);
            });
            const ms = performance.now() - t;
            return Response.json({ seconds, rounds: done, achievedHz: done / (ms / 1000), lateTicks: late });
        }
        if (url.pathname === '/instances') {
            /* The memory probe: bring up N more whole worlds beside this one.
             * Each is another 192 MB linear memory of which ~17 MB is ever
             * touched; if the platform accounts the reservation, the first
             * one already breaks the 128 MB limit, and if it accounts what
             * is touched, several fit.  The DO dying is the failure mode,
             * which is why this reports as it goes. */
            const n = Number(url.searchParams.get('n') || 1);
            this.extra = this.extra || [];
            const up = [];
            for (let i = 0; i < n; i++) {
                const t = performance.now();
                const w = await startWorld({
                    wasm, rom: this.rom,
                    deflate: () => new Uint8Array(0), onFrame: () => {},
                });
                this.extra.push(w);
                up.push({ world: this.extra.length, bootMs: Math.round(performance.now() - t) });
            }
            return Response.json({ extraWorlds: this.extra.length, brought: up });
        }
        return Response.json({ error: 'unknown path' }, { status: 404 });
    }
}

export default {
    async fetch(req, env) {
        const id = env.WorldSpike.idFromName('spike');
        return env.WorldSpike.get(id).fetch(req);
    },
};
