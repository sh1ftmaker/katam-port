#ifndef GUARD_INPUT_RECORDER_H
#define GUARD_INPUT_RECORDER_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"

#define RECORDER_DISABLED 0
#define RECORDER_RECORD   1
#define RECORDER_PLAYBACK 2

struct InputRecorder {
    s32 playbackHead;
    s32 recordHead;
    u8 mode;
};

/* PORT-FORK: in the port sim block (port/simblock.h), not host .bss. */
#include "port/simblock.h"
#define gInputRecorder (*(struct InputRecorder *)(uintptr_t)(PORT_SIM_BASE + PORT_SIM_GAME_INPUTREC))

void InputRecorderResetRecordHead(void);
void InputRecorderResetPlaybackHead(void);
void InputRecorderLoadTape(void);
u16 InputRecorderRead(void);
void InputRecorderWrite(u16);

#ifdef __cplusplus
}
#endif

#endif
