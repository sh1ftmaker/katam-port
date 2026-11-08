/* Input recording, replay, and the per-frame simulation hash.
 *
 * See platform/port/inputlog.h for the interface and docs/DETERMINISM.md for
 * what "simulation state" is taken to mean and why.  Everything here is
 * shared by the web and native builds: the point is that a recording made on
 * one is replayed by the identical code on the other, at the identical point
 * in the frame.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/port.h"
#include "port/backend.h"
#include "port/inputlog.h"

/* C linkage for the 64-bit builds -- see tools/legacy-sync/cxxify.py. */
#ifdef __cplusplus
extern "C" {
#endif

/* --- the event list --------------------------------------------------------
 *
 * Keys are stored per frame, uncompressed, because the latch indexes them by
 * frame; they are run-length encoded only in the file.  Warps are a short
 * list.  Both grow as needed. */
static u16 *sKeys;
static u32 sKeysCap;
static u32 sKeysLen;            /* frames covered                             */

struct Warp { u32 frame; u8 player, x, y; u16 room; };
static struct Warp *sWarps;
static u32 sNumWarps, sWarpsCap;

static int sReplaying, sRecording;
static u32 sRunFrames;          /* what the host ran, if it says              */

static int sHaveRando, sHaveView;
static u32 sRandoFlags, sRandoSeed;
static u32 sViewMode, sViewCull, sViewPin, sViewTile;
static s32 sViewPadX, sViewPadY;

static int GrowKeys(u32 want)
{
    u32 cap = sKeysCap ? sKeysCap : 4096;
    u16 *k;

    if (want <= sKeysCap)
        return 1;
    while (cap < want)
        cap *= 2;
    k = (u16 *)realloc(sKeys, cap * sizeof *k);
    if (k == NULL)
        return 0;
    memset(k + sKeysCap, 0, (cap - sKeysCap) * sizeof *k);
    sKeys = k;
    sKeysCap = cap;
    return 1;
}

static void AddWarp(u32 frame, u8 player, u16 room, u8 x, u8 y)
{
    if (sNumWarps == sWarpsCap) {
        u32 cap = sWarpsCap ? sWarpsCap * 2 : 16;
        struct Warp *w = (struct Warp *)realloc(sWarps, cap * sizeof *w);

        if (w == NULL)
            return;
        sWarps = w;
        sWarpsCap = cap;
    }
    sWarps[sNumWarps].frame = frame;
    sWarps[sNumWarps].player = player;
    sWarps[sNumWarps].room = room;
    sWarps[sNumWarps].x = x;
    sWarps[sNumWarps].y = y;
    sNumWarps++;
}

/* --- the file ------------------------------------------------------------- */

long PortInputLogLoad(const char *text, u32 len)
{
    const char *p = text, *end = text + len;
    u32 frames = 0, last = 0;
    u16 mask = 0;
    int sawHeader = 0;

    sKeysLen = 0;
    sNumWarps = 0;

    while (p < end) {
        char line[160];
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t n = (size_t)((nl ? nl : end) - p);
        unsigned a, b, c, d, e, f, g;
        int ia, ib;

        if (n >= sizeof line)
            n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        p = nl ? nl + 1 : end;

        if (line[0] == '#' || line[0] == '\0' || line[0] == '\r')
            continue;
        if (!sawHeader) {
            if (sscanf(line, "katam-input %u", &a) != 1 || a != 1) {
                PortError("[katam-port] replay: not a katam-input 1 file");
                return -1;
            }
            sawHeader = 1;
        } else if (sscanf(line, "frames %u", &a) == 1) {
            frames = a;
        } else if (sscanf(line, "k %u %x", &a, &b) == 2) {
            if (!GrowKeys(a + 1))
                return -1;
            while (last < a)
                sKeys[last++] = mask;
            mask = (u16)(b & 0x03FF);
            sKeys[last++] = mask;
        } else if (sscanf(line, "warp %u %u %u %u %u", &a, &b, &c, &d, &e) == 5) {
            AddWarp(a, (u8)b, (u16)c, (u8)d, (u8)e);
        } else if (sscanf(line, "option rando %u %i", &a, &ia) == 2) {
            sHaveRando = 1;
            sRandoSeed = a;
            sRandoFlags = (u32)ia;
        } else if (sscanf(line, "option view %u %d %d %u %u %u", &a, &ia, &ib,
                          &d, &f, &g) == 6) {
            sHaveView = 1;
            sViewMode = a; sViewPadX = ia; sViewPadY = ib;
            sViewCull = d; sViewPin = f; sViewTile = g;
        } else if (strncmp(line, "end", 3) == 0) {
            break;
        } else {
            PortError("[katam-port] replay: cannot read line: %s", line);
            return -1;
        }
        (void)c;
    }
    if (!sawHeader)
        return -1;
    if (frames == 0)
        frames = last;
    if (!GrowKeys(frames + 1))
        return -1;
    while (last < frames)
        sKeys[last++] = mask;
    sKeysLen = frames;
    sReplaying = 1;

    if (sHaveRando)
        PortSetRando(sRandoFlags, sRandoSeed);
    if (sHaveView)
        PortSetViewMode(sViewMode, sViewPadX, sViewPadY, sViewCull, sViewPin,
                        sViewTile);
    PortLog("[katam-port] replay: %u frames, %u warps", (unsigned)sKeysLen,
            (unsigned)sNumWarps);
    return (long)sKeysLen;
}

u32 PortInputLogFrames(void)
{
    return sReplaying ? sKeysLen : 0;
}

int PortInputLogReplaying(void)
{
    return sReplaying;
}

void PortInputLogRecordStart(void)
{
    sRecording = 1;
}

void PortInputLogNoteFrames(u32 frames)
{
    sRunFrames = frames;
}

void PortInputLogNoteRando(u32 flags, u32 seed)
{
    sHaveRando = 1;
    sRandoFlags = flags;
    sRandoSeed = seed;
}

void PortInputLogNoteView(u32 mode, s32 padX, s32 padY, u32 cull, u32 pinHud,
                          u32 tilemap)
{
    sHaveView = 1;
    sViewMode = mode; sViewPadX = padX; sViewPadY = padY;
    sViewCull = cull; sViewPin = pinHud; sViewTile = tilemap;
}

char *PortInputLogSerialize(u32 *outLen)
{
    size_t cap = 256 + (size_t)sKeysLen * 4 + (size_t)sNumWarps * 48, len = 0;
    char *s = (char *)malloc(cap);
    u32 f, w = 0;
    int have = 0;
    u16 prev = 0;

    if (s == NULL)
        return NULL;
#define EMIT(...) (len += (size_t)snprintf(s + len, cap - len, __VA_ARGS__))
    EMIT("katam-input 1\n");
    EMIT("# recorded by katam-port; inputs only, no game data\n");
    EMIT("frames %u\n", (unsigned)(sRunFrames > sKeysLen ? sRunFrames : sKeysLen));
    if (sHaveRando)
        EMIT("option rando %u 0x%X\n", (unsigned)sRandoSeed, (unsigned)sRandoFlags);
    if (sHaveView)
        EMIT("option view %u %d %d %u %u %u\n", (unsigned)sViewMode,
             (int)sViewPadX, (int)sViewPadY, (unsigned)sViewCull,
             (unsigned)sViewPin, (unsigned)sViewTile);
    for (f = 0; f < sKeysLen; f++) {
        while (w < sNumWarps && sWarps[w].frame <= f) {
            if (cap - len < 64) break;
            EMIT("warp %u %u %u %u %u\n", (unsigned)sWarps[w].frame,
                 (unsigned)sWarps[w].player, (unsigned)sWarps[w].room,
                 (unsigned)sWarps[w].x, (unsigned)sWarps[w].y);
            w++;
        }
        if (!have || sKeys[f] != prev) {
            if (cap - len < 32) {
                char *t;
                cap *= 2;
                t = (char *)realloc(s, cap);
                if (t == NULL) { free(s); return NULL; }
                s = t;
            }
            EMIT("k %u %03X\n", (unsigned)f, (unsigned)sKeys[f]);
            prev = sKeys[f];
            have = 1;
        }
    }
    while (w < sNumWarps) {
        if (cap - len < 64) break;
        EMIT("warp %u %u %u %u %u\n", (unsigned)sWarps[w].frame,
             (unsigned)sWarps[w].player, (unsigned)sWarps[w].room,
             (unsigned)sWarps[w].x, (unsigned)sWarps[w].y);
        w++;
    }
    EMIT("end\n");
#undef EMIT
    if (outLen)
        *outLen = (u32)len;
    return s;
}

void PortInputLogScheduleWarp(u32 frame, u8 player, u16 room, u8 cellX, u8 cellY)
{
    if (sReplaying)
        return;
    AddWarp(frame, player, room, cellX, cellY);
}

/* --- the state hash --------------------------------------------------------
 *
 * What counts as simulation state is the rollback snapshot's set
 * (platform/rollback.c, kRegions plus the port's out-of-map state), split in
 * two and with the host-only parts taken out:
 *
 *   sim   EWRAM, IWRAM, the I/O block with the DMA address registers zeroed
 *         (platform/dma.c mirrors the *host* source address into them, which
 *         for DmaFill is a stack address), the DMA channel table with its
 *         pointers made comparable, and the frame state (frame number, the
 *         VBlank DMA budget, the keys the next frame will latch).
 *   vid   palette, VRAM and OAM -- written by the game, read by the PPU --
 *         and the PPU's affine reference latches.  Separate because a
 *         renderer change is allowed to move the second half and nothing
 *         else, and because the game never reads the picture back.
 *   pix   (PORT_HASH_PIX, filled at the latch) the framebuffer the frame
 *         drew -- the column the "pixels" regression mode compares.
 *   sram  the save.  Not in a rollback snapshot (a rollback must not
 *         un-write a save), but it is game-written and decides what the next
 *         boot does, so it gets its own column.
 *
 * Not hashed, because it is the host's and not the game's: the framebuffer,
 * the audio mixer's own buffers, the diagnostics counters, the view and
 * rando *settings* (which are recorded as inputs instead).
 *
 * FNV-1a over little-endian 32-bit words; every region is a multiple of 4.
 * EWRAM and IWRAM go through PortHostCanonWord first, so a stored function
 * pointer hashes by the function's name rather than by where the linker put
 * it this time (platform/native/symtab.c). */
static u32 HashWords(u32 h, const void *base, u32 bytes)
{
    const u8 *p = (const u8 *)base;
    u32 i;

    for (i = 0; i < bytes; i += 4) {
        u32 w = (u32)p[i] | ((u32)p[i + 1] << 8) | ((u32)p[i + 2] << 16)
              | ((u32)p[i + 3] << 24);
        h = (h ^ w) * 16777619u;
    }
    return h;
}

/* Memory that never holds a pointer, so a word there is never canonicalised:
 * the two shadow-OAM buffers.  OAM attribute pairs are 32-bit words with
 * arbitrary bits, and one that happened to equal a function's entry point in
 * one build (0x10190050 = attr0 0x0050, attr1 0x1019, sub_0800E0AC in one
 * layout and mid-function in the next) was canonicalised there and not here
 * -- a sim "divergence" between two builds whose game state was identical. */
static const struct { u32 lo, hi; } kNoPointers[] = {
    { 0x030031C0, 0x030031C0 + 0x400 },    /* gUnk_030031C0, OamData[128] */
    { 0x030060B0, 0x030060B0 + 0x300 },    /* gOamBuffer */
};

static u32 CanonAt(u32 addr, u32 w)
{
    unsigned k;

    for (k = 0; k < sizeof kNoPointers / sizeof kNoPointers[0]; k++)
        if (addr >= kNoPointers[k].lo && addr < kNoPointers[k].hi)
            return w;
    return PortHostCanonWord(w);
}

/* The same over memory the game stores function pointers in, at GBA address
 * gba. */
static u32 HashWordsCanon(u32 h, u32 gba, u32 bytes)
{
    const u8 *p = (const u8 *)(uintptr_t)gba;
    u32 i;

    for (i = 0; i < bytes; i += 4) {
        u32 w = (u32)p[i] | ((u32)p[i + 1] << 8) | ((u32)p[i + 2] << 16)
              | ((u32)p[i + 3] << 24);
        h = (h ^ CanonAt(gba + i, w)) * 16777619u;
    }
    return h;
}

#define FNV_INIT 2166136261u

/* I/O with the four channels' SAD/DAD words zeroed: 0xB0..0xDF, three words
 * per channel, the first two of which are addresses. */
static void CanonIo(u8 io[GBA_IO_SIZE])
{
    int ch;

    memcpy(io, (const void *)(uintptr_t)GBA_IO_BASE, GBA_IO_SIZE);
    for (ch = 0; ch < 4; ch++)
        memset(io + 0xB0 + ch * 12, 0, 8);
}

static void FrameStateCanon(u8 out[12])
{
    u8 buf[64];

    /* Field by field: the struct has padding, and the padding of a local is
     * whatever was on the stack. */
    memset(out, 0, 12);
    if (PortFrameStateSize() <= sizeof buf) {
        PortFrameStateSave(buf);
        memcpy(out, buf, 10);       /* u32 frameCount, s32 budget, u16 keys */
    }
}

void PortStateHashes(u32 out[PORT_HASH_COUNT])
{
    static u8 io[GBA_IO_SIZE];
    u32 dma[PORT_DMA_CANON_WORDS];
    u8 fs[12], ppu[64];
    u32 h;

    CanonIo(io);
    PortDmaStateCanon(dma);
    FrameStateCanon(fs);

    h = FNV_INIT;
    h = HashWordsCanon(h, GBA_EWRAM_BASE, GBA_EWRAM_SIZE);
    h = HashWordsCanon(h, GBA_IWRAM_BASE, GBA_IWRAM_SIZE);
    h = HashWords(h, io, GBA_IO_SIZE);
    {
        u8 le[PORT_DMA_CANON_WORDS * 4];
        int i;

        for (i = 0; i < PORT_DMA_CANON_WORDS; i++) {
            le[i * 4] = (u8)dma[i]; le[i * 4 + 1] = (u8)(dma[i] >> 8);
            le[i * 4 + 2] = (u8)(dma[i] >> 16); le[i * 4 + 3] = (u8)(dma[i] >> 24);
        }
        h = HashWords(h, le, sizeof le);
    }
    h = HashWords(h, fs, sizeof fs);
    out[PORT_HASH_SIM] = h;

    h = FNV_INIT;
    h = HashWords(h, (const void *)(uintptr_t)GBA_PLTT_BASE, GBA_PLTT_SIZE);
    h = HashWords(h, (const void *)(uintptr_t)GBA_VRAM_BASE, GBA_VRAM_SIZE);
    h = HashWords(h, (const void *)(uintptr_t)GBA_OAM_BASE, GBA_OAM_SIZE);
    memset(ppu, 0, sizeof ppu);
    if (PortPpuStateSize() <= sizeof ppu) {
        PortPpuStateSave(ppu);
        h = HashWords(h, ppu, (PortPpuStateSize() + 3u) & ~3u);
    }
    out[PORT_HASH_VID] = h;

    out[PORT_HASH_SRAM] = HashWords(FNV_INIT, (const void *)(uintptr_t)GBA_SRAM_BASE,
                                    GBA_SRAM_SIZE);
}

/* The bytes behind the sim and vid hashes, for a checkpoint dump: what a
 * cross-build comparison diffs once the hashes say two runs differ.  Layout
 * (all little-endian): EWRAM, IWRAM, IO (canonical), DMA canon, frame state
 * (12 bytes), PLTT, VRAM, OAM, PPU latches (64 bytes, zero-padded), SRAM. */
static u32 DumpBytes(void)
{
    return GBA_EWRAM_SIZE + GBA_IWRAM_SIZE + GBA_IO_SIZE
         + PORT_DMA_CANON_WORDS * 4 + 12
         + GBA_PLTT_SIZE + GBA_VRAM_SIZE + GBA_OAM_SIZE + 64 + GBA_SRAM_SIZE;
}

static void DumpTo(u8 *d)
{
    u32 dma[PORT_DMA_CANON_WORDS];
    int i;

#define PUT(src, n) (memcpy(d, (const void *)(src), (n)), d += (n))
    /* EWRAM and IWRAM as hashed: through PortHostCanonWord, so the dump says
     * exactly what the hash saw. */
    {
        static const struct { uintptr_t base; u32 size; } r[2] = {
            { GBA_EWRAM_BASE, GBA_EWRAM_SIZE }, { GBA_IWRAM_BASE, GBA_IWRAM_SIZE } };
        int k;
        u32 j;

        for (k = 0; k < 2; k++)
            for (j = 0; j < r[k].size; j += 4) {
                const u8 *s = (const u8 *)(r[k].base + j);
                u32 w = CanonAt((u32)(r[k].base + j),
                                (u32)s[0] | ((u32)s[1] << 8)
                                | ((u32)s[2] << 16) | ((u32)s[3] << 24));
                d[0] = (u8)w; d[1] = (u8)(w >> 8); d[2] = (u8)(w >> 16);
                d[3] = (u8)(w >> 24);
                d += 4;
            }
    }
    CanonIo(d); d += GBA_IO_SIZE;
    PortDmaStateCanon(dma);
    for (i = 0; i < PORT_DMA_CANON_WORDS; i++) {
        d[0] = (u8)dma[i]; d[1] = (u8)(dma[i] >> 8);
        d[2] = (u8)(dma[i] >> 16); d[3] = (u8)(dma[i] >> 24);
        d += 4;
    }
    FrameStateCanon(d); d += 12;
    PUT((uintptr_t)GBA_PLTT_BASE, GBA_PLTT_SIZE);
    PUT((uintptr_t)GBA_VRAM_BASE, GBA_VRAM_SIZE);
    PUT((uintptr_t)GBA_OAM_BASE, GBA_OAM_SIZE);
    memset(d, 0, 64);
    if (PortPpuStateSize() <= 64)
        PortPpuStateSave(d);
    d += 64;
    PUT((uintptr_t)GBA_SRAM_BASE, GBA_SRAM_SIZE);
#undef PUT
}

static int sHashLog;
static u32 sHashes[PORT_HASH_COUNT];
static u32 sHashFrame;
static int sHashPending;

#define MAX_DUMPS 32
static u32 sDumpAt[MAX_DUMPS];
static int sNumDumps;
static u8 *sDump;
static u32 sDumpLen;

void PortSetFrameStateLog(int on)
{
    sHashLog = on;
}

void PortSetStateDumpAt(u32 frame)
{
    if (sNumDumps < MAX_DUMPS)
        sDumpAt[sNumDumps++] = frame;
    sHashLog = 1;
}

/* Top of PortPresentFrame: the state the previous frame's logic left. */
void PortInputLogFrameStart(u32 frame)
{
    int i;

    sDumpLen = 0;
    if (!sHashLog)
        return;
    PortStateHashes(sHashes);
    sHashes[PORT_HASH_PIX] = 0;
    sHashFrame = frame;
    sHashPending = 1;
    for (i = 0; i < sNumDumps; i++) {
        if (sDumpAt[i] != frame)
            continue;
        if (sDump == NULL)
            sDump = (u8 *)malloc(DumpBytes());
        if (sDump != NULL) {
            DumpTo(sDump);
            sDumpLen = DumpBytes();
        }
    }
}

/* The latch (platform/main.c, UpdateKeyInput), once a frame, after the
 * rollback and seat overrides.  `frame` is the frame these keys are for. */
void PortInputLogLatch(u32 frame, u16 *keys)
{
    u32 i;

    if (sReplaying) {
        if (frame < sKeysLen)
            *keys = sKeys[frame];
        else
            *keys = 0;
    }
    if (sRecording && GrowKeys(frame + 1)) {
        sKeys[frame] = *keys;
        if (frame + 1 > sKeysLen)
            sKeysLen = frame + 1;
    }

    /* Warps, from the recording or from the host -- either way they are in
     * the list, and either way they go to the game here. */
    for (i = 0; i < sNumWarps; i++)
        if (sWarps[i].frame == frame)
            PortWarp(sWarps[i].player, sWarps[i].room, sWarps[i].x, sWarps[i].y);

    if (sHashPending && sHashFrame == frame) {
        sHashPending = 0;
        /* The picture this frame drew (PortRenderFrame ran after the state
         * hash was taken, earlier in the same PortPresentFrame).  The one
         * column a renderer change is expected to move. */
        sHashes[PORT_HASH_PIX] = HashWords(FNV_INIT, gPortFramebuffer,
                                           (u32)(gPortViewW * gPortViewH) * 4u);
        PortHostFrameState(frame, *keys, sHashes, sDumpLen ? sDump : NULL,
                           sDumpLen);
    }
}

#ifdef __cplusplus
}
#endif
