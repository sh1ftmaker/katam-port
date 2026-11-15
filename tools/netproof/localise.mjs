// Where two images of the same frame differ: regions, byte ranges, the
// nearest named RAM symbol (game/generated/port/ram_symbols.h), and for
// the Kirby array the slot and field offset.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const here = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(here, '../..');
const IMG_HDR = 64;
let SYMS = null;

function symbols() {
    if (SYMS) return SYMS;
    SYMS = [];
    const txt = fs.readFileSync(path.join(ROOT, 'game/generated/port/ram_symbols.h'), 'utf8');
    for (const m of txt.matchAll(/#define (\w+) \((?:\*\()?.*?\)?(0x[0-9A-Fa-f]{8})\)/g))
        SYMS.push({ name: m[1], addr: parseInt(m[2], 16) });
    // a few the header does not have, from the port's own layout
    SYMS.push({ name: 'gPortSim.hdr', addr: 0x02040000 }, { name: 'gPortSim.frame', addr: 0x02040020 },
              { name: 'gPortSim.lmp', addr: 0x02040030 }, { name: 'gPortSim.warp', addr: 0x02040040 },
              { name: 'gPortSim.view', addr: 0x02040050 }, { name: 'gPortSim.game', addr: 0x02040070 },
              { name: 'gPortSim.session', addr: 0x020400B0 }, { name: 'gPortSim.rando', addr: 0x020400B8 });
    SYMS.sort((a, b) => a.addr - b.addr);
    return SYMS;
}

export function nameOf(addr) {
    if (addr >= 0x02020EE0 && addr < 0x02020EE0 + 4 * 0x1A8) {
        const o = addr - 0x02020EE0;
        return `gKirbys[${Math.floor(o / 0x1A8)}]+0x${(o % 0x1A8).toString(16)}`;
    }
    const s = symbols();
    let best = null;
    for (const x of s) { if (x.addr <= addr) best = x; else break; }
    if (!best) return '?';
    const off = addr - best.addr;
    if ((best.addr >>> 24) !== (addr >>> 24) || off > 0x4000) return `0x${addr.toString(16)}`;
    return off ? `${best.name}+0x${off.toString(16)}` : best.name;
}

// parts: [{name, base, offset, size}] from PortLnSnapPart.
export function layout(inst) {
    const M = inst.M, p = M._malloc(16), parts = [];
    const names = ['ewram', 'iwram', 'io', 'pltt', 'vram', 'oam', 'simblk', 'dma', 'ppu'];
    for (let i = 0; i < 9; i++) {
        if (!M._PortLnSnapPart(i, p)) break;
        parts.push({ name: names[i], base: M.HEAPU32[p >> 2], offset: M.HEAPU32[(p >> 2) + 1], size: M.HEAPU32[(p >> 2) + 2] });
    }
    M._free(p);
    const snap = parts.reduce((n, x) => Math.max(n, x.offset + x.size), 0);
    parts.push({ name: 'sram', base: 0x09000000, offset: snap, size: 0x10000 });
    return parts;
}

export function diffImages(a, b, parts, { maxRuns = 60, gap = 8 } = {}) {
    const out = { regions: {}, runs: [] };
    for (const p of parts) {
        let bytes = 0, run = null;
        for (let i = 0; i < p.size; i++) {
            const o = IMG_HDR + p.offset + i;
            if (a[o] === b[o]) continue;
            bytes++;
            if (run && i - run.e <= gap) run.e = i;
            else {
                if (run) out.runs.push(run);
                run = { region: p.name, s: i, e: i, base: p.base };
            }
        }
        if (run) out.runs.push(run);
        if (bytes) out.regions[p.name] = bytes;
    }
    out.runs = out.runs.slice(0, maxRuns).map((r) => {
        const addr = r.base ? r.base + r.s : null;
        const len = r.e - r.s + 1;
        const hex = (img) => Buffer.from(img.slice(IMG_HDR + partOff(parts, r.region) + r.s, IMG_HDR + partOff(parts, r.region) + r.s + Math.min(len, 16))).toString('hex');
        return { region: r.region, addr: addr === null ? `+0x${r.s.toString(16)}` : '0x' + addr.toString(16).padStart(8, '0'),
                 len, symbol: addr === null ? r.region : nameOf(addr), host: hex(a), guest: hex(b) };
    });
    return out;
}
function partOff(parts, name) { return parts.find((p) => p.name === name).offset; }
