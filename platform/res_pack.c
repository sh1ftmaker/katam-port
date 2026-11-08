/* placeholder: the pack compiler lands in M1 */
#include "port/port.h"
#include "res_internal.h"
int ResPackInstall(const char *packDir, const char *const *mods, u32 numMods)
{
    (void)packDir; (void)mods; (void)numMods;
    PortLog("[res] no pack compiler in this build");
    return -1;
}
