/* The divergence census (NETPLAY-V2 §3.4, M1).
 *
 * K seats run the same recording with gLocalPlayerId = 0..K-1 -- the one
 * thing that differs between peers of a session by design -- and after every
 * round (every seat at the same frame boundary) their snapshots are compared
 * in 64-byte blocks.  A block that differs is either
 *
 *   (a) per-peer presentation that sim code happens to write: a mask, listed
 *       in tests/census/masks.txt with its symbol and its class (S* or P), or
 *   (b) a real divergence -- simulation state that depends on which Kirby the
 *       local player is -- which is a bug to fix in game code.
 *
 * Driven from platform/seats.c (census mode: the seats re-pin the focus and
 * touch nothing else; the inputs come from the replayed recording).  Native:
 *
 *     katam --replay REC --census 4:OUT [--census-masks tests/census/masks.txt]
 *
 * OUT gets one line per block that ever differed (first and last frame,
 * frames differing, which seats, masked or not, and the owning task for heap
 * memory), then a summary.  tools/census.py maps the blocks to symbols and
 * writes the mask table. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/port.h"
#include "port/rollback.h"
#include "port/simblock.h"
#include "port/census.h"
#include "fnid_ids.h"
#include "port/seats.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLK 64

struct Blk {
    u32 first, last, frames;
    u32 seatMask;
    u64 diffBits;       /* bytes that ever differed (bit i = byte i)        */
    u64 firstBits;      /* ...and at the first differing frame               */
    u32 unmaskedFrames; /* frames with a differing byte outside the masks    */
    u32 owner;          /* main of the task whose struct holds it, raw      */
    u32 ownerBase;      /* that struct's address                            */
};

struct Mask { u32 lo, hi; };

static int sOn;
static char *sOut;
static u32 sBytes, sNumBlk;
static struct Blk *sBlk;
static struct Mask *sMasks;
static int sNumMasks;
static u32 sRounds, sRoundsAnyDiff, sRoundsUnmasked, sFirstUnmasked = 0xFFFFFFFFu;
static u32 sFirstAny = 0xFFFFFFFFu;
static u32 sSimOff;     /* the sim block's offset in a snapshot: the frame # */

/* Address of block b, or 0 for host state. */
static u32 BlkAddr(u32 b, const char **region)
{
    struct PortRbPart p;
    int i;
    u32 off = b * BLK;

    for (i = 0; PortRbSnapshotPart(i, &p); i++)
        if (off >= p.offset && off < p.offset + p.size) {
            if (region)
                *region = p.name;
            return p.base ? p.base + (off - p.offset) : 0x80000000u | off;
        }
    if (region)
        *region = "?";
    return 0;
}

/* The bytes of the block at `addr` the masks cover, as a bit set. */
static u64 MaskBits(u32 addr)
{
    u64 m = 0;
    int i, k;

    for (i = 0; i < sNumMasks; i++)
        for (k = 0; k < BLK; k++)
            if (addr + (u32)k >= sMasks[i].lo && addr + (u32)k < sMasks[i].hi)
                m |= (u64)1 << k;
    return m;
}
static u64 *sMaskBits;

/* Per-round masks that move with the heap: the screen-space position of every
 * sprite the census can find (S*: OAM coordinates relative to the local
 * viewport, NETPLAY-V2 §3.1).  Up to 512 four-byte spans a round. */
#define MAX_DYN 512
static u32 sDynAt[MAX_DYN];
static int sNumDyn;

static void DynAdd(u32 addr, u32 len)
{
    u32 k;

    for (k = 0; k < len && sNumDyn < MAX_DYN; k += 4)
        sDynAt[sNumDyn++] = addr + k;
}

/* The S* fields of a struct Sprite (gba_layout.h): tilesVram (0..3, the OBJ
 * VRAM allocator's answer), x/y (16..19, screen space of the local view),
 * unk1C/unk1D (28..29, the OAM slot order, which sorts against the local
 * Kirby) and palId's word (28..31). */
static void DynSprite(u32 sprite)
{
    DynAdd(sprite + 0, 4);
    DynAdd(sprite + 16, 4);
    DynAdd(sprite + 28, 4);
}

static void DynCollect(const u8 *state)
{
    struct PortRbPart iw;
    int i, k;

    sNumDyn = 0;
    /* gKirbys: base.sprite and sprites[], 424 bytes apart, sprites at 124,
     * 40 bytes each, three of them before `task` at 204. */
    for (i = 0; i < 4; i++) {
        u32 kb = 0x02020EE0u + (u32)i * 424u;

        DynSprite(kb + 16);
        for (k = 0; k < 2; k++)
            DynSprite(kb + 124 + (u32)k * 40);
    }
    if (!PortRbSnapshotPart(1, &iw))
        return;
    for (i = 0; i < 0x80; i++) {
        const u8 *t = state + iw.offset + (0x030019F0u - GBA_IWRAM_BASE) + (u32)i * 0x14;
        u32 fn = (u32)t[8] | ((u32)t[9] << 8) | ((u32)t[10] << 16) | ((u32)t[11] << 24);
        u16 off = (u16)(t[6] | (t[7] << 8));
        u16 flags = (u16)(t[0x12] | (t[0x13] << 8));

        /* Task::main holds a canonical code ID (platform/port/fnid.h). */
        if (fn == 0 || fn != FNID_ObjectMain)
            continue;
        DynSprite(((flags & 0x10) ? GBA_EWRAM_BASE + ((u32)off << 2)
                                  : GBA_IWRAM_BASE + off) + 16);
    }
}

/* The dynamic masks over block b (address a), as bits. */
static u64 DynBits(u32 a)
{
    u64 m = 0;
    int i;

    for (i = 0; i < sNumDyn; i++)
        if (sDynAt[i] + 4 > a && sDynAt[i] < a + BLK) {
            u32 k;

            for (k = 0; k < 4; k++)
                if (sDynAt[i] + k >= a && sDynAt[i] + k < a + BLK)
                    m |= (u64)1 << (sDynAt[i] + k - a);
        }
    return m;
}

static void LoadMasks(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];

    if (f == NULL) {
        PortError("[katam-port] census: cannot read masks %s", path);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        unsigned lo, hi;

        if (line[0] == '#' || sscanf(line, "%x %x", &lo, &hi) != 2)
            continue;
        sMasks = (struct Mask *)realloc(sMasks, (size_t)(sNumMasks + 1) * sizeof *sMasks);
        sMasks[sNumMasks].lo = lo;
        sMasks[sNumMasks].hi = hi;
        sNumMasks++;
    }
    fclose(f);
    PortLog("[katam-port] census: %d masks from %s", sNumMasks, path);
}

/* The task whose struct starts nearest below `addr` (within 4 KiB), from the
 * seat-0 snapshot's own task table. */
static void Owner(const u8 *state, u32 addr, u32 *main, u32 *base)
{
    struct PortRbPart iw;
    u32 best = 0, bestMain = 0;
    int i;

    *main = *base = 0;
    if (!PortRbSnapshotPart(1, &iw) || iw.base != GBA_IWRAM_BASE)
        return;
    for (i = 0; i < 0x80; i++) {
        const u8 *t = state + iw.offset + (0x030019F0u - GBA_IWRAM_BASE) + (u32)i * 0x14;
        u16 off = (u16)(t[6] | (t[7] << 8));
        u32 fn = (u32)t[8] | ((u32)t[9] << 8) | ((u32)t[10] << 16) | ((u32)t[11] << 24);
        u16 flags = (u16)(t[0x12] | (t[0x13] << 8));
        u32 at;

        if (fn == 0)
            continue;
        at = (flags & 0x10) ? GBA_EWRAM_BASE + ((u32)off << 2) : GBA_IWRAM_BASE + off;
        if (at <= addr && addr - at < 0x1000 && at > best) {
            best = at;
            bestMain = fn;
        }
    }
    *main = bestMain;
    *base = best;
}

static void Report(void)
{
    FILE *f;
    u32 b, n = 0, nUnmasked = 0;

    if (!sOn || sOut == NULL)
        return;
    f = fopen(sOut, "w");
    if (f == NULL) {
        PortError("[katam-port] census: cannot write %s", sOut);
        return;
    }
    fprintf(f, "census seats %d blocks %u rounds %u masks %d\n",
            PortSeatCount(), (unsigned)sNumBlk, (unsigned)sRounds, sNumMasks);
    fprintf(f, "# block REGION ADDR FIRST LAST FRAMES SEATMASK UNMASKEDFRAMES OWNERMAIN OWNERBASE DIFFBYTES FIRSTBYTES\n");
    for (b = 0; b < sNumBlk; b++) {
        const char *rg;
        u32 a;
        int m;

        if (sBlk[b].frames == 0)
            continue;
        a = BlkAddr(b, &rg);
        m = sBlk[b].unmaskedFrames != 0;
        n++;
        if (m)
            nUnmasked++;
        fprintf(f, "block %s %08X %u %u %u %X %u %08X %08X %016llX %016llX\n", rg,
                (unsigned)a, (unsigned)sBlk[b].first, (unsigned)sBlk[b].last,
                (unsigned)sBlk[b].frames, (unsigned)sBlk[b].seatMask,
                (unsigned)sBlk[b].unmaskedFrames, (unsigned)sBlk[b].owner,
                (unsigned)sBlk[b].ownerBase, (unsigned long long)sBlk[b].diffBits,
                (unsigned long long)sBlk[b].firstBits);
    }
    fprintf(f, "summary rounds %u differing %u unmasked %u first-any %d "
            "first-unmasked %d blocks %u unmasked-blocks %u\n",
            (unsigned)sRounds, (unsigned)sRoundsAnyDiff, (unsigned)sRoundsUnmasked,
            sFirstAny == 0xFFFFFFFFu ? -1 : (int)sFirstAny,
            sFirstUnmasked == 0xFFFFFFFFu ? -1 : (int)sFirstUnmasked,
            (unsigned)n, (unsigned)nUnmasked);
    fclose(f);
    PortLog("[katam-port] census: %u rounds, %u with any difference, %u with an "
            "unmasked one (first at frame %d); %u blocks, %u unmasked -> %s",
            (unsigned)sRounds, (unsigned)sRoundsAnyDiff, (unsigned)sRoundsUnmasked,
            sFirstUnmasked == 0xFFFFFFFFu ? -1 : (int)sFirstUnmasked,
            (unsigned)n, (unsigned)nUnmasked, sOut);
}

int PortCensusInit(const char *out, const char *masks)
{
    struct PortRbPart p;
    int i;

    sBytes = PortRbSnapshotBytes();
    sNumBlk = (sBytes + BLK - 1) / BLK;
    sBlk = (struct Blk *)calloc(sNumBlk, sizeof *sBlk);
    if (sBlk == NULL)
        return 0;
    sOut = out ? strdup(out) : NULL;
    if (masks != NULL)
        LoadMasks(masks);
    sMaskBits = (u64 *)calloc(sNumBlk, sizeof *sMaskBits);
    if (sMaskBits == NULL)
        return 0;
    if (sNumMasks) {
        u32 b;

        for (b = 0; b < sNumBlk; b++)
            sMaskBits[b] = MaskBits(BlkAddr(b, NULL));
    }
    for (i = 0; PortRbSnapshotPart(i, &p); i++)
        if (p.base == PORT_SIM_BASE)
            sSimOff = p.offset;
    sOn = 1;
    atexit(Report);
    return 1;
}

int PortCensusActive(void) { return sOn; }

void PortCensusRound(const u8 *const *states, int n)
{
    u32 frame, b, dynBlk;
    u64 dynBit = 0;
    int s, any = 0, unmasked = 0;

    if (!sOn || n < 2)
        return;
    memcpy(&frame, states[0] + sSimOff + PORT_SIM_OFF_FRAME, 4);
    sRounds++;
    /* PORT_CENSUS_DUMP=F[,F...]: every seat's raw snapshot at those frames,
     * to OUT.F.seatN, for a byte-level look at a divergence. */
    {
        const char *e = getenv("PORT_CENSUS_DUMP");

        while (e != NULL && *e) {
            if ((u32)strtoul(e, NULL, 10) == frame && sOut != NULL) {
                char path[1024];
                int k;

                for (k = 0; k < n; k++) {
                    FILE *f;

                    snprintf(path, sizeof path, "%s.%u.seat%d", sOut, (unsigned)frame, k);
                    f = fopen(path, "wb");
                    if (f) {
                        fwrite(states[k], 1, sBytes, f);
                        fclose(f);
                    }
                }
            }
            e = strchr(e, ',');
            if (e)
                e++;
        }
    }
    /* The display task's captured player id: GAME_REPIN_FOCUS writes it, so
     * it is per peer by design, and its struct moves with the heap. */
    {
        struct PortRbPart ew, iw;
        u32 task, at = 0;

        dynBlk = 0xFFFFFFFFu;
        if (PortRbSnapshotPart(0, &ew) && PortRbSnapshotPart(1, &iw)) {
            memcpy(&task, states[0] + ew.offset + (0x02023354u - GBA_EWRAM_BASE), 4);
            if (task >= GBA_IWRAM_BASE && task + 0x14 <= GBA_IWRAM_BASE + GBA_IWRAM_SIZE) {
                const u8 *t = states[0] + iw.offset + (task - GBA_IWRAM_BASE);
                u16 o = (u16)(t[6] | (t[7] << 8)), fl = (u16)(t[0x12] | (t[0x13] << 8));

                at = (fl & 0x10) ? GBA_EWRAM_BASE + ((u32)o << 2) : GBA_IWRAM_BASE + o;
            }
            if (at >= GBA_EWRAM_BASE && at < GBA_EWRAM_BASE + GBA_EWRAM_SIZE) {
                dynBlk = (ew.offset + at - GBA_EWRAM_BASE) / BLK;
                dynBit = (u64)1 << ((at - GBA_EWRAM_BASE) % BLK);
            } else if (at >= GBA_IWRAM_BASE && at < GBA_IWRAM_BASE + GBA_IWRAM_SIZE) {
                dynBlk = (iw.offset + at - GBA_IWRAM_BASE) / BLK;
                dynBit = (u64)1 << ((at - GBA_IWRAM_BASE) % BLK);
            }
        }
    }
    DynCollect(states[0]);
    for (b = 0; b < sNumBlk; b++) {
        u32 off = b * BLK, len = sBytes - off < BLK ? sBytes - off : BLK;
        u32 mask = 0;
        u64 bits = 0;

        for (s = 1; s < n; s++)
            if (memcmp(states[0] + off, states[s] + off, len) != 0) {
                u32 k;

                mask |= 1u << s;
                for (k = 0; k < len; k++)
                    if (states[0][off + k] != states[s][off + k])
                        bits |= (u64)1 << k;
            }
        if (!mask)
            continue;
        any = 1;
        if (sBlk[b].frames == 0) {
            u32 a = BlkAddr(b, NULL);

            sBlk[b].first = frame;
            sBlk[b].firstBits = bits;
            if (a >= GBA_EWRAM_BASE && a < GBA_IWRAM_BASE + GBA_IWRAM_SIZE)
                Owner(states[0], a, &sBlk[b].owner, &sBlk[b].ownerBase);
        }
        sBlk[b].last = frame;
        sBlk[b].frames++;
        sBlk[b].seatMask |= mask;
        sBlk[b].diffBits |= bits;
        if (dynBlk == b)
            bits &= ~dynBit;
        if (bits & ~sMaskBits[b]) {
            u32 a = BlkAddr(b, NULL);

            if (a >= GBA_EWRAM_BASE && a < GBA_IWRAM_BASE + GBA_IWRAM_SIZE)
                bits &= ~DynBits(a);
        }
        if (bits & ~sMaskBits[b]) {
            sBlk[b].unmaskedFrames++;
            unmasked = 1;
        }
    }
    if (any) {
        sRoundsAnyDiff++;
        if (sFirstAny == 0xFFFFFFFFu)
            sFirstAny = frame;
    }
    if (unmasked) {
        sRoundsUnmasked++;
        if (sFirstUnmasked == 0xFFFFFFFFu)
            sFirstUnmasked = frame;
    }
}

#ifdef __cplusplus
}
#endif
