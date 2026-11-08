
#ifdef __cplusplus
extern "C" {
#endif

#include "input_recorder.h"
#include "task.h"
#include "malloc_ewram.h"
#include "main.h"

#define TAPE_LENGTH 0x800

/* The module was simply never used in the game. Used in Sonic Advance 2. */

static void Task_InputRecorder(void);
static void InputRecorderEject(struct Task *);

/* PORT-FORK: both in the port sim block (port/simblock.h); the tape pointer
 * is an EWRAM address, so it is the same four bytes on every host. */
#define sTapeBuffer (*(PTR32(u16) *)(uintptr_t)(PORT_SIM_BASE + PORT_SIM_GAME_TAPEBUF))

void InputRecorderResetRecordHead(void) {
    gInputRecorder.recordHead = 0;
}

void InputRecorderResetPlaybackHead(void) {
    gInputRecorder.playbackHead = 0;
}

void InputRecorderLoadTape(void) {
    gInputRecorder.mode = RECORDER_DISABLED;
    sTapeBuffer = EwramMalloc(TAPE_LENGTH * sizeof(u16));
    // Load the playback tape into the recorder
    LZ77UnCompWram(gInputPlaybackData, sTapeBuffer);
    TaskCreate(Task_InputRecorder, 0, 0x2000, TASK_USE_IWRAM, InputRecorderEject); // Only emit the callback functions. No space needed
    gInputRecorder.recordHead = 0;
    gInputRecorder.playbackHead = 0;
}

u16 InputRecorderRead(void) {
    if (gInputRecorder.playbackHead < TAPE_LENGTH)
        return sTapeBuffer[gInputRecorder.playbackHead++];
    return 0;
}

void InputRecorderWrite(u16 input) {
    if (gInputRecorder.recordHead < TAPE_LENGTH) {
        sTapeBuffer[gInputRecorder.recordHead++] = input;
    }
}

static void Task_InputRecorder(void) {
    // Potentially generate input ??
}

static void InputRecorderEject(struct Task *unused __attribute__((unused))) {
    EwramFree(sTapeBuffer);
    gInputRecorder.mode = RECORDER_DISABLED;
    gInputRecorder.recordHead = 0;
    gInputRecorder.playbackHead = 0;
}

#ifdef __cplusplus
}
#endif
