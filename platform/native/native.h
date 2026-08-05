#ifndef GUARD_PORT_NATIVE_H
#define GUARD_PORT_NATIVE_H

#include <stddef.h>

#include "port/port.h"
#include "port/backend.h"

/* The native build, and the five functions that differ between operating
 * systems.
 *
 * Everything else here -- the window, the renderer, input, gamepads, audio,
 * frame pacing, the save file, the command line, the screenshot writer -- is
 * SDL2 and portable C, shared by every desktop platform.  If you are adding
 * Windows, macOS or a new CPU, what you have to write is the two files below
 * and nothing more.  See docs/NATIVE.md.
 *
 *   mem_posix.c    -> mem_win32.c     the memory reservation
 *   dialog_posix.c -> dialog_win32.c  the "which ROM?" file picker
 */

/* --------------------------------------------------------------------------
 * Host memory
 *
 * The port needs the GBA's regions at their true addresses -- EWRAM really at
 * 0x02000000, VRAM at 0x06000000, the ROM at 0x08000000.  In wasm the linker
 * gives that away for free (-sGLOBAL_BASE puts everything the compiler owns
 * above the map).  A native process has to take the addresses itself, before
 * anything else in the process is tempted to use them.
 *
 * platform/native/mem.c owns the region table and the policy: what to reserve,
 * in what order, how to round it, and how to prove afterwards that it worked.
 * These three are the only parts that are an operating system's business.
 * ------------------------------------------------------------------------ */

/* The system's allocation granularity, in bytes: what a reservation's size has
 * to be a multiple of.  4096 on x86-64 Linux; 16384 on Apple silicon and on
 * many arm64 Linux kernels; 65536 on Windows (VirtualAlloc's reservation
 * granularity, which is coarser than its page size and is the number that
 * matters here).
 *
 * Every base address in the GBA map is 64 KiB aligned, so no plausible value
 * changes where anything lands -- only how far each reservation is rounded
 * up.  Do not hardcode it. */
size_t PortHostPageSize(void);

/* Reserve [addr, addr+size) as readable, writable, private, zero-filled
 * memory.  addr and size are already rounded to PortHostPageSize().
 *
 * Return 0 on success, -1 on failure.  Failure must mean "these addresses were
 * not available", never "they were available and I gave you somewhere else":
 * mmap without MAP_FIXED silently relocates, and a relocated GBA map is a game
 * whose every pointer is wrong in a way nothing detects.  Use
 * MAP_FIXED_NOREPLACE (Linux 4.17+), or VirtualAlloc, which already refuses to
 * move.  Do not use bare MAP_FIXED: it unmaps whatever was there instead of
 * reporting the collision.
 *
 * Set *why to a short human-readable reason on failure, or leave it alone. */
int PortHostReserve(uintptr_t addr, size_t size, const char **why);

/* Is [addr, addr+len) memory this process can actually touch?
 *
 * Used by PortHostRangeOk (platform/native/mem.c) to tell one of the port's
 * own C pointers -- the address of a local that DmaFill is filling from, say --
 * from a stale GBA pointer that would fault.  Getting this wrong in the
 * permissive direction lets a wild pointer through to a segfault; getting it
 * wrong in the strict direction silently drops real transfers, which is the
 * bug that once hid every level tilemap in the game.
 *
 * A guess is not good enough: ask the kernel.  msync() and mincore() both
 * report ENOMEM for an unmapped range on Linux and macOS; VirtualQuery reports
 * MEM_FREE on Windows.  This is only reached for endpoints outside the GBA
 * map, which in a normal frame means a handful of calls, so a syscall is
 * affordable -- and mem.c memoises the last answer anyway. */
int PortHostAddrValid(uintptr_t addr, size_t len);

/* --------------------------------------------------------------------------
 * Host file dialog
 *
 * Optional.  Return 0 if this platform has no picker; the caller then falls
 * back to "drop a ROM on the window", which every platform has.
 * ------------------------------------------------------------------------ */

/* Ask the player for a ROM.  Writes an absolute path into out and returns 1,
 * or returns 0 if the player cancelled or there is no dialog available. */
int PortHostPickRomFile(char *out, size_t outSize);

/* --------------------------------------------------------------------------
 * Shared native internals.  Not a seam -- these are one implementation each.
 * ------------------------------------------------------------------------ */

/* mem.c */
void PortNativeReserveMap(void);          /* called from PortHostInit */
void PortNativeReportMap(void);           /* --verbose: what actually landed */

/* audio_sdl.c */
void PortNativeAudioClose(void);

/* save_file.c */
void PortNativeSaveInit(const u8 *rom, size_t romSize, const char *romPath);
void PortNativeSaveFlush(void);           /* write now if dirty */
void PortNativeSaveTick(void);            /* once a frame: honour the debounce */

/* png.c -- writes a PNG with no external dependency; see the file. */
int  PortNativeWritePng(const char *path, const u32 *rgba, int w, int h);

/* host_sdl.c */
extern int gPortNativeVerbose;
extern int gPortNativeNoAudio;

#endif /* GUARD_PORT_NATIVE_H */
