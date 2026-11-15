/* The LIGHTING section of a settings list screen, as a model.
 *
 * A list of rows over the lighting settings in a PortSettings
 * (platform/ui/settings.h; the lighting itself: platform/render/light.c).
 * The screen that shows it owns everything visual -- cursor, scrolling,
 * dimming, fonts -- and calls these for what to draw and what Left/Right/A
 * do.  Nothing here draws, reads input, or touches game memory.
 *
 * Text: the names are UPPER CASE, at most LIGHT_OPT_NAME_MAX characters;
 * the descriptions and values use only A-Z, 0-9, space and '%' (as the
 * settings screen's VOLUME row does), descriptions at most
 * LIGHT_OPT_DESC_MAX characters, values at most LIGHT_OPT_VALUE_MAX.
 *
 * Which device: the value and step functions take the PortSettings to work
 * on, and availability takes `host`, this device's lighting capabilities
 * (LightOptHost).  A screen on the host's own settings passes
 * &gPortSettings and LightOptHost(), and after a step that returns 1 calls
 * PortSettingsClamp + PortSettingsChanged (apply and save) -- or uses the
 * LightOptStepLive shortcut, which does exactly that.  A screen drawn from
 * inside the simulation (where every device of an online game must draw the
 * same thing) can pass a copy of the settings and LIGHT_OPT_HOST_ALL, so
 * nothing it draws depends on this device's GPU; the lighting is still each
 * device's own presentation and simply does not run where it cannot.
 *
 * Rows: LIGHTING (on/off) first, then the engine, a style preset, the light
 * source, strength and shadows, relief and shine, the sun, character
 * shadows, colour, indoor light and ground, the radiance cascades' bounce,
 * and RESET LIGHTING (an action: A or Left/Right runs it, value "").  Every
 * lighting setting is reachable.  A row is unavailable (draw it dimmed; a
 * step on it does nothing) when the GPU renderer is not in use, when the
 * lighting could not start on this GPU, when lighting is off (every row but
 * LIGHTING itself), or when the current engine, source or switches leave
 * the row without effect.  LightOptDesc then says why.
 */
#ifndef GUARD_PORT_UI_LIGHT_OPTS_H
#define GUARD_PORT_UI_LIGHT_OPTS_H

#include "ui/settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LIGHT_OPT_NAME_MAX 14
#define LIGHT_OPT_DESC_MAX 40
#define LIGHT_OPT_VALUE_MAX 12

/* This device: the GPU renderer is drawing; the lighting's shaders can run
 * (not failed); radiance cascades can run. */
#define LIGHT_OPT_HOST_GPU 1
#define LIGHT_OPT_HOST_OK  2
#define LIGHT_OPT_HOST_RC  4
#define LIGHT_OPT_HOST_ALL (LIGHT_OPT_HOST_GPU | LIGHT_OPT_HOST_OK | LIGHT_OPT_HOST_RC)
int LightOptHost(void);

int LightOptCount(void);
/* Row names and one-line descriptions; "" for a row out of range. */
const char *LightOptName(int row);
/* The row's description, or while it is unavailable the reason
 * ("NEEDS THE ENHANCED GRAPHICS", "TURN LIGHTING ON FIRST", ...). */
const char *LightOptDesc(const PortSettings *s, int row, int host);
int LightOptAvailable(const PortSettings *s, int row, int host);
/* 1 for RESET LIGHTING: no value; A runs it. */
int LightOptIsAction(int row);
/* The value as short text ("ON", "CLASSIC", "200%", "35 LEFT", "16 PX"). */
void LightOptValue(const PortSettings *s, int row, char *buf, int size);
/* Left (-1) / Right (+1), or A on an action row (pass +1).  Lists wrap,
 * numbers stop at their ends.  Returns 1 if *s changed.  Does not check
 * availability: the screen does (LightOptAvailable) before stepping. */
int LightOptStep(PortSettings *s, int row, int dir);
/* On gPortSettings with LightOptHost(): if available, step, clamp, apply
 * and save (PortSettingsChanged).  Returns 1 if anything changed. */
int LightOptStepLive(int row, int dir);

#ifdef __cplusplus
}
#endif

#endif
