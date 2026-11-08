/* Self-test for the code-ID lint (tools/codemod_fnid.py --check), run by
 * `make lint-determinism`.  Every line marked flag must be reported and no
 * other line may be.  Parsed with the game's flags; never compiled. */

#define FNID_SelfA 0x0E000001u
#define FNID_SelfB 0x0E000003u

struct SelfObj {
    void (*fn)(struct SelfObj *);
    void *any;
};

typedef void (*SelfFn)(struct SelfObj *);

void SelfA(struct SelfObj *o);
void SelfB(struct SelfObj *o);
void SelfTake(SelfFn f);
SelfFn SelfGet(void);

#define SELF_RUN(o) (o)->fn(o)

static const SelfFn sRaw[] = { SelfA, SelfB };          /* flag */
static const SelfFn sWrapped[] = { FN(SelfA), FN(SelfB) };

SelfFn SelfReturn(void)
{
    return SelfA;                                       /* flag */
}

void SelfBody(struct SelfObj *o, int i)
{
    SelfFn local;

    o->fn = SelfA;                                      /* flag */
    o->fn = &SelfB;                                     /* flag */
    o->any = (void *)SelfA;                             /* flag */
    SelfTake(SelfB);                                    /* flag */
    if (o->fn == SelfA)                                 /* flag */
        o->fn = FN(SelfB);
    if (o->fn == FN(SelfA))
        SelfA(o);
    (*SelfA)(o);
    o->fn(o);                                           /* flag */
    (*o->fn)(o);                                        /* flag */
    sRaw[i](o);                                         /* flag */
    SelfGet()(o);                                       /* flag */
    ((void (*)(struct SelfObj *))o->any)(o);            /* flag */
    SELF_RUN(o);                                        /* flag */
    CALL(o->fn)(o);
    CALL(sWrapped[i])(o);
    CALL((void (*)(struct SelfObj *))o->any)(o);
    local = FN(SelfA);
    CALL(local)(o);
    (void)sizeof(SelfA);
}

PORT_FNID_TABLE(tests_lint_fnid_selftest) {
    PORT_FNID(SelfA),
    PORT_FNID(SelfB),
    PORT_FNID_END
};
