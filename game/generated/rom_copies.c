/* ROM data symbols that needed real storage.
 *
 * There used to be one: gLevelObjLists, an array of an anonymous union that
 * gen_rom_data.py could not spell as an address macro, so it was memcpy'd out
 * of the ROM at startup.  Its union now has a name (union LevelObjList,
 * include/data.h) and it is an ordinary resource slot in port/rom_data.h, read
 * straight from the ROM like every other table.  The function stays as the
 * hook main() calls after the resource session is built. */

#include "port/port.h"

#ifdef __cplusplus
extern "C" {
#endif

void PortLoadRomDataCopies(void)
{
}

#ifdef __cplusplus
}
#endif
