/* The device side of an online session (platform/lmpnet.c).
 *
 * Two devices in a session simulate the same frames from the same image and
 * must hold byte-identical memory.  Whatever about *this* device the
 * simulation can see has to be taken out of its reach for the session's
 * length, and put back after.  tools/netaudit measured each of these (the
 * "in a session" rows of its matrix show them gone):
 *
 *   sound     the mixer runs against a canonical device -- fixed rate, fixed
 *             block, every frame, device or none (platform/audio.c);
 *   view      the page's widescreen experiment (PortSetViewMode) moves the
 *             camera clamp and the object windows: held at the hardware's
 *             view, the request kept for afterwards (platform/view.c);
 *   save      a guest's world is the host's save: its own stored save is
 *             neither loaded over the host's image nor overwritten by it
 *             (platform/sram.c);
 *   rando     the randomizer's settings are the host's (they travel in the
 *             image); the page's own wait for the session's end
 *             (platform/rando.c);
 *   rooms     a playtest room bundle is not reloaded mid-session
 *             (platform/res.c).
 *
 * The rest of the session's device-independence lives where the state is:
 * the session view (PortSimViewUseSession, in PortLnStart), the pause page's
 * settings copy (platform/menu_poc.c), AudioTopUp held off while
 * PortLnActive (platform/web/host_web.c). */

#include "port/port.h"
#include "port/backend.h"
#include "port/audio.h"
#include "port/lmpnet.h"

#ifdef __cplusplus
extern "C" {
#endif

void PortSramSetRestored(void);

void PortLnDeviceBegin(int role)
{
    PortAudioSessionBegin();
    PortViewSessionBegin();
    if (role == PORT_LN_GUEST) {
        PortSramSetRestored();
        PortSramPersistBlock();
    }
}

void PortLnDeviceEnd(void)
{
    PortAudioSessionEnd();
    PortViewSessionEnd();
    PortRandoSessionEnd();
}

#ifdef __cplusplus
}
#endif
