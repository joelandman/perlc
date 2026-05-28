#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* Called automatically via constructor attribute when eval_jit.o is linked.
   May also be called explicitly to pre-warm the JIT. */
void perl_eval_init(void);
#ifdef __cplusplus
}
#endif
