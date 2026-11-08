#ifndef GUARD_PORT_SIMBLOCK_H
#define GUARD_PORT_SIMBLOCK_H
/* The port simulation block: simulation state the port owns, at a fixed GBA
 * address, so that it is inside every snapshot, every hash and every state
 * transfer, and means the same bytes on every host.
 *
 * docs/design/NETPLAY-V2.md §3.1 (memory classes) and M1; ROADMAP.md settles
 * the address.  0x02040000 is the first byte past EWRAM: nothing on the
 * console decodes it (it mirrors EWRAM there), wasm has it as ordinary linear
 * memory below GLOBAL_BASE, and the native hosts map it as part of the
 * "EWRAM overrun" span (platform/native/mem.c).  PORT_OVERRUN_TRAP=1 on a
 * Linux x86-64 native build reports any game access to 0x02040000..0x020C0000;
 * none of the five committed recordings makes one.
 *
 * The rule (tools/check_sim_state.py, `make lint-simstate`): a writable
 * object in a simulation translation unit lives in the GBA map or here, or it
 * is on the allowlist with its reason.
 *
 * Layout.  Everything below is in units the GBA would have used -- u8, u16,
 * u32, s32, and GBA addresses as u32 -- never a host pointer, never a size_t,
 * and only the natural padding of those types, which every target this
 * builds for lays out alike (asserted at the bottom of this file), so the
 * block is the same bytes on wasm32, ILP32 and LP64.
 *
 * Seat count.  PORT_MAX_SEATS sizes every per-seat array.  It is 4 by default
 * because the game has four Kirby slots; -DPORT_MAX_SEATS=8 is the experiment
 * NETPLAY-V2 "Simulation past 4 players" discusses.  Nothing in this block
 * assumes 4; `seats` in the header records what the build was made with, and
 * snapshots carry it. */

#include "gba/types.h"

#ifndef PORT_MAX_SEATS
#define PORT_MAX_SEATS 4
#endif

#define PORT_SIM_BASE      0x02040000u
#define PORT_SIM_SPAN      0x00040000u   /* reserved: to 0x02080000           */
#define PORT_SIM_MAGIC     0x4D49534Bu   /* 'KSIM'                            */
#define PORT_SIM_VERSION   1u

/* Rando: two buffers per seat for the rewritten object lists (rando.c). */
#define PORT_RANDO_LIST_BYTES 2048

/* The mod sim store (NETPLAY-V2 §11, MODDING.md "XRAM").  Reserved, not yet
 * used: `modStoreUsed` stays 0, and the snapshot covers only what is used. */
#define PORT_SIM_MOD_STORE_BYTES 0x10000u

struct PortSimHeader {          /* 0x00 */
    u32 magic;                  /* PORT_SIM_MAGIC once initialised           */
    u32 version;
    u32 usedBytes;              /* the snapshot/hash covers [0, usedBytes)   */
    u16 seats;                  /* PORT_MAX_SEATS of the build               */
    u16 flags;
    u32 modStoreUsed;           /* bytes of the mod store in use             */
    u32 reserved[3];
};                              /* 0x20 */

/* The frame boundary state platform/main.c used to keep in host .bss. */
struct PortSimFrame {           /* 0x20 */
    u32 frameCount;             /* frames since power-on                     */
    s32 vblankBudget;           /* VBlank DMA bytes left (PortVBlankConsume) */
    u16 keysDown;               /* what the next latch writes to KEYINPUT    */
    u16 pad;
    u32 reserved;
};                              /* 0x10 */

/* Spare.  Not the PPU's affine reference latches: platform/ppu.c re-latches
 * them from the I/O registers at the top of every scan, so at the frame
 * boundary they are dead, and their value there depends on the view's height
 * (presentation).  They stay in the rollback ring as host bytes. */
struct PortSimSpare {           /* 0x30 */
    u32 reserved[4];
};                              /* 0x10 */

/* A warp asked for but not yet delivered (platform/warp.c). */
struct PortSimWarp {            /* 0x40 */
    u16 room;
    u8  cellX, cellY;
    u8  player;
    u8  pending;
    u16 pad;
    u32 reserved[2];
};                              /* 0x10 */

/* The session view the game's spawn/despawn windows are sized for, latched
 * once a frame from the input log's view events (platform/view.c,
 * PortSimViewLatch).  240x160 and stock = 1 at power-on. */
struct PortSimView {            /* 0x50 */
    s32 viewW, viewH;           /* this frame's session view                 */
    s32 prevW, prevH;           /* the previous frame's                      */
    s32 stock;                  /* both 240x160 and no --cull none           */
    u32 reserved[3];
};                              /* 0x20 */

/* Game globals the decompilation had in IWRAM by way of assembly (crt0) or
 * kept in host .bss, now here.  The game names them through the macros in
 * PORT_SIM_GAME_* below, so the game source is unchanged at the use sites. */
struct PortSimGame {            /* 0x70 */
    u32 hblankIntrs[4];         /* gHBlankIntrs: PTR32_TD(HBlankFunc)[4]     */
    u8  inputRecorder[12];      /* gInputRecorder: struct InputRecorder      */
    u32 tapeBuffer;             /* input_recorder.c sTapeBuffer: EWRAM addr  */
    u32 reserved[8];
};                              /* 0x40 */

/* The timeline's derived state: which peer drives each Kirby slot, how many
 * slots are in play, and the story floor (platform/rollback.c).  Changed only
 * by session events applied at the top of a frame; in the snapshot so a
 * snapshot restore reproduces it. */
struct PortSimSession {         /* 0xB0 */
    s8  slotPeer[PORT_MAX_SEATS];
    u8  slotsInPlay;
    u8  storyFloor;
    u16 pad;
};

/* The randomizer's rewritten object lists (platform/rando.c).  The game's
 * LevelInfo::objlistPtr points into `list`, so these are GBA addresses and
 * the same on every host. */
struct PortSimRandoHdr {        /* struct LevelInfo_1E4, 16 bytes            */
    u32 words[4];
};
struct PortSimRando {
    u8 parity[PORT_MAX_SEATS];
    struct PortSimRandoHdr hdr[PORT_MAX_SEATS][2];
    u8 list[PORT_MAX_SEATS][2][PORT_RANDO_LIST_BYTES];
};

#define PORT_SIM_ALIGN4(n) (((n) + 3u) & ~3u)

struct PortSimBlock {
    struct PortSimHeader  hdr;
    struct PortSimFrame   frame;
    struct PortSimSpare   spare;
    struct PortSimWarp    warp;
    struct PortSimView    view;
    struct PortSimGame    game;
    struct PortSimSession session;
    struct PortSimRando   rando;
    /* the mod store follows `rando`, at PortSimModStore() */
};

#define gPortSim ((struct PortSimBlock *)(uintptr_t)PORT_SIM_BASE)

/* hdr.flags */
#define PORT_SIM_F_SESSION 0x0001u  /* a NETPLAY-V2 session: one world, many peers */


/* Fixed offsets the game-side macros use; asserted below. */
#define PORT_SIM_OFF_FRAME      0x20u
#define PORT_SIM_OFF_GAME       0x70u
#define PORT_SIM_GAME_HBLANK    (PORT_SIM_OFF_GAME + 0x00u)
#define PORT_SIM_GAME_INPUTREC  (PORT_SIM_OFF_GAME + 0x10u)
#define PORT_SIM_GAME_TAPEBUF   (PORT_SIM_OFF_GAME + 0x1Cu)
#define PORT_SIM_OFF_SESSION    0xB0u

/* Bytes of the block that are state: the snapshot and the hash cover these. */
#define PORT_SIM_CORE_BYTES ((u32)sizeof(struct PortSimBlock))
#define PortSimModStore() ((u8 *)(uintptr_t)(PORT_SIM_BASE + PORT_SIM_ALIGN4(PORT_SIM_CORE_BYTES)))
#define PortSimUsedBytes() (PORT_SIM_ALIGN4(PORT_SIM_CORE_BYTES) + gPortSim->hdr.modStoreUsed)

#if defined(__cplusplus)
#define PORT_SIM_ASSERT(c, m) static_assert(c, m)
#else
#define PORT_SIM_ASSERT(c, m) _Static_assert(c, m)
#endif
PORT_SIM_ASSERT(sizeof(struct PortSimHeader) == 0x20, "sim block header");
PORT_SIM_ASSERT(sizeof(struct PortSimFrame) == 0x10, "sim block frame");
PORT_SIM_ASSERT(sizeof(struct PortSimGame) == 0x40, "sim block game");
PORT_SIM_ASSERT(sizeof(struct PortSimView) == 0x20, "sim block view");
PORT_SIM_ASSERT(sizeof(struct PortSimRandoHdr) == 16, "rando header must be a LevelInfo_1E4");
PORT_SIM_ASSERT(__builtin_offsetof(struct PortSimBlock, frame) == PORT_SIM_OFF_FRAME, "sim block layout");
PORT_SIM_ASSERT(__builtin_offsetof(struct PortSimBlock, game) == PORT_SIM_OFF_GAME, "sim block layout");
PORT_SIM_ASSERT(__builtin_offsetof(struct PortSimBlock, session) == PORT_SIM_OFF_SESSION, "sim block layout");
PORT_SIM_ASSERT(__builtin_offsetof(struct PortSimBlock, rando) % 4 == 0, "sim block layout");
PORT_SIM_ASSERT(PORT_SIM_ALIGN4(sizeof(struct PortSimBlock)) + PORT_SIM_MOD_STORE_BYTES
                <= PORT_SIM_SPAN, "the sim block outgrew its reservation");

#ifdef __cplusplus
extern "C" {
#endif
/* Power-on: zero the block and write the header.  Called once, before the
 * game's first instruction (platform/main.c); a soft reset does not clear it,
 * because the session state in it outlives the game's own reset. */
void PortSimBlockInit(void);
#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_SIMBLOCK_H */
