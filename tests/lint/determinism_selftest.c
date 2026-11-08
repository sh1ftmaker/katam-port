/* Self-test for tools/lint_determinism.py (make lint-determinism): every line
 * marked 'flag' must be reported and no other.  Not part of any build. */
#include "global.h"
#include "random.h"
#include "task.h"
void g(u32, u32);
static u32 adv(void) { return Rand16(); }
static u32 wrap(void) { return adv() & 3; }
void t1(void) { g(Rand16(), Rand16()); }                 /* flag */
void t2(void) { u32 x = Rand16() + wrap(); g(x, 0); }    /* flag: via call graph */
void t3(void) { g(gRngVal, Rand32()); }                  /* flag: read vs write */
void t4(void) { u32 a = Rand16(), b = Rand16(); g(a, b); } /* ok */
void t5(void) { g(Rand16() && Rand16(), 0); }            /* ok: && */
void t6(void) { g((u32)TaskCreate(0,0,0,0,0), (u32)TaskCreate(0,0,0,0,0)); } /* flag heap */
void t7(void) { u32 v[2] = { Rand16(), Rand16() }; g(v[0], v[1]); } /* flag: init list */
