/* Unit test for platform/port/p32.h -- the 4-byte pointer member.
 *
 * Built and run by `make p32-test`, twice: once as C++ on the host (where a
 * pointer is 8 bytes and P32 has to do real work) and once as C (where PTR32
 * is a plain pointer and this is checking that the spelling is transparent).
 * Both runs must pass, because the whole approach rests on the two builds
 * meaning the same thing.
 *
 * The subject has to live below 4 GiB.  A P32 stores 32 bits, so a stack
 * address -- 0x7ffd... on x86-64 Linux -- truncates and the first dereference
 * dies.  That is not a flaw being worked around, it is the constraint the
 * conversion is subject to, and it is why the port has to place its own code
 * and any game-visible storage low.  The test maps a page where the GBA's
 * EWRAM goes and works there, which is what the port itself does.
 */

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

/* `static_assert` is a keyword in C++ and a C11 macro; the port builds
 * -std=gnu99, where the macro does not exist.  _Static_assert is what
 * platform/port/gba_layout.h uses for the same reason. */
#ifdef __cplusplus
#define P32_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define P32_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#ifndef _WIN32
#include <sys/mman.h>
#endif

#include "port/p32.h"

struct P32TestTarget {
    int  state;
    short x;
};

struct P32TestHolder {
    PTR32(struct P32TestTarget) target;
    PTR32(void)                 blob;
    u16                         x;
    u16                         y;
};

/* The point of the exercise: on every host, this structure is shaped the way
 * the Game Boy Advance shaped it.  If these fail, nothing else matters. */
P32_ASSERT(sizeof(struct P32TestHolder) == 12,
               "a structure of two pointers and two u16 must be 12 bytes");
P32_ASSERT(offsetof(struct P32TestHolder, blob) == 4, "second pointer must be at 4");
P32_ASSERT(offsetof(struct P32TestHolder, x)    == 8, "u16 after two pointers must be at 8");
P32_ASSERT(offsetof(struct P32TestHolder, y)    == 10, "second u16 must be at 10");

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define LOW_ADDR ((void *)0x02000000)

int main(void)
{
    int failures = 0;
    struct P32TestTarget *t;
    struct P32TestHolder h;
    struct P32TestTarget *raw;
    void *low;

#ifdef _WIN32
    printf("p32_test: no low-address mapping on this host, skipping\n");
    return 0;
#else
    low = mmap(LOW_ADDR, 4096, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (low != LOW_ADDR) {
        printf("p32_test: could not map a page at %p, skipping\n", LOW_ADDR);
        return 0;
    }

    t = (struct P32TestTarget *)low;
    t->state = 42;
    t->x = 7;

    memset(&h, 0, sizeof h);            /* trivially copyable */

    h.target = t;
    h.blob   = (void *)t;
    h.x      = 1;

    CHECK(h.target != NULL);            /* contextual bool, and != NULL   */
    CHECK(h.target->state == 42);       /* operator->                     */
    CHECK((*h.target).x == 7);          /* dereference via conversion     */
    CHECK(h.target == t);               /* compare against a real pointer */
    CHECK(&h.target->x == &t->x);       /* address of a member            */
    CHECK((u32)h.target == (u32)(uintptr_t)t);   /* explicit integer cast */
    CHECK((u32)h.blob == (u32)(uintptr_t)t);

    raw = h.target;                     /* implicit conversion out        */
    CHECK(raw != NULL && raw->state == 42);

    CHECK(h.target[0].state == 42);     /* subscript                      */

    h.target = NULL;                    /* the two spellings of null      */
    CHECK(!h.target);
    h.target = 0;
    CHECK(!h.target);

    /* The storage really is four bytes and really holds the low half. */
    CHECK(sizeof h.target == 4);
    h.target = t;
    {
        u32 stored;
        memcpy(&stored, &h.target, 4);
        CHECK(stored == (u32)(uintptr_t)t);
    }

    printf("p32_test (%s, %zu-byte host pointer): %d failure(s)\n",
#ifdef __cplusplus
           "C++",
#else
           "C",
#endif
           sizeof(void *), failures);
    return failures != 0;
#endif /* _WIN32 */
}
