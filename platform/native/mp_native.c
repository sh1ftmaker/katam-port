/* The native counterpart of platform/web/mp_js.c.
 *
 * There is no page here to supply a transport, so PortMpUseJs has nothing to
 * attach and says so by returning NULL.  A native front end registers a C
 * transport through PortMpAttach directly -- see docs/MULTIPLAYER.md.
 */

#include "port/mp.h"

/* C linkage for the 64-bit builds -- see tools/cxxify.py.  Below the includes,
 * so SDL's headers stay outside the block. */
#ifdef __cplusplus
extern "C" {
#endif

struct PortMpTransport *PortMpJsTransport(void)
{
    return 0;
}

#ifdef __cplusplus
}
#endif
