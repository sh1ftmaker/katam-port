#ifndef GUARD_PORT_RES_JSON_H
#define GUARD_PORT_RES_JSON_H

/* A small JSON reader for the resource layer: the manifest, pack files and
 * mod files.  Integers only (a float is a parse error -- pack JSON is
 * canonical, MODDING.md §2.1), strings without \u escapes beyond ASCII,
 * objects keep their key order.  One allocation per value; JFree frees all. */

#include <stddef.h>
#include "gba/types.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { J_NULL, J_FALSE, J_TRUE, J_INT, J_STR, J_ARR, J_OBJ };

typedef struct JVal {
    int type;
    long long num;          /* J_INT */
    char *str;              /* J_STR */
    u32 n;                  /* J_ARR / J_OBJ: number of items */
    struct JVal **items;    /* J_ARR / J_OBJ */
    char **keys;            /* J_OBJ */
} JVal;

/* NULL on a syntax error; *err (if given) says where. */
JVal *JParse(const char *text, size_t len, char *err, size_t errLen);
void  JFree(JVal *v);
const JVal *JGet(const JVal *obj, const char *key);   /* NULL if absent */
const JVal *JAt(const JVal *arr, u32 i);              /* NULL if out of range */
/* Convenience: value of a key, or `dflt` if absent or of the wrong type. */
long long   JInt(const JVal *obj, const char *key, long long dflt);
const char *JStr(const JVal *obj, const char *key);   /* NULL if absent */

/* Structured edits, for mod patches (MODDING.md §3.2).  `path` is a
 * JSON-pointer-like "a/2/b" ("" is the root).  JSet replaces an existing
 * value; JMerge merges an object into the one at `path` (RFC 7396 without
 * deletion: objects recurse, anything else replaces, new keys are added).
 * Both deep-copy `value`.  0 on success. */
JVal *JClone(const JVal *v);
int   JSet(JVal *root, const char *path, const JVal *value);
int   JMerge(JVal *root, const char *path, const JVal *value);

/* A growable byte buffer, for writing canonical JSON and TOML. */
typedef struct Buf { char *p; size_t len, cap; } Buf;
void BufPut(Buf *b, const void *data, size_t n);
void BufPrintf(Buf *b, const char *fmt, ...);
void BufFree(Buf *b);

#ifdef __cplusplus
}
#endif

#endif
