/* Player settings: presentation only.
 *
 * Everything here changes how a frame is shown or heard, never what the game
 * does: the renderer choice (PPU or render list, both of which read the same
 * console state), the GPU view rectangle and its scaling, the HUD's anchoring,
 * and the output volume.  None of them is written to game memory, none is in a
 * recording, and `make regress` (which never loads them) is unaffected.  The
 * one exception is deliberate: the game's spawn/despawn windows follow the
 * widest view the settings allow, which present.c hands the simulation as a
 * recorded request (PortSimViewRequest, docs/VIEW.md "Spawn margin").  While
 * a netplay session holds them at the original, a view past the spawn margin
 * shows pop-in; PortLayoutPastSpawnMargin says when.
 *
 * The settings menu (ui_menu.cpp) is a view over this struct; a later HTML or
 * native UI can drive the same struct and call PortSettingsChanged().
 *
 * Persisted as "key=value" lines: localStorage on the web
 * (host_web.c), settings.ini in SDL's pref dir natively (host_sdl.c).
 */
#ifndef GUARD_PORT_UI_SETTINGS_H
#define GUARD_PORT_UI_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

enum { PS_RENDER_GPU = 0, PS_RENDER_PPU = 1 };
enum { PS_ASPECT_AUTO = 0, PS_ASPECT_3_2, PS_ASPECT_4_3, PS_ASPECT_16_10,
       PS_ASPECT_16_9, PS_ASPECT_21_9, PS_ASPECT_FILL, PS_ASPECT_COUNT };
enum { PS_SCALE_INTEGER = 0, PS_SCALE_FIT, PS_SCALE_SMOOTH, PS_SCALE_COUNT };
enum { PS_HUD_CORNERS = 0, PS_HUD_STRIP };
enum { PS_PARTY_OFF = 0, PS_PARTY_PLAYERS, PS_PARTY_HELPERS };
#define PS_HUD_SCALE_MAX 8

typedef struct PortSettings {
    int renderer;       /* PS_RENDER_*                                      */
    float zoom;         /* 1.0 = 160 world rows visible, as on the GBA      */
    int dynZoom;        /* ease in while the camera rests (docs/VIEW.md)    */
    int aspect;         /* PS_ASPECT_*                                      */
    int scaling;        /* PS_SCALE_*                                       */
    unsigned letterbox; /* 0xRRGGBB                                         */
    int hud;            /* PS_HUD_*                                         */
    int hudScale;       /* 0 auto (the size at zoom 1), else N window pixels
                         * per HUD pixel; PS_HUD_SCALE_MAX at most          */
    int vsync;          /* native: present on the display's refresh         */
    int showFps;
    int volume;         /* 0..100                                           */
    int mute;
    float uiScale;      /* menu size multiplier, 1.0 = automatic            */
    int smoothCam;      /* GPU: the camera at the game's sub-pixel position */
    int interp;         /* GPU: lerp between sim frames at display rate     */
    int partyHud;       /* PS_PARTY_*: the party HUD (party_hud.c) -- other
                         * players' cards, and the CPU helpers' with them  */
    int nameTags;       /* small name tags over the players' Kirbys (party_hud.c) */
    /* Lighting (GPU renderer, every level; platform/render/light.c). */
    int lightOn;        /* 0 off (the default, on every page), 1 on       */
    int lightMode;      /* LIGHT_REQ_AUTO / _SUN / _POINT                   */
    float lightStrength, lightShadow, lightSoft, lightAngle, lightWarm,
          lightCool, lightGlow;   /* PortLightParams                        */
    float lightRelief, lightShine, lightHeight;   /* ... normal maps        */
    int lightSunAnim;   /* the sun crosses the sky                          */
    float lightSunSpeed;  /* ... degrees a second                           */
    float lightSprShadow; /* sprites' drop shadows, 0 off                   */
    int lightReliefBg;  /* relief on background layers too                  */
    float lightDepth;   /* light into solid ground, px                      */
    float lightCharReach; /* character shadow length, px                    */
    float lightCharDepth; /* character shadows into solid ground, px        */
    int lightReliefOn;  /* normal maps at all (relief, shine)               */
    float lightIndoor;  /* point-lit rooms: shade away from the lamps       */
    int lightEngine;    /* LIGHT_ENGINE_*: classic, radiance cascades       */
    float lightBounce;  /* radiance cascades: light off solids              */
    int lightSolidShade; /* darken inside solid ground (below lightDepth)   */
    /* EXPERIMENTAL > CPU KIRBYS (platform/menu_poc.c): Kirbys in all, you
     * and the CPU helpers, 4 (the game as it is) .. 16.  Above 4 needs the
     * wide build (port/wide.h): on the web the page loads its second module
     * (katam16.js) at start when this is above 4, so a change from 4 or back
     * to 4 takes a reload; inside the wide module it applies live.  Per
     * device, never online (the wide module has no multiplayer). */
    int kirbys;
} PortSettings;

extern PortSettings gPortSettings;

void PortSettingsDefaults(PortSettings *s);
/* Apply "key=value" lines over *s; unknown keys and bad values are skipped.
 *
 * Schema: the text carries "schema=N".  Text from an older schema (no line,
 * or a smaller N -- every build before 2026-11-15) is migrated once: only
 * the player cards ("cardN=") and the volume ("volume=", "mute=") are kept,
 * everything else starts from the defaults.  Older builds' gear menu set
 * things the in-game menu never shows (dynamic zoom, the HUD's size and
 * anchoring, interpolation, the menu size...), and those must not linger
 * where nobody can see or undo them.  PortSettingsParseInfo says what the
 * last parse found; the host saves straight away after a migration. */
#define PORT_SETTINGS_SCHEMA 2
void PortSettingsParse(PortSettings *s, const char *text);
/* The last PortSettingsParse: the schema the text had (0: none -- an older
 * build, or nothing stored), and whether it was migrated (1) or kept. */
void PortSettingsParseInfo(int *storedSchema, int *migrated);
/* Every lighting setting but the on/off switch to its default. */
void LightDefaults(PortSettings *s);
/* Returns the length written (always NUL-terminated). */
int PortSettingsFormat(const PortSettings *s, char *buf, int size);
void PortSettingsClamp(PortSettings *s);
float PortAspectValue(int aspect, float windowAspect);
const char *PortAspectName(int aspect);

/* The view rectangle and where it goes on screen.
 *
 * fbW x fbH is the drawable in device pixels.  For gameplay on the GPU path
 * (`world`), viewW x viewH is the world viewport to ask the render list for;
 * otherwise the source is srcW x srcH (240x160 menus, the PPU picture) and
 * the view is that.  dst is where the source lands, top-left origin. */
typedef struct PortLayout {
    int viewW, viewH;
    int dst[4];
    int smooth;
} PortLayout;

void PortLayoutCompute(const PortSettings *s, int fbW, int fbH, int world,
                       float zoom, int srcW, int srcH, PortLayout *out);
/* Fit an srcW x srcH picture into the framebuffer per the scaling mode;
 * Scale is the factor Fit uses (framebuffer pixels per source pixel). */
void PortLayoutFit(const PortSettings *s, int fbW, int fbH, int srcW, int srcH,
                   int dst[4]);
double PortLayoutScale(const PortSettings *s, int fbW, int fbH, int srcW, int srcH);
/* The HUD's own scale (framebuffer pixels per HUD pixel) for a world picture
 * landing at world[4] = {x, y, w, h}: the setting, or with Auto the scale the
 * world itself has at zoom 1 in this framebuffer -- so zooming changes the
 * world, not the HUD -- and never more than lets the whole 240x160 HUD fit
 * inside the world picture. */
double PortLayoutHudScale(const PortSettings *s, int fbW, int fbH, const int world[4]);
/* The game creates objects 48 px past the 240x160 screen and removes them
 * 48 px past it.  The windows widen with the view, except while a session
 * holds them at the original; then a view wider/taller than this shows
 * objects popping in. */
#define PORT_SPAWN_MARGIN 48
int PortLayoutPastSpawnMargin(int viewW, int viewH);

/* --- what the picture runs on (platform/ui/present.c) -----------------------
 *
 * PortDisplayOptionsAvailable: 1 when the display options (GRAPHICS, SCREEN,
 * ZOOM, CAMERA, PIXELS) do something on this device, 0 when they cannot --
 * the web page fell back to its 2D canvas (no WebGL2, or the GPU renderer
 * did not start): there the picture is the GBA's 240x160, drawn by the
 * page, whatever the settings say.  The party HUD, name tags and the
 * volume work either way.  For the menus: show those rows as unavailable.
 *
 * PortDisplayStatus: one short line for a bug report -- the build, the
 * renderer in use (GPU / CLASSIC / 2D FALLBACK and why), WebGL2 yes/no and
 * the GPU, the settings schema and whether stored settings were migrated.
 * Plain ASCII, upper case, a few "WHAT: VALUE" lines separated by '\n'
 * (each under 40 characters except the GPU's name, which is cut to fit);
 * static storage, valid until the next call. */
int PortDisplayOptionsAvailable(void);
const char *PortDisplayStatus(void);
/* The host's half of the status, set before the first frame (host_web.c;
 * the native host leaves it and the GL renderer's own description is used).
 * Strings are copied. */
void PortDisplaySetHostInfo(int webgl2, const char *gpu, const char *why2d, const char *build);
/* The host: the GL context was lost (the picture has stopped). */
void PortDisplayNoteContextLost(int lost);

#ifdef __cplusplus
}
#endif

#endif
