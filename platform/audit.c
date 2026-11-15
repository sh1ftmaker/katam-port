/* Determinism audit: the raw whole image, hashed per part at every frame
 * start, and dumped raw at chosen frames.  Diagnostics only; off unless asked.
 *
 * The question it answers (tools/netaudit/): two devices fed the same inputs
 * -- does every byte a rollback snapshot restores agree, with no masks?  So,
 * unlike the regression hash (platform/inputlog.c), nothing is canonicalised
 * or left out: the GBA regions of platform/rollback.c's kRegions as they
 * stand, the port sim block's used bytes, and, as separate columns that are
 * NOT part of the image, the save memory, the I/O block with the DMA address
 * mirrors zeroed (to tell a host-address difference from a real one), and the
 * out-of-map host state a snapshot also saves (DMA channel table, PPU latches).
 *
 * Taken at the top of PortPresentFrame (from PortInputLogFrameStart): the
 * point rollback snapshots and the regression hash is taken.
 *
 * Natively: PORT_AUDIT_LOG=FILE (one line per frame), PORT_AUDIT_DUMP=F:PATH[,F:PATH].
 * On the web/node (getenv cannot see process.env there): PortAuditEnable(1),
 * PortAuditDumpAt(F), and read PortAuditHashes / PortAuditDumpPtr /
 * PortAuditDumpLen from the frame-state callback (tools/netaudit/run_node.js).
 *
 * Dump format (little-endian): "KAUD", u32 version 1, u32 frame, u32 nparts,
 * then nparts x { char name[8], u32 base, u32 size }, then the parts' bytes in
 * that order. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/port.h"
#include "port/backend.h"
#include "port/simblock.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define AUDIT_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define AUDIT_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A_IMAGE, A_EWRAM, A_IWRAM, A_IO, A_PLTT, A_VRAM, A_OAM, A_SIM,
    A_SRAM, A_IOCANON, A_DMAHOST, A_PPUHOST, A_COUNT
};

static int sOn = -1;
static u32 sHash[A_COUNT];
static u32 sHashFrame;
static FILE *sLog;
#define MAX_AUDIT_DUMPS 64
static u32 sDumpAt[MAX_AUDIT_DUMPS];
static char *sDumpPath[MAX_AUDIT_DUMPS];
static int sNumDumps;
static u8 *sDump;
static u32 sDumpLen, sDumpCap;
static u32 sDumpFrame;

static u32 Fnv(u32 h, const void *base, u32 n)
{
    const u8 *p = (const u8 *)base;
    u32 i;

    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

#define FNV0 2166136261u

static void ReadEnv(void)
{
#ifndef __EMSCRIPTEN__
    const char *l = getenv("PORT_AUDIT_LOG");
    const char *d = getenv("PORT_AUDIT_DUMP");

    if (l && *l) {
        sLog = fopen(l, "w");
        if (sLog)
            fprintf(sLog, "# frame image ewram iwram io pltt vram oam sim | sram iocanon dmahost ppuhost\n");
    }
    if (d && *d) {
        char *s = strdup(d), *tok, *save = NULL;

        for (tok = strtok_r(s, ",", &save); tok && sNumDumps < MAX_AUDIT_DUMPS;
             tok = strtok_r(NULL, ",", &save)) {
            char *c = strchr(tok, ':');

            if (!c)
                continue;
            *c = '\0';
            sDumpAt[sNumDumps] = (u32)strtoul(tok, NULL, 10);
            sDumpPath[sNumDumps] = strdup(c + 1);
            sNumDumps++;
        }
        free(s);
    }
    sOn = (sLog != NULL || sNumDumps > 0);
#else
    sOn = 0;
#endif
}

AUDIT_EXPORT void PortAuditEnable(int on)
{
    if (sOn < 0)
        ReadEnv();
    if (on)
        sOn = 1;
}

AUDIT_EXPORT void PortAuditDumpAt(u32 frame)
{
    if (sNumDumps < MAX_AUDIT_DUMPS) {
        sDumpAt[sNumDumps] = frame;
        sDumpPath[sNumDumps] = NULL;
        sNumDumps++;
    }
}

struct Part { const char *name; uintptr_t base; u32 size; };

static int Parts(struct Part *p)
{
    int n = 0;

    p[n].name = "ewram"; p[n].base = GBA_EWRAM_BASE; p[n++].size = GBA_EWRAM_SIZE;
    p[n].name = "iwram"; p[n].base = GBA_IWRAM_BASE; p[n++].size = GBA_IWRAM_SIZE;
    p[n].name = "io";    p[n].base = GBA_IO_BASE;    p[n++].size = GBA_IO_SIZE;
    p[n].name = "pltt";  p[n].base = GBA_PLTT_BASE;  p[n++].size = GBA_PLTT_SIZE;
    p[n].name = "vram";  p[n].base = GBA_VRAM_BASE;  p[n++].size = GBA_VRAM_SIZE;
    p[n].name = "oam";   p[n].base = GBA_OAM_BASE;   p[n++].size = GBA_OAM_SIZE;
    p[n].name = "sim";   p[n].base = PORT_SIM_BASE;  p[n++].size = PortSimUsedBytes();
    p[n].name = "sram";  p[n].base = GBA_SRAM_BASE;  p[n++].size = GBA_SRAM_SIZE;
    p[n].name = "dmahost"; p[n].base = 0;            p[n++].size = PortDmaStateSize();
    p[n].name = "ppuhost"; p[n].base = 0;            p[n++].size = PortPpuStateSize();
    return n;
}

static void Put32(u8 *d, u32 v)
{
    d[0] = (u8)v; d[1] = (u8)(v >> 8); d[2] = (u8)(v >> 16); d[3] = (u8)(v >> 24);
}

static void Dump(u32 frame)
{
    struct Part p[12];
    int n = Parts(p), i;
    u32 len = 16 + (u32)n * 16, off;

    for (i = 0; i < n; i++)
        len += p[i].size;
    if (len > sDumpCap) {
        free(sDump);
        sDump = (u8 *)malloc(len);
        sDumpCap = sDump ? len : 0;
        if (!sDump)
            return;
    }
    memcpy(sDump, "KAUD", 4);
    Put32(sDump + 4, 1);
    Put32(sDump + 8, frame);
    Put32(sDump + 12, (u32)n);
    off = 16;
    for (i = 0; i < n; i++) {
        memset(sDump + off, 0, 8);
        memcpy(sDump + off, p[i].name, strlen(p[i].name) < 8 ? strlen(p[i].name) : 8);
        Put32(sDump + off + 8, (u32)p[i].base);
        Put32(sDump + off + 12, p[i].size);
        off += 16;
    }
    for (i = 0; i < n; i++) {
        if (!strcmp(p[i].name, "dmahost"))
            PortDmaStateSave(sDump + off);
        else if (!strcmp(p[i].name, "ppuhost"))
            PortPpuStateSave(sDump + off);
        else
            memcpy(sDump + off, (const void *)p[i].base, p[i].size);
        off += p[i].size;
    }
    sDumpLen = len;
    sDumpFrame = frame;
}

/* A join: at the top of frame `frame`, before hashing, overwrite the parts of
 * a KAUD dump named by `mask` (bit i = the dump's part i: 0 ewram, 1 iwram,
 * 2 io, 3 pltt, 4 vram, 5 oam, 6 sim, 7 sram, 8 dmahost, 9 ppuhost) -- what a
 * device that receives only the host's image would have. */
static u8 *sLoad;
static u32 sLoadLen, sLoadFrame, sLoadMask;

AUDIT_EXPORT int PortAuditLoadAt(u32 frame, const u8 *src, u32 len, u32 mask)
{
    free(sLoad);
    sLoad = (u8 *)malloc(len);
    if (!sLoad)
        return 0;
    memcpy(sLoad, src, len);
    sLoadLen = len;
    sLoadFrame = frame;
    sLoadMask = mask;
    return 1;
}

static u32 Get32(const u8 *d)
{
    return (u32)d[0] | ((u32)d[1] << 8) | ((u32)d[2] << 16) | ((u32)d[3] << 24);
}

static void LoadNow(void)
{
    u32 n = Get32(sLoad + 12), off = 16 + n * 16, i;

    if (memcmp(sLoad, "KAUD", 4) != 0)
        return;
    for (i = 0; i < n; i++) {
        const u8 *h = sLoad + 16 + i * 16;
        u32 base = Get32(h + 8), size = Get32(h + 12);

        if (off + size > sLoadLen)
            break;
        if (sLoadMask & (1u << i)) {
            if (!strncmp((const char *)h, "dmahost", 8) && size == PortDmaStateSize())
                PortDmaStateLoad(sLoad + off);
            else if (!strncmp((const char *)h, "ppuhost", 8) && size == PortPpuStateSize())
                PortPpuStateLoad(sLoad + off);
            else if (base)
                memcpy((void *)(uintptr_t)base, sLoad + off, size);
        }
        off += size;
    }
    PortLog("[audit] frame %u: image loaded (mask 0x%X)", (unsigned)sLoadFrame, (unsigned)sLoadMask);
}

/* Top of PortPresentFrame, from PortInputLogFrameStart. */
void PortAuditFrameStart(u32 frame)
{
    static u8 io[GBA_IO_SIZE];
    u8 host[1024];
    u32 h;
    int ch, i;

    if (sOn < 0)
        ReadEnv();
    if (sLoad && frame == sLoadFrame) {
        LoadNow();
        free(sLoad);
        sLoad = NULL;
    }
    if (!sOn)
        return;

    sHash[A_EWRAM] = Fnv(FNV0, (const void *)(uintptr_t)GBA_EWRAM_BASE, GBA_EWRAM_SIZE);
    sHash[A_IWRAM] = Fnv(FNV0, (const void *)(uintptr_t)GBA_IWRAM_BASE, GBA_IWRAM_SIZE);
    sHash[A_IO]    = Fnv(FNV0, (const void *)(uintptr_t)GBA_IO_BASE, GBA_IO_SIZE);
    sHash[A_PLTT]  = Fnv(FNV0, (const void *)(uintptr_t)GBA_PLTT_BASE, GBA_PLTT_SIZE);
    sHash[A_VRAM]  = Fnv(FNV0, (const void *)(uintptr_t)GBA_VRAM_BASE, GBA_VRAM_SIZE);
    sHash[A_OAM]   = Fnv(FNV0, (const void *)(uintptr_t)GBA_OAM_BASE, GBA_OAM_SIZE);
    sHash[A_SIM]   = Fnv(FNV0, (const void *)(uintptr_t)PORT_SIM_BASE, PortSimUsedBytes());
    h = FNV0;
    for (i = A_EWRAM; i <= A_SIM; i++) {
        u8 le[4];

        Put32(le, sHash[i]);
        h = Fnv(h, le, 4);
    }
    sHash[A_IMAGE] = h;
    sHash[A_SRAM] = Fnv(FNV0, (const void *)(uintptr_t)GBA_SRAM_BASE, GBA_SRAM_SIZE);
    memcpy(io, (const void *)(uintptr_t)GBA_IO_BASE, GBA_IO_SIZE);
    for (ch = 0; ch < 4; ch++)
        memset(io + 0xB0 + ch * 12, 0, 8);
    sHash[A_IOCANON] = Fnv(FNV0, io, GBA_IO_SIZE);
    sHash[A_DMAHOST] = 0;
    if (PortDmaStateSize() <= sizeof host) {
        PortDmaStateSave(host);
        sHash[A_DMAHOST] = Fnv(FNV0, host, PortDmaStateSize());
    }
    sHash[A_PPUHOST] = 0;
    if (PortPpuStateSize() <= sizeof host) {
        PortPpuStateSave(host);
        sHash[A_PPUHOST] = Fnv(FNV0, host, PortPpuStateSize());
    }
    sHashFrame = frame;

    if (sLog) {
        fprintf(sLog, "%u", (unsigned)frame);
        for (i = 0; i < A_COUNT; i++)
            fprintf(sLog, " %08X", (unsigned)sHash[i]);
        fputc('\n', sLog);
        if ((frame & 255) == 0)
            fflush(sLog);
    }
    sDumpLen = 0;
    for (i = 0; i < sNumDumps; i++) {
        if (sDumpAt[i] != frame)
            continue;
        Dump(frame);
        if (sDumpPath[i] && sDumpLen) {
            FILE *f = fopen(sDumpPath[i], "wb");

            if (f) {
                fwrite(sDump, 1, sDumpLen, f);
                fclose(f);
            }
        }
        break;
    }
}

/* For the node driver: the hashes of the latest frame start, A_COUNT words. */
AUDIT_EXPORT u32 PortAuditHashes(u32 *out)
{
    memcpy(out, sHash, sizeof sHash);
    return sHashFrame;
}

AUDIT_EXPORT u32 PortAuditHashCount(void) { return A_COUNT; }
AUDIT_EXPORT const u8 *PortAuditDumpPtr(void) { return sDumpLen ? sDump : NULL; }
AUDIT_EXPORT u32 PortAuditDumpLen(void) { return sDumpLen; }
AUDIT_EXPORT u32 PortAuditDumpFrame(void) { return sDumpFrame; }

#ifndef __EMSCRIPTEN__
/* Flushed by the C runtime at exit; this makes sure a crash mid-run still
 * leaves the lines up to the last 256-frame boundary. */
#endif

#ifdef __cplusplus
}
#endif
