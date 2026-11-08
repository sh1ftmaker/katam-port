/* The settings menu (Dear ImGui, drawn into the game's GL context) and the
 * shared presentation path it sits on.  C API; the implementation is C++
 * (ui_menu.cpp, ui_gl.cpp) and C (present.c).
 *
 * Hosts call:
 *   PortShowInit(es)            once the GL context is current and GlrInit ran
 *   PortShowNewFrame(...)       for each simulated frame, in PortBlitFramebuffer
 *   PortShowAgain(...)          to re-present without simulating (menu pause,
 *                               display-rate presents)
 *   PortUiPointer/Wheel/Key     input, in framebuffer pixels
 * and implement the PortHost* hooks at the bottom.
 */
#ifndef GUARD_PORT_UI_UI_H
#define GUARD_PORT_UI_UI_H

/* no game headers: included from the C++ menu too */

#ifdef __cplusplus
extern "C" {
#endif

/* --- presentation (present.c) -------------------------------------------- */

/* gpuAvailable: the render list can be drawn here (GlrInit succeeded). */
int PortShowInit(int es, int gpuAvailable);
int PortShowReady(void);
void PortShowNewFrame(int fbW, int fbH, float dpr, double nowMs,
                      const unsigned int *ppu, int ppuW, int ppuH);
void PortShowAgain(int fbW, int fbH, float dpr, double nowMs);
/* After any change to gPortSettings: apply, and ask the host to persist. */
void PortSettingsChanged(void);
/* Applies the settings that need no GL (audio, HUD); hosts call it at start. */
void PortSettingsApplyEarly(void);
/* Which renderer is drawing right now (the setting, unless unavailable). */
int PortShowUsingGpu(void);
/* The host sets this when it calls PortShowAgain at the display's rate
 * between sim frames (vsync; not turbo, not a scripted run): the condition
 * for interpolating. */
extern int gPortShowDisplayRate;
/* The host, before PortShowNewFrame: when on its 59.73 Hz schedule the frame
 * about to be shown was due (the clock nowMs is on).  Optional: without it
 * the presenter follows the arrival times. */
void PortShowFrameDue(double dueMs);
/* The host: a picture just went to the display (KATAM_CAMLOG). */
void PortShowNoteSwap(double nowMs);

/* --- the menu (ui_menu.cpp) ---------------------------------------------- */

int PortUiInit(int es);
void PortUiDraw(int fbW, int fbH, float dpr, double nowMs);
int PortUiIsOpen(void);
void PortUiSetOpen(int open);
void PortUiToggle(void);

#define PORT_UI_MOVE   0
#define PORT_UI_DOWN   1
#define PORT_UI_UP     2
#define PORT_UI_CANCEL 3
/* touch: 1 for a finger (tap / drag-to-scroll / hold-to-drag gestures). */
void PortUiPointer(int kind, float x, float y, int touch);
void PortUiWheel(float dx, float dy);

enum { PORT_UIK_NONE = 0, PORT_UIK_TAB, PORT_UIK_LEFT, PORT_UIK_RIGHT,
       PORT_UIK_UP, PORT_UIK_DOWN, PORT_UIK_PGUP, PORT_UIK_PGDN,
       PORT_UIK_HOME, PORT_UIK_END, PORT_UIK_ENTER, PORT_UIK_ESCAPE,
       PORT_UIK_SPACE, PORT_UIK_BACKSPACE };
void PortUiKey(int key, int down);

/* What the menu shows about the running build. */
typedef struct PortUiStatus {
    const char *platform;       /* "web" / "native"                         */
    const char *build;          /* commit and date                          */
    const char *gpuDesc;
    int gpuAvailable;
    int vsyncControl;           /* the host can switch vsync                */
    int netplay;                /* menu does not pause                      */
    int touch;                  /* coarse pointer: bigger widgets           */
    int drawGear;               /* draw the open button (native)            */
    float simFps, drawFps;
    int viewW, viewH, world, gpu;
    float zoomNow;              /* effective zoom including dynamic         */
    float hudScaleNow;          /* window pixels per HUD pixel              */
} PortUiStatus;
extern PortUiStatus gPortUiStatus;

/* --- host hooks ---------------------------------------------------------- */

void PortHostSaveSettings(const char *text);
void PortHostSetVolume(float gain);         /* 0..1, output only          */
void PortHostSetVsync(int on);
void PortHostUiOpenChanged(int open);       /* release game input, etc.   */

#ifdef __cplusplus
}
#endif

#endif
