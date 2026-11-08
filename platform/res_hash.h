#ifndef GUARD_PORT_RES_HASH_H
#define GUARD_PORT_RES_HASH_H

#include <stddef.h>
#include "gba/types.h"

#ifdef __cplusplus
extern "C" {
#endif

void ResSha1(const void *data, size_t len, u8 out[20]);
void ResSha256(const void *data, size_t len, u8 out[32]);
void ResHex(const u8 *d, size_t n, char *out);   /* out: 2n+1 bytes */

#ifdef __cplusplus
}
#endif

#endif
