/* The online game's notices over the play picture: "WAITING FOR HOST 0:30"
 * on a guest whose host has gone quiet, "RECONNECTING 0:30" on a host whose
 * every guest has (web/online.js gameNotice).
 *
 * Made of the game's own pictures at run time, from the player's ROM, as the
 * ONLINE screens' messages are: the menus' banner and lettering
 * (platform/menu_poc.c PortPocNoticeCompose), in the label's own colours,
 * turned into RGBA here for the page to put on a canvas over the picture.
 * The page draws it, not the game's video: while it shows, the game holds
 * (no frames run, nothing is presented), and the notice is this device's
 * alone.  Host memory only -- nothing a session saves, rewinds or hashes,
 * nothing from the ROM kept past the next notice, nothing stored. */

#include <string.h>

#include "global.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define NOTICE_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define NOTICE_EXPORT
#endif

int PortPocNoticeCompose(const char *s, u8 *px, int w, int h, u16 pal[16]);   /* platform/menu_poc.c */

#define NOTICE_W 320
#define NOTICE_H 24

static u8 sIdx[NOTICE_W * NOTICE_H];
static u32 sRgba[NOTICE_W * NOTICE_H];
static int sW;

/* The notice for s (upper case, the fallback font's letters, digits, ':');
 * returns its width in pixels (NOTICE_H tall), 0 when it cannot be made. */
NOTICE_EXPORT int PortOnlineNoticeBuild(const char *s)
{
    u16 pal[16];
    int x, y;

    sW = 0;
    if (s == NULL)
        return 0;
    sW = PortPocNoticeCompose(s, sIdx, NOTICE_W, NOTICE_H, pal);
    if (sW <= 0)
        return sW = 0;
    for (y = 0; y < NOTICE_H; y++)
        for (x = 0; x < sW; x++) {
            u8 i = sIdx[y * NOTICE_W + x];
            u16 c = pal[i & 15];
            u32 r = (c & 31) * 255 / 31, g = ((c >> 5) & 31) * 255 / 31, b = ((c >> 10) & 31) * 255 / 31;

            /* colour 0 is the sprite's transparent one */
            sRgba[y * sW + x] = i ? (0xFFu << 24) | (b << 16) | (g << 8) | r : 0;
        }
    return sW;
}

NOTICE_EXPORT int PortOnlineNoticeHeight(void) { return sW ? NOTICE_H : 0; }

/* sW x NOTICE_H RGBA bytes, rows packed */
NOTICE_EXPORT const u32 *PortOnlineNoticePixels(void) { return sRgba; }
