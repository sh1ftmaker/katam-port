/* The vanilla pack: extractor, pack compiler, round-trip gate, overlay install.
 * docs/design/MODDING.md §2 (pack), §1.3 (overlay), §1.5 R1/R2.
 *
 *   extract   the player's ROM -> a folder of canonical JSON (+ pack.toml,
 *             pack.lock), driven by the committed manifest
 *             (assets/manifest.json, embedded as platform/res_manifest.h).
 *   compile   that folder, with mods overlaid (whole-file replace), -> the
 *             byte layouts the game reads: one table per kind, plus records
 *             and payloads for pointer tables.
 *   place     either IDENTITY -- every blob at the ROM address the vanilla
 *             blob has, which is the round-trip gate (F0: compile(extract(rom))
 *             must equal the ROM there, byte for byte) -- or ARENA, a
 *             deterministic bump allocator at RES_ARENA_BASE in kind order,
 *             id order, 16-byte aligned, which is what the game is served.
 *   install   copy the arena blobs in, retarget the slots (platform/res.c).
 *
 * Kinds in this first slice (manifest v1): room_props (gRoomProps), fg_palette
 * (gForegroundPalettes -> ForegroundPalette -> 6x16 BGR555), obj_desc
 * (gUnk_08351648).  Nothing here is ROM data: the folder is written from the
 * player's ROM at run time and never ships. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "port/port.h"
#include "port/res.h"
#include "res_internal.h"
#include "res_json.h"
#include "res_hash.h"
#include "res_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EXTRACTOR_VERSION "1.0.0"
#define PACK_FORMAT 1

/* --- the manifest ------------------------------------------------------------ */

enum { F_U8, F_U16, F_S16, F_U32, F_PTR32, F_BYTES };
enum { SHAPE_ARRAY, SHAPE_PTRTAB };

struct Field {
    const char *name;
    u8 type, size;
    u16 offset;
};

struct Kind {
    const char *kind, *realm, *label, *path;
    u32 addr, count, stride, slot;
    u8 shape, sim;
    struct Field f[32];
    u32 nf;
    int payload;            /* PTRTAB: index of the field holding the payload pointer, or -1 */
    u32 payloadBanks, payloadColors;
    const JVal *names;      /* obj_desc: J_ARR of strings */
};

#define MAX_KINDS 8
static struct Kind sKinds[MAX_KINDS];
static u32 sNumKinds;
static JVal *sManifest;
static const char *sManifestSha1;
static char sManifestSha256[65];

static u32 ParseHex(const char *s)
{
    return s ? (u32)strtoul(s, NULL, 16) : 0;
}

static u32 SlotByLabel(const char *label)
{
    u32 i;

    for (i = 0; i < RT_COUNT; i++)
        if (!strcmp(Res_SlotName(i), label))
            return i;
    return RT_COUNT;
}

static int LoadManifest(void)
{
    char err[128];
    const JVal *kinds;
    u8 sha[32];
    u32 i, j;

    if (sManifest)
        return 0;
    ResSha256(kResManifestJson, strlen(kResManifestJson), sha);
    ResHex(sha, 32, sManifestSha256);
    sManifest = JParse(kResManifestJson, strlen(kResManifestJson), err, sizeof err);
    if (!sManifest) {
        PortLog("[res] manifest: %s", err);
        return -1;
    }
    sManifestSha1 = JStr(JGet(sManifest, "rom"), "sha1");
    kinds = JGet(sManifest, "kinds");
    for (i = 0; kinds && i < kinds->n && sNumKinds < MAX_KINDS; i++) {
        const JVal *k = kinds->items[i], *fields = JGet(k, "fields"), *pl = JGet(k, "payload");
        struct Kind *K = &sKinds[sNumKinds];

        memset(K, 0, sizeof *K);
        K->kind = JStr(k, "kind");
        K->realm = JStr(k, "realm");
        K->label = JStr(k, "label");
        K->path = JStr(k, "path");
        K->addr = ParseHex(JStr(k, "addr"));
        K->count = (u32)JInt(k, "count", 0);
        K->stride = (u32)JInt(k, "stride", 0);
        K->shape = JStr(k, "shape") && !strcmp(JStr(k, "shape"), "ptrtab") ? SHAPE_PTRTAB : SHAPE_ARRAY;
        K->sim = K->realm && !strcmp(K->realm, "sim");
        K->slot = SlotByLabel(K->label ? K->label : "");
        K->names = JGet(k, "names");
        K->payload = -1;
        for (j = 0; fields && j < fields->n && j < 32; j++) {
            const JVal *f = fields->items[j];
            const char *t = JStr(f, "type");
            struct Field *F = &K->f[K->nf++];

            F->name = JStr(f, "name");
            F->offset = (u16)JInt(f, "offset", 0);
            if (!t)                       { F->type = F_U8; F->size = 1; }
            else if (!strcmp(t, "u8"))    { F->type = F_U8; F->size = 1; }
            else if (!strcmp(t, "u16"))   { F->type = F_U16; F->size = 2; }
            else if (!strcmp(t, "s16"))   { F->type = F_S16; F->size = 2; }
            else if (!strcmp(t, "u32"))   { F->type = F_U32; F->size = 4; }
            else if (!strcmp(t, "ptr32")) { F->type = F_PTR32; F->size = 4; }
            else if (!strncmp(t, "u8x", 3)) { F->type = F_BYTES; F->size = (u8)atoi(t + 3); }
            if (pl && F->name && JStr(pl, "field") && !strcmp(F->name, JStr(pl, "field")))
                K->payload = (int)j;
        }
        if (pl) {
            K->payloadBanks = (u32)JInt(pl, "banks", 0);
            K->payloadColors = (u32)JInt(pl, "colors", 0);
        }
        if (!K->kind || !K->path || K->slot == RT_COUNT || !K->count || !K->stride) {
            PortLog("[res] manifest: kind %u is incomplete or names an unknown label", (unsigned)i);
            return -1;
        }
        sNumKinds++;
    }
    return 0;
}

/* --- the ROM ----------------------------------------------------------------- */

static int RomHas(u32 addr, u32 len)
{
    return addr >= GBA_ROM_BASE && len <= gPortRomSize
        && addr - GBA_ROM_BASE <= gPortRomSize - len;
}

static const u8 *Rom(u32 addr)
{
    return (const u8 *)(uintptr_t)addr;
}

static u32 Rd(const u8 *p, u32 size)
{
    u32 v = 0, i;

    for (i = 0; i < size; i++)
        v |= (u32)p[i] << (8 * i);
    return v;
}

static void Wr(u8 *p, u32 size, u32 v)
{
    u32 i;

    for (i = 0; i < size; i++)
        p[i] = (u8)(v >> (8 * i));
}

static int CheckRom(char *why, size_t whyLen)
{
    u8 sha[20];
    char hex[41];

    ResSha1(Rom(GBA_ROM_BASE), gPortRomSize, sha);
    ResHex(sha, 20, hex);
    if (!sManifestSha1 || strcmp(hex, sManifestSha1) != 0) {
        snprintf(why, whyLen, "this ROM (sha1 %s) is not the one the manifest describes (%s); "
                 "only that region is supported for now", hex, sManifestSha1 ? sManifestSha1 : "?");
        return -1;
    }
    return 0;
}

/* --- files ------------------------------------------------------------------- */

static int MkdirP(const char *path)
{
    char tmp[1024];
    size_t i, n = strlen(path);

    if (n >= sizeof tmp)
        return -1;
    memcpy(tmp, path, n + 1);
    for (i = 1; i <= n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\0') {
            char c = tmp[i];

            tmp[i] = '\0';
#ifdef _WIN32
            if (_mkdir(tmp) != 0 && errno != EEXIST)
#else
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
#endif
                return -1;
            tmp[i] = c;
        }
    }
    return 0;
}

static char *ReadFile(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = '\0';
        *len = (size_t)n;
    }
    return buf;
}

/* Files the extractor wrote, for pack.lock. */
#define MAX_FILES 2048
static struct { char rel[96]; char sha[65]; } sFiles[MAX_FILES];
static u32 sNumFiles;

static int WriteRel(const char *dir, const char *rel, const Buf *b)
{
    char path[1024], *slash;
    u8 sha[32];
    FILE *f;

    snprintf(path, sizeof path, "%s/%s", dir, rel);
    slash = strrchr(path, '/');
    if (slash) {
        *slash = '\0';
        if (MkdirP(path) != 0) {
            PortLog("[res] cannot create %s", path);
            return -1;
        }
        *slash = '/';
    }
    f = fopen(path, "wb");
    if (!f || fwrite(b->p, 1, b->len, f) != b->len) {
        if (f)
            fclose(f);
        PortLog("[res] cannot write %s", path);
        return -1;
    }
    fclose(f);
    if (sNumFiles < MAX_FILES) {
        snprintf(sFiles[sNumFiles].rel, sizeof sFiles[sNumFiles].rel, "%s", rel);
        ResSha256(b->p, b->len, sha);
        ResHex(sha, 32, sFiles[sNumFiles].sha);
        sNumFiles++;
    }
    return 0;
}

static void RelPath(const struct Kind *K, u32 id, char *out, size_t n)
{
    snprintf(out, n, K->path, (unsigned)id);
}

/* --- extract ------------------------------------------------------------------ */

static void PutField(Buf *b, const struct Field *F, const u8 *rec)
{
    const u8 *p = rec + F->offset;
    u32 i;

    switch (F->type) {
    case F_S16:   BufPrintf(b, "%d", (int)(s16)Rd(p, 2)); break;
    case F_PTR32: BufPrintf(b, "\"0x%08X\"", (unsigned)Rd(p, 4)); break;
    case F_BYTES:
        BufPut(b, "[", 1);
        for (i = 0; i < F->size; i++)
            BufPrintf(b, "%s%u", i ? ", " : "", (unsigned)p[i]);
        BufPut(b, "]", 1);
        break;
    default:      BufPrintf(b, "%u", (unsigned)Rd(p, F->size)); break;
    }
}

/* BGR555 -> "#RRGGBB" by c5 << 3 | c5 >> 2 (exact inverse: >> 3). */
static void PutColor(Buf *b, u16 c)
{
    u32 r = c & 31, g = (c >> 5) & 31, bl = (c >> 10) & 31;

    BufPrintf(b, "\"#%02X%02X%02X\"", (unsigned)(r << 3 | r >> 2), (unsigned)(g << 3 | g >> 2),
              (unsigned)(bl << 3 | bl >> 2));
}

static int ExtractKind(const char *dir, const struct Kind *K)
{
    char rel[96];
    u32 id, i;
    int perId = strchr(K->path, '%') != NULL;
    Buf all = { 0 };

    if (!perId)
        BufPrintf(&all, "{\n  \"%s\": [\n", K->kind);
    for (id = 0; id < K->count; id++) {
        const u8 *rec;
        Buf b = { 0 };
        const char *ind = perId ? "  " : "";
        const char *sep = perId ? ",\n" : ", ";

        if (K->shape == SHAPE_ARRAY) {
            rec = Rom(K->addr + id * K->stride);
        } else {
            u32 at = Rd(Rom(K->addr + 4 * id), 4);

            if (!RomHas(at, K->stride)) {
                PortLog("[res] %s %u: record pointer 0x%08X is outside the ROM", K->kind, (unsigned)id, (unsigned)at);
                BufFree(&all);
                return -1;
            }
            rec = Rom(at);
        }

        BufPrintf(&b, perId ? "{\n  \"id\": %u" : "    {\"id\": %u", (unsigned)id);
        if (K->names && id < K->names->n && K->names->items[id]->type == J_STR)
            BufPrintf(&b, "%s%s\"name\": \"%s\"", sep, ind, K->names->items[id]->str);
        for (i = 0; i < K->nf; i++) {
            if ((int)i == K->payload)
                continue;
            BufPrintf(&b, "%s%s\"%s\": ", sep, ind, K->f[i].name);
            PutField(&b, &K->f[i], rec);
        }
        if (K->payload >= 0) {
            u32 pal = Rd(rec + K->f[K->payload].offset, 4), n = K->payloadBanks * K->payloadColors;
            u32 bank, c, any = 0;

            if (!RomHas(pal, n * 2)) {
                PortLog("[res] %s %u: payload 0x%08X is outside the ROM", K->kind, (unsigned)id, (unsigned)pal);
                BufFree(&b);
                BufFree(&all);
                return -1;
            }
            BufPrintf(&b, ",\n  \"colors\": [\n");
            for (bank = 0; bank < K->payloadBanks; bank++) {
                BufPut(&b, "    [", 5);
                for (c = 0; c < K->payloadColors; c++) {
                    if (c)
                        BufPut(&b, ", ", 2);
                    PutColor(&b, (u16)Rd(Rom(pal + 2 * (bank * K->payloadColors + c)), 2));
                }
                BufPrintf(&b, "]%s\n", bank + 1 < K->payloadBanks ? "," : "");
            }
            /* Bit 15 is not a colour bit, and #RRGGBB cannot carry it; 1639
             * vanilla entries have it set, so the indices that do are kept. */
            BufPrintf(&b, "  ],\n  \"bit15\": [");
            for (c = 0; c < n; c++)
                if (Rd(Rom(pal + 2 * c), 2) & 0x8000)
                    BufPrintf(&b, "%s%u", any++ ? ", " : "", (unsigned)c);
            BufPut(&b, "]", 1);
        }
        if (perId) {
            BufPut(&b, "\n}\n", 3);
            RelPath(K, id, rel, sizeof rel);
            if (WriteRel(dir, rel, &b) != 0) {
                BufFree(&b);
                return -1;
            }
        } else {
            BufPrintf(&b, "}%s\n", id + 1 < K->count ? "," : "");
            BufPut(&all, b.p, b.len);
        }
        BufFree(&b);
    }
    if (!perId) {
        int rc;

        BufPrintf(&all, "  ]\n}\n");
        rc = WriteRel(dir, K->path, &all);
        BufFree(&all);
        return rc;
    }
    return 0;
}

static int CmpFile(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int ResExtract(const char *dir)
{
    char why[256], romsha[41];
    u8 sha[20];
    Buf toml = { 0 }, lock = { 0 };
    u32 i;
    int rc = 0;

    if (LoadManifest() != 0)
        return -1;
    if (CheckRom(why, sizeof why) != 0) {
        PortLog("[res] refusing to extract: %s", why);
        return -1;
    }
    ResSha1(Rom(GBA_ROM_BASE), gPortRomSize, sha);
    ResHex(sha, 20, romsha);
    sNumFiles = 0;
    for (i = 0; i < sNumKinds && rc == 0; i++)
        rc = ExtractKind(dir, &sKinds[i]);
    if (rc != 0)
        return rc;

    BufPrintf(&toml, "# Extracted from your ROM by katam-port. Do not redistribute.\n"
                     "[pack]\nformat = %d\nextractor = \"%s\"\nrom_sha1 = \"%s\"\n"
                     "manifest_sha256 = \"%s\"\nkinds = [", PACK_FORMAT, EXTRACTOR_VERSION,
              romsha, sManifestSha256);
    for (i = 0; i < sNumKinds; i++)
        BufPrintf(&toml, "%s\"%s\"", i ? ", " : "", sKinds[i].kind);
    BufPrintf(&toml, "]\n");
    rc = WriteRel(dir, "pack.toml", &toml);
    BufFree(&toml);
    if (rc != 0)
        return rc;

    qsort(sFiles, sNumFiles, sizeof sFiles[0], CmpFile);   /* rel is the first member */
    for (i = 0; i < sNumFiles; i++)
        BufPrintf(&lock, "%s  %s\n", sFiles[i].sha, sFiles[i].rel);
    sNumFiles = 0;
    rc = WriteRel(dir, "pack.lock", &lock);
    BufFree(&lock);
    if (rc == 0)
        PortLog("[res] extracted %u kinds to %s (extractor %s, manifest %.12s)",
                (unsigned)sNumKinds, dir, EXTRACTOR_VERSION, sManifestSha256);
    return rc;
}

/* --- compile --------------------------------------------------------------------- */

enum { ROLE_TABLE, ROLE_RECORD, ROLE_PAYLOAD };

struct Blob {
    u32 kind, id;
    u8 role, layer;
    u32 size, at;
    u8 *data;
};

#define MAX_BLOBS 1024
static struct Blob sBlobs[MAX_BLOBS];
static u32 sNumBlobs;

static const char *sCPack;
static const char *const *sCMods;
static u32 sCNumMods;

static struct Blob *AddBlob(u32 kind, u32 id, u8 role, u32 size)
{
    struct Blob *b;

    if (sNumBlobs >= MAX_BLOBS)
        return NULL;
    b = &sBlobs[sNumBlobs++];
    memset(b, 0, sizeof *b);
    b->kind = kind;
    b->id = id;
    b->role = role;
    b->size = size;
    b->layer = RES_LAYER_PACK;
    b->data = (u8 *)calloc(1, size ? size : 1);
    return b;
}

static void FreeBlobs(void)
{
    u32 i;

    for (i = 0; i < sNumBlobs; i++)
        free(sBlobs[i].data);
    sNumBlobs = 0;
}

static struct Blob *FindBlob(u32 kind, u32 id, u8 role)
{
    u32 i;

    for (i = 0; i < sNumBlobs; i++)
        if (sBlobs[i].kind == kind && sBlobs[i].id == id && sBlobs[i].role == role)
            return &sBlobs[i];
    return NULL;
}

/* Mod patches (MODDING.md §3.2): a mod.toml line
 *     patches = ["patches/a.json", ...]
 * names JSON files of the form
 *     { "target": "gfx/palettes/fg/0.json",
 *       "ops": [ { "op": "set",   "path": "colors/1/3", "value": "#FF4080" },
 *                { "op": "merge", "path": "",           "value": { "unk8": 1 } } ] }
 * which edit the resolved file -- so a mod changes one colour or one field
 * and carries none of the vanilla record around it.  (uid-addressed list
 * edits, §3.2's "add"/"remove", come with the kinds that have lists.) */
#define MAX_PATCHES 64
static struct { u32 mod; JVal *doc; char file[256]; } sPatches[MAX_PATCHES];
static u32 sNumPatches;
static int sPatchError;

static void FreePatches(void)
{
    u32 i;

    for (i = 0; i < sNumPatches; i++)
        JFree(sPatches[i].doc);
    sNumPatches = 0;
}

static int LoadPatches(void)
{
    u32 m;

    FreePatches();
    for (m = 0; m < sCNumMods; m++) {
        char path[1024], err[160], *toml, *line;
        size_t len;

        snprintf(path, sizeof path, "%s/mod.toml", sCMods[m]);
        toml = ReadFile(path, &len);
        if (!toml) {
            PortLog("[res] %s: no mod.toml", sCMods[m]);
            return -1;
        }
        for (line = toml; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
            char *q, *end;

            if (strncmp(line, "patches", 7) != 0)
                continue;
            end = strchr(line, '\n');
            for (q = strchr(line, '"'); q && (!end || q < end); q = strchr(q, '"')) {
                char *close = strchr(q + 1, '"'), *text;
                JVal *doc;

                if (!close || (end && close > end))
                    break;
                *close = '\0';
                if (strstr(q + 1, "..") || q[1] == '/') {
                    PortLog("[res] %s: patch path %s leaves the mod", sCMods[m], q + 1);
                    free(toml);
                    return -1;
                }
                snprintf(path, sizeof path, "%s/%s", sCMods[m], q + 1);
                text = ReadFile(path, &len);
                doc = text ? JParse(text, len, err, sizeof err) : NULL;
                free(text);
                if (!doc || !JStr(doc, "target") || !JGet(doc, "ops")) {
                    PortLog("[res] %s: %s", path, doc ? "wants \"target\" and \"ops\"" : text ? err : "missing");
                    JFree(doc);
                    free(toml);
                    return -1;
                }
                if (sNumPatches < MAX_PATCHES) {
                    sPatches[sNumPatches].mod = m;
                    sPatches[sNumPatches].doc = doc;
                    snprintf(sPatches[sNumPatches].file, sizeof sPatches[0].file, "%s", path);
                    sNumPatches++;
                } else {
                    JFree(doc);
                }
                q = close + 1;
            }
        }
        free(toml);
    }
    return 0;
}

/* Apply, in load order, every patch on `rel` from the winning layer up;
 * patches below the winner were written against a file that has been
 * replaced, and are dropped with a warning (§3.2).  Returns how many. */
static u32 ApplyPatches(const char *rel, JVal *doc, int winner)
{
    u32 i, j, applied = 0;

    for (i = 0; i < sNumPatches; i++) {
        const JVal *ops = JGet(sPatches[i].doc, "ops");

        if (strcmp(JStr(sPatches[i].doc, "target"), rel) != 0)
            continue;
        if ((int)sPatches[i].mod < winner) {
            PortLog("[res] %s: dropped, %s is replaced by a later mod", sPatches[i].file, rel);
            continue;
        }
        for (j = 0; ops && j < ops->n; j++) {
            const JVal *op = ops->items[j];
            const char *kind = JStr(op, "op"), *path = JStr(op, "path");
            const JVal *value = JGet(op, "value");
            int rc = -1;

            if (kind && !strcmp(kind, "set") && path)
                rc = JSet(doc, path, value);
            else if (kind && !strcmp(kind, "merge"))
                rc = JMerge(doc, path ? path : "", value);
            if (rc != 0) {
                PortLog("[res] %s: op %u (%s %s) does not apply to %s", sPatches[i].file,
                        (unsigned)j, kind ? kind : "?", path ? path : "", rel);
                sPatchError = 1;
                return applied;
            }
            applied++;
        }
    }
    return applied;
}

/* The overlay: the highest mod that has the file wins, whole-file; else the
 * pack.  MODDING.md §1.3 "file replace".  Then the patches. */
static JVal *ReadOverlay(const char *rel, u8 *layer)
{
    char path[1024], err[160];
    size_t len;
    char *text = NULL;
    JVal *v;
    u32 i;
    int winner = -1;

    for (i = sCNumMods; i-- > 0;) {
        snprintf(path, sizeof path, "%s/assets/%s", sCMods[i], rel);
        if ((text = ReadFile(path, &len)) != NULL) {
            *layer = RES_LAYER_MOD;
            winner = (int)i;
            break;
        }
    }
    if (!text) {
        snprintf(path, sizeof path, "%s/%s", sCPack, rel);
        text = ReadFile(path, &len);
        *layer = RES_LAYER_PACK;
    }
    if (!text)
        return NULL;
    v = JParse(text, len, err, sizeof err);
    if (!v)
        PortLog("[res] %s: %s", path, err);
    free(text);
    if (v && ApplyPatches(rel, v, winner) > 0)
        *layer = RES_LAYER_MOD;
    if (sPatchError) {
        JFree(v);
        return NULL;
    }
    return v;
}

static int GetField(const struct Field *F, const JVal *obj, u8 *rec, const char *what)
{
    const JVal *v = JGet(obj, F->name);
    u8 *p = rec + F->offset;
    u32 i;

    if (!v) {
        PortLog("[res] %s: missing \"%s\"", what, F->name);
        return -1;
    }
    switch (F->type) {
    case F_PTR32:
        if (v->type != J_STR) {
            PortLog("[res] %s: \"%s\" wants a \"0x...\" address", what, F->name);
            return -1;
        }
        Wr(p, 4, ParseHex(v->str));
        return 0;
    case F_BYTES:
        if (v->type != J_ARR || v->n != F->size) {
            PortLog("[res] %s: \"%s\" wants %u bytes", what, F->name, (unsigned)F->size);
            return -1;
        }
        for (i = 0; i < F->size; i++)
            p[i] = (u8)v->items[i]->num;
        return 0;
    default:
        if (v->type != J_INT) {
            PortLog("[res] %s: \"%s\" wants an integer", what, F->name);
            return -1;
        }
        if ((F->type == F_S16 && (v->num < -32768 || v->num > 32767))
         || (F->type != F_S16 && (v->num < 0 || (F->size < 4 && v->num >= (1LL << (8 * F->size)))))
         || v->num > 0xFFFFFFFFLL) {
            PortLog("[res] %s: \"%s\" = %lld does not fit", what, F->name, v->num);
            return -1;
        }
        Wr(p, F->size, (u32)v->num);
        return 0;
    }
}

static int GetRecord(const struct Kind *K, const JVal *obj, u8 *rec, const char *what)
{
    u32 i;

    for (i = 0; i < K->nf; i++)
        if ((int)i != K->payload && GetField(&K->f[i], obj, rec, what) != 0)
            return -1;
    return 0;
}

static int ParseColor(const char *s, u16 *out)
{
    unsigned r, g, b;

    if (!s || strlen(s) != 7 || s[0] != '#' || sscanf(s + 1, "%2x%2x%2x", &r, &g, &b) != 3)
        return -1;
    *out = (u16)((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10);
    return 0;
}

static int GetPayload(const struct Kind *K, const JVal *obj, u8 *pal, const char *what)
{
    const JVal *colors = JGet(obj, "colors"), *b15 = JGet(obj, "bit15");
    u32 bank, c;

    if (!colors || colors->type != J_ARR || colors->n != K->payloadBanks) {
        PortLog("[res] %s: \"colors\" wants %u banks", what, (unsigned)K->payloadBanks);
        return -1;
    }
    for (bank = 0; bank < K->payloadBanks; bank++) {
        const JVal *row = colors->items[bank];

        if (row->type != J_ARR || row->n != K->payloadColors) {
            PortLog("[res] %s: bank %u wants %u colours", what, (unsigned)bank, (unsigned)K->payloadColors);
            return -1;
        }
        for (c = 0; c < K->payloadColors; c++) {
            u16 v;

            if (row->items[c]->type != J_STR || ParseColor(row->items[c]->str, &v) != 0) {
                PortLog("[res] %s: bank %u colour %u is not #RRGGBB", what, (unsigned)bank, (unsigned)c);
                return -1;
            }
            Wr(pal + 2 * (bank * K->payloadColors + c), 2, v);
        }
    }
    for (c = 0; b15 && b15->type == J_ARR && c < b15->n; c++) {
        long long i = b15->items[c]->num;

        if (i >= 0 && i < (long long)(K->payloadBanks * K->payloadColors))
            pal[2 * i + 1] |= 0x80;
    }
    return 0;
}

static int CompileKind(u32 k)
{
    const struct Kind *K = &sKinds[k];
    char rel[96], what[160];
    struct Blob *table;
    u32 id;
    u8 layer;

    table = AddBlob(k, 0, ROLE_TABLE, K->shape == SHAPE_ARRAY ? K->count * K->stride : K->count * 4);
    if (!table)
        return -1;

    if (!strchr(K->path, '%')) {                    /* one file, a list of records */
        JVal *doc = ReadOverlay(K->path, &layer);
        const JVal *list = JGet(doc, K->kind);
        u32 i;

        if (!doc || !list || list->type != J_ARR) {
            PortLog("[res] %s: missing or not {\"%s\": [...]}", K->path, K->kind);
            JFree(doc);
            return -1;
        }
        table->layer = layer;
        for (i = 0; i < list->n; i++) {
            long long rid = JInt(list->items[i], "id", -1);

            snprintf(what, sizeof what, "%s[%u]", K->path, (unsigned)i);
            if (rid < 0 || rid >= (long long)K->count) {
                PortLog("[res] %s: bad id", what);
                JFree(doc);
                return -1;
            }
            if (GetRecord(K, list->items[i], table->data + (u32)rid * K->stride, what) != 0) {
                JFree(doc);
                return -1;
            }
        }
        JFree(doc);
        return 0;
    }

    for (id = 0; id < K->count; id++) {
        JVal *doc;
        int rc = 0;

        RelPath(K, id, rel, sizeof rel);
        doc = ReadOverlay(rel, &layer);
        if (!doc) {
            PortLog("[res] %s: missing (re-extract the pack)", rel);
            return -1;
        }
        if (layer == RES_LAYER_MOD)
            table->layer = RES_LAYER_MOD;
        if (K->shape == SHAPE_ARRAY) {
            rc = GetRecord(K, doc, table->data + id * K->stride, rel);
        } else {
            struct Blob *rec = AddBlob(k, id, ROLE_RECORD, K->stride);
            struct Blob *pay = K->payload >= 0
                ? AddBlob(k, id, ROLE_PAYLOAD, K->payloadBanks * K->payloadColors * 2) : NULL;

            if (!rec || (K->payload >= 0 && !pay))
                rc = -1;
            else {
                rec->layer = layer;
                if (pay)
                    pay->layer = layer;
                rc = GetRecord(K, doc, rec->data, rel);
                if (rc == 0 && pay)
                    rc = GetPayload(K, doc, pay->data, rel);
            }
        }
        JFree(doc);
        if (rc != 0)
            return -1;
    }
    return 0;
}

static int CompileAll(const char *pack, const char *const *mods, u32 numMods)
{
    u32 k;

    FreeBlobs();
    sCPack = pack;
    sCMods = mods;
    sCNumMods = numMods;
    sPatchError = 0;
    if (LoadPatches() != 0)
        return -1;
    for (k = 0; k < sNumKinds; k++)
        if (CompileKind(k) != 0) {
            FreePatches();
            return -1;
        }
    FreePatches();
    return 0;
}

/* --- placement --------------------------------------------------------------------- */

enum { PLACE_IDENTITY, PLACE_ARENA };

static int Place(int mode)
{
    u32 i, k, next = RES_ARENA_BASE;

    if (mode == PLACE_ARENA) {
        /* Kinds in manifest order; within a kind payloads, records, then the
         * table, each in id order: identical inputs give identical addresses
         * on every host (MODDING.md §1.4). */
        static const u8 kOrder[] = { ROLE_PAYLOAD, ROLE_RECORD, ROLE_TABLE };
        u32 r;

        for (k = 0; k < sNumKinds; k++)
            for (r = 0; r < 3; r++)
                for (i = 0; i < sNumBlobs; i++)
                    if (sBlobs[i].kind == k && sBlobs[i].role == kOrder[r]) {
                        sBlobs[i].at = next;
                        next = (next + sBlobs[i].size + 15) & ~15u;
                    }
        if (next - RES_ARENA_BASE > RES_ARENA_SIZE) {
            PortLog("[res] the compiled pack needs %u bytes; the arena has %u",
                    (unsigned)(next - RES_ARENA_BASE), (unsigned)RES_ARENA_SIZE);
            return -1;
        }
    } else {
        /* Where vanilla has each blob -- read from the ROM's own pointers. */
        for (i = 0; i < sNumBlobs; i++) {
            struct Blob *b = &sBlobs[i];
            const struct Kind *K = &sKinds[b->kind];

            if (b->role == ROLE_TABLE)
                b->at = K->addr;
            else if (b->role == ROLE_RECORD)
                b->at = Rd(Rom(K->addr + 4 * b->id), 4);
            else
                b->at = Rd(Rom(Rd(Rom(K->addr + 4 * b->id), 4) + K->f[K->payload].offset), 4);
        }
    }

    /* Pointers between blobs: table -> record, record -> payload. */
    for (i = 0; i < sNumBlobs; i++) {
        struct Blob *b = &sBlobs[i];
        const struct Kind *K = &sKinds[b->kind];

        if (K->shape != SHAPE_PTRTAB)
            continue;
        if (b->role == ROLE_TABLE) {
            for (k = 0; k < K->count; k++) {
                struct Blob *rec = FindBlob(b->kind, k, ROLE_RECORD);

                if (rec)
                    Wr(b->data + 4 * k, 4, rec->at);
            }
        } else if (b->role == ROLE_RECORD && K->payload >= 0) {
            struct Blob *pay = FindBlob(b->kind, b->id, ROLE_PAYLOAD);

            if (pay)
                Wr(b->data + K->f[K->payload].offset, 4, pay->at);
        }
    }
    return 0;
}

/* levelSetHash (MODDING.md §3.5) over what this slice resolves: SHA-256 of
 * "katam-resolved-v1" and, for each sim-realm kind in manifest order, the
 * kind's name and the SHA-256 of its compiled bytes.  The sim kinds so far
 * are flat arrays with no pointers between blobs, so the hash does not
 * depend on where the arena put them.  Presentation kinds are logged, never
 * part of it. */
static u8 sPackSimSha[8][32];
static char sPackSimKind[8][32];
static u32 sPackSimN;

static void LogContentHashes(void)
{
    u8 sha[32];
    char hex[65];
    u32 k, i;

    sPackSimN = 0;
    for (k = 0; k < sNumKinds; k++) {
        Buf one = { 0 };

        for (i = 0; i < sNumBlobs; i++)
            if (sBlobs[i].kind == k && (sBlobs[i].role != ROLE_TABLE || sKinds[k].shape == SHAPE_ARRAY))
                BufPut(&one, sBlobs[i].data, sBlobs[i].size);
        ResSha256(one.p, one.len, sha);
        ResHex(sha, 32, hex);
        PortLog("[res]   %-11s %-12s sha256 %.16s", sKinds[k].kind, sKinds[k].realm, hex);
        if (sKinds[k].sim && sPackSimN < 8) {
            memcpy(sPackSimSha[sPackSimN], sha, 32);
            snprintf(sPackSimKind[sPackSimN], sizeof sPackSimKind[0], "%s", sKinds[k].kind);
            sPackSimN++;
        }
        BufFree(&one);
    }
}

/* levelSetHash (MODDING.md §3.5): SHA-256 of "katam-resolved-v1" and, for
 * each sim-realm kind the pack served in manifest order, the kind's name and
 * the SHA-256 of its compiled bytes; then, when room bundles are installed,
 * "rooms" and their SHA-256 (platform/res_rooms.c).  The sim kinds are flat
 * arrays and the bundles hold no addresses, so the hash does not depend on
 * where the arena put anything.  Presentation kinds are never part of it.
 * Without bundles the value is the one the pack alone always gave. */
int ResFinishLevelSetHash(u8 out[32])
{
    Buf all = { 0 };
    u8 rooms[32];
    char hex[65];
    int haveRooms = ResRoomsHash(rooms);
    u32 k;

    if (!sPackSimN && !haveRooms)
        return 0;
    BufPut(&all, "katam-resolved-v1", 17);
    for (k = 0; k < sPackSimN; k++) {
        BufPut(&all, sPackSimKind[k], strlen(sPackSimKind[k]));
        BufPut(&all, sPackSimSha[k], 32);
    }
    if (haveRooms) {
        BufPut(&all, "rooms", 5);
        BufPut(&all, rooms, 32);
    }
    ResSha256(all.p, all.len, out);
    ResHex(out, 32, hex);
    PortLog("[res] levelSetHash %s", hex);
    BufFree(&all);
    return 1;
}

/* --- the round-trip gate (F0) --------------------------------------------------------- */

int ResRoundTrip(const char *dir)
{
    char why[256];
    u32 i, bad = 0, bytes = 0, k;

    if (LoadManifest() != 0)
        return -1;
    if (CheckRom(why, sizeof why) != 0) {
        PortLog("[res] %s", why);
        return -1;
    }
    if (CompileAll(dir, NULL, 0) != 0 || Place(PLACE_IDENTITY) != 0) {
        FreeBlobs();
        return -1;
    }
    for (k = 0; k < sNumKinds; k++) {
        u32 kb = 0, kbad = 0, kn = 0;

        for (i = 0; i < sNumBlobs; i++) {
            const struct Blob *b = &sBlobs[i];

            if (b->kind != k)
                continue;
            kn++;
            kb += b->size;
            if (!RomHas(b->at, b->size) || memcmp(b->data, Rom(b->at), b->size) != 0) {
                u32 j = 0;

                if (RomHas(b->at, b->size))
                    while (j < b->size && b->data[j] == Rom(b->at)[j])
                        j++;
                if (kbad++ < 5)
                    PortLog("[res] F0 %s id %u role %u: differs at 0x%08X", sKinds[k].kind,
                            (unsigned)b->id, (unsigned)b->role, (unsigned)(b->at + j));
            }
        }
        PortLog("[res] F0 %-11s %4u blobs %7u bytes: %s", sKinds[k].kind, (unsigned)kn,
                (unsigned)kb, kbad ? "DIFFERS" : "identical to the ROM");
        bytes += kb;
        bad += kbad;
    }
    PortLog("[res] F0 round trip: %u bytes compared, %u blobs differ", (unsigned)bytes, (unsigned)bad);
    FreeBlobs();
    return bad ? 1 : 0;
}

/* --- install ------------------------------------------------------------------------- */

static int CheckPackToml(const char *dir)
{
    char path[1024], romsha[41], want[160];
    u8 sha[20];
    size_t len;
    char *t;
    int ok;

    snprintf(path, sizeof path, "%s/pack.toml", dir);
    t = ReadFile(path, &len);
    if (!t) {
        PortLog("[res] %s: no pack.toml -- extract one with --res-extract", dir);
        return -1;
    }
    ResSha1(Rom(GBA_ROM_BASE), gPortRomSize, sha);
    ResHex(sha, 20, romsha);
    snprintf(want, sizeof want, "rom_sha1 = \"%s\"", romsha);
    ok = strstr(t, want) != NULL;
    snprintf(want, sizeof want, "manifest_sha256 = \"%s\"", sManifestSha256);
    if (ok && !strstr(t, want)) {
        PortLog("[res] %s was extracted with another manifest; re-extract it", dir);
        ok = 0;
    } else if (!ok) {
        PortLog("[res] %s was extracted from a different ROM", dir);
    }
    free(t);
    return ok ? 0 : -1;
}

static void LogMods(const char *const *mods, u32 numMods)
{
    u32 i;

    for (i = 0; i < numMods; i++) {
        char path[1024], *t, *id;
        size_t len;

        snprintf(path, sizeof path, "%s/mod.toml", mods[i]);
        t = ReadFile(path, &len);
        id = t ? strstr(t, "id") : NULL;
        if (id) {
            char *nl = strchr(id, '\n');

            if (nl)
                *nl = '\0';
        }
        PortLog("[res] mod %u: %s (%s)", (unsigned)i, mods[i], id ? id : "no mod.toml");
        free(t);
    }
}

int ResPackInstall(const char *packDir, const char *const *mods, u32 numMods)
{
    u32 i, k, used = 0;

    sPackSimN = 0;
    if (LoadManifest() != 0 || CheckPackToml(packDir) != 0)
        return -1;
    LogMods(mods, numMods);
    if (CompileAll(packDir, mods, numMods) != 0 || Place(PLACE_ARENA) != 0) {
        FreeBlobs();
        return -1;
    }
    for (i = 0; i < sNumBlobs; i++) {
        memcpy((void *)(uintptr_t)sBlobs[i].at, sBlobs[i].data, sBlobs[i].size);
        if (sBlobs[i].at + sBlobs[i].size - RES_ARENA_BASE > used)
            used = sBlobs[i].at + sBlobs[i].size - RES_ARENA_BASE;
    }
    for (k = 0; k < sNumKinds; k++) {
        const struct Kind *K = &sKinds[k];
        const struct Blob *t = FindBlob(k, 0, ROLE_TABLE);
        u8 layer = t->layer;

        for (i = 0; i < sNumBlobs; i++)
            if (sBlobs[i].kind == k && sBlobs[i].layer > layer)
                layer = sBlobs[i].layer;
        Res_SetSlot(K->slot, (void *)(uintptr_t)t->at, layer);
        /* So PortPatchRomFunctionPointers reaches the served copy (obj_desc's
         * start functions), and so Res_ServedAddr answers for the table. */
        ResAddRelocation(K->addr, t->size, (void *)(uintptr_t)t->at);
        PortLog("[res] %-11s served from the %s at 0x%08X", K->kind,
                layer == RES_LAYER_MOD ? "mods" : "pack", (unsigned)t->at);
    }
    PortLog("[res] pack %s installed: %u blobs, %u bytes of arena", packDir,
            (unsigned)sNumBlobs, (unsigned)used);
    LogContentHashes();
    ResSetArenaTop(RES_ARENA_BASE + ((used + 15u) & ~15u));
    FreeBlobs();
    return 0;
}

/* --- command line actions ----------------------------------------------------------- */

static int sCliAction;
static const char *sCliDir;

void Res_SetCliAction(int action, const char *dir)
{
    sCliAction = action;
    sCliDir = dir;
}

int Res_RunCliAction(void)
{
    switch (sCliAction) {
    case RES_CLI_EXTRACT:
        return ResExtract(sCliDir) == 0 ? 0 : 1;
    case RES_CLI_ROUNDTRIP:
        return ResRoundTrip(sCliDir) == 0 ? 0 : 1;
    default:
        return -1;
    }
}

#ifdef __cplusplus
}
#endif
