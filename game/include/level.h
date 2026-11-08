#ifndef GUARD_LEVEL_H
#define GUARD_LEVEL_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"

void InitLevelTasks(void);
void SaveDisplayState(void);
void RestoreDisplayState(void);
void FillLevelInfo(u8 playerId, u16 roomId, const u16 **arg2, const u16 **arg3);

#ifdef __cplusplus
}
#endif

#endif
