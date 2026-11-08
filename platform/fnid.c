/* Code ID -> host function (netplay-v2 R5).  port/fnid.h says what an ID is
 * and why the game stores them instead of host pointers.
 *
 * The table is built once, from every file's registration block
 * (gPortFnIdTables, game/generated/fnid_tables.c), into an open-addressing
 * hash that port/fnid.h probes inline.  It lives in host memory: nothing here
 * is game state, so its layout may differ between builds freely. */

#include "port/port.h"
#include "port/fnid.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const struct PortFnIdEntry *const gPortFnIdTables[];

uint32_t gPortFnHashId[PORT_FNID_HASH_SIZE];
PortFn gPortFnHashFn[PORT_FNID_HASH_SIZE];
static const char *sHashName[PORT_FNID_HASH_SIZE];
static int sInited;
static u32 sCount;

/* An empty slot.  No ID is 1: ROM IDs are code addresses, name IDs live in
 * 0x0E000001.., and 0 (NULL) must miss so that a call through it reports. */
#define EMPTY 1u

void PortFnIdInit(void)
{
    u32 i;
    const struct PortFnIdEntry *const *t;

    if (sInited)
        return;
    sInited = 1;
    for (i = 0; i < PORT_FNID_HASH_SIZE; i++)
        gPortFnHashId[i] = EMPTY;
    for (t = gPortFnIdTables; *t; t++) {
        const struct PortFnIdEntry *e;
        for (e = *t; e->id; e++) {
            u32 h = PORT_FNID_HASH(e->id);
            while (gPortFnHashId[h] != EMPTY) {
                if (gPortFnHashId[h] == e->id) {
                    /* Two functions under one ID.  The generator rules this
                     * out; a hand edit of a registration block could not. */
                    fprintf(stderr, "[katam-port] fnid: 0x%08X registered twice "
                            "(%s and %s)\n", (unsigned)e->id, sHashName[h], e->name);
                    abort();
                }
                h = (h + 1) & (PORT_FNID_HASH_SIZE - 1);
            }
            gPortFnHashId[h] = e->id;
            gPortFnHashFn[h] = e->fn;
            sHashName[h] = e->name;
            sCount++;
        }
    }
    if (sCount != PORT_FNID_COUNT)
        PortLog("[katam-port] fnid: %u code IDs registered, fnid_ids.h names %u\n",
                (unsigned)sCount, (unsigned)PORT_FNID_COUNT);
}

static int Find(u32 id)
{
    u32 h = PORT_FNID_HASH(id);
    if (!sInited)
        PortFnIdInit();
    while (gPortFnHashId[h] != EMPTY) {
        if (gPortFnHashId[h] == id)
            return (int)h;
        h = (h + 1) & (PORT_FNID_HASH_SIZE - 1);
    }
    return -1;
}

PortFn PortFnLookupSlow(uint32_t id, const char *file, int line)
{
    int h = Find(id);
    if (h >= 0 && id != EMPTY)
        return gPortFnHashFn[h];
    /* Not a code ID.  Release builds stop too: the value is either a host
     * pointer that bypassed FN, corrupt state, or a ROM function the port
     * does not implement, and running on would execute the wrong code or
     * desync silently -- the same thing calling a garbage pointer did before,
     * only with the value and the call site in the report. */
    fprintf(stderr, "[katam-port] call through %s code ID 0x%08X at %s:%d\n",
            id ? "unknown" : "NULL", (unsigned)id, file ? file : "?", line);
    PortError("call through unknown code ID 0x%08X at %s:%d\n",
              (unsigned)id, file ? file : "?", line);
    abort();
    return 0;
}

PortFn PortFnHostOrNull(uint32_t id)
{
    int h = Find(id);
    return h >= 0 && id != EMPTY ? gPortFnHashFn[h] : 0;
}

const char *PortFnName(uint32_t id)
{
    int h = Find(id);
    return h >= 0 && id != EMPTY ? sHashName[h] : 0;
}

int PortFnIdKnown(uint32_t v)
{
    return v == 0 || (v != EMPTY && Find(v) >= 0);
}

uint32_t PortFnIdOfHost(PortFn fn)
{
    u32 i;
    if (!sInited)
        PortFnIdInit();
    for (i = 0; i < PORT_FNID_HASH_SIZE; i++)
        if (gPortFnHashId[i] != EMPTY && gPortFnHashFn[i] == fn)
            return gPortFnHashId[i];
    return 0;
}

#ifdef __cplusplus
}
#endif
