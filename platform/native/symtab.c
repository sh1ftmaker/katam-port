/* Host data addresses, made comparable across builds -- for the state hash.
 *
 * The game stores pointers to its C-defined const tables in its own memory
 * (the decompilation's data is compiled in, not read from the ROM), and
 * natively those are addresses in this executable, which move every time
 * anything linked ahead of them changes size -- so a per-frame hash of EWRAM
 * would report a "divergence" for a change that altered no behaviour at all.
 * docs/DEBUG-TOOLING.md section 7 is the original account.
 *
 * So the hash (platform/inputlog.c) passes every word of EWRAM and IWRAM
 * through PortHostCanonWord, and here a word that is exactly the start of one
 * of this executable's data objects becomes 0x80000000 | hash(object name):
 * the same value for the same object in any build of the port that keeps the
 * name.  Only exact starts, never "inside the data segment" -- the game's data
 * holds plenty of words that land in that range by coincidence.
 *
 * Function pointers used to be handled the same way (exact entry points became
 * 0xC0000000 | hash(name)).  They no longer need it: the game stores canonical
 * code IDs, never host code addresses (port/fnid.h, netplay-v2 R5), so that
 * half could only ever fire on coincidence -- a game word such as 0x10030000
 * that happened to equal some function's start in one layout -- and is gone.
 * PORT_HASH_FNCHECK=1 counts what it would have rewritten and prints the count
 * at exit, for checking that no host function pointer gets into game memory.
 *
 * The names come from the executable's own ELF symbol table (it is not
 * stripped).  Linux only, because that is where /proc/self/exe is; elsewhere
 * the hook is the identity and the hash is still deterministic, just sensitive
 * to data layout.
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
static int sFnCheck;
static u32 sFnHits;

static void SayFnHits(void)
{
    fprintf(stderr, "[katam-port] PORT_HASH_FNCHECK: %u hashed words equal a "
            "host function's entry point\n", (unsigned)sFnHits);
}

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
    sFnCheck = getenv("PORT_HASH_FNCHECK") != NULL && *getenv("PORT_HASH_FNCHECK") == '1';
    if (sFnCheck)
        atexit(SayFnHits);
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
    /* Data objects by their start address only, exactly as functions are.
     * Pointers *into* an object (an element of a table) would need a
     * containing-object lookup, and that was tried: the game's data -- the
     * shadow OAM, for one -- holds plenty of aligned words that fall inside
     * some object of this executable by coincidence, and every one of them
     * became a false divergence the moment the layout moved. */
    if (w >= sObjLo && w < sObjHi) {
        lo = 0;
        hi = sNumObjs;
        while (lo < hi) {
            u32 mid = (lo + hi) / 2;

            if (sObjs[mid].addr < w)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < sNumObjs && sObjs[lo].addr == w)
            return 0x80000000u | sObjs[lo].canon;
    }
    /* PORT-FNID: a word that is exactly a host function's entry point is not
     * rewritten any more (above).  PORT_HASH_FNCHECK=1 counts them instead,
     * which should stay at the coincidences the game's own data produces. */
    if (w < sLo || w > sHi)
        return w;
    if (sFnCheck) {
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
            sFnHits++;
    }
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
