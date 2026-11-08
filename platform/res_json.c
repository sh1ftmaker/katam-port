/* JSON reader and output buffer for the resource layer.  See res_json.h. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "res_json.h"

#ifdef __cplusplus
extern "C" {
#endif

struct P {
    const char *s, *end, *start;
    char *err;
    size_t errLen;
    int failed;
};

static void Fail(struct P *p, const char *what)
{
    if (!p->failed && p->err && p->errLen)
        snprintf(p->err, p->errLen, "%s at byte %ld", what, (long)(p->s - p->start));
    p->failed = 1;
}

static void Ws(struct P *p)
{
    while (p->s < p->end && (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r'))
        p->s++;
}

static JVal *New(int type)
{
    JVal *v = (JVal *)calloc(1, sizeof *v);

    if (v)
        v->type = type;
    return v;
}

static int Push(JVal *v, JVal *item, char *key)
{
    JVal **ni = (JVal **)realloc(v->items, (v->n + 1) * sizeof *ni);

    if (!ni)
        return 0;
    v->items = ni;
    if (v->type == J_OBJ) {
        char **nk = (char **)realloc(v->keys, (v->n + 1) * sizeof *nk);

        if (!nk)
            return 0;
        v->keys = nk;
        v->keys[v->n] = key;
    }
    v->items[v->n++] = item;
    return 1;
}

static char *String(struct P *p)
{
    Buf b = { 0 };

    if (p->s >= p->end || *p->s != '"') {
        Fail(p, "expected a string");
        return NULL;
    }
    p->s++;
    while (p->s < p->end && *p->s != '"') {
        char c = *p->s++;

        if (c == '\\') {
            if (p->s >= p->end)
                break;
            c = *p->s++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned u = 0;

                if (p->end - p->s < 4 || sscanf(p->s, "%4x", &u) != 1 || u > 0x7F) {
                    Fail(p, "unsupported \\u escape");
                    BufFree(&b);
                    return NULL;
                }
                p->s += 4;
                c = (char)u;
                break;
            }
            default: break;      /* \" \\ \/ */
            }
        }
        BufPut(&b, &c, 1);
    }
    if (p->s >= p->end) {
        Fail(p, "unterminated string");
        BufFree(&b);
        return NULL;
    }
    p->s++;
    BufPut(&b, "", 1);
    return b.p;
}

static JVal *Value(struct P *p, int depth)
{
    JVal *v;

    Ws(p);
    if (p->s >= p->end || depth > 64) {
        Fail(p, "expected a value");
        return NULL;
    }
    switch (*p->s) {
    case '{':
    case '[': {
        int obj = *p->s == '{';

        p->s++;
        v = New(obj ? J_OBJ : J_ARR);
        if (!v)
            return NULL;
        Ws(p);
        if (p->s < p->end && *p->s == (obj ? '}' : ']')) {
            p->s++;
            return v;
        }
        for (;;) {
            char *key = NULL;
            JVal *item;

            if (obj) {
                Ws(p);
                key = String(p);
                Ws(p);
                if (!key || p->s >= p->end || *p->s != ':') {
                    free(key);
                    Fail(p, "expected ':'");
                    JFree(v);
                    return NULL;
                }
                p->s++;
            }
            item = Value(p, depth + 1);
            if (!item || !Push(v, item, key)) {
                free(key);
                JFree(item);
                JFree(v);
                return NULL;
            }
            Ws(p);
            if (p->s < p->end && *p->s == ',') {
                p->s++;
                continue;
            }
            if (p->s < p->end && *p->s == (obj ? '}' : ']')) {
                p->s++;
                return v;
            }
            Fail(p, obj ? "expected ',' or '}'" : "expected ',' or ']'");
            JFree(v);
            return NULL;
        }
    }
    case '"':
        v = New(J_STR);
        if (v && !(v->str = String(p))) {
            JFree(v);
            return NULL;
        }
        return v;
    case 't':
        if (p->end - p->s >= 4 && !memcmp(p->s, "true", 4)) { p->s += 4; return New(J_TRUE); }
        break;
    case 'f':
        if (p->end - p->s >= 5 && !memcmp(p->s, "false", 5)) { p->s += 5; return New(J_FALSE); }
        break;
    case 'n':
        if (p->end - p->s >= 4 && !memcmp(p->s, "null", 4)) { p->s += 4; return New(J_NULL); }
        break;
    default: {
        long long n = 0;
        int neg = 0, digits = 0;

        if (*p->s == '-') {
            neg = 1;
            p->s++;
        }
        while (p->s < p->end && *p->s >= '0' && *p->s <= '9') {
            n = n * 10 + (*p->s++ - '0');
            digits++;
        }
        if (!digits) {
            Fail(p, "unexpected character");
            return NULL;
        }
        if (p->s < p->end && (*p->s == '.' || *p->s == 'e' || *p->s == 'E')) {
            Fail(p, "floats are not allowed in pack JSON");
            return NULL;
        }
        v = New(J_INT);
        if (v)
            v->num = neg ? -n : n;
        return v;
    }
    }
    Fail(p, "unexpected character");
    return NULL;
}

JVal *JParse(const char *text, size_t len, char *err, size_t errLen)
{
    struct P p;
    JVal *v;

    p.s = p.start = text;
    p.end = text + len;
    p.err = err;
    p.errLen = errLen;
    p.failed = 0;
    if (err && errLen)
        err[0] = '\0';
    v = Value(&p, 0);
    if (v) {
        Ws(&p);
        if (p.s != p.end) {
            Fail(&p, "trailing characters");
            JFree(v);
            v = NULL;
        }
    }
    return v;
}

void JFree(JVal *v)
{
    u32 i;

    if (!v)
        return;
    for (i = 0; i < v->n; i++) {
        JFree(v->items[i]);
        if (v->keys)
            free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->str);
    free(v);
}

const JVal *JGet(const JVal *obj, const char *key)
{
    u32 i;

    if (!obj || obj->type != J_OBJ)
        return NULL;
    for (i = 0; i < obj->n; i++)
        if (!strcmp(obj->keys[i], key))
            return obj->items[i];
    return NULL;
}

const JVal *JAt(const JVal *arr, u32 i)
{
    return arr && arr->type == J_ARR && i < arr->n ? arr->items[i] : NULL;
}

long long JInt(const JVal *obj, const char *key, long long dflt)
{
    const JVal *v = JGet(obj, key);

    return v && v->type == J_INT ? v->num : dflt;
}

const char *JStr(const JVal *obj, const char *key)
{
    const JVal *v = JGet(obj, key);

    return v && v->type == J_STR ? v->str : NULL;
}

/* --- edits ------------------------------------------------------------------ */

JVal *JClone(const JVal *v)
{
    JVal *c;
    u32 i;

    if (!v)
        return NULL;
    c = New(v->type);
    if (!c)
        return NULL;
    c->num = v->num;
    if (v->str) {
        c->str = (char *)malloc(strlen(v->str) + 1);
        if (c->str)
            strcpy(c->str, v->str);
    }
    for (i = 0; i < v->n; i++) {
        char *key = NULL;

        if (v->keys) {
            key = (char *)malloc(strlen(v->keys[i]) + 1);
            if (key)
                strcpy(key, v->keys[i]);
        }
        Push(c, JClone(v->items[i]), key);
    }
    return c;
}

/* The slot at `path`: a pointer to the JVal* that holds it, or NULL. */
static JVal **Slot(JVal **root, const char *path)
{
    JVal **cur = root;
    char seg[64];

    while (*path) {
        size_t n = strcspn(path, "/");
        JVal *v = *cur;
        u32 i;

        if (n >= sizeof seg || !v)
            return NULL;
        memcpy(seg, path, n);
        seg[n] = '\0';
        path += n + (path[n] == '/');
        if (v->type == J_ARR) {
            char *end;
            unsigned long idx = strtoul(seg, &end, 10);

            if (*end || idx >= v->n)
                return NULL;
            cur = &v->items[idx];
        } else if (v->type == J_OBJ) {
            for (i = 0; i < v->n; i++)
                if (!strcmp(v->keys[i], seg))
                    break;
            if (i == v->n)
                return NULL;
            cur = &v->items[i];
        } else {
            return NULL;
        }
    }
    return cur;
}

int JSet(JVal *root, const char *path, const JVal *value)
{
    JVal *r = root, **slot;
    JVal *c;

    if (!*path)
        return -1;                       /* the root is replaced by a file, not a patch */
    slot = Slot(&r, path);
    if (!slot || !(c = JClone(value)))
        return -1;
    JFree(*slot);
    *slot = c;
    return 0;
}

static int MergeInto(JVal *dst, const JVal *src)
{
    u32 i, j;

    for (i = 0; i < src->n; i++) {
        for (j = 0; j < dst->n; j++)
            if (!strcmp(dst->keys[j], src->keys[i]))
                break;
        if (j < dst->n && dst->items[j]->type == J_OBJ && src->items[i]->type == J_OBJ) {
            if (MergeInto(dst->items[j], src->items[i]) != 0)
                return -1;
        } else if (j < dst->n) {
            JVal *c = JClone(src->items[i]);

            if (!c)
                return -1;
            JFree(dst->items[j]);
            dst->items[j] = c;
        } else {
            char *key = (char *)malloc(strlen(src->keys[i]) + 1);

            if (!key)
                return -1;
            strcpy(key, src->keys[i]);
            if (!Push(dst, JClone(src->items[i]), key))
                return -1;
        }
    }
    return 0;
}

int JMerge(JVal *root, const char *path, const JVal *value)
{
    JVal *r = root, **slot = Slot(&r, path);

    if (!slot || !*slot || (*slot)->type != J_OBJ || !value || value->type != J_OBJ)
        return -1;
    return MergeInto(*slot, value);
}

/* --- output buffer -------------------------------------------------------- */

void BufPut(Buf *b, const void *data, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char *np;

        while (cap < b->len + n + 1)
            cap *= 2;
        np = (char *)realloc(b->p, cap);
        if (!np)
            return;
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void BufPrintf(Buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof tmp) {
        BufPut(b, tmp, (size_t)n);
    } else {
        char *big = (char *)malloc((size_t)n + 1);

        if (!big)
            return;
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        BufPut(b, big, (size_t)n);
        free(big);
    }
}

void BufFree(Buf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

#ifdef __cplusplus
}
#endif
