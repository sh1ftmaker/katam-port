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

static void Block(struct CpuBrain *brain, u16 from, u16 to)
{
    struct CpuAiBlocked *bl = BL(brain);

    if (bl->next >= 3)
        bl->next = 0;
    bl->from[bl->next] = from + 1;
    bl->to[bl->next] = to + 1;
    bl->next++;
    PortCpuAiNote(brain->kirby->base.unk56, "blocked", from, to, 0);
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

static bool32 Wanted(int want, u16 room, u16 target)
{
    switch (want) {
    case WANT_UNEXPLORED:
        return !sub_08002A5C(room) || sub_08002AAC(room) > sub_08002AD0(room);
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
#define HINT_POCKET  0xFE       /* arrival in a part of the room with no door */
#define NO_REC       0xFFFF

static const struct Unk_0888562C_3 *sHintRec[HINT_RECS];
static u16 sHintRoom[HINT_RECS];
static u16 sHintFirst[MAX_ROOMS];   /* first record of the room, + 1; 0 none */
static u8  sHintCount[MAX_ROOMS];
static u16 sHintTotal;
static u8  sHintGroup[HINT_TIERS][HINT_RECS];
static u8  sHintArrive[HINT_TIERS][HINT_RECS];
static u8  sHintBuilt;              /* bit 0: the index; bit 1 + t: tier t */
static uintptr_t sHintSig;
static u8  sHintCells[HINT_CELLS];  /* scratch, written before every read */
static u8  sHintLab[HINT_CELLS];    /* scratch */
static u16 sHintQueue[HINT_CELLS];  /* scratch */

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

/* A cell a floating Kirby can pass, breaking blocks up to `tier`. */
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
        }
    return sig;
}

static void HintIndex(void)
{
    u16 r;

    sHintTotal = 0;
    for (r = 0; r < MAX_ROOMS; r++) {
        const struct Unk_0888562C_3 *p = RoomRecords(r);
        int n = 0, guard;

        sHintFirst[r] = 0;
        sHintCount[r] = 0;
        if (p == NULL)
            continue;
        for (guard = 0; guard < 512 && p->unk00 != 0 && p->unk04 != 0; guard++) {
            if (p->unk00 == 3 && sHintTotal < HINT_RECS && n < 0xFD) {
                if (!n)
                    sHintFirst[r] = sHintTotal + 1;
                sHintRec[sHintTotal] = p;
                sHintRoom[sHintTotal] = r;
                sHintTotal++;
                n++;
            }
            p = (const struct Unk_0888562C_3 *)(const void *)((const u8 *)p + p->unk04);
        }
        sHintCount[r] = (u8)n;
    }
}

/* Flood `lab` (0xFF = not reached) with `mark` from cell `start` over the
 * map `cells` (w x h), passable at `tier`.  Returns the cells reached. */
static int Flood(const u8 *cells, int w, int h, int start, u8 mark, int tier, u8 *lab)
{
    int head = 0, tail = 0;

    if (start < 0 || start >= w * h || lab[start] != 0xFF)
        return 0;
    lab[start] = mark;
    sHintQueue[tail++] = (u16)start;
    while (head < tail) {
        int i = sHintQueue[head++], x = i % w, y = i / w, k;
        static const s8 dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };

        for (k = 0; k < 4; k++) {
            int nx = x + dx[k], ny = y + dy[k], j;

            if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                continue;
            j = ny * w + nx;
            if (lab[j] == 0xFF && Passable(gCollisionAttributes[cells[j]], tier)) {
                lab[j] = mark;
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
    const struct RoomTiledBG *fg = gForegroundTilemaps[gRoomProps[r].mapDataIdx];
    const u8 *src;
    u32 size;

    if (sm == NULL || fg == NULL || sm->unk0 == NULL)
        return FALSE;
    src = (const u8 *)(const void *)sm->unk0;
    size = (u32)src[1] | ((u32)src[2] << 8) | ((u32)src[3] << 16);
    *w = fg->width / 2;
    *h = fg->height / 2;
    if ((src[0] & 0xF0) != 0x30 || size > HINT_CELLS || (u32)(*w * *h) > size || *w <= 0)
        return FALSE;
    RLUnCompWram(src, sHintCells);
    return TRUE;
}

static void HintTier(int t)
{
    u16 r, s;

    for (s = 0; s < sHintTotal; s++) {
        sHintGroup[t][s] = 0xFF;
        sHintArrive[t][s] = HINT_POCKET;
    }
    for (r = 0; r < MAX_ROOMS; r++) {
        int w, h, i;
        u16 f = sHintFirst[r];

        if (!f || !RomCells(r, &w, &h))
            continue;
        f--;
        memset(sHintLab, 0xFF, sizeof(sHintLab));
        for (i = 0; i < sHintCount[r]; i++) {
            const struct Unk_0888562C_3 *p = sHintRec[f + i];
            int c = p->unk03 * w + p->unk02;

            if (c >= w * h || !Passable(gCollisionAttributes[sHintCells[c]], t))
                continue;               /* off the map, or covered (a block on it) */
            if (sHintLab[c] == 0xFF)    /* a new group: this door is its first */
                Flood(sHintCells, w, h, c, (u8)i, t, sHintLab);
            sHintGroup[t][f + i] = sHintLab[c];
        }
        /* where the doors into this room deliver */
        for (s = 0; s < sHintTotal; s++) {
            const struct Unk_0888562C_3 *p = sHintRec[s];
            int x = p->unk0A - 1, y = p->unk0B - 1;

            if (p->unk08 != r || x < 0 || y < 0 || x >= w || y >= h)
                continue;
            if (sHintLab[y * w + x] < 0xFE)
                sHintArrive[t][s] = sHintLab[y * w + x];
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

/* The doors of the Kirby's room it can reach now (its live map, from its
 * cell): sHintLab holds 0 at reached cells.  FALSE if it is nowhere sane. */
static bool32 LiveReach(struct CpuBrain *brain, int t)
{
    struct LevelInfo *li = brain->levelInfo;
    const u8 *cells = gUnk_02024ED0[li->unk65E];
    int w = li->roomWidth >> 4, h = li->roomHeight >> 4;
    int kx = brain->kirby->base.x >> 12, ky = brain->kirby->base.y >> 12, k;
    static const s8 ox[5] = { 0, 0, 0, -1, 1 }, oy[5] = { 0, -1, 1, 0, 0 };

    if (w <= 0 || h <= 0 || w * h > HINT_CELLS)
        return FALSE;
    memset(sHintLab, 0xFF, sizeof(sHintLab));
    for (k = 0; k < 5; k++) {          /* its cell, else one beside it */
        int x = kx + ox[k], y = ky + oy[k];

        if (x < 0 || y < 0 || x >= w || y >= h)
            continue;
        if (Passable(gCollisionAttributes[cells[y * w + x]], t)) {
            Flood(cells, w, h, y * w + x, 0, t, sHintLab);
            return TRUE;
        }
    }
    return FALSE;
}

/* A door record of the Kirby's room it can use now: the door tile is in the
 * live map (not behind a switch, a wall, an object) and it can get there. */
static bool32 DoorOpenHere(struct CpuBrain *brain, const struct Unk_0888562C_3 *p)
{
    struct LevelInfo *li = brain->levelInfo;
    int w = li->roomWidth >> 4, h = li->roomHeight >> 4, c = p->unk03 * w + p->unk02, k;
    u8 v;

    if (p->unk02 >= w || p->unk03 >= h)
        return FALSE;
    v = gUnk_02024ED0[li->unk65E][c];
    if (v != p->unk01 || !(gCollisionAttributes[v] & 0x4000))
        return FALSE;
    if (sHintLab[c] == 0)
        return TRUE;
    for (k = 0; k < 4; k++) {          /* reached beside it */
        int x = p->unk02 + (k == 0) - (k == 1), y = p->unk03 + (k == 2) - (k == 3);

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
    u16 first[HINT_RECS];               /* per state (its group's first record) */
    u16 queue[HINT_RECS];
    u16 head = 0, tail = 0, f, i, s;
    int t = AbilityTier(brain->kirby->ability);
    s32 kx = brain->kirby->base.x >> 12, ky = brain->kirby->base.y >> 12;
    u8 used[256];
    int n;

    if (DoorList(from) == NULL)
        return NO_REC;
    HintEnsure(t);
    if (!sHintFirst[from] || !LiveReach(brain, t))
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
            const struct Unk_0888562C_3 *p = sHintRec[f + i];
            s32 dx = p->unk02 - kx, dy = p->unk03 - ky;
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
            const struct Unk_0888562C_3 *p = sHintRec[f + bi];
            u16 d = p->unk08, df;
            u8 a = sHintArrive[t][f + bi];

            if (d >= MAX_ROOMS || DoorList(d) == NULL || d == avoid || Blocked(brain, from, d)
                || !DoorOpenHere(brain, p))
                continue;
            if (d != from && Wanted(want, d, target))
                return f + bi;
            if (a >= HINT_POCKET || !sHintFirst[d])
                continue;
            df = sHintFirst[d] - 1;
            s = df + a;
            if (first[s] == NO_REC) {
                first[s] = f + bi;
                queue[tail++] = s;
            }
        }
    }
    while (head < tail) {
        u16 g = queue[head++], r = sHintRoom[g], rf = sHintFirst[r] - 1;
        u8 grp = (u8)(g - rf);

        for (i = 0; i < sHintCount[r]; i++) {
            const struct Unk_0888562C_3 *p = sHintRec[rf + i];
            u16 d = p->unk08;
            u8 a;

            if (sHintGroup[t][rf + i] != grp || d >= MAX_ROOMS || DoorList(d) == NULL
                || Blocked(brain, r, d))
                continue;
            if (d != from && Wanted(want, d, target))
                return first[g];
            a = sHintArrive[t][rf + i];
            if (a >= HINT_POCKET || !sHintFirst[d])
                continue;
            s = sHintFirst[d] - 1 + a;
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

    return j != NO_REC ? j : RouteEx(brain, from, want, target, NO_ROOM);
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

static void GoTo(struct CpuBrain *brain, s32 x, s32 y)
{
    GoToEx(brain, x, y, FALSE);
}

static struct Object *NearestObject(struct CpuBrain *brain, int kind)
{
    PTR32(struct ObjectBase) *o = GetRoomObjectCollisionList(&brain->kirby->base);
    struct Object *best = NULL;
    s32 bd = 0x7FFFFFFF;

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

/* Choose door record `j` (sHintRec) of the Kirby's room. */
static void SetDoor(struct CpuBrain *brain, u16 j)
{
    struct CpuAiState *st = ST(brain);
    const struct Unk_0888562C_3 *p = sHintRec[j];

    st->dest = p->unk08;
    st->doorX = p->unk02;
    st->doorY = p->unk03;
    st->destFrames = 0;
    PR(brain)->best = 0;
    PR(brain)->stall = 0;
    PortCpuAiNote(brain->kirby->base.unk56, "door", brain->levelInfo->currentRoom, st->dest,
                  st->doorX | (st->doorY << 8));
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
        struct Unk_0888562C_3 *p = sub_080025AC(k->base.unk56, st->doorX, st->doorY).pat3;

        if (p != NULL && p->unk08 == dest && sub_080551FC(k, dest, p->unk0A, p->unk0B)) {
            st->doors++;
            PortCpuAiNote(k->base.unk56, "through", room, dest, k->hp);
            return TRUE;
        }
    }
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
        GoToEx(brain, ((s32)st->doorX << 12) + 0x800, ((s32)st->doorY << 12) + 0x800,
               (pr->stall / 300) & 1);
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
        u16 hop = j != NO_REC ? sHintRec[j]->unk08 : NO_ROOM;

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
            const struct Unk_0888562C_3 *p = sHintRec[j];

            PortCpuAiNote(-1, "hint-door", sHintRoom[j] | (p->unk08 << 16), p->unk02 | (p->unk03 << 8),
                          sHintGroup[0][j] | (sHintGroup[1][j] << 8) | (sHintGroup[3][j] << 16)
                          | (sHintArrive[1][j] << 24));
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
