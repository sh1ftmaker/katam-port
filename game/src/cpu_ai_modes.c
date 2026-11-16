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

/* The rooms the door tiles of the Kirby's room lead to now (up to 16). */
static int DoorsHere(struct CpuBrain *brain, u16 *out)
{
    struct LevelInfo *li = brain->levelInfo;
    u8 slot = brain->kirby->base.unk56;
    u16 w = li->roomWidth >> 4, h = li->roomHeight >> 4, x, y;
    int n = 0, i;

    for (y = 0; y < h && y < 0x100; y++)
        for (x = 0; x < w && x < 0x100; x++)
            if (gCollisionAttributes[sub_08002434(li, x, y)] & 0x4000) {
                struct Unk_0888562C_3 *p = sub_080025AC(slot, x, y).pat3;

                if (p == NULL)
                    continue;
                for (i = 0; i < n && out[i] != p->unk08; i++)
                    ;
                if (i == n && n < 16)
                    out[n++] = p->unk08;
            }
    return n;
}

/* Breadth first over the door lists from `from`: the first room of the
 * shortest route to a room that is wanted (not `from` itself).  NO_ROOM
 * when none is reachable.  Doors this brain gave up on are left out. */
static u16 RouteEx(struct CpuBrain *brain, u16 from, int want, u16 target, u16 avoid)
{
    u16 parent[MAX_ROOMS];
    u16 queue[300];
    u16 here[16];
    u16 head = 0, tail = 0, r, i;
    int nhere, j;

    if (DoorList(from) == NULL)
        return NO_ROOM;
    nhere = DoorsHere(brain, here);
    for (i = 0; i < MAX_ROOMS; i++)
        parent[i] = NO_ROOM;
    parent[from] = from;
    queue[tail++] = from;
    while (head < tail) {
        const struct Unk_08930E00_2 *e;

        r = queue[head++];
        if (r != from && Wanted(want, r, target)) {
            while (parent[r] != from)
                r = parent[r];
            return r;
        }
        for (e = DoorList(r); e != NULL && e->unk00 != 0 && e->unk01 != 0; e = NEXT_ENTRY(e)) {
            u16 d = e->unk04;

            if (e->unk00 != 2 || d >= MAX_ROOMS || parent[d] != NO_ROOM || DoorList(d) == NULL)
                continue;
            if (Blocked(brain, r, d))
                continue;
            if (r == from && d == avoid)
                continue;
            if (r == from) {            /* the first step: a door this room shows now */
                for (j = 0; j < nhere && here[j] != d; j++)
                    ;
                if (j == nhere)
                    continue;
            }
            parent[d] = r;
            if (tail < ARRAY_COUNT(queue))
                queue[tail++] = d;
        }
    }
    return NO_ROOM;
}

/* Not straight back where it came from unless nothing else leads there
 * (two rooms each other's way to a goal ping-pong otherwise: the door
 * tiles of only the room it is in are known). */
static u16 Route(struct CpuBrain *brain, u16 from, int want, u16 target)
{
    u16 hop = PREV(brain) ? RouteEx(brain, from, want, target, (u16)(PREV(brain) - 1)) : NO_ROOM;

    return hop != NO_ROOM ? hop : RouteEx(brain, from, want, target, NO_ROOM);
}

/* The door tile in the Kirby's room that leads to `dest`, nearest the
 * Kirby.  FALSE if there is none (a door the list has but the room does
 * not show yet: behind a switch, a wall, an object). */
static bool32 FindDoorTile(struct CpuBrain *brain, u16 dest, u8 *ox, u8 *oy)
{
    struct LevelInfo *li = brain->levelInfo;
    u8 slot = brain->kirby->base.unk56;
    u16 w = li->roomWidth >> 4, h = li->roomHeight >> 4, x, y;
    s32 kx = brain->kirby->base.x >> 12, ky = brain->kirby->base.y >> 12;
    u32 best = 0xFFFFFFFF;

    for (y = 0; y < h && y < 0x100; y++)
        for (x = 0; x < w && x < 0x100; x++) {
            if (gCollisionAttributes[sub_08002434(li, x, y)] & 0x4000) {
                struct Unk_0888562C_3 *p = sub_080025AC(slot, x, y).pat3;

                if (p != NULL && p->unk08 == dest) {
                    u32 d = (u32)((x - kx) * (x - kx) + (y - ky) * (y - ky));

                    if (d < best) {
                        best = d;
                        *ox = (u8)x;
                        *oy = (u8)y;
                    }
                }
            }
        }
    return best != 0xFFFFFFFF;
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

/* Walk to the door toward `dest` (the first room of a route) and through
 * it.  FALSE if this room shows no such door (it is then given up). */
static bool32 HeadFor(struct CpuBrain *brain, u16 dest)
{
    struct CpuAiState *st = ST(brain);
    struct Kirby *k = brain->kirby;
    u16 room = brain->levelInfo->currentRoom;
    s32 kx, ky;

    if (st->dest != dest) {
        if (!FindDoorTile(brain, dest, &st->doorX, &st->doorY)) {
            Block(brain, room, dest);
            st->dest = NO_ROOM;
            return FALSE;
        }
        st->dest = dest;
        st->destFrames = 0;
        PR(brain)->best = 0;
        PR(brain)->stall = 0;
        PortCpuAiNote(k->base.unk56, "door", room, dest, st->doorX | (st->doorY << 8));
    }
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
    u16 hop;
    int tries;

    if (st->dest == NO_ROUTE) {         /* nothing last time: ask again now and then */
        if (st->roomFrames % 300)
            return FALSE;
        st->dest = NO_ROOM;
    }
    for (tries = 0; tries < 3; tries++) {
        if (st->dest != NO_ROOM && st->dest != 0) {
            hop = st->dest;
        } else {
            hop = Route(brain, room, want, target);
            if (hop == NO_ROOM && want2 >= 0)
                hop = Route(brain, room, want2, target);
        }
        if (hop == NO_ROOM) {
            st->dest = NO_ROUTE;
            return FALSE;
        }
        if (HeadFor(brain, hop))
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
        u16 hop = Route(brain, room, WANT_ROOM, gCurLevelInfo[ward->base.unk56].currentRoom);

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
