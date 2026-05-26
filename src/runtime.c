#define PCRE2_CODE_UNIT_WIDTH 8
/* Force-inline helpers: HOT for file-private fns, HOTX for exported fns */
#define HOT  __attribute__((always_inline)) static inline
#define HOTX __attribute__((always_inline))
#include <pcre2.h>
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <setjmp.h>
#include <pthread.h>
#include <semaphore.h>

/* ── PerlValue freelist pool ─────────────────────────────────────────────── *
 * Avoids malloc/free per temp: freed PVs go onto a singly-linked list (next
 * pointer stored in pval union field), re-used on the next alloc. The pool
 * stabilises quickly at the peak concurrent-live count for the hot loop.
 */
/* Per-thread freelist: no mutex needed since each thread has its own */
static __thread PerlValue *pv_freelist_ = NULL;

static inline PerlValue *pv_alloc(void) {
    if (__builtin_expect(pv_freelist_ != NULL, 1)) {
        PerlValue *v  = pv_freelist_;
        pv_freelist_  = (PerlValue *)v->pval;
        return v;
    }
    /* calloc zeroes flags (and all other fields) on fresh allocation */
    return calloc(1, sizeof(PerlValue));
}

static inline void pv_pool_push(PerlValue *v) {
    v->pval      = pv_freelist_;
    pv_freelist_ = v;
}

/* ── local() save/restore stack ─────────────────────────────────────────── */

#define LOCAL_STACK_MAX 256
#define LOCAL_SCALAR  0   /* save/restore a PerlValue (existing behaviour) */
#define LOCAL_LOCK_PV 1   /* auto-unlock a PerlSharedVar on scope exit */
#define LOCAL_LOCK_AV 2   /* auto-unlock a PerlArray->mu on scope exit */
#define LOCAL_LOCK_HV 3   /* auto-unlock a PerlHash->mu on scope exit */
typedef struct { int type; PerlValue *ptr; PerlValue saved; } LocalEntry;
static __thread LocalEntry s_local_stack[LOCAL_STACK_MAX];
static __thread int        s_local_depth = 0;

int perl_local_save_depth(void) { return s_local_depth; }

void perl_local_save(PerlValue *pv) {
    if (s_local_depth >= LOCAL_STACK_MAX) return;
    s_local_stack[s_local_depth].type  = LOCAL_SCALAR;
    s_local_stack[s_local_depth].ptr   = pv;
    s_local_stack[s_local_depth].saved = *pv;
    /* deep-copy string/blessed_class so the saved value is independent */
    if (pv->tag == PERL_STRING && pv->sval)
        s_local_stack[s_local_depth].saved.sval = strdup(pv->sval);
    if (pv->blessed_class)
        s_local_stack[s_local_depth].saved.blessed_class = strdup(pv->blessed_class);
    s_local_depth++;
}

void perl_local_restore_to(int depth) {
    while (s_local_depth > depth) {
        s_local_depth--;
        LocalEntry *e = &s_local_stack[s_local_depth];
        if (e->type == LOCAL_LOCK_PV) {
            pthread_mutex_unlock(&((PerlSharedVar *)e->ptr)->mu);
        } else if (e->type == LOCAL_LOCK_AV) {
            pthread_mutex_unlock(((PerlArray *)e->ptr)->mu);
        } else if (e->type == LOCAL_LOCK_HV) {
            pthread_mutex_unlock(((PerlHash *)e->ptr)->mu);
        } else { /* LOCAL_SCALAR */
            /* free any existing string/blessed_class in the target */
            if ((e->ptr->tag == PERL_STRING) && e->ptr->sval) free(e->ptr->sval);
            if (e->ptr->blessed_class) free(e->ptr->blessed_class);
            *e->ptr = e->saved;  /* restore saved value */
        }
    }
}

/* ── eval / $@ support ───────────────────────────────────────────────────── */

/* jmp_buf pointers are pushed by callers (codegen allocates jmp_buf on stack) */
#define EVAL_STACK_MAX 64
static __thread jmp_buf *s_eval_stack[EVAL_STACK_MAX];
static __thread int      s_eval_depth = 0;
static __thread PerlValue s_dollar_at; /* $@ — zero-initialized = UNDEF per thread */

/* $/ — input record separator (default "\n", undef = slurp mode) */
static PerlValue s_input_sep = { .tag = PERL_STRING };
static int       s_input_sep_inited = 0;

static void ensure_input_sep(void) {
    if (!s_input_sep_inited) {
        s_input_sep.tag  = PERL_STRING;
        s_input_sep.sval = strdup("\n");
        s_input_sep_inited = 1;
    }
}

/* $/ getter — returns stable pointer so local $/ works */
PerlValue *perl_get_input_sep(void) {
    ensure_input_sep();
    return &s_input_sep;
}

/* $. — current input line number */
static PerlValue s_dollar_dot   = { .tag = PERL_INT,   .ival = 0 };
/* $, — output field separator (undef = no separator) */
static PerlValue s_dollar_comma = { .tag = PERL_UNDEF };
/* $\ — output record separator (undef = no separator) */
static PerlValue s_dollar_bsl   = { .tag = PERL_UNDEF };
/* $& — last successful regex match string */
static PerlValue s_dollar_amp   = { .tag = PERL_UNDEF };

PerlValue *perl_get_dollar_dot(void)   { return &s_dollar_dot;   }
PerlValue *perl_get_dollar_comma(void) { return &s_dollar_comma; }
PerlValue *perl_get_dollar_bsl(void)   { return &s_dollar_bsl;   }
PerlValue *perl_get_dollar_amp(void)   { return &s_dollar_amp;   }

void perl_print_sep(void) {
    if (s_dollar_comma.tag == PERL_UNDEF) return;
    perl_print(&s_dollar_comma);
}
void perl_print_sep_fh(PerlValue *fh) {
    if (s_dollar_comma.tag == PERL_UNDEF) return;
    perl_print_fh(fh, &s_dollar_comma);
}
void perl_print_ors(void) {
    if (s_dollar_bsl.tag == PERL_UNDEF) return;
    perl_print(&s_dollar_bsl);
}
void perl_print_ors_fh(PerlValue *fh) {
    if (s_dollar_bsl.tag == PERL_UNDEF) return;
    perl_print_fh(fh, &s_dollar_bsl);
}

/* $! — errno as a string */
static PerlValue s_dollar_bang = { .tag = PERL_UNDEF };
PerlValue *perl_get_dollar_bang(void) {
    if (s_dollar_bang.tag == PERL_STRING && s_dollar_bang.sval) free(s_dollar_bang.sval);
    if (errno) {
        s_dollar_bang.tag  = PERL_STRING;
        s_dollar_bang.sval = strdup(strerror(errno));
    } else {
        s_dollar_bang.tag  = PERL_STRING;
        s_dollar_bang.sval = strdup("");
    }
    return &s_dollar_bang;
}

/* wantarray — returns 1 in list context, 0 in scalar; stub always 0 */
static __thread int s_wantarray_stack[64];
static __thread int s_wantarray_depth = 0;

int perl_push_wantarray(int ctx) {
  if (s_wantarray_depth < 64) s_wantarray_stack[s_wantarray_depth++] = ctx;
  return ctx;
}
int perl_pop_wantarray(void) {
  if (s_wantarray_depth > 0) s_wantarray_depth--;
  return s_wantarray_depth ? s_wantarray_stack[s_wantarray_depth] : 0;
}
PerlValue *perl_wantarray(void) {
  return perl_alloc_int(s_wantarray_stack[s_wantarray_depth - 1]);
}

/* caller — stub returning (package, file, line) */
PerlArray *perl_caller(void) {
    PerlArray *a = perl_array_new();
    perl_array_push(a, perl_alloc_string("main"));
    perl_array_push(a, perl_alloc_string("unknown"));
    perl_array_push(a, perl_alloc_int(0));
    return a;
}

void perl_eval_push(jmp_buf *jb) {
    if (s_eval_depth < EVAL_STACK_MAX)
        s_eval_stack[s_eval_depth++] = jb;
}

void perl_eval_pop(void) {
    if (s_eval_depth > 0) s_eval_depth--;
}

PerlValue *perl_get_dollar_at(void) { return &s_dollar_at; }

/* ── allocation ──────────────────────────────────────────────────────────── */

HOTX PerlValue *perl_alloc_undef(void) {
    PerlValue *v = pv_alloc();
    v->tag = PERL_UNDEF;
    v->ival = 0;
    v->matchpos = 0;
    v->blessed_class = NULL;
    return v;
}

HOTX PerlValue *perl_alloc_int(long long n) {
    PerlValue *v = pv_alloc();
    v->tag = PERL_INT;
    v->ival = n;
    v->matchpos = 0;
    v->blessed_class = NULL;
    return v;
}

HOTX PerlValue *perl_alloc_float(double f) {
    PerlValue *v = pv_alloc();
    v->tag = PERL_FLOAT;
    v->fval = f;
    v->matchpos = 0;
    v->blessed_class = NULL;
    return v;
}

PerlValue *perl_alloc_flat_array(long long n) {
    PerlValue *v = pv_alloc();
    v->tag = PERL_FLAT_ARRAY;
    v->pval = n > 0 ? malloc(sizeof(double) * (size_t)n) : NULL;
    v->matchpos = n;
    v->blessed_class = NULL;
    return v;
}

/* Returns 1 if every element of av is a PERL_FLAT_ARRAY PV, 0 otherwise.
   Used by Stage 23 codegen to guard flat-only loop specializations. */
long long perl_array_is_all_flat(PerlArray *av) {
    if (!av) return 0;
    for (long long i = 0; i < av->len; i++) {
        PerlValue *pv = av->elems[i];
        if (!pv || pv->tag != PERL_FLAT_ARRAY) return 0;
    }
    return 1;
}

PerlValue *perl_alloc_string(const char *s) {
    PerlValue *v = pv_alloc();
    v->tag = PERL_STRING;
    v->sval = strdup(s ? s : "");
    v->matchpos = 0;
    v->blessed_class = NULL;
    return v;
}

PerlValue *perl_clone(const PerlValue *src) {
    if (!src) return perl_alloc_undef();
    if (src->tag == PERL_STRING) {
        PerlValue *v = perl_alloc_string(src->sval);
        v->blessed_class = src->blessed_class ? strdup(src->blessed_class) : NULL;
        return v;
    }
    if (src->tag == PERL_FLAT_ARRAY) {
        long long n = src->matchpos;
        PerlValue *v = pv_alloc();
        v->tag = PERL_FLAT_ARRAY;
        v->matchpos = n;
        v->blessed_class = src->blessed_class ? strdup(src->blessed_class) : NULL;
        v->pval = n > 0 ? malloc(sizeof(double) * (size_t)n) : NULL;
        if (n > 0) memcpy(v->pval, src->pval, sizeof(double) * (size_t)n);
        return v;
    }
    PerlValue *v = pv_alloc();
    *v = *src;
    v->flags = 0;      /* clones are never shared — that's a property of the slot, not the value */
    v->matchpos = 0;
    v->blessed_class = src->blessed_class ? strdup(src->blessed_class) : NULL;
    if (src->tag == PERL_REF_ARRAY && src->pval) {
        PerlArray *av = (PerlArray *)src->pval;
        if (av->refcount > 0) av->refcount++;
    } else if (src->tag == PERL_REF_HASH && src->pval) {
        PerlHash *hv = (PerlHash *)src->pval;
        if (hv->refcount > 0) hv->refcount++;
    }
    return v;
}

HOTX void perl_free(PerlValue *v) {
    if (!v) return;
    /* Shared vars are PerlSharedVar (larger than PerlValue) — never pool them. */
    if (v->flags & PV_FLAG_SHARED) return;
    if (v->tag == PERL_STRING) free(v->sval);
    if (v->tag == PERL_FLAT_ARRAY) free(v->pval);
    if (v->tag == PERL_REF_ARRAY && v->pval) {
        PerlArray *av = (PerlArray *)v->pval;
        if (av->refcount > 0 && --av->refcount == 0) perl_array_free(av);
    }
    if (v->tag == PERL_REF_HASH && v->pval) {
        PerlHash *hv = (PerlHash *)v->pval;
        if (hv->refcount > 0 && --hv->refcount == 0) perl_hash_free(hv);
    }
    /* CODE_REF: pval points to a shared PerlClosure; don't free it here since
       perl_clone() shallow-copies the pval pointer — freeing it would dangle
       any other references to the same closure. */
    if (v->blessed_class) free(v->blessed_class);
    pv_pool_push(v);   /* return struct to freelist instead of free() */
}

/* ── coercions ───────────────────────────────────────────────────────────── */

__attribute__((pure)) HOTX long long perl_to_int(const PerlValue *v) {
    if (!v) return 0;
    switch (v->tag) {
        case PERL_INT:    return v->ival;
        case PERL_FLOAT:  return (long long)v->fval;
        case PERL_STRING: return atoll(v->sval);
        default:          return 0;
    }
}

__attribute__((pure)) HOTX double perl_to_float(const PerlValue *v) {
    if (!v) return 0.0;
    switch (v->tag) {
        case PERL_INT:    return (double)v->ival;
        case PERL_FLOAT:  return v->fval;
        case PERL_STRING: return atof(v->sval);
        default:          return 0.0;
    }
}

/* caller must free */
char *perl_to_string(const PerlValue *v) {
    if (!v || v->tag == PERL_UNDEF) return strdup("");
    char buf[64];
    switch (v->tag) {
        case PERL_INT:
            snprintf(buf, sizeof buf, "%lld", v->ival);
            return strdup(buf);
        case PERL_FLOAT:
            snprintf(buf, sizeof buf, "%g", v->fval);
            return strdup(buf);
        case PERL_STRING:
            return strdup(v->sval);
        case PERL_REF_SCALAR:
            if (v->blessed_class)
                snprintf(buf, sizeof buf, "%s=SCALAR(0x%llx)", v->blessed_class, (unsigned long long)(uintptr_t)v->pval);
            else
                snprintf(buf, sizeof buf, "SCALAR(0x%llx)", (unsigned long long)(uintptr_t)v->pval);
            return strdup(buf);
        case PERL_REF_ARRAY:
            if (v->blessed_class)
                snprintf(buf, sizeof buf, "%s=ARRAY(0x%llx)", v->blessed_class, (unsigned long long)(uintptr_t)v->pval);
            else
                snprintf(buf, sizeof buf, "ARRAY(0x%llx)", (unsigned long long)(uintptr_t)v->pval);
            return strdup(buf);
        case PERL_REF_HASH:
            if (v->blessed_class)
                snprintf(buf, sizeof buf, "%s=HASH(0x%llx)", v->blessed_class, (unsigned long long)(uintptr_t)v->pval);
            else
                snprintf(buf, sizeof buf, "HASH(0x%llx)", (unsigned long long)(uintptr_t)v->pval);
            return strdup(buf);
        case PERL_FILEHANDLE:
            snprintf(buf, sizeof buf, "GLOB(0x%llx)", (unsigned long long)(uintptr_t)v->pval);
            return strdup(buf);
        default:
            return strdup("");
    }
}

int perl_defined(const PerlValue *v) {
    return v && v->tag != PERL_UNDEF;
}

int perl_is_true(const PerlValue *v) {
    if (!v || v->tag == PERL_UNDEF) return 0;
    switch (v->tag) {
        case PERL_INT:    return v->ival != 0;
        case PERL_FLOAT:  return v->fval != 0.0;
        case PERL_STRING:
            return !(v->sval[0] == '\0' ||
                     (v->sval[0] == '0' && v->sval[1] == '\0'));
        case PERL_REF_SCALAR:
        case PERL_REF_ARRAY:
        case PERL_REF_HASH:
        case PERL_FLAT_ARRAY:
            return 1;
        case PERL_FILEHANDLE:
            return v->pval != NULL;
        default: return 0;
    }
}

HOTX void perl_assign(PerlValue *dst, const PerlValue *src) {
    if (!dst) return;
    int shared = dst->flags & PV_FLAG_SHARED;
    /* No implicit mutex here — caller must hold lock() for concurrent safety.
       Locking inside perl_assign would deadlock when lock() is already held. */
    /* Acquire new reference first (handles self-assignment safely) */
    if (src && src->tag == PERL_REF_ARRAY && src->pval) {
        PerlArray *av = (PerlArray *)src->pval;
        if (av->refcount > 0) av->refcount++;
    } else if (src && src->tag == PERL_REF_HASH && src->pval) {
        PerlHash *hv = (PerlHash *)src->pval;
        if (hv->refcount > 0) hv->refcount++;
    }
    /* Release old value */
    if (dst->tag == PERL_STRING) { free(dst->sval); dst->sval = NULL; }
    if (dst->tag == PERL_FLAT_ARRAY && dst->pval) { free(dst->pval); dst->pval = NULL; }
    if (dst->tag == PERL_REF_ARRAY && dst->pval) {
        PerlArray *av = (PerlArray *)dst->pval;
        if (av->refcount > 0 && --av->refcount == 0) perl_array_free(av);
    }
    if (dst->tag == PERL_REF_HASH && dst->pval) {
        PerlHash *hv = (PerlHash *)dst->pval;
        if (hv->refcount > 0 && --hv->refcount == 0) perl_hash_free(hv);
    }
    if (dst->blessed_class) { free(dst->blessed_class); dst->blessed_class = NULL; }
    if (!src) { dst->tag = PERL_UNDEF; dst->ival = 0; dst->matchpos = 0; return; }
    *dst = *src;
    dst->flags = (unsigned int)shared;  /* restore shared flag — *dst = *src clobbered it */
    if (src->tag == PERL_STRING) {
        dst->sval = strdup(src->sval);
        dst->matchpos = 0;
    } else if (src->tag == PERL_FLAT_ARRAY && src->pval) {
        /* Deep-copy the double[] so src and dst each own their own buffer.
           matchpos is the element count for FLAT_ARRAY — must NOT be zeroed. */
        long long n = src->matchpos;
        double *copy = (double *)malloc((size_t)n * sizeof(double));
        memcpy(copy, (double *)src->pval, (size_t)n * sizeof(double));
        dst->pval = copy;
    } else {
        dst->matchpos = 0;
    }
    dst->blessed_class = src->blessed_class ? strdup(src->blessed_class) : NULL;
    /* Note: the refcount increment above already accounts for dst's ownership of pval */
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

HOT int both_int(const PerlValue *a, const PerlValue *b) {
    return a->tag == PERL_INT && b->tag == PERL_INT;
}

/* ── arithmetic ──────────────────────────────────────────────────────────── */

HOTX PerlValue *perl_add(const PerlValue *a, const PerlValue *b) {
    if (both_int(a, b)) return perl_alloc_int(a->ival + b->ival);
    return perl_alloc_float(perl_to_float(a) + perl_to_float(b));
}

HOTX PerlValue *perl_sub(const PerlValue *a, const PerlValue *b) {
    if (both_int(a, b)) return perl_alloc_int(a->ival - b->ival);
    return perl_alloc_float(perl_to_float(a) - perl_to_float(b));
}

HOTX PerlValue *perl_mul(const PerlValue *a, const PerlValue *b) {
    if (both_int(a, b)) return perl_alloc_int(a->ival * b->ival);
    return perl_alloc_float(perl_to_float(a) * perl_to_float(b));
}

HOTX PerlValue *perl_div(const PerlValue *a, const PerlValue *b) {
    double bv = perl_to_float(b);
    if (bv == 0.0) { fprintf(stderr, "Illegal division by zero\n"); exit(1); }
    if (both_int(a, b) && a->ival % b->ival == 0)
        return perl_alloc_int(a->ival / b->ival);
    return perl_alloc_float(perl_to_float(a) / bv);
}

PerlValue *perl_mod(const PerlValue *a, const PerlValue *b) {
    long long bv = perl_to_int(b);
    if (bv == 0) { fprintf(stderr, "Illegal modulus zero\n"); exit(1); }
    return perl_alloc_int(perl_to_int(a) % bv);
}

PerlValue *perl_pow(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_float(pow(perl_to_float(a), perl_to_float(b)));
}

PerlValue *perl_negate(const PerlValue *a) {
    if (a->tag == PERL_INT)   return perl_alloc_int(-a->ival);
    if (a->tag == PERL_FLOAT) return perl_alloc_float(-a->fval);
    return perl_alloc_float(-perl_to_float(a));
}

/* ── string ops ──────────────────────────────────────────────────────────── */

PerlValue *perl_concat(const PerlValue *a, const PerlValue *b) {
    char *sa = perl_to_string(a);
    char *sb = perl_to_string(b);
    size_t len = strlen(sa) + strlen(sb) + 1;
    char *buf = malloc(len);
    strcpy(buf, sa); strcat(buf, sb);
    PerlValue *r = perl_alloc_string(buf);
    free(sa); free(sb); free(buf);
    return r;
}

PerlValue *perl_repeat_str(const PerlValue *str, const PerlValue *n) {
    char *s = perl_to_string(str);
    long long reps = perl_to_int(n);
    if (reps <= 0) { free(s); return perl_alloc_string(""); }
    size_t slen = strlen(s);
    char *buf = malloc(slen * reps + 1);
    buf[0] = '\0';
    for (long long i = 0; i < reps; i++) strcat(buf, s);
    PerlValue *r = perl_alloc_string(buf);
    free(s); free(buf);
    return r;
}

/* ── numeric comparisons ─────────────────────────────────────────────────── */

HOTX PerlValue *perl_num_eq(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_to_float(a) == perl_to_float(b));
}
HOTX PerlValue *perl_num_ne(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_to_float(a) != perl_to_float(b));
}
HOTX PerlValue *perl_num_lt(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_to_float(a) <  perl_to_float(b));
}
HOTX PerlValue *perl_num_gt(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_to_float(a) >  perl_to_float(b));
}
HOTX PerlValue *perl_num_le(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_to_float(a) <= perl_to_float(b));
}
HOTX PerlValue *perl_num_ge(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_to_float(a) >= perl_to_float(b));
}

/* ── string comparisons ──────────────────────────────────────────────────── */

PerlValue *perl_str_eq(const PerlValue *a, const PerlValue *b) {
    char *sa = perl_to_string(a), *sb = perl_to_string(b);
    PerlValue *r = perl_alloc_int(strcmp(sa, sb) == 0);
    free(sa); free(sb); return r;
}
PerlValue *perl_str_ne(const PerlValue *a, const PerlValue *b) {
    char *sa = perl_to_string(a), *sb = perl_to_string(b);
    PerlValue *r = perl_alloc_int(strcmp(sa, sb) != 0);
    free(sa); free(sb); return r;
}
PerlValue *perl_str_lt(const PerlValue *a, const PerlValue *b) {
    char *sa = perl_to_string(a), *sb = perl_to_string(b);
    PerlValue *r = perl_alloc_int(strcmp(sa, sb) < 0);
    free(sa); free(sb); return r;
}
PerlValue *perl_str_gt(const PerlValue *a, const PerlValue *b) {
    char *sa = perl_to_string(a), *sb = perl_to_string(b);
    PerlValue *r = perl_alloc_int(strcmp(sa, sb) > 0);
    free(sa); free(sb); return r;
}
PerlValue *perl_str_le(const PerlValue *a, const PerlValue *b) {
    char *sa = perl_to_string(a), *sb = perl_to_string(b);
    PerlValue *r = perl_alloc_int(strcmp(sa, sb) <= 0);
    free(sa); free(sb); return r;
}
PerlValue *perl_str_ge(const PerlValue *a, const PerlValue *b) {
    char *sa = perl_to_string(a), *sb = perl_to_string(b);
    PerlValue *r = perl_alloc_int(strcmp(sa, sb) >= 0);
    free(sa); free(sb); return r;
}

/* ── logical ─────────────────────────────────────────────────────────────── */

PerlValue *perl_not(const PerlValue *a) {
    return perl_alloc_int(!perl_is_true(a));
}
PerlValue *perl_and(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_is_true(a) && perl_is_true(b));
}
PerlValue *perl_or(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_is_true(a) || perl_is_true(b));
}

/* ── I/O ─────────────────────────────────────────────────────────────────── */

void perl_print(const PerlValue *v) {
    char *s = perl_to_string(v);
    fputs(s, stdout);
    free(s);
}

void perl_say(const PerlValue *v) {
    perl_print(v);
    fputc('\n', stdout);
}

void perl_print_string(const char *s) {
    fputs(s, stdout);
}

/* ── inc/dec ─────────────────────────────────────────────────────────────── */

PerlValue *perl_inc(PerlValue *v) {
    if (v->tag == PERL_FLOAT) { v->fval += 1.0; return v; }
    if (v->tag == PERL_INT)   { v->ival++; return v; }
    if (v->tag == PERL_STRING && v->sval) {
        /* magical string increment: only for /^[a-zA-Z][a-zA-Z0-9]*$/ */
        const char *s = v->sval;
        size_t len = strlen(s);
        int magic = (len > 0);
        for (size_t i = 0; i < len && magic; i++) {
            char c = s[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (i > 0 && c >= '0' && c <= '9')))
                magic = 0;
        }
        if (magic) {
            char *buf = malloc(len + 2);
            memcpy(buf, s, len + 1);
            int carry = 1;
            for (long long i = (long long)len - 1; i >= 0 && carry; i--) {
                char c = buf[i];
                if (c >= 'a' && c <= 'z') {
                    if (c < 'z') { buf[i]++; carry = 0; }
                    else { buf[i] = 'a'; }
                } else if (c >= 'A' && c <= 'Z') {
                    if (c < 'Z') { buf[i]++; carry = 0; }
                    else { buf[i] = 'A'; }
                } else if (c >= '0' && c <= '9') {
                    if (c < '9') { buf[i]++; carry = 0; }
                    else { buf[i] = '0'; }
                }
            }
            if (carry) {
                char first = buf[0];
                char prefix = (first >= 'a' && first <= 'z') ? 'a'
                            : (first >= 'A' && first <= 'Z') ? 'A' : '1';
                memmove(buf + 1, buf, len + 1);
                buf[0] = prefix;
            }
            free(v->sval);
            v->sval = buf;
            return v;
        }
    }
    /* non-magic: convert to int 0 and increment */
    if (v->tag == PERL_STRING) { free(v->sval); v->sval = NULL; }
    v->tag = PERL_INT; v->ival = 1;
    return v;
}

PerlValue *perl_dec(PerlValue *v) {
    if (v->tag == PERL_FLOAT) { v->fval -= 1.0; }
    else { if (v->tag != PERL_INT) { v->tag = PERL_INT; v->ival = 0; } v->ival--; }
    return v;
}

/* ── arrays ──────────────────────────────────────────────────────────────── */

PerlArray *perl_array_new(void) {
    PerlArray *a = malloc(sizeof *a);
    a->len = 0; a->cap = 8; a->refcount = 0; a->mu = NULL;
    a->elems = malloc(a->cap * sizeof(PerlValue *));
    return a;
}

PerlArray *perl_anon_array_new(void) {
    PerlArray *a = perl_array_new();
    a->refcount = 1;
    return a;
}

void perl_array_free(PerlArray *a) {
    if (!a) return;
    for (long long i = 0; i < a->len; i++) perl_free(a->elems[i]);
    free(a->elems);
    if (a->mu) { pthread_mutex_destroy(a->mu); free(a->mu); }
    free(a);
}

void perl_array_make_shared(PerlArray *a) {
    if (!a || a->mu) return;
    a->mu = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(a->mu, NULL);
}

void perl_lock_array(PerlArray *a) {
    if (!a || !a->mu) return;
    pthread_mutex_lock(a->mu);
    if (s_local_depth < LOCAL_STACK_MAX) {
        s_local_stack[s_local_depth].type = LOCAL_LOCK_AV;
        s_local_stack[s_local_depth].ptr  = (PerlValue *)a;
        s_local_depth++;
    }
}

void perl_array_push(PerlArray *a, PerlValue *v) {
    if (a->len == a->cap) {
        a->cap *= 2;
        a->elems = realloc(a->elems, a->cap * sizeof(PerlValue *));
    }
    a->elems[a->len++] = perl_clone(v);
}

/* Like perl_array_push but preserves the original pointer for shared vars
   (no clone).  Used exclusively for closure capture arrays. */
void perl_array_push_capture(PerlArray *a, PerlValue *v) {
    if (a->len == a->cap) {
        a->cap *= 2;
        a->elems = realloc(a->elems, a->cap * sizeof(PerlValue *));
    }
    if (v && (v->flags & PV_FLAG_SHARED))
        a->elems[a->len++] = v;        /* shared: store original pointer */
    else
        a->elems[a->len++] = perl_clone(v);
}

PerlValue *perl_array_pop(PerlArray *a) {
    if (a->len == 0) return perl_alloc_undef();
    return a->elems[--a->len];
}

PerlValue *perl_array_get(PerlArray *a, long long idx) {
    if (idx < 0) idx += a->len;
    if (idx < 0 || idx >= a->len) return perl_alloc_undef();
    return perl_clone(a->elems[idx]);
}

/* Shared read-only sentinel returned by _ref functions on missing key/index.
 * Callers must never free or mutate this pointer. */
static PerlValue pv_undef_sentinel_ = { .tag = PERL_UNDEF };

/* Borrow-read: returns raw pointer into the array (no clone, no alloc).
 * Valid until the array is next modified. Never call perl_free on the result. */
__attribute__((pure)) HOTX PerlValue *perl_array_get_ref(PerlArray *a, long long idx) {
    if (idx < 0) idx += a->len;
    if (idx < 0 || idx >= a->len) return &pv_undef_sentinel_;
    return a->elems[idx];
}

void perl_array_set(PerlArray *a, long long idx, PerlValue *v) {
    if (idx < 0) idx += a->len;
    /* extend if needed */
    while (idx >= a->cap) {
        a->cap *= 2;
        a->elems = realloc(a->elems, a->cap * sizeof(PerlValue *));
    }
    while (a->len <= idx) a->elems[a->len++] = perl_alloc_undef();
    perl_free(a->elems[idx]);
    a->elems[idx] = perl_clone(v);
}

/* Mutate an existing array element's float value in-place without allocation.
   Falls back to perl_array_set for out-of-bounds indices. */
HOTX void perl_array_update_float(PerlArray *a, long long idx, double f) {
    if (idx < 0) idx += a->len;
    if (idx >= 0 && idx < a->len) {
        PerlValue *pv = a->elems[idx];
        if (pv->tag == PERL_STRING) { free(pv->sval); pv->sval = NULL; }
        pv->tag = PERL_FLOAT;
        pv->fval = f;
        return;
    }
    perl_array_set(a, idx, perl_alloc_float(f));
}

PerlValue *perl_array_len(PerlArray *a) {
    return perl_alloc_int(a->len);
}

void perl_array_extend(PerlArray *dst, PerlArray *src) {
    for (long long i = 0; i < src->len; i++)
        perl_array_push(dst, src->elems[i]);
}

void perl_array_extend_hash(PerlArray *dst, PerlHash *h) {
    for (int i = 0; i < PERL_HASH_BUCKETS; i++) {
        for (PerlHashEntry *e = h->buckets[i]; e; e = e->next) {
            PerlValue *kv = perl_alloc_string(e->key);
            perl_array_push(dst, kv);
            perl_free(kv);
            perl_array_push(dst, e->val);
        }
    }
}

static int cmp_str_pv(const void *a, const void *b) {
    char *sa = perl_to_string(*(PerlValue **)a);
    char *sb = perl_to_string(*(PerlValue **)b);
    int r = strcmp(sa, sb);
    free(sa); free(sb);
    return r;
}

void perl_array_sort_str(PerlArray *a) {
    if (a->len > 1)
        qsort(a->elems, (size_t)a->len, sizeof(PerlValue *), cmp_str_pv);
}

PerlValue *perl_array_shift(PerlArray *a) {
    if (a->len == 0) return perl_alloc_undef();
    PerlValue *v = a->elems[0];
    memmove(a->elems, a->elems + 1, (size_t)(a->len - 1) * sizeof(PerlValue *));
    a->len--;
    return v;
}

void perl_array_unshift(PerlArray *a, PerlValue *v) {
    if (a->len == a->cap) {
        a->cap *= 2;
        a->elems = realloc(a->elems, (size_t)a->cap * sizeof(PerlValue *));
    }
    memmove(a->elems + 1, a->elems, (size_t)a->len * sizeof(PerlValue *));
    a->elems[0] = perl_clone(v);
    a->len++;
}

/* ── string builtins ─────────────────────────────────────────────────────── */

long long perl_chomp_array(PerlArray *a) {
    if (!a) return 0;
    long long removed = 0;
    for (long long i = 0; i < a->len; i++)
        removed += perl_chomp(a->elems[i]);
    return removed;
}

long long perl_chomp(PerlValue *v) {
    if (!v) return 0;
    if (v->tag == PERL_STRING) {
        size_t len = strlen(v->sval);
        if (len > 0 && v->sval[len - 1] == '\n') {
            v->sval[len - 1] = '\0';
            return 1;
        }
        return 0;
    }
    /* numeric values: convert to string, chomp, store back */
    char *s = perl_to_string(v);
    size_t len = strlen(s);
    long long removed = 0;
    if (len > 0 && s[len - 1] == '\n') { s[len - 1] = '\0'; removed = 1; }
    if (v->tag == PERL_STRING) free(v->sval);
    v->tag = PERL_STRING;
    v->sval = s;
    return removed;
}

PerlValue *perl_chop(PerlValue *v) {
    char *s = perl_to_string(v);   /* newly heap-allocated */
    size_t len = strlen(s);
    PerlValue *removed;
    if (len > 0) {
        char buf[2] = { s[len - 1], '\0' };
        s[len - 1] = '\0';
        removed = perl_alloc_string(buf);
    } else {
        removed = perl_alloc_string("");
    }
    if (v->tag == PERL_STRING && v->sval) free(v->sval);
    v->tag  = PERL_STRING;
    v->sval = s;
    return removed;
}

PerlValue *perl_length(PerlValue *v) {
    if (!v || v->tag == PERL_UNDEF) return perl_alloc_int(0);
    char *s = perl_to_string(v);
    long long n = (long long)strlen(s);
    free(s);
    return perl_alloc_int(n);
}

/* common helper: clamp offset/len to string bounds */
static void substr_bounds(long long slen, long long *off, long long *n) {
    if (*off < 0) *off += slen;
    if (*off < 0) *off = 0;
    if (*off > slen) *off = slen;
    if (*n < 0 || *off + *n > slen) *n = slen - *off;
    if (*n < 0) *n = 0;
}

PerlValue *perl_substr2(PerlValue *str, PerlValue *off_v) {
    char *s  = perl_to_string(str);
    long long slen = (long long)strlen(s);
    long long off  = perl_to_int(off_v);
    long long n    = slen;
    substr_bounds(slen, &off, &n);
    PerlValue *r = perl_alloc_string(s + off);  /* NUL at s+off+n is fine — we'll truncate */
    /* actually we need to truncate at n chars */
    if (r->tag == PERL_STRING) r->sval[n] = '\0';  /* safe: perl_alloc_string strdup'd */
    /* Hmm, we can't truncate that way safely; build explicitly */
    perl_free(r);
    char *buf = malloc((size_t)n + 1);
    memcpy(buf, s + off, (size_t)n);
    buf[n] = '\0';
    r = perl_alloc_string(buf);
    free(buf); free(s);
    return r;
}

PerlValue *perl_substr3(PerlValue *str, PerlValue *off_v, PerlValue *len_v) {
    char *s  = perl_to_string(str);
    long long slen = (long long)strlen(s);
    long long off  = perl_to_int(off_v);
    long long n    = perl_to_int(len_v);
    substr_bounds(slen, &off, &n);
    char *buf = malloc((size_t)n + 1);
    memcpy(buf, s + off, (size_t)n);
    buf[n] = '\0';
    PerlValue *r = perl_alloc_string(buf);
    free(buf); free(s);
    return r;
}

PerlValue *perl_join(PerlValue *sep, PerlArray *arr) {
    char *ssep = perl_to_string(sep);
    size_t seplen = strlen(ssep);
    /* collect stringified parts */
    char **parts = arr->len ? malloc((size_t)arr->len * sizeof(char *)) : NULL;
    size_t total = 0;
    for (long long i = 0; i < arr->len; i++) {
        parts[i] = perl_to_string(arr->elems[i]);
        total += strlen(parts[i]);
    }
    if (arr->len > 1) total += seplen * (size_t)(arr->len - 1);
    char *buf = malloc(total + 1);
    size_t cur = 0;
    for (long long i = 0; i < arr->len; i++) {
        if (i > 0) { memcpy(buf + cur, ssep, seplen); cur += seplen; }
        size_t plen = strlen(parts[i]);
        memcpy(buf + cur, parts[i], plen); cur += plen;
        free(parts[i]);
    }
    buf[cur] = '\0';
    PerlValue *r = perl_alloc_string(buf);
    free(ssep); free(parts); free(buf);
    return r;
}

PerlArray *perl_split(PerlValue *sep, PerlValue *str) {
    char *s  = perl_to_string(str);
    char *sp = perl_to_string(sep);
    PerlArray *arr = perl_array_new();

    int ws_split = (strcmp(sp, " ") == 0 || strcmp(sp, "\\s+") == 0 ||
                    strcmp(sp, "\\s") == 0);
    if (ws_split) {
        /* split on runs of whitespace, trimming leading */
        char *p = s;
        while (isspace((unsigned char)*p)) p++;
        while (*p) {
            char *start = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            size_t len = (size_t)(p - start);
            char *elem = malloc(len + 1);
            memcpy(elem, start, len); elem[len] = '\0';
            PerlValue *v = perl_alloc_string(elem); free(elem);
            perl_array_push(arr, v); perl_free(v);
            while (isspace((unsigned char)*p)) p++;
        }
    } else if (strlen(sp) == 0) {
        /* split each character */
        for (char *p = s; *p; p++) {
            char buf[2] = {*p, '\0'};
            PerlValue *v = perl_alloc_string(buf);
            perl_array_push(arr, v); perl_free(v);
        }
    } else {
        /* handle simple escape sequences in the pattern */
        char real_sep[256]; size_t ri = 0;
        for (size_t i = 0; sp[i] && ri < sizeof(real_sep) - 1; i++) {
            if (sp[i] == '\\' && sp[i+1]) {
                i++;
                switch (sp[i]) {
                    case 'n': real_sep[ri++] = '\n'; break;
                    case 't': real_sep[ri++] = '\t'; break;
                    case 'r': real_sep[ri++] = '\r'; break;
                    default:  real_sep[ri++] = sp[i]; break;
                }
            } else { real_sep[ri++] = sp[i]; }
        }
        real_sep[ri] = '\0';
        size_t splen = strlen(real_sep);
        char *p = s;
        char *found;
        while (splen > 0 && (found = strstr(p, real_sep)) != NULL) {
            size_t len = (size_t)(found - p);
            char *elem = malloc(len + 1);
            memcpy(elem, p, len); elem[len] = '\0';
            PerlValue *v = perl_alloc_string(elem); free(elem);
            perl_array_push(arr, v); perl_free(v);
            p = found + splen;
        }
        PerlValue *v = perl_alloc_string(p);
        perl_array_push(arr, v); perl_free(v);
    }
    free(s); free(sp);
    return arr;
}

/* ── hash ────────────────────────────────────────────────────────────────── */

static unsigned int hash_str(const char *s) {
    unsigned int h = 5381;
    while (*s) h = ((h << 5) + h) ^ (unsigned char)*s++;
    return h % PERL_HASH_BUCKETS;
}

PerlHash *perl_hash_new(void) {
    PerlHash *h = calloc(1, sizeof *h);
    /* refcount=0: scope-managed (calloc zeros it) */
    return h;
}

PerlHash *perl_anon_hash_new(void) {
    PerlHash *h = perl_hash_new();
    h->refcount = 1;
    return h;
}

void perl_hash_free(PerlHash *h) {
    if (!h) return;
    for (int i = 0; i < PERL_HASH_BUCKETS; i++) {
        PerlHashEntry *e = h->buckets[i];
        while (e) {
            PerlHashEntry *next = e->next;
            free(e->key);
            perl_free(e->val);
            free(e);
            e = next;
        }
    }
    if (h->mu) { pthread_mutex_destroy(h->mu); free(h->mu); }
    free(h);
}

void perl_hash_make_shared(PerlHash *h) {
    if (!h || h->mu) return;
    h->mu = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(h->mu, NULL);
}

void perl_lock_hash(PerlHash *h) {
    if (!h || !h->mu) return;
    pthread_mutex_lock(h->mu);
    if (s_local_depth < LOCAL_STACK_MAX) {
        s_local_stack[s_local_depth].type = LOCAL_LOCK_HV;
        s_local_stack[s_local_depth].ptr  = (PerlValue *)h;
        s_local_depth++;
    }
}

static PerlHashEntry *hash_find(PerlHash *h, const char *key) {
    unsigned int b = hash_str(key);
    for (PerlHashEntry *e = h->buckets[b]; e; e = e->next)
        if (strcmp(e->key, key) == 0) return e;
    return NULL;
}

PerlValue *perl_hash_get_sv(PerlHash *h, PerlValue *key) {
    char *ks = perl_to_string(key);
    PerlHashEntry *e = hash_find(h, ks);
    free(ks);
    return e ? perl_clone(e->val) : perl_alloc_undef();
}

/* Borrow-read: returns raw pointer into hash bucket (no clone, no alloc).
 * Valid until the hash is next modified. Never call perl_free on the result. */
HOTX PerlValue *perl_hash_get_sv_ref(PerlHash *h, PerlValue *key) {
    char *ks = perl_to_string(key);
    PerlHashEntry *e = hash_find(h, ks);
    free(ks);
    return e ? e->val : &pv_undef_sentinel_;
}

/* Constant-key variants: key is a C string literal — no strdup/free needed. */
HOTX PerlValue *perl_hash_get_str_ref(PerlHash *h, const char *key) {
    PerlHashEntry *e = hash_find(h, key);
    return e ? e->val : &pv_undef_sentinel_;
}

void perl_hash_set_sv(PerlHash *h, PerlValue *key, PerlValue *val) {
    char *ks = perl_to_string(key);
    unsigned int b = hash_str(ks);
    PerlHashEntry *e = hash_find(h, ks);
    if (e) {
        perl_free(e->val);
        e->val = perl_clone(val);
        free(ks);
    } else {
        PerlHashEntry *ne = malloc(sizeof *ne);
        ne->key  = ks;
        ne->val  = perl_clone(val);
        ne->next = h->buckets[b];
        h->buckets[b] = ne;
        h->size++;
    }
}

HOTX void perl_hash_set_str(PerlHash *h, const char *key, PerlValue *val) {
    unsigned int b = hash_str(key);
    PerlHashEntry *e = hash_find(h, key);
    if (e) {
        perl_free(e->val);
        e->val = perl_clone(val);
    } else {
        PerlHashEntry *ne = malloc(sizeof *ne);
        ne->key  = strdup(key);
        ne->val  = perl_clone(val);
        ne->next = h->buckets[b];
        h->buckets[b] = ne;
        h->size++;
    }
}

int perl_hash_exists_sv(PerlHash *h, PerlValue *key) {
    char *ks = perl_to_string(key);
    int r = hash_find(h, ks) != NULL;
    free(ks);
    return r;
}

HOTX int perl_hash_exists_str(PerlHash *h, const char *key) {
    return hash_find(h, key) != NULL;
}

PerlValue *perl_hash_delete_str(PerlHash *h, const char *key) {
    unsigned int b = hash_str(key);
    PerlHashEntry **pp = &h->buckets[b];
    while (*pp) {
        PerlHashEntry *e = *pp;
        if (strcmp(e->key, key) == 0) {
            *pp = e->next;
            PerlValue *v = e->val;
            free(e->key); free(e);
            h->size--;
            return v;
        }
        pp = &e->next;
    }
    return perl_alloc_undef();
}

PerlValue *perl_hash_delete_sv(PerlHash *h, PerlValue *key) {
    char *ks = perl_to_string(key);
    unsigned int b = hash_str(ks);
    PerlHashEntry **pp = &h->buckets[b];
    while (*pp) {
        PerlHashEntry *e = *pp;
        if (strcmp(e->key, ks) == 0) {
            *pp = e->next;
            PerlValue *v = e->val;
            free(e->key); free(e);
            h->size--;
            free(ks);
            return v;
        }
        pp = &e->next;
    }
    free(ks);
    return perl_alloc_undef();
}

PerlArray *perl_hash_keys(PerlHash *h) {
    PerlArray *a = perl_array_new();
    for (int i = 0; i < PERL_HASH_BUCKETS; i++) {
        for (PerlHashEntry *e = h->buckets[i]; e; e = e->next) {
            PerlValue *kv = perl_alloc_string(e->key);
            perl_array_push(a, kv);
            perl_free(kv);
        }
    }
    return a;
}

PerlArray *perl_hash_values(PerlHash *h) {
    PerlArray *a = perl_array_new();
    for (int i = 0; i < PERL_HASH_BUCKETS; i++) {
        for (PerlHashEntry *e = h->buckets[i]; e; e = e->next)
            perl_array_push(a, e->val);
    }
    return a;
}

PerlValue *perl_hash_size(PerlHash *h) {
    return perl_alloc_int(h ? h->size : 0);
}

void perl_hash_from_list(PerlHash *h, PerlArray *list) {
    for (long long i = 0; i + 1 < list->len; i += 2)
        perl_hash_set_sv(h, list->elems[i], list->elems[i + 1]);
}

/* ── references ──────────────────────────────────────────────────────────── */

PerlValue *perl_ref_scalar(PerlValue *v) {
    PerlValue *r = pv_alloc();
    r->tag = PERL_REF_SCALAR;
    r->pval = v;
    r->matchpos = 0;
    r->blessed_class = NULL;
    return r;
}

PerlValue *perl_ref_array(PerlArray *a) {
    PerlValue *r = pv_alloc();
    r->tag = PERL_REF_ARRAY;
    r->pval = a;
    r->matchpos = 0;
    r->blessed_class = NULL;
    return r;
}

PerlValue *perl_ref_hash(PerlHash *h) {
    PerlValue *r = pv_alloc();
    r->tag = PERL_REF_HASH;
    r->pval = h;
    r->matchpos = 0;
    r->blessed_class = NULL;
    return r;
}

PerlValue *perl_deref_scalar(PerlValue *ref) {
    if (!ref || ref->tag != PERL_REF_SCALAR) return perl_alloc_undef();
    return (PerlValue *)ref->pval;
}

PerlArray *perl_deref_array(PerlValue *ref) {
    if (!ref) return perl_array_new();
    if (ref->tag == PERL_FLAT_ARRAY) {
        /* Lazy conversion: box flat double[] into a proper PerlArray in-place.
           Mutates the PV from FLAT_ARRAY to REF_ARRAY so subsequent accesses
           (including the Stage 22 inline GEP path) see the correct tag. */
        long long n = ref->matchpos;
        double *dbl = (double *)ref->pval;
        PerlArray *av = perl_anon_array_new();
        for (long long i = 0; i < n; i++) {
            PerlValue *fv = perl_alloc_float(dbl[i]);
            perl_array_push(av, fv);
            perl_free(fv);
        }
        free(dbl);
        ref->tag   = PERL_REF_ARRAY;
        ref->pval  = av;
        ref->matchpos = 0;
        return av;
    }
    if (ref->tag != PERL_REF_ARRAY) return perl_array_new();
    return (PerlArray *)ref->pval;
}

/* Fast read-only deref — caller guarantees ref is a valid REF_ARRAY */
__attribute__((pure)) HOTX PerlArray *perl_deref_array_ro(PerlValue *ref) {
    return (PerlArray *)ref->pval;
}

PerlHash *perl_deref_hash(PerlValue *ref) {
    if (!ref || ref->tag != PERL_REF_HASH) return perl_hash_new();
    return (PerlHash *)ref->pval;
}

PerlValue *perl_ref_type(PerlValue *ref) {
    if (!ref) return perl_alloc_string("");
    if (ref->blessed_class) return perl_alloc_string(ref->blessed_class);
    switch (ref->tag) {
        case PERL_REF_SCALAR: return perl_alloc_string("SCALAR");
        case PERL_REF_ARRAY:  return perl_alloc_string("ARRAY");
        case PERL_REF_HASH:   return perl_alloc_string("HASH");
        case PERL_CODE_REF:   return perl_alloc_string("CODE");
        default:              return perl_alloc_string("");
    }
}

/* ── code references & closures ──────────────────────────────────────────── */

/* active capture context — saved/restored on each code-ref call */
static __thread PerlValue **s_current_captures = NULL;
static __thread int        s_ncaptures         = 0;

static PerlValue *make_code_ref_impl(PerlSubFnCtx fp, PerlValue **caps, int ncaps) {
    PerlClosure *cl = malloc(sizeof *cl);
    cl->fn = fp;
    cl->ncaptures = ncaps;
    cl->captures  = ncaps > 0 ? malloc(ncaps * sizeof(PerlValue*)) : NULL;
    for (int i = 0; i < ncaps; i++) cl->captures[i] = caps[i];
    PerlValue *v = pv_alloc();
    v->tag = PERL_CODE_REF;
    v->pval = cl;
    v->matchpos = 0;
    v->blessed_class = NULL;
    return v;
}

PerlValue *perl_make_code_ref(PerlSubFnCtx fp) {
    return make_code_ref_impl(fp, NULL, 0);
}

PerlValue *perl_make_closure(PerlSubFnCtx fp, PerlArray *captures) {
    int n = captures ? (int)captures->len : 0;
    PerlValue **caps = n > 0 ? captures->elems : NULL;
    return make_code_ref_impl(fp, caps, n);
}

/* ── threads::shared ─────────────────────────────────────────────────────── */

PerlValue *perl_make_shared_scalar(void) {
    PerlSharedVar *sv = calloc(1, sizeof(PerlSharedVar));
    sv->pv.tag   = PERL_UNDEF;
    sv->pv.flags = PV_FLAG_SHARED;
    pthread_mutex_init(&sv->mu,   NULL);
    pthread_cond_init( &sv->cond, NULL);
    return &sv->pv;
}

void perl_lock_shared(PerlValue *pv) {
    if (!pv || !(pv->flags & PV_FLAG_SHARED)) return;
    PerlSharedVar *sv = (PerlSharedVar *)pv;
    pthread_mutex_lock(&sv->mu);
    if (s_local_depth < LOCAL_STACK_MAX) {
        s_local_stack[s_local_depth].type = LOCAL_LOCK_PV;
        s_local_stack[s_local_depth].ptr  = pv;
        s_local_depth++;
    }
}

void perl_cond_wait(PerlValue *pv) {
    if (!pv || !(pv->flags & PV_FLAG_SHARED)) return;
    PerlSharedVar *sv = (PerlSharedVar *)pv;
    pthread_cond_wait(&sv->cond, &sv->mu);
}

void perl_cond_signal(PerlValue *pv) {
    if (!pv || !(pv->flags & PV_FLAG_SHARED)) return;
    pthread_cond_signal(&((PerlSharedVar *)pv)->cond);
}

void perl_cond_broadcast(PerlValue *pv) {
    if (!pv || !(pv->flags & PV_FLAG_SHARED)) return;
    pthread_cond_broadcast(&((PerlSharedVar *)pv)->cond);
}

/* ── threads ─────────────────────────────────────────────────────────────── */

/* Clone a CODE_REF for thread isolation: each captured variable gets a fresh
   stable slot initialized from the parent's value, so perl_assign inside the
   thread only modifies the thread's copy.  Arrays/hashes behind a REF_ARRAY
   or REF_HASH are still shared (refcount bumped) — full deep-copy deferred
   until threads::shared. */
static PerlValue *clone_code_ref_for_thread(PerlValue *code_pv) {
    if (!code_pv || code_pv->tag != PERL_CODE_REF) return perl_clone(code_pv);
    PerlClosure *old_cl = (PerlClosure *)code_pv->pval;
    PerlClosure *new_cl = malloc(sizeof *new_cl);
    new_cl->fn        = old_cl->fn;
    new_cl->ncaptures = old_cl->ncaptures;
    new_cl->captures  = old_cl->ncaptures > 0
        ? malloc(old_cl->ncaptures * sizeof(PerlValue *)) : NULL;
    for (int i = 0; i < old_cl->ncaptures; i++) {
        PerlValue *os = old_cl->captures[i];   /* parent's stable slot */
        /* threads::shared variables: pass the original slot so both threads
           see the same PerlValue (writes are mutex-protected in perl_assign). */
        if (os->flags & PV_FLAG_SHARED) {
            new_cl->captures[i] = os;
            continue;
        }
        PerlValue *ns = malloc(sizeof(PerlValue));  /* thread's isolated stable slot */
        *ns = *os;
        ns->flags = 0;
        ns->matchpos = 0;
        ns->blessed_class = os->blessed_class ? strdup(os->blessed_class) : NULL;
        if (os->tag == PERL_STRING && os->sval) {
            ns->sval = strdup(os->sval);
        } else if (os->tag == PERL_FLAT_ARRAY) {
            long long n = os->matchpos;
            ns->matchpos = n;
            ns->pval = n > 0 ? malloc(sizeof(double) * (size_t)n) : NULL;
            if (n > 0) memcpy(ns->pval, os->pval, sizeof(double) * (size_t)n);
        } else if (os->tag == PERL_REF_ARRAY && os->pval) {
            ((PerlArray *)os->pval)->refcount++;  /* shared array */
        } else if (os->tag == PERL_REF_HASH && os->pval) {
            ((PerlHash *)os->pval)->refcount++;   /* shared hash */
        }
        new_cl->captures[i] = ns;
    }
    PerlValue *pv = pv_alloc();
    pv->tag          = PERL_CODE_REF;
    pv->pval         = new_cl;
    pv->matchpos     = 0;
    pv->blessed_class = code_pv->blessed_class ? strdup(code_pv->blessed_class) : NULL;
    return pv;
}

#define THREAD_REGISTRY_MAX 1024
static PerlValue        *thread_registry[THREAD_REGISTRY_MAX];
static int               thread_registry_count = 0;
static long long         next_tid = 1;           /* main thread is tid 0 */
static pthread_mutex_t   thread_registry_mu = PTHREAD_MUTEX_INITIALIZER;

/* Each thread's own PerlValue* (PERL_THREAD tag) stored in TLS */
static __thread PerlValue *current_thread_pv = NULL;

static PerlValue *make_thread_pv(PerlThread *thr) {
    PerlValue *pv = malloc(sizeof(PerlValue));
    pv->tag            = PERL_THREAD;
    pv->pval           = thr;
    pv->matchpos       = 0;
    pv->blessed_class  = NULL;
    return pv;
}

typedef struct { PerlValue *code_pv; PerlArray *args; PerlThread *thread; PerlValue *thr_pv; } ThreadArgs;

static void *thread_wrapper(void *arg) {
    ThreadArgs *ta = arg;
    current_thread_pv = ta->thr_pv;
    /* call the closure/code-ref with the supplied args */
    PerlValue *result = perl_call_code_ref(ta->code_pv, ta->args);
    ta->thread->result = result ? perl_clone(result) : perl_alloc_undef();
    perl_array_free(ta->args);
    free(ta);
    /* remove from live registry */
    pthread_mutex_lock(&thread_registry_mu);
    for (int i = 0; i < thread_registry_count; i++) {
        PerlThread *t = (PerlThread *)thread_registry[i]->pval;
        if (t == ta->thread) {
            thread_registry[i] = thread_registry[--thread_registry_count];
            break;
        }
    }
    pthread_mutex_unlock(&thread_registry_mu);
    return NULL;
}

PerlValue *perl_threads_create(PerlValue *code_pv, PerlArray *args) {
    if (!code_pv || code_pv->tag != PERL_CODE_REF) return perl_alloc_undef();

    PerlThread *thr = calloc(1, sizeof(PerlThread));
    pthread_mutex_lock(&thread_registry_mu);
    thr->tid = next_tid++;
    pthread_mutex_unlock(&thread_registry_mu);

    PerlValue *thr_pv = make_thread_pv(thr);

    /* register before spawn to avoid race in perl_threads_list */
    pthread_mutex_lock(&thread_registry_mu);
    if (thread_registry_count < THREAD_REGISTRY_MAX)
        thread_registry[thread_registry_count++] = thr_pv;
    pthread_mutex_unlock(&thread_registry_mu);

    ThreadArgs *ta = malloc(sizeof(ThreadArgs));
    ta->code_pv  = clone_code_ref_for_thread(code_pv);
    ta->args     = args ? args : perl_array_new();
    ta->thread   = thr;
    ta->thr_pv   = thr_pv;

    pthread_create(&thr->pth, NULL, thread_wrapper, ta);
    return thr_pv;
}

PerlValue *perl_threads_join(PerlValue *thr_pv) {
    if (!thr_pv || thr_pv->tag != PERL_THREAD) return perl_alloc_undef();
    PerlThread *thr = (PerlThread *)thr_pv->pval;
    if (!thr || thr->joined || thr->detached) return perl_alloc_undef();
    pthread_join(thr->pth, NULL);
    thr->joined = 1;
    return thr->result ? thr->result : perl_alloc_undef();
}

void perl_threads_detach(PerlValue *thr_pv) {
    if (!thr_pv || thr_pv->tag != PERL_THREAD) return;
    PerlThread *thr = (PerlThread *)thr_pv->pval;
    if (!thr || thr->joined || thr->detached) return;
    pthread_detach(thr->pth);
    thr->detached = 1;
}

PerlValue *perl_threads_tid(PerlValue *thr_pv) {
    if (!thr_pv || thr_pv->tag != PERL_THREAD) return perl_alloc_int(0);
    PerlThread *thr = (PerlThread *)thr_pv->pval;
    return perl_alloc_int(thr ? thr->tid : 0);
}

PerlValue *perl_threads_self(void) {
    if (current_thread_pv) return current_thread_pv;
    /* main thread: synthesize a tid=0 object on first call */
    static PerlThread main_thread = {0};
    static PerlValue *main_thread_pv = NULL;
    if (!main_thread_pv) main_thread_pv = make_thread_pv(&main_thread);
    return main_thread_pv;
}

PerlArray *perl_threads_list(void) {
    PerlArray *out = perl_array_new();
    pthread_mutex_lock(&thread_registry_mu);
    for (int i = 0; i < thread_registry_count; i++)
        perl_array_push(out, perl_clone(thread_registry[i]));
    pthread_mutex_unlock(&thread_registry_mu);
    return out;
}

void perl_threads_yield(void) {
    sched_yield();
}

PerlValue *perl_call_code_ref(PerlValue *ref, PerlArray *args) {
    if (!ref || ref->tag != PERL_CODE_REF || !ref->pval)
        return perl_alloc_undef();
    PerlClosure *cl = (PerlClosure*)ref->pval;
    PerlValue **saved_caps = s_current_captures;
    int         saved_n    = s_ncaptures;
    s_current_captures = cl->captures;
    s_ncaptures        = cl->ncaptures;
    PerlValue *result = ((PerlSubFnCtx)cl->fn)(args, perl_push_wantarray(0));
    perl_pop_wantarray();
    s_current_captures = saved_caps;
    s_ncaptures        = saved_n;
    return result;
}

PerlValue *perl_get_capture(long long idx) {
    if (!s_current_captures || idx < 0 || idx >= s_ncaptures)
        return NULL;
    return s_current_captures[idx];
}

/* ── OOP / bless / method dispatch ──────────────────────────────────────── */

PerlValue *perl_bless(PerlValue *ref, PerlValue *class_pv) {
    if (!ref) return perl_alloc_undef();
    char *cls = perl_to_string(class_pv);
    if (ref->blessed_class) free(ref->blessed_class);
    ref->blessed_class = cls;
    return ref;
}

/* ── inheritance (@ISA) ──────────────────────────────────────────────────── */

typedef struct { char *child; char *parent; } IsaEntry;
#define ISA_TABLE_MAX 64
static IsaEntry s_isa_table[ISA_TABLE_MAX];
static int      s_isa_count = 0;

void perl_set_isa(const char *child, const char *parent) {
    /* update if already registered */
    for (int i = 0; i < s_isa_count; i++) {
        if (strcmp(s_isa_table[i].child, child) == 0) {
            free(s_isa_table[i].parent);
            s_isa_table[i].parent = strdup(parent);
            return;
        }
    }
    if (s_isa_count < ISA_TABLE_MAX) {
        s_isa_table[s_isa_count].child  = strdup(child);
        s_isa_table[s_isa_count].parent = strdup(parent);
        s_isa_count++;
    }
}

static const char *perl_get_parent(const char *class_name) {
    for (int i = 0; i < s_isa_count; i++)
        if (strcmp(s_isa_table[i].child, class_name) == 0)
            return s_isa_table[i].parent;
    return NULL;
}

/* ── method dispatch table ───────────────────────────────────────────────── */

typedef struct { char *key; PerlSubFnCtx fn; } MethodEntry;
#define METHOD_TABLE_MAX 1024
static MethodEntry s_method_table[METHOD_TABLE_MAX];
static int s_method_count = 0;

void perl_register_method(const char *key, PerlSubFnCtx fn) {
    if (s_method_count < METHOD_TABLE_MAX) {
        s_method_table[s_method_count].key = strdup(key);
        s_method_table[s_method_count].fn  = fn;
        s_method_count++;
    }
}

/* walk class and its @ISA chain; returns NULL if not found */
static PerlSubFnCtx perl_find_method(const char *class_name, const char *method) {
    const char *cls = class_name;
    while (cls) {
        char key[512];
        snprintf(key, sizeof key, "%s::%s", cls, method);
        for (int i = 0; i < s_method_count; i++)
            if (strcmp(s_method_table[i].key, key) == 0)
                return s_method_table[i].fn;
        cls = perl_get_parent(cls);
    }
    return NULL;
}

static PerlArray *build_dispatch_args(PerlValue *obj, PerlArray *args) {
    PerlArray *full = perl_array_new();
    perl_array_push(full, obj);
    if (args)
        for (long long i = 0; i < args->len; i++)
            perl_array_push(full, args->elems[i]);
    return full;
}

PerlValue *perl_dispatch_method(PerlValue *obj, const char *method, PerlArray *args) {
    /* thread instance methods */
    if (obj && obj->tag == PERL_THREAD) {
        if (strcmp(method, "join")    == 0) return perl_threads_join(obj);
        if (strcmp(method, "detach")  == 0) { perl_threads_detach(obj); return perl_alloc_undef(); }
        if (strcmp(method, "tid")     == 0) return perl_threads_tid(obj);
        if (strcmp(method, "is_running") == 0) {
            PerlThread *t = (PerlThread *)obj->pval;
            return perl_alloc_int(t && !t->joined && !t->detached ? 1 : 0);
        }
        fprintf(stderr, "Unknown thread method: %s\n", method);
        return perl_alloc_undef();
    }
    /* threads class methods */
    if (obj && obj->tag == PERL_STRING && obj->sval && strcmp(obj->sval, "threads") == 0) {
        if (strcmp(method, "self")  == 0) return perl_threads_self();
        if (strcmp(method, "yield") == 0) { perl_threads_yield(); return perl_alloc_undef(); }
        if (strcmp(method, "list")  == 0) return perl_alloc_undef(); /* array context only */
        if (strcmp(method, "create") == 0) {
            /* args[0] = code_ref, args[1..] = thread args */
            if (!args || args->len < 1) return perl_alloc_undef();
            PerlValue *code_pv = args->elems[0];
            PerlArray *thread_args = perl_array_new();
            for (long long i = 1; i < args->len; i++)
                perl_array_push(thread_args, perl_clone(args->elems[i]));
            return perl_threads_create(code_pv, thread_args);
        }
    }

    const char *class_name = NULL;
    if (obj && obj->tag == PERL_STRING && obj->sval)
        class_name = obj->sval;
    else if (obj && obj->blessed_class)
        class_name = obj->blessed_class;

    if (!class_name) {
        fprintf(stderr, "Can't call method \"%s\" on unblessed reference\n", method);
        exit(1);
    }
    PerlSubFnCtx fn = perl_find_method(class_name, method);
    if (!fn) {
        fprintf(stderr, "Can't locate object method \"%s\" via package \"%s\"\n",
                method, class_name);
        exit(1);
    }
    PerlValue *result = fn(build_dispatch_args(obj, args), perl_push_wantarray(0));
    perl_pop_wantarray();
    return result;
}

PerlValue *perl_dispatch_method_super(PerlValue *obj, const char *caller_pkg,
                                      const char *method, PerlArray *args) {
    const char *parent = perl_get_parent(caller_pkg);
    if (!parent) {
        fprintf(stderr, "Can't call SUPER::%s — no parent for package \"%s\"\n",
                method, caller_pkg);
        exit(1);
    }
    PerlSubFnCtx fn = perl_find_method(parent, method);
    if (!fn) {
        fprintf(stderr, "Can't locate SUPER method \"%s\" starting from \"%s\"\n",
                method, parent);
        exit(1);
    }
    PerlValue *result = fn(build_dispatch_args(obj, args), perl_push_wantarray(0));
    perl_pop_wantarray();
    return result;
}

/* ── file I/O ────────────────────────────────────────────────────────────── */

static PerlValue s_stdin_pv  = { .tag = PERL_UNDEF };
static PerlValue s_stdout_pv = { .tag = PERL_UNDEF };
static PerlValue s_stderr_pv = { .tag = PERL_UNDEF };

PerlValue *perl_get_stdin(void) {
    if (s_stdin_pv.tag != PERL_FILEHANDLE) { s_stdin_pv.tag = PERL_FILEHANDLE; s_stdin_pv.pval = stdin; }
    return &s_stdin_pv;
}
PerlValue *perl_get_stdout(void) {
    if (s_stdout_pv.tag != PERL_FILEHANDLE) { s_stdout_pv.tag = PERL_FILEHANDLE; s_stdout_pv.pval = stdout; }
    return &s_stdout_pv;
}
PerlValue *perl_get_stderr(void) {
    if (s_stderr_pv.tag != PERL_FILEHANDLE) { s_stderr_pv.tag = PERL_FILEHANDLE; s_stderr_pv.pval = stderr; }
    return &s_stderr_pv;
}

static const char *mode_to_cmode(const char *m) {
    if (strcmp(m, "<")  == 0) return "r";
    if (strcmp(m, ">")  == 0) return "w";
    if (strcmp(m, ">>") == 0) return "a";
    if (strcmp(m, "+<") == 0) return "r+";
    if (strcmp(m, "+>") == 0) return "w+";
    return "r";
}

PerlValue *perl_open_fh(PerlValue *target, PerlValue *mode_pv, PerlValue *filename_pv) {
    char *ms = perl_to_string(mode_pv);
    char *fs = perl_to_string(filename_pv);
    if (target->tag == PERL_FILEHANDLE && target->pval) fclose((FILE*)target->pval);
    FILE *fp = fopen(fs, mode_to_cmode(ms));
    free(ms); free(fs);
    if (fp) { target->tag = PERL_FILEHANDLE; target->pval = fp; }
    else    { target->tag = PERL_UNDEF;      target->pval = NULL; }
    target->matchpos = 0;
    return target;
}

PerlValue *perl_open2_fh(PerlValue *target, PerlValue *mode_file_pv) {
    char *s = perl_to_string(mode_file_pv);
    const char *filename = s;
    char mode[4] = "<";
    if      (s[0]=='>' && s[1]=='>') { strcpy(mode,">>"); filename=s+2; }
    else if (s[0]=='>')              { strcpy(mode,">");  filename=s+1; }
    else if (s[0]=='<')              { strcpy(mode,"<");  filename=s+1; }
    else if (s[0]=='+' && s[1]=='<') { strcpy(mode,"+<"); filename=s+2; }
    else if (s[0]=='+' && s[1]=='>')  { strcpy(mode,"+>"); filename=s+2; }
    while (*filename==' '||*filename=='\t') filename++;
    if (target->tag == PERL_FILEHANDLE && target->pval) fclose((FILE*)target->pval);
    FILE *fp = fopen(filename, mode_to_cmode(mode));
    free(s);
    if (fp) { target->tag = PERL_FILEHANDLE; target->pval = fp; }
    else    { target->tag = PERL_UNDEF;      target->pval = NULL; }
    target->matchpos = 0;
    return target;
}

void perl_close_fh(PerlValue *fh) {
    if (fh && fh->tag == PERL_FILEHANDLE && fh->pval) {
        fclose((FILE*)fh->pval);
        fh->pval = NULL;
        fh->tag  = PERL_UNDEF;
    }
}

PerlValue *perl_readline(PerlValue *fh) {
    if (!fh || fh->tag != PERL_FILEHANDLE || !fh->pval)
        return perl_alloc_undef();
    FILE *fp = (FILE*)fh->pval;
    ensure_input_sep();
    /* slurp mode: $/ is undef */
    if (s_input_sep.tag == PERL_UNDEF) {
        size_t cap = 4096, len = 0;
        char *buf = malloc(cap);
        int c;
        while ((c = fgetc(fp)) != EOF) {
            if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
            buf[len++] = (char)c;
        }
        if (len == 0) { free(buf); return perl_alloc_undef(); }
        buf[len] = '\0';
        s_dollar_dot.ival++;
        PerlValue *pv = perl_alloc_string(buf);
        free(buf);
        return pv;
    }
    /* normal mode: read until $/ (default "\n") */
    const char *sep    = (s_input_sep.tag == PERL_STRING && s_input_sep.sval)
                         ? s_input_sep.sval : "\n";
    size_t       seplen = strlen(sep);
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = (char)c;
        if (seplen == 1 && (char)c == sep[0]) break;
        if (seplen > 1 && len >= seplen &&
            memcmp(buf + len - seplen, sep, seplen) == 0) break;
    }
    if (len == 0) { free(buf); return perl_alloc_undef(); }
    buf[len] = '\0';
    s_dollar_dot.ival++;
    PerlValue *pv = perl_alloc_string(buf);
    free(buf);
    return pv;
}

PerlArray *perl_readline_all(PerlValue *fh) {
    PerlArray *a = perl_array_new();
    for (;;) {
        PerlValue *line = perl_readline(fh);
        if (line->tag == PERL_UNDEF) { perl_free(line); break; }
        perl_array_push(a, line);
    }
    return a;
}

PerlValue *perl_readline_stdin(void) {
    PerlValue tmp = { .tag = PERL_FILEHANDLE, .pval = stdin };
    return perl_readline(&tmp);
}

PerlArray *perl_readline_all_stdin(void) {
    PerlValue tmp = { .tag = PERL_FILEHANDLE, .pval = stdin };
    return perl_readline_all(&tmp);
}

void perl_print_fh(PerlValue *fh, PerlValue *v) {
    if (!fh || fh->tag != PERL_FILEHANDLE || !fh->pval) return;
    char *s = perl_to_string(v);
    fputs(s, (FILE*)fh->pval);
    free(s);
}

void perl_say_fh(PerlValue *fh, PerlValue *v) {
    if (!fh || fh->tag != PERL_FILEHANDLE || !fh->pval) return;
    char *s = perl_to_string(v);
    FILE *fp = (FILE*)fh->pval;
    fputs(s, fp);
    fputc('\n', fp);
    free(s);
}

void perl_printf_fh(PerlValue *fh, PerlValue *fmt, PerlArray *args) {
    PerlValue *s = perl_sprintf(fmt, args);
    perl_print_fh(fh, s);
    perl_free(s);
}

PerlValue *perl_eof_fh(PerlValue *fh) {
    if (!fh || fh->tag != PERL_FILEHANDLE || !fh->pval) return perl_alloc_int(1);
    return perl_alloc_int(feof((FILE*)fh->pval) ? 1 : 0);
}

void perl_die(PerlValue *msg) {
    if (s_eval_depth > 0) {
        /* preserve the original value in $@ (refs stay as refs) */
        if (msg) {
            perl_assign(&s_dollar_at, msg);
        } else {
            if (s_dollar_at.tag == PERL_STRING && s_dollar_at.sval) free(s_dollar_at.sval);
            s_dollar_at.tag  = PERL_STRING;
            s_dollar_at.sval = strdup("Died");
        }
        longjmp(*s_eval_stack[s_eval_depth - 1], 1);
    }
    char *s = msg ? perl_to_string(msg) : strdup("Died");
    fputs(s, stderr);
    size_t n = strlen(s);
    if (n == 0 || s[n-1] != '\n') fputc('\n', stderr);
    free(s);
    exit(1);
}

PerlValue *perl_unlink_files(PerlArray *files) {
    long long removed = 0;
    for (long long i = 0; i < files->len; i++) {
        char *name = perl_to_string(files->elems[i]);
        if (unlink(name) == 0) removed++;
        free(name);
    }
    return perl_alloc_int(removed);
}

/* ── filesystem ops ──────────────────────────────────────────────────────── */

PerlValue *perl_chdir(PerlValue *path) {
    char *p = perl_to_string(path);
    int r = chdir(p); free(p);
    return perl_alloc_int(r == 0 ? 1 : 0);
}

PerlValue *perl_mkdir_op(PerlValue *path, PerlValue *mode) {
    char *p = perl_to_string(path);
    mode_t m = (mode && mode->tag != PERL_UNDEF) ? (mode_t)perl_to_int(mode) : 0777;
    int r = mkdir(p, m); free(p);
    return perl_alloc_int(r == 0 ? 1 : 0);
}

PerlValue *perl_rmdir_op(PerlValue *path) {
    char *p = perl_to_string(path);
    int r = rmdir(p); free(p);
    return perl_alloc_int(r == 0 ? 1 : 0);
}

PerlValue *perl_rename_op(PerlValue *oldp, PerlValue *newp) {
    char *o = perl_to_string(oldp);
    char *n = perl_to_string(newp);
    int r = rename(o, n); free(o); free(n);
    return perl_alloc_int(r == 0 ? 1 : 0);
}

PerlValue *perl_chmod_op(PerlValue *mode, PerlArray *files) {
    mode_t m = (mode_t)perl_to_int(mode);
    long long changed = 0;
    for (long long i = 0; i < files->len; i++) {
        char *p = perl_to_string(files->elems[i]);
        if (chmod(p, m) == 0) changed++;
        free(p);
    }
    return perl_alloc_int(changed);
}

/* ── directory I/O ───────────────────────────────────────────────────────── */

PerlValue *perl_opendir_fh(PerlValue *target, PerlValue *path) {
    char *p = perl_to_string(path);
    DIR *d = opendir(p); free(p);
    if (!d) return perl_alloc_int(0);
    if (target->tag == PERL_DIRHANDLE && target->pval) closedir((DIR*)target->pval);
    if (target->tag == PERL_STRING && target->sval) free(target->sval);
    if (target->blessed_class) { free(target->blessed_class); target->blessed_class = NULL; }
    target->tag  = PERL_DIRHANDLE;
    target->pval = d;
    return perl_alloc_int(1);
}

PerlValue *perl_readdir(PerlValue *dh) {
    if (!dh || dh->tag != PERL_DIRHANDLE || !dh->pval) return perl_alloc_undef();
    struct dirent *e = readdir((DIR*)dh->pval);
    if (!e) return perl_alloc_undef();
    return perl_alloc_string(e->d_name);
}

PerlArray *perl_readdir_all(PerlValue *dh) {
    PerlArray *a = perl_array_new();
    if (!dh || dh->tag != PERL_DIRHANDLE || !dh->pval) return a;
    struct dirent *e;
    while ((e = readdir((DIR*)dh->pval)) != NULL)
        perl_array_push(a, perl_alloc_string(e->d_name));
    return a;
}

void perl_closedir_fh(PerlValue *dh) {
    if (dh && dh->tag == PERL_DIRHANDLE && dh->pval) {
        closedir((DIR*)dh->pval);
        dh->pval = NULL;
        dh->tag  = PERL_UNDEF;
    }
}

/* ── sprintf / printf ────────────────────────────────────────────────────── */

PerlValue *perl_sprintf(PerlValue *fmt_pv, PerlArray *args) {
    char *fmt = perl_to_string(fmt_pv);

    /* dynamic output buffer */
    size_t cap = 256, pos = 0;
    char *out = malloc(cap);

#define OUT_ENSURE(n) do { while (pos+(n)+1 > cap) { cap*=2; out=realloc(out,cap); } } while(0)
#define OUT_PUTS(s,l) do { size_t _l=(l); OUT_ENSURE(_l); memcpy(out+pos,(s),_l); pos+=_l; } while(0)

    long long argidx = 0;

    for (const char *p = fmt; *p; ) {
        if (*p != '%') { OUT_PUTS(p, 1); p++; continue; }
        p++;
        if (*p == '%') { OUT_PUTS("%", 1); p++; continue; }

        /* collect specifier: [flags][width][.prec]type */
        char spec[128]; int si = 1; spec[0] = '%';
        while (*p && strchr("-+ 0#", *p)) spec[si++] = *p++;
        /* width — digits or * (from next arg) */
        if (*p == '*') {
            PerlValue *wa = (argidx < args->len) ? args->elems[argidx++] : perl_alloc_undef();
            long long w = perl_to_int(wa);
            si += snprintf(spec+si, sizeof(spec)-si-2, "%lld", w);
            p++;
        } else { while (*p && isdigit(*p)) spec[si++] = *p++; }
        /* precision */
        if (*p == '.') {
            spec[si++] = *p++;
            if (*p == '*') {
                PerlValue *pa = (argidx < args->len) ? args->elems[argidx++] : perl_alloc_undef();
                long long pr = perl_to_int(pa);
                si += snprintf(spec+si, sizeof(spec)-si-2, "%lld", pr);
                p++;
            } else { while (*p && isdigit(*p)) spec[si++] = *p++; }
        }
        /* skip length modifiers (l, ll, h, hh, L, z, t, j) — we normalise internally */
        while (*p && strchr("lhLztj", *p)) p++;
        char conv = *p ? *p++ : 's';

        PerlValue *arg = (argidx < args->len) ? args->elems[argidx++] : perl_alloc_undef();

        char tmp[512];
        switch (conv) {
        case 's': {
            char *s = perl_to_string(arg);
            size_t needed = strlen(s) + (size_t)(si + 16);
            char *tbuf = needed <= sizeof(tmp) ? tmp : malloc(needed);
            spec[si++] = 's'; spec[si] = '\0';
            snprintf(tbuf, needed, spec, s);
            OUT_PUTS(tbuf, strlen(tbuf));
            if (tbuf != tmp) free(tbuf);
            free(s);
            continue;
        }
        case 'd': case 'i': {
            char ls[138]; memcpy(ls, spec, si);
            ls[si]=ls[si+1]='l'; ls[si+2]=conv; ls[si+3]='\0';
            snprintf(tmp, sizeof(tmp), ls, perl_to_int(arg));
            break;
        }
        case 'u': {
            char ls[138]; memcpy(ls, spec, si);
            ls[si]=ls[si+1]='l'; ls[si+2]='u'; ls[si+3]='\0';
            snprintf(tmp, sizeof(tmp), ls, (unsigned long long)perl_to_int(arg));
            break;
        }
        case 'f': case 'e': case 'E': case 'g': case 'G': {
            spec[si++] = conv; spec[si] = '\0';
            snprintf(tmp, sizeof(tmp), spec, perl_to_float(arg));
            break;
        }
        case 'x': case 'X': case 'o': {
            char ls[138]; memcpy(ls, spec, si);
            ls[si]='l'; ls[si+1]='l'; ls[si+2]=conv; ls[si+3]='\0';
            unsigned long long uv = (unsigned long long)perl_to_int(arg);
            snprintf(tmp, sizeof(tmp), ls, uv);
            break;
        }
        case 'b': case 'B': {
            unsigned long long uv = (unsigned long long)perl_to_int(arg);
            char binbuf[65]; int bi = 0;
            if (uv == 0) { binbuf[bi++] = '0'; }
            else { for (int bit = 63; bit >= 0; bit--) if ((uv>>bit)&1 || bi) binbuf[bi++] = ((uv>>bit)&1)?'1':'0'; }
            binbuf[bi] = '\0';
            snprintf(tmp, sizeof(tmp), "%s", binbuf);
            break;
        }
        case 'c': {
            tmp[0] = (char)perl_to_int(arg); tmp[1] = '\0';
            break;
        }
        default:
            spec[si++] = conv; spec[si] = '\0';
            snprintf(tmp, sizeof(tmp), spec, perl_to_int(arg));
            break;
        }
        OUT_PUTS(tmp, strlen(tmp));
    }
    out[pos] = '\0';
#undef OUT_ENSURE
#undef OUT_PUTS

    free(fmt);
    PerlValue *result = perl_alloc_string(out);
    free(out);
    return result;
}

void perl_printf(PerlValue *fmt, PerlArray *args) {
    PerlValue *s = perl_sprintf(fmt, args);
    perl_print(s);
    perl_free(s);
}

/* ── range ───────────────────────────────────────────────────────────────── */

/* ── math builtins ───────────────────────────────────────────────────────── */
PerlValue *perl_abs_val(PerlValue *v) {
    if (!v) return perl_alloc_int(0);
    if (v->tag == PERL_FLOAT) return perl_alloc_float(v->fval < 0 ? -v->fval : v->fval);
    long long i = perl_to_int(v);
    return perl_alloc_int(i < 0 ? -i : i);
}

PerlValue *perl_int_trunc(PerlValue *v) {
    if (!v) return perl_alloc_int(0);
    if (v->tag == PERL_INT) return perl_alloc_int(v->ival);
    double d = perl_to_float(v);
    return perl_alloc_int((long long)d);   /* truncates toward zero */
}

PerlValue *perl_sqrt_val(PerlValue *v) {
    double d = v ? perl_to_float(v) : 0.0;
    return perl_alloc_float(sqrt(d));
}

/* ── string case ─────────────────────────────────────────────────────────── */
PerlValue *perl_uc_str(PerlValue *v) {
    char *s = perl_to_string(v);
    for (char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p);
    PerlValue *r = perl_alloc_string(s); free(s); return r;
}

PerlValue *perl_lc_str(PerlValue *v) {
    char *s = perl_to_string(v);
    for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p);
    PerlValue *r = perl_alloc_string(s); free(s); return r;
}

PerlValue *perl_ucfirst_str(PerlValue *v) {
    char *s = perl_to_string(v);
    if (s[0]) s[0] = (char)toupper((unsigned char)s[0]);
    PerlValue *r = perl_alloc_string(s); free(s); return r;
}

PerlValue *perl_lcfirst_str(PerlValue *v) {
    char *s = perl_to_string(v);
    if (s[0]) s[0] = (char)tolower((unsigned char)s[0]);
    PerlValue *r = perl_alloc_string(s); free(s); return r;
}

/* ── index / rindex ──────────────────────────────────────────────────────── */
PerlValue *perl_index_str(PerlValue *str_pv, PerlValue *sub_pv, PerlValue *pos_pv) {
    char *s = perl_to_string(str_pv);
    char *sub = perl_to_string(sub_pv);
    long long pos = (pos_pv && pos_pv->tag != PERL_UNDEF) ? perl_to_int(pos_pv) : 0;
    long long slen = (long long)strlen(s);
    if (pos < 0) pos = 0;
    long long result = -1;
    if (pos <= slen) {
        char *found = strstr(s + pos, sub);
        if (found) result = (long long)(found - s);
    }
    free(s); free(sub);
    return perl_alloc_int(result);
}

PerlValue *perl_rindex_str(PerlValue *str_pv, PerlValue *sub_pv, PerlValue *pos_pv) {
    char *s = perl_to_string(str_pv);
    char *sub = perl_to_string(sub_pv);
    long long slen = (long long)strlen(s);
    long long sublen = (long long)strlen(sub);
    long long pos = (pos_pv && pos_pv->tag != PERL_UNDEF) ? perl_to_int(pos_pv) : slen;
    if (pos > slen - sublen) pos = slen - sublen;
    long long result = -1;
    for (long long i = pos; i >= 0; i--) {
        if (strncmp(s + i, sub, (size_t)sublen) == 0) { result = i; break; }
    }
    free(s); free(sub);
    return perl_alloc_int(result);
}

/* ── chr / ord / hex / oct ───────────────────────────────────────────────── */
PerlValue *perl_chr_val(PerlValue *v) {
    long long n = perl_to_int(v);
    char buf[2] = { (char)(n & 0xFF), '\0' };
    return perl_alloc_string(buf);
}

PerlValue *perl_ord_val(PerlValue *v) {
    char *s = perl_to_string(v);
    long long r = s[0] ? (long long)(unsigned char)s[0] : 0;
    free(s);
    return perl_alloc_int(r);
}

PerlValue *perl_hex_val(PerlValue *v) {
    char *s = perl_to_string(v);
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    long long r = (long long)strtoll(p, NULL, 16);
    free(s);
    return perl_alloc_int(r);
}

PerlValue *perl_oct_val(PerlValue *v) {
    char *s = perl_to_string(v);
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    long long r;
    if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) r = (long long)strtoll(p + 2, NULL, 2);
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) r = (long long)strtoll(p + 2, NULL, 16);
    else r = (long long)strtoll(p, NULL, 8);
    free(s);
    return perl_alloc_int(r);
}

/* ── reverse ─────────────────────────────────────────────────────────────── */
PerlArray *perl_reverse_array(PerlArray *a) {
    PerlArray *r = perl_array_new();
    for (long long i = a->len - 1; i >= 0; i--)
        perl_array_push(r, perl_clone(a->elems[i]));
    return r;
}

PerlValue *perl_reverse_str(PerlValue *v) {
    char *s = perl_to_string(v);
    long long len = (long long)strlen(s);
    char *r = malloc((size_t)len + 1);
    for (long long i = 0; i < len; i++) r[i] = s[len - 1 - i];
    r[len] = '\0';
    PerlValue *res = perl_alloc_string(r);
    free(s); free(r);
    return res;
}

/* ── sort with comparator ────────────────────────────────────────────────── */
static int cmp_num_asc(const void *a, const void *b) {
    double da = perl_to_float(*(PerlValue**)a);
    double db = perl_to_float(*(PerlValue**)b);
    return (da > db) - (da < db);
}
static int cmp_num_desc(const void *a, const void *b) { return cmp_num_asc(b, a); }
static int cmp_str_asc(const void *a, const void *b) {
    char *sa = perl_to_string(*(PerlValue**)a);
    char *sb = perl_to_string(*(PerlValue**)b);
    int r = strcmp(sa, sb); free(sa); free(sb); return r;
}
static int cmp_str_desc(const void *a, const void *b) { return cmp_str_asc(b, a); }

static PerlArray *sort_copy_with(PerlArray *a, int(*cmp)(const void*, const void*)) {
    PerlArray *r = perl_array_new();
    for (long long i = 0; i < a->len; i++) perl_array_push(r, perl_clone(a->elems[i]));
    qsort(r->elems, (size_t)r->len, sizeof(PerlValue*), cmp);
    return r;
}

PerlArray *perl_sort_num_asc(PerlArray *a)  { return sort_copy_with(a, cmp_num_asc);  }
PerlArray *perl_sort_num_desc(PerlArray *a) { return sort_copy_with(a, cmp_num_desc); }
PerlArray *perl_sort_str_asc(PerlArray *a)  { return sort_copy_with(a, cmp_str_asc);  }
PerlArray *perl_sort_str_desc(PerlArray *a) { return sort_copy_with(a, cmp_str_desc); }

/* ── spaceship and cmp ───────────────────────────────────────────────────── */
PerlValue *perl_spaceship(PerlValue *a, PerlValue *b) {
    double da = perl_to_float(a), db = perl_to_float(b);
    return perl_alloc_int((da > db) - (da < db));
}

PerlValue *perl_str_spaceship(PerlValue *a, PerlValue *b) {
    char *sa = perl_to_string(a), *sb = perl_to_string(b);
    int r = strcmp(sa, sb);
    free(sa); free(sb);
    return perl_alloc_int(r < 0 ? -1 : r > 0 ? 1 : 0);
}

PerlArray *perl_range(PerlValue *from, PerlValue *to) {
    PerlArray *a = perl_array_new();
    long long lo = perl_to_int(from);
    long long hi = perl_to_int(to);
    for (long long i = lo; i <= hi; i++) {
        PerlValue *pv = perl_alloc_int(i);
        perl_array_push(a, pv);
        perl_free(pv);
    }
    return a;
}

/* ── regex (PCRE2) ───────────────────────────────────────────────────────── */

#define PERL_MAX_CAPTURES 10
static PerlValue *perl_captures_[PERL_MAX_CAPTURES + 1];  /* $1..$10 */
static PerlHash *perl_plus_hash = NULL;

static int pcre_flags(const char *flags) {
    int opts = 0;
    for (; *flags; flags++) {
        switch (*flags) {
            case 'i': opts |= PCRE2_CASELESS;  break;
            case 's': opts |= PCRE2_DOTALL;    break;
            case 'm': opts |= PCRE2_MULTILINE; break;
            case 'x': opts |= PCRE2_EXTENDED;  break;
        }
    }
    return opts;
}

PerlValue *perl_capture(long long n) {
    if (n < 1 || n > PERL_MAX_CAPTURES) return perl_alloc_undef();
    return perl_captures_[n] ? perl_clone(perl_captures_[n]) : perl_alloc_undef();
}

static void populate_named_captures(pcre2_match_data *md, const char *s, pcre2_code *re) {
  if (perl_plus_hash) perl_hash_free(perl_plus_hash);
  perl_plus_hash = perl_hash_new();

  uint32_t namecount;
  pcre2_pattern_info(re, PCRE2_INFO_NAMECOUNT, &namecount);
  if (namecount == 0) return;

  PCRE2_SIZE name_table_entry_size;
  uint32_t name_entry_size;
  uint32_t name_count;
  pcre2_pattern_info(re, PCRE2_INFO_NAMETABLE, &name_table_entry_size);
  pcre2_pattern_info(re, PCRE2_INFO_NAMEENTRYSIZE, &name_entry_size);
  pcre2_pattern_info(re, PCRE2_INFO_NAMECOUNT, &name_count);

  PCRE2_SPTR name_table;
  pcre2_pattern_info(re, PCRE2_INFO_NAMETABLE, &name_table);
  PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);

  for (uint32_t i = 0; i < name_count; i++) {
    PCRE2_SPTR name = name_table + name_entry_size * i;
    uint32_t group_num = *(uint32_t *)(name_table + name_entry_size * i + name_table_entry_size);
    size_t cstart = ov[2 * group_num], cend = ov[2 * group_num + 1];
    if (cstart < cend) {
      char *cap = malloc(cend - cstart + 1);
      memcpy(cap, s + cstart, cend - cstart);
      cap[cend - cstart] = '\0';
      PerlValue *key = perl_alloc_string((char*)name);
      PerlValue *val = perl_alloc_string(cap);
      perl_hash_set_sv(perl_plus_hash, key, val);
      perl_free(key);
      perl_free(val);
      free(cap);
    }
  }
}

PerlValue *perl_regex_match(PerlValue *str, const char *pattern, const char *flags) {
    int errcode; PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                   pcre_flags(flags), &errcode, &erroffset, NULL);
    if (!re) return perl_alloc_int(0);

    char *s = perl_to_string(str);
    size_t slen = strlen(s);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    int rc = pcre2_match(re, (PCRE2_SPTR)s, slen, 0, 0, md, NULL);
    if (rc > 0) populate_named_captures(md, s, re);

    if (rc > 0) {
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
        populate_named_captures(md, s, re);
        /* store $& — full match (group 0) */
        if (s_dollar_amp.tag == PERL_STRING && s_dollar_amp.sval) free(s_dollar_amp.sval);
        { size_t ms = ov[0], me = ov[1];
          char *ms_str = malloc(me - ms + 1);
          memcpy(ms_str, s + ms, me - ms); ms_str[me-ms] = '\0';
          s_dollar_amp.tag = PERL_STRING; s_dollar_amp.sval = ms_str; }
        for (int i = 1; i <= PERL_MAX_CAPTURES; i++) {
            if (perl_captures_[i]) { perl_free(perl_captures_[i]); perl_captures_[i] = NULL; }
        }
        for (int i = 1; i < rc && i <= PERL_MAX_CAPTURES; i++) {
            size_t cstart = ov[2*i], cend = ov[2*i+1];
            char *cap = malloc(cend - cstart + 1);
            memcpy(cap, s + cstart, cend - cstart);
            cap[cend - cstart] = '\0';
            perl_captures_[i] = perl_alloc_string(cap);
            free(cap);
        }
    }

    free(s);
    pcre2_match_data_free(md);
    pcre2_code_free(re);
    return perl_alloc_int(rc > 0 ? 1 : 0);
}

long long perl_regex_subst(PerlValue *str, const char *pattern, const char *repl, const char *flags) {
    /* separate /g from PCRE options */
    int global = 0;
    char clean[64]; int ci = 0;
    for (const char *fp = flags; *fp; fp++) {
        if (*fp == 'g') global = 1;
        else if (ci < 63) clean[ci++] = *fp;
    }
    clean[ci] = '\0';

    int errcode; PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                   pcre_flags(clean), &errcode, &erroffset, NULL);
    if (!re) return 0;

    char *s = perl_to_string(str);
    size_t slen = strlen(s);

#define ENSURE(need) do { \
    while (out_len + (need) + 1 > out_cap) { out_cap *= 2; out = realloc(out, out_cap); } \
} while(0)

    size_t out_cap = slen * 2 + 64;
    char *out = malloc(out_cap);
    size_t out_len = 0;
    long long count = 0;
    size_t pos = 0;

    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    while (pos <= slen) {
        int rc = pcre2_match(re, (PCRE2_SPTR)s, slen, pos, 0, md, NULL);
        if (rc > 0) populate_named_captures(md, s, re);
        if (rc <= 0) {
            size_t rem = slen - pos;
            ENSURE(rem); memcpy(out + out_len, s + pos, rem); out_len += rem; break;
        }
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
        size_t mstart = ov[0], mend = ov[1];

        /* text before match */
        size_t pre = mstart - pos;
        ENSURE(pre); memcpy(out + out_len, s + pos, pre); out_len += pre;

        /* expand replacement: handle $0 (whole match), $1..$9 */
        for (const char *rp = repl; *rp; ) {
            if (*rp == '$' && isdigit((unsigned char)rp[1])) {
                int n = rp[1] - '0'; rp += 2;
                size_t cstart = (n == 0) ? mstart : (n < rc ? ov[2*n]   : 0);
                size_t cend   = (n == 0) ? mend   : (n < rc ? ov[2*n+1] : 0);
                size_t caplen = (cstart < cend) ? cend - cstart : 0;
                ENSURE(caplen); memcpy(out + out_len, s + cstart, caplen); out_len += caplen;
            } else {
                ENSURE(1); out[out_len++] = *rp++;
            }
        }
        count++;

        if (mend == mstart) {
            /* zero-length match: copy current char to prevent infinite loop */
            if (pos < slen) { ENSURE(1); out[out_len++] = s[pos]; }
            pos = mstart + 1;
        } else {
            pos = mend;
        }

        if (!global) {
            size_t rem = slen - pos;
            ENSURE(rem); memcpy(out + out_len, s + pos, rem); out_len += rem; break;
        }
    }
    out[out_len] = '\0';
#undef ENSURE

    /* update PerlValue in-place */
    if (str->tag == PERL_STRING && str->sval) free(str->sval);
    str->tag  = PERL_STRING;
    str->sval = out;

    free(s);
    pcre2_match_data_free(md);
    pcre2_code_free(re);
    return count;
}

PerlArray *perl_split_regex(const char *pattern, const char *flags, PerlValue *str) {
    int errcode; PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                   pcre_flags(flags), &errcode, &erroffset, NULL);
    PerlArray *arr = perl_array_new();
    if (!re) return arr;

    char *s = perl_to_string(str);
    size_t slen = strlen(s);
    size_t pos = 0;
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);

    while (pos <= slen) {
        int rc = pcre2_match(re, (PCRE2_SPTR)s, slen, pos, 0, md, NULL);
        if (rc > 0) populate_named_captures(md, s, re);
        if (rc <= 0) {
            PerlValue *v = perl_alloc_string(s + pos);
            perl_array_push(arr, v); perl_free(v); break;
        }
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
        size_t mstart = ov[0], mend = ov[1];

        size_t pre = mstart - pos;
        char *elem = malloc(pre + 1);
        memcpy(elem, s + pos, pre); elem[pre] = '\0';
        PerlValue *v = perl_alloc_string(elem); free(elem);
        perl_array_push(arr, v); perl_free(v);

        pos = (mend > mstart) ? mend : mend + 1;
    }

    free(s);
    pcre2_match_data_free(md);
    pcre2_code_free(re);
    return arr;
}

PerlValue *perl_get_plus_hash(void) {
  return perl_plus_hash ? perl_ref_hash(perl_plus_hash) : perl_alloc_undef();
}

void perl_clear_named_captures(void) {
  if (perl_plus_hash) {
    perl_hash_free(perl_plus_hash);
    perl_plus_hash = NULL;
  }
}

PerlValue *perl_regex_match_g(PerlValue *str, const char *pattern, const char *flags) {
    int errcode; PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                   pcre_flags(flags), &errcode, &erroffset, NULL);
    if (!re) { str->matchpos = 0; return perl_alloc_int(0); }

    char *s = perl_to_string(str);
    size_t slen = strlen(s);
    size_t startpos = (str->matchpos > 0 && (size_t)str->matchpos <= slen)
                      ? (size_t)str->matchpos : 0;

    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    int rc = pcre2_match(re, (PCRE2_SPTR)s, slen, startpos, 0, md, NULL);
    if (rc > 0) populate_named_captures(md, s, re);
    if (rc > 0) populate_named_captures(md, s, re);

    if (rc > 0) {
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
        size_t mend = ov[1];
        str->matchpos = (long long)(mend > startpos ? mend : mend + 1);
        for (int i = 1; i <= PERL_MAX_CAPTURES; i++) {
            if (perl_captures_[i]) { perl_free(perl_captures_[i]); perl_captures_[i] = NULL; }
        }
        for (int i = 1; i < rc && i <= PERL_MAX_CAPTURES; i++) {
            size_t cs = ov[2*i], ce = ov[2*i+1];
            char *cap = malloc(ce - cs + 1);
            memcpy(cap, s + cs, ce - cs); cap[ce - cs] = '\0';
            perl_captures_[i] = perl_alloc_string(cap); free(cap);
        }
        free(s); pcre2_match_data_free(md); pcre2_code_free(re);
        return perl_alloc_int(1);
    } else {
        str->matchpos = 0;
        free(s); pcre2_match_data_free(md); pcre2_code_free(re);
        return perl_alloc_int(0);
    }
}

PerlArray *perl_regex_match_all(PerlValue *str, const char *pattern, const char *flags) {
    int errcode; PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                   pcre_flags(flags), &errcode, &erroffset, NULL);
    PerlArray *arr = perl_array_new();
    if (!re) return arr;

    uint32_t capturecount = 0;
    pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &capturecount);

    char *s = perl_to_string(str);
    size_t slen = strlen(s);
    size_t pos = 0;
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);

    while (pos <= slen) {
        int rc = pcre2_match(re, (PCRE2_SPTR)s, slen, pos, 0, md, NULL);
        if (rc > 0) populate_named_captures(md, s, re);
        if (rc <= 0) break;
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
        size_t mstart = ov[0], mend = ov[1];

        if (capturecount == 0) {
            /* no captures: collect whole match */
            size_t mlen = mend - mstart;
            char *m = malloc(mlen + 1);
            memcpy(m, s + mstart, mlen); m[mlen] = '\0';
            PerlValue *v = perl_alloc_string(m); free(m);
            perl_array_push(arr, v); perl_free(v);
        } else {
            /* captures: collect each group */
            for (int i = 1; i < rc; i++) {
                size_t cs = ov[2*i], ce = ov[2*i+1];
                size_t clen = ce - cs;
                char *m = malloc(clen + 1);
                memcpy(m, s + cs, clen); m[clen] = '\0';
                PerlValue *v = perl_alloc_string(m); free(m);
                perl_array_push(arr, v); perl_free(v);
            }
        }
        pos = (mend > mstart) ? mend : mend + 1;
    }

    free(s); pcre2_match_data_free(md); pcre2_code_free(re);
    return arr;
}

/* ── splice ──────────────────────────────────────────────────────────────── */

PerlArray *perl_splice(PerlArray *arr, PerlValue *off_pv, PerlValue *len_pv, PerlArray *repl) {
    if (!arr) return perl_array_new();

    long long off = (off_pv && off_pv->tag != PERL_UNDEF) ? perl_to_int(off_pv) : 0;
    if (off < 0) off = arr->len + off;
    if (off < 0) off = 0;
    if (off > arr->len) off = arr->len;

    long long len;
    if (!len_pv || len_pv->tag == PERL_UNDEF) {
        len = arr->len - off;
    } else {
        len = perl_to_int(len_pv);
        if (len < 0) { len = arr->len - off + len; if (len < 0) len = 0; }
    }
    if (off + len > arr->len) len = arr->len - off;

    /* collect removed elements */
    PerlArray *removed = perl_array_new();
    for (long long i = 0; i < len; i++)
        perl_array_push(removed, arr->elems[off + i]);

    long long repl_len = repl ? repl->len : 0;
    long long new_len  = arr->len - len + repl_len;

    /* grow if needed */
    while (arr->cap < new_len) {
        arr->cap = arr->cap ? arr->cap * 2 : 8;
        arr->elems = realloc(arr->elems, (size_t)arr->cap * sizeof(PerlValue *));
    }
    /* shift tail */
    if (repl_len != len)
        memmove(arr->elems + off + repl_len,
                arr->elems + off + len,
                (size_t)(arr->len - off - len) * sizeof(PerlValue *));
    /* insert replacement clones */
    for (long long i = 0; i < repl_len; i++)
        arr->elems[off + i] = perl_clone(repl->elems[i]);

    arr->len = new_len;
    return removed;
}

/* ── environment ─────────────────────────────────────────────────────────── */

PerlValue *perl_env_get(PerlValue *key) {
    char *k = perl_to_string(key);
    const char *val = getenv(k);
    free(k);
    return val ? perl_alloc_string(val) : perl_alloc_undef();
}

void perl_env_set(PerlValue *key, PerlValue *val) {
    char *k = perl_to_string(key);
    char *v = perl_to_string(val);
    setenv(k, v, 1);
    free(k); free(v);
}

void perl_warn(PerlValue *msg) {
    char *s = perl_to_string(msg);
    size_t len = strlen(s);
    fputs(s, stderr);
    if (len == 0 || s[len - 1] != '\n') fputc('\n', stderr);
    free(s);
}

PerlValue *perl_system(PerlValue *cmd) {
    char *s = perl_to_string(cmd);
    int ret = system(s);
    free(s);
    if (ret == -1) return perl_alloc_int(-1);
    return perl_alloc_int(WIFEXITED(ret) ? WEXITSTATUS(ret) : -1);
}

PerlValue *perl_backtick(PerlValue *cmd) {
    char *s = perl_to_string(cmd);
    FILE *fp = popen(s, "r");
    free(s);
    if (!fp) return perl_alloc_undef();
    char *out = NULL; size_t cap = 0, len = 0;
    char buf[4096];
    while (fgets(buf, (int)sizeof(buf), fp)) {
        size_t n = strlen(buf);
        if (len + n + 1 > cap) {
            cap = cap ? cap * 2 : 4096;
            out = realloc(out, cap);
        }
        memcpy(out + len, buf, n); len += n;
    }
    pclose(fp);
    if (!out) { PerlValue *r = perl_alloc_string(""); return r; }
    out[len] = '\0';
    PerlValue *r = perl_alloc_string(out);
    free(out);
    return r;
}

/* ── tr/// character translation ────────────────────────────────────────── */

/* Expand tr ranges like a-z into individual characters */
static char *tr_expand(const char *s, size_t *outlen) {
    size_t len = strlen(s);
    /* worst case: all ranges a-z = 24 chars each → allocate generously */
    char *buf = malloc(len * 256 + 1);
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        if (i + 2 < len && s[i+1] == '-' && s[i+2] >= s[i]) {
            for (unsigned char c = (unsigned char)s[i]; c <= (unsigned char)s[i+2]; c++)
                buf[out++] = (char)c;
            i += 2;
        } else if (s[i] == '\\' && i + 1 < len) {
            i++;
            switch (s[i]) {
                case 'n': buf[out++] = '\n'; break;
                case 't': buf[out++] = '\t'; break;
                case 'r': buf[out++] = '\r'; break;
                default:  buf[out++] = s[i]; break;
            }
        } else {
            buf[out++] = s[i];
        }
    }
    buf[out] = '\0';
    *outlen = out;
    return buf;
}

long long perl_tr(PerlValue *str, const char *search, const char *replace, const char *flags) {
    if (!str) return 0;
    int do_delete  = strchr(flags, 'd') != NULL;
    int do_squeeze = strchr(flags, 's') != NULL;
    int do_compl   = strchr(flags, 'c') != NULL;

    size_t slen, rlen;
    char *sch = tr_expand(search, &slen);
    char *rch = tr_expand(replace, &rlen);

    /* build 256-entry translation table */
    int table[256];
    for (int i = 0; i < 256; i++) table[i] = -1; /* -1 = no translation */

    if (!do_compl) {
        for (size_t i = 0; i < slen; i++) {
            unsigned char sc = (unsigned char)sch[i];
            if (table[sc] == -1) { /* first occurrence wins */
                if (i < rlen)          table[sc] = (unsigned char)rch[i];
                else if (rlen == 0) {
                    if (do_delete)     table[sc] = -2; /* delete */
                    else               table[sc] = sc;  /* replicate = identity */
                } else                 table[sc] = (unsigned char)rch[rlen - 1];
            }
        }
    } else {
        /* complement: translate chars NOT in search list */
        char in_search[256] = {0};
        for (size_t i = 0; i < slen; i++) in_search[(unsigned char)sch[i]] = 1;
        size_t ri = 0;
        for (int c = 0; c < 256; c++) {
            if (!in_search[c]) {
                if (ri < rlen)         table[c] = (unsigned char)rch[ri++];
                else if (rlen == 0) {
                    if (do_delete)     table[c] = -2;
                    else               table[c] = c;
                } else                 table[c] = (unsigned char)rch[rlen - 1];
            }
        }
    }
    free(sch); free(rch);

    char *s = perl_to_string(str);
    size_t in_len = strlen(s);
    char *out = malloc(in_len + 1);
    size_t out_len = 0;
    long long count = 0;
    char last_out = 0;
    int has_last = 0;

    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)s[i];
        int mapped = table[c];
        if (mapped == -1) {
            /* no translation — pass through */
            if (!do_squeeze || !has_last || (char)c != last_out) {
                out[out_len++] = (char)c;
                last_out = (char)c; has_last = 1;
            }
        } else if (mapped == -2) {
            /* explicit delete marker */
            count++;
        } else {
            count++;
            char mc = (char)mapped;
            if (do_squeeze && has_last && mc == last_out) continue;
            out[out_len++] = mc;
            last_out = mc; has_last = 1;
        }
    }
    out[out_len] = '\0';

    /* update str in-place */
    if (str->tag == PERL_STRING && str->sval) free(str->sval);
    str->tag  = PERL_STRING;
    str->sval = out;
    free(s);
    return count;
}

/* ── command-line arguments ─────────────────────────────────────────────── */

static PerlArray *perl_argv_arr = NULL;
static PerlValue  perl_dollar0_val = { .tag = PERL_STRING };

PerlArray *perl_init_argv(int argc, char **argv) {
    perl_argv_arr = perl_array_new();
    /* $0 = script name (argv[0]) */
    perl_dollar0_val.sval = strdup(argc > 0 ? argv[0] : "");
    /* @ARGV = argv[1..] */
    for (int i = 1; i < argc; i++) {
        PerlValue *v = perl_alloc_string(argv[i]);
        perl_array_push(perl_argv_arr, v);
        perl_free(v);
    }
    return perl_argv_arr;
}

PerlValue *perl_get_dollar0(void) {
    return &perl_dollar0_val;
}

/* ── file test operators ─────────────────────────────────────────────────── */

PerlValue *perl_filetest(int op, PerlValue *path_pv) {
    char *path = perl_to_string(path_pv);
    struct stat st;
    PerlValue *result;
    switch (op) {
        case 'e': result = perl_alloc_int(access(path, F_OK) == 0); break;
        case 'f': result = perl_alloc_int(stat(path, &st) == 0 && S_ISREG(st.st_mode)); break;
        case 'd': result = perl_alloc_int(stat(path, &st) == 0 && S_ISDIR(st.st_mode)); break;
        case 'r': result = perl_alloc_int(access(path, R_OK) == 0); break;
        case 'w': result = perl_alloc_int(access(path, W_OK) == 0); break;
        case 'x': result = perl_alloc_int(access(path, X_OK) == 0); break;
        case 'z': result = perl_alloc_int(stat(path, &st) == 0 && st.st_size == 0); break;
        case 's': result = (stat(path, &st) == 0) ? perl_alloc_int(st.st_size) : perl_alloc_undef(); break;
        case 'l': result = perl_alloc_int(lstat(path, &st) == 0 && S_ISLNK(st.st_mode)); break;
        case 'p': result = perl_alloc_int(stat(path, &st) == 0 && S_ISFIFO(st.st_mode)); break;
        default:  result = perl_alloc_int(0); break;
    }
    free(path);
    return result;
}

/* ── time / randomness / process ────────────────────────────────────────── */
#include <time.h>

PerlValue *perl_rand_val(PerlValue *max) {
    double m = max ? perl_to_float(max) : 1.0;
    return perl_alloc_float(m * ((double)rand() / ((double)RAND_MAX + 1.0)));
}

void perl_srand_val(PerlValue *seed) {
    unsigned s = seed ? (unsigned)perl_to_int(seed) : (unsigned)time(NULL);
    srand(s);
}

PerlValue *perl_time_val(void) {
    return perl_alloc_int((long long)time(NULL));
}

static PerlArray *_broken_time(time_t t) {
    struct tm *tm = localtime(&t);
    PerlArray *a = perl_array_new();
    perl_array_push(a, perl_alloc_int(tm->tm_sec));
    perl_array_push(a, perl_alloc_int(tm->tm_min));
    perl_array_push(a, perl_alloc_int(tm->tm_hour));
    perl_array_push(a, perl_alloc_int(tm->tm_mday));
    perl_array_push(a, perl_alloc_int(tm->tm_mon));
    perl_array_push(a, perl_alloc_int(tm->tm_year));
    perl_array_push(a, perl_alloc_int(tm->tm_wday));
    perl_array_push(a, perl_alloc_int(tm->tm_yday));
    perl_array_push(a, perl_alloc_int(tm->tm_isdst));
    return a;
}

static PerlArray *_broken_time_gm(time_t t) {
    struct tm *tm = gmtime(&t);
    PerlArray *a = perl_array_new();
    perl_array_push(a, perl_alloc_int(tm->tm_sec));
    perl_array_push(a, perl_alloc_int(tm->tm_min));
    perl_array_push(a, perl_alloc_int(tm->tm_hour));
    perl_array_push(a, perl_alloc_int(tm->tm_mday));
    perl_array_push(a, perl_alloc_int(tm->tm_mon));
    perl_array_push(a, perl_alloc_int(tm->tm_year));
    perl_array_push(a, perl_alloc_int(tm->tm_wday));
    perl_array_push(a, perl_alloc_int(tm->tm_yday));
    perl_array_push(a, perl_alloc_int(tm->tm_isdst));
    return a;
}

PerlArray *perl_localtime_val(PerlValue *t) {
    time_t ts = t ? (time_t)perl_to_int(t) : time(NULL);
    return _broken_time(ts);
}

PerlArray *perl_gmtime_val(PerlValue *t) {
    time_t ts = t ? (time_t)perl_to_int(t) : time(NULL);
    return _broken_time_gm(ts);
}

PerlValue *perl_sleep_val(PerlValue *secs) {
    unsigned s = secs ? (unsigned)perl_to_int(secs) : 0;
    return perl_alloc_int((long long)sleep(s));
}

PerlValue *perl_alarm_val(PerlValue *secs) {
    unsigned s = secs ? (unsigned)perl_to_int(secs) : 0;
    return perl_alloc_int((long long)alarm(s));
}

/* ── List::Util ───────────────────────────────────────────────────────────── */

PerlValue *perl_sum_list(PerlArray *a) {
    if (!a || a->len == 0) return perl_alloc_undef();
    double s = 0.0;
    int has_float = 0;
    for (long long i = 0; i < a->len; i++) {
        PerlValue *v = a->elems[i];
        if (v && v->tag == PERL_FLOAT) has_float = 1;
        s += v ? perl_to_float(v) : 0.0;
    }
    if (has_float) return perl_alloc_float(s);
    return perl_alloc_int((long long)s);
}

PerlValue *perl_min_list(PerlArray *a) {
    if (!a || a->len == 0) return perl_alloc_undef();
    double m = perl_to_float(a->elems[0]);
    for (long long i = 1; i < a->len; i++) {
        double v = perl_to_float(a->elems[i]);
        if (v < m) m = v;
    }
    return perl_alloc_float(m);
}

PerlValue *perl_max_list(PerlArray *a) {
    if (!a || a->len == 0) return perl_alloc_undef();
    double m = perl_to_float(a->elems[0]);
    for (long long i = 1; i < a->len; i++) {
        double v = perl_to_float(a->elems[i]);
        if (v > m) m = v;
    }
    return perl_alloc_float(m);
}

PerlArray *perl_uniq_list(PerlArray *a) {
    PerlArray *res = perl_array_new();
    if (!a) return res;
    char *prev = NULL;
    for (long long i = 0; i < a->len; i++) {
        char *s = perl_to_string(a->elems[i]);
        if (!prev || strcmp(prev, s) != 0) {
            perl_array_push(res, perl_clone(a->elems[i]));
            free(prev);
            prev = s;
        } else {
            free(s);
        }
    }
    free(prev);
    return res;
}

/* ── sort with custom comparator ─────────────────────────────────────────── */

static PerlSortCmpFn sort_custom_cmp_ = NULL;

static int sort_qsort_wrap_(const void *pa, const void *pb) {
    PerlValue *a = *(PerlValue **)pa;
    PerlValue *b = *(PerlValue **)pb;
    long long r = sort_custom_cmp_(a, b);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

PerlArray *perl_sort_custom(PerlArray *a, PerlSortCmpFn cmp) {
    PerlArray *res = perl_array_new();
    if (!a) return res;
    /* copy element pointers (shallow) */
    for (long long i = 0; i < a->len; i++)
        perl_array_push(res, perl_clone(a->elems[i]));
    sort_custom_cmp_ = cmp;
    qsort(res->elems, (size_t)res->len, sizeof(PerlValue *), sort_qsort_wrap_);
    sort_custom_cmp_ = NULL;
    return res;
}

/* ── POSIX ────────────────────────────────────────────────────────────────── */
#include <math.h>

PerlValue *perl_posix_floor(PerlValue *v) {
    return perl_alloc_float(floor(perl_to_float(v)));
}
PerlValue *perl_posix_ceil(PerlValue *v) {
    return perl_alloc_float(ceil(perl_to_float(v)));
}
PerlValue *perl_posix_fmod(PerlValue *a, PerlValue *b) {
    return perl_alloc_float(fmod(perl_to_float(a), perl_to_float(b)));
}
PerlValue *perl_posix_strftime(PerlArray *args) {
    if (!args || args->len < 1) return perl_alloc_string("");
    char *fmt = perl_to_string(args->elems[0]);
    struct tm tm = {0};
    if (args->len >= 7) {
        tm.tm_sec   = args->len > 1 ? (int)perl_to_int(args->elems[1]) : 0;
        tm.tm_min   = args->len > 2 ? (int)perl_to_int(args->elems[2]) : 0;
        tm.tm_hour  = args->len > 3 ? (int)perl_to_int(args->elems[3]) : 0;
        tm.tm_mday  = args->len > 4 ? (int)perl_to_int(args->elems[4]) : 1;
        tm.tm_mon   = args->len > 5 ? (int)perl_to_int(args->elems[5]) : 0;
        tm.tm_year  = args->len > 6 ? (int)perl_to_int(args->elems[6]) : 0;
        tm.tm_wday  = args->len > 7 ? (int)perl_to_int(args->elems[7]) : 0;
        tm.tm_yday  = args->len > 8 ? (int)perl_to_int(args->elems[8]) : 0;
        tm.tm_isdst = args->len > 9 ? (int)perl_to_int(args->elems[9]) : -1;
    } else {
        time_t now = time(NULL);
        localtime_r(&now, &tm);
    }
    char buf[512];
    strftime(buf, sizeof(buf), fmt, &tm);
    free(fmt);
    return perl_alloc_string(buf);
}

/* ── Scalar::Util ─────────────────────────────────────────────────────────── */

PerlValue *perl_su_blessed(PerlValue *v) {
    if (!v) return perl_alloc_undef();
    if ((v->tag == PERL_REF_ARRAY || v->tag == PERL_REF_HASH ||
         v->tag == PERL_REF_SCALAR) && v->blessed_class)
        return perl_alloc_string(v->blessed_class);
    return perl_alloc_undef();
}

PerlValue *perl_su_reftype(PerlValue *v) {
    if (!v) return perl_alloc_undef();
    switch (v->tag) {
    case PERL_REF_SCALAR: return perl_alloc_string("SCALAR");
    case PERL_REF_ARRAY:  return perl_alloc_string("ARRAY");
    case PERL_REF_HASH:   return perl_alloc_string("HASH");
    case PERL_CODE_REF:   return perl_alloc_string("CODE");
    default:              return perl_alloc_undef();
    }
}

PerlValue *perl_su_looks_like_number(PerlValue *v) {
    if (!v || v->tag == PERL_UNDEF) return perl_alloc_int(0);
    if (v->tag == PERL_INT || v->tag == PERL_FLOAT) return perl_alloc_int(1);
    if (v->tag != PERL_STRING || !v->sval) return perl_alloc_int(0);
    const char *s = v->sval;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '+' || *s == '-') s++;
    if (*s == '\0') return perl_alloc_int(0);
    int has_digit = 0;
    while (*s >= '0' && *s <= '9') { has_digit = 1; s++; }
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { has_digit = 1; s++; } }
    if (!has_digit) return perl_alloc_int(0);
    if (*s == 'e' || *s == 'E') {
        s++;
        if (*s == '+' || *s == '-') s++;
        if (*s < '0' || *s > '9') return perl_alloc_int(0);
        while (*s >= '0' && *s <= '9') s++;
    }
    while (*s == ' ' || *s == '\t') s++;
    return perl_alloc_int(*s == '\0' ? 1 : 0);
}

/* ── Carp ─────────────────────────────────────────────────────────────────── */

void perl_carp_croak(PerlArray *args) {
    char *msg = (args && args->len > 0) ? perl_to_string(args->elems[0]) : strdup("Died");
    fprintf(stderr, "%s\n", msg);
    free(msg);
    exit(1);
}

void perl_carp_carp(PerlArray *args) {
    char *msg = (args && args->len > 0) ? perl_to_string(args->elems[0]) : strdup("Warning: something's wrong");
    fprintf(stderr, "%s\n", msg);
    free(msg);
}

/* ── File I/O ─────────────────────────────────────────────────────────────── */

PerlValue *perl_seek_fh(PerlValue *fh, PerlValue *off, PerlValue *whence) {
    if (!fh || fh->tag != PERL_FILEHANDLE || !fh->pval)
        return perl_alloc_int(0);
    long offset  = (long)perl_to_int(off);
    int  whencei = (int)perl_to_int(whence);
    int r = fseek((FILE*)fh->pval, offset, whencei);
    return perl_alloc_int(r == 0 ? 1 : 0);
}

PerlValue *perl_tell_fh(PerlValue *fh) {
    if (!fh || fh->tag != PERL_FILEHANDLE || !fh->pval)
        return perl_alloc_int(-1);
    return perl_alloc_int((long long)ftell((FILE*)fh->pval));
}

PerlValue *perl_binmode_fh(PerlValue *fh, PerlValue *layer) {
    (void)fh; (void)layer;
    return perl_alloc_int(1);
}

/* ── Filesystem ───────────────────────────────────────────────────────────── */
#include <sys/stat.h>

static PerlArray *_stat_to_array(struct stat *st) {
    PerlArray *a = perl_array_new();
    perl_array_push(a, perl_alloc_int((long long)st->st_dev));
    perl_array_push(a, perl_alloc_int((long long)st->st_ino));
    perl_array_push(a, perl_alloc_int((long long)st->st_mode));
    perl_array_push(a, perl_alloc_int((long long)st->st_nlink));
    perl_array_push(a, perl_alloc_int((long long)st->st_uid));
    perl_array_push(a, perl_alloc_int((long long)st->st_gid));
    perl_array_push(a, perl_alloc_int((long long)st->st_rdev));
    perl_array_push(a, perl_alloc_int((long long)st->st_size));
    perl_array_push(a, perl_alloc_int((long long)st->st_blksize));
    perl_array_push(a, perl_alloc_int((long long)st->st_blocks));
    perl_array_push(a, perl_alloc_int((long long)st->st_atime));
    perl_array_push(a, perl_alloc_int((long long)st->st_mtime));
    perl_array_push(a, perl_alloc_int((long long)st->st_ctime));
    return a;
}

PerlArray *perl_stat_path(PerlValue *v) {
    char *path = perl_to_string(v);
    struct stat st;
    PerlArray *a;
    if (stat(path, &st) == 0) {
        a = _stat_to_array(&st);
    } else {
        a = perl_array_new();
    }
    free(path);
    return a;
}

PerlArray *perl_lstat_path(PerlValue *v) {
    char *path = perl_to_string(v);
    struct stat st;
    PerlArray *a;
    if (lstat(path, &st) == 0) {
        a = _stat_to_array(&st);
    } else {
        a = perl_array_new();
    }
    free(path);
    return a;
}

#include <glob.h>

PerlArray *perl_glob_val(PerlValue *pattern) {
    char *pat = perl_to_string(pattern);
    PerlArray *res = perl_array_new();
    glob_t g;
    if (glob(pat, GLOB_TILDE | GLOB_NOCHECK, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++)
            perl_array_push(res, perl_alloc_string(g.gl_pathv[i]));
        globfree(&g);
    }
    free(pat);
    return res;
}

/* ── UNIVERSAL: isa / can ─────────────────────────────────────────────────── */

PerlValue *perl_isa_check(PerlValue *obj, PerlValue *class_pv) {
    if (!obj || !class_pv) return perl_alloc_int(0);
    const char *want = (class_pv->tag == PERL_STRING && class_pv->sval)
                       ? class_pv->sval : "";
    const char *got  = obj->blessed_class;
    if (!got) {
        /* check tag-based type names */
        const char *tname = NULL;
        switch (obj->tag) {
        case PERL_REF_ARRAY:  tname = "ARRAY"; break;
        case PERL_REF_HASH:   tname = "HASH"; break;
        case PERL_REF_SCALAR: tname = "SCALAR"; break;
        case PERL_CODE_REF:   tname = "CODE"; break;
        default: break;
        }
        if (tname && strcmp(tname, want) == 0) return perl_alloc_int(1);
        return perl_alloc_int(0);
    }
    /* walk ISA chain */
    const char *cls = got;
    for (int depth = 0; depth < 32; depth++) {
        if (strcmp(cls, want) == 0) return perl_alloc_int(1);
        const char *parent = perl_get_parent(cls);
        if (!parent) break;
        cls = parent;
    }
    return perl_alloc_int(0);
}

PerlValue *perl_can_check(PerlValue *obj, PerlValue *method_pv) {
    if (!obj || !method_pv) return perl_alloc_undef();
    char *method = perl_to_string(method_pv);
    const char *cls = obj->blessed_class;
    if (!cls) { free(method); return perl_alloc_undef(); }
    PerlSubFnCtx fn = perl_find_method(cls, method);
    free(method);
    if (!fn) return perl_alloc_undef();
    return perl_make_code_ref(fn);
}

/* ── Tier 3: read / fileno / truncate / each / pos / getpid / $^O ──────── */

PerlValue *perl_read_fh(PerlValue *fh, PerlValue *buf_pv, PerlValue *nbytes, PerlValue *offset) {
    if (!fh || fh->tag != PERL_FILEHANDLE || !fh->pval) return perl_alloc_int(0);
    FILE *fp = (FILE *)fh->pval;
    long long n = perl_to_int(nbytes);
    long long off = offset ? perl_to_int(offset) : 0;
    if (n <= 0) return perl_alloc_int(0);
    char *tmp = (char *)malloc(n + 1);
    if (!tmp) return perl_alloc_int(0);
    size_t got = fread(tmp, 1, (size_t)n, fp);
    tmp[got] = '\0';
    /* write into buf_pv (which is a stable PerlValue*) */
    if (buf_pv) {
        if (off > 0) {
            /* append at offset */
            char *cur = (buf_pv->tag == PERL_STRING && buf_pv->sval) ? buf_pv->sval : (char *)"";
            long long curlen = (long long)strlen(cur);
            if (off > curlen) off = curlen;
            char *newbuf = (char *)malloc(off + got + 1);
            memcpy(newbuf, cur, (size_t)off);
            memcpy(newbuf + off, tmp, got);
            newbuf[off + got] = '\0';
            if (buf_pv->tag == PERL_STRING && buf_pv->sval) free(buf_pv->sval);
            buf_pv->tag = PERL_STRING;
            buf_pv->sval = newbuf;
        } else {
            if (buf_pv->tag == PERL_STRING && buf_pv->sval) free(buf_pv->sval);
            buf_pv->tag = PERL_STRING;
            buf_pv->sval = tmp;
            tmp = NULL;
        }
    }
    if (tmp) free(tmp);
    return perl_alloc_int((long long)got);
}

PerlValue *perl_fileno_fh(PerlValue *fh) {
    if (!fh || fh->tag != PERL_FILEHANDLE || !fh->pval) return perl_alloc_undef();
    FILE *fp = (FILE *)fh->pval;
    int fd = fileno(fp);
    if (fd < 0) return perl_alloc_undef();
    return perl_alloc_int((long long)fd);
}

PerlValue *perl_truncate_fh(PerlValue *fh_or_path, PerlValue *len) {
    long long sz = perl_to_int(len);
    if (!fh_or_path) return perl_alloc_int(0);
    int rc = -1;
    if (fh_or_path->tag == PERL_FILEHANDLE && fh_or_path->pval) {
        FILE *fp = (FILE *)fh_or_path->pval;
        rc = ftruncate(fileno(fp), (off_t)sz);
    } else {
        char *path = perl_to_string(fh_or_path);
        rc = truncate(path, (off_t)sz);
        free(path);
    }
    return perl_alloc_int(rc == 0 ? 1 : 0);
}

PerlArray *perl_each_hash(PerlHash *h) {
    PerlArray *out = perl_array_new();
    if (!h) return out;
    static struct { PerlHash *h; int bucket; int chain; } iters[256];
    static int niters = 0;
    int idx = -1;
    for (int i = 0; i < niters; i++) {
        if (iters[i].h == h) { idx = i; break; }
    }
    if (idx < 0) {
        if (niters < 256) idx = niters++;
        else idx = 0;
        iters[idx].h = h;
        iters[idx].bucket = 0;
        iters[idx].chain = 0;
    }
    for (int b = iters[idx].bucket; b < PERL_HASH_BUCKETS; b++) {
        PerlHashEntry *e = h->buckets[b];
        int skip = (b == iters[idx].bucket) ? iters[idx].chain : 0;
        int pos = 0;
        while (e) {
            if (pos >= skip) {
                PerlValue *kv = perl_alloc_string(e->key);
                PerlValue *vv = perl_clone(e->val);
                perl_array_push(out, kv);
                perl_array_push(out, vv);
                if (e->next) {
                    iters[idx].bucket = b;
                    iters[idx].chain = pos + 1;
                } else {
                    iters[idx].bucket = b + 1;
                    iters[idx].chain = 0;
                }
                return out;
            }
            pos++;
            e = e->next;
        }
        if (b == iters[idx].bucket) iters[idx].chain = 0;
    }
    iters[idx].bucket = 0;
    iters[idx].chain = 0;
    return out;
}

PerlValue *perl_pos_str(PerlValue *pv) {
    if (!pv) return perl_alloc_undef();
    if (pv->matchpos <= 0) return perl_alloc_undef();
    return perl_alloc_int(pv->matchpos);
}

PerlValue *perl_getpid(void) {
    return perl_alloc_int((long long)getpid());
}

PerlValue *perl_get_os_name(void) {
    return perl_alloc_string("linux");
}
