#ifndef GUARD_PORT_FNID_H
#define GUARD_PORT_FNID_H

/* Canonical code IDs (netplay-v2 R5; docs/DETERMINISM.md "Code IDs").
 *
 * The game keeps function pointers in its own memory: Task::main,
 * Object2::unk78, the AI's state table, gIntrTable, the sound engine's jump
 * table and callbacks.  A host function pointer is an address in this
 * executable natively and an index into the function table in wasm, so the
 * same game state used to be spelled differently on every host and to move
 * whenever code layout changed -- which is what kept snapshots from being
 * portable and the `sim` hash from being comparable across builds.
 *
 * So no function-pointer *value* the game handles is a host pointer any more.
 * It is the function's code ID, carried in the same pointer type:
 *
 *   - for a function the ROM has, its GBA ROM address with the Thumb bit as
 *     the ROM stores it (katam.elf's symbol value) -- a value the game copies
 *     out of a ROM table is therefore already an ID, and the ROM tables need
 *     no patching;
 *   - for one it does not (port-only code, PORT-FORK replacements, stubs),
 *     0x0E000001 | (fnv1a(name) & 0xFFFFFE): a range no ROM function uses,
 *     fixed by the name alone, collision-checked by the generator.
 *
 *     task->main = FN(sub_0803D21C);        stores 0x0803D21D
 *     if (obj->unk78 == FN(sub_0809F840))   compares IDs
 *     CALL(task->main)(task);               ID -> host function -> call
 *
 * FN(f) is a constant expression of f's own pointer type, so it works in
 * static initialisers and everywhere f did.  CALL(p) looks the ID up and casts
 * the host function back to p's type.  tools/codemod_fnid.py wrote every one
 * of them (and reruns cleanly); `make lint-determinism` fails on a function
 * that decays to a pointer without FN or a call through a pointer without
 * CALL.  An ID nobody registered -- a host pointer that got in, a corrupt
 * value, a ROM function the port does not have -- is reported with the value
 * and the call site, and aborts (platform/fnid.c: continuing would run the
 * wrong code or desync silently).
 *
 * Host code (platform/) keeps real function pointers for host things.  Where
 * it calls a game-stored pointer (interrupt dispatch, the mixer) it uses CALL;
 * where it hands a function to the game it hands FN(f). */

#include <stdint.h>
#include "fnid_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PortFn)(void);

struct PortFnIdEntry {
    uint32_t id;
    PortFn fn;
    const char *name;
};

/* ID -> host function, open addressing, built from the files' registration
 * blocks on first use.  Two arrays rather than one of pairs: the probe reads
 * the ID array, and on a hit exactly one entry of the other. */
#define PORT_FNID_HASH_BITS 14
#define PORT_FNID_HASH_SIZE (1u << PORT_FNID_HASH_BITS)
#define PORT_FNID_HASH(id) (((uint32_t)(id) * 0x9E3779B1u) >> (32 - PORT_FNID_HASH_BITS))
extern uint32_t gPortFnHashId[PORT_FNID_HASH_SIZE];
extern PortFn gPortFnHashFn[PORT_FNID_HASH_SIZE];

PortFn PortFnLookupSlow(uint32_t id, const char *file, int line);

/* The common case, inline: one multiply, one compare, one load. */
static inline PortFn PortFnHostAt(uint32_t id, const char *file, int line)
{
    uint32_t h = PORT_FNID_HASH(id);
    if (__builtin_expect(gPortFnHashId[h] == id, 1))
        return gPortFnHashFn[h];
    return PortFnLookupSlow(id, file, line);
}

/* For host code: the host function for an ID, or NULL if none (no report). */
PortFn PortFnHostOrNull(uint32_t id);
/* The registered name of an ID, or NULL. */
const char *PortFnName(uint32_t id);
/* The ID of a host function (reverse lookup; slow, diagnostics only), or 0. */
uint32_t PortFnIdOfHost(PortFn fn);
/* Builds the table.  Lazily, on the first lookup that misses (the empty
 * table misses everything), so nothing has to call it first. */
void PortFnIdInit(void);
/* Is v a value a code-pointer field may hold: 0 or a registered ID? */
int PortFnIdKnown(uint32_t v);

#ifdef __cplusplus
}
#endif

#define FN(f) ((__typeof__(&f))(uintptr_t)FNID_##f)
#define CALL(p) ((__typeof__(&*(p)))PortFnHostAt((uint32_t)(uintptr_t)(p), __FILE__, __LINE__))

/* A file's registration block (written by tools/codemod_fnid.py --gen):
 *
 *     PORT_FNID_TABLE(game_src_kirby) {
 *         PORT_FNID(sub_08033478),
 *         PORT_FNID_END
 *     };
 *
 * The defining file is the only one that can name a static function, so each
 * hands its own over.  game/generated/fnid_tables.c lists them all. */
#ifdef __cplusplus
#define PORT_FNID_EXTERN extern "C"
#else
#define PORT_FNID_EXTERN extern
#endif
#define PORT_FNID_TABLE(tu) \
    PORT_FNID_EXTERN const struct PortFnIdEntry gPortFnIds_##tu[]; \
    const struct PortFnIdEntry gPortFnIds_##tu[] =
#define PORT_FNID(f) { FNID_##f, (PortFn)(f), #f }
#define PORT_FNID_END { 0, 0, 0 }

#endif /* GUARD_PORT_FNID_H */
