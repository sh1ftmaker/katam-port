// A SessionCore with a manual clock and fake connections, for unit tests.
import { SessionCore, PROTO_VERSION } from '../session.mjs';
import { FrameDecoder, encodeInput, decodeChunk, T_CHUNK } from '../wire.mjs';
import { seededRng } from '../codes.mjs';

export class Clock {
    constructor(t = 1000000) { this.t = t; }
    now = () => this.t;
    advance(ms) { this.t += ms; }
}

export function makeRoom(params = {}, clock = new Clock()) {
    const conns = new Map();
    const core = new SessionCore({
        now: clock.now, rng: seededRng(7), params,
        send: (id, data) => { const c = conns.get(id); if (c) c.recv(data); },
        close: (id) => { const c = conns.get(id); if (c) c.open = false; },
    });
    let n = 0;
    const room = {
        core, clock, conns,
        // advance the clock in ticks, running the timers
        run(ms, step = 5) {
            for (let t = 0; t < ms; t += step) { clock.advance(step); core.tick(); }
        },
        client(name, hello = {}) {
            const id = 'k' + (++n);
            const c = new FakeClient(room, id, name);
            conns.set(id, c);
            core.open(id, { ip: hello.ip || '10.0.0.' + n });
            c.sendJson({
                t: 'hello', proto: PROTO_VERSION, sim: 'sim1', rom: 'rom1', content: 'vanilla',
                identity: hello.identity || ('identity-' + name + '-0123456789'),
                name, platform: 'test', caps: { seats: 128, sync: true }, ...hello,
            });
            return c;
        },
    };
    return room;
}

export class FakeClient {
    constructor(room, id, name) {
        this.room = room; this.id = id; this.name = name; this.open = true;
        this.json = []; this.bin = []; this.dec = new FrameDecoder();
        this.sealed = new Map();   // frame -> Uint16Array
        this.wm = 0; this.events = [];
    }
    recv(data) {
        if (typeof data === 'string') {
            const m = JSON.parse(data);
            this.json.push(m);
            if (m.t === 'ev') this.events.push(m);
            if (m.t === 'start' || m.t === 'archive_begin') this.events.push(...m.events);
            if (m.t === 'welcome') this.welcome = m;
            if (m.t === 'roster') this.roster = m;
            if (m.t === 'error') this.error = m;
            return;
        }
        this.bin.push(data);
        if (data[0] === T_CHUNK) { this.chunks = (this.chunks || []).concat([decodeChunk(data)]); return; }
        const r = this.dec.decode(data);
        if (!r) throw new Error('undecodable frames');
        r.frames.forEach((f, i) => this.sealed.set(r.firstFrame + i, f));
        this.wm = r.watermark;
    }
    last(t) { for (let i = this.json.length - 1; i >= 0; i--) if (this.json[i].t === t) return this.json[i]; return null; }
    all(t) { return this.json.filter((m) => m.t === t); }
    sendJson(o) { this.room.core.message(this.id, JSON.stringify(o)); }
    input(first, keys) { this.room.core.message(this.id, encodeInput(first, keys)); }
    disconnect() { this.open = false; this.room.conns.delete(this.id); this.room.core.closed(this.id); }
    get mid() { return this.welcome && this.welcome.mid; }
    get me() { return this.roster && this.roster.members.find((m) => m.mid === this.mid); }
}
