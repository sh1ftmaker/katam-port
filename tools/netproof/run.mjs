// One online session, host and guest, over a simulated link, on a virtual
// clock.  Returns everything measured.  Deterministic for a given config.
import { createInstance } from './instance.mjs';
import { Scheduler, Link } from './link.mjs';
import { HostSession, GuestSession, engineOf, PERIOD } from './session.mjs';
import { BOOT_END } from './scenario.mjs';

const F_HASH_EVERY = 1, F_HOST_KEYS = 2, F_NO_SRAM_KF = 4;

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
    }, cfg);
    const hostCfg = Object.assign({ rate: 60, drift: 0, freezes: [] }, c.host);
    const guestCfg = Object.assign({ rate: 60, drift: 0, freezes: [] }, c.guest);
    const sc = c.scenario;
    const wall0 = performance.now();
    const res = { name: c.name, cfg: summaryCfg(c), events: [], coverage: {}, mismatches: [], corrupt: [] };

    // --- host: boots alone through the menus, local-multiplayer on ---
    // menus: 'session' (default: off for the boot, on from the session start, so
    // the pause SETTINGS page exists), 'boot' (on from power-on), 'off'
    const menus = c.menus || sc.menus || 'session';
    // Audio device models (c.hostAudio / c.guestAudio, instance.mjs
    // audioDevice): on each side's own clock; the host's boot runs on a
    // clock of its own frames.
    let bootT = 0, hostClockF = () => bootT, guestClockF = () => 0;
    const host = await createInstance({ modulePath: c.modulePath, rom: c.rom, name: 'host', menus: menus === 'boot' ? 1 : 0,
                                        audio: c.hostAudio || null, clock: () => hostClockF(), modulePre: c.hostModulePre || null });
    if (c.hostPre) c.hostPre(host);
    host.M._PortLmpSetHumans(sc.localmp || 2);
    // events before the session (a recording's boot: its view, early warps)
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
    // the pause SETTINGS page needs the new menus; turned on with the session
    if (menus === 'session') host.M._PortMenuPocSet(1);
    if (!host.M._PortLnStart(1, c.kfEvery, c.kfCount, c.flags | F_HASH_EVERY | (c.hostKeyframes ? F_HOST_KEYS : 0)))
        throw new Error('host PortLnStart failed');
    const S = host.frame();
    const joinFrame = c.joinFrame ?? S;
    const endFrame = S + c.durationFrames;

    // --- guest: a fresh instance that never sees the menus ---
    const guest = await createInstance({ modulePath: c.modulePath, rom: c.rom, name: 'guest', render: c.renderGuest,
                                         audio: c.guestAudio || null, clock: () => guestClockF(), modulePre: c.guestModulePre || null });
    if (c.guestPre) c.guestPre(guest);
    if (!guest.M._PortLnStart(2, c.kfEvery, c.kfCount, c.flags | F_HASH_EVERY))
        throw new Error('guest PortLnStart failed (memory for keyframes?)');

    const sch = new Scheduler();
    const link = new Link(sch, c.seed * 1000003 + 17, c.link);
    const hostClock = () => sch.now * (1 + hostCfg.drift);
    const guestClock = () => sch.now * (1 + guestCfg.drift);
    hostClockF = () => bootT + hostClock();
    guestClockF = () => guestClock();
    const frozen = (cf, t) => cf.freezes.some(([a, d]) => t >= a && t < a + d);
    const backlog = { host: [], guest: [] };
    const deliver = (who, fn) => (x) => {
        const cf = who === 'host' ? hostCfg : guestCfg;
        if (frozen(cf, sch.now) || (who === 'guest' && !guestStarted)) backlog[who].push(() => fn(x)); else fn(x);
    };
    let guestStarted = false;
    let hs, gs;
    const hostTx = {
        datagram: (b) => link.datagram('hg', b, deliver('guest', (x) => gs.onDatagram(x))),
        reliable: (m, n) => link.reliable('hg', m, n, deliver('guest', (x) => gs.onReliable(x))),
    };
    const guestTx = {
        datagram: (b) => link.datagram('gh', b, deliver('host', (x) => hs.onDatagram(x))),
        reliable: (m, n) => link.reliable('gh', m, n, deliver('host', (x) => hs.onReliable(x))),
    };
    const he = engineOf(host), ge = engineOf(guest);
    hs = new HostSession(he, hostTx, hostClock, c.hostOpts);
    gs = new GuestSession(ge, guestTx, guestClock, Object.assign({ lockstep: c.lockstep,
        leadCap: c.kfEvery * (c.kfCount - 2) - 10 }, c.guestOpts));

    // --- coverage, from the host's own logs and state ---
    const cov = { rooms: new Set(), arrivals: { 1: 0, 2: 0 }, carries: 0, deaths: [0, 0], pauses: { 0: 0, 1: 0 },
                  settingsPage: { 0: 0, 1: 0 }, phone: [0, 0], bossFrames: 0, abilities: new Set(), warps: 0, views: 0, pokes: 0 };
    let lastObs = host.observe(), lastK = [host.kirby(0), host.kirby(1)];
    let hostLogIdx = host.logs.length;
    const scanHostLogs = () => {
        for (; hostLogIdx < host.logs.length; hostLogIdx++) {
            const l = host.logs[hostLogIdx];
            let m;
            if ((m = /localmp: f\d+ player (\d) arrives in room/.exec(l))) cov.arrivals[m[1]]++;
            else if (/localmp: f\d+ player \d -> room/.test(l)) cov.carries++;
            else if (/warp: /.test(l) && /deliver|->|room/.test(l)) cov.warps++;
        }
    };
    const observeHost = () => {
        const o = host.observe(), k = [host.kirby(0), host.kirby(1)];
        cov.rooms.add(k[0].room);
        if (o.room === 324) cov.bossFrames++;
        for (let i = 0; i < 2; i++) {
            if (lastK[i].hp > 0 && k[i].hp <= 0) cov.deaths[i]++;
            if (o.battery[i] < lastObs.battery[i]) cov.phone[i]++;
            if (o.ability[i]) cov.abilities.add(o.ability[i]);
        }
        if (o.interlude && !lastObs.interlude && o.pauser < 2) cov.pauses[o.pauser]++;
        if (o.interlude && o.menuId === 8 && !(lastObs.interlude && lastObs.menuId === 8) && o.pauser < 2) cov.settingsPage[o.pauser]++;
        lastObs = o; lastK = k;
    };

    // --- the independent comparator: every frame final on the guest
    //     against the host's hash of the same frame, whole image and regions ---
    let cmpNext = joinFrame, compared = 0, equal = 0, skipped = 0, firstMismatch = null;
    const regionNames = host.tel.regions;
    const compare = () => {
        const fl = guest.tel.read().floor;
        if (cmpNext < fl) { skipped += fl - cmpNext; cmpNext = fl; }
        for (;;) {
            if (cmpNext >= endFrame) break;
            const g = guest.hashAt(cmpNext);
            if (!g || !g.final) break;
            const h = host.hashAt(cmpNext);
            if (!h) { skipped++; cmpNext++; continue; }
            compared++;
            if (g.lo === h.lo && g.hi === h.hi) equal++;
            else {
                const regs = regionNames.filter((_, i) => g.rg[i] !== h.rg[i]);
                if (res.mismatches.length < 200) res.mismatches.push({ f: cmpNext, regions: regs, t: +sch.now.toFixed(1) });
                if (firstMismatch === null) firstMismatch = cmpNext;
            }
            cmpNext++;
        }
    };

    // --- costs and feel ---
    const stepMs = { host: [], guest: [] };
    let guestStepMax = 0, hostStepMax = 0, leadMax = 0;
    const corrDone = new Set();
    const t0wall = performance.now();
    const dumps = {};

    async function stepOne(inst, which) {
        const t = performance.now();
        await inst.step();
        const d = performance.now() - t;
        stepMs[which].push(d);
        if (which === 'guest' && d > guestStepMax) guestStepMax = d;
        if (which === 'host' && d > hostStepMax) hostStepMax = d;
        if (c.dumpAt !== null) {
            const dp = inst.dump();
            if (dp) dumps[which] = dp;
        }
    }

    // host ticks
    let hostDue = 0, hostRan = 0, reanchors = { host: 0, guest: 0 };
    const hostTickMs = (hostCfg.rate === 30 ? 2 : 1) * PERIOD / (1 + hostCfg.drift);
    const guestTickMs = (guestCfg.rate === 30 ? 2 : 1) * PERIOD / (1 + guestCfg.drift);
    let evIdx = 0;
    while (evIdx < sc.events.length && sc.events[evIdx].f < S) evIdx++;
    let done = false, joinRequested = false;
    if (c.dumpAt !== null) { host.M._PortLnDumpAt(c.dumpAt); guest.M._PortLnDumpAt(c.dumpAt); }

    async function hostTick() {
        if (done) return;
        if (frozen(hostCfg, sch.now)) { sch.at(sch.now + hostTickMs, hostTick); return; }
        for (const fn of backlog.host.splice(0)) fn();
        hostDue += hostTickMs * (1 + hostCfg.drift) / PERIOD;
        let n = Math.floor(hostDue) - hostRan;
        if (n > 8) { reanchors.host++; hostRan = Math.floor(hostDue) - 1; n = 1; }
        n = Math.min(n, 3);
        for (let i = 0; i < n; i++) {
            const f = host.frame();
            if (f >= joinFrame && !joinRequested) { joinRequested = true; hs.wantImage('join'); }
            while (evIdx < sc.events.length && sc.events[evIdx].f <= f) {
                const e = sc.events[evIdx++];
                he.event(e.type, e.a, e.b, e.c, e.d);
                if (e.type === 2) cov.views++; else if (e.type === 4) cov.pokes++;
            }
            lastObs.me = host.kirby(0); lastObs.enemy = host.findEnemy();
            hs.beforeFrame(f, sc.p1(f, lastObs));
            await stepOne(host, 'host');
            hostRan++;
            observeHost();
            if (host.frame() >= endFrame + 240) done = true;
        }
        scanHostLogs();
        hs.afterTick();
        sch.at(sch.now + hostTickMs, hostTick);
    }

    let guestObs = null;
    async function guestTick() {
        if (done) return;
        if (frozen(guestCfg, sch.now)) { sch.at(sch.now + guestTickMs, guestTick); return; }
        for (const fn of backlog.guest.splice(0)) fn();
        const nominal = guestCfg.rate === 30 ? 2 : 1;
        const n = gs.framesToRun(nominal);
        for (let i = 0; i < n; i++) {
            const f = guest.frame();
            // corruption injections land on the guest while it is parked
            for (const cz of c.corrupt) if (!cz.done && f >= cz.frame && gs.joined) {
                cz.done = true;
                cz.apply(guest);
                res.corrupt.push({ what: cz.what, frame: f, t: sch.now, tel0: guest.tel.read(), target: guest.lastCorrupt || null });
            }
            if (c.forceRewind && gs.joined && f % c.forceRewind.every === 0 && f > joinFrame + 60)
                guest.M._PortLnForceRewind(c.forceRewind.depth);
            guestObs = guest.observe(); guestObs.me = guest.kirby(1); guestObs.enemy = guest.findEnemy();
            gs.beforeFrame(f, f >= joinFrame ? sc.p2(f, guestObs) : 0);
            await stepOne(guest, 'guest');
        }
        if (n === 0 && !gs.joined) { /* waiting for the image */ }
        if (gs.joined && n > 0) {
            const tel = guest.tel.read();
            leadMax = Math.max(leadMax, guest.frame() - tel.sealedUpTo);
        }
        gs.afterTick();
        compare();
        for (const r of res.corrupt) if (r.detectT === undefined) {
            const tel = guest.tel.read();
            if (tel.mismatches > r.tel0.mismatches) { r.detectT = sch.now; r.detectFrame = tel.firstMismatch; }
            if (tel.rewinds > r.tel0.rewinds && r.healedByRewind === undefined) r.rewindAfter = true;
        }
        for (const r of res.corrupt) if (r.detectT !== undefined && r.repairT === undefined) {
            const tel = guest.tel.read();
            if (tel.imagesLoaded > r.tel0.imagesLoaded) r.repairT = sch.now;
        }
        sch.at(sch.now + guestTickMs, guestTick);
    }

    sch.at(0, hostTick);
    const joinMs = Math.max(0, (joinFrame - S) * PERIOD);
    sch.at(joinMs + 0.5, () => { guestStarted = true; for (const fn of backlog.guest.splice(0)) fn(); sch.at(sch.now, guestTick); });
    let lastWallCheck = performance.now();
    while (!done) {
        const e = sch.pop();
        if (!e) break;
        await e.fn();
        if (performance.now() - lastWallCheck > 5000) {
            lastWallCheck = performance.now();
            if (performance.now() - wall0 > c.maxWallMs) { res.aborted = 'wall-clock limit'; break; }
            if (c.progress) c.progress(`${c.name}: host f${host.frame()}/${endFrame} guest f${guest.frame()} t=${(sch.now / 1000).toFixed(0)}s`);
        }
    }
    compare();

    // --- results ---
    const ht = host.tel.read(), gt = guest.tel.read();
    const corr = guest.corrections().map((o) => Object.fromEntries(host.tel.corrNames.map((n, i) => [n, o[i]])));
    const gameMin = (host.frame() - S) * PERIOD / 60000;
    const rew = corr.filter((x) => x.kind === 1);
    const disp = (arr, who) => arr.filter((x) => x.sameRoom & (who === 0 ? 1 : 2)).map((x) => Math.hypot(who === 0 ? x.dx0 : x.dx1, who === 0 ? x.dy0 : x.dy1));
    res.frames = { start: S, join: joinFrame, end: endFrame, hostFinal: host.frame(), guestFinal: guest.frame(), gameMinutes: +gameMin.toFixed(2) };
    res.compare = { compared, equal, mismatched: compared - equal, skipped, firstMismatch, expected: endFrame - joinFrame };
    res.host = { tel: ht, session: hs.st, stepMs: dist(stepMs.host), reanchors: reanchors.host };
    res.guest = { tel: gt, session: Object.assign({}, gs.st, { leadSamples: undefined, marginSamples: undefined }),
                  stepMs: dist(stepMs.guest), lead: dist(gs.st.leadSamples), margin: dist(gs.st.marginSamples), leadMax,
                  rtt: gs.rtt };
    res.feel = {
        rewindsPerMin: +(rew.length / Math.max(gameMin, 1e-9)).toFixed(1),
        replayedFramesPerSec: +(gt.replayedFrames / Math.max(gameMin * 60, 1e-9)).toFixed(1),
        depth: dist(rew.map((x) => x.depth)),
        replayed: dist(rew.map((x) => x.replayed)),
        ownDisp: dist(disp(rew, 1)), otherDisp: dist(disp(rew, 0)),
        ownDispNonzero: rew.filter((x) => (x.sameRoom & 2) && (x.dx1 || x.dy1)).length,
        otherDispNonzero: rew.filter((x) => (x.sameRoom & 1) && (x.dx0 || x.dy0)).length,
        ownOverriddenPct: +(100 * gt.ownOverridden / Math.max(1, gt.sealedRecv)).toFixed(2),
        hostKeyLatePct: +(100 * ht.hostKeyMissing / Math.max(1, ht.hostSealed)).toFixed(2),
        holdsPerMin: +(gs.st.holds / Math.max(gameMin, 1e-9)).toFixed(1),
        skipsPerMin: +(gs.st.skips / Math.max(gameMin, 1e-9)).toFixed(1),
        burstsPerMin: +(gs.st.bursts / Math.max(gameMin, 1e-9)).toFixed(2),
        capHolds: gs.st.capHolds,
        worstGuestStepMs: +guestStepMax.toFixed(2), worstHostStepMs: +hostStepMax.toFixed(2),
        imageCorrections: corr.filter((x) => x.kind === 2).length,
    };
    res.corrList = corr.slice(0, 20);
    res.feel.soundsMispredicted = gt.soundsSpurious + gt.soundsMissed; res.feel.soundsJudged = gt.soundsJudged;
    const own = (x) => Math.hypot(x.dx1, x.dy1), oth = (x) => Math.hypot(x.dx0, x.dy0);
    res.bigCorrections = corr.filter((x) => (x.sameRoom & 2) && own(x) > 16 || (x.sameRoom & 1) && oth(x) > 16).length;
    res.topOwn = corr.filter((x) => x.sameRoom & 2).sort((a, b) => own(b) - own(a)).slice(0, 8);
    res.topOther = corr.filter((x) => x.sameRoom & 1).sort((a, b) => oth(b) - oth(a)).slice(0, 8);
    res.feel.roomChangeCorrections = corr.filter((x) => x.kind === 1 && x.sameRoom !== 3).length;
    res.link = link.st;
    res.audio = { host: host.audioStats(), guest: guest.audioStats() };
    res.coverage = Object.assign({}, cov, { rooms: [...cov.rooms].sort((a, b) => a - b), abilities: [...cov.abilities] });
    res.wallSec = +((performance.now() - t0wall) / 1000).toFixed(1);
    res.dumps = c.dumpAt !== null ? dumps : undefined;
    res.hostLogs = host.logs.filter((l) => /lmpnet|error|ERROR|fault|trap/i.test(l)).slice(-40);
    res.guestLogs = guest.logs.filter((l) => /lmpnet|error|ERROR|fault|trap/i.test(l)).slice(-40);
    res._host = host; res._guest = guest;
    return res;
}

function summaryCfg(c) {
    return { seed: c.seed, link: c.link, host: c.host, guest: c.guest, kfEvery: c.kfEvery, kfCount: c.kfCount,
             durationFrames: c.durationFrames, joinFrame: c.joinFrame, lockstep: c.lockstep, forceRewind: c.forceRewind,
             flags: c.flags, renderGuest: c.renderGuest, corrupt: (c.corrupt || []).map((x) => ({ what: x.what, frame: x.frame })),
             hostAudio: c.hostAudio || null, guestAudio: c.guestAudio || null };
}
