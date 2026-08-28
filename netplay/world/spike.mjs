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
        if (req.method === 'POST' && url.pathname === '/rom') {
            this.rom = new Uint8Array(await req.arrayBuffer());
            this.world = null;
            return Response.json({ rom: this.rom.length });
        }
        if (!this.rom) return Response.json({ error: 'POST the ROM to /rom first' }, { status: 400 });
        if (!this.world) {
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
        return Response.json({ error: 'unknown path' }, { status: 404 });
    }
}

export default {
    async fetch(req, env) {
        const id = env.WorldSpike.idFromName('spike');
        return env.WorldSpike.get(id).fetch(req);
    },
};
