/* Online co-op: the game's objects, as the co-op layers need to see them
 * (shared progress, platform/coop_world.c; shared enemies,
 * platform/coop_enemy.c).
 *
 *   room object lists   gLevelObjLists[gRoomProps[room].objectListIdx].obj
 *                       ->unk0, records of obj->unk1 bytes (36), ended by
 *                       unk1 == 0 (CreateLevelObjects, code_080023A4.c).
 *                       The ROM's, not PortRandoObjList's per-player copy:
 *                       a record's index is the same on every client.
 *   live objects        gCurLevelInfo[p].unk1F0, a linked list of up to 64
 *                       nodes; node->unk08 points at the object's
 *                       base.unk56 (CreateObject, sub_08002918).  In co-op
 *                       only player 0 has a live room, so p is always 0. */

#include <stddef.h>
#include <string.h>

#include "global.h"
#include "data.h"
#include "level.h"
#include "object.h"
#include "port/port.h"
#include "port/res.h"
#include "port/coop.h"

#ifdef __cplusplus
extern "C" {
#endif

static int RoomOk(u32 room)
{
    if (room == 0xFFFF || room >= Res_Count(RES_ROOM_PROPS))
        return 0;
    if (gUnk_08D6CD0C[room] == NULL)
        return 0;
    return gRoomProps[room].objectListIdx < Res_Count(RES_ROOM_OBJLIST);
}

/* The room's object records and how many there are; NULL when none. */
const struct ObjectTemplate *PortCoopRoomObjects(u32 room, u32 *count)
{
    const struct LevelInfo_1E4 *l;
    const struct ObjectTemplate *t, *first;
    u32 n = 0;

    *count = 0;
    if (!RoomOk(room))
        return NULL;
    l = gLevelObjLists[gRoomProps[room].objectListIdx].obj;
    if (l == NULL || l->unk0 == NULL)
        return NULL;
    first = l->unk0;
    for (t = first; t->unk1 != 0 && n < 1024; t = (const void *)((const u8 *)t + t->unk1))
        n++;
    /* every vanilla record is 36 bytes; a list that is not cannot be
     * indexed as an array, so say there is none */
    for (t = first; n && t->unk1 != 0; t = (const void *)((const u8 *)t + t->unk1))
        if (t->unk1 != sizeof(struct ObjectTemplate))
            return NULL;
    *count = n;
    return first;
}

/* Index of a level-spawned object's record in its room's list, or -1 for an
 * object made at run time (spawners, popups, CreateObjTemplateAndObj). */
int PortCoopObjectIndex(const struct Object *obj)
{
    const struct ObjectTemplate *first;
    u32 n;
    ptrdiff_t d;

    if (obj->objTemplate == NULL)
        return -1;
    if (obj->objTemplate->unk2 == 0 && obj->objTemplate->unk3 == 31)
        return -1;
    first = PortCoopRoomObjects(obj->base.roomId, &n);
    if (first == NULL)
        return -1;
    d = (const u8 *)obj->objTemplate - (const u8 *)first;
    if (d < 0 || d % (ptrdiff_t)sizeof(struct ObjectTemplate) != 0)
        return -1;
    d /= (ptrdiff_t)sizeof(struct ObjectTemplate);
    return d < (ptrdiff_t)n ? (int)d : -1;
}

/* Calls fn for every live struct Object of player 0's object list until it
 * returns nonzero (which is returned).  fn may destroy-flag the object but
 * must not create or free list nodes. */
int PortCoopForEachObject(int (*fn)(struct Object *, void *), void *arg)
{
    struct Unk_02023720 *node, *next;
    int guard = 0, r;

    if (gUnk_02023354 == NULL)
        return 0;
    for (node = gCurLevelInfo[0].unk1F0.first; node != NULL && guard < 0x40; node = next, guard++) {
        struct ObjectBase *b;

        next = node->next;
        if (node->unk08 == NULL)
            continue;
        b = (struct ObjectBase *)(void *)((u8 *)node->unk08 - offsetof(struct ObjectBase, unk56));
        if (b->header.kind != 1)
            continue;
        r = fn((struct Object *)b, arg);
        if (r)
            return r;
    }
    return 0;
}

/* Tests and diagnostics: the object records of types lo..hi in every room,
 * as (u16 room, u16 index, u8 type, u8 subtype1, s16 x, s16 y) -- 10 bytes
 * each.  Returns how many there are (writes up to max). */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EMSCRIPTEN_KEEPALIVE
#endif
int PortCoopFindObjects(int lo, int hi, unsigned char *out, int max)
{
    u32 room, rooms = Res_Count(RES_ROOM_PROPS), n, i;
    int found = 0;

    for (room = 0; room < rooms; room++) {
        const struct ObjectTemplate *t = PortCoopRoomObjects(room, &n);

        for (i = 0; t != NULL && i < n; i++) {
            u8 *p;

            if (t[i].type < lo || t[i].type > hi)
                continue;
            if (out != NULL && found < max) {
                p = out + found * 10;
                p[0] = (u8)room; p[1] = (u8)(room >> 8);
                p[2] = (u8)i; p[3] = (u8)(i >> 8);
                p[4] = t[i].type; p[5] = t[i].subtype1;
                p[6] = (u8)t[i].x; p[7] = (u8)((u16)t[i].x >> 8);
                p[8] = (u8)t[i].y; p[9] = (u8)((u16)t[i].y >> 8);
            }
            found++;
        }
    }
    return found;
}

#ifdef __cplusplus
}
#endif
