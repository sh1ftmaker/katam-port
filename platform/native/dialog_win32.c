/* "Which ROM?", on Windows.
 *
 * The POSIX side shells out to zenity or kdialog because a desktop Unix has no
 * file dialog of its own and SDL2 has none either (SDL3 grew
 * SDL_ShowOpenFileDialog).  Windows does: GetOpenFileName has been in
 * comdlg32 since 3.1, and it is one call.
 *
 * Three details that are not obvious:
 *
 *   The W entry point, not the A one.  SDL's file functions take UTF-8 paths
 *   on Windows and convert internally to UTF-16; GetOpenFileNameA would hand
 *   back a path in the system ANSI code page, which round-trips through
 *   SDL_RWFromFile as mojibake for any player whose ROM lives under a name the
 *   code page cannot spell.  So: ask for wide, convert to UTF-8 here, and the
 *   two agree.
 *
 *   OFN_NOCHANGEDIR.  Without it the dialog leaves the process's current
 *   directory wherever the player was browsing.  Everything in this port that
 *   writes a relative path afterwards -- F12 screenshots, --screenshot,
 *   --window-shot -- would then land somewhere the player did not choose and
 *   the log line would name a file that is not where it says it is.
 *
 *   The filter is a run of NUL-separated pairs ending in a double NUL, which
 *   is why it is a wide array with embedded \0 rather than a plain string.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <stddef.h>
#include <string.h>

#include <windows.h>
#include <commdlg.h>

#include "native.h"

int PortHostPickRomFile(char *out, size_t outSize)
{
    /* "label\0pattern\0label\0pattern\0\0" -- the literal supplies the last
     * NUL, so the visible text ends with one and the array ends with two. */
    static const WCHAR kFilter[] =
        L"Game Boy Advance ROM\0*.gba;*.agb;*.bin\0All files\0*.*\0";
    static const WCHAR kTitle[] =
        L"Choose your Kirby & The Amazing Mirror ROM";

    OPENFILENAMEW ofn;
    WCHAR path[MAX_PATH];
    int n;

    if (out == NULL || outSize < 2)
        return 0;

    memset(&ofn, 0, sizeof(ofn));
    memset(path, 0, sizeof(path));

    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = NULL;    /* the SDL window is not needed; a NULL owner
                                   gives a dialog the task bar owns, which is
                                   what a player expects from a launcher */
    ofn.lpstrFilter  = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile    = path;
    ofn.nMaxFile     = (DWORD)(sizeof(path) / sizeof(path[0]));
    ofn.lpstrTitle   = kTitle;
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER
                     | OFN_NOCHANGEDIR;

    /* FALSE is both "the player cancelled" and "the dialog failed".  Neither
     * is an error here: PortAwaitRom falls back to drag-and-drop, which needs
     * nothing from the operating system at all. */
    if (!GetOpenFileNameW(&ofn))
        return 0;

    n = WideCharToMultiByte(CP_UTF8, 0, path, -1, out, (int)outSize, NULL, NULL);
    if (n <= 0) {
        out[0] = '\0';
        return 0;
    }
    return out[0] != '\0';
}
