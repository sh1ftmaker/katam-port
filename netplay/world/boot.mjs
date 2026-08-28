// Boot-to-world through the C exports alone: the rb_boot.js recipe (F0, the
// baked menu script, the activation events) without a relay or a lobby.
// Shared by the world server and the headless tests; the constants are
// session-defining and must match web/rb_boot.js while both exist.

export const F0 = 2200;             /* the world is up by here                */
export const DEPTH = 16;
export const AHEAD = 8;             /* frames confirmed past the frame loop   */
const EV_PLAYERS = 1, EV_NETPLAY = 3, EV_STORY = 4;
const PRESSES = [[480, 1], [500, 0], [560, 1], [580, 0], [720, 1], [740, 0],
                 [810, 0x80], [825, 0], [850, 1], [870, 0]];

export function bootMask(f) {
    if (f >= 300 && f < 440) return ((f - 300) % 8) < 4 ? 1 : 0;
    let mask = 0;
    for (const [at, m] of PRESSES) if (f >= at) mask = m;
    return f < 900 ? mask : 0;
}

/* Arm a freshly initialised module (ROM in place, before its first frame) to
 * replay the boot with the picture off.  Returns a confirm(frame) the host
 * calls every present so the timeline stays AHEAD frames past the loop
 * until the seats take over. */
export function armBoot(Module, players) {
    if (!Module._PortRbInit(DEPTH, players)) throw new Error('PortRbInit failed');
    Module._PortRbSetSelf(0);
    Module._PortRbScheduleEvent(0, EV_PLAYERS, 1, 0);
    Module._PortRbScheduleEvent(0, EV_STORY, 3, 0);
    Module._PortRbScheduleEvent(F0, EV_PLAYERS, 4, 0);
    Module._PortRbScheduleEvent(F0, EV_NETPLAY, 0, 0);
    let confirmed = -1;
    function confirmThrough(frame) {
        for (; confirmed < frame; confirmed++)
            for (let s = 0; s < players; s++)
                Module._PortRbConfirmInput(s, confirmed + 1, confirmed + 1 < F0 ? bootMask(confirmed + 1) : 0);
    }
    confirmThrough(F0 + AHEAD);
    Module._PortRbReplayTo(F0);
    return confirmThrough;
}
