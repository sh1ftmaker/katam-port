/* PORT: CPU AI modes -- host-selectable goals for the CPU Kirby brain
 * (docs/design/CPU-AI.md, platform/port/cpu_ai.h).
 *
 * The stock brain (code_0800ECAC*.c, docs/naming/cpu-brain.md) runs four
 * layers a frame: SCRIPT, GOAL, SENSE, ACTION.  A mode other than CLASSIC
 * replaces only the GOAL layer for that frame (CpuAiGoal, called by
 * CpuBrainTask instead of brain->layers[CPU_LAYER_GOAL]): it sets the goal
 * point and nav nodes the stock SENSE and ACTION layers already steer by,
 * and takes a door when the Kirby reaches the one it chose (the same call,
 * sub_080551FC, the stock brain makes when it happens to stand on a door).
 * Nothing new is stored in the brain's layer pointers, so no new code IDs.
 *
 * Route knowledge is computed from the loaded game data at run time: the
 * per-room door lists (gUnk_08D640A4, docs/naming/doors.md: destination
 * room of every door tile), the door tiles of the room the Kirby is in
 * (collision attribute 0x4000 and its warp descriptor), the per-room nav
 * graph (gUnk_08D63C28 via levelInfo->unk1E8), the game's own "room
 * entered" flags (gVisitedDoors bit 15) and the CPU brain's own boss-room
 * pairs (gCpuLinkedRooms).
 *
 * State.  The mode word is the sim block's spare word (port/cpu_ai.h); each
 * brain's own state is in struct CpuBrain's filler bytes, which the game
 * zeroes at CpuBrainCreate and never touches otherwise, so it is in every
 * snapshot and hash, and all zero while a brain is CLASSIC. */

#include "global.h"
#include "data.h"
#include "functions.h"
#include "kirby.h"
#include "object.h"
#include "object_collision.h"
#include "code_0800ECAC.h"
#include "treasures.h"
#include "vert_sliding_door.h"
#include "constants/kirby.h"
#include "constants/object_types.h"
#include "port/localmp.h"
#include "port/simblock.h"
#include "port/res.h"
#include "port/cpu_ai.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const u16 gCpuLinkedRooms[][2];
void CpuUpdateGoalVectors(struct CpuBrain *);
void CpuSenseMoving(struct CpuBrain *);
bool32 CpuWarpToRoom(struct CpuBrain *, u16);
u8 CpuNavNodeAt(struct ObjectBase *);
void CpuActLeaveFlight(struct CpuBrain *);
bool16 CpuBreakBlockCheck(struct CpuBrain *);

/* brain->fillerC0 (12 bytes, 4-aligned) */
struct CpuAiState {
    u16 room;          /* the room this state is about (arrival: it differs) */
    u16 dest;          /* the room behind the chosen door, 0xFFFF none        */
    u8  doorX, doorY;  /* the chosen door tile                                */
    u16 roomFrames;    /* frames in this room                                 */
    u16 destFrames;    /* frames since the door was chosen                    */
    u8  modeSeen;      /* mode + 1 last frame (a change resets the choice)    */
    u8  doors;         /* doors walked through (diagnostics, wraps)          */
};
/* brain->fillerCE (14 bytes, 2-aligned): door choices that failed */
struct CpuAiBlocked {
    u16 from[3], to[3];
    u8  next;
    u8  lastHp;
};

/* brain->fillerF0 (4 bytes): progress toward the chosen door */
struct CpuAiProgress {
    u16 best;          /* nearest it has been, tiles (|dx| + |dy|), + 1      */
    u16 stall;         /* frames without getting nearer                      */
};

#define ST(b)  ((struct CpuAiState *)(void *)(b)->fillerC0)
#define PR(b)  ((struct CpuAiProgress *)(void *)(b)->fillerF0)
/* brain->fillerE2 (2 bytes): the room it came from, + 1 */
#define PREV(b) (*(u16 *)(void *)(b)->fillerE2)
#define BL(b)  ((struct CpuAiBlocked *)(void *)(b)->fillerCE)

#define NO_ROOM      0xFFFF
#define NO_ROUTE     0xFFFE               /* st->dest: no route found        */
#define MAX_ROOMS    RES_FIRST_NEW_ROOM   /* vanilla rooms; new rooms: no routes */
#define DOOR_GIVEUP  1800                 /* 30 s walking at one door        */
#define ROOM_GIVEUP  3600                 /* 60 s in one room: warp on       */
#define FORCE_FREE   600                  /* a room script past 10 s: ours   */

static int CpuAiMode(struct CpuBrain *brain)
{
    u32 slot = brain->kirby->base.unk56;

    if (gUnk_0203AD10 & 0x10)           /* the attract demo: the game's own */
        return PORT_CPUAI_CLASSIC;
    if (slot >= PORT_CPUAI_SLOTS)
        return PORT_CPUAI_CLASSIC;
    return PortCpuAiWordMode(gPortSim->game.reserved[0], slot);
}

/* --- the door graph ------------------------------------------------------ */

static const struct Unk_08930E00_2 *DoorList(u16 room)
{
    u16 idx;

    if (room >= MAX_ROOMS)
        return NULL;
    idx = gRoomProps[room].doorsIdx;
    if (idx == 0xFFFF || idx > 286)
        return NULL;
    return gUnk_08D640A4[idx]->unk00.pat2;
}

#define NEXT_ENTRY(e) ((const struct Unk_08930E00_2 *)(const void *)((const u8 *)(e) + (e)->unk01))

static bool32 Blocked(struct CpuBrain *brain, u16 from, u16 to)
{
    struct CpuAiBlocked *bl = BL(brain);
    int i;

    for (i = 0; i < 3; i++)
        if (bl->to[i] == to + 1 && bl->from[i] == from + 1)
            return TRUE;
    return FALSE;
}

static void BlockWhy(struct CpuBrain *brain, u16 from, u16 to, const char *why)
{
    struct CpuAiBlocked *bl = BL(brain);

    if (bl->next >= 3)
        bl->next = 0;
    bl->from[bl->next] = from + 1;
    bl->to[bl->next] = to + 1;
    bl->next++;
    PortCpuAiNote(brain->kirby->base.unk56, why, from, to, 0);
}

/* a door given up: not reached in DOOR_GIVEUP */
static void Block(struct CpuBrain *brain, u16 from, u16 to)
{
    BlockWhy(brain, from, to, "blocked");
}

enum { WANT_UNEXPLORED, WANT_BOSS, WANT_ROOM };

static bool32 BossRoomOpen(u16 room)
{
    u16 i;

    for (i = 0; i < 8; i++)
        if (gCpuLinkedRooms[i][1] == room)
            return !*GetStateSlot(STATE_SLOT_SESSION, i + 1, 0);
    return FALSE;
}

static bool32 EntryReachesChest(int t, u16 entry);
static bool32 HumanHere(u16 room);

/* `entry`: the door record it would enter `room` by (NO_REC: not known),
 * `t` its breaking tier: a room is wanted for its chests only if a chest
 * is reachable from where that door puts it. */
static bool32 Wanted(int want, u16 room, u16 target, int t, u16 entry)
{
    switch (want) {
    case WANT_UNEXPLORED:
        return !sub_08002A5C(room)
            || (sub_08002AAC(room) > sub_08002AD0(room) && EntryReachesChest(t, entry));
    case WANT_BOSS:
        return BossRoomOpen(room);
    default:
        return room == target;
    }
}

/* --- room hints: door groups --------------------------------------------
 *
 * A room's doors do not all reach one another: room 709 (Moonlight
 * Mansion) is three sealed chambers with one door each.  The game's own CPU
 * hints (the nav graph's next-hop table, docs/design/CPU-HINTS.md) know some
 * of this but are wrong in places (709's graph links the lower chamber to
 * the upper right one through solid floor), so the groups come from the
 * room's collision map: the cells a floating Kirby can pass (not solid, not
 * a Mini-only gap; a breakable block passable when the Kirby's ability
 * breaks its level), flooded from each door.  Entering a room by a door puts
 * the Kirby in the group of the cell it arrives at, and only that group's
 * doors lead on, so the route runs over (room, door group) states.
 *
 * The door records are the solidity map's tag-3 cell records (door cell ->
 * destination room and arrival cell, docs/naming/room-tables.md); the
 * arrival cell is (unk0A - 1, unk0B - 1), the +1 sub_080551FC's callers
 * add.  Per tier of block breaking, every record's group (the index, in its
 * room, of the first door of its group) and the group its arrival cell is in
 * are computed once from the ROM maps and kept: a pure function of the
 * loaded game data, the same on every device (tests/sim_state_allowlist.txt,
 * rom-cache).  The Kirby's own room uses its live map instead (blocks
 * already broken are open). */

#define HINT_RECS    1024
#define HINT_TIERS   4          /* breaks: nothing, level <= 1, <= 2, <= 3 */
#define HINT_CELLS   1950       /* sizeof gUnk_02024ED0[0] */
#define HINT_GATES   512
#define NO_REC       0xFFFF

/* A way out of a room: a door tile (the solidity map's tag-3 cell record)
 * or an exit object the door records do not have -- a warp star
 * (OBJ_WARP_STAR: template unk1E the room, unk1A/unk1C the arrival) or a
 * cannon (OBJ_CANNON: unk12 the room, unk18 the arrival, high byte x). */
enum { EXIT_DOOR, EXIT_STAR, EXIT_CANNON };
struct HintDoor {
    u16 dest;          /* the room it leads to                               */
    u8  x, y;          /* the door cell / the object's cell                  */
    u8  ax, ay;        /* the arrival cell + 1 (sub_080551FC's spawn point)   */
    u8  col;           /* the door tile's collision value (EXIT_DOOR)         */
    u8  kind;          /* EXIT_*                                             */
};

/* Gate hints: the objects that close or open a passage the collision map
 * shows open (or shows shut when it can be opened), per room, from its ROM
 * object list (docs/design/CPU-HINTS.md, "Gates").
 *   GATE_SLIDE   OBJ_VERTICAL_SLIDING_DOOR: shuts the two cells above its
 *                floor cell until the room-state slot `slot` (template
 *                unk4) is non-zero;
 *   GATE_SWITCH  OBJ_SMALL_BUTTON (counting mode): a Kirby standing on it
 *                (the cell above its floor) adds one to slot `slot` and
 *                takes it off again `timer` (template unk18) + 2 frames
 *                after it leaves; template unk22 bit 2: it stays down;
 *   GATE_CRUMBLE OBJ_DESTROYABLE_ROCK_BLOCK: a solid cell that crumbles
 *                when a Kirby stands on it, not flying: passable downwards;
 *   GATE_WALL    OBJ_TRIGGERABLE_WALL: a chain-blast source that goes off
 *                when the room has `need` (unkF) chests/items collected
 *                (sub 1) or marked enemies beaten (sub 0); the blast opens
 *                the chain cells (0x40) joined to it. */
enum { GATE_SLIDE = 1, GATE_SWITCH, GATE_CRUMBLE, GATE_WALL };
struct HintGate {
    u8  kind, x, y, slot;
    u8  need, sub;
    u16 timer;
    u8  sp2, sp3;      /* the template's spawn flag (unk2 kind, unk3 bit)    */
};

static struct HintDoor sHintRec[HINT_RECS];
static u16 sHintRoom[HINT_RECS];
static u16 sHintFirst[MAX_ROOMS];   /* first record of the room, + 1; 0 none */
static u8  sHintCount[MAX_ROOMS];
static u16 sHintTotal;
static struct HintGate sGate[HINT_GATES];
static u16 sGateFirst[MAX_ROOMS];   /* first gate of the room; count in sGateCount */
static u8  sGateCount[MAX_ROOMS];
static u16 sGateTotal;
static u32 sHintReach[HINT_TIERS][HINT_RECS]; /* doors of the room behind it reached from its arrival cell */
static u8  sHintChest[HINT_TIERS][HINT_RECS]; /* a chest of the room behind it reached from its arrival cell */
static u8  sHintBuilt;              /* bit 0: the index; bit 1 + t: tier t */
static uintptr_t sHintSig;
static u8  sHintCells[HINT_CELLS];  /* scratch, written before every read */
static u8  sHintLab[HINT_CELLS];    /* scratch */
static u16 sHintQueue[2 * HINT_CELLS];  /* scratch */
static u8  sHintIgn[HINT_CELLS];    /* scratch: a chain explosion reaches the cell */
static u8  sGateOv[HINT_CELLS];     /* scratch: the gates over the map flooded next (OV_*) */

/* sGateOv: what the gates make of a cell */
enum { OV_NONE, OV_SHUT, OV_DOWN, OV_OPEN };

/* The highest breakable level the stock brain's block breaking
 * (gCpuBreakBlockChecks) handles for an ability, as a tier. */
static int AbilityTier(u8 ability)
{
    switch (ability) {
    case KIRBY_ABILITY_MINI:
        return 0;
    case KIRBY_ABILITY_BURNING: case KIRBY_ABILITY_WHEEL: case KIRBY_ABILITY_STONE:
    case KIRBY_ABILITY_MISSILE:
        return 2;
    case KIRBY_ABILITY_HAMMER: case KIRBY_ABILITY_MASTER:
        return 3;
    default:
        return 1;
    }
}

/* A cell a floating Kirby can pass, breaking blocks up to `tier`.  A chain
 * block counts as open here; Flood() opens it only where a trigger the
 * Kirby can reach and break sets the chain off. */
static bool32 Passable(u32 a, int tier)
{
    if (a & 0x400)                      /* Mini-only gap */
        return FALSE;
    if (a & 0x1000)                     /* breakable, level 0..4 */
        return tier > 0 && (int)((a >> 20) & 0xF) <= tier;
    if ((a & 0x240) == 0x240)           /* solid until a chain explosion */
        return tier >= 1;
    return !(a & 0x200);
}

static const struct Unk_0888562C_3 *RoomRecords(u16 room)
{
    const struct SolidityMap *sm;

    if (room >= MAX_ROOMS || DoorList(room) == NULL)
        return NULL;
    sm = gSolidityMaps[gRoomProps[room].solidityMapIdx];
    return sm != NULL ? sm->unk4.pat3 : NULL;
}

static const struct ObjectTemplate *RoomObjects(u16 room)
{
    const struct LevelInfo_1E4 *ol;

    if (room >= MAX_ROOMS || DoorList(room) == NULL)
        return NULL;
    ol = gLevelObjLists[gRoomProps[room].objectListIdx].obj;
    return ol != NULL ? ol->unk0 : NULL;
}

#define NEXT_OBJ(o) ((const struct ObjectTemplate *)(const void *)((const u8 *)(o) + (o)->unk1))

/* Room `r`'s size in cells (its foreground map); FALSE when it has none. */
static bool32 RoomSize(u16 r, int *w, int *h)
{
    const struct RoomTiledBG *fg = gForegroundTilemaps[gRoomProps[r].mapDataIdx];

    if (fg == NULL)
        return FALSE;
    *w = fg->width / 2;
    *h = fg->height / 2;
    return *w > 0 && *h > 0;
}

static uintptr_t HintSig(void)
{
    uintptr_t sig = (uintptr_t)(const void *)gSolidityMaps;
    u16 r;

    for (r = 0; r < MAX_ROOMS; r++)
        if (DoorList(r) != NULL) {
            const struct SolidityMap *sm = gSolidityMaps[gRoomProps[r].solidityMapIdx];

            sig = sig * 31 + (uintptr_t)(const void *)sm + r;
            if (sm != NULL)
                sig ^= (uintptr_t)(const void *)sm->unk0;
            sig = sig * 31 + (uintptr_t)(const void *)RoomObjects(r);
        }
    return sig;
}

/* An exit object's arrival: its own when it lies inside the room it leads
 * to, else the room's default spawn point (the stock warp's,
 * sub_08002D40) -- warp stars that fly in from off the map. */
static void ExitArrival(struct HintDoor *e, s32 ax, s32 ay)
{
    int w, h;
    u8 a, b;

    if (RoomSize(e->dest, &w, &h) && ax >= 1 && ay >= 1 && ax <= w && ay <= h) {
        e->ax = (u8)ax;
        e->ay = (u8)ay;
        return;
    }
    sub_08002D40(e->dest, &a, &b);
    e->ax = a;
    e->ay = b;
}

static void HintIndex(void)
{
    u16 r;

    sHintTotal = 0;
    sGateTotal = 0;
    for (r = 0; r < MAX_ROOMS; r++) {
        const struct Unk_0888562C_3 *p = RoomRecords(r);
        const struct ObjectTemplate *o = RoomObjects(r);
        int n = 0, guard;

        sHintFirst[r] = 0;
        sHintCount[r] = 0;
        sGateFirst[r] = sGateTotal;
        sGateCount[r] = 0;
        if (p == NULL)
            continue;
        for (guard = 0; guard < 512 && p->unk00 != 0 && p->unk04 != 0; guard++) {
            if (p->unk00 == 3 && sHintTotal < HINT_RECS && n < 32) {
                struct HintDoor *e = &sHintRec[sHintTotal];

                if (!n)
                    sHintFirst[r] = sHintTotal + 1;
                e->dest = p->unk08;
                e->x = p->unk02;
                e->y = p->unk03;
                e->ax = p->unk0A;
                e->ay = p->unk0B;
                e->col = p->unk01;
                e->kind = EXIT_DOOR;
                sHintRoom[sHintTotal] = r;
                sHintTotal++;
                n++;
            }
            p = (const struct Unk_0888562C_3 *)(const void *)((const u8 *)p + p->unk04);
        }
        /* the room's objects: exits, and the gates */
        for (guard = 0; o != NULL && guard < 256 && o->unk1 != 0; guard++, o = NEXT_OBJ(o)) {
            int kind = o->type == OBJ_WARP_STAR ? EXIT_STAR : o->type == OBJ_CANNON ? EXIT_CANNON : -1;
            int gk = 0;

            if (kind >= 0 && sHintTotal < HINT_RECS && n < 32) {
                struct HintDoor *e = &sHintRec[sHintTotal];
                u16 d = kind == EXIT_STAR ? o->unk1E : o->unk12;

                if (d >= MAX_ROOMS || d == r || DoorList(d) == NULL || o->x < 0 || o->y < 16)
                    continue;
                if (!n)
                    sHintFirst[r] = sHintTotal + 1;
                e->dest = d;
                e->x = (u8)(o->x >> 4);
                e->y = (u8)(kind == EXIT_STAR ? o->y >> 4 : (o->y - 1) >> 4);
                e->col = 0;
                e->kind = (u8)kind;
                if (kind == EXIT_STAR)
                    ExitArrival(e, o->unk1A, o->unk1C);
                else
                    ExitArrival(e, o->unk18 >> 8, o->unk18 & 0xFF);
                sHintRoom[sHintTotal] = r;
                sHintTotal++;
                n++;
                continue;
            }
            switch (o->type) {
            case OBJ_VERTICAL_SLIDING_DOOR: gk = GATE_SLIDE; break;
            case OBJ_SMALL_BUTTON:          gk = (o->unk22 & 3) ? 0 : GATE_SWITCH; break;
            case OBJ_DESTROYABLE_ROCK_BLOCK: gk = GATE_CRUMBLE; break;
            case OBJ_TRIGGERABLE_WALL:      gk = o->subtype1 <= 1 ? GATE_WALL : 0; break;
            }
            if (gk && sGateTotal < HINT_GATES && sGateCount[r] < 32 && o->x >= 0 && o->y >= 16) {
                struct HintGate *g = &sGate[sGateTotal++];

                g->kind = (u8)gk;
                g->x = (u8)(o->x >> 4);
                g->y = (u8)((o->y >> 4) - (gk == GATE_SLIDE || gk == GATE_SWITCH));
                g->slot = (u8)(o->unk4 & 0xF);
                g->need = o->unkF;
                g->sub = gk == GATE_SWITCH ? (o->unk22 & 4) != 0 : o->subtype1;
                g->timer = o->unk18;
                g->sp2 = o->unk2;
                g->sp3 = o->unk3;
                sGateCount[r]++;
            }
        }
        sHintCount[r] = (u8)n;
    }
}

/* The chain cells (attribute 0x40) joined to cell `c`, `c` too, as open. */
static void BlastOpen(const u8 *cells, int w, int h, int c)
{
    int head = 0, tail = 0;

    sGateOv[c] = OV_OPEN;
    sHintQueue[tail++] = (u16)c;
    while (head < tail) {
        int i = sHintQueue[head++], x = i % w, y = i / w, k;

        for (k = 0; k < 4; k++) {
            int nx = x + (k == 0) - (k == 1), ny = y + (k == 2) - (k == 3), j;

            if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                continue;
            j = ny * w + nx;
            if (sGateOv[j] != OV_OPEN && (gCollisionAttributes[cells[j]] & 0x40) && tail < 2 * HINT_CELLS) {
                sGateOv[j] = OV_OPEN;
                sHintQueue[tail++] = (u16)j;
            }
        }
    }
}

/* sGateOv for room `r`'s map `cells`: the gates not in `open` shut, the
 * ones in it open (bit i: the room's gate i).  Objects spawn only in the
 * views of people (CreateLevelObjects), so in a room no person is in there
 * is no sliding door to shut anything: `live` says the room's sliding
 * doors are judged by their objects (LiveOpen), else they are open. */
static void GateOverlay(u16 r, const u8 *cells, int w, int h, u32 open, bool32 live)
{
    int i;

    memset(sGateOv, OV_NONE, (size_t)(w * h));
    if (r >= MAX_ROOMS)
        return;
    for (i = 0; i < sGateCount[r]; i++) {
        const struct HintGate *g = &sGate[sGateFirst[r] + i];
        int c = g->y * w + g->x;

        if (g->x >= w || g->y >= h)
            continue;
        switch (g->kind) {
        case GATE_SLIDE:
            if (live && !(open & (1u << i))) {
                sGateOv[c] = OV_SHUT;
                if (g->y > 0)
                    sGateOv[c - w] = OV_SHUT;
            }
            break;
        case GATE_CRUMBLE:
            if (gCollisionAttributes[cells[c]] & 0x200)
                sGateOv[c] = OV_DOWN;
            break;
        case GATE_WALL:
            if ((open & (1u << i)) && (gCollisionAttributes[cells[c]] & 0x200))
                BlastOpen(cells, w, h, c);
            break;
        }
    }
}

/* Flood `lab` (0xFF = not reached) with `mark` from cell `start` over the
 * map `cells` (w x h), passable at `tier`, with the gates of sGateOv.
 * Returns the cells reached. */
static int Flood(const u8 *cells, int w, int h, int start, u8 mark, int tier, u8 *lab)
{
    int head = 0, tail = 0;

    if (start < 0 || start >= w * h || lab[start] != 0xFF)
        return 0;
    memset(sHintIgn, 0, (size_t)(w * h));
    lab[start] = mark;
    sHintQueue[tail++] = (u16)start;
    while (head < tail) {
        int i = sHintQueue[head++], x = i % w, y = i / w, k;
        static const s8 dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };

        for (k = 0; k < 4; k++) {
            int nx = x + dx[k], ny = y + dy[k], j;
            u32 a;
            u8 ign;

            if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                continue;
            j = ny * w + nx;
            a = gCollisionAttributes[cells[j]];
            if (sGateOv[j] != OV_NONE) {
                if (sGateOv[j] == OV_SHUT || (sGateOv[j] == OV_DOWN && dy[k] != 1))
                    continue;
                ign = 0;                        /* a gate open, a crumble block from above */
            } else if ((a & 0x1240) == 0x240) { /* chain block: only once the blast gets here */
                if (tier < 1 || !sHintIgn[i])
                    continue;
                ign = 1;
            } else {
                if (!Passable(a, tier))
                    continue;
                ign = (a & 0x1020) == 0x1020    /* a trigger it breaks sets it off */
                   || (sHintIgn[i] && (a & 0x40));
            }
            if (lab[j] == 0xFF) {
                lab[j] = mark;
                sHintIgn[j] = ign;
                sHintQueue[tail++] = (u16)j;
            } else if (lab[j] == mark && ign && !sHintIgn[j] && tail < 2 * HINT_CELLS) {
                sHintIgn[j] = 1;                /* again, now with the blast */
                sHintQueue[tail++] = (u16)j;
            }
        }
    }
    return tail;
}

/* Room `r`'s ROM collision map into sHintCells; FALSE if it does not fit. */
static bool32 RomCells(u16 r, int *w, int *h)
{
    const struct SolidityMap *sm = gSolidityMaps[gRoomProps[r].solidityMapIdx];
    const u8 *src;
    u32 size;

    if (sm == NULL || sm->unk0 == NULL || !RoomSize(r, w, h))
        return FALSE;
    src = (const u8 *)(const void *)sm->unk0;
    size = (u32)src[1] | ((u32)src[2] << 8) | ((u32)src[3] << 16);
    if ((src[0] & 0xF0) != 0x30 || size > HINT_CELLS || (u32)(*w * *h) > size)
        return FALSE;
    RLUnCompWram(src, sHintCells);
    return TRUE;
}

/* sHintLab marks cell (x, y) or the one above or below it reached (0):
 * where an object standing on the floor or floating can be touched. */
static bool32 NearReached(int w, int h, int x, int y)
{
    int c = y * w + x;

    if (x < 0 || y < 0 || x >= w || y >= h)
        return FALSE;
    return sHintLab[c] == 0 || (y > 0 && sHintLab[c - w] == 0) || (y + 1 < h && sHintLab[c + w] == 0);
}

/* A chest of room `r` (its ROM object list) in a cell sHintLab marks 0. */
static int ChestsReached(u16 r, int w, int h)
{
    const struct ObjectTemplate *o = RoomObjects(r);
    int guard, n = 0;

    for (guard = 0; o != NULL && guard < 256 && o->unk1 != 0; guard++, o = NEXT_OBJ(o))
        if ((o->type == OBJ_SMALL_CHEST || o->type == OBJ_BIG_CHEST)
            && o->x >= 0 && o->y >= 0 && NearReached(w, h, o->x >> 4, o->y >> 4))
            n++;
    return n;
}

static bool32 ChestReached(u16 r, int w, int h)
{
    return ChestsReached(r, w, h) > 0;
}

static bool32 EntryReachesChest(int t, u16 entry)
{
    return entry == NO_REC || entry >= sHintTotal || sHintChest[t][entry];
}

/* Marked enemies of room `r` (template unk22 bit 0: beating them counts,
 * object.c) in cells sHintLab marks 0. */
static int MarkedReached(u16 r, int w, int h)
{
    const struct ObjectTemplate *o = RoomObjects(r);
    int guard, n = 0;

    for (guard = 0; o != NULL && guard < 256 && o->unk1 != 0; guard++, o = NEXT_OBJ(o))
        if (o->type < OBJ_SMALL_FOOD && (o->unk22 & 1) && o->x >= 0 && o->y >= 0
            && NearReached(w, h, o->x >> 4, o->y >> 4))
            n++;
    return n;
}

/* The gates of room `r` not in `open` whose opener the flood in sHintLab
 * reaches: a switch of a sliding door's slot; for a wall, enough chests or
 * marked enemies (`have`: what the room has collected / beaten already). */
static u32 GateOpeners(u16 r, int w, int h, u32 open, bool32 walls, int haveItems, int haveKills)
{
    u32 add = 0;
    int i, j, items = -1, kills = -1;

    for (i = 0; i < sGateCount[r]; i++) {
        const struct HintGate *g = &sGate[sGateFirst[r] + i];

        if (open & (1u << i))
            continue;
        if (g->kind == GATE_SLIDE) {
            for (j = 0; j < sGateCount[r]; j++) {
                const struct HintGate *s = &sGate[sGateFirst[r] + j];

                if (s->kind == GATE_SWITCH && s->slot == g->slot && s->x < w && s->y < h
                    && sHintLab[s->y * w + s->x] == 0) {
                    add |= 1u << i;
                    break;
                }
            }
        } else if (g->kind == GATE_WALL && walls) {
            if (g->sub == 1) {
                if (items < 0)
                    items = ChestsReached(r, w, h);
                if (haveItems + items >= g->need)
                    add |= 1u << i;
            } else {
                if (kills < 0)
                    kills = MarkedReached(r, w, h);
                if (haveKills + kills >= g->need)
                    add |= 1u << i;
            }
        }
    }
    return add;
}

/* Flood room `r`'s map from `start` with its gates: those in `open` open,
 * and (`openers`) each further one whose opener the flood reaches, until
 * nothing more opens.  Returns the gates it took as open. */
static u32 FloodGated(u16 r, const u8 *cells, int w, int h, int start, int tier, u32 open,
                      bool32 live, bool32 openers, int haveItems, int haveKills)
{
    int iter;

    for (iter = 0; iter < 8; iter++) {
        u32 add;

        GateOverlay(r, cells, w, h, open, live);
        memset(sHintLab, 0xFF, sizeof(sHintLab));
        Flood(cells, w, h, start, 0, tier, sHintLab);
        if (!openers || r >= MAX_ROOMS || !sGateCount[r])
            break;
        add = GateOpeners(r, w, h, open, live, haveItems, haveKills);
        if (!add)
            break;
        open |= add;
    }
    return open;
}

static void HintTier(int t)
{
    u16 r, s;

    for (s = 0; s < sHintTotal; s++) {
        sHintReach[t][s] = 0;
        sHintChest[t][s] = 0;
    }
    for (r = 0; r < MAX_ROOMS; r++) {
        int w, h, i;
        u16 f = sHintFirst[r];

        if (!f || !RomCells(r, &w, &h))
            continue;
        f--;
        /* every door into this room: the doors its arrival cell reaches,
         * the gates it can open on the way open */
        for (s = 0; s < sHintTotal; s++) {
            const struct HintDoor *p = &sHintRec[s];
            int x = p->ax - 1, y = p->ay - 1;
            u32 m = 0;

            if (p->dest != r || x < 0 || y < 0 || x >= w || y >= h)
                continue;
            /* as a room no person is in (the likely case away from them):
             * sliding doors open, crumble blocks passable from above,
             * walls shut */
            FloodGated(r, sHintCells, w, h, y * w + x, t, 0, FALSE, FALSE, 0, 0);
            for (i = 0; i < sHintCount[r] && i < 32; i++) {
                const struct HintDoor *q = &sHintRec[f + i];

                if (q->x < w && q->y < h && NearReached(w, h, q->x, q->y)
                    && (q->kind != EXIT_DOOR || sHintLab[q->y * w + q->x] == 0))
                    m |= 1u << i;
            }
            sHintReach[t][s] = m;
            sHintChest[t][s] = (u8)ChestReached(r, w, h);
        }
    }
}

static void HintEnsure(int t)
{
    uintptr_t sig = HintSig();

    if (!(sHintBuilt & 1) || sig != sHintSig) {
        sHintBuilt = 0;
        sHintSig = sig;
        HintIndex();
        sHintBuilt = 1;
    }
    if (!(sHintBuilt & (2 << t))) {
        HintTier(t);
        sHintBuilt |= (u8)(2 << t);
    }
}

/* The live objects of the Kirby's room: every object spawned in it is on
 * the object list (gCurLevelInfo[p].unk1F0, CreateObject) of one of the
 * players in the room, as a node pointing at its base.unk56.  (The
 * collision lists, GetRoomObjectCollisionList, hold only some of them when
 * the brains run.)  Objects spawn only in people's views
 * (CreateLevelObjects), so a room no person is in has none.  Into `out`
 * (at most `max`); returns how many. */
static int RoomObjectList(struct CpuBrain *brain, struct Object **out, int max)
{
    struct LevelInfo *li = brain->levelInfo;
    u32 p;
    int n = 0;

    for (p = 0; p < gNumKirbys; p++) {
        const struct Unk_02023720 *e;
        int guard = 0;

        if (gCurLevelInfo[p].currentRoom != li->currentRoom || gCurLevelInfo[p].unk65E != li->unk65E)
            continue;
        for (e = gCurLevelInfo[p].unk1F0.first; e != NULL && guard < 0x40 && n < max; e = e->next, guard++) {
            struct Object *ob;

            if (e->unk08 == NULL || !e->unk0C)
                continue;
            ob = (struct Object *)(void *)((u8 *)e->unk08 - offsetof(struct ObjectBase, unk56));
            if (ob->base.header.kind == 1 && !(ob->base.flags & 0x1000))
                out[n++] = ob;
        }
    }
    return n;
}

#define ROOM_OBJS 128

/* The live object of `type` the ROM template at cell (tx, ty) spawned. */
static struct Object *RoomObjectAt(struct CpuBrain *brain, u8 type, int tx, int ty)
{
    struct Object *o[ROOM_OBJS];
    int n = RoomObjectList(brain, o, ROOM_OBJS), i;

    for (i = 0; i < n; i++)
        if (o[i]->type == type && o[i]->objTemplate != NULL
            && (o[i]->objTemplate->x >> 4) == tx && (o[i]->objTemplate->y >> 4) == ty)
            return o[i];
    return NULL;
}

/* The live sliding door of gate `g` of the Kirby's room: open enough to
 * pass now (open and still, or closing but not yet below 12 px).  Not
 * spawned: by its slot. */
static bool32 SlideOpenNow(struct CpuBrain *brain, const struct HintGate *g)
{
    struct Object *ob = RoomObjectAt(brain, OBJ_VERTICAL_SLIDING_DOOR, g->x, g->y + 1);

    if (ob != NULL) {
        const struct VertSlidingDoor *d = (const struct VertSlidingDoor *)(const void *)ob;
        u16 r = brain->levelInfo->currentRoom;
        int i;

        if ((d->unkB4 & 3) == 2)                /* open and still */
            return TRUE;
        if ((d->unkB4 & 3) == 3)                /* closing: while still high enough */
            return d->unkB7 >= 3;
        if (!(d->unkB4 & 1))                    /* shut */
            return FALSE;
        /* opening: go now if a switch of its keeps it open a while after
         * one leaves (timed or staying down); a switch held only while
         * stood on, not before it is fully open */
        for (i = 0; i < sGateCount[r]; i++) {
            const struct HintGate *s = &sGate[sGateFirst[r] + i];

            if (s->kind == GATE_SWITCH && s->slot == g->slot && (s->timer > 0 || s->sub))
                return TRUE;
        }
        return FALSE;
    }
    return TRUE;
}

/* The gates of the Kirby's room open now (bit i: gate i): a sliding door
 * whose object is not spawned is no obstacle. */
static u32 LiveOpen(struct CpuBrain *brain)
{
    u16 r = brain->levelInfo->currentRoom;
    u32 open = 0;
    int i;

    if (r >= MAX_ROOMS)
        return 0;
    for (i = 0; i < sGateCount[r]; i++) {
        const struct HintGate *g = &sGate[sGateFirst[r] + i];

        if (g->kind == GATE_SLIDE && SlideOpenNow(brain, g))
            open |= 1u << i;
    }
    return open;
}

/* The Kirby's cell in its live map (its own, else one beside it it can be
 * in), -1 nowhere sane. */
static int KirbyCell(struct CpuBrain *brain, int t)
{
    struct LevelInfo *li = brain->levelInfo;
    const u8 *cells = gUnk_02024ED0[li->unk65E];
    int w = li->roomWidth >> 4, h = li->roomHeight >> 4;
    int kx = brain->kirby->base.x >> 12, ky = brain->kirby->base.y >> 12, k;
    static const s8 ox[5] = { 0, 0, 0, -1, 1 }, oy[5] = { 0, -1, 1, 0, 0 };

    if (w <= 0 || h <= 0 || w * h > HINT_CELLS)
        return -1;
    for (k = 0; k < 5; k++) {
        int x = kx + ox[k], y = ky + oy[k];

        if (x >= 0 && y >= 0 && x < w && y < h && Passable(gCollisionAttributes[cells[y * w + x]], t))
            return y * w + x;
    }
    return -1;
}

/* The Kirby's room flooded from where it is (its live map: blocks already
 * broken are open), into sHintLab (0 at reached cells), its gates as they
 * are now -- and, with `openers`, also the ones whose opener it reaches.
 * FALSE if it is nowhere sane.  sGateOv is left as the last flood used. */
static bool32 LiveReachEx(struct CpuBrain *brain, int t, bool32 openers)
{
    struct LevelInfo *li = brain->levelInfo;
    u16 r = li->currentRoom;
    int c = KirbyCell(brain, t);

    if (c < 0)
        return FALSE;
    HintEnsure(t);
    FloodGated(r, gUnk_02024ED0[li->unk65E], li->roomWidth >> 4, li->roomHeight >> 4, c, t,
               LiveOpen(brain), HumanHere(r), openers, GetRoomItemCount(li->unk65E), GetRoomKillCount(li->unk65E));
    return TRUE;
}

/* As things are now (the gates as they stand). */
static bool32 LiveReach(struct CpuBrain *brain, int t)
{
    return LiveReachEx(brain, t, FALSE);
}

/* A door record of the Kirby's room it can use now: the door tile is in the
 * live map (not behind a switch, a wall, an object) and it can get there;
 * an exit object: it can get there. */
static bool32 DoorOpenHere(struct CpuBrain *brain, const struct HintDoor *p)
{
    struct LevelInfo *li = brain->levelInfo;
    int w = li->roomWidth >> 4, h = li->roomHeight >> 4, c = p->y * w + p->x, k;
    u8 v;

    if (p->x >= w || p->y >= h)
        return FALSE;
    if (p->kind != EXIT_DOOR)
        return NearReached(w, h, p->x, p->y);
    v = gUnk_02024ED0[li->unk65E][c];
    if (v != p->col || !(gCollisionAttributes[v] & 0x4000))
        return FALSE;
    if (sHintLab[c] == 0)
        return TRUE;
    for (k = 0; k < 4; k++) {          /* reached beside it */
        int x = p->x + (k == 0) - (k == 1), y = p->y + (k == 2) - (k == 3);

        if (x >= 0 && y >= 0 && x < w && y < h && sHintLab[y * w + x] == 0)
            return TRUE;
    }
    return FALSE;
}

/* Breadth first over (room, door group) from the Kirby's place: the door
 * record (sHintRec index) of the first door of the shortest route to a room
 * that is wanted (not `from` itself).  NO_REC when none is reachable.  Doors
 * this brain gave up on are left out, and the first step not through a door
 * back to `avoid`. */
static u16 RouteEx(struct CpuBrain *brain, u16 from, int want, u16 target, u16 avoid)
{
    u16 first[HINT_RECS];               /* per state (the door record taken): the first door */
    u16 queue[HINT_RECS];
    u16 head = 0, tail = 0, f, i, s;
    int t = AbilityTier(brain->kirby->ability);
    s32 kx = brain->kirby->base.x >> 12, ky = brain->kirby->base.y >> 12;
    u8 used[256];
    int n;

    if (DoorList(from) == NULL)
        return NO_REC;
    HintEnsure(t);
    if (!sHintFirst[from] || !LiveReachEx(brain, t, TRUE))   /* gates it can open count open */
        return NO_REC;
    for (i = 0; i < sHintTotal; i++)
        first[i] = NO_REC;
    f = sHintFirst[from] - 1;
    n = sHintCount[from];
    memset(used, 0, sizeof(used));
    /* the first step: the doors it can reach now, nearest first */
    for (;;) {
        u32 best = 0xFFFFFFFF;
        int bi = -1;

        for (i = 0; i < n; i++) {
            const struct HintDoor *p = &sHintRec[f + i];
            s32 dx = p->x - kx, dy = p->y - ky;
            u32 d = (u32)(dx * dx + dy * dy);

            if (!used[i] && d < best) {
                best = d;
                bi = i;
            }
        }
        if (bi < 0)
            break;
        used[bi] = 1;
        {
            const struct HintDoor *p = &sHintRec[f + bi];
            u16 d = p->dest;

            if (d >= MAX_ROOMS || DoorList(d) == NULL || d == avoid || Blocked(brain, from, d)
                || !DoorOpenHere(brain, p))
                continue;
            if (d != from && Wanted(want, d, target, t, f + bi))
                return f + bi;
            s = f + bi;
            if (sHintReach[t][s] && first[s] == NO_REC) {
                first[s] = s;
                queue[tail++] = s;
            }
        }
    }
    while (head < tail) {
        u16 g = queue[head++], r = sHintRec[g].dest, rf;
        u32 m = sHintReach[t][g];

        if (r >= MAX_ROOMS || !sHintFirst[r])
            continue;
        rf = sHintFirst[r] - 1;
        for (i = 0; i < sHintCount[r] && i < 32; i++) {
            const struct HintDoor *p = &sHintRec[rf + i];
            u16 d = p->dest;

            if (!(m & (1u << i)) || d >= MAX_ROOMS || DoorList(d) == NULL || Blocked(brain, r, d)
                || d == from)           /* not back through the room it starts in */
                continue;
            if (Wanted(want, d, target, t, rf + i))
                return first[g];
            s = rf + i;
            if (first[s] == NO_REC && tail < HINT_RECS) {
                first[s] = first[g];
                queue[tail++] = s;
            }
        }
    }
    return NO_REC;
}

/* Not straight back where it came from unless nothing else leads there
 * (two rooms each other's way to a goal ping-pong otherwise). */
static u16 Route(struct CpuBrain *brain, u16 from, int want, u16 target)
{
    u16 j = PREV(brain) ? RouteEx(brain, from, want, target, (u16)(PREV(brain) - 1)) : NO_REC;

    if (j != NO_REC || !PREV(brain))
        return j != NO_REC ? j : RouteEx(brain, from, want, target, NO_ROOM);
    j = RouteEx(brain, from, want, target, NO_ROOM);
    /* only straight back: the door that brought it here was a wrong turn
     * (the two rooms would send it to and fro), so not that way again */
    if (j != NO_REC && ST(brain)->roomFrames < 120 && sHintRec[j].dest == PREV(brain) - 1)
        BlockWhy(brain, (u16)(PREV(brain) - 1), from, "no-reenter");
    return j;
}

/* --- steering ------------------------------------------------------------- */

static void GoToEx(struct CpuBrain *brain, s32 x, s32 y, bool32 direct)
{
    const struct LevelInfo_1E8 *g = brain->levelInfo->unk1E8;
    struct LevelInfo *li = brain->levelInfo;
    s32 tx = x >> 12, ty = y >> 12;

    if (brain->curNode == 0xFF)
        brain->curNode = CpuNavNodeAt(&brain->kirby->base);
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;
    if (tx >= li->roomWidth >> 4) tx = (li->roomWidth >> 4) - 1;
    if (ty >= li->roomHeight >> 4) ty = (li->roomHeight >> 4) - 1;
    brain->goalNode = sub_080026A8(brain->kirby->base.unk56, (u16)tx, (u16)ty);
    if (direct && brain->curNode != 0xFF)
        brain->goalNode = brain->curNode;   /* steer straight at the point */
    brain->goalX = x;
    brain->goalY = y;
    if (brain->curNode != 0xFF && brain->goalNode != 0xFF)
        brain->nextNode = g->unk14[brain->curNode].unk0[brain->goalNode];
    else
        brain->nextNode = 0xFF;
    CpuUpdateGoalVectors(brain);
    brain->flags &= ~CPU_FLAG_GOAL_REACHED;
    brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseMoving);
    /* the last stretch, inside the goal's own node: the stock ACTION layer
     * stands still within a node's radius, so nudge it along the ground */
    if (brain->curNode == brain->goalNode && brain->goalDx > 6) {
        brain->keys |= x > brain->kirby->base.x ? 0x10 : 0x20;
        if (brain->goalDy > 0x18 && y < brain->kirby->base.y && !(brain->kirby->base.flags & 0x20) && (gPortSim->frame.frameCount & 31) == 0)
            brain->keys |= 1;           /* a hop now and then if it is above */
    }
}

static void PathTo(struct CpuBrain *brain, int gx, int gy);

static void GoTo(struct CpuBrain *brain, s32 x, s32 y)
{
    GoToEx(brain, x, y, FALSE);
}

static struct Object *NearestObject(struct CpuBrain *brain, int kind)
{
    PTR32(struct ObjectBase) *o = GetRoomObjectCollisionList(&brain->kirby->base);
    struct Object *best = NULL;
    s32 bd = 0x7FFFFFFF;
    struct LevelInfo *li = brain->levelInfo;
    int w = li->roomWidth >> 4, h = li->roomHeight >> 4;
    /* only what it can get to: its live map flooded from where it is,
     * with the blocks its ability breaks (a sealed chamber's chest, a
     * shard behind a wall are not targets) */
    bool32 reach = LiveReach(brain, AbilityTier(brain->kirby->ability));

    for (; o != NULL && *o; ++o) {
        struct Object *ob = (struct Object *)*o;
        bool32 ok;
        s32 dx, dy, d;

        if (ob->base.flags & 0x1000)
            continue;
        switch (kind) {
        case 0:     /* food */
            ok = ob->type == OBJ_SMALL_FOOD || ob->type == OBJ_POP_SODA || ob->type == OBJ_MEAT
              || ob->type == OBJ_TOMATO;
            break;
        case 1:     /* a mirror shard */
            ok = ob->type == OBJ_MIRROR_SHARD;
            break;
        case 2:     /* a boss or mid-boss, alive */
            ok = ObjType38To52(ob) && ob->unk80 > 0;
            break;
        default:    /* a chest */
            ok = ob->type == OBJ_SMALL_CHEST || ob->type == OBJ_BIG_CHEST;
            break;
        }
        if (!ok)
            continue;
        if (reach) {
            int x = ob->base.x >> 12, y = ob->base.y >> 12, c = y * w + x;

            if (x < 0 || y < 0 || x >= w || y >= h
                || (sHintLab[c] != 0 && (y == 0 || sHintLab[c - w] != 0)
                    && (y + 1 >= h || sHintLab[c + w] != 0)))
                continue;
        }
        dx = (ob->base.x - brain->kirby->base.x) >> 8;
        dy = (ob->base.y - brain->kirby->base.y) >> 8;
        d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (d < bd) {
            bd = d;
            best = ob;
        }
    }
    return best;
}

static bool32 HumanHere(u16 room)
{
    u32 i;

    for (i = 0; i < PortHumanEnd(); i = PortHumanNext(i))
        if (gCurLevelInfo[i].currentRoom == room)
            return TRUE;
    return FALSE;
}

/* The person a BODYGUARD guards: the nearest in its room, else the first
 * person who is in a room. */
static struct Kirby *Ward(struct CpuBrain *brain)
{
    u16 room = brain->levelInfo->currentRoom;
    struct Kirby *best = NULL, *any = NULL;
    s32 bd = 0x7FFFFFFF;
    u32 i;

    for (i = 0; i < PortHumanEnd(); i = PortHumanNext(i)) {
        struct Kirby *k = &gKirbys[i];
        s32 dx, dy, d;

        if (gCurLevelInfo[i].currentRoom == 0xFFFF || i == brain->kirby->base.unk56)
            continue;
        if (any == NULL)
            any = k;
        if (gCurLevelInfo[i].currentRoom != room)
            continue;
        dx = (k->base.x - brain->kirby->base.x) >> 8;
        dy = (k->base.y - brain->kirby->base.y) >> 8;
        d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    return best != NULL ? best : any;
}

/* --- breakable blocks in the way ------------------------------------------
 *
 * The route to a door may cross blocks the Kirby's ability breaks (the
 * tier the door groups were computed with).  The stock ACTION layer breaks
 * a block only when it is thinking on the ground with the block right in
 * front of, above-in-front or below-in-front of it (gCpuBreakBlockChecks);
 * a flying Kirby pushes into it forever.  So: walk the cell path from the
 * Kirby toward the door (a breadth-first distance field from the door over
 * the live map at the Kirby's tier); when a block that is not open at tier 0
 * is within three steps, go and stand beside it (same row, or on top of it),
 * stop flying, face it and run the stock break check.  A chain block is
 * opened through its trigger (attribute 0x20): the trigger of the same
 * chain the Kirby can reach. */

static u16 sHintDist[HINT_CELLS];   /* scratch, written before every read */

/* `pits`: leave out the cells open all the way down to the room's bottom
 * edge (falling there is a death), marked in sHintIgn. */
static void DistFieldEx(const u8 *cells, int w, int h, int goal, int tier, bool32 pits)
{
    int head = 0, tail = 0, i;

    if (pits) {
        int x, y;

        for (x = 0; x < w; x++) {
            u8 open = 1;

            for (y = h - 1; y >= 0; y--) {
                if (gCollisionAttributes[cells[y * w + x]] & 0x206)   /* solid, top-solid, water */
                    open = 0;
                sHintIgn[y * w + x] = open;
            }
        }
    }
    for (i = 0; i < w * h; i++)
        sHintDist[i] = 0xFFFF;
    sHintDist[goal] = 0;
    sHintQueue[tail++] = (u16)goal;
    while (head < tail) {
        int c = sHintQueue[head++], x = c % w, y = c / w, k;
        static const s8 dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };

        for (k = 0; k < 4; k++) {
            int nx = x + dx[k], ny = y + dy[k], j;

            if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                continue;
            j = ny * w + nx;
            if (sHintDist[j] == 0xFFFF && sGateOv[j] != OV_SHUT && !(pits && sHintIgn[j])
                && (sGateOv[j] != OV_NONE || Passable(gCollisionAttributes[cells[j]], tier))) {
                sHintDist[j] = sHintDist[c] + 1;
                sHintQueue[tail++] = (u16)j;
            }
        }
    }
}

static void DistField(const u8 *cells, int w, int h, int goal, int tier)
{
    DistFieldEx(cells, w, h, goal, tier, FALSE);
}

/* The chain trigger of the chain cell `c` belongs to, nearest (x0, y0),
 * that has a cell beside it the Kirby reaches at tier 0 (sHintLab == 0).
 * -1 none.  Uses sHintDist's spare high values as a visited mark: call
 * after the path walk is done with it. */
static int ChainTrigger(const u8 *cells, int w, int h, int c, int x0, int y0)
{
    int head = 0, tail = 0, best = -1;
    u32 bd = 0xFFFFFFFF;

    sHintQueue[tail++] = (u16)c;
    sHintDist[c] = 0xFFFE;
    while (head < tail) {
        int i = sHintQueue[head++], x = i % w, y = i / w, k;
        u32 a = gCollisionAttributes[cells[i]];

        if (a & 0x20) {
            for (k = 0; k < 4; k++) {
                int nx = x + (k == 0) - (k == 1), ny = y + (k == 2) - (k == 3);

                if (nx >= 0 && ny >= 0 && nx < w && ny < h && sHintLab[ny * w + nx] == 0) {
                    u32 d = (u32)((x - x0) * (x - x0) + (y - y0) * (y - y0));

                    if (d < bd) {
                        bd = d;
                        best = i;
                    }
                }
            }
        }
        for (k = 0; k < 4; k++) {
            int nx = x + (k == 0) - (k == 1), ny = y + (k == 2) - (k == 3), j;

            if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                continue;
            j = ny * w + nx;
            if (sHintDist[j] != 0xFFFE && (gCollisionAttributes[cells[j]] & 0x40) && tail < HINT_CELLS) {
                sHintDist[j] = 0xFFFE;
                sHintQueue[tail++] = (u16)j;
            }
        }
    }
    return best;
}

/* A crumble block at (bx, by) the Kirby stands on whose object is not
 * spawned (no person sees it, so it cannot crumble): do what the object
 * would -- its cell's tile record (sub_08001408, as sub_0811C07C does)
 * and its destroyed flag, so that it does not spawn later
 * (sub_08001678, as DestroyDestroyableRockBlock does).  FALSE: not that
 * case (the object is there and does it itself, or there is no record). */
static bool32 CrumbleUnseen(struct CpuBrain *brain, int bx, int by)
{
    struct LevelInfo *li = brain->levelInfo;
    u16 r = li->currentRoom;
    const struct HintGate *g = NULL;
    const struct Unk_0888562C *rec;
    u8 v;
    int i, guard;

    if (r >= MAX_ROOMS)
        return FALSE;
    for (i = 0; i < sGateCount[r]; i++)
        if (sGate[sGateFirst[r] + i].kind == GATE_CRUMBLE && sGate[sGateFirst[r] + i].x == bx
            && sGate[sGateFirst[r] + i].y == by)
            g = &sGate[sGateFirst[r] + i];
    if (g == NULL || RoomObjectAt(brain, OBJ_DESTROYABLE_ROCK_BLOCK, g->x, g->y) != NULL || li->unk1E0.pat1 == NULL)
        return FALSE;
    v = gUnk_02024ED0[li->unk65E][by * (li->roomWidth >> 4) + bx];
    rec = li->unk1E0.pat1;
    for (guard = 0; guard < 1024 && rec->unk00 != 0 && rec->unk04 != 0; guard++) {
        if (rec->unk00 == 1 && rec->unk01 == v && rec->unk02 == bx && rec->unk03 == by) {
            union LevelInfo_1E0 u;

            u.pat1 = (struct Unk_0888562C *)(uintptr_t)rec;
            sub_08001408(brain->kirby->base.unk56, u, NULL, NULL);
            sub_08001678(g->sp2, g->sp3, li->unk65E, 1);
            PortCpuAiNote(brain->kirby->base.unk56, "crumble-unseen", r, bx | (by << 8), v);
            return TRUE;
        }
        rec = (const struct Unk_0888562C *)(const void *)((const u8 *)rec + rec->unk04);
    }
    return FALSE;
}

/* TRUE when it is dealing with a block on the way to the door (st->doorX/Y). */
static bool32 BreakWay(struct CpuBrain *brain)
{
    struct LevelInfo *li = brain->levelInfo;
    struct Kirby *k = brain->kirby;
    struct CpuAiState *st = ST(brain);
    const u8 *cells = gUnk_02024ED0[li->unk65E];
    int w = li->roomWidth >> 4, h = li->roomHeight >> 4;
    int t = AbilityTier(k->ability), kx = k->base.x >> 12, ky = k->base.y >> 12;
    int cur, step, block = -1, bx, by, sx, sy, s, i, crumble = 0;
    static const s8 ox[5] = { 0, 0, 0, -1, 1 }, oy[5] = { 0, -1, 1, 0, 0 };

    if (w <= 0 || h <= 0 || w * h > HINT_CELLS || st->doorX >= w || st->doorY >= h)
        return FALSE;
    GateOverlay(li->currentRoom, cells, w, h, LiveOpen(brain), HumanHere(li->currentRoom));
    DistField(cells, w, h, st->doorY * w + st->doorX, t);
    cur = -1;
    for (i = 0; i < 5; i++) {           /* its cell, else one beside it on the path */
        int x = kx + ox[i], y = ky + oy[i];

        if (x >= 0 && y >= 0 && x < w && y < h && sHintDist[y * w + x] != 0xFFFF
            && Passable(gCollisionAttributes[cells[y * w + x]], 0)) {
            cur = y * w + x;
            break;
        }
    }
    if (cur < 0)
        return FALSE;
    /* down the distance field, up to three steps: the first cell not open now */
    for (step = 0; step < 3 && sHintDist[cur] > 0; step++) {
        int x = cur % w, y = cur / w, next = -1;
        u16 nd = sHintDist[cur];

        for (i = 1; i < 5; i++) {
            int nx = x + ox[i], ny = y + oy[i];

            if (nx >= 0 && ny >= 0 && nx < w && ny < h && sHintDist[ny * w + nx] < nd) {
                nd = sHintDist[ny * w + nx];
                next = ny * w + nx;
            }
        }
        if (next < 0)
            break;
        if (sGateOv[next] == OV_DOWN) {         /* a crumble block: stand on it */
            block = next;
            crumble = 1;
            break;
        }
        if (sGateOv[next] == OV_NONE && !Passable(gCollisionAttributes[cells[next]], 0)) {
            block = next;
            break;
        }
        cur = next;
    }
    if (block < 0)
        return FALSE;
    LiveReach(brain, 0);                /* sHintLab: cells it reaches without breaking */
    if (crumble) {
        bx = block % w;
        by = block / w;
        if (by == 0 || sHintLab[block - w] != 0)
            return FALSE;
        if ((st->destFrames % 120) == 0)
            PortCpuAiNote(k->base.unk56, "crumble", bx | (by << 8), kx | (ky << 8), !!(k->base.flags & 0x40));
        if (kx == bx && ky == by - 1) {         /* on it: land and wait */
            if ((k->base.flags & 0x40) && brain->action != CPU_ACT_THINK)
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActLeaveFlight);
            else if (k->base.flags & 0x40)
                brain->keys |= 2;
            else if (!(k->base.flags & 0x20))
                CrumbleUnseen(brain, bx, by);
            GoToEx(brain, ((s32)bx << 12) + 0x800, k->base.y, TRUE);
            return TRUE;
        }
        GoToEx(brain, ((s32)bx << 12) + 0x800, ((s32)(by - 1) << 12) + 0x800, TRUE);
        if ((kx == bx || kx - bx == 1 || bx - kx == 1) && by - 1 > ky && (k->base.flags & 0x40))
            brain->keys |= 2;
        return TRUE;
    }
    if (t == 0)
        return FALSE;
    if (!(gCollisionAttributes[cells[block]] & 0x1000)) {
        block = ChainTrigger(cells, w, h, block, kx, ky);
        if (block < 0)
            return FALSE;
    }
    bx = block % w;
    by = block / w;
    /* where to stand: beside it on its row (its side first), else on top */
    s = kx <= bx ? -1 : 1;
    {
        static const s8 cx[4] = { 1, -1, 0, 0 }, cy[4] = { 0, 0, -1, 1 };
        int found = 0;

        for (i = 0; i < 4 && !found; i++) {
            sx = bx + (i < 2 ? s * cx[i] : 0);
            sy = by + cy[i];
            if (sx >= 0 && sy >= 0 && sx < w && sy < h && sHintLab[sy * w + sx] == 0)
                found = 1;
        }
        if (!found)
            return FALSE;
    }
    if ((st->destFrames % 120) == 0)
        PortCpuAiNote(k->base.unk56, "block", bx | (by << 8), sx | (sy << 8),
                      kx | (ky << 8) | (int)(gCollisionAttributes[cells[block]] >> 20 & 0xF) << 16
                      | !!(k->base.flags & 0x40) << 20 | (k->base.flags & 1) << 21 | brain->action << 24);
    if (kx == sx && ky == sy) {
        bool32 left = bx < kx || (bx == kx && (k->base.flags & 1));

        if (k->base.flags & 0x40) {     /* flying: let the air out first */
            if (brain->action != CPU_ACT_THINK)
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActLeaveFlight);
            return TRUE;
        }
        if (!!(k->base.flags & 1) != left) {
            brain->keys |= left ? 0x20 : 0x10;
            return TRUE;
        }
        if (!(brain->flags & CPU_FLAG_ACTION_BUSY))
            CpuBreakBlockCheck(brain);
        GoToEx(brain, k->base.x, k->base.y, TRUE);
        return TRUE;
    }
    GoToEx(brain, ((s32)sx << 12) + 0x800, ((s32)sy << 12) + 0x800, TRUE);
    /* the stock ACTION layer stands still near a point straight above or
     * below it (its distance is |dx - dy|): flap up, or let the air out */
    if (kx == sx || kx - sx == 1 || sx - kx == 1) {
        if (sy < ky && (gPortSim->frame.frameCount & 3) == 0)
            brain->keys |= 1;
        else if (sy > ky && (k->base.flags & 0x40))
            brain->keys |= 2;
    }
    return TRUE;
}

/* --- gates in the way -------------------------------------------------------
 *
 * The chosen door may lie behind a gate the Kirby can open (the route
 * counted it open because its opener is reachable): a sliding door whose
 * switch it can stand on, a wall that waits for the room's chests (or
 * marked enemies).  Flooded as things are now, the door is not reached:
 * then find the gate whose opening reaches it, and go to its opener -- stand on the switch (not flying: a timed
 * switch is pressed by standing) until the door is fully open, or go to the
 * nearest chest or marked enemy the wall waits for (touching a chest opens
 * it; the enemy scan fights the enemy). */

/* The record of room `room` at (x, y) leading to `dest`; NO_REC none. */
static u16 FindRec(u16 room, u8 x, u8 y, u16 dest)
{
    u16 f, i;

    if (room >= MAX_ROOMS || !sHintFirst[room])
        return NO_REC;
    f = sHintFirst[room] - 1;
    for (i = 0; i < sHintCount[room]; i++)
        if (sHintRec[f + i].x == x && sHintRec[f + i].y == y && sHintRec[f + i].dest == dest)
            return f + i;
    return NO_REC;
}

/* sHintLab reaches cell (x, y), or one beside, above or below it. */
static bool32 CellNear(int w, int h, int x, int y)
{
    return NearReached(w, h, x, y) || NearReached(w, h, x - 1, y) || NearReached(w, h, x + 1, y);
}

static bool32 GateWay(struct CpuBrain *brain)
{
    struct LevelInfo *li = brain->levelInfo;
    struct CpuAiState *st = ST(brain);
    struct Kirby *k = brain->kirby;
    u16 r = li->currentRoom;
    const u8 *cells = gUnk_02024ED0[li->unk65E];
    int w = li->roomWidth >> 4, h = li->roomHeight >> 4;
    int t = AbilityTier(k->ability), c, i, pick = -1;
    int items = GetRoomItemCount(li->unk65E), kills = GetRoomKillCount(li->unk65E);
    s32 kx = k->base.x >> 12, ky = k->base.y >> 12;
    u32 open, cand;
    const struct HintGate *g;

    if (r >= MAX_ROOMS || !sGateCount[r])
        return FALSE;
    HintEnsure(t);
    c = KirbyCell(brain, t);
    if (c < 0)
        return FALSE;
    if (!HumanHere(r))
        return FALSE;                   /* no objects: nothing shuts, nothing opens */
    open = LiveOpen(brain);
    FloodGated(r, cells, w, h, c, t, open, TRUE, FALSE, 0, 0);
    if (CellNear(w, h, st->doorX, st->doorY)) {
        /* through a sliding door that is open now: hurry, along the cell
         * path (the switches are timed; no time for the nav graph's
         * detours) */
        u32 slides = 0;

        for (i = 0; i < sGateCount[r]; i++)
            if (sGate[sGateFirst[r] + i].kind == GATE_SLIDE)
                slides |= 1u << i;
        if (!(open & slides))
            return FALSE;
        FloodGated(r, cells, w, h, c, t, open & ~slides, TRUE, FALSE, 0, 0);
        if (CellNear(w, h, st->doorX, st->doorY))
            return FALSE;
        if ((st->destFrames % 120) == 0)
            PortCpuAiNote(k->base.unk56, "gate-through", r, st->doorX | (st->doorY << 8), (int)kx | (int)(ky << 8));
        PathTo(brain, st->doorX, st->doorY);
        return TRUE;
    }
    cand = GateOpeners(r, w, h, open, TRUE, items, kills);
    if (!cand)
        return FALSE;
    for (i = 0; i < sGateCount[r] && pick < 0; i++)
        if (cand & (1u << i)) {
            FloodGated(r, cells, w, h, c, t, open | (1u << i), TRUE, FALSE, 0, 0);
            if (CellNear(w, h, st->doorX, st->doorY))
                pick = i;
        }
    if (pick < 0)
        return FALSE;                   /* none of them is the way to it */
    FloodGated(r, cells, w, h, c, t, open, TRUE, FALSE, 0, 0);   /* where its opener is reached */
    g = &sGate[sGateFirst[r] + pick];
    if (g->kind == GATE_SLIDE) {
        int sx = -1, sy = -1;
        u32 bd = 0xFFFFFFFF;

        for (i = 0; i < sGateCount[r]; i++) {
            const struct HintGate *s = &sGate[sGateFirst[r] + i];
            u32 d = (u32)((s->x - kx) * (s->x - kx) + (s->y - ky) * (s->y - ky));

            if (s->kind == GATE_SWITCH && s->slot == g->slot && s->x < w && s->y < h
                && sHintLab[s->y * w + s->x] == 0 && d < bd) {
                bd = d;
                sx = s->x;
                sy = s->y;
            }
        }
        if (sx < 0)
            return FALSE;
        if ((st->destFrames % 120) == 0)
            PortCpuAiNote(k->base.unk56, "gate-switch", r, sx | (sy << 8),
                          g->x | (g->y << 8) | (int)kx << 16 | (int)(ky & 0x7F) << 24);
        {
            s32 px = ((s32)sx << 12) + 0x800, dx = k->base.x - px;

            if (ky == sy && dx < 0x600 && dx > -0x600) {
                /* on it: land, and stay until the door is fully open */
                if ((k->base.flags & 0x40) && brain->action != CPU_ACT_THINK)
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActLeaveFlight);
                else if (k->base.flags & 0x40)
                    brain->keys |= 2;
                brain->flags |= CPU_FLAG_ACTION_BUSY;   /* no wandering off to fight */
                GoToEx(brain, px, k->base.y, TRUE);
                return TRUE;
            }
            PathTo(brain, sx, sy);
        }
        return TRUE;
    }
    if (g->kind == GATE_WALL) {
        struct Object *o[ROOM_OBJS], *best = NULL;
        int n = RoomObjectList(brain, o, ROOM_OBJS);
        s32 bd = 0x7FFFFFFF;

        /* the nearest reached chest (sub 1) / live marked enemy (sub 0) */
        for (i = 0; i < n; i++) {
            struct Object *ob = o[i];
            s32 d, dx, dy;
            bool32 ok;

            if (g->sub == 1)
                ok = ob->type == OBJ_SMALL_CHEST || ob->type == OBJ_BIG_CHEST;
            else
                ok = ob->type < OBJ_SMALL_FOOD && ob->objTemplate != NULL && (ob->objTemplate->unk22 & 1)
                  && ob->unk80 > 0;
            if (!ok || !NearReached(w, h, ob->base.x >> 12, ob->base.y >> 12))
                continue;
            dx = (ob->base.x - k->base.x) >> 8;
            dy = (ob->base.y - k->base.y) >> 8;
            d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (d < bd) {
                bd = d;
                best = ob;
            }
        }
        if (best == NULL)
            return FALSE;
        if ((st->destFrames % 120) == 0)
            PortCpuAiNote(k->base.unk56, "gate-wall", r, best->type, g->x | (g->y << 8));
        GoTo(brain, best->base.x, best->base.y);
        return TRUE;
    }
    return FALSE;
}

/* A cell a few steps along the flood's cell path from the Kirby to the
 * chosen door (the live map at its tier, the gates as they are); FALSE
 * when it has none. */
static int sPathFirstDy;            /* PathWaypoint: the first step's dy (-1 up, 1 down) */

static bool32 PathWaypoint(struct CpuBrain *brain, int gx, int gy, int *wx, int *wy)
{
    struct LevelInfo *li = brain->levelInfo;
    const u8 *cells = gUnk_02024ED0[li->unk65E];
    int w = li->roomWidth >> 4, h = li->roomHeight >> 4;
    int t = AbilityTier(brain->kirby->ability), cur = KirbyCell(brain, t), step, i;

    if (cur < 0 || gx < 0 || gy < 0 || gx >= w || gy >= h)
        return FALSE;
    GateOverlay(li->currentRoom, cells, w, h, LiveOpen(brain), HumanHere(li->currentRoom));
    DistFieldEx(cells, w, h, gy * w + gx, t, TRUE);
    sPathFirstDy = 0;
    if (sHintDist[cur] == 0xFFFF)
        return FALSE;
    for (step = 0; step < 3 && sHintDist[cur] > 0; step++) {
        int x = cur % w, y = cur / w, next = -1;
        u16 nd = sHintDist[cur];

        for (i = 0; i < 4; i++) {
            int nx = x + (i == 0) - (i == 1), ny = y + (i == 2) - (i == 3);

            if (nx >= 0 && ny >= 0 && nx < w && ny < h && sHintDist[ny * w + nx] < nd) {
                nd = sHintDist[ny * w + nx];
                next = ny * w + nx;
            }
        }
        if (next < 0)
            break;
        if (step == 0)
            sPathFirstDy = next / w - y;
        cur = next;
    }
    *wx = cur % w;
    *wy = cur / w;
    return TRUE;
}

/* Steer along the flood's cell path to cell (gx, gy); straight at it when
 * there is none. */
static void PathTo(struct CpuBrain *brain, int gx, int gy)
{
    struct Kirby *k = brain->kirby;
    int wx, wy;
    s32 kx = k->base.x >> 12, ky = k->base.y >> 12;

    if (!PathWaypoint(brain, gx, gy, &wx, &wy)) {
        wx = gx;
        wy = gy;
        sPathFirstDy = wy < ky ? -1 : wy > ky;
    }
    GoToEx(brain, ((s32)wx << 12) + 0x800, ((s32)wy << 12) + 0x800, TRUE);
    /* the stock ACTION layer stands still near a point straight above or
     * below it, and walks into a wall it must go over: the path's next
     * step up, flap; down, let the air out / drop through */
    if (sPathFirstDy < 0 && (gPortSim->frame.frameCount & 3) == 0)
        brain->keys |= 1;
    else if (sPathFirstDy > 0 && (kx - wx <= 1 && wx - kx <= 1))
        brain->keys |= 2;
}

/* Choose door record `j` (sHintRec) of the Kirby's room. */
static void SetDoor(struct CpuBrain *brain, u16 j)
{
    struct CpuAiState *st = ST(brain);
    const struct HintDoor *p = &sHintRec[j];

    st->dest = p->dest;
    st->doorX = p->x;
    st->doorY = p->y;
    st->destFrames = 0;
    PR(brain)->best = 0;
    PR(brain)->stall = 0;
    PortCpuAiNote(brain->kirby->base.unk56, "door", brain->levelInfo->currentRoom, st->dest,
                  st->doorX | (st->doorY << 8) | (brain->kirby->base.x >> 12 & 0xFF) << 16
                  | (brain->kirby->base.y >> 12 & 0x7F) << 24);
}

/* Walk to the chosen door (st->dest, st->doorX/Y) and through it.  FALSE
 * when it has taken too long (the door is then given up). */
static bool32 HeadFor(struct CpuBrain *brain)
{
    struct CpuAiState *st = ST(brain);
    struct Kirby *k = brain->kirby;
    u16 room = brain->levelInfo->currentRoom, dest = st->dest;
    s32 kx, ky;

    if (++st->destFrames > DOOR_GIVEUP) {
        Block(brain, room, dest);
        st->dest = NO_ROOM;
        return FALSE;
    }
    kx = k->base.x >> 12;
    ky = k->base.y >> 12;
    if ((st->destFrames % 300) == 0)
        PortCpuAiNote(k->base.unk56, "pos", kx | (ky << 8), brain->curNode | (brain->goalNode << 8),
                      brain->nextNode | (brain->action << 8));
    if (kx - st->doorX <= 1 && st->doorX - kx <= 1 && ky - st->doorY <= 1 && st->doorY - ky <= 1) {
        u16 j = FindRec(room, st->doorX, st->doorY, dest);

        if (j != NO_REC && sHintRec[j].kind != EXIT_DOOR) {
            /* a warp star or a cannon: they take only people (PortIsHuman),
             * so it goes where the object would send it, as it takes doors */
            if (sub_080551FC(k, dest, sHintRec[j].ax, sHintRec[j].ay)) {
                st->doors++;
                PortCpuAiNote(k->base.unk56, "through", room, dest, k->hp);
                PortCpuAiNote(k->base.unk56, "exit-object", room, dest, sHintRec[j].kind);
                return TRUE;
            }
        } else {
            struct Unk_0888562C_3 *p = sub_080025AC(k->base.unk56, st->doorX, st->doorY).pat3;

            if (p != NULL && p->unk08 == dest && sub_080551FC(k, dest, p->unk0A, p->unk0B)) {
                st->doors++;
                PortCpuAiNote(k->base.unk56, "through", room, dest, k->hp);
                return TRUE;
            }
        }
    }
    if (GateWay(brain))                 /* a gate on the way: open it first */
        return TRUE;
    if (BreakWay(brain))                /* a block on the way: break it first */
        return TRUE;
    {
        /* by the nav graph; when that stops getting it nearer for 5 s,
         * straight at the door for 5 s, and so on in turn */
        struct CpuAiProgress *pr = PR(brain);
        u16 dist = (u16)((kx > st->doorX ? kx - st->doorX : st->doorX - kx)
                       + (ky > st->doorY ? ky - st->doorY : st->doorY - ky));

        if (!pr->best || dist + 1 < pr->best) {
            pr->best = dist + 1;
            pr->stall = 0;
        } else if (pr->stall < 0xFFFF) {
            pr->stall++;
        }
        if ((pr->stall / 300) & 1) {
            /* the graph is not getting it nearer: follow the cell path of
             * the flood (the gates as they are) a few cells ahead, else
             * straight at the door */
            PathTo(brain, st->doorX, st->doorY);
        } else {
            GoToEx(brain, ((s32)st->doorX << 12) + 0x800, ((s32)st->doorY << 12) + 0x800, FALSE);
        }
    }
    return TRUE;
}

/* Low on health: food in the room first, and no fighting on the way. */
static bool32 Mend(struct CpuBrain *brain)
{
    struct Kirby *k = brain->kirby;
    struct Object *food;

    if (k->maxHp <= 0 || k->hp * 3 > k->maxHp)
        return FALSE;
    food = NearestObject(brain, 0);
    if (food == NULL)
        return FALSE;
    brain->flags |= CPU_FLAG_ACTION_BUSY;   /* the SENSE layer's enemy scan waits */
    GoTo(brain, food->base.x, food->base.y);
    return TRUE;
}

/* Head for a room that is `want` (then `want2`, -1 none), door by door. */
static bool32 Travel(struct CpuBrain *brain, int want, int want2, u16 target)
{
    struct CpuAiState *st = ST(brain);
    u16 room = brain->levelInfo->currentRoom;
    int tries;

    if (st->dest == NO_ROUTE) {         /* nothing last time: ask again now and then */
        /* a dead end for the door tiles (one-way rooms, exits that are
         * objects -- cannons, warp stars -- or room edges, a pocket):
         * after 20 s, the stock warp to a neighbour in the brain's own room
         * table (gUnk_08D6CD0C, the table its off-screen hops use) -- one
         * that is wanted first, else one it did not come from */
        if (st->roomFrames == ROOM_GIVEUP / 3 && room < MAX_ROOMS && DoorList(room) != NULL) {
            const struct Unk_08D6CD0C *rt = gUnk_08D6CD0C[room];
            u16 pick = NO_ROOM, alt = NO_ROOM, i;

            for (i = 0; i < rt->unk49 && i < 0x20; i++) {
                u16 n = rt->unk4[i];

                if (n == room || n >= MAX_ROOMS || DoorList(n) == NULL || Blocked(brain, room, n))
                    continue;
                if (pick == NO_ROOM && Wanted(want, n, target, AbilityTier(brain->kirby->ability), NO_REC))
                    pick = n;
                if (alt == NO_ROOM && (!PREV(brain) || n != PREV(brain) - 1))
                    alt = n;
            }
            if (pick == NO_ROOM)
                pick = alt;
            if (pick != NO_ROOM && CpuWarpToRoom(brain, pick)) {
                PortCpuAiNote(brain->kirby->base.unk56, "warp-on", room, pick, 0);
                BlockWhy(brain, pick, room, "no-reenter");    /* not straight back in */
                return TRUE;
            }
        }
        if (st->roomFrames % 300)
            return FALSE;
        st->dest = NO_ROOM;
    }
    for (tries = 0; tries < 3; tries++) {
        if (st->dest == NO_ROOM || st->dest == 0) {
            u16 j = Route(brain, room, want, target);

            if (j == NO_REC && want2 >= 0)
                j = Route(brain, room, want2, target);
            if (j == NO_REC) {
                st->dest = NO_ROUTE;
                return FALSE;
            }
            SetDoor(brain, j);
        }
        if (HeadFor(brain))
            return TRUE;
    }
    return FALSE;
}

static bool32 GoalBodyguard(struct CpuBrain *brain)
{
    struct CpuAiState *st = ST(brain);
    struct Kirby *ward = Ward(brain);
    u16 room = brain->levelInfo->currentRoom;

    if (ward == NULL)
        return FALSE;
    if (Mend(brain))
        return TRUE;
    if (gCurLevelInfo[ward->base.unk56].currentRoom == room) {
        st->dest = NO_ROOM;
        brain->followKirby = ward;
        GoTo(brain, ward->base.x + ((brain->kirby->base.unk56 & 1) ? 0x1800 : -0x1800), ward->base.y);
        return TRUE;
    }
    /* another room: walk there, door by door; warp (as the stock brain
     * does off-screen) once a room has held it for a minute */
    if (st->roomFrames > ROOM_GIVEUP / 3) {
        u16 j = Route(brain, room, WANT_ROOM, gCurLevelInfo[ward->base.unk56].currentRoom);
        u16 hop = j != NO_REC ? sHintRec[j].dest : NO_ROOM;

        if (CpuWarpToRoom(brain, hop != NO_ROOM ? hop : gCurLevelInfo[ward->base.unk56].currentRoom)) {
            PortCpuAiNote(brain->kirby->base.unk56, "warp", room, hop, 0);
            return TRUE;
        }
    }
    return Travel(brain, WANT_ROOM, -1, gCurLevelInfo[ward->base.unk56].currentRoom);
}

static bool32 GoalExplorer(struct CpuBrain *brain, int adventurer)
{
    struct CpuAiState *st = ST(brain);
    u16 room = brain->levelInfo->currentRoom;
    struct Object *o;

    if (Mend(brain))
        return TRUE;
    if (adventurer) {
        /* a shard to take, a boss to fight: stay (the SENSE layer's scan
         * attacks it; the goal keeps it in reach) */
        /* (the hub shows the shards as objects of the same type: not those,
         * and no shard is chased for more than 20 s) */
        o = room != 0x321 && st->roomFrames < 1200 ? NearestObject(brain, 1) : NULL;
        if (o != NULL || (o = NearestObject(brain, 2)) != NULL) {
            if ((gPortSim->frame.frameCount % 600) == 0)
                PortCpuAiNote(brain->kirby->base.unk56, "engage", o->type, o->unk80, room);
            st->dest = NO_ROOM;
            GoTo(brain, o->base.x, o->base.y);
            return TRUE;
        }
    }
    if (sub_08002AAC(room) > sub_08002AD0(room) && st->roomFrames < 1200
        && (o = NearestObject(brain, 3)) != NULL) {
        GoTo(brain, o->base.x, o->base.y);          /* a chest here */
        return TRUE;
    }
    if (st->roomFrames > ROOM_GIVEUP && st->dest != NO_ROOM && st->dest != NO_ROUTE) {
        u16 d = st->dest;

        if (CpuWarpToRoom(brain, d)) {          /* do not stall: on, as stock */
            PortCpuAiNote(brain->kirby->base.unk56, "warp", room, d, 0);
            return TRUE;
        }
    }
    if (adventurer)
        return Travel(brain, WANT_BOSS, WANT_UNEXPLORED, 0);
    return Travel(brain, WANT_UNEXPLORED, -1, 0);
}

/* KATAM_CPUAI_LOG=2: the door graph as the game data has it, once (a
 * diagnostic for docs/design/CPU-AI.md; printed, never stored). */
void CpuAiDumpGraph(void)
{
    u16 r;

    for (r = 0; r < MAX_ROOMS; r++) {
        const struct Unk_08930E00_2 *e = DoorList(r);

        if (e == NULL)
            continue;
        PortCpuAiNote(-1, "graph-room", r, gUnk_08D6CD0C[r]->unk46, gUnk_08D6CD0C[r]->unk47);
        for (; e->unk00 != 0 && e->unk01 != 0; e = NEXT_ENTRY(e))
            if (e->unk00 == 2)
                PortCpuAiNote(-1, "graph-door", r, e->unk04, e->unk06 | (e->unk07 << 8));
    }
    if (PortCpuAiLogOn() > 2) {         /* KATAM_CPUAI_LOG=3: the door groups too */
        int t;
        u16 j;

        for (t = 0; t < HINT_TIERS; t++)
            HintEnsure(t);
        for (j = 0; j < sHintTotal; j++) {
            const struct HintDoor *p = &sHintRec[j];

            PortCpuAiNote(-1, "hint-door", sHintRoom[j] | (p->dest << 16), p->x | (p->y << 8) | (p->kind << 16),
                          (int)sHintReach[0][j]);
            PortCpuAiNote(-1, "hint-reach", j, (int)sHintReach[1][j], (int)sHintReach[3][j]);
        }
        for (j = 0; j < MAX_ROOMS; j++) {
            int i;

            for (i = 0; i < sGateCount[j]; i++) {
                const struct HintGate *g = &sGate[sGateFirst[j] + i];

                PortCpuAiNote(-1, "hint-gate", j | (g->kind << 16), g->x | (g->y << 8) | (g->slot << 16),
                              g->need | (g->sub << 8) | (g->timer << 16));
            }
        }
    }
}

/* CpuBrainTask's GOAL layer for a brain whose mode is not CLASSIC.  FALSE:
 * run the stock goal layer this frame. */
bool32 CpuAiGoal(struct CpuBrain *brain)
{
    struct CpuAiState *st = ST(brain);
    struct CpuAiBlocked *bl = BL(brain);
    struct Kirby *k = brain->kirby;
    int mode = CpuAiMode(brain);
    u16 room = brain->levelInfo->currentRoom;

    if (mode == PORT_CPUAI_CLASSIC) {
        st->modeSeen = 0;
        return FALSE;
    }
    if (st->modeSeen != mode + 1) {
        st->modeSeen = (u8)(mode + 1);
        st->dest = NO_ROOM;
    }
    if (st->room != room) {
        PortCpuAiNote(k->base.unk56, "room", st->room, room, k->hp);
        PREV(brain) = st->room ? st->room + 1 : 0;
        st->room = room;
        st->dest = NO_ROOM;
        st->roomFrames = 0;
        if (mode != PORT_CPUAI_BODYGUARD && room < MAX_ROOMS && DoorList(room) != NULL)
            sub_08002A80(room);         /* the team has been here: the map shows it */
    }
    if (st->roomFrames < 0xFFFF)
        st->roomFrames++;
    if (bl->lastHp != (u8)k->hp) {
        if (k->hp < (s8)bl->lastHp)
            PortCpuAiNote(k->base.unk56, "hurt", k->hp, k->maxHp, room);
        bl->lastHp = (u8)k->hp;
    }
    if (!(brain->flags & CPU_FLAG_FREE)) {
        if (st->roomFrames < FORCE_FREE)
            return FALSE;               /* the room's script first */
        brain->flags |= CPU_FLAG_FREE | CPU_FLAG_SCRIPT_WAITING;
    }
    if (brain->levelInfo->unk1E8 == NULL || room == 0x397)
        return FALSE;
    /* the opening (rooms 0x323..0x325) is the people's: the stock brain
     * takes a CPU left there to the hub (CpuTryRoomHop) */
    if (room >= 0x323 && room <= 0x325 && !HumanHere(room))
        return FALSE;
    switch (mode) {
    case PORT_CPUAI_BODYGUARD:
        return GoalBodyguard(brain);
    case PORT_CPUAI_EXPLORER:
        return GoalExplorer(brain, 0);
    default:
        return GoalExplorer(brain, 1);
    }
}

#ifdef __cplusplus
}
#endif
