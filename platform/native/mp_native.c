/* The native counterpart of platform/web/mp_js.c.
 *
 * There is no page here to supply a transport, so PortMpUseJs has nothing to
 * attach and says so by returning NULL.  A native front end registers a C
 * transport through PortMpAttach directly -- see docs/MULTIPLAYER.md.
 */

#include "port/mp.h"

struct PortMpTransport *PortMpJsTransport(void)
{
    return 0;
}
