/* Save memory on disk.
 *
 * platform/sram.c calls two functions: "load whatever you have for this ROM"
 * and "a byte changed".  In the browser those reach IndexedDB.  Here they
 * reach a file in the platform config directory -- ~/.local/share on Linux,
 * ~/Library/Application Support on macOS, %APPDATA% on Windows, all of which
 * SDL_GetPrefPath already knows.
 *
 * The file is the plain 64 KiB image an emulator reads and writes.  That is
 * not a nicety: the web build can already export and import exactly this, so a
 * save made in the browser can be dropped next to the native binary and picked
 * up, and vice versa.  Nothing is wrapped around it -- no header, no
 * compression, no length prefix.
 *
 * Keyed per ROM
 * -------------
 * The key is the same one web/shell.html computes, byte for byte, so the two
 * builds agree on which save belongs to which image: the four-letter game code
 * at 0xAC, the version byte at 0xBC, the image length, and two FNV-1a hashes of
 * the whole file.  The cartridge header alone is not enough -- every dump of
 * this game shares it -- and a translation patch or a ROM hack is emphatically
 * a different game as far as a save file is concerned.
 *
 * Writing
 * -------
 * Debounced, because WriteSramEx writes-verifies-retries and one in-game save
 * is many calls.  A dirty flag is set by the hook, and PortNativeSaveTick
 * writes it out once the game has been quiet for half a second, or when the
 * process is going away.  The write is to a temporary file which is then
 * renamed over the real one, so a crash mid-write loses the new save rather
 * than the old one.
 */

#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "native.h"

/* C linkage for the 64-bit builds -- see tools/cxxify.py.  Below the includes,
 * so SDL's headers stay outside the block. */
#ifdef __cplusplus
extern "C" {
#endif

#define SAVE_QUIET_MS 500

static char sPath[1024];
static char sTmpPath[1024 + 8];
static int  sHaveKey;
static int  sDirty;
static Uint32 sDirtyAt;

/* --- which ROM is this? --------------------------------------------------
 *
 * Kept identical to identify() in web/shell.html.  If you change one, change
 * the other, or the two builds stop sharing saves. */

static void Ascii(const u8 *bytes, size_t from, size_t to, size_t len,
                  char *out, size_t outSize)
{
    size_t i, n = 0;

    for (i = from; i < to && i < len && n + 1 < outSize; i++) {
        u8 c = bytes[i];

        if (c >= 0x20 && c < 0x7F)
            out[n++] = (char)c;
    }
    /* Header fields are space- or NUL-padded. */
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t'))
        n--;
    out[n] = '\0';
}

static void RomKey(const u8 *rom, size_t size, char *out, size_t outSize)
{
    u32 a = 0x811C9DC5u, b = 0x84222325u;
    char code[8];
    size_t i;

    for (i = 0; i < size; i++) {
        a = (a ^ rom[i]) * 16777619u;
        b = (b ^ rom[i]) * 2166136261u;
    }

    Ascii(rom, 0xAC, 0xB0, size, code, sizeof(code));
    if (code[0] == '\0')
        strcpy(code, "????");

    snprintf(out, outSize, "%s-%u-%lx-%08x%08x", code,
             (unsigned)(size > 0xBC ? rom[0xBC] : 0),
             (unsigned long)size, (unsigned)a, (unsigned)b);
}

/* --- the file ------------------------------------------------------------ */

void PortNativeSaveInit(const u8 *rom, size_t romSize, const char *romPath)
{
    char key[128];
    char *pref;
    char title[16];

    (void)romPath;

    pref = SDL_GetPrefPath("katam-port", "katam-port");
    if (pref == NULL) {
        PortError("[katam-port] no writable config directory (%s) -- progress "
                  "will not be kept", SDL_GetError());
        return;
    }

    RomKey(rom, romSize, key, sizeof(key));
    snprintf(sPath, sizeof(sPath), "%s%s.sav", pref, key);
    snprintf(sTmpPath, sizeof(sTmpPath), "%s.tmp", sPath);
    SDL_free(pref);
    sHaveKey = 1;

    Ascii(rom, 0xA0, 0xAC, romSize, title, sizeof(title));
    PortLog("[katam-port] save file: %s  (%s)", sPath,
            title[0] ? title : "unknown cartridge");
}

int PortSramLoad(u8 *dest, u32 size)
{
    SDL_RWops *f;
    Sint64 n;

    if (!sHaveKey)
        return 0;

    f = SDL_RWFromFile(sPath, "rb");
    if (f == NULL)
        return 0;

    n = SDL_RWread(f, dest, 1, size);
    SDL_RWclose(f);

    if (n <= 0)
        return 0;
    /* A short file is still a save: emulators trim trailing zeros and some
     * write only the 32 KiB the game actually uses.  The rest was memset to
     * zero by PortMemInit, which is what a blank region reads as anyway. */
    if ((u32)n < size)
        PortLog("[katam-port] save file is %ld bytes, expected %u -- the rest "
                "reads as blank", (long)n, (unsigned)size);
    return 1;
}

void PortSramMarkDirty(void)
{
    if (!sHaveKey)
        return;
    sDirty = 1;
    sDirtyAt = SDL_GetTicks();
}

void PortNativeSaveFlush(void)
{
    SDL_RWops *f;

    if (!sDirty || !sHaveKey)
        return;
    sDirty = 0;

    f = SDL_RWFromFile(sTmpPath, "wb");
    if (f == NULL) {
        PortError("[katam-port] cannot write %s (%s) -- progress is not being "
                  "kept", sTmpPath, SDL_GetError());
        sHaveKey = 0;           /* say it once, not sixty times a second */
        return;
    }
    SDL_RWwrite(f, (const void *)GBA_SRAM_BASE, 1, GBA_SRAM_SIZE);
    SDL_RWclose(f);

    /* rename(2) over the live file: atomic on every platform that matters, so
     * an interrupted write cannot leave a half-save behind.  Windows needs the
     * destination gone first, which remove() handles and which is why this is
     * not simply rename(). */
    remove(sPath);
    if (rename(sTmpPath, sPath) != 0)
        PortError("[katam-port] could not replace %s", sPath);
}

void PortNativeSaveTick(void)
{
    if (sDirty && SDL_GetTicks() - sDirtyAt >= SAVE_QUIET_MS)
        PortNativeSaveFlush();
}

#ifdef __cplusplus
}
#endif
