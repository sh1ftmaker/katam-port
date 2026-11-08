/* The native host: window, input, frame pacing, ROM loading, and the command
 * line.
 *
 * This is the native answer to platform/web/host_web.c.  Everything in it is
 * SDL2 and portable C -- the parts that are genuinely an operating system's
 * business are five functions in platform/native/native.h.
 *
 * Frame pacing
 * ------------
 * The web build takes one game frame per requestAnimationFrame, which is why
 * it runs at double speed on a 120 Hz display.  Nothing here is allowed to
 * inherit that.  The GBA's LCD runs at 59.7275 Hz -- 280896 cycles of a
 * 16.78 MHz clock -- and that, not the monitor, is what a frame is worth.  So
 * the pace comes from SDL_GetPerformanceCounter and the presentation is
 * deliberately *not* vsynced: a 60 Hz panel would drag the game 0.46% fast, a
 * 144 Hz panel would be a disaster, and the audio clock would fight it either
 * way.  --vsync is there for anyone who would rather have no tearing.
 *
 * The sleep is a coarse SDL_Delay down to the last millisecond and a spin
 * after that, because SDL_Delay's granularity is a scheduler tick and landing
 * 3 ms late every frame is audible before it is visible.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "native.h"
#include "port/audio.h"
#include "port/mp.h"
#include "port/inputlog.h"
#include "render/render_list.h"
#include "render/gl_renderer.h"
#include "render/gl_api.h"
#include "gba/gba.h"

/* C linkage for the 64-bit builds -- see tools/legacy-sync/cxxify.py.  Below the includes,
 * so SDL's headers stay outside the block. */
#ifdef __cplusplus
extern "C" {
#endif

/* 280896 cycles per frame at 16.777216 MHz.  Do not round this to 60. */
#define GBA_FRAME_HZ 59.7275

extern int gPortPpuCompose;     /* platform/ppu.c */

/* The SDL entry points only the GPU renderer uses are called through the GOT
 * rather than the PLT.  A new PLT entry sits ahead of all of the game's code
 * and moves every game function by sixteen bytes, and the regression hash is
 * sensitive to where game functions are (see the end of
 * platform/render/rl_build.c).  This keeps the game's code where it was. */
#if defined(__GNUC__) && defined(__ELF__) && defined(__x86_64__)
#define KATAM_NOPLT(fn) extern "C" __typeof__(fn) fn __attribute__((noplt));
#ifndef __cplusplus
#undef KATAM_NOPLT
#define KATAM_NOPLT(fn) extern __typeof__(fn) fn __attribute__((noplt));
#endif
KATAM_NOPLT(SDL_DestroyWindow)
KATAM_NOPLT(SDL_GL_CreateContext)
KATAM_NOPLT(SDL_GL_DeleteContext)
KATAM_NOPLT(SDL_GL_GetDrawableSize)
KATAM_NOPLT(SDL_GL_GetProcAddress)
KATAM_NOPLT(SDL_GL_MakeCurrent)
KATAM_NOPLT(SDL_GL_SetAttribute)
KATAM_NOPLT(SDL_GL_SetSwapInterval)
KATAM_NOPLT(SDL_GL_SwapWindow)
#endif

int gPortNativeVerbose;
int gPortNativeNoAudio;

static SDL_Window   *sWindow;
static SDL_Renderer *sRenderer;
static SDL_Texture  *sTexture;
static SDL_GameController *sPads[4];

static int  sScale = 3;
static int  sFullscreen;
static int  sVsync;
static int  sTurbo;
static long sFrameLimit = -1;       /* --frames N, -1 for "run forever" */
static const char *sShotPath;
static const char *sReadbackPath;
static const char *sRomArg;

/* Scripted input, the same shape tools/headless_test.js uses so a native run
 * can be compared against a web one: --hold keeps buttons down, --mash taps a
 * button with a period, because the game edge-triggers menu confirmation and a
 * held button registers once and then does nothing. */
static int sHoldMask;
static long sMashStart = -1;
static long sMashEnd = -1;      /* optional 4th --mash field: stop again */

/* --- scripted presses, and a link session, natively -----------------------
 *
 * tools/headless_test.js has had PRESS_AT and MP=loopback for a while and the
 * native host has not, which meant the only way to reach the game's own link
 * lobby was under node -- where there is no debugger.  Everything about the
 * multiplayer path was therefore diagnosable only by printf.
 *
 * These three options are the parity fix.  They exist so that a hardware
 * watchpoint can be put on a GBA address while the lobby runs, which is the
 * instrument that found the save-file contamination in one step and has no
 * equivalent in a browser.
 *
 *   --press F:MASK[,F:MASK...]   set the button mask at frame F
 *   --mp loopback[:N]            attach the in-process transport
 *   --mp-at F                    ... at frame F, so a run has a before and after
 */
#define MAX_PRESSES 32
static struct { long frame; int mask; } sPresses[MAX_PRESSES];
static int sNumPresses;
static int sPressMask;          /* whatever the last --press set             */

static const char *sMpKind;
static int sMpPlayers = 2;
static long sMpAt = -1;
static int sMpAttached;
static int  sMashMask, sMashPeriod;

/* --rando.  Applied in PortHostInit rather than at parse time so that it
 * lands after the platform layer exists and its report goes to the same
 * console as everything else. */
static unsigned sRandoSeed, sRandoFlags;

/* --view and --cull: the web page's view controls (platform/view.c,
 * docs/VIEW.md).  Off unless asked for -- PORT_VIEW_NATIVE is the 240x160
 * picture the hardware draws.  sTexW/H follow whatever size the core hands
 * PortBlitFramebuffer, which changes every frame under the zoom modes. */
static int sViewSet;
static unsigned sViewMode, sViewCull;
static int sViewPadX = 40, sViewPadY;
static int sTexW = PORT_SCREEN_W, sTexH = PORT_SCREEN_H;

static long sFrames;
static Uint64 sPerfFreq, sNextFrame;

/* --- the GPU renderer (platform/render, docs/RENDERER.md) -----------------
 *
 *   --renderer ppu|gpu|both  ppu is the emulated scanline PPU and the
 *                            default; gpu draws the render list with GL;
 *                            both runs the two and shows the GPU picture,
 *                            for comparison (--compare-log)
 *   --gpu-view WxH|auto[:H]  the world viewport in game pixels; auto takes
 *                            the window's aspect at height H (default 160)
 *   --gpu-shot PATH          PNG of the GPU picture at the last frame
 *   --shot-at F:PREFIX       PREFIX-ppu.png / PREFIX-gpu.png at frame F
 *   --compare-log FILE       with both: per frame, pixels where GPU != PPU */
#define RENDERER_PPU  0
#define RENDERER_GPU  1
#define RENDERER_BOTH 2
static int sRendererMode = RENDERER_PPU;
static SDL_GLContext sGl;
static int sGpuViewW = 240, sGpuViewH = 160, sGpuViewAuto;
static const char *sGpuShotPath, *sCompareLogPath;
static FILE *sCompareLog;
#define MAX_SHOTS 64
static struct { long frame; const char *prefix; } sShots[MAX_SHOTS];
static int sNumShots;
static u32 *sGpuPixels;
static long sCompareFrames, sCompareDiffFrames, sCompareDiffPixels;
static long sCompareSkipped;
static Uint64 sPresentInterval;   /* measured swap interval, perf ticks */
static long sPresents;

/* --- the determinism harness (platform/inputlog.c, docs/DETERMINISM.md) ---
 *
 *   --record FILE        write every latched key mask (and --warp) to FILE
 *   --replay FILE        drive the game from FILE instead of the keyboard;
 *                        runs exactly the frames it covers unless --frames
 *   --hash-log FILE      one line per frame: frame keys sim vid sram pix
 *   --dump-state F:PATH  the canonical state bytes at frame F, for a
 *                        cross-build diff (tools/state_diff.py)
 *   --warp F:ROOM[:X:Y]  put Kirby in ROOM at frame F (platform/warp.c);
 *                        recorded, so a replay warps on the same frame */
static const char *sRecordPath, *sReplayPath, *sHashLogPath;
static FILE *sHashLog;
#define MAX_DUMPS 16
static struct { long frame; const char *path; } sDumps[MAX_DUMPS];
static int sNumDumps;
#define MAX_WARPS 64
static struct { long frame; unsigned room, x, y; } sWarps[MAX_WARPS];
static int sNumWarps;

/* --- console -------------------------------------------------------------- */

void PortConsole(const char *text, int isErr)
{
    FILE *f = isErr ? stderr : stdout;

    fputs(text, f);
    fputc('\n', f);
    /* Line-buffered when it is a terminal, block-buffered when it is a log
     * file -- and the interesting runs are the ones that end in a crash, where
     * a block-buffered tail is the part that explains it. */
    fflush(f);
}

/* --- command line --------------------------------------------------------- */

static void Usage(const char *argv0)
{
    printf(
"katam-port -- Kirby & The Amazing Mirror, from the decompilation\n"
"\n"
"  %s [options] [rom.gba]\n"
"\n"
"This program contains no game data.  Supply your own ROM: as the argument\n"
"above, by dropping the file onto the window, or through the file picker it\n"
"offers when you start it with no argument.\n"
"\n"
"  --scale N          window is N times 240x160 (default 3)\n"
"  --fullscreen       start fullscreen; F11 toggles, 1-6 set the scale\n"
"  --vsync            present on the display's refresh as well as pacing to\n"
"                     the GBA's 59.7275 Hz.  Off by default: the two clocks\n"
"                     disagree, and the game's is the one that matters.\n"
"  --no-audio         open no audio device\n"
"  --frames N         run N frames and exit (for testing)\n"
"  --screenshot PATH  write a PNG of the last frame; F12 writes one any time\n"
"  --window-shot PATH write a PNG of the scaled window, read back from the\n"
"                     renderer -- what --screenshot cannot tell you is whether\n"
"                     the picture reached the screen in the right pixel format\n"
"  --view MODE[:PX[:PY]]  native (default), wide, zoom or widezoom;\n"
"                     PX/PY pad each side in pixels (default 40:0, so wide\n"
"                     is 320x160).  See docs/VIEW.md\n"
"  --cull MODE        with --view: stock (default), match or none\n"
"  --rando SEED[:F]   shuffle the enemies, with F the flag word from\n"
"                     platform/port/port.h (default 0x11: enemies, sizes\n"
"                     matched, abilities kept; 0x17 adds bosses)\n"
"  --hold MASK        hold these buttons down for the whole run\n"
"  --mash S:MASK:P[:E] from frame S, tap MASK every P frames, stop at E\n"
"  --press F:MASK,... set the button mask at frame F and leave it set\n"
"  --mp loopback[:N]  attach the in-process link transport, N units\n"
"  --mp-at F          ... at frame F (default 60)\n"
"  --turbo            do not pace; run as fast as the machine allows\n"
"  --record FILE      write the inputs of this run to FILE\n"
"  --replay FILE      drive the run from a recording instead of the keyboard\n"
"  --hash-log FILE    write one line of simulation-state hashes per frame\n"
"  --dump-state F:PATH write the canonical state bytes at frame F to PATH\n"
"  --warp F:ROOM[:X:Y] warp to ROOM (cell X,Y) at frame F; recorded\n"
"  --renderer R       ppu (default), gpu or both: see docs/RENDERER.md\n"
"  --gpu-view WxH     the GPU renderer's world viewport (default 240x160);\n"
"                     auto[:H] follows the window's aspect at height H\n"
"  --gpu-shot PATH    write a PNG of the GPU picture at the last frame\n"
"  --shot-at F:PREFIX write PREFIX-ppu.png and/or PREFIX-gpu.png at frame F\n"
"  --compare-log FILE with --renderer both: per-frame GPU/PPU pixel diffs\n"
"  --verbose          report the memory map and the frame timing\n"
"  --help\n"
"\n"
"Buttons: arrows or WASD, A = J/Z, B = K/X, L/R = Q/E, Start = Enter,\n"
"Select = Backspace or right shift.  A gamepad works too.\n"
"MASK is the GBA button mask: A=1 B=2 Select=4 Start=8 Right=16 Left=32\n"
"Up=64 Down=128 R=256 L=512.\n", argv0);
}

static int NeedValue(int i, int argc, char **argv)
{
    if (i + 1 >= argc) {
        fprintf(stderr, "katam-port: %s needs a value\n", argv[i]);
        exit(2);
    }
    return i + 1;
}

static void ParseArgs(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            Usage(argv[0]);
            exit(0);
        } else if (strcmp(a, "--scale") == 0) {
            sScale = atoi(argv[i = NeedValue(i, argc, argv)]);
            if (sScale < 1) sScale = 1;
            if (sScale > 16) sScale = 16;
        } else if (strcmp(a, "--fullscreen") == 0) {
            sFullscreen = 1;
        } else if (strcmp(a, "--vsync") == 0) {
            sVsync = 1;
        } else if (strcmp(a, "--no-audio") == 0) {
            gPortNativeNoAudio = 1;
        } else if (strcmp(a, "--turbo") == 0) {
            sTurbo = 1;
        } else if (strcmp(a, "--verbose") == 0) {
            gPortNativeVerbose = 1;
        } else if (strcmp(a, "--frames") == 0) {
            sFrameLimit = atol(argv[i = NeedValue(i, argc, argv)]);
        } else if (strcmp(a, "--screenshot") == 0) {
            sShotPath = argv[i = NeedValue(i, argc, argv)];
        } else if (strcmp(a, "--window-shot") == 0) {
            sReadbackPath = argv[i = NeedValue(i, argc, argv)];
        } else if (strcmp(a, "--view") == 0) {
            static const char *const kModes[] = { "native", "wide", "zoom",
                                                  "widezoom" };
            const char *v = argv[i = NeedValue(i, argc, argv)];
            size_t len = strcspn(v, ":");
            unsigned m;

            for (m = 0; m < 4; m++)
                if (strlen(kModes[m]) == len && strncmp(v, kModes[m], len) == 0)
                    break;
            if (m == 4) {
                fprintf(stderr, "katam-port: --view wants native, wide, zoom "
                                "or widezoom, optionally :PADX[:PADY]\n");
                exit(2);
            }
            sViewSet = 1;
            sViewMode = m;
            if (v[len] == ':')
                sscanf(v + len + 1, "%d:%d", &sViewPadX, &sViewPadY);
        } else if (strcmp(a, "--cull") == 0) {
            const char *v = argv[i = NeedValue(i, argc, argv)];

            if (strcmp(v, "stock") == 0)      sViewCull = PORT_CULL_STOCK;
            else if (strcmp(v, "match") == 0) sViewCull = PORT_CULL_MATCH;
            else if (strcmp(v, "none") == 0)  sViewCull = PORT_CULL_NONE;
            else {
                fprintf(stderr, "katam-port: --cull wants stock, match or none\n");
                exit(2);
            }
        } else if (strcmp(a, "--renderer") == 0) {
            const char *v = argv[i = NeedValue(i, argc, argv)];

            if (strcmp(v, "ppu") == 0)       sRendererMode = RENDERER_PPU;
            else if (strcmp(v, "gpu") == 0)  sRendererMode = RENDERER_GPU;
            else if (strcmp(v, "both") == 0) sRendererMode = RENDERER_BOTH;
            else {
                fprintf(stderr, "katam-port: --renderer wants ppu, gpu or both\n");
                exit(2);
            }
        } else if (strcmp(a, "--gpu-view") == 0) {
            const char *v = argv[i = NeedValue(i, argc, argv)];

            if (strncmp(v, "auto", 4) == 0) {
                sGpuViewAuto = 1;
                sGpuViewH = 160;
                if (v[4] == ':')
                    sGpuViewH = atoi(v + 5);
            } else if (sscanf(v, "%dx%d", &sGpuViewW, &sGpuViewH) != 2) {
                fprintf(stderr, "katam-port: --gpu-view wants WxH or auto[:H]\n");
                exit(2);
            }
        } else if (strcmp(a, "--gpu-shot") == 0) {
            sGpuShotPath = argv[i = NeedValue(i, argc, argv)];
        } else if (strcmp(a, "--compare-log") == 0) {
            sCompareLogPath = argv[i = NeedValue(i, argc, argv)];
        } else if (strcmp(a, "--shot-at") == 0) {
            const char *v = argv[i = NeedValue(i, argc, argv)];
            const char *colon = strchr(v, ':');

            if (colon == NULL || sNumShots >= MAX_SHOTS) {
                fprintf(stderr, "katam-port: --shot-at wants FRAME:PREFIX\n");
                exit(2);
            }
            sShots[sNumShots].frame = atol(v);
            sShots[sNumShots].prefix = colon + 1;
            sNumShots++;
        } else if (strcmp(a, "--rando") == 0) {
            /* Seed first, flags optional: the seed is the thing anyone
             * actually types, and the flag word is for the two people who
             * want bosses in it.  See docs/RANDO.md. */
            const char *v = argv[i = NeedValue(i, argc, argv)];
            unsigned seed = 0, flags = PORT_RANDO_ENEMIES
                                     | PORT_RANDO_KEEP_ABILITY;

            if (sscanf(v, "%u:%i", &seed, (int *)&flags) < 1) {
                fprintf(stderr, "katam-port: --rando wants SEED or SEED:FLAGS, "
                                "e.g. 4127 or 4127:0x17\n");
                exit(2);
            }
            sRandoSeed = seed;
            sRandoFlags = flags;
        } else if (strcmp(a, "--hold") == 0) {
            sHoldMask = (int)strtol(argv[i = NeedValue(i, argc, argv)], NULL, 0);
        } else if (strcmp(a, "--press") == 0) {
            const char *v = argv[i = NeedValue(i, argc, argv)];

            while (*v != '\0' && sNumPresses < MAX_PRESSES) {
                long f; int m;

                if (sscanf(v, "%ld:%i", &f, &m) != 2) {
                    fprintf(stderr, "katam-port: --press wants "
                            "FRAME:MASK[,FRAME:MASK...]\n");
                    exit(2);
                }
                sPresses[sNumPresses].frame = f;
                sPresses[sNumPresses].mask = m;
                sNumPresses++;
                while (*v != '\0' && *v != ',') v++;
                if (*v == ',') v++;
            }
        } else if (strcmp(a, "--mp") == 0) {
            const char *v = argv[i = NeedValue(i, argc, argv)];

            sMpKind = v;
            if (strncmp(v, "loopback", 8) == 0 && v[8] == ':')
                sMpPlayers = atoi(v + 9);
            if (sMpAt < 0)
                sMpAt = 60;
        } else if (strcmp(a, "--mp-at") == 0) {
            sMpAt = atol(argv[i = NeedValue(i, argc, argv)]);
            if (sMpKind == NULL)
                sMpKind = "loopback";
        } else if (strcmp(a, "--mash") == 0) {
            const char *v = argv[i = NeedValue(i, argc, argv)];

            /* A fourth field stops the mashing again, exactly as MASH= does
             * in tools/headless_test.js.  Without it a sequence copied from
             * the harness keeps tapping A through every later menu and lands
             * somewhere else -- which is what happened the first time this was
             * used to reach the link lobby. */
            sscanf(v, "%ld:%i:%i:%ld", &sMashStart, &sMashMask,
                   &sMashPeriod, &sMashEnd);
            if (sscanf(v, "%ld:%i:%i", &sMashStart, &sMashMask,
                       &sMashPeriod) != 3 || sMashPeriod < 2) {
                fprintf(stderr, "katam-port: --mash wants START:MASK:PERIOD, "
                                "e.g. 600:1:6\n");
                exit(2);
            }
        } else if (strcmp(a, "--record") == 0) {
            sRecordPath = argv[i = NeedValue(i, argc, argv)];
        } else if (strcmp(a, "--replay") == 0) {
            sReplayPath = argv[i = NeedValue(i, argc, argv)];
        } else if (strcmp(a, "--hash-log") == 0) {
            sHashLogPath = argv[i = NeedValue(i, argc, argv)];
        } else if (strcmp(a, "--dump-state") == 0) {
            const char *v = argv[i = NeedValue(i, argc, argv)];
            const char *colon = strchr(v, ':');

            if (colon == NULL || sNumDumps >= MAX_DUMPS) {
                fprintf(stderr, "katam-port: --dump-state wants FRAME:PATH\n");
                exit(2);
            }
            sDumps[sNumDumps].frame = atol(v);
            sDumps[sNumDumps].path = colon + 1;
            sNumDumps++;
        } else if (strcmp(a, "--warp") == 0) {
            const char *v = argv[i = NeedValue(i, argc, argv)];
            long f; unsigned r, x = 0, y = 0;

            if (sscanf(v, "%ld:%u:%u:%u", &f, &r, &x, &y) < 2
             || sNumWarps >= MAX_WARPS) {
                fprintf(stderr, "katam-port: --warp wants FRAME:ROOM[:X:Y]\n");
                exit(2);
            }
            sWarps[sNumWarps].frame = f;
            sWarps[sNumWarps].room = r;
            sWarps[sNumWarps].x = x;
            sWarps[sNumWarps].y = y;
            sNumWarps++;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "katam-port: unknown option %s (--help)\n", a);
            exit(2);
        } else {
            sRomArg = a;
        }
    }
}

/* --- shutdown ------------------------------------------------------------- */

/* Is the picture made of anything?
 *
 * A port that has stopped rendering still presents a frame every 16 ms, and it
 * exits 0 while doing it, so the useful question is how much is *in* the
 * frame.  Distinct colours answers it: a forced blank is 1, a fading logo is a
 * few dozen, a title screen a couple of hundred, a level a couple of thousand.
 *
 * Open addressing into a fixed table, so there is no allocation on a path that
 * may be running because something has already gone wrong. */
static int CountDistinctColours(void)
{
    enum { SLOTS = 8192 };
    static u32 seen[SLOTS];
    static u8 used[SLOTS];
    int distinct = 0;
    int i;

    memset(used, 0, sizeof(used));
    for (i = 0; i < gPortViewW * gPortViewH; i++) {
        u32 c = gPortFramebuffer[i];
        u32 h = (c * 2654435761u) % SLOTS;

        while (used[h] && seen[h] != c)
            h = (h + 1) % SLOTS;
        if (!used[h]) {
            used[h] = 1;
            seen[h] = c;
            distinct++;
        }
    }
    return distinct;
}

/* The best frame of the run, not the last one.
 *
 * The last frame was the obvious thing to measure and it is the wrong thing.
 * The game fades the screen constantly -- between the logo and the title, on
 * every room transition -- and a brightness fade collapses the palette to a
 * handful of greys.  A run that walked Kirby through a level and happened to
 * end mid-transition scored 47 colours and failed a check it should have
 * passed.  So the run is sampled, and what is reported is the most the port
 * ever managed: proof that it drew a real picture at some point, which is the
 * thing being asked. */
static int sBestColours;
static long sBestFrame;

/* And: is it still moving?
 *
 * "Drew a real picture" and "is still running the game" are different
 * questions, and a port that wedges after the title screen answers the first
 * one perfectly well.  A hash of each sampled frame, and a count of how many
 * came out different, separates them: a wedged game repeats one picture (or
 * cycles through a two-frame idle animation), and a game being played does
 * not. */
#define MAX_FRAME_HASHES 64
static u32 sFrameHashes[MAX_FRAME_HASHES];
static int sNumFrameHashes;

static void SampleFrame(void)
{
    int n = CountDistinctColours();
    u32 h = 2166136261u;
    int i;

    if (n > sBestColours) {
        sBestColours = n;
        sBestFrame = sFrames;
    }

    for (i = 0; i < gPortViewW * gPortViewH; i += 7)
        h = (h ^ gPortFramebuffer[i]) * 16777619u;

    for (i = 0; i < sNumFrameHashes; i++)
        if (sFrameHashes[i] == h)
            return;
    if (sNumFrameHashes < MAX_FRAME_HASHES)
        sFrameHashes[sNumFrameHashes++] = h;
}

static void ReportFrame(void)
{
    u32 backdrop = gPortFramebuffer[0];
    int nonBackdrop = 0;
    int i;

    for (i = 0; i < gPortViewW * gPortViewH; i++)
        if (gPortFramebuffer[i] != backdrop)
            nonBackdrop++;

    SampleFrame();
    PortLog("[katam-port] final frame: %d distinct colours, %d%% of pixels "
            "differ from the corner, DISPCNT=0x%04X",
            CountDistinctColours(),
            nonBackdrop * 100 / (gPortViewW * gPortViewH),
            (unsigned)*(vu16 *)(GBA_IO_BASE + REG_OFFSET_DISPCNT));
    PortLog("[katam-port] best frame: %d distinct colours, at frame %ld of %ld",
            sBestColours, sBestFrame, sFrames);
    PortLog("[katam-port] motion: %d distinct pictures among %ld samples%s",
            sNumFrameHashes, sFrames / 30 + 1,
            sNumFrameHashes >= MAX_FRAME_HASHES ? " (capped)" : "");
}

static void WriteRecording(void)
{
    u32 len = 0;
    char *text;
    FILE *f;

    if (sRecordPath == NULL)
        return;
    PortInputLogNoteFrames((u32)sFrames);
    text = PortInputLogSerialize(&len);
    f = text ? fopen(sRecordPath, "wb") : NULL;
    if (f == NULL || fwrite(text, 1, len, f) != len)
        PortError("[katam-port] could not write the recording to %s", sRecordPath);
    else
        PortLog("[katam-port] recorded %s", sRecordPath);
    if (f)
        fclose(f);
    free(text);
}

void PortHostFrameState(u32 frame, u16 keys, const u32 *hashes,
                        const u8 *dump, u32 dumpLen)
{
    int i;

    if (sHashLog != NULL)
        fprintf(sHashLog, "%u %03X %08X %08X %08X %08X\n", (unsigned)frame,
                (unsigned)keys, (unsigned)hashes[PORT_HASH_SIM],
                (unsigned)hashes[PORT_HASH_VID], (unsigned)hashes[PORT_HASH_SRAM],
                (unsigned)hashes[PORT_HASH_PIX]);
    if (dump == NULL)
        return;
    for (i = 0; i < sNumDumps; i++) {
        FILE *f;

        if (sDumps[i].frame != (long)frame)
            continue;
        f = fopen(sDumps[i].path, "wb");
        if (f == NULL || fwrite(dump, 1, dumpLen, f) != dumpLen)
            PortError("[katam-port] could not write %s", sDumps[i].path);
        if (f)
            fclose(f);
    }
}

static int WriteGpuShot(const char *path)
{
    int w, h;

    if (sGpuPixels == NULL)
        sGpuPixels = (u32 *)malloc(1024 * 512 * sizeof(u32));
    if (sGpuPixels == NULL || !GlrReadback(sGpuPixels, 1024 * 512, &w, &h)) {
        PortError("[katam-port] no GPU picture to write to %s", path);
        return 0;
    }
    if (!PortNativeWritePng(path, sGpuPixels, w, h)) {
        PortError("[katam-port] could not write %s", path);
        return 0;
    }
    PortLog("[katam-port] wrote %s (%dx%d, gpu)", path, w, h);
    return 1;
}

static void Shutdown(int code)
{
    WriteRecording();
    if (sHashLog != NULL) {
        fclose(sHashLog);
        sHashLog = NULL;
    }
    ReportFrame();
    if (sShotPath != NULL && sRendererMode == RENDERER_GPU && sGpuShotPath == NULL)
        sGpuShotPath = sShotPath;       /* the only picture there is */
    else if (sShotPath != NULL) {
        if (PortNativeWritePng(sShotPath, gPortFramebuffer,
                               gPortViewW, gPortViewH))
            PortLog("[katam-port] wrote %s", sShotPath);
        else
            PortError("[katam-port] could not write %s", sShotPath);
    }
    if (sGpuShotPath != NULL)
        WriteGpuShot(sGpuShotPath);
    PortRlReportPopIn();
    if (sCompareLog != NULL) {
        PortLog("[katam-port] gpu vs ppu: %ld frames compared, %ld differ, "
                "%ld pixels in all; %ld frames not comparable (view not 240x160)",
                sCompareFrames, sCompareDiffFrames, sCompareDiffPixels,
                sCompareSkipped);
        fclose(sCompareLog);
        sCompareLog = NULL;
    }
    PortNativeSaveFlush();
    PortNativeAudioClose();
    PortReportGaps();
    SDL_Quit();
    exit(code);
}

/* Last-ditch.  Writing a file from a signal handler is not something to be
 * proud of, but the alternative is losing a save the player already made, and
 * the process is going down either way. */
static void OnFatalSignal(int sig)
{
    if (sHashLog != NULL)
        fflush(sHashLog);       /* so a crash's log ends at the crash */
    PortNativeSaveFlush();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* --- input ---------------------------------------------------------------- */

static const struct { SDL_Scancode key; u16 bit; } kKeys[] = {
    { SDL_SCANCODE_RIGHT,     DPAD_RIGHT    }, { SDL_SCANCODE_D, DPAD_RIGHT },
    { SDL_SCANCODE_LEFT,      DPAD_LEFT     }, { SDL_SCANCODE_A, DPAD_LEFT  },
    { SDL_SCANCODE_UP,        DPAD_UP       }, { SDL_SCANCODE_W, DPAD_UP    },
    { SDL_SCANCODE_DOWN,      DPAD_DOWN     }, { SDL_SCANCODE_S, DPAD_DOWN  },
    { SDL_SCANCODE_J,         A_BUTTON      }, { SDL_SCANCODE_Z, A_BUTTON   },
    { SDL_SCANCODE_K,         B_BUTTON      }, { SDL_SCANCODE_X, B_BUTTON   },
    { SDL_SCANCODE_Q,         L_BUTTON      }, { SDL_SCANCODE_E, R_BUTTON   },
    { SDL_SCANCODE_RETURN,    START_BUTTON  },
    { SDL_SCANCODE_KP_ENTER,  START_BUTTON  },
    { SDL_SCANCODE_BACKSPACE, SELECT_BUTTON },
    { SDL_SCANCODE_RSHIFT,    SELECT_BUTTON },
};

/* A two-button game on a modern pad: the face cluster is split the way a
 * Nintendo layout maps onto an Xbox one, so the bottom and right buttons are
 * both A and the left and top are both B.  Nobody has to look it up. */
static const struct { SDL_GameControllerButton pad; u16 bit; } kPadButtons[] = {
    { SDL_CONTROLLER_BUTTON_A,             A_BUTTON      },
    { SDL_CONTROLLER_BUTTON_B,             A_BUTTON      },
    { SDL_CONTROLLER_BUTTON_X,             B_BUTTON      },
    { SDL_CONTROLLER_BUTTON_Y,             B_BUTTON      },
    { SDL_CONTROLLER_BUTTON_START,         START_BUTTON  },
    { SDL_CONTROLLER_BUTTON_BACK,          SELECT_BUTTON },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  L_BUTTON      },
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, R_BUTTON      },
    { SDL_CONTROLLER_BUTTON_DPAD_UP,       DPAD_UP       },
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN,     DPAD_DOWN     },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT,     DPAD_LEFT     },
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    DPAD_RIGHT    },
};

/* Half of full deflection.  A GBA d-pad is a switch; anything less than a
 * decided push should not read as one, and the diagonal has to be reachable. */
#define STICK_DEADZONE 16384

static u16 ReadInput(void)
{
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    u16 down = 0;
    size_t i;
    int p;

    for (i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++)
        if (keys[kKeys[i].key])
            down |= kKeys[i].bit;

    for (p = 0; p < 4; p++) {
        SDL_GameController *c = sPads[p];
        Sint16 ax, ay;

        if (c == NULL || !SDL_GameControllerGetAttached(c))
            continue;
        for (i = 0; i < sizeof(kPadButtons) / sizeof(kPadButtons[0]); i++)
            if (SDL_GameControllerGetButton(c, kPadButtons[i].pad))
                down |= kPadButtons[i].bit;

        ax = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        ay = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        if (ax < -STICK_DEADZONE) down |= DPAD_LEFT;
        if (ax >  STICK_DEADZONE) down |= DPAD_RIGHT;
        if (ay < -STICK_DEADZONE) down |= DPAD_UP;
        if (ay >  STICK_DEADZONE) down |= DPAD_DOWN;

        if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16384)
            down |= L_BUTTON;
        if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384)
            down |= R_BUTTON;
    }

    down |= (u16)sHoldMask;
    if (sMashStart >= 0 && sFrames >= sMashStart
     && (sMashEnd < 0 || sFrames < sMashEnd)
     && (sFrames - sMashStart) % sMashPeriod < sMashPeriod / 2)
        down |= (u16)sMashMask;

    /* --press is a *level*, not an edge: it sets the mask and it stays set
     * until the next entry says otherwise.  That matches PRESS_AT in
     * tools/headless_test.js, so a sequence copied from one runs in the other,
     * which is the whole point of adding it. */
    {
        int p;

        for (p = 0; p < sNumPresses; p++)
            if (sPresses[p].frame == sFrames)
                sPressMask = sPresses[p].mask;
    }
    down |= (u16)sPressMask;

    return down;
}

static void OpenPads(void)
{
    int i, n = SDL_NumJoysticks(), slot = 0;

    for (i = 0; i < n && slot < 4; i++) {
        if (!SDL_IsGameController(i))
            continue;
        if (sPads[slot] != NULL)
            continue;
        sPads[slot] = SDL_GameControllerOpen(i);
        if (sPads[slot] != NULL) {
            PortLog("[katam-port] gamepad %d: %s", slot,
                    SDL_GameControllerName(sPads[slot]));
            slot++;
        }
    }
}

/* --- window --------------------------------------------------------------- */

static void ApplyScale(void)
{
    if (sWindow == NULL || sFullscreen)
        return;
    SDL_SetWindowSize(sWindow, sTexW * sScale, sTexH * sScale);
}

static void ToggleFullscreen(void)
{
    sFullscreen = !sFullscreen;
    SDL_SetWindowFullscreen(sWindow,
                            sFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (!sFullscreen)
        ApplyScale();
}

/* The GPU renderer's window: a GL context instead of an SDL_Renderer.
 *
 * GLES 3.0 first where it is native (Linux, and what WebGL2 is), desktop 3.3
 * core first elsewhere (macOS and Windows have no GLES without ANGLE); each
 * falls back to the other.  KATAM_GL=es|core forces one. */
static int TryGl(int es)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        es ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, es ? 0 : 3);
#ifdef __APPLE__
    if (!es)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    sWindow = SDL_CreateWindow("katam-port",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               sTexW * sScale, sTexH * sScale,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL
                               | SDL_WINDOW_ALLOW_HIGHDPI
                               | (sFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (sWindow == NULL)
        return 0;
    sGl = SDL_GL_CreateContext(sWindow);
    if (sGl == NULL) {
        SDL_DestroyWindow(sWindow);
        sWindow = NULL;
        return 0;
    }
    SDL_GL_MakeCurrent(sWindow, sGl);
    if (!GlrInit((void *(*)(const char *))SDL_GL_GetProcAddress, es)) {
        SDL_GL_DeleteContext(sGl);
        SDL_DestroyWindow(sWindow);
        sGl = NULL;
        sWindow = NULL;
        return 0;
    }
    return 1;
}

static void OpenGpuWindow(void)
{
    const char *force = getenv("KATAM_GL");
#if defined(__linux__)
    int esFirst = 1;
#else
    int esFirst = 0;
#endif

    if (force != NULL && strcmp(force, "core") == 0) esFirst = 0;
    if (force != NULL && strcmp(force, "es") == 0) esFirst = 1;

    sTexW = sGpuViewAuto ? 284 : sGpuViewW;
    sTexH = sGpuViewH;
    if (!TryGl(esFirst) && !(force == NULL && TryGl(!esFirst))) {
        PortError("[katam-port] --renderer %s: no OpenGL ES 3.0 / GL 3.3 context "
                  "(%s); try --renderer ppu", sRendererMode == RENDERER_GPU ? "gpu" : "both",
                  SDL_GetError());
        exit(2);
    }
    SDL_GL_SetSwapInterval(sVsync ? 1 : 0);
    gPortRlEnabled = 1;
    PortRlSetViewSize(sGpuViewW, sGpuViewH);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    OpenPads();
    PortLog("[katam-port] video: %s, gpu renderer (%s), view %s%dx%d",
            SDL_GetCurrentVideoDriver(), GlrDescribe(),
            sGpuViewAuto ? "auto " : "", sGpuViewW, sGpuViewH);
}

static void OpenWindow(void)
{
    Uint32 rflags = SDL_RENDERER_ACCELERATED | (sVsync ? SDL_RENDERER_PRESENTVSYNC : 0);

#ifdef _WIN32
    /* The Windows build keeps its own main() -- CMakeLists.txt defines
     * SDL_MAIN_HANDLED there, so SDL_main.h does not rename it and SDL2main's
     * WinMain is not linked.  That is deliberate: WinMain would make this a
     * GUI-subsystem program with no stdout, and every diagnostic in this port
     * (including the one that explains a failed memory reservation) goes to
     * stdout.  SDL asks only that it be told the entry point was handled. */
    SDL_SetMainReady();
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        PortError("[katam-port] SDL_Init failed: %s", SDL_GetError());
        exit(2);
    }

    /* Nearest-neighbour.  A 240x160 picture blown up eight times with bilinear
     * filtering is a smear, and the PPU's output is exact. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    if (sRendererMode != RENDERER_PPU) {
        OpenGpuWindow();
        return;
    }

    if (sViewMode == PORT_VIEW_WIDE || sViewMode == PORT_VIEW_WIDE_ZOOM) {
        sTexW = PORT_SCREEN_W + 2 * sViewPadX;
        sTexH = PORT_SCREEN_H + 2 * sViewPadY;
    }

    sWindow = SDL_CreateWindow("katam-port",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               sTexW * sScale, sTexH * sScale,
                               SDL_WINDOW_RESIZABLE
                               | (sFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (sWindow == NULL) {
        PortError("[katam-port] no window: %s", SDL_GetError());
        exit(2);
    }

    sRenderer = SDL_CreateRenderer(sWindow, -1, rflags);
    if (sRenderer == NULL)
        sRenderer = SDL_CreateRenderer(sWindow, -1, SDL_RENDERER_SOFTWARE);
    if (sRenderer == NULL) {
        PortError("[katam-port] no renderer: %s", SDL_GetError());
        exit(2);
    }

    /* Aspect and integer ratio in two calls: the logical size makes SDL
     * letterbox 3:2 inside whatever the window is, and integer scale stops it
     * landing on a 2.37x that gives every third pixel row a different height. */
    SDL_RenderSetLogicalSize(sRenderer, sTexW, sTexH);
    SDL_RenderSetIntegerScale(sRenderer, SDL_TRUE);

    /* ABGR8888 is exactly what ToRgba in platform/ppu.c produces:
     * 0xFF000000 | b << 16 | g << 8 | r. */
    sTexture = SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_ABGR8888,
                                 SDL_TEXTUREACCESS_STREAMING,
                                 sTexW, sTexH);
    if (sTexture == NULL) {
        PortError("[katam-port] no texture: %s", SDL_GetError());
        exit(2);
    }

    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    OpenPads();

    /* Worth a line of its own.  SDL falls back through its whole driver list
     * and will happily end up on `dummy` if X11 and Wayland both refuse -- so
     * a run with no window looks exactly like a run with one until you ask. */
    {
        SDL_RendererInfo info;

        SDL_GetRendererInfo(sRenderer, &info);
        PortLog("[katam-port] video: %s, renderer %s%s",
                SDL_GetCurrentVideoDriver(), info.name,
                (info.flags & SDL_RENDERER_ACCELERATED) ? " (accelerated)" : "");
    }
}

/* --- ROM loading ---------------------------------------------------------- */

static int LoadRom(const char *path)
{
    SDL_RWops *f = SDL_RWFromFile(path, "rb");
    Sint64 size;
    size_t got;

    if (f == NULL) {
        PortError("[katam-port] cannot open %s: %s", path, SDL_GetError());
        return 0;
    }
    size = SDL_RWsize(f);
    if (size <= 0) {
        PortError("[katam-port] %s is empty", path);
        SDL_RWclose(f);
        return 0;
    }
    /* The ROM window is 32 MiB of address space, but save memory was relocated
     * to 0x09000000 -- inside it -- to save the wasm build 80 MiB of
     * reservation, so anything past 16 MiB would be read straight over the
     * save.  No commercial GBA cartridge is larger. */
    if (size > 0x01000000) {
        PortError("[katam-port] %s is %lld bytes; only the first 16 MiB is "
                  "mapped", path, (long long)size);
        size = 0x01000000;
    }

    got = SDL_RWread(f, (void *)GBA_ROM_BASE, 1, (size_t)size);
    SDL_RWclose(f);
    if (got != (size_t)size) {
        PortError("[katam-port] short read on %s", path);
        return 0;
    }

    /* Is this a GBA cartridge at all?
     *
     * Without this the port loads whatever it was handed, starts the game, and
     * segfaults following a pointer read out of it -- which is a confusing way
     * to be told you typed the wrong filename.  0xB2 is the header's "fixed
     * value" byte, 0x96 on every commercial cartridge and every ROM any
     * toolchain produces. */
    if (size < 0xC0 || ((const u8 *)GBA_ROM_BASE)[0xB2] != 0x96) {
        PortError("[katam-port] %s is not a Game Boy Advance ROM (no cartridge "
                  "header)", path);
        return 0;
    }
    /* The decompilation matches one specific build.  Another region or a hack
     * will load and will not work, so say so rather than let it look like a
     * bug in the port. */
    if (memcmp((const void *)(GBA_ROM_BASE + 0xAC), "B8KE", 4) != 0)
        PortError("[katam-port] this is game code %.4s, not B8KE -- the "
                  "decompilation matches the US release of Kirby & The Amazing "
                  "Mirror and nothing else.  Expect it to misbehave.",
                  (const char *)(GBA_ROM_BASE + 0xAC));

    PortRomLoaded((u32)size);
    PortNativeSaveInit((const u8 *)GBA_ROM_BASE, (size_t)size, path);
    if (sWindow != NULL) {
        char title[256];
        /* Both separators, because Windows accepts either and the file picker
         * hands back backslashes: a title of "katam-port -- C:\roms\x.gba" is
         * the whole path, which is not what was wanted. */
        const char *base = strrchr(path, '/');
        const char *back = strrchr(path, '\\');

        if (back != NULL && (base == NULL || back > base))
            base = back;

        snprintf(title, sizeof(title), "katam-port -- %s",
                 base ? base + 1 : path);
        SDL_SetWindowTitle(sWindow, title);
    }
    PortLog("[katam-port] loaded %s (%lld bytes)", path, (long long)size);
    return 1;
}

void PortAwaitRom(void)
{
    char picked[1024];

    if (sRomArg != NULL && LoadRom(sRomArg))
        return;

    if (sRomArg == NULL && PortHostPickRomFile(picked, sizeof(picked))
     && LoadRom(picked))
        return;

    /* No path, no picker, or the player cancelled.  The window is up; a ROM
     * dropped on it starts the game. */
    fprintf(stderr,
        "\n"
        "katam-port contains no game data.  It needs your own copy of\n"
        "Kirby & The Amazing Mirror.\n"
        "\n"
        "    katam-port /path/to/your-rom.gba\n"
        "\n"
        "or drop the file onto the window.\n\n");
    SDL_SetWindowTitle(sWindow, "katam-port -- drop a GBA ROM here");

    for (;;) {
        SDL_Event ev;

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                return;                 /* main() reports and exits */
            if (ev.type == SDL_DROPFILE) {
                char *file = ev.drop.file;
                int ok = LoadRom(file);

                SDL_free(file);
                if (ok)
                    return;
            }
        }
        if (sRenderer != NULL) {
            SDL_SetRenderDrawColor(sRenderer, 24, 24, 32, 255);
            SDL_RenderClear(sRenderer);
            SDL_RenderPresent(sRenderer);
        }
        SDL_Delay(16);
    }
}

/* --- the frame boundary --------------------------------------------------- */

/* The GPU path's blit: draw the render list built for this frame, compare
 * and capture as asked, present. */
static void GpuDrawSize(int *w, int *h)
{
    SDL_GL_GetDrawableSize(sWindow, w, h);
    if (*w <= 0 || *h <= 0) {
        *w = sTexW;
        *h = sTexH;
    }
}

static void GpuCompare(const RlFrame *f)
{
    int w, h, i, diff = 0;

    if (sGpuPixels == NULL)
        sGpuPixels = (u32 *)malloc(1024 * 512 * sizeof(u32));
    if (!GlrReadback(sGpuPixels, 1024 * 512, &w, &h))
        return;
    if (w != gPortViewW || h != gPortViewH) {
        sCompareSkipped++;
        return;
    }
    for (i = 0; i < w * h; i++)
        if (sGpuPixels[i] != gPortFramebuffer[i])
            diff++;
    sCompareFrames++;
    if (diff) {
        sCompareDiffFrames++;
        sCompareDiffPixels += diff;
    }
    fprintf(sCompareLog, "%ld %d world=%d hooks=%d spr=%u/%u/%u/%u\n", sFrames, diff,
            f->world, f->hookSpritesUsed, (unsigned)f->spritesFromHooks,
            (unsigned)f->spritesFromOam, (unsigned)f->spritesOffscreen,
            (unsigned)f->spritesOverflow);
}

static void GpuBlit(void)
{
    const RlFrame *f = PortRlCurrentFrame();
    int w, h, i;

    GpuDrawSize(&w, &h);
    if (sGpuViewAuto) {
        /* The world viewport follows the window's shape at a fixed height
         * in game pixels -- applied to the next frame built. */
        s32 vw = (s32)((long)sGpuViewH * w / (h ? h : 1));
        PortRlSetViewSize(vw & ~1, sGpuViewH);
    }
    if (f == NULL)
        return;
    GlrDraw(f);
    if (sCompareLog != NULL)
        GpuCompare(f);
    for (i = 0; i < sNumShots; i++) {
        char name[1024];

        if (sShots[i].frame != sFrames)
            continue;
        snprintf(name, sizeof(name), "%s-gpu.png", sShots[i].prefix);
        WriteGpuShot(name);
        if (sRendererMode == RENDERER_BOTH) {
            snprintf(name, sizeof(name), "%s-ppu.png", sShots[i].prefix);
            if (PortNativeWritePng(name, gPortFramebuffer, gPortViewW, gPortViewH))
                PortLog("[katam-port] wrote %s", name);
        }
    }
    GlrPresent(w, h);
    if (sReadbackPath != NULL && sFrames + 1 == sFrameLimit) {
        u32 *buf = (u32 *)malloc((size_t)w * h * 4);
        int y;

        if (buf != NULL) {
            /* The window, read back: bottom row first in GL, so flip. */
            GLR_FN(glReadPixels)(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            for (y = 0; y < h / 2; y++) {
                u32 tmp[4096];
                int n = w < 4096 ? w : 4096;
                memcpy(tmp, buf + y * w, n * 4);
                memcpy(buf + y * w, buf + (h - 1 - y) * w, n * 4);
                memcpy(buf + (h - 1 - y) * w, tmp, n * 4);
            }
            if (PortNativeWritePng(sReadbackPath, buf, w, h))
                PortLog("[katam-port] wrote %s (%dx%d, the window)", sReadbackPath, w, h);
            free(buf);
        }
    }
    /* With --vsync the swap happens in PaceFrame, at display rate. */
    if (!sVsync || sTurbo) {
        SDL_GL_SwapWindow(sWindow);
        sPresents++;
    }
}

void PortBlitFramebuffer(const u32 *pixels, int w, int h)
{
    if (sRendererMode != RENDERER_PPU) {
        (void)pixels; (void)w; (void)h;
        GpuBlit();
        return;
    }
    if (sTexture == NULL)
        return;
    /* The view rectangle can be any size up to PORT_MAX_VIEW_W/H, and under
     * zoom it changes as the camera creeps; a texture of the wrong size would
     * read the frame with the wrong stride. */
    if (w != sTexW || h != sTexH) {
        SDL_Texture *t = SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_ABGR8888,
                                           SDL_TEXTUREACCESS_STREAMING, w, h);
        if (t == NULL)
            return;
        SDL_DestroyTexture(sTexture);
        sTexture = t;
        sTexW = w;
        sTexH = h;
        SDL_RenderSetLogicalSize(sRenderer, w, h);
    }
    SDL_UpdateTexture(sTexture, NULL, pixels, w * (int)sizeof(u32));
    SDL_RenderClear(sRenderer);
    SDL_RenderCopy(sRenderer, sTexture, NULL, NULL);
    if (sReadbackPath != NULL && sFrames + 1 == sFrameLimit) {
        /* What the window is really showing, read back from the renderer
         * rather than from the framebuffer that fed it -- the one way to tell
         * a correct picture from a correct picture drawn in the wrong pixel
         * format.  Before the present, because the back buffer is undefined
         * after it. */
        int w2, h2;
        u32 *buf;

        SDL_GetRendererOutputSize(sRenderer, &w2, &h2);
        buf = (u32 *)malloc((size_t)w2 * h2 * 4);
        if (buf != NULL) {
            if (SDL_RenderReadPixels(sRenderer, NULL, SDL_PIXELFORMAT_ABGR8888,
                                     buf, w2 * 4) == 0
             && PortNativeWritePng(sReadbackPath, buf, w2, h2))
                PortLog("[katam-port] wrote %s (%dx%d, read back from the "
                        "renderer)", sReadbackPath, w2, h2);
            else
                PortError("[katam-port] renderer readback failed: %s",
                          SDL_GetError());
            free(buf);
        }
    }
    SDL_RenderPresent(sRenderer);
}

static void HandleEvents(void)
{
    SDL_Event ev;

    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            Shutdown(0);
            break;
        case SDL_DROPFILE:
            /* A second ROM mid-run would need the whole game restarted; say so
             * rather than half-doing it. */
            PortLog("[katam-port] a ROM is already running -- restart to load "
                    "a different one");
            SDL_free(ev.drop.file);
            break;
        case SDL_CONTROLLERDEVICEADDED:
            OpenPads();
            break;
        case SDL_KEYDOWN:
            if (ev.key.repeat)
                break;
            switch (ev.key.keysym.sym) {
            case SDLK_F11:
                ToggleFullscreen();
                break;
            case SDLK_RETURN:
                if (ev.key.keysym.mod & KMOD_ALT)
                    ToggleFullscreen();
                break;
            case SDLK_ESCAPE:
                if (sFullscreen)
                    ToggleFullscreen();
                break;
            case SDLK_F12:
                {
                    char name[64];

                    snprintf(name, sizeof(name), "katam-%06ld.png", sFrames);
                    if (sRendererMode != RENDERER_PPU)
                        WriteGpuShot(name);
                    else if (PortNativeWritePng(name, gPortFramebuffer,
                                                gPortViewW, gPortViewH))
                        PortLog("[katam-port] wrote %s", name);
                }
                break;
            case SDLK_q:
                if (ev.key.keysym.mod & KMOD_CTRL)
                    Shutdown(0);
                break;
            case SDLK_1: case SDLK_2: case SDLK_3:
            case SDLK_4: case SDLK_5: case SDLK_6:
                sScale = ev.key.keysym.sym - SDLK_1 + 1;
                if (sFullscreen)
                    ToggleFullscreen();
                else
                    ApplyScale();
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }
    }
}

/* Sleep until the next 59.7275 Hz boundary.
 *
 * Falling behind is normal -- a room transition decompresses tilesets and
 * takes longer than a frame -- and the right response is to give up the time
 * rather than to try to earn it back, which only makes the next frame late
 * too.  More than four frames behind and the schedule is simply restarted:
 * that is a machine that was suspended, or a debugger, and running 200 frames
 * of catch-up at once helps nobody. */
static void PaceFrame(void)
{
    Uint64 period, now;

    if (sTurbo)
        return;

    period = (Uint64)((double)sPerfFreq / GBA_FRAME_HZ);
    if (sNextFrame == 0)
        sNextFrame = SDL_GetPerformanceCounter();
    sNextFrame += period;

    now = SDL_GetPerformanceCounter();
    if (now > sNextFrame + 4 * period) {
        sNextFrame = now;
        return;
    }

    /* The GPU renderer with --vsync: present at the display's rate until
     * the next simulation frame is due, re-showing the latest frame.  This is
     * where interpolation goes (docs/RENDERER.md): the render list of the
     * previous and the current frame, blended by how far `now` is between
     * them.  Each swap blocks for one refresh, so stop when less than half a
     * refresh is left and let the sleep below land on the boundary. */
    if (sRendererMode != RENDERER_PPU && sVsync) {
        for (;;) {
            Uint64 t0 = SDL_GetPerformanceCounter(), t1;
            int w, h;

            GpuDrawSize(&w, &h);
            GlrPresent(w, h);
            SDL_GL_SwapWindow(sWindow);
            sPresents++;
            t1 = SDL_GetPerformanceCounter();
            sPresentInterval = sPresentInterval
                ? (sPresentInterval * 7 + (t1 - t0)) / 8 : (t1 - t0);
            now = t1;
            if (now + sPresentInterval / 2 >= sNextFrame)
                break;
        }
    }

    while (now < sNextFrame) {
        Uint64 left = sNextFrame - now;

        /* Coarse then fine.  SDL_Delay rounds up to a scheduler tick, so
         * asking it for the last millisecond overshoots every time. */
        if (left * 1000 / sPerfFreq > 2)
            SDL_Delay((Uint32)(left * 1000 / sPerfFreq) - 1);
        else
            SDL_Delay(0);       /* yield; the remainder is a spin */
        now = SDL_GetPerformanceCounter();
    }
}

/* Catch-up's yield: keep the window responsive, do nothing else.  No
 * PaceFrame -- catch-up exists to outrun the clock -- and no ReadInput, since
 * a caught-up frame takes its input from the rollback timeline, not the
 * keyboard.  PaceFrame's own more-than-four-frames-behind reset absorbs the
 * schedule debt when normal pacing resumes. */
void PortAwaitYield(void)
{
    HandleEvents();
}

/* Nothing to pump natively: the loopback transport has no receive queue
 * outside the engine. */
void PortNetIdle(void)
{
}

void PortAwaitAnimationFrame(void)
{
    HandleEvents();
    PortSetKeys(ReadInput());
    PortNativeSaveTick();

    /* Attach the link transport at the requested frame, so a run has a
     * before and an after and a link that comes up mid-run can be told apart
     * from one that was there all along -- the same reason MP_AT exists in
     * tools/headless_test.js. */
    if (sMpKind != NULL && !sMpAttached && sFrames >= sMpAt) {
        sMpAttached = 1;
        if (!PortMpUseLoopback(sMpPlayers))
            PortError("[katam-port] --mp %s: could not attach", sMpKind);
    }

    sFrames++;
    /* Every half second or so.  Often enough that no phase of the boot goes
     * unmeasured, rare enough that 38400 pixel probes do not show up next to
     * the software PPU that produced them. */
    if (sFrames % 30 == 0)
        SampleFrame();

    if (sFrameLimit >= 0 && sFrames >= sFrameLimit) {
        PortLog("[katam-port] %ld frames, %d audio underruns", sFrames,
                PortAudioUnderruns());
        Shutdown(0);
    }

    if (gPortNativeVerbose && sFrames % 300 == 0) {
        PortLog("[katam-port] frame %ld, audio queue %d frames, %d underruns",
                sFrames, PortAudioQueuedFrames(), PortAudioUnderruns());
    }

    PaceFrame();
}

/* --- startup -------------------------------------------------------------- */

void PortHostInit(int argc, char **argv)
{
    ParseArgs(argc, argv);

    /* First, before SDL allocates a byte or starts a thread.  The GBA's
     * addresses have to be claimed while nothing else in the process has had
     * the chance to want them. */
    PortNativeReserveMap();
    if (gPortNativeVerbose)
        PortNativeReportMap();

    OpenWindow();
    sPerfFreq = SDL_GetPerformanceFrequency();
    if (sRendererMode == RENDERER_GPU)
        gPortPpuCompose = 0;
    if (sCompareLogPath != NULL) {
        sCompareLog = fopen(sCompareLogPath, "w");
        if (sCompareLog == NULL) {
            PortError("[katam-port] cannot write %s", sCompareLogPath);
            exit(2);
        }
        if (sRendererMode != RENDERER_BOTH)
            PortError("[katam-port] --compare-log needs --renderer both");
    }

    signal(SIGSEGV, OnFatalSignal);
    signal(SIGABRT, OnFatalSignal);
    signal(SIGINT, OnFatalSignal);
    signal(SIGTERM, OnFatalSignal);

    /* --frames deliberately does *not* imply --turbo.  Keeping them separate
     * is what makes the pacing measurable: `time katam rom --frames 600`
     * should take 600/59.7275 = 10.05 seconds, and a build where that comes
     * out at 10.00 has quietly gone back to 60 Hz.  tools/native_smoke.sh
     * asks for --turbo when it wants the run to be quick. */
    if (sRandoFlags)
        PortSetRando(sRandoFlags, sRandoSeed);

    /* Only when asked: never calling PortSetViewMode leaves every view global
     * at the hardware's number.  The tilemap setting is the page's default,
     * 2 -- read the columns the hardware had no room for from the room's own
     * map -- and the HUD floats. */
    if (sViewSet) {
        PortSetViewMode(sViewMode, sViewPadX, sViewPadY, sViewCull, 0, 2);
        if (sViewMode != PORT_VIEW_NATIVE)
            PortLog("[katam-port] view: %s, pad %d:%d, cull %u",
                    sViewMode == PORT_VIEW_WIDE ? "wide"
                    : sViewMode == PORT_VIEW_ZOOM ? "zoom" : "wide+zoom",
                    sViewPadX, sViewPadY, sViewCull);
    }

    /* The harness.  After rando and view, so that a recording made with
     * either notes it, and so that a replay's own options win. */
    if (sReplayPath != NULL) {
        FILE *f = fopen(sReplayPath, "rb");
        char *text = NULL;
        long len = 0, frames = -1;

        if (f != NULL) {
            fseek(f, 0, SEEK_END);
            len = ftell(f);
            fseek(f, 0, SEEK_SET);
            text = (char *)malloc((size_t)len + 1);
            if (text != NULL && fread(text, 1, (size_t)len, f) == (size_t)len)
                frames = PortInputLogLoad(text, (u32)len);
            fclose(f);
            free(text);
        }
        if (frames < 0) {
            PortError("[katam-port] cannot replay %s", sReplayPath);
            exit(2);
        }
        if (sFrameLimit < 0)
            sFrameLimit = frames;
        /* Nothing the host polls reaches the game during a replay -- the
         * latch override wins -- but switch the scripted inputs off anyway,
         * so that nothing about the run depends on them. */
        sHoldMask = 0;
        sMashStart = -1;
        sNumPresses = 0;
    }
    if (sRecordPath != NULL) {
        PortInputLogRecordStart();
        if (sRandoFlags)
            PortInputLogNoteRando(sRandoFlags, sRandoSeed);
        if (sViewSet)
            PortInputLogNoteView(sViewMode, sViewPadX, sViewPadY, sViewCull, 0, 2);
    }
    {
        int w;

        for (w = 0; w < sNumWarps; w++)
            PortInputLogScheduleWarp((u32)sWarps[w].frame, 0,
                                     (u16)sWarps[w].room, (u8)sWarps[w].x,
                                     (u8)sWarps[w].y);
    }
    if (sHashLogPath != NULL) {
        sHashLog = fopen(sHashLogPath, "w");
        if (sHashLog == NULL) {
            PortError("[katam-port] cannot write %s", sHashLogPath);
            exit(2);
        }
        PortSetFrameStateLog(1);
    }
    {
        int d;

        for (d = 0; d < sNumDumps; d++)
            PortSetStateDumpAt((u32)sDumps[d].frame);
    }

    if (sTurbo)
        PortLog("[katam-port] frame pacing off -- running as fast as this "
                "machine allows");
}

#ifdef __cplusplus
}
#endif
