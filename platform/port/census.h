#ifndef GUARD_PORT_CENSUS_H
#define GUARD_PORT_CENSUS_H
/* The divergence census -- see platform/census.c. */
#include "gba/types.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Start collecting; `out` is written at exit, `masks` (may be NULL) is a
 * tests/census/masks.txt table.  Call before PortSeatInit in census mode. */
int  PortCensusInit(const char *out, const char *masks);
int  PortCensusActive(void);
/* One round: every seat's snapshot at the same frame boundary. */
void PortCensusRound(const u8 *const *states, int n);
/* Native/host hook: arm a census of `seats` seats from the first frame. */
/* sessionFrom < 0: focus-only (gLocalPlayerId differs from the first frame);
 * else session mode from that frame (platform/seats.c, CensusBeginFrame). */
void PortSeatCensusArm(int seats, s32 sessionFrom);
#ifdef __cplusplus
}
#endif
#endif
