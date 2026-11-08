/* Force-included last by tools/lint_determinism.py: gRngVal as a plain
 * variable instead of *(u32 *)0x030068D8, so the AST names it. */
#undef gRngVal
extern unsigned int gRngVal;
