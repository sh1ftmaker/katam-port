#ifndef GUARD_TASK_H
#define GUARD_TASK_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"

struct Task;

typedef void (*TaskMain)(void);
typedef void (*TaskDestructor)(struct Task*);

#define MAX_TASK_NUM                0x80

#define TASK_USE_IWRAM              0x0000
#define TASK_INACTIVE               0x0001
#define TASK_DESTROY_DISABLED       0x0002
#define TASK_x0004                  0x0004
#define TASK_x0008                  0x0008 // deprecated in current task.c
#define TASK_USE_EWRAM              0x0010 // when it fails, use the other work ram instead. 

struct Task {
    /* 0x00 */ u16 parent; // the current task on creation
    /* 0x02 */ u16 prev; 
    /* 0x04 */ u16 next;
    /* 0x06 */ u16 structOffset;
    /* 0x08 */ PTR32_TD(TaskMain) main;
    /* 0x0C */ PTR32_TD(TaskDestructor) dtor;
    /* 0x10 */ u16 priority; // lower value => higher priority
    /* 0x12 */ u16 flags;
};

struct IwramNode {
    u16 next;
    s16 state;
    u8 space[0];
};

#ifdef PORT_CHECK_POINTERS
struct Task;
void *PortTaskStruct(struct Task *);
#define TaskGetStructPtr(taskp) PortTaskStruct(taskp)
#else
#define TaskGetStructPtr(taskp) \
    ((taskp)->flags & TASK_USE_EWRAM \
    ? (void *)EWRAM_START + ((taskp)->structOffset << 2) \
    : (void *)IWRAM_START + (taskp)->structOffset)
#endif

/* PORT: replaced by port/ram_symbols.h -- extern struct Task gTasks[]; */
/* PORT: replaced by port/ram_symbols.h -- extern struct Task gEmptyTask; */
/* PORT: replaced by port/ram_symbols.h -- extern struct Task* gTaskPtrs[]; */
/* PORT: replaced by port/ram_symbols.h -- extern s32 gNumTasks; */
/* PORT: replaced by port/ram_symbols.h -- extern struct Task* gNextTaskToCheckForDestruction; */
/* PORT: replaced by port/ram_symbols.h -- extern struct Task* gNextTask; */
/* PORT: replaced by port/ram_symbols.h -- extern struct Task* gCurTask; */
/* PORT: replaced by port/ram_symbols.h -- extern u32 gIwramHeap[0x2604/4]; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gExecSoundMain; */

u32 TasksInit(void);
void TasksExec(void);
struct Task* TaskCreate(TaskMain, u16, u16, u16, TaskDestructor);
void TaskDestroy(struct Task*);
void TasksDestroyInPriorityRange(u16, u16);

#ifdef __cplusplus
}
#endif

#endif
