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
#include "gba/gba.h"

/* C linkage for the 64-bit builds -- see tools/cxxify.py.  Below the includes,
 * so SDL's headers stay outside the block. */
#ifdef __cplusplus
extern "C" {
#endif

/* 280896 cycles per frame at 16.777216 MHz.  Do not round this to 60. */
#define GBA_FRAME_HZ 59.7275

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
static int  sMashMask, sMashPeriod;

static long sFrames;
static Uint64 sPerfFreq, sNextFrame;

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
"  --hold MASK        hold these buttons down for the whole run\n"
"  --mash S:MASK:P    from frame S, tap MASK with a period of P frames\n"
"  --turbo            do not pace; run as fast as the machine allows\n"
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
        } else if (strcmp(a, "--hold") == 0) {
            sHoldMask = (int)strtol(argv[i = NeedValue(i, argc, argv)], NULL, 0);
        } else if (strcmp(a, "--mash") == 0) {
            const char *v = argv[i = NeedValue(i, argc, argv)];

            if (sscanf(v, "%ld:%i:%i", &sMashStart, &sMashMask,
                       &sMashPeriod) != 3 || sMashPeriod < 2) {
                fprintf(stderr, "katam-port: --mash wants START:MASK:PERIOD, "
                                "e.g. 600:1:6\n");
                exit(2);
            }
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
    for (i = 0; i < PORT_SCREEN_W * PORT_SCREEN_H; i++) {
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

    for (i = 0; i < PORT_SCREEN_W * PORT_SCREEN_H; i += 7)
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

    for (i = 0; i < PORT_SCREEN_W * PORT_SCREEN_H; i++)
        if (gPortFramebuffer[i] != backdrop)
            nonBackdrop++;

    SampleFrame();
    PortLog("[katam-port] final frame: %d distinct colours, %d%% of pixels "
            "differ from the corner, DISPCNT=0x%04X",
            CountDistinctColours(),
            nonBackdrop * 100 / (PORT_SCREEN_W * PORT_SCREEN_H),
            (unsigned)*(vu16 *)(GBA_IO_BASE + REG_OFFSET_DISPCNT));
    PortLog("[katam-port] best frame: %d distinct colours, at frame %ld of %ld",
            sBestColours, sBestFrame, sFrames);
    PortLog("[katam-port] motion: %d distinct pictures among %ld samples%s",
            sNumFrameHashes, sFrames / 30 + 1,
            sNumFrameHashes >= MAX_FRAME_HASHES ? " (capped)" : "");
}

static void Shutdown(int code)
{
    ReportFrame();
    if (sShotPath != NULL) {
        if (PortNativeWritePng(sShotPath, gPortFramebuffer,
                               PORT_SCREEN_W, PORT_SCREEN_H))
            PortLog("[katam-port] wrote %s", sShotPath);
        else
            PortError("[katam-port] could not write %s", sShotPath);
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
     && (sFrames - sMashStart) % sMashPeriod < sMashPeriod / 2)
        down |= (u16)sMashMask;

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
    SDL_SetWindowSize(sWindow, PORT_SCREEN_W * sScale, PORT_SCREEN_H * sScale);
}

static void ToggleFullscreen(void)
{
    sFullscreen = !sFullscreen;
    SDL_SetWindowFullscreen(sWindow,
                            sFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (!sFullscreen)
        ApplyScale();
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

    sWindow = SDL_CreateWindow("katam-port",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               PORT_SCREEN_W * sScale, PORT_SCREEN_H * sScale,
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
    SDL_RenderSetLogicalSize(sRenderer, PORT_SCREEN_W, PORT_SCREEN_H);
    SDL_RenderSetIntegerScale(sRenderer, SDL_TRUE);

    /* ABGR8888 is exactly what ToRgba in platform/ppu.c produces:
     * 0xFF000000 | b << 16 | g << 8 | r. */
    sTexture = SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_ABGR8888,
                                 SDL_TEXTUREACCESS_STREAMING,
                                 PORT_SCREEN_W, PORT_SCREEN_H);
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
        SDL_SetRenderDrawColor(sRenderer, 24, 24, 32, 255);
        SDL_RenderClear(sRenderer);
        SDL_RenderPresent(sRenderer);
        SDL_Delay(16);
    }
}

/* --- the frame boundary --------------------------------------------------- */

void PortBlitFramebuffer(const u32 *pixels, int w, int h)
{
    if (sTexture == NULL)
        return;
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
                    if (PortNativeWritePng(name, gPortFramebuffer,
                                           PORT_SCREEN_W, PORT_SCREEN_H))
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

void PortAwaitAnimationFrame(void)
{
    HandleEvents();
    PortSetKeys(ReadInput());
    PortNativeSaveTick();

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

    signal(SIGSEGV, OnFatalSignal);
    signal(SIGABRT, OnFatalSignal);
    signal(SIGINT, OnFatalSignal);
    signal(SIGTERM, OnFatalSignal);

    /* --frames deliberately does *not* imply --turbo.  Keeping them separate
     * is what makes the pacing measurable: `time katam rom --frames 600`
     * should take 600/59.7275 = 10.05 seconds, and a build where that comes
     * out at 10.00 has quietly gone back to 60 Hz.  tools/native_smoke.sh
     * asks for --turbo when it wants the run to be quick. */
    if (sTurbo)
        PortLog("[katam-port] frame pacing off -- running as fast as this "
                "machine allows");
}

#ifdef __cplusplus
}
#endif
