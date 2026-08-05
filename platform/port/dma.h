#ifndef GUARD_PORT_DMA_H
#define GUARD_PORT_DMA_H
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

/* The decomp reaches DMA through the DmaSet macro in include/gba/macro.h,
 * which writes the source, destination and control registers and relies on the
 * hardware to act on the write.  portify.py redirects that macro here.
 *
 * `control` is the packed 32-bit value the macro builds: flags in the high
 * half, transfer count in the low half. */
void PortDmaSet(int channel, const void *src, void *dest, u32 control);
void PortDmaStop(int channel);

/* Run any channel armed for HBlank on the given scanline.  Called by the PPU;
 * this is how the game's per-scanline scroll and window effects happen. */
void PortDmaHBlank(int line);

/* Run any channel armed for VBlank. */
void PortDmaVBlank(void);


#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_DMA_H */
