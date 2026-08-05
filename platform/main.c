/* Entry point and frame loop.
 *
 * On hardware, crt0.s sets up the stack, clears RAM, installs the interrupt
 * vector and jumps to AgbMain, which never returns -- GameLoop runs forever and
 * blocks in VBlankIntrWait once a frame.
 *
 * Neither host can be blocked that way.  A browser cannot be blocked at all,
 * so the web build is compiled with Asyncify: VBlankIntrWait unwinds the wasm
 * stack, hands control back to the page, and resumes on the next animation
 * frame.  Natively the same call is a real sleep to the next frame boundary.
 * Either way the game's own loop is left exactly as it is, and the whole of
 * the difference lives behind PortAwaitAnimationFrame -- see
 * platform/port/backend.h.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* For PortCallStack.  glibc and macOS have <execinfo.h>; MinGW does not, and
 * emscripten has its own. */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#elif defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#define PORT_HAVE_EXECINFO 1
#endif

#include "port/port.h"
#include "port/backend.h"
#include "port/dma.h"
#include "port/mp.h"
#include "gba/gba.h"
#include "main.h"
#include "multi_sio.h"

/* C linkage for the 64-bit builds.
 *
 * Those compile the game as C++ and tools/cxxify.py gives it C linkage, so
 * everything on the seam has to agree.  It is applied to platform/*.c as a
 * class rather than to the files that happened to break: the failure is an
 * undefined reference to a mangled name, or -- for a `const`, which is
 * internal-linkage in C++ and external in C -- to a symbol that is plainly
 * defined a few lines away.  Neither says which file to fix, and the set of
 * files that need it changes whenever a declaration moves.  A no-op in C.
 *
 * The block opens below the includes so that SDL and the system headers stay
 * outside it. */
#ifdef __cplusplus
extern "C" {
#endif

u32 gPortRomSize;
static u16 sKeysDown;          /* 1 = pressed, in GBA button-bit order */
static int sRomReady;
static u32 sFrameCount;

/* Roughly what a real VBlank fits: 68 scanlines of DMA to VRAM.  Large enough
 * that the mandatory OAM and palette flush never eats the whole window, small
 * enough that a backed-up queue takes several frames to drain, as on hardware. */
#define PORT_VBLANK_BUDGET_BYTES 24576
static s32 sVBlankBudget;

/* --- diagnostics --------------------------------------------------------- */

#define MAX_REPORTED 64
static const char *sReported[MAX_REPORTED];
static int sNumReported;
static int sMissingCalls;

/* PortConsole is the host's -- the browser console plus the page's own log, or
 * the process's stderr.  See platform/port/backend.h. */

void PortLog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    PortConsole(buf, 0);
}

/* Same, at error level.  Everything the port says arrives in the browser's
 * console via console.log, which means a genuine fault reads exactly like a
 * routine note and cannot be filtered for.  Things that are actually wrong go
 * through here so they show as errors, carry a stack in devtools, and survive
 * a "Errors only" filter -- which is what someone reporting a bug from a phone
 * will be looking at. */
void PortError(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    PortConsole(buf, 1);
}

static int ReportOnce(const char *name)
{
    int i;

    for (i = 0; i < sNumReported; i++)
        if (sReported[i] == name || strcmp(sReported[i], name) == 0)
            return 0;
    if (sNumReported < MAX_REPORTED)
        sReported[sNumReported++] = name;
    return 1;
}

void PortMissingFunction(const char *name)
{
    sMissingCalls++;
    if (ReportOnce(name))
        PortError("[katam-port] called %s(), which has no C body yet "
                "(still ARM assembly in the decomp)", name);
}

/* --- write watchpoint ---------------------------------------------------- */

u32 gPortWatchAddr;
static u32 sWatchHits;

void PortSetWatch(u32 addr)
{
    gPortWatchAddr = addr;
    sWatchHits = 0;
}

void PortWatchCheck(const char *who, uintptr_t dest, u32 bytes, uintptr_t src)
{
    if (gPortWatchAddr < dest || gPortWatchAddr >= dest + bytes)
        return;
    /* Frame number included because the same transfer runs every frame in
     * most of these paths; what matters is which one ran at the moment the
     * memory changed. */
    if (++sWatchHits <= 30)
        PortLog("[katam-port] WATCH 0x%08X written by %s: dest=0x%08X..0x%08X "
                "src=0x%08X (frame %u)",
                (unsigned)gPortWatchAddr, who, (unsigned)dest,
                (unsigned)(dest + bytes), (unsigned)src,
                (unsigned)sFrameCount);
}

/* --- state tracing --------------------------------------------------------
 *
 * For bugs that only appear somewhere a scripted run cannot reach -- the end
 * of a level, a boss, a specific door.  A state machine that stops advancing
 * gives nothing to work with after the fact: there is no trap, no log, and the
 * game keeps running perfectly.  What is needed is the value of the condition
 * it is waiting on, from the machine of whoever can actually get there.
 *
 * Each distinct (tag, a, b, c) is reported once, so a handler that runs every
 * frame prints one line per state it passes through and then goes quiet.  A
 * machine stuck in a loop therefore shows exactly one line, holding the
 * numbers that explain why. */

#define MAX_TRACED    256   /* distinct tuples remembered in total */
#define MAX_PER_TAG     4   /* lines any single site may ever print */

static struct { const char *tag; u32 a, b, c; } sTraced[MAX_TRACED];
static int sNumTraced;

void PortTrace(const char *tag, u32 a, u32 b, u32 c)
{
    int i, perTag = 0;

    for (i = 0; i < sNumTraced; i++) {
        if (sTraced[i].tag == tag) {
            /* Already said this exact thing -- say nothing. */
            if (sTraced[i].a == a && sTraced[i].b == b && sTraced[i].c == c)
                return;
            perTag++;
        }
    }

    /* A per-site cap as well as a global one.  Some of these sites report a
     * value that changes every frame; without a per-site limit one of them
     * would spend the whole budget in two seconds and drown out the rest.
     * Once a site is capped it stops recording too, so it cannot go on eating
     * slots -- perTag stays at the cap and every later call returns here. */
    if (perTag >= MAX_PER_TAG || sNumTraced >= MAX_TRACED)
        return;

    sTraced[sNumTraced].tag = tag;
    sTraced[sNumTraced].a = a;
    sTraced[sNumTraced].b = b;
    sTraced[sNumTraced].c = c;
    sNumTraced++;

    PortLog("[katam-port] trace %s: %u (0x%X), %u (0x%X), %u (0x%X)",
            tag, a, a, b, b, c, c);
    if (perTag + 1 == MAX_PER_TAG)
        PortLog("[katam-port] trace %s: capped, no further values", tag);
}

void PortUnimplemented(const char *what)
{
    if (ReportOnce(what))
        PortLog("[katam-port] unimplemented: %s", what);
}

void PortReportGaps(void)
{
    PortLog("[katam-port] %d distinct gaps reported, %d calls into missing "
            "functions", sNumReported, sMissingCalls);
}

/* --- memory -------------------------------------------------------------- */

void PortMemInit(void)
{
    /* The GBA regions are plain linear memory here, so "reset" is a memset.
     * crt0.s would have done this before AgbMain. */
    memset((void *)GBA_EWRAM_BASE, 0, GBA_EWRAM_SIZE);
    memset((void *)GBA_IWRAM_BASE, 0, GBA_IWRAM_SIZE);
    memset((void *)GBA_IO_BASE, 0, GBA_IO_SIZE);
    memset((void *)GBA_PLTT_BASE, 0, GBA_PLTT_SIZE);
    memset((void *)GBA_VRAM_BASE, 0, GBA_VRAM_SIZE);
    memset((void *)GBA_OAM_BASE, 0, GBA_OAM_SIZE);
    memset((void *)GBA_SRAM_BASE, 0, GBA_SRAM_SIZE);

    /* No buttons held at power-on; KEYINPUT is active-low. */
    *(vu16 *)(GBA_IO_BASE + REG_OFFSET_KEYINPUT) = 0x03FF;
}

void PortRomLoaded(u32 size)
{
    gPortRomSize = size;
    sRomReady = 1;
}

/* --- input ---------------------------------------------------------------
 * The shell sends a mask of held buttons; KEYINPUT wants the complement. */

void PortSetKeys(u16 downMask)
{
    sKeysDown = downMask & 0x03FF;
}

static void UpdateKeyInput(void)
{
    *(vu16 *)(GBA_IO_BASE + REG_OFFSET_KEYINPUT) = (~sKeysDown) & 0x03FF;
}

/* --- interrupts ----------------------------------------------------------
 * gIntrTable is the game's own handler table, in the order crt0's dispatcher
 * used: SIO, VBlank, HBlank, VCount, timers, DMA, keypad, gamepak. */

static const struct { u32 flag; int index; } sIntrIndex[] = {
    { INTR_FLAG_SERIAL, 0 },
    { INTR_FLAG_VBLANK, 1 },
    { INTR_FLAG_HBLANK, 2 },
    { INTR_FLAG_VCOUNT, 3 },
    { INTR_FLAG_TIMER0, 4 },
    { INTR_FLAG_TIMER1, 5 },
    { INTR_FLAG_TIMER2, 6 },
    { INTR_FLAG_TIMER3, 7 },
};

/* gIntrTable[0] does not always hold a function.
 *
 * MultiSioInit copies the machine code of MultiSioIntr into an IWRAM buffer so
 * that a cartridge waitstate cannot delay a serial interrupt, and the game
 * then installs the *buffer* as the handler -- gIntrTableTemplate[0] is
 * literally `(void *)gMultiSioIntrFuncBuf`, and multi_08019F28.c and
 * multi_sio_08158934.c write it into gIntrTable[0] again by hand.
 *
 * Here that address is data, not code, and a wasm function pointer is a table
 * index rather than an address, so calling it goes out of bounds.  It is the
 * same problem agb_sram.c had at boot, with the same answer: recognise the
 * buffer and call the real function.  platform/multi_sio_intr.c is the real
 * function. */
static void CallIntr(int index)
{
    IntrFunc handler = gIntrTable[index];

    if ((const void *)handler == (const void *)gMultiSioIntrFuncBuf) {
        static int said;

        /* Said once, because "did the link driver's interrupt actually run"
         * is otherwise unanswerable from outside: MultiSioIntr leaves its
         * traces in a struct the linker placed, not at a fixed address. */
        if (!said) {
            said = 1;
            PortLog("[katam-port] gIntrTable[0] holds gMultiSioIntrFuncBuf "
                    "(the IWRAM copy of the link driver's interrupt); "
                    "calling MultiSioIntr");
        }
        MultiSioIntr();
        return;
    }
    if (handler)
        handler();
}

void PortDispatchInterrupt(u32 flag)
{
    u32 i;

    /* Honour the game's own masking, exactly as the BIOS dispatcher would. */
    if (!(*(vu16 *)(GBA_IO_BASE + REG_OFFSET_IME) & 1))
        return;

    /* crt0.s's IntrMain tests INTR_FLAG_TIMER3 and INTR_FLAG_SERIAL together,
     * before every other flag, and sends both to gIntrTable[0] -- which makes
     * the per-timer entry for timer 3, further down the same chain,
     * unreachable on hardware.
     *
     * That is not an oddity to preserve for its own sake, it is how the link
     * driver works.  MultiSioMain's state 0 promotes a console to parent by
     * masking the serial interrupt off and the timer-3 interrupt on
     * (multi_sio.c), because the parent is clocked by its own timer rather
     * than by the cable, and it expects the same handler to keep running.
     * Dispatching timer 3 to gIntrTable[7] instead would call Timer3Intr,
     * which belongs to an unrelated protocol. */
    if (flag == INTR_FLAG_SERIAL || flag == INTR_FLAG_TIMER3) {
        u16 ie = *(vu16 *)(GBA_IO_BASE + REG_OFFSET_IE);

        if (!(ie & (INTR_FLAG_SERIAL | INTR_FLAG_TIMER3)))
            return;
        CallIntr(0);
        return;
    }

    if (!(*(vu16 *)(GBA_IO_BASE + REG_OFFSET_IE) & flag))
        return;

    for (i = 0; i < sizeof(sIntrIndex) / sizeof(sIntrIndex[0]); i++) {
        if (sIntrIndex[i].flag == flag) {
            CallIntr(sIntrIndex[i].index);
            return;
        }
    }
}

/* --- frame boundary ------------------------------------------------------ */

/* --- cross-build state tracing --------------------------------------------
 *
 * One line per frame: the input, and a hash of each region of the emulated
 * console.  Set PORT_STATE_TRACE=1 to turn it on; it costs nothing when off.
 *
 * The point is comparing two *builds* of the port against each other, which
 * this port can do and an emulator comparison normally cannot: every build
 * reserves the GBA map at the same true addresses, so EWRAM at 0x02000000 in
 * the wasm build and EWRAM at 0x02000000 in the aarch64 build hold the same
 * bytes for the same reason.  Hash them per frame, diff the two logs, and the
 * first differing line is the frame the two builds stopped agreeing -- and its
 * columns say whether the input diverged (a harness difference) or the state
 * did (a port bug), and which region.
 *
 * Comparing final screenshots cannot do this.  It says the pictures differ,
 * which is where the divergence *ended up* rather than where it began, and it
 * cannot distinguish "the two harnesses pressed different buttons" from "the
 * two builds computed different things".  That distinction is exactly what was
 * missing when the 64-bit play path could not be attributed.
 *
 * FNV-1a, chosen because it is eight lines and needs no library: the hash only
 * has to be identical across builds, not strong.
 */
static u32 TraceHash(const void *base, u32 len)
{
    const u8 *p = (const u8 *)base;
    u32 h = 2166136261u;
    u32 i;

    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static int sStateTrace = -1;
static long sStateDetailFrame = -1;
static long sDumpFrame = -1;
static unsigned long sDumpAddr;
static long sDumpLen;

/* The web build cannot read the environment: the Makefile links it
 * -sENVIRONMENT=web, so emscripten's getenv never sees node's process.env and
 * the trace stayed silent there while working natively.  An explicit entry
 * point costs one export and works on every host. */
void PortSetStateTrace(int on)
{
    sStateTrace = on ? 1 : 0;
}

static int sDmaTrace = -1;

int PortDmaTracing(void)
{
    if (sDmaTrace < 0) {
        const char *e = getenv("PORT_DMA_TRACE");
        sDmaTrace = (e != NULL && *e != '\0' && *e != '0');
    }
    return sDmaTrace;
}

void PortSetDmaTrace(int on)
{
    sDmaTrace = on ? 1 : 0;
}

u32 PortFrameNumber(void)
{
    return sFrameCount;
}

/* The call-stack instrument described in port/backend.h.  Window first, so
 * that the cost of taking a stack is paid only where it is wanted -- a
 * backtrace per transfer over 63000 transfers is minutes, not seconds. */
static long sDmaStackLo = -1;
static long sDmaStackHi;

void PortSetDmaStack(long lo, long hi)
{
    sDmaStackLo = lo;
    sDmaStackHi = hi;
    sDmaTrace = 1;
}

int PortDmaStackWanted(u32 transfer)
{
    if (sDmaStackLo < 0) {
        const char *e = getenv("PORT_DMA_STACK");

        sDmaStackLo = 0;
        sDmaStackHi = -1;
        if (e != NULL && *e != '\0') {
            char *end;

            sDmaStackLo = strtol(e, &end, 0);
            sDmaStackHi = (*end == ':') ? strtol(end + 1, NULL, 0) : sDmaStackLo;
        }
    }
    return sDmaStackHi >= 0
        && (long)transfer >= sDmaStackLo && (long)transfer <= sDmaStackHi;
}

void PortCallStack(const char *tag)
{
#ifdef __EMSCRIPTEN__
    /* EM_LOG_C_STACK asks for the wasm frames rather than the JS ones; the
     * names come from the name section, so this is the whole of the work on
     * this host.  Verified to survive an Asyncify rewind -- see
     * docs/DEBUG-TOOLING.md section 5. */
    char buf[8192];
    char *line;
    char *save;

    emscripten_get_callstack(EM_LOG_C_STACK, buf, sizeof buf);
    for (line = strtok_r(buf, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save))
        PortLog("[stack] %s %s", tag, line);
#elif defined(PORT_HAVE_EXECINFO)
    /* Return addresses, not names.  backtrace_symbols() would name them only
     * if the game's symbols were in the *dynamic* table, which needs -rdynamic
     * on the shipping link for the sake of a diagnostic.  The addresses are
     * absolute because the 64-bit builds link -no-pie at a fixed text address
     * (see CMakeLists.txt), so `nm` resolves them exactly, offline, from a
     * binary built without any special flag. */
    void *frames[32];
    int n = backtrace(frames, (int)(sizeof frames / sizeof frames[0]));
    int i;

    for (i = 0; i < n; i++)
        PortLog("[stack] %s %p", tag, frames[i]);
#else
    PortLog("[stack] %s <no unwinder on this host>", tag);
#endif
}

void PortSetStateDetailFrame(long frame)
{
    sStateDetailFrame = frame;
}

void PortSetStateDump(long frame, unsigned long addr, long len)
{
    sDumpFrame = frame;
    sDumpAddr = addr;
    sDumpLen = len;
}

/* One window, hashed on every frame, as an extra column on the [trace] line.
 *
 * PORT_STATE_DETAIL answers "which block differs" at a frame you already
 * suspect, and PORT_DUMP answers "which bytes" once you have the block.  The
 * question in between -- "*when* did this particular window start to differ" --
 * needed one run per candidate frame, which is the slow way to bisect several
 * hundred frames.
 *
 * The window is chosen by address rather than by symbol on purpose: by the
 * time this is wanted, the address is what you have.  PORT_WINDOW=addr:len,
 * hex address, decimal length. */
static unsigned long sWindowAddr;
static long sWindowLen = -1;

void PortSetStateWindow(unsigned long addr, long len)
{
    sWindowAddr = addr;
    sWindowLen = len;
}

static void PortStateTrace(void)
{
    static u32 frame;
    int enabled = sStateTrace;

    if (enabled < 0) {
        const char *e = getenv("PORT_STATE_TRACE");
        enabled = (e != NULL && *e != '\0' && *e != '0');
        sStateTrace = enabled;
        if (sStateDetailFrame < 0) {
            const char *d = getenv("PORT_STATE_DETAIL");
            if (d != NULL && *d != '\0')
                sStateDetailFrame = strtol(d, NULL, 0);
        }
        if (sDumpFrame < 0) {
            const char *w = getenv("PORT_DUMP");
            if (w != NULL && *w != '\0')
                sscanf(w, "%ld:%lx:%ld", &sDumpFrame, &sDumpAddr, &sDumpLen);
        }
        if (sWindowLen < 0) {
            const char *w = getenv("PORT_WINDOW");
            if (w != NULL && *w != '\0')
                sscanf(w, "%lx:%ld", &sWindowAddr, &sWindowLen);
        }
    }
    if (!enabled)
        return;

    /* A raw window, when the block hash has already narrowed things down and
     * the question is which *bytes*.  PORT_DUMP=frame:addr:len. */
    if (sDumpFrame >= 0 && frame == (u32)sDumpFrame && sDumpLen > 0) {
        const u8 *d = (const u8 *)(uintptr_t)sDumpAddr;
        u32 i;

        for (i = 0; i < (u32)sDumpLen; i += 16)
            PortLog("[dump] %08X %02X%02X%02X%02X %02X%02X%02X%02X "
                    "%02X%02X%02X%02X %02X%02X%02X%02X",
                    (unsigned)(sDumpAddr + i),
                    d[i+0], d[i+1], d[i+2], d[i+3], d[i+4], d[i+5], d[i+6], d[i+7],
                    d[i+8], d[i+9], d[i+10], d[i+11], d[i+12], d[i+13], d[i+14], d[i+15]);
    }

    /* Level 2 breaks EWRAM into 1 KiB blocks at one chosen frame.  The
     * whole-region hash says *that* two builds diverged; this says *where*,
     * which is the difference between a fact and a lead.  256 lines, once. */
    if (sStateDetailFrame >= 0 && frame == (u32)sStateDetailFrame) {
        u32 b;

        for (b = 0; b < GBA_EWRAM_SIZE / 1024u; b++)
            PortLog("[trace-ewram] f=%u block=%u addr=0x%08X hash=%08X",
                    frame, b, GBA_EWRAM_BASE + b * 1024u,
                    TraceHash((const void *)(uintptr_t)(GBA_EWRAM_BASE + b * 1024u),
                              1024u));

        /* IWRAM too, in 256-byte blocks.  The whole-region hash is useless
         * here because IWRAM holds host function pointers -- gIntrTable,
         * gMPlayJumpTable, and the callbacks inside gSoundInfo -- so it always
         * differs between builds and says nothing.  Per-block, the host
         * pointers are confined to a few blocks and every other block is
         * directly comparable, which is what was needed to see that the sound
         * engine's own state diverged before the tracks did. */
        for (b = 0; b < GBA_IWRAM_SIZE / 256u; b++)
            PortLog("[trace-iwram] f=%u block=%u addr=0x%08X hash=%08X",
                    frame, b, GBA_IWRAM_BASE + b * 256u,
                    TraceHash((const void *)(uintptr_t)(GBA_IWRAM_BASE + b * 256u),
                              256u));
    }

    /* The sound engine's driving state, alongside the memory hashes.  The
     * hashes say the tracks diverged; these say whether the mixer ran a
     * different number of times, or was handed a different position in the
     * PCM buffer, which is the actual input a host controls. */
    {
        extern u32 gPortSoundMainCalls;
        static u32 lastCalls;

        PortLog("[snd] f=%u mainCalls=%u delta=%u dmaCounter=%u period=%u "
                "perVBlank=%d",
                frame, gPortSoundMainCalls, gPortSoundMainCalls - lastCalls,
                (unsigned)gSoundInfo.pcmDmaCounter,
                (unsigned)gSoundInfo.pcmDmaPeriod,
                (int)gSoundInfo.pcmSamplesPerVBlank);
        lastCalls = gPortSoundMainCalls;
    }

    if (sWindowLen > 0)
        PortLog("[window] f=%u addr=0x%08X len=%ld hash=%08X",
                frame, (unsigned)sWindowAddr, sWindowLen,
                TraceHash((const void *)(uintptr_t)sWindowAddr,
                          (u32)sWindowLen));

    PortLog("[trace] f=%u keys=%03X ewram=%08X iwram=%08X vram=%08X "
            "pltt=%08X oam=%08X io=%08X",
            frame, sKeysDown,
            TraceHash((const void *)(uintptr_t)GBA_EWRAM_BASE, GBA_EWRAM_SIZE),
            TraceHash((const void *)(uintptr_t)GBA_IWRAM_BASE, GBA_IWRAM_SIZE),
            TraceHash((const void *)(uintptr_t)GBA_VRAM_BASE,  GBA_VRAM_SIZE),
            TraceHash((const void *)(uintptr_t)GBA_PLTT_BASE,  GBA_PLTT_SIZE),
            TraceHash((const void *)(uintptr_t)GBA_OAM_BASE,   GBA_OAM_SIZE),
            TraceHash((const void *)(uintptr_t)GBA_IO_BASE,    0x400u));
    frame++;
}

void PortPresentFrame(void)
{
    PortStateTrace();
    vu16 *dispstat = (vu16 *)(GBA_IO_BASE + REG_OFFSET_DISPSTAT);

    /* Draw the visible frame from whatever the game last wrote. */
    PortRenderFrame();

    /* Enter VBlank: run the flush DMAs, then the game's VBlank handler.
     *
     * The budget has to be restored *before* the window opens, not after the
     * handler runs.  PortVBlankEnd leaves it at zero, so opening VBlank with a
     * spent budget meant the first transfer inside the handler drove it
     * negative and closed the window again for the whole frame. */
    sVBlankBudget = PORT_VBLANK_BUDGET_BYTES;
    *(vu16 *)(GBA_IO_BASE + REG_OFFSET_VCOUNT) = 160;
    *dispstat |= DISPSTAT_VBLANK;
    PortDmaVBlank();
    PortDispatchInterrupt(INTR_FLAG_VBLANK);

    /* This frame's link-cable transfers.  On hardware they are spread across
     * the whole frame by timer 3; here they are a burst, because the I/O block
     * is plain memory and there is nothing to notice a store to SIOCNT at the
     * moment it happens.  What has to hold is that a frame's worth of them
     * happens between one MultiSioMain and the next, and it does.  Returns
     * immediately unless something has attached a transport. */
    PortMpFrame();

    PortBlitFramebuffer(gPortFramebuffer, PORT_SCREEN_W, PORT_SCREEN_H);
    PortAwaitAnimationFrame();

    UpdateKeyInput();
    sFrameCount++;

    /* Return with VBlank still in progress.  GameLoop does its screen flush
     * and queue draining now, and PortVBlankConsume ends VBlank once those
     * transfers have used up the window -- which is also what releases the
     * `while (REG_DISPSTAT & DISPSTAT_VBLANK);` spin at the end of the loop.
     * The window reopens here with whatever the handler above left unspent. */
}

void PortVBlankConsume(u32 bytes)
{
    vu16 *dispstat = (vu16 *)(GBA_IO_BASE + REG_OFFSET_DISPSTAT);

    if (!(*dispstat & DISPSTAT_VBLANK))
        return;
    sVBlankBudget -= (s32)bytes;
    if (sVBlankBudget <= 0)
        PortVBlankEnd();
}

void PortVBlankEnd(void)
{
    *(vu16 *)(GBA_IO_BASE + REG_OFFSET_DISPSTAT) &= ~DISPSTAT_VBLANK;
    *(vu16 *)(GBA_IO_BASE + REG_OFFSET_VCOUNT) = 0;
    sVBlankBudget = 0;
}

/* --- startup ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    /* Parses the command line, reserves the GBA memory map at its true
     * addresses and brings the window up -- or, on the web, does nothing at
     * all.  It has to precede PortMemInit, which memsets regions that on a
     * native host do not exist until they have been reserved. */
    PortHostInit(argc, argv);

    PortMemInit();

    /* The host puts the player's own ROM at 0x08000000 before we start; every
     * pointer the game follows into ROM depends on it being there. */
    PortAwaitRom();
    if (!sRomReady || gPortRomSize == 0) {
        PortLog("[katam-port] no ROM supplied; refusing to start");
        return 1;
    }
    PortLog("[katam-port] ROM loaded (%u bytes), starting AgbMain", gPortRomSize);
    PortLoadRomDataCopies();
    PortPatchRomFunctionPointers();

    UpdateKeyInput();
    AgbMain();      /* never returns */
    return 0;
}

#ifdef __cplusplus
}
#endif
