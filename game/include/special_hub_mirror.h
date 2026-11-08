#ifndef GUARD_SPECIAL_HUB_MIRROR_H
#define GUARD_SPECIAL_HUB_MIRROR_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

struct SpecialHubMirror
{
    struct Object obj;
    PTR32(struct EffectObject) unkB4;
    u16 unkB8;
    u16 unkBA;
    u16 unkBC;
}; /* size = 0xC0 */

void *CreateSpecialHubMirror(struct ObjectTemplate *, u8);
void nullsub_109(struct SpecialHubMirror *);

#ifdef __cplusplus
}
#endif

#endif // GUARD_SPECIAL_HUB_MIRROR_H
