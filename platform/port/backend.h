#ifndef GUARD_PORT_BACKEND_H
#define GUARD_PORT_BACKEND_H
/* C linkage for the 64-bit builds.
 *
 * They compile the game as C++ so that its structures keep 4-byte pointer
 * members (docs/SIXTYFOUR.md), and tools/cxxify.py gives every game header and
 * source C linkage so the C++ build links by the same rules the C builds do.
 * This header declares the seam between the two, so it has to say the same
 * thing -- otherwise the game calls a mangled name and the platform defines an
 * unmangled one, or the reverse.  A no-op in C. */
#ifdef __cplusplus
extern "C" {
#endif


#include "gba/types.h"

/* Everything the platform layer needs from whatever is hosting it.
 *
 * platform/*.c is the GBA: a scanline PPU, DMA channels, the BIOS calls, the
 * m4a mixer, the frame loop.  None of it knows whether it is running in a
 * browser tab or in a window on a desktop.  This header is the whole of that
 * difference -- fourteen functions, implemented twice:
 *
 *   platform/web/     emscripten.  EM_JS bodies that reach into the page.
 *   platform/native/  SDL2, on top of a small per-OS memory seam.
 *
 * If you are adding a third host, this is the list.  Nothing outside
 * platform/web and platform/native is allowed to know which one is present.
 *
 * (Adding a third *operating system* to the native build is a much smaller
 * job: see platform/native/native.h, which is five functions.)
 */

/* --- startup -------------------------------------------------------------
 *
 * Called from main() before anything else, with the process arguments.
 *
 * On the web this does nothing: the page has already set the module up by the
 * time main runs.  Natively it is where the command line is parsed, the GBA
 * memory map is reserved at its true addresses, and SDL is brought up -- in
 * that order, because PortMemInit memsets regions that must exist first. */
void PortHostInit(int argc, char **argv);

/* --- console -------------------------------------------------------------
 * Where PortLog and PortError end up.  isErr distinguishes "this is wrong"
 * from "this is a note", which matters when the reader is filtering. */
void PortConsole(const char *text, int isErr);

/* --- frame boundary ------------------------------------------------------
 *
 * PortPresentFrame calls these two in order, once a frame.  Blit hands over
 * the finished picture; Await returns when it is time to draw the next one.
 *
 * On the web, Await is an Asyncify suspension over requestAnimationFrame --
 * the wasm stack unwinds and resumes on the next frame.  Natively it is a
 * plain sleep to the GBA's 59.7275 Hz, which is also where SDL's event queue
 * is pumped and PortSetKeys is called. */
/* Pixel composition, on or off.  State is unaffected -- see PortRenderFrame
 * in platform/ppu.c.  For frames that will be re-simulated or discarded. */
void PortSetRenderEnabled(int on);
int  PortRenderEnabled(void);

void PortBlitFramebuffer(const u32 *pixels, int w, int h);
void PortAwaitAnimationFrame(void);

/* Returns once the player has supplied a ROM: the image is at GBA_ROM_BASE and
 * PortRomLoaded has been called.  Returns without doing either if the player
 * gave up, which main() reports and exits on. */
void PortAwaitRom(void);

/* --- DMA range check -----------------------------------------------------
 *
 * platform/dma.c refuses a transfer whose endpoints are not real memory,
 * because a stale GBA pointer otherwise faults inside the copy loop, frames
 * after whatever queued it.  Addresses inside the GBA map it can judge for
 * itself.  This is the other case: a legitimate endpoint that is *host* data
 * rather than console memory -- DmaFill sources its value from the address of
 * a local, and build/generated/rom_copies.c gives a few ROM arrays real
 * storage in the binary's .bss.
 *
 * Return 1 only if [addr, addr+len) is memory this process can actually touch
 * and is not part of the reserved GBA map.  Guessing "yes" here is how a wild
 * pointer gets through; guessing "no" is how a real transfer gets dropped, and
 * that is the bug that once hid every level tilemap. */
int PortHostRangeOk(uintptr_t addr, u32 len);
int PortDmaTracing(void);

/* --- who issued this transfer? -------------------------------------------
 *
 * The DMA log says two builds moved different bytes.  It cannot say why: a
 * transfer carries no record of the code that asked for it, because the game
 * writes four registers and the port copies memory.  When two streams diverge
 * at a transfer whose source is a *valid* address in both builds -- ROM in one
 * and EWRAM in the other, same destination, same count -- the question stops
 * being "which pointer is wrong" and becomes "which branch was taken".  That
 * is a question about the call stack, and nothing that hashes memory can
 * answer it.
 *
 * PORT_DMA_STACK=<lo>:<hi> prints the call stack for traced transfers lo..hi.
 * The two hosts spell a stack differently and neither spelling is the
 * interesting part -- the *game* function names are, and both can supply them:
 *
 *   web      emscripten_get_callstack reads the engine's frames and names them
 *            from the wasm name section.  --profiling-funcs is already on, so
 *            names arrive directly.
 *   native   backtrace() gives return addresses; the binary is linked -no-pie
 *            at a fixed text address, so they resolve offline against `nm`.
 *
 * tools/symbolize_stack.py turns either form into the same list of decompiled
 * C names.  That is the symbol-normalising comparison the state hasher wanted,
 * applied to control flow rather than to memory -- where it is far cheaper,
 * because a stack is twenty frames and EWRAM is 256 KiB of which any word
 * might be a function pointer. */
u32 PortFrameNumber(void);
int PortDmaStackWanted(u32 transfer);
void PortCallStack(const char *tag);
/* The web build is linked -sENVIRONMENT=web, so emscripten's getenv never sees
 * node's process.env and every environment-driven switch in this file needs an
 * explicit entry point beside it.  Turns DMA tracing on as a side effect,
 * since asking for a stack without the line it belongs to is useless. */
void PortSetDmaStack(long lo, long hi);

/* The GBA BIOS ROM, 16 KiB at address zero.
 *
 * In wasm that is ordinary low linear memory: it reads as zero and a transfer
 * naming it is harmless.  A native process cannot have it -- every OS reserves
 * page zero (Linux via vm.mmap_min_addr, macOS via __PAGEZERO) and refuses to
 * map anything there.  So natively the region is not present, and dma.c treats
 * a transfer that names it as leaving the map: reported once and skipped,
 * which is what the hardware's open bus amounts to anyway.  The alternative is
 * a segfault. */
#ifdef __EMSCRIPTEN__
#define PORT_BIOS_REGION_SIZE 0x4000u
#else
#define PORT_BIOS_REGION_SIZE 0u
#endif

/* --- audio transport -----------------------------------------------------
 *
 * The producer side is in platform/audio.c and platform/audio_out.c and is
 * shared: m4a renders exactly one VBlank's worth of samples and pushes them,
 * so everything here is push-driven with a queue underneath.  What differs is
 * only where the queue lives -- an AudioWorklet's ring in the browser, an
 * SDL callback's ring natively.
 *
 * PortAudioOpen must be safe to call twice; the second call does nothing.
 * PortAudioRate returns the device's real rate, or 0 for "no audio at all",
 * which the rest of the port treats as a supported configuration. */
void PortAudioOpen(int ringFrames);
int  PortAudioRate(void);
/* Frames handed over but not yet played.  Used for clock feedback, so a
 * slightly stale answer is fine -- but it must never read zero while audio is
 * queued, or the producer widens every block and drifts long. */
int  PortAudioQueued(void);
int  PortAudioUnderruns(void);
/* One block of interleaved stereo float, `frames` frames long. */
void PortAudioSubmit(const float *samples, int frames);

/* --- save memory ---------------------------------------------------------
 *
 * A cartridge keeps its save with the console switched off.  Neither a tab nor
 * a process does, so the host owns the storage: IndexedDB in the browser, a
 * 64 KiB .sav file in the platform config directory natively.
 *
 * PortSramLoad fills `dest` with the stored save and returns 1, or returns 0
 * if there is none for this ROM.  It is called the first time the *game*
 * touches save memory, not at startup -- see the ordering note at the top of
 * platform/sram.c, which is load-bearing.
 *
 * PortSramMarkDirty says a byte changed.  The host debounces. */
int  PortSramLoad(u8 *dest, u32 size);
void PortSramMarkDirty(void);


#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_BACKEND_H */
