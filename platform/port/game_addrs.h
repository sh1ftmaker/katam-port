#ifndef GUARD_PORT_GAME_ADDRS_H
#define GUARD_PORT_GAME_ADDRS_H
/* The game's own storage, by address -- the handful of fields the port's
 * session machinery (platform/rollback.c, platform/seats.c) reads and writes
 * directly.  Platform code must not depend on the generated tree, so the
 * addresses are written out; the offsets come from platform/port/gba_layout.h,
 * which asserts every one of them against the console layout on every build.
 *
 *   gUnk_0203AD30                  the number of Kirbys the game treats as
 *                                  network-driven -- the threshold in
 *                                  src/kirby.c:6475.
 *   gUnk_020382D0.unk8[3][4]       per-player input at offset 8: [0] held,
 *                                  [1] pressed this frame, [2] released.
 *                                  Written by the link layer at
 *                                  src/multi_08030C94.c:332 and read by Kirby.
 *   gUnk_02038590[i].unk9E         the AI controller's synthesised buttons,
 *                                  written by src/code_0800ECAC_2.c.  Stride
 *                                  244, field at 158; both asserted.
 *   gUnk_0203AD10 bit 1            the network-input branch (src/code_080332BC.c
 *                                  sets it for a link session).
 *   gUnk_0203AD3C                  gLocalPlayerId: camera, pause menu, HUD.
 *   gUnk_0203AD2C                  gAIKirbyState (the story stage).
 *   gUnk_02023354                  the display task, whose captured player id
 *                                  decides which viewport drives the BG scroll
 *                                  registers -- see GAME_REPIN_FOCUS below.
 */
#define GAME_PLAYER_COUNT (*(vu8 *)0x0203AD30)
#define GAME_HELD(slot)     (*(vu16 *)(0x020382D0 + 8      + 2 * (slot)))
#define GAME_PRESSED(slot)  (*(vu16 *)(0x020382D0 + 8 +  8 + 2 * (slot)))
#define GAME_RELEASED(slot) (*(vu16 *)(0x020382D0 + 8 + 16 + 2 * (slot)))
#define GAME_AI_INPUT(slot) (*(vu8  *)(0x02038590 + 244 * (slot) + 158))
#define GAME_MODE_FLAGS     (*(vu32 *)0x0203AD10)
#define GAME_FOCUS          (*(vu8  *)0x0203AD3C)
#define GAME_STORY          (*(vu16 *)0x0203AD2C)
#define GAME_DISPLAY_TASK   (*(vu32 *)0x02023354)

/* "Whose screen is this" lives in two places: gLocalPlayerId, read live by the
 * sprite positioning and the pause menu -- and the display task's player id,
 * captured from gLocalPlayerId at world creation, which decides which viewport
 * drives the background scroll registers (sub_08002E48 passes `p == unk0` down
 * to the BG writer).  On hardware the lobby sets gLocalPlayerId before the
 * world exists, so creation-time capture is always right; anything that
 * changes the focus after the world exists has to re-point both. */
static inline void GAME_REPIN_FOCUS(int slot)
{
    u32 task = GAME_DISPLAY_TASK;

    GAME_FOCUS = (u8)slot;
    if (task >= 0x02000000 && task < 0x03008000) {
        u16 off = *(vu16 *)(task + 6);
        u16 flags = *(vu16 *)(task + 0x12);
        u32 obj = (flags & 0x10) ? 0x02000000 + ((u32)off << 2)
                                 : 0x03000000 + off;

        *(vu8 *)obj = (u8)slot;
    }
}

#endif /* GUARD_PORT_GAME_ADDRS_H */
