// One online session -- the host and one to three guests, a star, each guest
// over its own simulated link -- on a virtual clock.  Returns everything
// measured.  Deterministic for a given config.
//
// Guests come from c.guests (default: one guest, device 1, as the two-device
// rig always was).  A guest spec:
//   dev        device id (1..3; the host numbers them, here the config does)
//   join       host frame at which it connects (default c.joinFrame or the start)
//   leave      host frame at which it says bye and goes (null: stays)
//   rejoin     host frame at which a FRESH instance of it comes back (after leave)
//   seats      [[frame, seat, on], ...] local joins / leaves on that device,
//              made through PortLmpRequestHuman (the device layer's door)
//   link       its link (default c.link); outages here model a link drop --
//              longer than DEAD_MS and the host hands its Kirbys to the CPU,
//              and takes it back (a new join image) when it is heard again
//   rate, drift, freezes   as c.guest
// c.hostSeats: [[frame, seat, on], ...] local joins / leaves on the host.
// c.rooms: the host seals rooms mode (PORT_LN_EV_ROOMS) at the session's
// first frame: players in different rooms, each device on its own screen
// (tools/rooms/gates.mjs).  c.onHostFrame(host, f) / c.onGuestTick(g) are
// its observers; sc.pad(dev, seat), when a scenario has it, gives the pads.
import { createInstance } from './instance.mjs';
import { Scheduler, Link } from './link.mjs';
import { HostHub, GuestSession, engineOf, PERIOD } from './session.mjs';
import { BOOT_END, human } from './scenario.mjs';

const F_HASH_EVERY = 1, F_HOST_KEYS = 2, F_NO_SRAM_KF = 4;
export const DEAD_MS = 6000;      // web/online.js: a link silent this long is gone

export function pct(a, p) {
    if (!a.length) return null;
    const s = Float64Array.from(a).sort();
    return s[Math.min(s.length - 1, Math.floor((p / 100) * s.length))];
}
export function dist(a) {
    return a.length ? { n: a.length, p50: pct(a, 50), p90: pct(a, 90), p99: pct(a, 99), max: Math.max(...a) } : { n: 0 };
}

export async function runSession(cfg) {
    const c = Object.assign({
        seed: 1, link: {}, host: {}, guest: {}, kfEvery: 15, kfCount: 24, flags: 0,
        startFrame: BOOT_END, joinFrame: null, durationFrames: 6000, forceRewind: null,
        corrupt: [], lockstep: false, renderGuest: false, dumpAt: null, maxWallMs: 30 * 60 * 1000,
        guests: null, hostSeats: [], deadMs: DEAD_MS,
    }, cfg);
    const hostCfg = Object.assign({ rate: 60, drift: 0, freezes: [] }, c.host);
    const sc = c.scenario;
    const wall0 = performance.now();
    const res = { name: c.name, cfg: summaryCfg(c), events: [], coverage: {}, mismatches: [], corrupt: [], churn: [] };

    // --- host: boots alone through the menus, local-multiplayer on ---
    // menus: 'session' (default: off for the boot, on from the session start, so
    // the pause SETTINGS page exists), 'boot' (on from power-on), 'off'
    const menus = c.menus || sc.menus || 'session';
    let bootT = 0, hostClockF = () => bootT;
    const host = await createInstance({ modulePath: c.modulePath, rom: c.rom, name: 'host', menus: menus === 'boot' ? 1 : 0,
                                        audio: c.hostAudio || null, clock: () => hostClockF(), modulePre: c.hostModulePre || null });
    if (c.hostPre) c.hostPre(host);
    host.M._PortLmpSetHumans(sc.localmp || 2);
    for (const e of sc.events) if (e.f < c.startFrame) {
        if (e.type === 2) host.M._PortInputLogScheduleView(e.f, e.a, e.b);
        else if (e.type === 1) host.M._PortInputLogScheduleWarp(e.f, e.a, e.b, e.c, e.d);
    }
    while (host.frame() < c.startFrame) {
        const f = host.frame();
        host.M._PortSetKeys(sc.p1(f, null));
        host.M._PortLmpSetHostKeys(1, sc.p2(f, null));
        await host.step();
        bootT += PERIOD;
    }
    if (menus === 'session') host.M._PortMenuPocSet(1);
    if (!host.M._PortLnStart(1, c.kfEvery, c.kfCount, c.flags | F_HASH_EVERY | (c.hostKeyframes ? F_HOST_KEYS : 0)))
        throw new Error('host PortLnStart failed');
    host.M._PortLmpSetHostKeys(1, 0);
    if (c.rooms) host.M._PortLnEvent(7, 1, 0, 0, 0);
    const S = host.frame();
    const joinFrame = c.joinFrame ?? S;
    const endFrame = S + c.durationFrames;

    const sch = new Scheduler();
    const hostClock = () => sch.now * (1 + hostCfg.drift);
    hostClockF = () => bootT + hostClock();
    const frozen = (cf, t) => cf.freezes.some(([a, d]) => t >= a && t < a + d);
    const he = engineOf(host);
    const hub = new HostHub(he, hostClock, c.hostOpts);
    const hostBacklog = [];

    // --- who plays: (device, seat) -> a pad function of (frame, own screen) ---
    const extra = new Map();
    const playerFn = (dev, seat) => {
        if (sc.pad) { const p = sc.pad(dev, seat); if (p) return p; }
        if (dev === 0 && seat === 0) return sc.p1;
        if (dev === 1 && seat === 0) return sc.p2;
        const k = dev * 4 + seat;
        if (!extra.has(k)) { const h = human(c.seed * 131 + k * 17 + 5); extra.set(k, (f) => (f < BOOT_END ? 0 : h(f))); }
        return extra.get(k);
    };

    // --- guests ---
    const specs = (c.guests || [{ dev: 1 }]).map((g, i) => Object.assign({ dev: i + 1, join: null, leave: null, rejoin: null, seats: [], link: null }, g));
    const G = [];
    function newGuest(spec, gen) {
        const gc = Object.assign({ rate: 60, drift: 0, freezes: [] }, c.guest, spec.guest || {});
        const g = { spec, gen, dev: spec.dev, cfg: gc, inst: null, gs: null, link: null, started: false, gone: false,
                    backlog: [], cmpNext: 0, compared: 0, equal: 0, skipped: 0, firstMismatch: null, stepMs: [], stepMax: 0, leadMax: 0,
                    joinAt: null, seatsOn: 1, seatIdx: 0, name: `guest${spec.dev}${gen ? '.' + gen : ''}`, heardAt: 0, admitted: false };
        return g;
    }
    for (const sp of specs) G.push(newGuest(sp, 0));

    async function startGuest(g) {
        g.inst = await createInstance({ modulePath: c.modulePath, rom: c.rom, name: g.name, render: c.renderGuest && g.dev === 1,
                                        audio: (g.dev === 1 ? c.guestAudio : c.guestAudioN) || null, clock: () => sch.now * (1 + g.cfg.drift),
                                        modulePre: c.guestModulePre || null });
        if (c.guestPre) c.guestPre(g.inst, g.dev);
        if (!g.inst.M._PortLnStart(2, c.kfEvery, c.kfCount, c.guestFlags ?? (c.flags | F_HASH_EVERY)))
            throw new Error('guest PortLnStart failed (memory for keyframes?)');
        g.link = new Link(sch, c.seed * 1000003 + 17 + (g.dev - 1) * 7919 + g.gen * 104729, Object.assign({}, spec(g).link || c.link));
        const ge = engineOf(g.inst);
        const guestClock = () => sch.now * (1 + g.cfg.drift);
        g.tx = {
            datagram: (b) => g.link.datagram('gh', b, deliverHost(() => hostRecv(g, 'd', b))),
            reliable: (m, n) => g.link.reliable('gh', m, n, deliverHost(() => hostRecv(g, 'r', m))),
        };
        g.hostTx = {
            datagram: (b) => g.link.datagram('hg', b, deliverGuest(g, (x) => g.gs.onDatagram(x))),
            reliable: (m, n) => g.link.reliable('hg', m, n, deliverGuest(g, (x) => g.gs.onReliable(x))),
        };
        g.gs = new GuestSession(ge, g.tx, guestClock, Object.assign({ lockstep: c.lockstep,
            leadCap: c.kfEvery * (c.kfCount - 2) - 10 }, c.guestOpts));
        g.started = true;
        g.joinAt = host.frame();
        g.cmpNext = g.cmpStart = Math.max(joinFrame, host.frame());
        // connected when the host starts: in from the first frame (web/online.js
        // newHostSession for each ready peer); later, a 'hi' the host admits
        if (host.frame() <= S) admit(g, 'start');
        else g.tx.reliable({ t: 'hi' }, 16);
        sch.at(sch.now, () => guestTick(g));
    }
    const spec = (g) => g.spec;
    const deliverHost = (fn) => () => { if (frozen(hostCfg, sch.now)) hostBacklog.push(fn); else fn(); };
    const deliverGuest = (g, fn) => (x) => {
        if (g.gone) return;
        if (frozen(g.cfg, sch.now) || !g.started) g.backlog.push(() => fn(x)); else fn(x);
    };
    function admit(g, why) {
        if (hub.get(g.dev)) return;
        const hs = hub.add(g.dev, g.hostTx);
        g.admitted = true;
        g.hs = hs;
        res.churn.push({ t: +sch.now.toFixed(0), f: host.frame(), dev: g.dev, what: 'admit', why, slot: hs.slot });
    }
    function hostRecv(g, kind, m) {
        if (kind === 'r' && m.t === 'bye') {
            if (hub.get(g.dev) === g.hs) { hub.remove(g.dev); res.churn.push({ t: +sch.now.toFixed(0), f: host.frame(), dev: g.dev, what: 'bye' }); }
            return;
        }
        if (g.gone) return;
        g.heardAt = sch.now;
        if (kind === 'r' && m.t === 'hi') { admit(g, 'hi'); return; }
        let hs = hub.get(g.dev);
        // heard again after the host gave it up: a link that came back
        if (!hs && kind === 'd' && g.admitted) { admit(g, 'back'); hs = hub.get(g.dev); }
        if (!hs) return;
        if (kind === 'd') hs.onDatagram(m); else hs.onReliable(m);
    }

    // --- coverage, from the host's own logs and state ---
    const cov = { rooms: new Set(), arrivals: { 1: 0, 2: 0, 3: 0, 4: 0 }, carries: 0, deaths: [0, 0, 0, 0], pauses: { 0: 0, 1: 0 },
                  settingsPage: { 0: 0, 1: 0 }, phone: [0, 0], bossFrames: 0, abilities: new Set(), warps: 0, views: 0, pokes: 0,
                  slotChanges: 0, humansMax: 0 };
    let lastObs = host.observe(), lastK = [0, 1, 2, 3].map((i) => host.kirby(i));
    let hostLogIdx = host.logs.length;
    const scanHostLogs = () => {
        for (; hostLogIdx < host.logs.length; hostLogIdx++) {
            const l = host.logs[hostLogIdx];
            let m;
            if ((m = /localmp: f\d+ player (\d) arrives in room/.exec(l))) cov.arrivals[m[1]]++;
            else if (/localmp: f\d+ player \d -> room/.test(l)) cov.carries++;
            else if (/localmp: f\d+ player \d (joins|leaves)/.test(l)) cov.slotChanges++;
            else if (/warp: /.test(l) && /deliver|->|room/.test(l)) cov.warps++;
        }
    };
    const observeHost = () => {
        const o = host.observe(), k = [0, 1, 2, 3].map((i) => host.kirby(i));
        cov.rooms.add(k[0].room);
        if (o.room === 324) cov.bossFrames++;
        if (o.numHumans > cov.humansMax) cov.humansMax = o.numHumans;
        for (let i = 0; i < 4; i++) if (lastK[i].hp > 0 && k[i].hp <= 0) cov.deaths[i]++;
        for (let i = 0; i < 2; i++) {
            if (o.battery[i] < lastObs.battery[i]) cov.phone[i]++;
            if (o.ability[i]) cov.abilities.add(o.ability[i]);
        }
        if (o.interlude && !lastObs.interlude && o.pauser < 2) cov.pauses[o.pauser]++;
        if (o.interlude && o.menuId === 8 && !(lastObs.interlude && lastObs.menuId === 8) && o.pauser < 2) cov.settingsPage[o.pauser]++;
        lastObs = o; lastK = k;
    };

    // --- the independent comparator: every frame final on a guest against
    //     the host's hash of the same frame, whole image and regions ---
    const regionNames = host.tel.regions;
    const compare = (g) => {
        if (!g.inst || g.gone) return;
        const fl = g.inst.tel.read().floor;
        if (g.cmpNext < fl) { g.skipped += fl - g.cmpNext; g.cmpNext = fl; }
        const stop = g.leaveFrame ?? endFrame;
        for (;;) {
            if (g.cmpNext >= Math.min(endFrame, stop)) break;
            const gh = g.inst.hashAt(g.cmpNext);
            if (!gh || !gh.final) break;
            const h = host.hashAt(g.cmpNext);
            if (!h) { g.skipped++; g.cmpNext++; continue; }
            g.compared++;
            if (gh.lo === h.lo && gh.hi === h.hi) g.equal++;
            else {
                const regs = regionNames.filter((_, i) => gh.rg[i] !== h.rg[i]);
                if (res.mismatches.length < 200) res.mismatches.push({ dev: g.dev, f: g.cmpNext, regions: regs, t: +sch.now.toFixed(1) });
                if (g.firstMismatch === null) g.firstMismatch = g.cmpNext;
            }
            g.cmpNext++;
        }
    };

    const stepMsHost = [];
    let hostStepMax = 0;
    const t0wall = performance.now();
    const dumps = {};
    async function stepOne(inst, g) {
        const t = performance.now();
        await inst.step();
        const d = performance.now() - t;
        if (g) { g.stepMs.push(d); if (d > g.stepMax) g.stepMax = d; }
        else { stepMsHost.push(d); if (d > hostStepMax) hostStepMax = d; }
        if (c.dumpAt !== null) {
            const dp = inst.dump();
            if (dp) dumps[g ? (g.dev === 1 ? 'guest' : g.name) : 'host'] = dp;
        }
    }

    // --- host ticks ---
    let hostDue = 0, hostRan = 0;
    const reanchors = { host: 0 };
    const hostTickMs = (hostCfg.rate === 30 ? 2 : 1) * PERIOD / (1 + hostCfg.drift);
    let evIdx = 0;
    while (evIdx < sc.events.length && sc.events[evIdx].f < S) evIdx++;
    let done = false;
    if (c.dumpAt !== null) host.M._PortLnDumpAt(c.dumpAt);
    const hostSeats = (c.hostSeats || []).slice().sort((a, b) => a[0] - b[0]);
    let hostSeatIdx = 0, hostSeatsOn = 1;
    const seatKeys = (inst, dev, on, f, obs) => {
        // seats 1..3 through the host keys (what a device layer sets), read back
        // the way the page reads them: the narrow seam
        const k = [0, 0, 0, 0];
        k[0] = playerFn(dev, 0)(f, obs);
        for (let s = 1; s < 4; s++) {
            inst.M._PortLmpSetHostKeys(s, on & (1 << s) ? playerFn(dev, s)(f, obs) : 0);
            k[s] = inst.M._PortLmpHostKeys(s);
        }
        return k;
    };

    async function hostTick() {
        if (done) return;
        if (frozen(hostCfg, sch.now)) { sch.at(sch.now + hostTickMs, hostTick); return; }
        for (const fn of hostBacklog.splice(0)) fn();
        // liveness: a guest not heard from for DEAD_MS is gone
        for (const d of hub.devs()) {
            const g = G.find((x) => x.dev === d && x.started && !x.gone);
            const hs = hub.get(d);
            if (hostClock() - hs.lastHeard > c.deadMs) {
                hub.remove(d);
                res.churn.push({ t: +sch.now.toFixed(0), f: host.frame(), dev: d, what: 'dead' });
                void g;
            }
        }
        hostDue += hostTickMs * (1 + hostCfg.drift) / PERIOD;
        let n = Math.floor(hostDue) - hostRan;
        if (n > 8) { reanchors.host++; hostRan = Math.floor(hostDue) - 1; n = 1; }
        n = Math.min(n, 3);
        for (let i = 0; i < n; i++) {
            const f = host.frame();
            // guests that connect now, leave now, come back now
            for (const g of G) {
                if (!g.started && !g.gone && f >= (g.spec.join ?? joinFrame)) await startGuest(g);
                if (g.started && !g.gone && g.spec.leave !== null && f >= g.spec.leave) {
                    g.tx.reliable({ t: 'bye' }, 8);
                    g.gone = true; g.leaveFrame = f;
                    res.churn.push({ t: +sch.now.toFixed(0), f, dev: g.dev, what: 'leave' });
                    if (g.spec.rejoin !== null) {
                        const ng = newGuest(Object.assign({}, g.spec, { join: g.spec.rejoin, leave: null, seats: g.spec.seatsAfter || [] }), g.gen + 1);
                        G.push(ng);
                    }
                }
            }
            while (hostSeatIdx < hostSeats.length && hostSeats[hostSeatIdx][0] <= f) {
                const [, seat, on] = hostSeats[hostSeatIdx++];
                const r = host.M._PortLmpRequestHuman(seat, on);
                if (on) hostSeatsOn |= 1 << seat; else hostSeatsOn &= ~(1 << seat);
                res.churn.push({ t: +sch.now.toFixed(0), f, dev: 0, what: on ? 'local join' : 'local leave', seat, accepted: r });
            }
            while (evIdx < sc.events.length && sc.events[evIdx].f <= f) {
                const e = sc.events[evIdx++];
                he.event(e.type, e.a, e.b, e.c, e.d);
                if (e.type === 2) cov.views++; else if (e.type === 4) cov.pokes++; else if (e.type === 6) cov.paints = (cov.paints || 0) + 1;
            }
            lastObs.me = host.kirby(0); lastObs.enemy = host.findEnemy();
            hub.beforeFrame(f, seatKeys(host, 0, hostSeatsOn, f, lastObs));
            await stepOne(host, null);
            hostRan++;
            observeHost();
            if (c.onHostFrame) c.onHostFrame(host, host.frame());
            if (host.frame() >= endFrame + 240) done = true;
        }
        scanHostLogs();
        hub.afterTick();
        sch.at(sch.now + hostTickMs, hostTick);
    }

    async function guestTick(g) {
        if (done || g.gone) return;
        const tickMs = (g.cfg.rate === 30 ? 2 : 1) * PERIOD / (1 + g.cfg.drift);
        if (frozen(g.cfg, sch.now)) { sch.at(sch.now + tickMs, () => guestTick(g)); return; }
        for (const fn of g.backlog.splice(0)) fn();
        const nominal = g.cfg.rate === 30 ? 2 : 1;
        const n = g.gs.framesToRun(nominal);
        const seats = (g.spec.seats || []).slice().sort((a, b) => a[0] - b[0]);
        for (let i = 0; i < n; i++) {
            const f = g.inst.frame();
            // local joins on this device, by its own frame (its controllers)
            while (g.seatIdx < seats.length && seats[g.seatIdx][0] <= f && g.gs.joined) {
                const [, seat, on] = seats[g.seatIdx++];
                const r = g.inst.M._PortLmpRequestHuman(seat, on);
                if (on) g.seatsOn |= 1 << seat; else g.seatsOn &= ~(1 << seat);
                res.churn.push({ t: +sch.now.toFixed(0), f, dev: g.dev, what: on ? 'local join' : 'local leave', seat, accepted: r });
            }
            if (g.dev === (c.corruptDev || 1) && !g.gen) for (const cz of c.corrupt) if (!cz.done && f >= cz.frame && g.gs.joined) {
                cz.done = true;
                cz.apply(g.inst);
                res.corrupt.push({ what: cz.what, frame: f, t: sch.now, tel0: g.inst.tel.read(), target: g.inst.lastCorrupt || null });
            }
            if (c.forceRewind && g.gs.joined && f % c.forceRewind.every === 0 && f > g.cmpStart + 60)
                g.inst.M._PortLnForceRewind(c.forceRewind.depth);
            const obs = g.inst.observe(); obs.me = g.inst.kirby(Math.max(1, g.inst.M._PortLnSeatSlot(g.dev, 0))); obs.enemy = g.inst.findEnemy();
            g.gs.beforeFrame(f, f >= joinFrame ? seatKeys(g.inst, g.dev, g.seatsOn, f, obs) : 0);
            await stepOne(g.inst, g);
        }
        if (g.gs.joined && n > 0) {
            const tel = g.inst.tel.read();
            g.leadMax = Math.max(g.leadMax, g.inst.frame() - tel.sealedUpTo);
        }
        g.gs.afterTick();
        compare(g);
        if (c.onGuestTick) c.onGuestTick(g);
        if (g.dev === (c.corruptDev || 1) && !g.gen) for (const r of res.corrupt) {
            const tel = g.inst.tel.read();
            if (r.detectT === undefined) {
                if (tel.mismatches > r.tel0.mismatches) { r.detectT = sch.now; r.detectFrame = tel.firstMismatch; }
                if (tel.rewinds > r.tel0.rewinds && r.healedByRewind === undefined) r.rewindAfter = true;
            } else if (r.repairT === undefined && tel.imagesLoaded > r.tel0.imagesLoaded) r.repairT = sch.now;
        }
        sch.at(sch.now + tickMs, () => guestTick(g));
    }

    for (const g of G) if ((g.spec.join ?? joinFrame) <= S) await startGuest(g);
    sch.at(0, hostTick);
    let lastWallCheck = performance.now();
    while (!done) {
        const e = sch.pop();
        if (!e) break;
        await e.fn();
        if (performance.now() - lastWallCheck > 5000) {
            lastWallCheck = performance.now();
            if (performance.now() - wall0 > c.maxWallMs) { res.aborted = 'wall-clock limit'; break; }
            if (c.progress) c.progress(`${c.name}: host f${host.frame()}/${endFrame} ${G.filter((g) => g.inst).map((g) => `${g.name} f${g.inst.frame()}`).join(' ')} t=${(sch.now / 1000).toFixed(0)}s`);
        }
    }
    for (const g of G) compare(g);

    // --- results ---
    const ht = host.tel.read();
    const gameMin = (host.frame() - S) * PERIOD / 60000;
    const ht0 = host.tel.read();
    const perGuest = G.filter((g) => g.inst).map((g) => guestResult(g, host, ht0, endFrame));
    const sum = (k) => perGuest.reduce((n, x) => n + x.compare[k], 0);
    res.frames = { start: S, join: joinFrame, end: endFrame, hostFinal: host.frame(), guestFinal: perGuest[0] ? perGuest[0].frame : null, gameMinutes: +gameMin.toFixed(2) };
    res.compare = { compared: sum('compared'), equal: sum('equal'), mismatched: sum('mismatched'), skipped: sum('skipped'),
                    firstMismatch: perGuest.map((x) => x.compare.firstMismatch).filter((x) => x !== null).sort((a, b) => a - b)[0] ?? null,
                    expected: sum('expected') };
    res.host = { tel: ht, session: hubStats(G), stepMs: dist(stepMsHost), reanchors: reanchors.host, hub: hub.st };
    const g1 = perGuest[0];
    res.guest = g1 ? { tel: g1.tel, session: g1.session, stepMs: g1.stepMs, lead: g1.lead, margin: g1.margin, leadMax: g1.leadMax, rtt: g1.rtt } : null;
    res.feel = g1 ? g1.feel : null;
    res.guests = perGuest.map((x) => Object.assign({}, x, { tel: undefined, corrAll: undefined }));
    res.guestTels = perGuest.map((x) => x.tel);
    const corr = g1 ? g1.corrAll : [];
    res.corrList = corr.slice(0, 20);
    const own = (x) => Math.hypot(x.dx1, x.dy1), oth = (x) => Math.hypot(x.dx0, x.dy0);
    res.bigCorrections = corr.filter((x) => (x.sameRoom & 2) && own(x) > 16 || (x.sameRoom & 1) && oth(x) > 16).length;
    res.topOwn = corr.filter((x) => x.sameRoom & 2).sort((a, b) => own(b) - own(a)).slice(0, 8);
    res.topOther = corr.filter((x) => x.sameRoom & 1).sort((a, b) => oth(b) - oth(a)).slice(0, 8);
    res.link = g1 ? g1.link : null;
    res.audio = { host: host.audioStats(), guest: G[0] && G[0].inst ? G[0].inst.audioStats() : null };
    res.coverage = Object.assign({}, cov, { rooms: [...cov.rooms].sort((a, b) => a - b), abilities: [...cov.abilities] });
    res.owners = [0, 1, 2, 3].map((s) => host.M._PortLnSlotOwner(s));
    res.wallSec = +((performance.now() - t0wall) / 1000).toFixed(1);
    res.dumps = c.dumpAt !== null ? dumps : undefined;
    res.hostLogs = host.logs.filter((l) => /lmpnet|error|ERROR|fault|trap/i.test(l)).slice(-60);
    res.guestLogs = G[0] && G[0].inst ? G[0].inst.logs.filter((l) => /lmpnet|error|ERROR|fault|trap/i.test(l)).slice(-40) : [];
    res._host = host; res._guest = G[0] ? G[0].inst : null;
    Object.defineProperty(res, '_G', { value: G, enumerable: false });   // tools/rooms/gates.mjs; never serialised
    return res;
}

function hubStats(G) {
    // the first guest's HostSession numbers (the two-device gates read these), and each one's
    const first = G[0] && G[0].hs;
    return Object.assign({}, first ? first.st : {}, { perDev: G.filter((g) => g.hs).map((g) => Object.assign({ name: g.name }, g.hs.st)) });
}

function guestResult(g, host, ht, endFrame) {
    const gt = g.inst.tel.read();
    const corr = g.inst.corrections().map((o) => Object.fromEntries(host.tel.corrNames.map((n, i) => [n, o[i]])));
    const until = Math.min(g.leaveFrame ?? host.frame(), host.frame());
    const mins = Math.max((until - g.joinAt) * PERIOD / 60000, 1e-9);
    const rew = corr.filter((x) => x.kind === 1);
    const disp = (arr, who) => arr.filter((x) => x.sameRoom & (who === 0 ? 1 : 2)).map((x) => Math.hypot(who === 0 ? x.dx0 : x.dx1, who === 0 ? x.dy0 : x.dy1));
    const st = g.gs.st;
    const expected = Math.max(0, Math.min(g.leaveFrame ?? endFrame, endFrame) - g.cmpStart);
    const miss = ht[`d${g.dev}KeyMissing`] || 0, on = ht[`d${g.dev}KeyOnTime`] || 0;
    return {
        name: g.name, dev: g.dev, gen: g.gen, joinAt: g.joinAt, leaveFrame: g.leaveFrame ?? null, frame: g.inst.frame(), minutes: +mins.toFixed(2),
        compare: { compared: g.compared, equal: g.equal, mismatched: g.compared - g.equal, skipped: g.skipped, firstMismatch: g.firstMismatch, expected },
        tel: gt, session: Object.assign({}, st, { leadSamples: undefined, marginSamples: undefined }),
        stepMs: dist(g.stepMs), lead: dist(st.leadSamples), margin: dist(st.marginSamples), leadMax: g.leadMax, rtt: g.gs.rtt,
        link: g.link ? g.link.st : null, corrAll: corr,
        feel: {
            rewindsPerMin: +(gt.rewinds / mins).toFixed(1),   // the correction records stop at 4096
            replayedFramesPerSec: +(gt.replayedFrames / (mins * 60)).toFixed(1),
            depth: dist(rew.map((x) => x.depth)), replayed: dist(rew.map((x) => x.replayed)),
            ownDisp: dist(disp(rew, 1)), otherDisp: dist(disp(rew, 0)),
            ownDispNonzero: rew.filter((x) => (x.sameRoom & 2) && (x.dx1 || x.dy1)).length,
            otherDispNonzero: rew.filter((x) => (x.sameRoom & 1) && (x.dx0 || x.dy0)).length,
            ownOverriddenPct: +(100 * gt.ownOverridden / Math.max(1, gt.sealedRecv)).toFixed(2),
            mispredHostPct: +(100 * gt.mispredHost / Math.max(1, gt.sealedRecv)).toFixed(2),
            mispredOtherPct: +(100 * gt.mispredOther / Math.max(1, gt.sealedRecv)).toFixed(2),
            hostKeyLatePct: +(100 * miss / Math.max(1, miss + on)).toFixed(2),
            holdsPerMin: +(st.holds / mins).toFixed(1), skipsPerMin: +(st.skips / mins).toFixed(1), burstsPerMin: +(st.bursts / mins).toFixed(2),
            capHolds: st.capHolds, worstGuestStepMs: +g.stepMax.toFixed(2),
            imageCorrections: corr.filter((x) => x.kind === 2).length,
            roomChangeCorrections: corr.filter((x) => x.kind === 1 && x.sameRoom !== 3).length,
            soundsMispredicted: gt.soundsSpurious + gt.soundsMissed, soundsJudged: gt.soundsJudged,
        },
    };
}

function summaryCfg(c) {
    return { seed: c.seed, link: c.link, host: c.host, guest: c.guest, kfEvery: c.kfEvery, kfCount: c.kfCount,
             durationFrames: c.durationFrames, joinFrame: c.joinFrame, lockstep: c.lockstep, forceRewind: c.forceRewind,
             flags: c.flags, renderGuest: c.renderGuest, corrupt: (c.corrupt || []).map((x) => ({ what: x.what, frame: x.frame })),
             hostAudio: c.hostAudio || null, guestAudio: c.guestAudio || null,
             guests: c.guests ? c.guests.map((g) => Object.assign({}, g, { link: g.link ? Object.assign({}, g.link) : undefined })) : null, hostSeats: c.hostSeats };
}
