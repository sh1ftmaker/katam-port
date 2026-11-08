/* Host code addresses, made comparable across builds -- for the state hash.
 *
 * The game stores function pointers in its own memory: task callbacks,
 * Object2::unk78 state functions, gIntrTable, the sound engine's jump table.
 * Natively those are the code addresses of this executable, so they move every
 * time any code linked ahead of them changes size -- and a per-frame hash of
 * EWRAM would then report a "divergence" for a change that altered no
 * behaviour at all.  docs/DEBUG-TOOLING.md section 7 is the original account.
 *
 * So the hash (platform/inputlog.c) passes every word of EWRAM and IWRAM
 * through PortHostCanonWord, and here a word that is *exactly* the entry point
 * of a function in this executable becomes 0xC0000000 | hash(function name):
 * the same value for the same function in any build of the port that keeps
 * the name, and a word that is exactly the start of one of its data objects
 * (the game's C-defined const tables are compiled in, and the game stores
 * pointers to them), or a word inside one, becomes 0x80000000 | hash(object
 * name, offset).  Functions by exact entry only, never "inside the text
 * segment".  Both rules depend on the image sitting where game data cannot
 * reach by coincidence -- 0xB5000000, see CMakeLists.txt and
 * docs/DETERMINISM.md.
 *
 * The names come from the executable's own ELF symbol table (it is not
 * stripped; static functions are in it too).  Linux only, because that is
 * where /proc/self/exe is; elsewhere the hook is the identity and the hash is
 * still deterministic, just sensitive to code layout.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native.h"
#include "port/backend.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__linux__)
#include <elf.h>

struct Sym { u32 addr; u32 canon; };
static struct Sym *sSyms;
static u32 sNumSyms;
static u32 sLo, sHi;

/* Data objects too, by start address. */
struct Obj { u32 addr, size, canon; };
static struct Obj *sObjs;
static u32 sNumObjs;
static u32 sObjLo, sObjHi;
static int sLoaded;

static u32 NameHash(const char *s)
{
    u32 h = 2166136261u;

    while (*s) {
        h ^= (u8)*s++;
        h *= 16777619u;
    }
    return 0xC0000000u | (h & 0x3FFFFFFFu);
}

static int CmpObj(const void *a, const void *b)
{
    u32 x = ((const struct Obj *)a)->addr, y = ((const struct Obj *)b)->addr;
    return x < y ? -1 : x > y;
}

static int CmpSym(const void *a, const void *b)
{
    u32 x = ((const struct Sym *)a)->addr, y = ((const struct Sym *)b)->addr;
    return x < y ? -1 : x > y;
}

static void Load(void)
{
    FILE *f;
    long size;
    u8 *img;
    const Elf64_Ehdr *eh;
    const Elf64_Shdr *sh;
    u32 i, n = 0, no = 0;

    sLoaded = 1;
    /* PORT_HASH_RAW=1: no canonicalisation, for looking at the raw words. */
    if (getenv("PORT_HASH_RAW") != NULL && *getenv("PORT_HASH_RAW") == '1')
        return;
    f = fopen("/proc/self/exe", "rb");
    if (f == NULL)
        return;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    img = (u8 *)malloc((size_t)size);
    if (img == NULL || fread(img, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(img);
        return;
    }
    fclose(f);

    eh = (const Elf64_Ehdr *)img;
    if (size < (long)sizeof *eh || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0
     || eh->e_ident[EI_CLASS] != ELFCLASS64) {
        free(img);
        return;
    }
    sh = (const Elf64_Shdr *)(img + eh->e_shoff);
    for (i = 0; i < eh->e_shnum; i++) {
        const Elf64_Sym *st;
        const char *str;
        u32 count, j;

        if (sh[i].sh_type != SHT_SYMTAB)
            continue;
        st = (const Elf64_Sym *)(img + sh[i].sh_offset);
        str = (const char *)(img + sh[sh[i].sh_link].sh_offset);
        count = (u32)(sh[i].sh_size / sizeof *st);
        sSyms = (struct Sym *)malloc(count * sizeof *sSyms);
        sObjs = (struct Obj *)malloc(count * sizeof *sObjs);
        if (sSyms == NULL || sObjs == NULL)
            break;
        for (j = 0; j < count; j++) {
            if (ELF64_ST_TYPE(st[j].st_info) == STT_OBJECT && st[j].st_size > 0
             && st[j].st_value >= 0x10000000u && st[j].st_value <= 0xFFFFFFFFu) {
                sObjs[no].addr = (u32)st[j].st_value;
                sObjs[no].size = (u32)st[j].st_size;
                sObjs[no].canon = NameHash(str + st[j].st_name) & 0x3FFFFFFFu;
                no++;
                continue;
            }
            if (ELF64_ST_TYPE(st[j].st_info) != STT_FUNC || st[j].st_value == 0
             || st[j].st_value > 0xFFFFFFFFu)
                continue;
            sSyms[n].addr = (u32)st[j].st_value;
            sSyms[n].canon = NameHash(str + st[j].st_name);
            n++;
        }
        break;
    }
    free(img);
    if (no > 0) {
        u32 w = 0, r;

        qsort(sObjs, no, sizeof *sObjs, CmpObj);
        for (r = 0; r < no; r++) {          /* aliases: one token per address */
            if (w > 0 && sObjs[w - 1].addr == sObjs[r].addr) {
                if (sObjs[r].canon < sObjs[w - 1].canon)
                    sObjs[w - 1].canon = sObjs[r].canon;
                if (sObjs[r].size > sObjs[w - 1].size)
                    sObjs[w - 1].size = sObjs[r].size;
            } else {
                sObjs[w++] = sObjs[r];
            }
        }
        no = w;
        sNumObjs = no;
        sObjLo = sObjs[0].addr;
        sObjHi = sObjs[no - 1].addr + sObjs[no - 1].size;
    }
    if (n == 0)
        return;
    qsort(sSyms, n, sizeof *sSyms, CmpSym);
    /* One token per address.  Identical-code folding gives several names one
     * entry point (the nullsubs, ply_fine and its twins), and which alias a
     * binary search lands on would otherwise depend on qsort's order.  The
     * smallest token of the group stands for all of them. */
    {
        u32 w = 0, r;

        for (r = 0; r < n; r++) {
            if (w > 0 && sSyms[w - 1].addr == sSyms[r].addr) {
                if (sSyms[r].canon < sSyms[w - 1].canon)
                    sSyms[w - 1].canon = sSyms[r].canon;
            } else {
                sSyms[w++] = sSyms[r];
            }
        }
        n = w;
    }
    sNumSyms = n;
    sLo = sSyms[0].addr;
    sHi = sSyms[n - 1].addr;
}

u32 PortHostCanonWord(u32 w)
{
    u32 lo, hi;

    if (!sLoaded)
        Load();
    /* Data objects: a pointer to the start of one, into it (an element of a
     * table -- the attract demo keeps one into a static array of the
     * port's), or one past its end.  The token is the object's name plus the
     * offset, the same in any build that keeps the name.
     *
     * Interior pointers were tried once and dropped, because with the image
     * at 0x10000000 the game's own data -- the shadow OAM, packed u16 pairs,
     * palette colours -- held plenty of words that fell inside some object by
     * coincidence, and each became a false divergence the moment the layout
     * moved.  With the image at 0xB5000000 (CMakeLists.txt) a coincidence
     * needs a high half of 0xB5xx; and without them, any change that grew
     * the code moved the port's tables and every pointer into one diverged.
     * Exact starts keep their old token (offset 0). */
    if (w >= sObjLo && w <= sObjHi) {
        lo = 0;
        hi = sNumObjs;
        while (lo < hi) {               /* first object starting after w */
            u32 mid = (lo + hi) / 2;

            if (sObjs[mid].addr <= w)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo > 0) {
            const struct Obj *o = &sObjs[lo - 1];
            u32 off = w - o->addr;

            if (off == 0)
                return 0x80000000u | o->canon;
            if (off <= o->size)
                return 0x80000000u | ((o->canon + off * 0x9E3779B1u) & 0x3FFFFFFFu);
        }
    }
    if (w < sLo || w > sHi)
        return w;
    lo = 0;
    hi = sNumSyms;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;

        if (sSyms[mid].addr < w)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < sNumSyms && sSyms[lo].addr == w)
        return sSyms[lo].canon;
    return w;
}

#else

u32 PortHostCanonWord(u32 w)
{
    return w;
}

#endif

#ifdef __cplusplus
}
#endif
