#define PCRE2_CODE_UNIT_WIDTH 8
/* Force-inline helpers: HOT for file-private fns, HOTX for exported fns */
#define HOT  __attribute__((always_inline)) static inline
#define HOTX __attribute__((always_inline))
#include <pcre2.h>
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
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
#include <dlfcn.h>
#include <sqlite3.h>

/* D24: main.cpp bakes the perlc compiler's own absolute path into every
   compiled executable via -DPERLC_SELF_PATH, so perl_do_file() can
   re-invoke it at runtime. Builds that don't go through that dynamic
   compile step (the perlc binary's own Makefile build, perlc_tsan) never
   define this — perl_do_file() there is dead code (the compiler never
   runs Perl programs itself), but it still needs to compile. */
#ifndef PERLC_SELF_PATH
#define PERLC_SELF_PATH "perlc"
#endif

/* ── PerlValue freelist pool ─────────────────────────────────────────────── *
 * Avoids malloc/free per temp: freed PVs go onto a singly-linked list (next
 * pointer stored in pval union field), re-used on the next alloc. The pool
 * stabilises quickly at the peak concurrent-live count for the hot loop.
 */
/* Per-thread freelist: no mutex needed since each thread has its own */
static __thread PerlValue *pv_freelist_ = NULL;

/* Slab tracking: keeps a list of all allocated slabs for cleanup at exit.
 * This is always enabled (not just for PERL_ALLOC_DEBUG) so valgrind
 * can report zero leaks. */
#define PV_SLAB_CAP 256
static PerlValue *pv_slabs_[PV_SLAB_CAP];
static int pv_slab_count_ = 0;

/* ── PV leak-check debug mode (PERL_ALLOC_DEBUG) ────────────────────────────
 * When compiled with -DPERL_ALLOC_DEBUG, tracks every pv_alloc/pv_pool_push
 * pair.  At exit, reports any PVs that were allocated but never returned to
 * the freelist (i.e., genuinely leaked).  The freelist pool hides leaks from
 * valgrind because freed PVs are reused, not returned to the OS.
 *
 * Each allocated PV gets a sentinel value (0xDEADBEEF) in its `ival` field.
 * On pv_pool_push, the sentinel is cleared.  At exit, any PV with the
 * sentinel still set is a leak.
 *
 * This works because the PV freelist uses the pval union field as the next
 * pointer, and ival is separate.  The sentinel is harmless for normal use
 * since most ops check the tag first.
 */
#ifdef PERL_ALLOC_DEBUG
#define PV_LEAK_SENTINEL 0xDEADBEEFLL
static inline void pv_alloc_track(PerlValue *v) { v->ival = PV_LEAK_SENTINEL; }
static inline void pv_free_track(PerlValue *v)  { v->ival = 0; }
#else
static inline void pv_alloc_track(PerlValue *v) {}
static inline void pv_free_track(PerlValue *v)  {}
#endif

/* Slab size: allocate this many PVs at once on a cold miss.
 * Contiguous allocation keeps pool entries cache-hot, dramatically reducing
 * the cache-miss penalty that dominates perl_clone time in tight loops. */
#define PV_SLAB 128

static inline PerlValue *pv_alloc(void) {
    if (__builtin_expect(pv_freelist_ != NULL, 1)) {
        PerlValue *v  = pv_freelist_;
        pv_freelist_  = (PerlValue *)v->pval;
        return v;
    }
    /* Cold miss: allocate a contiguous slab, return first entry, link rest. */
    PerlValue *slab = calloc(PV_SLAB, sizeof(PerlValue));
    if (pv_slab_count_ < PV_SLAB_CAP) {
        pv_slabs_[pv_slab_count_++] = slab;
    }
    for (int i = PV_SLAB - 1; i >= 1; i--) {
        slab[i].pval  = (void *)pv_freelist_;
        pv_freelist_  = &slab[i];
    }
    PerlValue *result = &slab[0];   /* slab[0] is already zeroed by calloc */
    pv_alloc_track(result);
    return result;
}

static inline void pv_pool_push(PerlValue *v) {
    pv_free_track(v);
    v->pval      = pv_freelist_;
    pv_freelist_ = v;
}

/* ── PerlArray freelist pool ─────────────────────────────────────────────── *
 * Each sub call creates a PerlArray for @_ and frees it after. For tight
 * loops (e.g. mbs.pl calling cmul/cadd 21M times) this is ~84M malloc/free
 * pairs and dominates sys time. Pool both the struct and the elems buffer:
 * the "next" pointer is stored in a->mu (a void* field unused outside locks).
 * Max cap preserved is PA_POOL_CAP_MAX; larger elems are freed to prevent bloat.
 */
#define PA_POOL_CAP_MAX 4096
static __thread PerlArray *pa_pool_ = NULL;

static inline PerlArray *pa_alloc(void) {
    if (__builtin_expect(pa_pool_ != NULL, 1)) {
        PerlArray *a = pa_pool_;
        pa_pool_ = (PerlArray *)a->mu;  /* restore pool chain from mu field */
        a->mu = NULL;
        a->len = 0;
        a->refcount = 0;
        /* a->elems and a->cap are preserved from the previous use */
        return a;
    }
    PerlArray *a = malloc(sizeof *a);
    a->len = 0; a->cap = 8; a->refcount = 0; a->mu = NULL;
    a->elems = malloc(a->cap * sizeof(PerlValue *));
    return a;
}

static inline void pa_pool_push(PerlArray *a) {
    if (a->cap > PA_POOL_CAP_MAX) {
        free(a->elems);
        a->elems = malloc(8 * sizeof(PerlValue *));
        a->cap = 8;
    }
    a->mu = (pthread_mutex_t *)pa_pool_;   /* store next in mu field */
    pa_pool_ = a;
}

/* ── threads::shared side-table (Phase 2) ─────────────────────────────────── *
 * Each shared scalar's PerlValue carries PV_FLAG_SHARED.  The mutex+condvar
 * that lock()/cond_wait() needs is allocated lazily on the first such call
 * and kept in a process-wide hash table keyed by the cell address.  This
 * means:
 *   - allocation cost is paid only by shared vars that ever see lock()
 *   - the hot path (perl_atomic_load) is a single acquire fence, no
 *     function call into the runtime for ordering (on x86 it compiles to
 *     a plain MOV)
 *   - the install path (the first lock() on a given scalar) is guarded by
 *     a single global mutex; subsequent lookups are lock-free
 *
 * The plan acknowledges this is the trade-off: see THREADS_SHARED_ATOMIC_PLAN.md
 * §"Data layout" and §"Lock path".  The mutex is intentionally never
 * freed during the program's lifetime.
 *
 * Phase 3 re-entry handling: a shared scalar's `lock()` may legitimately
 * be called by the same thread that is also about to call
 * `perl_atomic_add/inc/dec` via the codegen for `$x = $x + 1`, `$x++`,
 * `$x += N`, etc.  Because pthread_mutex is non-recursive by default, the
 * atomic helpers would deadlock.  We track the (mutex, depth) per thread
 * via `s_held_mutex_*` TLS, and the atomic helpers consult it before
 * locking.  When the same thread already holds the mutex, the atomic
 * helper skips the lock entirely: the user has already serialised
 * access, so the RMW is by definition safe within this thread.
 */
#define SHARED_MUTEX_TABLE_BUCKETS 64
typedef struct SharedMutexEntry {
    PerlValue                   *pv;   /* key: cell address (identity) */
    SharedMutex                 *mu;   /* value: lazy-installed */
    struct SharedMutexEntry     *next;
} SharedMutexEntry;
static SharedMutexEntry *s_mutex_table[SHARED_MUTEX_TABLE_BUCKETS];
static pthread_mutex_t   s_mutex_table_mu = PTHREAD_MUTEX_INITIALIZER;

/* Per-thread "what SharedMutex is this thread currently holding, and at
 * what depth?"  Only the *innermost* held mutex is tracked (re-entry into
 * a different shared scalar would be detected and rejected by the
 * pthread_mutex_lock below).  The Phase 2 plan does not explicitly
 * cover this case, but the existing pattern in tests/threads.pl
 * (lock($x); $x = $x + 1;) requires it for correctness. */
static __thread SharedMutex *s_held_mutex_     = NULL;
static __thread int          s_held_mutex_depth_ = 0;

/* Pointer identity hash on the cell address. */
static inline unsigned int pv_bucket(const PerlValue *pv) {
    uintptr_t k = (uintptr_t)pv;
    k = (k >> 3) ^ (k >> 11) ^ (k >> 19);
    return (unsigned int)(k % SHARED_MUTEX_TABLE_BUCKETS);
}

/* Lock-free lookup.  Returns NULL if the cell has no SharedMutex yet. */
static SharedMutex *lookup_shared_mutex(PerlValue *pv) {
    if (!pv) return NULL;
    unsigned int b = pv_bucket(pv);
    for (SharedMutexEntry *e = s_mutex_table[b]; e; e = e->next) {
        if (e->pv == pv) return e->mu;
    }
    return NULL;
}

/* Get the SharedMutex for `pv`, installing one if missing.  Thread-safe
 * via s_mutex_table_mu; the common path (already-installed) does not take
 * the install mutex.  Never returns NULL. */
static SharedMutex *get_or_install_mutex(PerlValue *pv) {
    if (!pv) return NULL;
    SharedMutex *mu = lookup_shared_mutex(pv);
    if (mu) return mu;
    SharedMutex *fresh = calloc(1, sizeof *fresh);
    pthread_mutex_init(&fresh->mu,   NULL);
    pthread_cond_init( &fresh->cond, NULL);
    unsigned int b = pv_bucket(pv);
    pthread_mutex_lock(&s_mutex_table_mu);
    /* Re-check after taking the install lock — another thread may have
     * raced ahead of us. */
    for (SharedMutexEntry *e = s_mutex_table[b]; e; e = e->next) {
        if (e->pv == pv) {
            pthread_mutex_unlock(&s_mutex_table_mu);
            pthread_mutex_destroy(&fresh->mu);
            pthread_cond_destroy(&fresh->cond);
            free(fresh);
            return e->mu;
        }
    }
    SharedMutexEntry *ne = malloc(sizeof *ne);
    ne->pv   = pv;
    ne->mu   = fresh;
    ne->next = s_mutex_table[b];
    s_mutex_table[b] = ne;
    pthread_mutex_unlock(&s_mutex_table_mu);
    return fresh;
}

/* ── local() save/restore stack ─────────────────────────────────────────── */

#define LOCAL_STACK_MAX 4096
#define LOCAL_SCALAR  0   /* save/restore a PerlValue (existing behaviour) */
#define LOCAL_LOCK_PV 1   /* auto-unlock a SharedMutex (lazy-installed on shared scalar) on scope exit */
#define LOCAL_LOCK_AV 2   /* auto-unlock a PerlArray->mu on scope exit */
#define LOCAL_LOCK_HV 3   /* auto-unlock a PerlHash->mu on scope exit */
#define LOCAL_ARRAY   4   /* save/restore a PerlArray* (local @arr) */
#define LOCAL_HASH    5   /* save/restore a PerlHash*  (local %hash) */
typedef struct {
    int type;
    PerlValue *ptr;   /* LOCAL_SCALAR: target PerlValue* */
    PerlValue saved;  /* LOCAL_SCALAR: saved content */
    void *ptr2;       /* LOCAL_ARRAY/HASH: slot (PerlArray** or PerlHash**) */
    void *saved2;     /* LOCAL_ARRAY/HASH: saved pointer value */
} LocalEntry;
static __thread LocalEntry s_local_stack[LOCAL_STACK_MAX];
static __thread int        s_local_depth = 0;

int perl_local_save_depth(void) { return s_local_depth; }

void perl_local_save(PerlValue *pv) {
    if (s_local_depth >= LOCAL_STACK_MAX) return;
    s_local_stack[s_local_depth].type  = LOCAL_SCALAR;
    s_local_stack[s_local_depth].ptr   = pv;
    s_local_stack[s_local_depth].saved = *pv;
    /* deep-copy string/blessed_class so the saved value is independent */
    if (pv->tag == PERL_STRING && pv->sval) {
        /* D85: length-aware copy — strdup would truncate at an embedded
           NUL; `saved = *pv` above already copied pv->slen correctly. */
        long long n = pv->slen;
        char *copy = malloc((size_t)n + 1);
        if (n > 0) memcpy(copy, pv->sval, (size_t)n);
        copy[n] = '\0';
        s_local_stack[s_local_depth].saved.sval = copy;
    }
    if (pv->blessed_class)
        s_local_stack[s_local_depth].saved.blessed_class = strdup(pv->blessed_class);
    s_local_depth++;
}

void perl_local_restore_to(int depth) {
    while (s_local_depth > depth) {
        s_local_depth--;
        LocalEntry *e = &s_local_stack[s_local_depth];
        if (e->type == LOCAL_LOCK_PV) {
            /* Auto-unlock mirrors perl_unlock_shared: respect re-entry. */
            SharedMutex *mu = lookup_shared_mutex((PerlValue *)e->ptr);
            if (mu) {
                if (s_held_mutex_ == mu) {
                    s_held_mutex_depth_--;
                    if (s_held_mutex_depth_ == 0) {
                        pthread_mutex_unlock(&mu->mu);
                        s_held_mutex_ = NULL;
                    }
                } else {
                    pthread_mutex_unlock(&mu->mu);
                }
            }
        } else if (e->type == LOCAL_LOCK_AV) {
            pthread_mutex_unlock(((PerlArray *)e->ptr)->mu);
        } else if (e->type == LOCAL_LOCK_HV) {
            pthread_mutex_unlock(((PerlHash *)e->ptr)->mu);
        } else if (e->type == LOCAL_ARRAY) {
            PerlArray **slot = (PerlArray **)e->ptr2;
            PerlArray *cur   = *slot;
            *slot = (PerlArray *)e->saved2;
            if (cur != e->saved2) perl_array_free(cur);
        } else if (e->type == LOCAL_HASH) {
            PerlHash **slot = (PerlHash **)e->ptr2;
            PerlHash *cur   = *slot;
            *slot = (PerlHash *)e->saved2;
            if (cur != e->saved2) perl_hash_free(cur);
        } else { /* LOCAL_SCALAR */
            /* free any existing string/blessed_class in the target */
            if ((e->ptr->tag == PERL_STRING) && e->ptr->sval) free(e->ptr->sval);
            if (e->ptr->blessed_class) free(e->ptr->blessed_class);
            *e->ptr = e->saved;  /* restore saved value */
        }
    }
    /* If the stack overflowed (s_local_depth was capped at LOCAL_STACK_MAX),
        there may be unreleased locks whose entries were never pushed.
        Check if held_mutex_depth > depth and force-release if so. */
    if (s_held_mutex_depth_ > depth) {
        SharedMutex *mu = s_held_mutex_;
        if (mu) {
            s_held_mutex_depth_ = depth;
            pthread_mutex_unlock(&mu->mu);
            s_held_mutex_ = NULL;
        }
    }
}

void perl_local_save_array(PerlArray **slot) {
    if (s_local_depth >= LOCAL_STACK_MAX) return;
    s_local_stack[s_local_depth].type   = LOCAL_ARRAY;
    s_local_stack[s_local_depth].ptr    = NULL;
    s_local_stack[s_local_depth].ptr2   = slot;
    s_local_stack[s_local_depth].saved2 = *slot;
    s_local_depth++;
}

void perl_local_save_hash(PerlHash **slot) {
    if (s_local_depth >= LOCAL_STACK_MAX) return;
    s_local_stack[s_local_depth].type   = LOCAL_HASH;
    s_local_stack[s_local_depth].ptr    = NULL;
    s_local_stack[s_local_depth].ptr2   = slot;
    s_local_stack[s_local_depth].saved2 = *slot;
    s_local_depth++;
}

/* ── eval / $@ support ───────────────────────────────────────────────────── */

/* jmp_buf pointers are pushed by callers (codegen allocates jmp_buf on stack) */
#define EVAL_STACK_MAX 64
static __thread jmp_buf *s_eval_stack[EVAL_STACK_MAX];
static __thread int      s_eval_local_depth[EVAL_STACK_MAX]; /* local()-stack depth at eval entry */
static __thread int      s_eval_depth = 0;
 static __thread PerlValue s_dollar_at; /* $@ — zero-initialized = UNDEF per thread */

/* Forward declarations for die-related functions — needed so calls before
   the definition (perl_mod, div-by-zero, substr) see the correct signature. */
static char *appendDieLocation(char *msg, const char *filename, int line);
void perl_die(PerlValue *msg, const char *filename, int line);

/* $/ — input record separator (default "\n", undef = slurp mode) */
static PerlValue s_input_sep = { .tag = PERL_STRING };
static int       s_input_sep_inited = 0;

static void ensure_input_sep(void) {
    if (!s_input_sep_inited) {
        s_input_sep.tag  = PERL_STRING;
        s_input_sep.sval = strdup("\n");
        s_input_sep.slen = 1;
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

/* forward decl — PerlArray defined later in this file */
typedef struct PerlArray PerlArray;
void perl_print_array(PerlArray *a) {
    for (long long i = 0; i < a->len; i++) {
        if (i > 0) perl_print_sep();
        perl_print(a->elems[i]);
    }
}


/* $! — errno as a string */
static PerlValue s_dollar_bang = { .tag = PERL_UNDEF };
PerlValue *perl_get_dollar_bang(void) {
    if (s_dollar_bang.tag == PERL_STRING && s_dollar_bang.sval) free(s_dollar_bang.sval);
    if (errno) {
        s_dollar_bang.tag  = PERL_STRING;
        s_dollar_bang.sval = strdup(strerror(errno));
        s_dollar_bang.slen = (long long)strlen(s_dollar_bang.sval);
    } else {
        s_dollar_bang.tag  = PERL_STRING;
        s_dollar_bang.sval = strdup("");
        s_dollar_bang.slen = 0;
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
  /* D43: called outside any sub (empty stack) — real Perl's wantarray()
     returns undef here, not a false-but-defined 0. Also avoids reading
     s_wantarray_stack[-1] (out of bounds) when the stack is empty. */
  if (s_wantarray_depth <= 0) return perl_alloc_undef();
  return perl_alloc_int(s_wantarray_stack[s_wantarray_depth - 1]);
}

/* peek current wantarray context without modifying the stack */
int perl_current_wantarray_ctx(void) {
    return (s_wantarray_depth > 0) ? s_wantarray_stack[s_wantarray_depth - 1] : 0;
}

/* list-context return: wrap PerlArray* in REF_ARRAY if wantarray, else take last elem */
PerlValue *perl_array_to_list_return(PerlArray *av) {
    int ctx = (s_wantarray_depth > 0) ? s_wantarray_stack[s_wantarray_depth - 1] : 0;
    if (ctx) {
        PerlValue *r = pv_alloc();
        r->tag = PERL_LIST_RESULT;  /* distinct from PERL_REF_ARRAY so scalar refs are not spread */
        r->flags = 0; r->matchpos = 0; r->blessed_class = NULL;
        r->pval = av;
        av->refcount = 1;
        return r;
    } else {
        PerlValue *last = (av->len > 0) ? perl_clone(av->elems[av->len - 1])
                                        : perl_alloc_undef();
        perl_array_free(av);
        return last;
    }
}

/* caller-side unwrap: extract PerlArray* from list-context function result.
   Only PERL_LIST_RESULT (from perl_array_to_list_return) is spread; plain
   PERL_REF_ARRAY scalar refs are passed as a single element so that e.g.
   cadd(cmul($z,$z), $c) does not incorrectly flatten the cmul return ref. */
PerlArray *perl_unwrap_list_return(PerlValue *pv) {
    if (!pv) return perl_array_new();
    if (pv->tag == PERL_LIST_RESULT && pv->pval) {
        PerlArray *av = (PerlArray *)pv->pval;
        pv->pval = NULL;
        perl_free(pv);
        return av;
    }
    PerlArray *av = perl_array_new();
    perl_array_push(av, pv);
    return av;
}

/* ── call stack for caller() ─────────────────────────────────────────────── */
#define CALL_STACK_MAX 512
typedef struct { const char *package; const char *file; int line; } CallFrame;
static __thread CallFrame s_call_stack[CALL_STACK_MAX];
static __thread int       s_call_depth = 0;

void perl_push_call_frame(const char *pkg, const char *file, int line) {
    if (s_call_depth < CALL_STACK_MAX) {
        s_call_stack[s_call_depth].package = pkg;
        s_call_stack[s_call_depth].file    = file;
        s_call_stack[s_call_depth].line    = line;
        s_call_depth++;
    }
}

void perl_pop_call_frame(void) {
    if (s_call_depth > 0) s_call_depth--;
}

PerlArray *perl_caller(int level) {
    /* s_call_depth-1 is the frame pushed by the current sub's call site.
       level 0 = immediate caller, level 1 = caller's caller, etc. */
    int idx = s_call_depth - 1 - level;
    if (idx < 0) return perl_array_new(); /* out of range → empty list */
    PerlArray *a = perl_array_new();
    perl_array_push(a, perl_alloc_string(s_call_stack[idx].package
                                          ? s_call_stack[idx].package : "main"));
    perl_array_push(a, perl_alloc_string(s_call_stack[idx].file
                                          ? s_call_stack[idx].file : ""));
    perl_array_push(a, perl_alloc_int(s_call_stack[idx].line));
    return a;
}

void perl_eval_push(jmp_buf *jb) {
    if (s_eval_depth < EVAL_STACK_MAX) {
        s_eval_local_depth[s_eval_depth] = perl_local_save_depth();
        s_eval_stack[s_eval_depth++] = jb;
    }
}

void perl_eval_pop(void) {
    if (s_eval_depth > 0) s_eval_depth--;
}

PerlValue *perl_get_dollar_at(void) { return &s_dollar_at; }

static void perl_set_dollar_at_cstr(const char *msg) {
    /* D85: .slen must be set explicitly — perl_assign now copies exactly
       src->slen bytes, and a designated initializer zero-inits any field
       not listed, which would silently truncate the whole message to "". */
    const char *m = msg ? msg : "";
    PerlValue pv = { .tag = PERL_STRING, .sval = (char *)m, .slen = (long long)strlen(m) };
    perl_assign(&s_dollar_at, &pv);
}

static int perl_resolve_runtime_path(const char *name, int module_semantics,
                                     char *resolved, size_t resolved_sz) {
    char candidate[1024];
    if (!name || !*name) return 0;

    if (module_semantics && strstr(name, "::")) {
        size_t n = strlen(name);
        if (n >= sizeof(candidate) - 4) return 0;
        size_t j = 0;
        for (size_t i = 0; i < n; i++) {
            if (name[i] == ':' && i + 1 < n && name[i + 1] == ':') {
                candidate[j++] = '/';
                i++;
            } else {
                candidate[j++] = name[i];
            }
        }
        if (j < 3 || memcmp(candidate + j - 3, ".pm", 3) != 0) {
            candidate[j++] = '.';
            candidate[j++] = 'p';
            candidate[j++] = 'm';
        }
        candidate[j] = '\0';
    } else {
        snprintf(candidate, sizeof(candidate), "%s", name);
    }

    FILE *f = fopen(candidate, "r");
    if (f) {
        fclose(f);
        snprintf(resolved, resolved_sz, "%s", candidate);
        return 1;
    }

    char libpath[1024];
    snprintf(libpath, sizeof(libpath), "lib/%s", candidate);
    f = fopen(libpath, "r");
    if (f) {
        fclose(f);
        snprintf(resolved, resolved_sz, "%s", libpath);
        return 1;
    }

    if (module_semantics) {
        char errmsg[1200];
        snprintf(errmsg, sizeof(errmsg), "Can't locate %s in @INC", candidate);
        perl_set_dollar_at_cstr(errmsg);
    } else {
        char errmsg[1200];
        snprintf(errmsg, sizeof(errmsg), "do \"%s\" failed, '.' is no longer in @INC; did you mean do \"./%s\"?", candidate, candidate);
        perl_set_dollar_at_cstr(errmsg);
    }
    return 0;
}

static char *perl_read_runtime_file(const char *path) {
    FILE *f = fopen(path, "r");
    char *code;
    long sz;
    size_t got;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);

    code = malloc((size_t)sz + 1);
    if (!code) { fclose(f); return NULL; }
    got = fread(code, 1, (size_t)sz, f);
    fclose(f);
    code[got] = '\0';
    return code;
}

static PerlValue *perl_eval_loaded_code(const char *code) {
    if (!code) return perl_alloc_undef();
    /* runtime source eval (require/do of dynamic files) not available after JIT removal.
       Static compile-time require/use via driver inlining still works.
       The caller (perl_do_file / perl_runtime_require) already validated
       that the file exists and is readable.  We return 1 to indicate success.
       The file is NOT actually parsed/executed — that requires JIT. */
    return perl_alloc_int(1);
}

/* ── runtime require ─────────────────────────────────────────────────────── */
PerlValue *perl_runtime_require(const char *modname) {
    char path[1024];
    char *code;
    PerlValue *result;

    if (!modname || !*modname) return perl_alloc_int(1);
    if (!perl_resolve_runtime_path(modname, 1, path, sizeof(path)))
        return perl_alloc_undef();

    code = perl_read_runtime_file(path);
    if (!code) {
        perl_set_dollar_at_cstr("require: failed to read file");
        return perl_alloc_undef();
    }

    result = perl_eval_loaded_code(code);
    free(code);
    if (!result) return perl_alloc_undef();
    if (s_dollar_at.tag != PERL_UNDEF && s_dollar_at.sval && s_dollar_at.sval[0]) {
        perl_free(result);
        return perl_alloc_undef();
    }

    perl_free(result);
    return perl_alloc_int(1);
}

/* ── do FILE (D24) ────────────────────────────────────────────────────────
   Real `do FILE` semantics require compiling and running arbitrary Perl
   code at runtime, which isn't possible directly since the JIT was
   removed. Implemented instead by re-invoking the perlc compiler itself
   (via PERLC_SELF_PATH, baked into this executable at link time) as a
   subprocess to compile the target file into a shared library
   (`--do-lib`), then dlopen()ing it. That .so deliberately does NOT link
   its own copy of runtime.c (see main.cpp's --do-lib link step) — its
   perl_* symbol references stay undefined and resolve at dlopen() time
   against *this* process's own already-linked runtime (this process must
   have been linked with -rdynamic, also done in main.cpp), so a do'd
   file's registered subs, $@, the PV allocator, etc. all share the same
   runtime state as the caller, instead of an isolated second copy of
   every global perl_do_lib_cleanup() dlclose()s at process exit — kept
   loaded for the process's lifetime so registered subs stay callable. */
typedef struct PerlDoLibInfo {
    void *handle;
    struct PerlDoLibInfo *next;
} PerlDoLibInfo;
static __thread PerlDoLibInfo *s_do_lib_list = NULL;

void perl_do_lib_cleanup(void) {
    PerlDoLibInfo *n = s_do_lib_list;
    while (n) {
        PerlDoLibInfo *next = n->next;
        if (n->handle) dlclose(n->handle);
        free(n);
        n = next;
    }
    s_do_lib_list = NULL;
}

PerlValue *perl_do_file(PerlValue *path_pv) {
    char resolved[1024];
    char *path;

    if (!path_pv || path_pv->tag == PERL_UNDEF) return perl_alloc_undef();
    path = perl_to_string_dup(path_pv);
    if (!path) return perl_alloc_undef();

    if (!perl_resolve_runtime_path(path, 0, resolved, sizeof(resolved))) {
        free(path);
        return perl_alloc_undef();
    }
    free(path);

    static long long s_do_counter = 0;
    long long id = __atomic_fetch_add(&s_do_counter, 1, __ATOMIC_RELAXED);
    char soPath[256], errPath[256];
    snprintf(soPath,  sizeof(soPath),  "/tmp/_perlc_do_%d_%lld.so",  (int)getpid(), id);
    snprintf(errPath, sizeof(errPath), "/tmp/_perlc_do_%d_%lld.err", (int)getpid(), id);

    char cmd[2560];
    snprintf(cmd, sizeof(cmd), "%s --do-lib \"%s\" -o \"%s\" >\"%s\" 2>&1",
             PERLC_SELF_PATH, resolved, soPath, errPath);
    int rc = system(cmd);

    if (!WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
        char *errtext = perl_read_runtime_file(errPath);
        char msg[1200];
        if (errtext && errtext[0])
            snprintf(msg, sizeof(msg), "do \"%s\" failed: %s", resolved, errtext);
        else
            snprintf(msg, sizeof(msg), "do \"%s\" failed: compilation error", resolved);
        perl_set_dollar_at_cstr(msg);
        free(errtext);
        unlink(soPath);
        unlink(errPath);
        return perl_alloc_undef();
    }
    unlink(errPath);

    void *handle = dlopen(soPath, RTLD_NOW);
    unlink(soPath); /* safe post-dlopen on Linux (inode stays alive while mapped) */
    if (!handle) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "do \"%s\" failed to load: %s", resolved, dlerror());
        perl_set_dollar_at_cstr(msg);
        return perl_alloc_undef();
    }

    PerlSubFnCtx entry = (PerlSubFnCtx)dlsym(handle, "__perlc_do_run");
    if (!entry) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "do \"%s\": internal error (%s)", resolved, dlerror());
        perl_set_dollar_at_cstr(msg);
        dlclose(handle);
        return perl_alloc_undef();
    }

    /* keep the library loaded for the rest of the process's life — any
       package-qualified subs it registered must stay callable. */
    PerlDoLibInfo *info = malloc(sizeof(*info));
    info->handle = handle;
    info->next = s_do_lib_list;
    s_do_lib_list = info;

    /* run the file's top-level code wrapped exactly like eval{} — a
       die() inside longjmps back here via the same s_eval_stack
       mechanism perl_die() already uses, setting $@ and giving us
       undef here instead of unwinding further. */
    jmp_buf jb;
    PerlValue *result;
    PerlArray *emptyArgs = perl_array_new();
    if (setjmp(jb) == 0) {
        perl_eval_push(&jb);
        result = entry(emptyArgs, 0); /* do FILE is always scalar context */
        perl_eval_pop();
        /* success: clear $@, matching eval{}'s post-success contract */
        PerlValue empty = { .tag = PERL_STRING, .sval = "" };
        perl_assign(&s_dollar_at, &empty);
    } else {
        perl_eval_pop();
        result = perl_alloc_undef();
        /* $@ already set by perl_die() before the longjmp */
    }
    perl_array_free(emptyArgs);
    return result ? result : perl_alloc_undef();
}

/* ── tie/untie (minimal TIESCALAR/TIEARRAY/TIEHASH) ───────────────────────── */
PerlValue *perl_tie(PerlValue *args_arr) {
    if (!args_arr || args_arr->tag != PERL_REF_ARRAY) {
        perl_set_dollar_at_cstr("tie: expected args array ref");
        return perl_alloc_undef();
    }

    PerlArray *arr = (PerlArray *)args_arr->pval;
    if (!arr || arr->len < 2) {
        perl_set_dollar_at_cstr("tie: need at least var and class");
        return perl_alloc_undef();
    }

    PerlValue *var_pv = arr->elems[0];
    const char *class_name = perl_to_string(arr->elems[1]);

    if (!class_name || !class_name[0]) return perl_alloc_undef();

    /* build extra args (skip var_pv and class_name) */
    int extra_count = arr->len - 2;
    PerlArray *extra_arr = perl_array_new();
    for (int i = 2; i < arr->len; i++) {
        perl_array_set(extra_arr, i - 2, arr->elems[i]);
    }

    /* call CLASS->TIEHASH/TIESCALAR/TIEARRAY(var, extra_args...) */
    PerlValue *result = perl_dispatch_method(var_pv, class_name, extra_arr);

    perl_array_free(extra_arr);

    if (!result || result->tag == PERL_UNDEF) {
        perl_set_dollar_at_cstr("tie: class has no TIESCALAR/TIEARRAY/TIEHASH");
        return perl_alloc_undef();
    }

    /* bless the result */
    PerlValue *class_pv = perl_alloc_string(class_name);
    perl_bless(result, class_pv);
    perl_free(class_pv);

    /* store the blessed reference in the tied variable */
    if (var_pv && var_pv->tag == PERL_REF_SCALAR) {
        PerlValue **slot = (PerlValue **)var_pv->pval;
        if (slot && *slot) perl_free(*slot);
        *slot = perl_clone(result);
    }

    return result;
}

void perl_untie(PerlValue *var_pv) {
    if (!var_pv || var_pv->tag == PERL_UNDEF) return;

    PerlValue *obj = var_pv;
    if (var_pv->tag == PERL_REF_SCALAR) {
        PerlValue **slot = (PerlValue **)var_pv->pval;
        if (slot && *slot) obj = *slot;
    }

    if (!obj || !obj->blessed_class) return;

    /* call UNTIE() on the blessed object */
    perl_dispatch_method(obj, "UNTIE", NULL);
}

/* ── string eval (disabled — no JIT) ────────────────────────────────────── */
PerlValue *perl_eval_string(PerlValue *code_pv) {
    PerlValue empty = { .tag = PERL_STRING, .sval = "" };
    perl_assign(&s_dollar_at, &empty);
    static const char evalmsg[] = "eval: string eval not available (JIT removed)";
    /* D85: .slen must be set explicitly on this literal — perl_assign now
       copies exactly src->slen bytes, and a designated initializer
       zero-inits any field not listed, silently truncating to "". */
    PerlValue msg = { .tag = PERL_STRING,
        .sval = (char *)evalmsg, .slen = (long long)(sizeof(evalmsg) - 1) };
    perl_assign(&s_dollar_at, &msg);
    return perl_alloc_undef();
}

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

/* 2-element float array stored inline in one PerlValue: no inner PerlArray, no heap.
   fval = elem[0]; matchpos bits reinterpreted as double = elem[1]. */
PerlValue *perl_alloc_float_pair(double re, double im) {
    PerlValue *v = pv_alloc();
    v->tag = PERL_FLOAT_PAIR;
    v->fval = re;
    memcpy(&v->matchpos, &im, sizeof(double)); /* bitcast double→long long */
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

PerlValue *perl_alloc_float_array(long long n) {
    PerlValue *v = pv_alloc();
    v->tag = PERL_FLAT_ARRAY;
    if (n > 0) {
        v->pval = calloc((size_t)n, sizeof(double));  /* zero-initialized */
    }
    v->matchpos = n;
    v->blessed_class = NULL;
    return v;
}

PerlValue *perl_alloc_xs_ptr(void *p) {
    PerlValue *v = pv_alloc();
    v->tag = PERL_XS_PTR;
    v->pval = p;
    v->matchpos = 0;
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

/* D85: allocate a PERL_STRING PV whose data is exactly `len` bytes of `s`,
   which may contain embedded NUL bytes anywhere within that range. The
   allocated buffer is len+1 bytes, with sval[len] forced to '\0' for
   backward compatibility with any remaining strlen()-based consumer that
   hasn't been made length-aware — such a consumer sees a truncated PREFIX
   (no worse than the pre-D85 behavior everywhere), while every
   length-aware consumer (perl_to_string_dup_len, perl_clone, perl_assign,
   perl_length, perl_print/say, perl_concat, perl_str_eq/etc., and pack/
   unpack itself) sees the true, full data via `slen`. */
PerlValue *perl_alloc_string_len(const char *s, long long len) {
    PerlValue *v = pv_alloc();
    v->tag = PERL_STRING;
    if (len < 0) len = 0;
    v->sval = malloc((size_t)len + 1);
    if (len > 0 && s) memcpy(v->sval, s, (size_t)len);
    v->sval[len] = '\0';
    v->slen = len;
    v->matchpos = 0;
    v->blessed_class = NULL;
    return v;
}

PerlValue *perl_alloc_string(const char *s) {
    return perl_alloc_string_len(s, s ? (long long)strlen(s) : 0);
}

PerlValue *perl_clone(const PerlValue *src) {
    if (!src) return perl_alloc_undef();
    if (src->tag == PERL_STRING) {
        PerlValue *v = perl_alloc_string_len(src->sval, src->slen);
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
    /* FLOAT_PAIR stores the imaginary part in matchpos — must NOT be zeroed. */
    if (src->tag != PERL_FLOAT_PAIR) v->matchpos = 0;
    v->blessed_class = src->blessed_class ? strdup(src->blessed_class) : NULL;
    if (src->tag == PERL_REF_ARRAY && src->pval) {
        PerlArray *av = (PerlArray *)src->pval;
        if (av->refcount > 0) av->refcount++;
    } else if (src->tag == PERL_REF_HASH && src->pval) {
        PerlHash *hv = (PerlHash *)src->pval;
        if (hv->refcount > 0) hv->refcount++;
    } else if (src->tag == PERL_LIST_RESULT && src->pval) {
        /* LIST_RESULT wraps a PerlArray* — increment refcount so freeIfOwned
           on the original doesn't free the array while the clone still holds it. */
        PerlArray *av = (PerlArray *)src->pval;
        if (av->refcount > 0) av->refcount++;
    } else if (src->tag == PERL_DBI_DBH && src->pval) {
        ((PerlDBIHandle *)src->pval)->refcount++;
    } else if (src->tag == PERL_DBI_STH && src->pval) {
        ((PerlDBIStatement *)src->pval)->refcount++;
    } else if (src->tag == PERL_CODE_REF && src->pval) {
        /* D62: pval is shallow-copied above (*v = *src) — both v and src
           now point at the same PerlClosure; bump its refcount so freeing
           either one doesn't tear down the closure while the other still
           references it. */
        ((PerlClosure *)src->pval)->refcount++;
    }
    return v;
}

static __thread int s_destroy_depth = 0;
static PerlSubFnCtx perl_find_method(const char *class_name, const char *method);
static PerlDBIHandle *perl_dbi_get_dbh(PerlValue *pv);
static PerlDBIStatement *perl_dbi_get_sth(PerlValue *pv);
static void perl_dbi_handle_release(PerlDBIHandle *dbh);
static void perl_dbi_statement_release(PerlDBIStatement *sth);
static void perl_release_capture(PerlValue *v);   /* D62 */
static void perl_closure_release(PerlClosure *cl); /* D62 */

static inline unsigned pv_capture_count(const PerlValue *v) {
    return (v->flags & PV_CAPTURE_MASK) >> PV_CAPTURE_SHIFT;
}
static inline void pv_capture_count_set(PerlValue *v, unsigned n) {
    v->flags = (unsigned)((v->flags & ~PV_CAPTURE_MASK) | ((n << PV_CAPTURE_SHIFT) & PV_CAPTURE_MASK));
}

HOTX void perl_free(PerlValue *v) {
    if (!v) return;
    /* Shared vars are program-lifetime: never returned to the pv pool and
       never freed.  The cell's contents (string/array/hash payloads) are
       owned by program-lifetime shared scope and will be cleaned up
       implicitly at process exit.  (Phase 2: the cell *is* the PerlValue
       itself, so we no longer skip a PerlSharedVar wrapper — but we still
       must not pool the cell, because shared scalars live for the entire
       program and the pool assumes the contents have been torn down.) */
    if (v->flags & PV_FLAG_SHARED) return;
    /* D62: still captured by a live closure/sort-comparator? Defer the
       real free — record that this release happened, and let whichever
       capture-holder's own release brings the count to 0 perform the
       actual free (see perl_release_capture below). Must come before any
       tag-specific payload teardown: a still-captured PV must not have
       its string/array/hash payload freed, run DESTROY, or be pooled —
       any of those would dangle the closure(s) still holding it. */
    unsigned ccount = pv_capture_count(v);
    if (ccount > 0) {
        v->flags |= PV_FLAG_CAPTURE_RELEASED;
        return;
    }
    if (v->tag == PERL_STRING) free(v->sval);
    if (v->tag == PERL_FLAT_ARRAY) free(v->pval);
    if (v->tag == PERL_LIST_RESULT && v->pval) {
        PerlArray *av = (PerlArray *)v->pval;
        if (av->refcount > 0 && --av->refcount == 0) perl_array_free(av);
    }
    if (v->tag == PERL_DBI_STH && v->pval)
        perl_dbi_statement_release((PerlDBIStatement *)v->pval);
    if (v->tag == PERL_DBI_DBH && v->pval)
        perl_dbi_handle_release((PerlDBIHandle *)v->pval);
    if (v->tag == PERL_REF_ARRAY && v->pval) {
        PerlArray *av = (PerlArray *)v->pval;
        /* call DESTROY on blessed array objects */
        if (av->refcount == 1 && v->blessed_class && s_destroy_depth < 100) {
            PerlSubFnCtx fn = perl_find_method(v->blessed_class, "DESTROY");
            if (fn) {
                s_destroy_depth++;
                PerlArray *args = perl_array_new();
                PerlValue *self = perl_clone(v);
                perl_array_push(args, self);
                PerlValue *ret = fn(args, perl_push_wantarray(0));
                perl_pop_wantarray();
                perl_free(ret);
                perl_array_free(args);
                perl_free(self);
                s_destroy_depth--;
            }
        }
        if (av->refcount > 0 && --av->refcount == 0) perl_array_free(av);
    }
    if (v->tag == PERL_REF_HASH && v->pval) {
        PerlHash *hv = (PerlHash *)v->pval;
        /* call DESTROY before the hash is freed */
        if (hv->refcount == 1 && v->blessed_class && s_destroy_depth < 100) {
            PerlSubFnCtx fn = perl_find_method(v->blessed_class, "DESTROY");
            if (fn) {
                s_destroy_depth++;
                PerlArray *args = perl_array_new();
                PerlValue *self = perl_clone(v);
                perl_array_push(args, self);
                PerlValue *ret = fn(args, perl_push_wantarray(0));
                perl_pop_wantarray();
                perl_free(ret);
                perl_array_free(args);
                perl_free(self);
                s_destroy_depth--;
            }
        }
        if (hv->refcount > 0 && --hv->refcount == 0) perl_hash_free(hv);
    }
    /* CODE_REF: pval points to a refcounted PerlClosure (D62) — release
       this wrapper's share; perl_closure_release only actually tears down
       the closure (and releases its own captures) once no PerlValue
       wrapper references it any more. */
    if (v->tag == PERL_CODE_REF && v->pval) {
        perl_closure_release((PerlClosure *)v->pval);
    }
    if (v->blessed_class) free(v->blessed_class);
    v->flags = 0;      /* D62: guarantee pooled PVs always start clean */
    pv_pool_push(v);   /* return struct to freelist instead of free() */
}

/* D62: decrement a captured PV's outstanding-release count; the release
   (scope-pop or closure-teardown) that brings it to 0 *after* the
   declaring scope has also already released its own share (recorded via
   PV_FLAG_CAPTURE_RELEASED) performs the actual free by re-entering
   perl_free(), which this time sees ccount==0 and falls through. */
static void perl_release_capture(PerlValue *v) {
    if (!v) return;
    if (v->flags & PV_FLAG_SHARED) return;   /* never tracked — matches capture-time skip */
    unsigned n = pv_capture_count(v);
    if (n == 0 || n == PV_CAPTURE_MAX) return;  /* not tracked, or pinned/leaked on overflow */
    n--;
    pv_capture_count_set(v, n);
    if (n == 0 && (v->flags & PV_FLAG_CAPTURE_RELEASED)) perl_free(v);
}

/* D62: release this PerlValue wrapper's share of the closure; tears the
   closure down (freeing its captures array and itself) only once no
   wrapper references it any more. */
static void perl_closure_release(PerlClosure *cl) {
    if (!cl) return;
    if (--cl->refcount > 0) return;
    for (int i = 0; i < cl->ncaptures; i++) perl_release_capture(cl->captures[i]);
    free(cl->captures);
    free(cl);
}

/* ── coercions ───────────────────────────────────────────────────────────── */

/* D79: Perl's implicit string→number coercion only recognizes decimal digits
   (and an optional leading sign). Unlike C's atof/strtoll, it does NOT
   auto-detect hex (0x…), octal (0…), or binary (0b…) prefixes — those are
   reserved for explicit hex()/oct() calls only.  We implement our own
   decimal-only parsers to avoid C library auto-detection behavior. */

static __attribute__((pure)) HOTX long long perl_atoll_decimal(const char *s) {
    if (!s) return 0;
    long long result = 0;
    int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return sign * result;
}

static __attribute__((pure)) HOTX double perl_atof_decimal(const char *s) {
    if (!s) return 0.0;
    double result = 0.0;
    int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            result += frac * (*s - '0');
            frac *= 0.1;
            s++;
        }
    }
    /* Skip exponent (rare in Perl string coercion, but handle for completeness) */
    if (*s == 'e' || *s == 'E') {
        s++;
        int exp_sign = 1;
        if (*s == '-') { exp_sign = -1; s++; }
        else if (*s == '+') s++;
        long long exp = 0;
        while (*s >= '0' && *s <= '9') {
            exp = exp * 10 + (*s - '0');
            s++;
        }
        double mul = 1.0;
        for (long long i = 0; i < exp; i++) mul *= (exp_sign > 0) ? 10.0 : 0.1;
        result *= mul;
    }
    return sign * result;
}

__attribute__((pure)) HOTX long long perl_to_int(const PerlValue *v) {
    if (!v) return 0;
    switch (v->tag) {
        case PERL_INT:    return v->ival;
        case PERL_FLOAT:  return (long long)v->fval;
        case PERL_STRING: return perl_atoll_decimal(v->sval);
        case PERL_XS_PTR: return (long long)(uintptr_t)v->pval;
        default:          return 0;
    }
}

__attribute__((pure)) HOTX double perl_to_float(const PerlValue *v) {
    if (!v) return 0.0;
    switch (v->tag) {
        case PERL_INT:    return (double)v->ival;
        case PERL_FLOAT:  return v->fval;
        case PERL_STRING: return perl_atof_decimal(v->sval);
        case PERL_XS_PTR: return (double)(uintptr_t)v->pval;
        default:          return 0.0;
    }
}

/* D31: default float stringification. Real Perl formats with %.15g (not
   C's default %g, which is only 6 significant digits -- `10/3` printed
   "3.33333" instead of "3.33333333333333"), and special-cases the values
   %.15g renders differently from Perl: negative zero prints as plain "0"
   (no sign), +-infinity as "Inf"/"-Inf" (capitalized, not C's "inf"), and
   NaN as "NaN" regardless of its sign bit (glibc's %g can render a NaN
   with a negative sign bit as "-nan", which Perl never does). */
static void perl_format_float(char *buf, size_t bufsz, double d) {
    if (isnan(d)) { snprintf(buf, bufsz, "NaN"); return; }
    if (isinf(d)) { snprintf(buf, bufsz, d < 0 ? "-Inf" : "Inf"); return; }
    if (d == 0.0) { snprintf(buf, bufsz, "0"); return; }
    /* D78: If the float represents an integer value within i64 range,
       format as integer to match Perl's default stringification.
       Perl tracks an "integer-ness" flag when integers overflow to float;
       we approximate this by checking if the value is a whole number
       within i64 range. */
    if (d > -9223372036854775807.0 && d < 9223372036854775807.0) {
        long long iv = (long long)d;
        if ((double)iv == d) {
            snprintf(buf, bufsz, "%lld", iv);
            return;
        }
    }
    snprintf(buf, bufsz, "%.15g", d);
}

/* For PERL_STRING and PERL_UNDEF, returns a stable pointer — caller must
   NOT free the result.  For all other tags (INT, FLOAT, refs, etc.),
   returns a heap-allocated string that the caller must free. */
const char *perl_to_string(const PerlValue *v) {
    static const char empty_str[] = "";
    if (!v || v->tag == PERL_UNDEF) return empty_str;
    if (v->tag == PERL_STRING) return v->sval ? v->sval : empty_str;
    /* Below here: non-string tags — must strdup (caller must free). */
    char buf[64];
    switch (v->tag) {
        case PERL_INT:
            snprintf(buf, sizeof buf, "%lld", v->ival);
            return strdup(buf);
        case PERL_FLOAT:
            perl_format_float(buf, sizeof buf, v->fval);
            return strdup(buf);
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
        case PERL_XS_PTR:
            if (v->blessed_class)
                snprintf(buf, sizeof buf, "%s=PTR(0x%llx)", v->blessed_class, (unsigned long long)(uintptr_t)v->pval);
            else
                snprintf(buf, sizeof buf, "PTR(0x%llx)", (unsigned long long)(uintptr_t)v->pval);
            return strdup(buf);
        case PERL_DBI_DBH:
            return strdup("DBI::db");
        case PERL_DBI_STH:
            return strdup("DBI::st");
        default:
            return strdup("");
    }
}

/* Like perl_to_string but always returns a heap-allocated string
   (caller must free).  Used by callers that already free the result
   of perl_to_string regardless of tag. */
char *perl_to_string_dup(const PerlValue *v) {
    if (!v || v->tag == PERL_UNDEF) return strdup("");
    if (v->tag == PERL_STRING) {
        /* D85: strdup() alone would truncate at the first embedded NUL —
           allocate the true slen+1 bytes and memcpy, so a caller that goes
           on to (incorrectly) strlen() this result is no worse off than
           before D85, while a length-aware caller can use
           perl_to_string_dup_len instead to get the true length too. */
        long long n = v->slen;
        char *r = malloc((size_t)n + 1);
        if (n > 0) memcpy(r, v->sval ? v->sval : "", (size_t)n);
        r[n] = '\0';
        return r;
    }
    /* For non-string tags, re-implement the conversion here to avoid
       calling perl_to_string (which would return a heap-allocated
       string that we'd then strdup again). */
    char buf[64];
    switch (v->tag) {
        case PERL_INT:
            snprintf(buf, sizeof buf, "%lld", v->ival);
            return strdup(buf);
        case PERL_FLOAT:
            perl_format_float(buf, sizeof buf, v->fval);
            return strdup(buf);
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
        case PERL_XS_PTR:
            if (v->blessed_class)
                snprintf(buf, sizeof buf, "%s=PTR(0x%llx)", v->blessed_class, (unsigned long long)(uintptr_t)v->pval);
            else
                snprintf(buf, sizeof buf, "PTR(0x%llx)", (unsigned long long)(uintptr_t)v->pval);
            return strdup(buf);
        case PERL_DBI_DBH:
            return strdup("DBI::db");
        case PERL_DBI_STH:
            return strdup("DBI::st");
        default:
            return strdup("");
    }
}

/* D85: length-aware counterpart to perl_to_string_dup — for PERL_STRING
   values, *out_len is the true byte length (v->slen), correctly reflecting
   any embedded NUL bytes, instead of the implicit strlen() a plain char*
   return forces on every caller. Non-string tags behave identically to
   perl_to_string_dup (their formatted forms never contain embedded NULs),
   with *out_len set via strlen() of the formatted result. */
char *perl_to_string_dup_len(const PerlValue *v, long long *out_len) {
    if (v && v->tag == PERL_STRING) {
        if (out_len) *out_len = v->slen;
        return perl_to_string_dup(v);
    }
    char *r = perl_to_string_dup(v);
    if (out_len) *out_len = (long long)strlen(r);
    return r;
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
            /* D85: gate on slen, not a bare sval[0]=='\0' check — a
               genuine 1-byte string holding just an embedded NUL
               (slen==1, sval[0]=='\0') is neither "" nor "0" and must be
               TRUE in real Perl, but the old check treated any string
               whose first byte was 0x00 as falsy regardless of length. */
            return !(v->slen == 0 ||
                     (v->slen == 1 && v->sval[0] == '0'));
        case PERL_REF_SCALAR:
        case PERL_REF_ARRAY:
        case PERL_REF_HASH:
        case PERL_FLAT_ARRAY:
            return 1;
        case PERL_FILEHANDLE:
        case PERL_XS_PTR:
        case PERL_DBI_DBH:
        case PERL_DBI_STH:
            return v->pval != NULL;
        default: return 0;
    }
}

HOTX void perl_assign(PerlValue *dst, const PerlValue *src) {
    if (!dst) return;
    /* Self-assignment ($x = $x, or die $@ where $@ IS the dst cell since
       perl_get_dollar_at() returns the live global's address, not a copy)
       is always a correct no-op — the value is already right. Below, the
       STRING/FLAT_ARRAY paths free dst's old payload before reading src's,
       and when dst==src that free() invalidates src too (same pointer),
       so the later strdup/memcpy from the just-freed/NULLed src crashes.
       Handling it here once, rather than case-by-case in every tag branch
       below, closes the whole class of ordering bugs at once. */
    if (dst == src) return;
    /* D62: preserve every identity-persistent bit across the reassignment
       below, not just PV_FLAG_SHARED — dst's capture-count/released bits
       belong to the STABLE POINTER's identity (which closures may still
       hold), not to whatever value currently lives in it, and must
       survive any number of perl_assign calls on that same pointer. */
    unsigned int preserved_flags = dst->flags & (PV_FLAG_SHARED | PV_CAPTURE_MASK | PV_FLAG_CAPTURE_RELEASED);
    /* No implicit mutex here — caller must hold lock() for concurrent safety.
       Locking inside perl_assign would deadlock when lock() is already held.
       Visibility for cross-thread write/read is now the responsibility of
       perl_atomic_store (release fence) and perl_atomic_load (acquire
       fence), called from the codegen for shared scalars.  The non-atomic
       perl_assign path used by `local` save/restore and by some inter-
       mediate writes (e.g. array push, hash set) is not visibility-safe
       on its own — those callers must hold the cell's SharedMutex. */
    /* Acquire new reference first (handles self-assignment safely) */
    if (src && src->tag == PERL_REF_ARRAY && src->pval) {
        PerlArray *av = (PerlArray *)src->pval;
        if (av->refcount > 0) av->refcount++;
    } else if (src && src->tag == PERL_REF_HASH && src->pval) {
        PerlHash *hv = (PerlHash *)src->pval;
        if (hv->refcount > 0) hv->refcount++;
    } else if (src && src->tag == PERL_DBI_DBH && src->pval) {
        ((PerlDBIHandle *)src->pval)->refcount++;
    } else if (src && src->tag == PERL_DBI_STH && src->pval) {
        ((PerlDBIStatement *)src->pval)->refcount++;
    } else if (src && src->tag == PERL_CODE_REF && src->pval) {
        /* D62: dst is about to shallow-copy src's pval (*dst = *src below) —
           bump the closure's refcount so it isn't torn down while dst also
           references it. */
        ((PerlClosure *)src->pval)->refcount++;
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
        /* trigger DESTROY when last reference is being released */
        if (hv->refcount == 1 && dst->blessed_class && s_destroy_depth < 100) {
            PerlSubFnCtx fn = perl_find_method(dst->blessed_class, "DESTROY");
            if (fn) {
                PerlValue *self = perl_clone(dst);
                s_destroy_depth++;
                PerlArray *args = perl_array_new();
                perl_array_push(args, self);
                PerlValue *ret = fn(args, perl_push_wantarray(0));
                perl_pop_wantarray();
                perl_free(ret);
                perl_array_free(args);
                perl_free(self);
                s_destroy_depth--;
            }
        }
        if (hv->refcount > 0 && --hv->refcount == 0) perl_hash_free(hv);
    }
    if (dst->tag == PERL_DBI_STH && dst->pval)
        perl_dbi_statement_release((PerlDBIStatement *)dst->pval);
    if (dst->tag == PERL_DBI_DBH && dst->pval)
        perl_dbi_handle_release((PerlDBIHandle *)dst->pval);
    if (dst->tag == PERL_CODE_REF && dst->pval) {
        /* D62: dst is about to be overwritten (*dst = *src below) — release
           dst's old share of whatever closure it was pointing at first. */
        perl_closure_release((PerlClosure *)dst->pval);
    }
    if (dst->blessed_class) { free(dst->blessed_class); dst->blessed_class = NULL; }
    if (!src) { dst->tag = PERL_UNDEF; dst->ival = 0; dst->matchpos = 0; return; }
    *dst = *src;
    dst->flags = preserved_flags;  /* restore identity bits — *dst = *src clobbered them */
    if (src->tag == PERL_STRING) {
        /* D85: strdup(src->sval) would truncate at the first embedded NUL —
           *dst = *src above already copied src->slen correctly (a plain
           struct-field copy), so just deep-copy exactly that many bytes
           instead of stopping at strlen(). */
        long long n = src->slen;
        dst->sval = malloc((size_t)n + 1);
        if (n > 0) memcpy(dst->sval, src->sval, (size_t)n);
        dst->sval[n] = '\0';
        dst->slen = n;
        dst->matchpos = 0;
    } else if (src->tag == PERL_FLAT_ARRAY && src->pval) {
        /* Deep-copy the double[] so src and dst each own their own buffer.
           matchpos is the element count for FLAT_ARRAY — must NOT be zeroed. */
        long long n = src->matchpos;
        double *copy = (double *)malloc((size_t)n * sizeof(double));
        memcpy(copy, (double *)src->pval, (size_t)n * sizeof(double));
        dst->pval = copy;
    } else if (src->tag == PERL_FLOAT_PAIR) {
        /* matchpos holds the imaginary part as double bits — must NOT be zeroed. */
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
    if (both_int(a, b)) {
        long long r = a->ival + b->ival;
        /* D78: Detect signed integer overflow — auto-promote to float.
           Overflow: pos+pos=neg or neg+neg=pos. */
        if ((b->ival > 0 && a->ival > 0 && r < 0) ||
            (b->ival < 0 && a->ival < 0 && r > 0))
            return perl_alloc_float((double)a->ival + (double)b->ival);
        return perl_alloc_int(r);
    }
    return perl_alloc_float(perl_to_float(a) + perl_to_float(b));
}

HOTX PerlValue *perl_sub(const PerlValue *a, const PerlValue *b) {
    if (both_int(a, b)) {
        long long r = a->ival - b->ival;
        /* D78: Detect signed integer overflow — auto-promote to float.
           Overflow: pos-neg=neg or neg-pos=pos. */
        if ((b->ival < 0 && a->ival > 0 && r < 0) ||
            (b->ival > 0 && a->ival < 0 && r > 0))
            return perl_alloc_float((double)a->ival - (double)b->ival);
        return perl_alloc_int(r);
    }
    return perl_alloc_float(perl_to_float(a) - perl_to_float(b));
}

HOTX PerlValue *perl_mul(const PerlValue *a, const PerlValue *b) {
    if (both_int(a, b)) {
        /* D78: Detect signed integer overflow — auto-promote to float.
           Simple check: if either operand is non-zero and the division
           of result by one operand doesn't give back the other, overflow.
           Also check sign patterns. */
        long long r = a->ival * b->ival;
        int overflow = 0;
        if (a->ival != 0 && b->ival != 0) {
            if (r / a->ival != b->ival) overflow = 1;
        } else if (a->ival == 0 || b->ival == 0) {
            /* 0 * anything = 0, no overflow */
        }
        if (overflow)
            return perl_alloc_float((double)a->ival * (double)b->ival);
        return perl_alloc_int(r);
    }
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
     if (bv == 0) {
         PerlValue *msg = perl_alloc_string("Illegal modulus zero");
         perl_die(msg, NULL, 0);
     }
    long long av = perl_to_int(a);
    /* Perl's %, unlike C's, uses floored-division semantics: the result
       always has the same sign as the right operand (or is zero) — e.g.
       -7 % 3 == 2, 7 % -3 == -2. C's '%' truncates toward zero instead,
       so a nonzero result with a sign mismatch against bv needs bv added
       back to floor it. */
    long long r = av % bv;
    if (r != 0 && ((r < 0) != (bv < 0))) r += bv;
    return perl_alloc_int(r);
}

/* D84: i64 fast-path modulo with eval-catchable zero-divisor check.
    Returns the floored modulo result, or calls perl_die if divisor is zero. */
 HOTX long long perl_mod_i64(long long a, long long b) {
     if (b == 0) {
         PerlValue *msg = perl_alloc_string("Illegal modulus zero");
         perl_die(msg, NULL, 0);
     }
    long long r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

PerlValue *perl_pow(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_float(pow(perl_to_float(a), perl_to_float(b)));
}

PerlValue *perl_negate(const PerlValue *a) {
    if (a->tag == PERL_INT)   return perl_alloc_int(-a->ival);
    if (a->tag == PERL_FLOAT) return perl_alloc_float(-a->fval);
    return perl_alloc_float(-perl_to_float(a));
}

/* ── bitwise ops ─────────────────────────────────────────────────────────── */

PerlValue *perl_bitand(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_to_int(a) & perl_to_int(b));
}
PerlValue *perl_bitor(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_to_int(a) | perl_to_int(b));
}
PerlValue *perl_bitxor(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_to_int(a) ^ perl_to_int(b));
}
PerlValue *perl_bitnot(const PerlValue *a) {
    return perl_alloc_int(~perl_to_int(a));
}
PerlValue *perl_lshift(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_to_int(a) << perl_to_int(b));
}
PerlValue *perl_rshift(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int((unsigned long long)perl_to_int(a) >> perl_to_int(b));
}

/* ── string ops ──────────────────────────────────────────────────────────── */

PerlValue *perl_concat(const PerlValue *a, const PerlValue *b) {
    /* D85: NUL-safe — memcpy exact byte lengths instead of strcpy/strcat,
       which both stop at the first embedded NUL. */
    long long la, lb;
    char *sa = perl_to_string_dup_len(a, &la);
    char *sb = perl_to_string_dup_len(b, &lb);
    char *buf = malloc((size_t)(la + lb) + 1);
    if (la > 0) memcpy(buf, sa, (size_t)la);
    if (lb > 0) memcpy(buf + la, sb, (size_t)lb);
    buf[la + lb] = '\0';
    PerlValue *r = perl_alloc_string_len(buf, la + lb);
    free(sa); free(sb); free(buf);
    return r;
}

PerlValue *perl_repeat_str(const PerlValue *str, const PerlValue *n) {
    /* D85: NUL-safe — memcpy per repetition instead of strcat, which
       would stop at the first embedded NUL on every iteration and never
       actually grow the buffer past that point. */
    long long slen_ll;
    char *s = perl_to_string_dup_len(str, &slen_ll);
    long long reps = perl_to_int(n);
    if (reps <= 0) { free(s); return perl_alloc_string(""); }
    size_t slen = (size_t)slen_ll;
    char *buf = malloc(slen * (size_t)reps + 1);
    for (long long i = 0; i < reps; i++)
        if (slen > 0) memcpy(buf + (size_t)i * slen, s, slen);
    buf[slen * (size_t)reps] = '\0';
    PerlValue *r = perl_alloc_string_len(buf, (long long)(slen * (size_t)reps));
    free(s); free(buf);
    return r;
}

PerlArray *perl_repeat_list(PerlArray *src, PerlValue *n_pv) {
    long long reps = perl_to_int(n_pv);
    PerlArray *dst = perl_array_new();
    for (long long i = 0; i < reps; i++)
        perl_array_extend(dst, src);
    return dst;
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

/* D85: NUL-safe tri-state comparison — memcmp up to the shorter length,
   then break ties by length, matching Perl's byte-wise string ordering
   (an embedded NUL is just an ordinary byte value 0, which memcmp already
   orders correctly; strcmp would instead treat it as a false end-of-string). */
static int perl_strcmp_len(const char *a, long long la, const char *b, long long lb) {
    long long n = la < lb ? la : lb;
    int c = (n > 0) ? memcmp(a, b, (size_t)n) : 0;
    if (c != 0) return c;
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

PerlValue *perl_str_eq(const PerlValue *a, const PerlValue *b) {
    long long la, lb;
    char *sa = perl_to_string_dup_len(a, &la), *sb = perl_to_string_dup_len(b, &lb);
    PerlValue *r = perl_alloc_int(perl_strcmp_len(sa, la, sb, lb) == 0);
    free(sa); free(sb); return r;
}
PerlValue *perl_str_ne(const PerlValue *a, const PerlValue *b) {
    long long la, lb;
    char *sa = perl_to_string_dup_len(a, &la), *sb = perl_to_string_dup_len(b, &lb);
    PerlValue *r = perl_alloc_int(perl_strcmp_len(sa, la, sb, lb) != 0);
    free(sa); free(sb); return r;
}
PerlValue *perl_str_lt(const PerlValue *a, const PerlValue *b) {
    long long la, lb;
    char *sa = perl_to_string_dup_len(a, &la), *sb = perl_to_string_dup_len(b, &lb);
    PerlValue *r = perl_alloc_int(perl_strcmp_len(sa, la, sb, lb) < 0);
    free(sa); free(sb); return r;
}
PerlValue *perl_str_gt(const PerlValue *a, const PerlValue *b) {
    long long la, lb;
    char *sa = perl_to_string_dup_len(a, &la), *sb = perl_to_string_dup_len(b, &lb);
    PerlValue *r = perl_alloc_int(perl_strcmp_len(sa, la, sb, lb) > 0);
    free(sa); free(sb); return r;
}
PerlValue *perl_str_le(const PerlValue *a, const PerlValue *b) {
    long long la, lb;
    char *sa = perl_to_string_dup_len(a, &la), *sb = perl_to_string_dup_len(b, &lb);
    PerlValue *r = perl_alloc_int(perl_strcmp_len(sa, la, sb, lb) <= 0);
    free(sa); free(sb); return r;
}
PerlValue *perl_str_ge(const PerlValue *a, const PerlValue *b) {
    long long la, lb;
    char *sa = perl_to_string_dup_len(a, &la), *sb = perl_to_string_dup_len(b, &lb);
    PerlValue *r = perl_alloc_int(perl_strcmp_len(sa, la, sb, lb) >= 0);
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
    /* D85: fwrite the true byte length instead of fputs — fputs stops at
       the first embedded NUL, silently truncating output for e.g. a
       pack()'d value being printed/written out (the direct binary-
       protocol use case that motivated this fix). */
    long long n;
    char *s = perl_to_string_dup_len(v, &n);
    fwrite(s, 1, (size_t)n, stdout);
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
            v->slen = (long long)strlen(buf); /* D85: buf may have grown a char (carry) — refresh slen */
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
    return pa_alloc();
}

PerlArray *perl_anon_array_new(void) {
    PerlArray *a = pa_alloc();
    a->refcount = 1;
    return a;
}

void perl_array_free(PerlArray *a) {
    if (!a) return;
    for (long long i = 0; i < a->len; i++) perl_free(a->elems[i]);
    /* shared arrays have a mutex — clean it up then fall through to pool */
    if (a->mu) { pthread_mutex_destroy(a->mu); free(a->mu); a->mu = NULL; }
    pa_pool_push(a);
}

void perl_array_clear(PerlArray *a) {
    if (!a) return;
    for (long long i = 0; i < a->len; i++) perl_free(a->elems[i]);
    a->len = 0;
}

void perl_array_replace(PerlArray *dst, PerlArray *src) {
    if (!dst) return;
    perl_array_clear(dst);
    if (src) perl_array_extend(dst, src);
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

/* Preserves the original pointer (no clone) for every capture — shared
   vars because they're program-lifetime cells that must stay identical
   across threads, non-shared vars because real Perl closures capture by
   reference (D62): a mutation from either side of the closure boundary
   must be visible to the other. Non-shared captures bump the PV's D62
   capture-refcount so perl_free() defers the real free until every
   capturing closure/sort-comparator (see perl_release_capture) AND the
   declaring scope have both released their share — used exclusively for
   closure/sort-comparator capture arrays. */
void perl_array_push_capture(PerlArray *a, PerlValue *v) {
    if (a->len == a->cap) {
        a->cap *= 2;
        a->elems = realloc(a->elems, a->cap * sizeof(PerlValue *));
    }
    if (v && !(v->flags & PV_FLAG_SHARED)) {
        unsigned n = pv_capture_count(v);
        if (n < PV_CAPTURE_MAX) pv_capture_count_set(v, n + 1);
    }
    a->elems[a->len++] = v;
}

/* Push without cloning: the caller retains ownership of v and guarantees v
   outlives the array. Used for @_ construction in emitCall so we avoid 63M
   clone/free pairs per mbs.pl run. Never call on a persistent array. */
void perl_array_push_nc(PerlArray *a, PerlValue *v) {
    if (a->len >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->elems = realloc(a->elems, a->cap * sizeof(PerlValue *));
    }
    a->elems[a->len++] = v;   /* no perl_clone */
}

/* Free array without freeing elements: companion to perl_array_push_nc.
   Elements are owned by the caller, not this array. */
void perl_array_free_nc(PerlArray *a) {
    if (!a) return;
    /* do NOT call perl_free on elems — caller owns them */
    if (a->mu) { pthread_mutex_destroy(a->mu); free(a->mu); a->mu = NULL; }
    pa_pool_push(a);
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
    if (a->len <= idx) {
        /* Sequential extension: fill gaps with undef, then directly clone v
           into the target slot without allocating+freeing an intermediate undef. */
        while (a->len < idx) a->elems[a->len++] = perl_alloc_undef();
        a->elems[a->len++] = perl_clone(v);
    } else if (a->elems[idx] != v) {
        /* Self-assignment ($a[i] = $a[i], e.g. via a borrowed-read RHS that
           points at the same slot) must be a no-op — freeing the old slot
           before cloning v would read v from memory it just freed, since
           v IS that slot's pointer. Same bug class as perl_assign. */
        perl_free(a->elems[idx]);
        a->elems[idx] = perl_clone(v);
    }
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

 /* Return the last element of an array, or undef for empty arrays — used for
    ternary conditional scalar-context fallback when the true/false branch is
    a list literal (e.g. `wantarray() ? (1,2,3) : "scalar"`). */
 PerlValue *perl_array_last(PerlArray *a) {
     if (!a || a->len == 0) return perl_alloc_undef();
     return perl_clone(a->elems[a->len - 1]);
 }

 /* F64 fast path: return array length as a bare double (no PV boxing). */
double perl_array_len_f64(PerlArray *a) {
    return (double)a->len;
}

void perl_array_extend(PerlArray *dst, PerlArray *src) {
    for (long long i = 0; i < src->len; i++)
        perl_array_push(dst, src->elems[i]);
}

/* Like perl_array_extend, but starting at src[start] — used for the
 * trailing @rest/%rest slurp in list assignment (my ($a,$b,@rest) = LIST). */
void perl_array_extend_from(PerlArray *dst, PerlArray *src, long long start) {
    if (start < 0) start = 0;
    for (long long i = start; i < src->len; i++)
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

/* Unwrap PERL_LIST_RESULT and extend dst array with its elements.
   If pv is not PERL_LIST_RESULT, push pv as a single element. */
void perl_array_push_list_or_scalar(PerlArray *dst, PerlValue *pv) {
    if (!pv) return;
    if (pv->tag == PERL_LIST_RESULT && pv->pval) {
        PerlArray *av = (PerlArray *)pv->pval;
        for (long long i = 0; i < av->len; i++)
            perl_array_push(dst, av->elems[i]);
        perl_free(pv);  /* free the LIST_RESULT wrapper */
    } else {
        perl_array_push(dst, pv);
    }
}

static int cmp_str_pv(const void *a, const void *b) {
    /* D85: NUL-safe default-sort comparator. */
    long long la, lb;
    char *sa = perl_to_string_dup_len(*(PerlValue **)a, &la);
    char *sb = perl_to_string_dup_len(*(PerlValue **)b, &lb);
    int r = perl_strcmp_len(sa, la, sb, lb);
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

PerlValue *perl_chop_array(PerlArray *a) {
    if (!a || a->len == 0) return perl_alloc_string("");
    PerlValue *last_removed = NULL;
    for (long long i = 0; i < a->len; i++) {
        PerlValue *rem = perl_chop(a->elems[i]);
        if (last_removed) perl_free(last_removed);
        last_removed = rem;
    }
    return last_removed ? last_removed : perl_alloc_string("");
}

long long perl_chomp(PerlValue *v) {
    if (!v) return 0;
    if (v->tag == PERL_STRING) {
        /* D85: use v->slen, not strlen() — an embedded NUL earlier in the
           string would otherwise make strlen() find a premature "end" and
           either miss a real trailing newline or corrupt the wrong byte. */
        long long len = v->slen;
        if (len > 0 && v->sval[len - 1] == '\n') {
            v->sval[len - 1] = '\0';
            v->slen = len - 1;
            return 1;
        }
        return 0;
    }
    /* numeric values: convert to string, chomp, store back — stringified
       numbers never contain embedded NULs, so strlen() here is exact. */
    char *s = perl_to_string_dup(v);
    size_t len = strlen(s);
    long long removed = 0;
    if (len > 0 && s[len - 1] == '\n') { s[len - 1] = '\0'; removed = 1; len--; }
    if (v->tag == PERL_STRING) free(v->sval);
    v->tag = PERL_STRING;
    v->sval = s;
    v->slen = (long long)len;
    return removed;
}

PerlValue *perl_chop(PerlValue *v) {
    /* D85: NUL-safe fetch of v's string form + true length. */
    long long len;
    char *s = perl_to_string_dup_len(v, &len);   /* newly heap-allocated */
    PerlValue *removed;
    if (len > 0) {
        removed = perl_alloc_string_len(&s[len - 1], 1); /* the removed byte may itself be NUL */
        s[len - 1] = '\0';
        len--;
    } else {
        removed = perl_alloc_string("");
    }
    if (v->tag == PERL_STRING && v->sval) free(v->sval);
    v->tag  = PERL_STRING;
    v->sval = s;
    v->slen = len;
    return removed;
}

/* UTF-8 helper forward declarations */
static int utf8_encode(unsigned char *buf, long long cp);
static int utf8_decode(const unsigned char *buf, long long *out);
static long long utf8_strlen(const char *s);
static long long utf8_strlen_n(const char *s, long long n); /* D85: bounded, NUL-safe variant */
static long long utf8_char_to_byte(const char *s, long long n);

PerlValue *perl_length(PerlValue *v) {
    if (!v || v->tag == PERL_UNDEF) return perl_alloc_int(0);
    /* D85: NUL-safe — a string with embedded NUL bytes (e.g. pack() output)
       must report its true byte/character count, each embedded NUL
       counting as its own 1-byte character, not stopping length() early. */
    long long slen_ll;
    char *s = perl_to_string_dup_len(v, &slen_ll);
    long long n = (v->tag == PERL_STRING) ? utf8_strlen_n(s, slen_ll) : utf8_strlen(s);
    free(s);
    return perl_alloc_int(n);
}

/* common helper: clamp offset/len to string bounds (UTF-8 code point aware) */
/* D77: real Perl's substr() returns just the in-string overlap of the
   requested (offset,length) window, however that window relates to the
   string — UNLESS the window doesn't overlap the string AT ALL, in which
   case it returns `undef` (with a "substr outside of string" warning/
   die, depending on call form). The previous implementation got both
   halves of this wrong: a negative LENGTH ("stop N chars before the
   end", real Perl's own documented meaning) was treated as "no
   truncation at all" (returning too much), and an offset so far negative
   that it fell entirely before the string was silently clamped to the
   start instead of correctly returning `undef`/dying.
   Algorithm (reverse-engineered empirically against real Perl across a
   961-case (-15..15 offset) x (-15..15 length) matrix on a 10-char
   string, 100% match, plus a separate offset-only sweep for the 2-arg
   form — see the D77 fix's REMEDIATION.md entry for the derivation):
     start = OFFSET; if start<0, start += slen
     if start > slen: NOT in range (offset positioned beyond the string)
     raw_end = slen                      if no LENGTH given (2-arg form)
             = start + LENGTH            if LENGTH >= 0
             = slen + LENGTH             if LENGTH < 0 ("N back from end")
     if start < 0:
         if raw_end < 0: NOT in range (window entirely before the string)
         start = 0                       (partial overlap — clip to 0)
     clamp raw_end to [start, slen]
   Returns 1 with off/n rewritten (via the out params) to the clamped, in-bounds character
   start/length on success; 0 (window doesn't overlap the string at all)
   on "not in range" — callers translate that to `undef` (read forms) or
   a catchable die (the 4-arg replace/lvalue form, matching real Perl).
   has_length=0 is substr's 2-arg form (no LENGTH argument at all — not
   the same as LENGTH==0); *n is ignored on input in that case. */
static int substr_bounds_utf8(long long slen, long long *off, long long *n, int has_length) {
    long long start = *off;
    if (start < 0) start += slen;
    if (start > slen) return 0;
    long long raw_end;
    if (!has_length)      raw_end = slen;
    else if (*n >= 0)     raw_end = start + *n;
    else                  raw_end = slen + *n;
    if (start < 0) {
        if (raw_end < 0) return 0;
        start = 0;
    }
    if (raw_end < start) raw_end = start;
    if (raw_end > slen)  raw_end = slen;
    *off = start;
    *n   = raw_end - start;
    return 1;
}

PerlValue *perl_substr2(PerlValue *str, PerlValue *off_v) {
    char *s  = perl_to_string_dup(str);
    long long slen = utf8_strlen(s);
    long long off  = perl_to_int(off_v);
    long long n    = 0;
    if (!substr_bounds_utf8(slen, &off, &n, 0)) { free(s); return perl_alloc_undef(); }
    long long byte_off = utf8_char_to_byte(s, off);
    const unsigned char *p = (const unsigned char *)(s + byte_off);
    long long bc = 0;
    for (long long c = 0; c < n; c++) {
        if (*p < 0x80) { p++; bc++; }
        else if ((*p & 0xE0) == 0xC0) { p += 2; bc += 2; }
        else if ((*p & 0xF0) == 0xE0) { p += 3; bc += 3; }
        else if ((*p & 0xF8) == 0xF0) { p += 4; bc += 4; }
        else { p++; bc++; }
    }
    char *buf = malloc((size_t)bc + 1);
    memcpy(buf, s + byte_off, (size_t)bc);
    buf[bc] = '\0';
    PerlValue *r = perl_alloc_string_len(buf, bc);
    free(buf); free(s);
    return r;
}

PerlValue *perl_substr3(PerlValue *str, PerlValue *off_v, PerlValue *len_v) {
    char *s  = perl_to_string_dup(str);
    long long slen = utf8_strlen(s);
    long long off  = perl_to_int(off_v);
    long long n    = perl_to_int(len_v);
    if (!substr_bounds_utf8(slen, &off, &n, 1)) { free(s); return perl_alloc_undef(); }
    long long byte_off = utf8_char_to_byte(s, off);
    const unsigned char *p = (const unsigned char *)(s + byte_off);
    long long bc = 0;
    for (long long c = 0; c < n; c++) {
        if (*p < 0x80) { p++; bc++; }
        else if ((*p & 0xE0) == 0xC0) { p += 2; bc += 2; }
        else if ((*p & 0xF0) == 0xE0) { p += 3; bc += 3; }
        else if ((*p & 0xF8) == 0xF0) { p += 4; bc += 4; }
        else { p++; bc++; }
    }
    char *buf = malloc((size_t)bc + 1);
    memcpy(buf, s + byte_off, (size_t)bc);
    buf[bc] = '\0';
    PerlValue *r = perl_alloc_string_len(buf, bc);
    free(buf); free(s);
    return r;
}

void perl_substr_replace(PerlValue *str, PerlValue *off_v, PerlValue *len_v, PerlValue *repl) {
    char *s     = perl_to_string_dup(str);
    char *r     = perl_to_string_dup(repl);
    long long slen = utf8_strlen(s);
    long long off  = perl_to_int(off_v);
    long long n    = perl_to_int(len_v);
    if (!substr_bounds_utf8(slen, &off, &n, 1)) {
        /* D77: matches real Perl — the 4-arg replace/lvalue form dies
           (catchable via eval) on a completely out-of-range offset,
           leaving the target string unmodified, rather than silently
           clamping like the read-only 2/3-arg forms' undef return. */
        free(s); free(r);
        static const char msg[] = "substr outside of string";
        PerlValue diemsg = { .tag = PERL_STRING, .sval = (char *)msg, .slen = (long long)(sizeof(msg) - 1) };
         perl_die(&diemsg, NULL, 0);
        return;
    }
    long long byte_off = utf8_char_to_byte(s, off);
    /* calculate byte length of n code points */
    const unsigned char *p = (const unsigned char *)(s + byte_off);
    long long byte_n = 0;
    for (long long c = 0; c < n; c++) {
        if (*p < 0x80) { p++; byte_n++; }
        else if ((*p & 0xE0) == 0xC0) { p += 2; byte_n += 2; }
        else if ((*p & 0xF0) == 0xE0) { p += 3; byte_n += 3; }
        else if ((*p & 0xF8) == 0xF0) { p += 4; byte_n += 4; }
        else { p++; byte_n++; }
    }
    long long rlen  = (long long)strlen(r);
    long long newlen = (long long)(byte_off + byte_n + rlen + (slen - byte_off - (long long)strlen(s + byte_off) + byte_n));
    /* recalculate: newlen = byte_off + rlen + (original_bytes_after_removal) */
    long long orig_after = (long long)strlen(s) - byte_off - byte_n;
    newlen = byte_off + rlen + orig_after;
    char *buf = malloc((size_t)newlen + 1);
    memcpy(buf, s, (size_t)byte_off);
    memcpy(buf + byte_off, r, (size_t)rlen);
    memcpy(buf + byte_off + rlen, s + byte_off + byte_n, (size_t)orig_after);
    buf[newlen] = '\0';
    if (str->tag == PERL_STRING) free(str->sval);
    str->tag  = PERL_STRING;
    str->sval = buf;
    /* D85: keep slen consistent with the new buffer — note substr's own
       byte-offset math above is still strlen()/utf8-scan based internally
       (not NUL-safe on an embedded-NUL source string), a narrower,
       documented remaining gap; this at least avoids leaving a stale
       slen dangling after the replace. */
    str->slen = newlen;
    free(s); free(r);
}

PerlValue *perl_join(PerlValue *sep, PerlArray *arr) {
    char *ssep = perl_to_string_dup(sep);
    size_t seplen = strlen(ssep);
    /* collect stringified parts */
    char **parts = arr->len ? malloc((size_t)arr->len * sizeof(char *)) : NULL;
    size_t total = 0;
    for (long long i = 0; i < arr->len; i++) {
        parts[i] = perl_to_string_dup(arr->elems[i]);
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
    char *s  = perl_to_string_dup(str);
    char *sp = perl_to_string_dup(sep);
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

void perl_hash_clear(PerlHash *h) {
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
        h->buckets[i] = NULL;
    }
    h->size = 0;
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
    char *ks = perl_to_string_dup(key);
    PerlHashEntry *e = hash_find(h, ks);
    free(ks);
    return e ? perl_clone(e->val) : perl_alloc_undef();
}

/* Borrow-read: returns raw pointer into hash bucket (no clone, no alloc).
 * Valid until the hash is next modified. Never call perl_free on the result. */
HOTX PerlValue *perl_hash_get_sv_ref(PerlHash *h, PerlValue *key) {
    char *ks = perl_to_string_dup(key);
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
    char *ks = perl_to_string_dup(key);
    unsigned int b = hash_str(ks);
    PerlHashEntry *e = hash_find(h, ks);
    if (e) {
        /* Self-assignment ($h{k} = $h{k}) must be a no-op — see the
           identical fix/comment in perl_array_set. */
        if (e->val != val) {
            perl_free(e->val);
            e->val = perl_clone(val);
        }
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
        /* Self-assignment ($h{k} = $h{k}) must be a no-op — see the
           identical fix/comment in perl_array_set. */
        if (e->val != val) {
            perl_free(e->val);
            e->val = perl_clone(val);
        }
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
    char *ks = perl_to_string_dup(key);
    int r = hash_find(h, ks) != NULL;
    free(ks);
    return r;
}

HOTX int perl_hash_exists_str(PerlHash *h, const char *key) {
    return hash_find(h, key) != NULL;
}

/* Return a writable pointer to $h{key}, creating an undef slot if missing.
   Unlike perl_hash_get_str_ref, never returns the read-only sentinel. */
PerlValue *perl_hash_lvalue_str(PerlHash *h, const char *key) {
    PerlHashEntry *e = hash_find(h, key);
    if (e) return e->val;
    PerlValue *v = perl_alloc_undef();
    unsigned int b = hash_str(key);
    PerlHashEntry *ne = malloc(sizeof *ne);
    ne->key = strdup(key); ne->val = v;
    ne->next = h->buckets[b]; h->buckets[b] = ne; h->size++;
    return v;
}

PerlValue *perl_hash_lvalue_sv(PerlHash *h, PerlValue *key) {
    char *ks = perl_to_string_dup(key);
    PerlValue *r = perl_hash_lvalue_str(h, ks);
    free(ks); return r;
}

/* ── autovivification ────────────────────────────────────────────────────── */

/* Ensure $h{key} holds a hash ref; create one if missing/undef.  Return the
   inner PerlHash* so the caller can set a subkey without a second lookup. */
PerlHash *perl_hash_autoviv_hash(PerlHash *h, const char *key) {
    PerlHashEntry *e = hash_find(h, key);
    if (e) {
        if (e->val->tag == PERL_REF_HASH) return (PerlHash *)e->val->pval;
        PerlHash *inner = perl_anon_hash_new();
        PerlValue *ref  = perl_ref_hash(inner);
        perl_free(e->val);
        e->val = ref;
        return inner;
    }
    PerlHash *inner = perl_anon_hash_new();
    PerlValue *ref  = perl_ref_hash(inner);
    unsigned int b  = hash_str(key);
    PerlHashEntry *ne = malloc(sizeof *ne);
    ne->key  = strdup(key); ne->val = ref;
    ne->next = h->buckets[b]; h->buckets[b] = ne; h->size++;
    return inner;
}

PerlHash *perl_hash_autoviv_hash_sv(PerlHash *h, PerlValue *key) {
    char *ks = perl_to_string_dup(key);
    PerlHash *r = perl_hash_autoviv_hash(h, ks);
    free(ks); return r;
}

PerlArray *perl_hash_autoviv_array_sv(PerlHash *h, PerlValue *key) {
    char *ks = perl_to_string_dup(key);
    PerlArray *r = perl_hash_autoviv_array(h, ks);
    free(ks); return r;
}

/* Ensure $h{key} holds an array ref; create one if missing/undef. */
PerlArray *perl_hash_autoviv_array(PerlHash *h, const char *key) {
    PerlHashEntry *e = hash_find(h, key);
    if (e) {
        if (e->val->tag == PERL_REF_ARRAY) return (PerlArray *)e->val->pval;
        /* FLAT_ARRAY: extract inline doubles into a proper array */
        if (e->val->tag == PERL_FLAT_ARRAY && e->val->matchpos > 0) {
            PerlArray *inner = perl_anon_array_new();
            double *dptr = (double *)e->val->pval;
            long long n = e->val->matchpos;
            for (long long i = 0; i < n; i++)
                perl_array_push(inner, perl_alloc_float(dptr[i]));
            perl_free(e->val);
            PerlValue *ref = perl_ref_array(inner);
            e->val = ref;
            return inner;
        }
        /* FLOAT_PAIR: extract two doubles */
        if (e->val->tag == PERL_FLOAT_PAIR) {
            PerlArray *inner = perl_anon_array_new();
            double elem[2]; elem[0] = e->val->fval; elem[1] = (double)(long long)e->val->matchpos;
            perl_array_push(inner, perl_alloc_float(elem[0]));
            perl_array_push(inner, perl_alloc_float(elem[1]));
            perl_free(e->val);
            PerlValue *ref = perl_ref_array(inner);
            e->val = ref;
            return inner;
        }
        PerlArray *inner = perl_anon_array_new();
        PerlValue *ref   = perl_ref_array(inner);
        perl_free(e->val);
        e->val = ref;
        return inner;
    }
    PerlArray *inner = perl_anon_array_new();
    PerlValue *ref   = perl_ref_array(inner);
    unsigned int b   = hash_str(key);
    PerlHashEntry *ne = malloc(sizeof *ne);
    ne->key  = strdup(key); ne->val = ref;
    ne->next = h->buckets[b]; h->buckets[b] = ne; h->size++;
    return inner;
}

/* Ensure $a[idx] holds a hash ref; create one if missing/undef. */
PerlHash *perl_array_autoviv_hash(PerlArray *a, long long idx) {
    long long i = idx < 0 ? idx + a->len : idx;
    if (i >= 0 && i < a->len && a->elems[i]->tag == PERL_REF_HASH)
        return (PerlHash *)a->elems[i]->pval;
    PerlHash *inner = perl_anon_hash_new();
    PerlValue *ref  = perl_ref_hash(inner);
    perl_array_set(a, idx, ref);  /* clones ref → inner->refcount bumped */
    perl_free(ref);               /* drop original ref; inner survives via clone */
    return inner;
}

/* Ensure $a[idx] holds an array ref; create one if missing/undef. */
PerlArray *perl_array_autoviv_array(PerlArray *a, long long idx) {
    long long i = idx < 0 ? idx + a->len : idx;
    if (i >= 0 && i < a->len && a->elems[i]->tag == PERL_REF_ARRAY)
        return (PerlArray *)a->elems[i]->pval;
    PerlArray *inner = perl_anon_array_new();
    PerlValue *ref   = perl_ref_array(inner);
    perl_array_set(a, idx, ref);
    perl_free(ref);
    return inner;
}

/* Lvalue hash-slice assignment: @h{@keys} = @vals  (zip keys→vals) */
void perl_hash_assign_slice(PerlHash *h, PerlArray *keys, PerlArray *vals) {
    long long n = keys->len < vals->len ? keys->len : vals->len;
    for (long long i = 0; i < n; i++) {
        char *key = perl_to_string_dup(keys->elems[i]);
        perl_hash_set_str(h, key, vals->elems[i]);
        free(key);
    }
}

/* Lvalue array-slice assignment: @a[@indices] = @vals  (zip indices→vals) */
void perl_array_assign_slice(PerlArray *a, PerlArray *indices, PerlArray *vals) {
    long long n = indices->len < vals->len ? indices->len : vals->len;
    for (long long i = 0; i < n; i++) {
        long long idx = perl_to_int(indices->elems[i]);
        perl_array_set(a, idx, vals->elems[i]);
    }
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
    char *ks = perl_to_string_dup(key);
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

/* Return array of values for a dynamic list of keys (hash slice) */
PerlArray *perl_hash_slice(PerlHash *h, PerlArray *keys) {
    PerlArray *result = perl_array_new();
    for (long long i = 0; i < keys->len; i++) {
        PerlValue *val = perl_hash_get_sv(h, keys->elems[i]);
        if (val) {
            perl_array_push(result, val);
            perl_free(val);
        } else {
            PerlValue *u = perl_alloc_undef();
            perl_array_push(result, u);
            perl_free(u);
        }
    }
    return result;
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
    long long i;
    for (i = 0; i + 1 < list->len; i += 2)
        perl_hash_set_sv(h, list->elems[i], list->elems[i + 1]);
    /* Odd number of elements: Perl still assigns the trailing unpaired key,
       with an undef value (and a "Odd number of elements" warning at
       runtime, which perlc does not currently emit — no warnings system). */
    if (i < list->len) {
        PerlValue *undef = perl_alloc_undef();
        perl_hash_set_sv(h, list->elems[i], undef);
        perl_free(undef);
    }
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
        /* Lazy conversion: box flat double[] into a proper PerlArray in-place. */
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
    if (ref->tag == PERL_FLOAT_PAIR) {
        /* Lazy conversion: expand inline (re,im) into a 2-element PerlArray. */
        double im; memcpy(&im, &ref->matchpos, sizeof(double));
        PerlArray *av = perl_anon_array_new();
        PerlValue *re_pv = perl_alloc_float(ref->fval);
        PerlValue *im_pv = perl_alloc_float(im);
        perl_array_push(av, re_pv); perl_free(re_pv);
        perl_array_push(av, im_pv); perl_free(im_pv);
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
        case PERL_REF_SCALAR: {
            /* Deref the scalar to get the referent's type.
               ref(\$x) where $x is a plain scalar → "SCALAR"
               ref(\$ary) where $ary is an array ref → "REF"
               ref(\$hsh) where $hsh is a hash ref → "REF"
               ref(\$code) where $code is a code ref → "CODE"
               FLAT_ARRAY / FLOAT_PAIR are inline array refs → "REF" */
            if (ref->pval) {
                PerlValue *inner = (PerlValue *)ref->pval;
                switch (inner->tag) {
                    case PERL_REF_ARRAY:
                    case PERL_REF_HASH:
                    case PERL_CODE_REF:
                    case PERL_REF_SCALAR:
                    case PERL_FLAT_ARRAY:
                    case PERL_FLOAT_PAIR:
                    case PERL_XS_PTR:
                        return perl_alloc_string("REF");
                    default:               return perl_alloc_string("SCALAR");
                }
            }
            return perl_alloc_string("SCALAR");
        }
        case PERL_REF_ARRAY:   return perl_alloc_string("ARRAY");
        case PERL_FLAT_ARRAY:  return perl_alloc_string("ARRAY");
        case PERL_FLOAT_PAIR:  return perl_alloc_string("ARRAY");
        case PERL_REF_HASH:    return perl_alloc_string("HASH");
        case PERL_CODE_REF:    return perl_alloc_string("CODE");
        case PERL_XS_PTR:      return perl_alloc_string("PTR");
        default:               return perl_alloc_string("");
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
    cl->refcount = 1;   /* D62: mirrors PerlArray/PerlHash's anon-creation init */
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
    PerlValue *v = make_code_ref_impl(fp, caps, n);
    /* D62: elements were already copied into cl->captures (and refcounted
       by perl_array_push_capture) — free just the wrapper array/struct. */
    if (captures) perl_array_free_nc(captures);
    return v;
}

/* ── threads::shared ─────────────────────────────────────────────────────── */

/* Phase 2 cell layout: a shared scalar IS a PerlValue* with PV_FLAG_SHARED
   set.  No wrapper struct, no per-cell mutex.  The mutex+condvar is
   allocated lazily on the first lock()/cond_wait() call (see
   get_or_install_mutex above).  This is what THREADS_SHARED_ATOMIC_PLAN.md
   §"Data layout" specifies. */
PerlValue *perl_make_shared_scalar(void) {
    PerlValue *pv = pv_alloc();
    pv->tag   = PERL_UNDEF;
    pv->flags = PV_FLAG_SHARED;
    return pv;
}

void perl_lock_shared(PerlValue *pv) {
    if (!pv || !(pv->flags & PV_FLAG_SHARED)) return;
    SharedMutex *mu = get_or_install_mutex(pv);
    /* Re-entry: if the current thread already holds this mutex, just
        bump the depth.  The pthread_mutex is non-recursive but we make
        it act re-entrant via the per-thread depth counter.  The auto-
        unlock stack tracks depth implicitly via entry count. */
    if (s_held_mutex_ == mu) {
        s_held_mutex_depth_++;
    } else {
        pthread_mutex_lock(&mu->mu);
        s_held_mutex_       = mu;
        s_held_mutex_depth_ = 1;
    }
    if (s_local_depth < LOCAL_STACK_MAX) {
        s_local_stack[s_local_depth].type = LOCAL_LOCK_PV;
        s_local_stack[s_local_depth].ptr  = pv;
        s_local_depth++;
    }
}

void perl_unlock_shared(PerlValue *pv) {
    if (!pv || !(pv->flags & PV_FLAG_SHARED)) return;
    SharedMutex *mu = lookup_shared_mutex(pv);
    if (mu) {
        if (s_held_mutex_ == mu) {
            s_held_mutex_depth_--;
            if (s_held_mutex_depth_ == 0) {
                pthread_mutex_unlock(&mu->mu);
                s_held_mutex_ = NULL;
            }
        } else {
            /* unlock() without a matching lock() — the user is doing
               something exotic; just unlock the mutex anyway.  This
               matches the old single-thread-of-execution semantics. */
            pthread_mutex_unlock(&mu->mu);
        }
    }
    if (s_local_depth > 0) {
        s_local_depth--;
    }
}

/* Conditional wait with timeout for shared variables */
int perl_cond_timedwait(PerlValue *pv, long long timeout_ms) {
    if (!pv || !(pv->flags & PV_FLAG_SHARED)) return -1;
    SharedMutex *mu = get_or_install_mutex(pv);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    ts.tv_sec += timeout_ms / 1000;
    int result = pthread_cond_timedwait(&mu->cond, &mu->mu, &ts);
    return (result == ETIMEDOUT) ? 1 : 0;
}

/* Broadcast to all waiting threads */
void perl_cond_broadcast_shared(PerlValue *pv) {
    if (!pv || !(pv->flags & PV_FLAG_SHARED)) return;
    SharedMutex *mu = get_or_install_mutex(pv);
    pthread_cond_broadcast(&mu->cond);
}

/* ── atomic primitives for threads::shared scalars (Phase 2-4) ──────────── */
/* Visibility on a plain load/store is provided by the fences in
   perl_atomic_load / perl_atomic_store; the codegen calls these
   directly for shared scalars (no separate perl_shared_load primitive).
   Read-modify-write (inc/dec/add) goes through a lock-free CAS loop
   for the int and float payload shapes, and falls back to the
   lazy-installed SharedMutex for everything else (string, ref, array,
   hash, undef, code_ref, filehandle, list_result, float_pair,
   flat_array, thread).  perl_atomic_swap also takes the mutex
   because swapping requires replacing the whole cell.  Per-thread
   re-entry is tracked via `s_held_mutex_*` TLS so the helpers are
   safe to call when the user has already wrapped the same scalar
   in lock().

   Lock-free CAS design (Phase 4): the 16 bytes at the start of
   PerlValue — { tag (4B), flags (4B), ival/fval/sval/pval (8B) } —
   are exposed as a packed struct PerlValueAtomic16.  The CAS covers
   the tag + flags + 8-byte value, which is exactly what an RMW on
   an int or float scalar needs to update atomically.  On x86_64 the
   primitive compiles to lock cmpxchg16b; on aarch64 to ldxp+stxp.
   The address of every PerlValue is 16-byte aligned (slab allocation
   via calloc(128, 32)), so the alignment requirement is met.
   Anything where the 16 bytes don't include the full payload state
   (string, ref, float_pair) or where the 16 bytes don't fit a
   single-store mutation (e.g. ++ on a string needs to allocate a
   fresh string and bump the pointer) falls back to the mutex path. */

typedef struct {
    PerlTag      tag;
    unsigned int flags;
    union {
        long long ival;
        double    fval;
        char     *sval;
        void     *pval;
    } v;
} PerlValueAtomic16;
_Static_assert(sizeof(PerlValueAtomic16) == 16,
               "PerlValueAtomic16 must be exactly 16 bytes for cmpxchg16b / ldxp+stxp");
_Static_assert(offsetof(PerlValue, ival) == 8,
               "ival field must sit at offset 8 inside PerlValue so the "
               "atomic-16 shadow covers {tag, flags, ival}");
/* D85: adding the `slen`/`_pad_reserved` fields must keep sizeof(PerlValue)
   a multiple of 16 — pv_alloc()'s slab (calloc(PV_SLAB, sizeof(PerlValue)))
   packs PerlValues contiguously, and every entry must stay 16-byte aligned
   for the cmpxchg16b/ldxp+stxp atomic path above (shared scalars are
   slab-allocated too, via perl_make_shared_scalar -> pv_alloc). A stride
   that isn't a 16-byte multiple would misalign every other slab entry. */
_Static_assert(sizeof(PerlValue) % 16 == 0,
               "sizeof(PerlValue) must be a multiple of 16 for slab alignment");

/* Acquire-load the {tag, flags, value} 16 bytes from a PerlValue.
   The payload's ival/fval are read by the caller after this returns.
   On x86 this compiles to a plain MOV + compiler barrier; on aarch64
   LLVM emits LDAR (16-byte load-acquire).  Used by perl_atomic_load
   on the int/float path. */
static inline void perl_atomic16_load(PerlValue *pv, PerlValueAtomic16 *out) {
    /* Use a 16-byte aligned local to satisfy __atomic_load's natural
       alignment requirement; the load itself targets the 16-byte
       shadow {tag, flags, ival/fval} inside *pv.  The slab allocator
       guarantees pv is 16-byte aligned (calloc(128, 32) where
       alignof(PerlValue) >= 16 on x86_64), so the CAS/load target
       is naturally aligned. */
    _Alignas(16) PerlValueAtomic16 aligned_out;
    __atomic_load((PerlValueAtomic16 *)pv, &aligned_out, __ATOMIC_ACQUIRE);
    *out = aligned_out;
}

/* Strong CAS on the 16-byte payload shadow.  Returns 1 on success
   (out is unchanged), 0 on failure (out is updated with the observed
   value so the caller can retry).  ACQ_REL ordering so subsequent
   reads in the caller see the value, and the writer's RMW is
   visible to subsequent readers' acquire loads. */
static inline int perl_atomic16_cas(PerlValue *pv,
                                    PerlValueAtomic16 *expected,
                                    const PerlValueAtomic16 *desired) {
    return __atomic_compare_exchange(
        (PerlValueAtomic16 *)pv, expected, (PerlValueAtomic16 *)desired,
        0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Release-store the 16-byte payload shadow.  Used by perl_atomic_store
   on the int/float path. */
static inline void perl_atomic16_store(PerlValue *pv, const PerlValueAtomic16 *src) {
    __atomic_store((PerlValueAtomic16 *)pv, (PerlValueAtomic16 *)src, __ATOMIC_RELEASE);
}

PerlValue *perl_atomic_load(PerlValue *pv) {
    /* Plain load + acquire fence.  On x86 this compiles to a plain MOV +
       compiler barrier; on aarch64 LLVM emits `ldar`.  No mutex taken —
       this is the hot path for cross-thread reads of shared scalars. */
    if (!pv) return NULL;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return pv;
}

PerlValue *perl_atomic_store(PerlValue *pv, PerlValue *v) {
    if (!pv) return NULL;
    /* Run the same payload-update logic as perl_assign (refcount +
       string deep-copy + tag dispatch), then release-fence so the
       contents are visible to other threads' perl_atomic_load. */
    perl_assign(pv, v);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return v;
}

/* Helper: take the SharedMutex with re-entry tracking.  Returns 1 if
   the caller should re-acquire (i.e. not currently held by this
   thread), 0 if it's already held.  Used by the RMW fallback paths. */
static inline int atomic_mutex_acquire(PerlValue *pv) {
    SharedMutex *mu = get_or_install_mutex(pv);
    if (s_held_mutex_ == mu) {
        s_held_mutex_depth_++;
        return 0;  /* already held — caller skips pthread_mutex_lock */
    }
    pthread_mutex_lock(&mu->mu);
    s_held_mutex_       = mu;
    s_held_mutex_depth_ = 1;
    return 1;  /* newly held — caller must pthread_mutex_unlock on the way out */
}
static inline void atomic_mutex_release(int newly_held) {
    if (newly_held) {
        pthread_mutex_unlock(&s_held_mutex_->mu);
        s_held_mutex_ = NULL;
        s_held_mutex_depth_ = 0;
    } else {
        s_held_mutex_depth_--;
    }
}

/* Lock-free integer increment via 16-byte CAS on {tag, flags, ival}.
   Returns 1 on success (RMW done lock-free), 0 on CAS-abort
   (caller should retry).  The CAS is strong (no spurious failure);
   the only retry is for actual contention. */
static inline int try_atomic_inc_int(PerlValue *pv, long long delta) {
    PerlValueAtomic16 cur, next;
    perl_atomic16_load(pv, &cur);
    while (1) {
        if (cur.tag != PERL_INT) return 0;   /* not an int — caller falls back */
        next = cur;
        next.v.ival += delta;
        if (perl_atomic16_cas(pv, &cur, &next)) return 1;
        /* CAS failed — cur was reloaded by gcc, retry */
    }
}

/* Lock-free float increment via 16-byte CAS on {tag, flags, fval}. */
static inline int try_atomic_inc_float(PerlValue *pv, double delta) {
    PerlValueAtomic16 cur, next;
    perl_atomic16_load(pv, &cur);
    while (1) {
        if (cur.tag != PERL_FLOAT) return 0;
        next = cur;
        next.v.fval += delta;
        if (perl_atomic16_cas(pv, &cur, &next)) return 1;
    }
}

/* Lock-free integer/float add (used for `+= N` where N may be a
   different numeric type — caller coerces the delta). */
static inline int try_atomic_add_int (PerlValue *pv, long long di) { return try_atomic_inc_int (pv, di); }
static inline int try_atomic_add_float(PerlValue *pv, double   df) { return try_atomic_inc_float(pv, df); }

PerlValue *perl_atomic_swap(PerlValue *pv, PerlValue *v) {
    /* swap replaces the whole cell, including matchpos and blessed_class
       (16 bytes don't cover them).  Take the SharedMutex. */
    if (!pv) return NULL;
    int newly_held = atomic_mutex_acquire(pv);
    PerlValue *old = pv_alloc();
    *old = *pv;                      /* snapshot current contents (flags included) */
    perl_assign(pv, v);              /* mutate in place */
    atomic_mutex_release(newly_held);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return old;
}

PerlValue *perl_atomic_inc(PerlValue *pv) {
    if (!pv) return NULL;
    /* Lock-free fast path for int/float payloads.  On the rare
       contention case, gcc's __atomic_compare_exchange spins for us
       without falling back to the kernel. */
    if (pv->tag == PERL_INT) {
        if (try_atomic_inc_int(pv, 1)) return pv;
    } else if (pv->tag == PERL_FLOAT) {
        if (try_atomic_inc_float(pv, 1.0)) return pv;
    }
    /* Fallback: non-numeric payload, or contended int/float that we
       abandoned (in practice we never abandon on x86/aarch64, but be
       robust).  Take the mutex. */
    int newly_held = atomic_mutex_acquire(pv);
    if (pv->tag == PERL_INT)   { pv->ival++; }
    else if (pv->tag == PERL_FLOAT) { pv->fval += 1.0; }
    atomic_mutex_release(newly_held);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return pv;
}

PerlValue *perl_atomic_dec(PerlValue *pv) {
    if (!pv) return NULL;
    if (pv->tag == PERL_INT) {
        if (try_atomic_inc_int(pv, -1)) return pv;
    } else if (pv->tag == PERL_FLOAT) {
        if (try_atomic_inc_float(pv, -1.0)) return pv;
    }
    int newly_held = atomic_mutex_acquire(pv);
    if (pv->tag == PERL_INT)   { pv->ival--; }
    else if (pv->tag == PERL_FLOAT) { pv->fval -= 1.0; }
    atomic_mutex_release(newly_held);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return pv;
}

PerlValue *perl_atomic_add(PerlValue *pv, PerlValue *delta) {
    if (!pv || !delta) return pv;
    /* Coerce the delta to a numeric PV (mirror perl_add's behaviour) */
    long long di = perl_to_int(delta);
    double   df = perl_to_float(delta);
    /* Lock-free fast path.  try_atomic_* returns 0 if the tag isn't
       INT or FLOAT respectively, in which case we fall through. */
    if (pv->tag == PERL_INT) {
        if (try_atomic_add_int(pv, di)) return pv;
    } else if (pv->tag == PERL_FLOAT) {
        if (try_atomic_add_float(pv, df)) return pv;
    }
    /* Ensure mutex is installed before acquiring — lazy installation
       means it may not exist yet if no lock()/cond_wait() has been called. */
    get_or_install_mutex(pv);
    int newly_held = atomic_mutex_acquire(pv);
    if (pv->tag == PERL_INT)   { pv->ival += di; }
    else if (pv->tag == PERL_FLOAT) { pv->fval += df; }
    atomic_mutex_release(newly_held);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return pv;
}

/* Atomic RMW for *, /, % on shared scalars.  Uses mutex to ensure the
   read-modify-write is atomic.  op: 1=*, 2=/, 3=% */
PerlValue *perl_atomic_rmw(PerlValue *pv, PerlValue *rhs, int op) {
    if (!pv || !rhs) return pv;
    get_or_install_mutex(pv);
    int newly_held = atomic_mutex_acquire(pv);
    /* Read current value */
    double df = perl_to_float(pv);
    double rhs_f = perl_to_float(rhs);
    double result;
    if (op == 1) result = df * rhs_f;
    else if (op == 2) result = rhs_f != 0.0 ? df / rhs_f : df;
    else result = rhs_f != 0.0 ? fmod(df, rhs_f) : df;
    /* Store result */
    if (pv->tag == PERL_INT) {
        pv->ival = (long long)result;
    } else {
        pv->tag = PERL_FLOAT;
        pv->fval = result;
    }
    atomic_mutex_release(newly_held);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return pv;
}

void perl_cond_wait(PerlValue *pv) {
    if (!pv || !(pv->flags & PV_FLAG_SHARED)) return;
    SharedMutex *mu = get_or_install_mutex(pv);
    pthread_cond_wait(&mu->cond, &mu->mu);
}

void perl_cond_signal(PerlValue *pv) {
    if (!pv || !(pv->flags & PV_FLAG_SHARED)) return;
    SharedMutex *mu = lookup_shared_mutex(pv);
    if (mu) pthread_cond_signal(&mu->cond);
}

void perl_cond_broadcast(PerlValue *pv) {
    if (!pv || !(pv->flags & PV_FLAG_SHARED)) return;
    SharedMutex *mu = lookup_shared_mutex(pv);
    if (mu) pthread_cond_broadcast(&mu->cond);
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
    new_cl->refcount  = 1;   /* D62: independent closure, own lifetime */
    new_cl->captures  = old_cl->ncaptures > 0
        ? malloc(old_cl->ncaptures * sizeof(PerlValue *)) : NULL;
    for (int i = 0; i < old_cl->ncaptures; i++) {
        PerlValue *os = old_cl->captures[i];   /* parent's stable slot */
        /* threads::shared variables: pass the original slot so both threads
           see the same cell.  Under the Phase-2 layout the shared flag is
           on the cell itself, so the cell pointer is the canonical shared
           pointer and we must NOT deep-copy it.  The actual concurrent
           writes go through perl_atomic_* helpers, which take the
           SharedMutex (lazy-installed) for the cell. */
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
            /* D85: length-aware copy — *ns = *os above already copied
               os->slen correctly; strdup alone would truncate at an
               embedded NUL. */
            long long n = os->slen;
            ns->sval = malloc((size_t)n + 1);
            if (n > 0) memcpy(ns->sval, os->sval, (size_t)n);
            ns->sval[n] = '\0';
            ns->slen = n;
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
    /* Use caller's wantarray context (set by codegen via perl_push_wantarray) */
    int ctx = perl_current_wantarray_ctx();
    PerlValue *result = ((PerlSubFnCtx)cl->fn)(args, perl_push_wantarray(ctx));
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
    char *cls = perl_to_string_dup(class_pv);
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
    /* D75: this used to "update if already registered" — overwriting any
       existing entry for `child` in place instead of adding a new one.
       `our @ISA = ('B','C')` calls this once per @ISA element in order
       (codegen.cpp), so for a multiple-inheritance class the SECOND call
       (parent="C") silently overwrote the first (parent="B"), and only
       "C" was ever remembered — a class with 2+ parents could only ever
       see the last one. Real Perl's default (non-C3) MRO is a depth-first
       search over @ISA in left-to-right order, which requires remembering
       ALL parents, not just one. Fixed by always appending a new
       (child,parent) entry — perl_isa_direct_parents/perl_find_method/
       perl_isa_check/perl_dispatch_method_super below now do the actual
       DFS over every registered parent instead of assuming exactly one. */
    if (s_isa_count < ISA_TABLE_MAX) {
        s_isa_table[s_isa_count].child  = strdup(child);
        s_isa_table[s_isa_count].parent = strdup(parent);
        s_isa_count++;
    }
}

/* D75: collect ALL direct parents of `class_name`, in @ISA registration
   (i.e. left-to-right @ISA list) order, into `out` (capped at `max`).
   Returns the count found. Multiple entries for the same child are now
   possible (see perl_set_isa above) — this walks the whole table instead
   of returning only the first match. */
#define ISA_MAX_DIRECT_PARENTS 16
static int perl_isa_direct_parents(const char *class_name, const char **out, int max) {
    int n = 0;
    for (int i = 0; i < s_isa_count && n < max; i++) {
        if (strcmp(s_isa_table[i].child, class_name) == 0)
            out[n++] = s_isa_table[i].parent;
    }
    return n;
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

PerlValue *perl_call_named_sub(const char *name, PerlArray *args, int ctx) {
    if (!name) return perl_alloc_undef();
    for (int i = 0; i < s_method_count; i++) {
        if (strcmp(s_method_table[i].key, name) == 0) {
            PerlSubFnCtx fn = s_method_table[i].fn;
            PerlValue *result;
            if (!fn) return perl_alloc_undef();
            result = fn(args, perl_push_wantarray(ctx));
            perl_pop_wantarray();
            return result;
        }
    }
    return perl_alloc_undef();
}

/* ── global (package) scalar registry (D58) ──────────────────────────────────
   A `--do-lib`-compiled file's file-scope `our`/`my` scalars are looked up
   here (keyed by "Package::name") instead of getting an ordinary per-
   compilation-unit GlobalVariable — each separate `do` call compiles and
   dlopen()s an independent shared library, so a plain GlobalVariable would
   give every call its own disconnected storage. Routing through this
   process-wide table instead means a package scalar's storage is the same
   stable PerlValue* across repeated `do` calls on the same file, matching
   real Perl's package-variable semantics (same table used regardless of
   which particular do-lib compile references the name). Same intentionally
   simple, unlocked design as the method table above — do-lib loading isn't
   otherwise thread-safety-hardened either, and locking only this table
   wouldn't fix that; kept consistent rather than over-engineered. */
typedef struct { char *key; PerlValue *pv; } GlobalScalarEntry;
#define GLOBAL_SCALAR_TABLE_MAX 1024
static GlobalScalarEntry s_global_scalar_table[GLOBAL_SCALAR_TABLE_MAX];
static int s_global_scalar_count = 0;

PerlValue *perl_get_or_create_global_scalar(const char *key) {
    if (!key) return perl_alloc_undef();
    for (int i = 0; i < s_global_scalar_count; i++) {
        if (strcmp(s_global_scalar_table[i].key, key) == 0)
            return s_global_scalar_table[i].pv;
    }
    PerlValue *pv = perl_alloc_undef();
    if (s_global_scalar_count < GLOBAL_SCALAR_TABLE_MAX) {
        s_global_scalar_table[s_global_scalar_count].key = strdup(key);
        s_global_scalar_table[s_global_scalar_count].pv  = pv;
        s_global_scalar_count++;
    }
    return pv;
}

/* D75: depth-first search over `class_name` and its full @ISA tree,
   matching real Perl's default (non-C3) method resolution order — check
   the class itself, then recursively search each of its direct @ISA
   parents' entire subtrees in left-to-right order (the first parent's
   whole ancestry is exhausted before the second parent is even tried),
   not just a single linear chain. `depth` guards against a pathological
   @ISA cycle. */
static PerlSubFnCtx perl_find_method_dfs(const char *class_name, const char *method, int depth) {
    if (!class_name || depth > 32) return NULL;
    char key[512];
    snprintf(key, sizeof key, "%s::%s", class_name, method);
    for (int i = 0; i < s_method_count; i++)
        if (strcmp(s_method_table[i].key, key) == 0)
            return s_method_table[i].fn;
    const char *parents[ISA_MAX_DIRECT_PARENTS];
    int np = perl_isa_direct_parents(class_name, parents, ISA_MAX_DIRECT_PARENTS);
    for (int i = 0; i < np; i++) {
        PerlSubFnCtx fn = perl_find_method_dfs(parents[i], method, depth + 1);
        if (fn) return fn;
    }
    return NULL;
}

/* walk class and its full @ISA tree (D75: multiple inheritance, DFS);
   returns NULL if not found */
static PerlSubFnCtx perl_find_method(const char *class_name, const char *method) {
    return perl_find_method_dfs(class_name, method, 0);
}

static PerlArray *build_dispatch_args(PerlValue *obj, PerlArray *args) {
    PerlArray *full = perl_array_new();
    perl_array_push(full, obj);
    if (args)
        for (long long i = 0; i < args->len; i++)
            perl_array_push(full, args->elems[i]);
    return full;
}

/* ── $AUTOLOAD global ───────────────────────────────────────────────────── */
static PerlValue s_autoload_pv = { .tag = PERL_UNDEF };

PerlValue *perl_get_autoload_name(void) { return &s_autoload_pv; }

PerlValue *perl_dispatch_method(PerlValue *obj, const char *method, PerlArray *args) {
    if (obj && obj->tag == PERL_STRING && obj->sval && strcmp(obj->sval, "DBI") == 0) {
        if (strcmp(method, "connect") == 0) {
            PerlValue *dsn  = (args && args->len > 0) ? args->elems[0] : perl_alloc_undef();
            PerlValue *user = (args && args->len > 1) ? args->elems[1] : perl_alloc_undef();
            PerlValue *pass = (args && args->len > 2) ? args->elems[2] : perl_alloc_undef();
            return perl_dbi_connect(dsn, user, pass);
        }
    }
    if (obj && obj->tag == PERL_DBI_DBH) {
        if (strcmp(method, "prepare") == 0)
            return perl_dbi_prepare(obj, (args && args->len > 0) ? args->elems[0] : perl_alloc_undef());
        if (strcmp(method, "disconnect") == 0)
            return perl_dbi_disconnect(obj);
        if (strcmp(method, "commit") == 0)
            return perl_dbi_commit(obj);
        if (strcmp(method, "rollback") == 0)
            return perl_dbi_rollback(obj);
        if (strcmp(method, "errstr") == 0)
            return perl_dbi_error(obj);
        if (strcmp(method, "do") == 0) {
            PerlValue *sth = perl_dbi_prepare(obj, (args && args->len > 0) ? args->elems[0] : perl_alloc_undef());
            PerlValue *ret;
            if (!sth || sth->tag == PERL_UNDEF) return perl_alloc_undef();
            ret = perl_dbi_execute(sth, NULL);
            perl_free(sth);
            return ret;
        }
    }
    if (obj && obj->tag == PERL_DBI_STH) {
        if (strcmp(method, "execute") == 0)
            return perl_dbi_execute(obj, args);
        if (strcmp(method, "fetchrow_arrayref") == 0)
            return perl_dbi_fetch(obj);
        if (strcmp(method, "fetchall_arrayref") == 0)
            return perl_dbi_fetchall(obj);
        if (strcmp(method, "rows") == 0)
            return perl_dbi_rows(obj);
        if (strcmp(method, "finish") == 0) {
            PerlDBIStatement *sth = perl_dbi_get_sth(obj);
            if (sth && sth->stmt) sqlite3_reset((sqlite3_stmt *)sth->stmt);
            return perl_alloc_int(1);
        }
        if (strcmp(method, "errstr") == 0) {
            PerlDBIStatement *sth = perl_dbi_get_sth(obj);
            return perl_alloc_string((sth && sth->last_error) ? sth->last_error : "");
        }
        if (strcmp(method, "fetchrow_array") == 0) {
            PerlValue *rowref = perl_dbi_fetch(obj);
            if (!rowref || rowref->tag == PERL_UNDEF) return perl_alloc_undef();
            if (rowref->tag == PERL_REF_ARRAY && rowref->pval) {
                PerlValue *ret = perl_array_to_list_return((PerlArray *)rowref->pval);
                rowref->pval = NULL;
                perl_free(rowref);
                return ret;
            }
            return rowref;
        }
    }
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
        /* Try AUTOLOAD in the package's ISA chain */
        fn = perl_find_method(class_name, "AUTOLOAD");
        if (fn) {
            /* Set $AUTOLOAD = "Package::method" */
            char autoload_name[512];
            snprintf(autoload_name, sizeof(autoload_name), "%s::%s", class_name, method);
            if (s_autoload_pv.tag == PERL_STRING && s_autoload_pv.sval)
                free(s_autoload_pv.sval);
            s_autoload_pv.tag  = PERL_STRING;
            s_autoload_pv.sval = strdup(autoload_name);
            s_autoload_pv.slen = (long long)strlen(autoload_name);
        } else {
            fprintf(stderr, "Can't locate object method \"%s\" via package \"%s\"\n",
                    method, class_name);
            exit(1);
        }
    }
    PerlValue *result = fn(build_dispatch_args(obj, args), perl_push_wantarray(0));
    perl_pop_wantarray();
    return result;
}

PerlValue *perl_dispatch_method_super(PerlValue *obj, const char *caller_pkg,
                                      const char *method, PerlArray *args) {
    /* D75: SUPER::method searches caller_pkg's @ISA parents in order,
       each one's full subtree via DFS (perl_find_method already does
       this per-parent) — previously this only ever looked at a single
       parent (whichever perl_get_parent's single-entry lookup returned),
       so SUPER:: from a multiply-inherited class could silently miss a
       method that only the second-or-later @ISA parent actually defines. */
    const char *parents[ISA_MAX_DIRECT_PARENTS];
    int np = perl_isa_direct_parents(caller_pkg, parents, ISA_MAX_DIRECT_PARENTS);
    if (np == 0) {
        fprintf(stderr, "Can't call SUPER::%s — no parent for package \"%s\"\n",
                method, caller_pkg);
        exit(1);
    }
    PerlSubFnCtx fn = NULL;
    for (int i = 0; i < np && !fn; i++)
        fn = perl_find_method(parents[i], method);
    if (!fn) {
        fprintf(stderr, "Can't locate SUPER method \"%s\" starting from \"%s\"\n",
                method, caller_pkg);
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
    char *ms = perl_to_string_dup(mode_pv);
    char *fs = perl_to_string_dup(filename_pv);
    if (target->tag == PERL_FILEHANDLE && target->pval) fclose((FILE*)target->pval);
    FILE *fp = fopen(fs, mode_to_cmode(ms));
    free(ms); free(fs);
    if (fp) { target->tag = PERL_FILEHANDLE; target->pval = fp; }
    else    { target->tag = PERL_UNDEF;      target->pval = NULL; }
    target->matchpos = 0;
    return target;
}

PerlValue *perl_open2_fh(PerlValue *target, PerlValue *mode_file_pv) {
    char *s = perl_to_string_dup(mode_file_pv);
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
        s_dollar_dot.ival = 0;  /* D32: real Perl resets $. on close() */
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
        /* D85: slurp mode reads raw bytes via fgetc — perl_alloc_string(buf)
           would truncate at an embedded NUL, silently losing everything a
           binary file wrote past its first NUL byte. `len` is already the
           exact tracked byte count. */
        PerlValue *pv = perl_alloc_string_len(buf, (long long)len);
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
    /* D85: same fix as slurp mode above — a binary read up to the line
       separator may itself contain embedded NUL bytes before that
       separator; `len` is already the exact tracked byte count. */
    PerlValue *pv = perl_alloc_string_len(buf, (long long)len);
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
    /* D85: fwrite the true byte length — see perl_print's comment. This
       is the write half of the pack -> file -> unpack binary round trip. */
    long long n;
    char *s = perl_to_string_dup_len(v, &n);
    fwrite(s, 1, (size_t)n, (FILE*)fh->pval);
    free(s);
}

void perl_say_fh(PerlValue *fh, PerlValue *v) {
    if (!fh || fh->tag != PERL_FILEHANDLE || !fh->pval) return;
    long long n;
    char *s = perl_to_string_dup_len(v, &n);
    FILE *fp = (FILE*)fh->pval;
    fwrite(s, 1, (size_t)n, fp);
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

/* Append " at FILE line N." to a die message if it doesn't already end in \n.
   Returns a newly-allocated string that the caller must free. */
static char *appendDieLocation(char *msg, const char *filename, int line) {
     size_t n = strlen(msg);
     if (n > 0 && msg[n-1] == '\n') {
         return strdup(msg);
     }
     if (filename && line > 0) {
         char *buf = malloc(n + 48 + strlen(filename));
         sprintf(buf, "%s at \"%s\" line %d.\n", msg, filename, line);
         return buf;
     }
     char *buf = malloc(n + 20);
     sprintf(buf, "%s at line %d.\n", msg, line);
     return buf;
 }

void perl_die(PerlValue *msg, const char *filename, int line) {
     if (s_eval_depth > 0) {
         char *s = msg ? perl_to_string_dup(msg) : strdup("Died");
         char *full = appendDieLocation(s, filename, line);
         PerlValue pv = { .tag = PERL_STRING, .sval = full, .slen = (long long)strlen(full) };
         perl_assign(&s_dollar_at, &pv);
         free(s);
         perl_local_restore_to(s_eval_local_depth[s_eval_depth - 1]);
         longjmp(*s_eval_stack[s_eval_depth - 1], 1);
     }
     char *s = msg ? perl_to_string_dup(msg) : strdup("Died");
     char *full = appendDieLocation(s, filename, line);
     fputs(full, stderr);
     free(full);
     free(s);
     exit(1);
 }

PerlValue *perl_unlink_files(PerlArray *files) {
    long long removed = 0;
    for (long long i = 0; i < files->len; i++) {
        char *name = perl_to_string_dup(files->elems[i]);
        if (unlink(name) == 0) removed++;
        free(name);
    }
    return perl_alloc_int(removed);
}

/* ── filesystem ops ──────────────────────────────────────────────────────── */

PerlValue *perl_chdir(PerlValue *path) {
    char *p = perl_to_string_dup(path);
    int r = chdir(p); free(p);
    return perl_alloc_int(r == 0 ? 1 : 0);
}

PerlValue *perl_mkdir_op(PerlValue *path, PerlValue *mode) {
    char *p = perl_to_string_dup(path);
    mode_t m = (mode && mode->tag != PERL_UNDEF) ? (mode_t)perl_to_int(mode) : 0777;
    int r = mkdir(p, m); free(p);
    return perl_alloc_int(r == 0 ? 1 : 0);
}

PerlValue *perl_rmdir_op(PerlValue *path) {
    char *p = perl_to_string_dup(path);
    int r = rmdir(p); free(p);
    return perl_alloc_int(r == 0 ? 1 : 0);
}

PerlValue *perl_rename_op(PerlValue *oldp, PerlValue *newp) {
    char *o = perl_to_string_dup(oldp);
    char *n = perl_to_string_dup(newp);
    int r = rename(o, n); free(o); free(n);
    return perl_alloc_int(r == 0 ? 1 : 0);
}

PerlValue *perl_chmod_op(PerlValue *mode, PerlArray *files) {
    mode_t m = (mode_t)perl_to_int(mode);
    long long changed = 0;
    for (long long i = 0; i < files->len; i++) {
        char *p = perl_to_string_dup(files->elems[i]);
        if (chmod(p, m) == 0) changed++;
        free(p);
    }
    return perl_alloc_int(changed);
}

/* ── directory I/O ───────────────────────────────────────────────────────── */

PerlValue *perl_opendir_fh(PerlValue *target, PerlValue *path) {
    char *p = perl_to_string_dup(path);
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
    char *fmt = perl_to_string_dup(fmt_pv);

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
            char *s = perl_to_string_dup(arg);
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
    /* D85: preserve the tracked byte length, not strlen(out) — note a %s
       argument that itself contains an embedded NUL still truncates at
       the inner snprintf(..., "%s", ...) call above (a libc limitation,
       documented remaining gap); this fixes the outer buffer wrapping
       for every other case (numeric conversions, literal % text, etc). */
    PerlValue *result = perl_alloc_string_len(out, (long long)pos);
    free(out);
    return result;
}

void perl_printf(PerlValue *fmt, PerlArray *args) {
    PerlValue *s = perl_sprintf(fmt, args);
    perl_print(s);
    perl_free(s);
}

/* ── pack / unpack ───────────────────────────────────────────────────────── */

/* Helper: read a signed integer from buffer in given endianness */
static long long read_int_from_buf(const unsigned char *buf, int size, int little_endian) {
    long long val = 0;
    if (little_endian) {
        for (int i = 0; i < size; i++) val |= ((long long)buf[i]) << (i * 8);
    } else {
        for (int i = 0; i < size; i++) val |= ((long long)buf[i]) << ((size - 1 - i) * 8);
    }
    return val;
}

/* Helper: write a signed integer to buffer in given endianness */
static void write_int_to_buf(unsigned char *buf, long long val, int size, int little_endian) {
    if (little_endian) {
        for (int i = 0; i < size; i++) { buf[i] = (unsigned char)(val & 0xFF); val >>= 8; }
    } else {
        for (int i = size - 1; i >= 0; i--) { buf[i] = (unsigned char)(val & 0xFF); val >>= 8; }
    }
}

PerlValue *perl_pack(PerlValue *fmt_pv, PerlArray *args) {
    char *fmt = perl_to_string_dup(fmt_pv);
    size_t cap = 256, pos = 0;
    char *out = malloc(cap);
    if (!out) { free(fmt); return perl_alloc_undef(); }

#define ENSURE(n) do { while (pos+(n)+1 > cap) { cap*=2; out=realloc(out,cap); } } while(0)
#define PUT(p,n) do { ENSURE(n); memcpy(out+pos,(p),(n)); pos+=(n); } while(0)

    long long argidx = 0;
    for (const char *p = fmt; *p; ) {
        if (*p == '\\') { p++; if (!*p) break; unsigned char c = (unsigned char)*p; PUT(&c, 1); p++; continue; }

        /* read type char first */
        char type = *p++;
        if (!type) break;

        /* read count multiplier (comes after type in Perl format): a plain
           digit string, or '*' meaning "all remaining args" (numeric types)
           or "the exact length of the one string arg" (a/A/h/H/b/B) — D67's
           original wiring omitted '*' entirely, silently treating it as a
           default count of 1 (only the first value/byte). */
        long long count = 1;
        int starCount = 0;
        if (*p == '*') { starCount = 1; p++; }
        else if (*p && *p >= '0' && *p <= '9') {
            count = 0;
            while (*p && *p >= '0' && *p <= '9') { count = count * 10 + (*p - '0'); p++; }
        }
        /* p already points past the digits/'*' or at the next type char */
        if (starCount && type != 'a' && type != 'A' && type != 'h' && type != 'H' &&
            type != 'b' && type != 'B') {
            /* '*' for a numeric type means "consume all remaining args"
               (the a/A/h/H/b/B string types resolve '*' themselves, against
               their one arg's own length, inside their own case below).
               x/X/@ don't consume args at all; '*' there is a much rarer,
               less-defined edge case in real Perl too — left as a no-op
               (count 0) rather than guessed at. */
            count = (type == 'x' || type == 'X' || type == '@') ? 0 : (args->len - argidx);
            if (count < 0) count = 0;
        }

        int little_endian = 0;
        int signed_flag = 0;
        int size_bytes = 0;

        switch (type) {
        case 'a': case 'A': case 'h': case 'H': case 'b': case 'B': {
            /* D67: the count for a string type is a FIELD WIDTH applied to
               a single arg (pack("A5","hi") -> "hi   ", one arg) — NOT a
               repeat count over that many separate args (that's only how
               the numeric types below use their count). '*' means "the
               field width is exactly this one arg's own length". 'A' pads
               with spaces (and its unpack counterpart strips them back
               off); 'a' pads with NUL bytes; h/H/b/B (hex/bit strings)
               still just copy raw bytes rather than actually doing nibble/bit-level
               encoding — a narrower, separate, not-fixed-here gap (see
               TESTS.md D85). */
            PerlValue *arg = (argidx < args->len) ? args->elems[argidx++] : perl_alloc_undef();
            /* D85: NUL-safe — the arg being packed may itself already be
               NUL-containing binary data (e.g. re-packing a value that
               came from an earlier unpack()). */
            long long slen_ll;
            char *s = perl_to_string_dup_len(arg, &slen_ll);
            size_t slen = (size_t)slen_ll;
            size_t fieldWidth = starCount ? slen : (size_t)count;
            ENSURE(fieldWidth);
            size_t copyLen = slen < fieldWidth ? slen : fieldWidth;
            memcpy(out + pos, s, copyLen);
            if (fieldWidth > copyLen) {
                char padChar = (type == 'A') ? ' ' : '\0';
                memset(out + pos + copyLen, padChar, fieldWidth - copyLen);
            }
            pos += fieldWidth;
            free(s);
            continue;
        }
        case 'x': /* skip byte */
            pos += count;
            ENSURE(0); /* ensure buffer big enough */
            for (size_t z = pos; z < pos; z++) out[z] = '\0';
            continue;
        case 'X': /* back up one byte */
            if (pos > 0) pos -= count;
            continue;
        case '@': /* null to absolute position */
            if (count > cap) { cap = count; out = realloc(out, cap); }
            for (size_t z = pos; z < count; z++) out[z] = '\0';
            pos = count;
            continue;
        case 'c': case 'C':
            size_bytes = 1; signed_flag = (type == 'c');
            break;
        case 's': case 'S':
            size_bytes = 2; signed_flag = (type == 's');
            if (type == 'S') little_endian = 1;
            break;
        case 'n': case 'N':
            size_bytes = 2; signed_flag = 0;
            little_endian = 0; /* big-endian */
            if (type == 'N') { size_bytes = 4; little_endian = 0; }
            if (type == 'n') { size_bytes = 2; little_endian = 0; }
            break;
        case 'v': case 'V':
            size_bytes = 2; signed_flag = 0;
            little_endian = 1; /* little-endian */
            if (type == 'V') { size_bytes = 4; little_endian = 1; }
            if (type == 'v') { size_bytes = 2; little_endian = 1; }
            break;
        case 'l': case 'L':
            size_bytes = 4; signed_flag = (type == 'l');
            if (type == 'L') little_endian = 1;
            break;
        case 'i': case 'I':
            size_bytes = (int)sizeof(int); signed_flag = (type == 'i');
            if (type == 'I') little_endian = 1;
            break;
        case 'q': case 'Q':
            size_bytes = 8; signed_flag = (type == 'q');
            if (type == 'Q') little_endian = 1;
            break;
        case 'f':
            size_bytes = 4;
            for (long long c = 0; c < count; c++) {
                PerlValue *arg = (argidx < args->len) ? args->elems[argidx++] : perl_alloc_undef();
                float fv = (float)perl_to_float(arg);
                ENSURE(4);
                unsigned char *bp = (unsigned char *)(out + pos);
                memcpy(bp, &fv, 4);
                pos += 4;
            }
            continue;
        case 'd':
            size_bytes = 8;
            for (long long c = 0; c < count; c++) {
                PerlValue *arg = (argidx < args->len) ? args->elems[argidx++] : perl_alloc_undef();
                double dv = perl_to_float(arg);
                ENSURE(8);
                unsigned char *bp = (unsigned char *)(out + pos);
                memcpy(bp, &dv, 8);
                pos += 8;
            }
            continue;
        default:
            /* unknown type — skip */
            continue;
        }

        /* integer packing */
        for (long long c = 0; c < count; c++) {
            PerlValue *arg = (argidx < args->len) ? args->elems[argidx++] : perl_alloc_undef();
            long long val = perl_to_int(arg);
            ENSURE(size_bytes);
            unsigned char *bp = (unsigned char *)(out + pos);
            if (signed_flag) {
                write_int_to_buf(bp, val, size_bytes, little_endian);
            } else {
                write_int_to_buf(bp, (unsigned long long)val, size_bytes, little_endian);
            }
            pos += size_bytes;
        }
    }

    out[pos] = '\0';
#undef ENSURE
#undef PUT

    free(fmt);
    /* D85: this is the exact original repro — pack("N", 1234567) (and any
       other pack producing a leading/embedded NUL byte, extremely common
       for network-order integer packs) was silently truncated here, since
       perl_alloc_string(out) computed its length via strlen(out) even
       though `out`/`pos` had already correctly tracked the true byte
       count through the whole packing loop above. */
    PerlValue *result = perl_alloc_string_len(out, (long long)pos);
    free(out);
    return result;
}

PerlValue *perl_unpack(PerlValue *fmt_pv, PerlValue *str_pv) {
    /* Scalar-context unpack: real Perl returns just the FIRST value from
       the list unpack() would produce in list context — NOT a reference to
       the whole list (this function previously duplicated the entire
       list-unpacking loop, independently of perl_unpack_to_array, and its
       own copy of that loop had drifted: it returned perl_ref_array(result)
       — an ARRAY ref — instead of a single scalar, and separately lacked
       '*' count and 'A' trailing-strip support, both fixed as part of D67).
       Delegating here eliminates the duplication and both drifted bugs at
       once — there is now exactly one unpack-format-parsing implementation. */
    PerlArray *arr = perl_unpack_to_array(fmt_pv, str_pv);
    PerlValue *result = (arr->len > 0) ? perl_clone(arr->elems[0]) : perl_alloc_undef();
    perl_array_free(arr);
    return result;
}

/* ── range ───────────────────────────────────────────────────────────────── */

/* Helper: unpack and return array directly (for emitArrayPtr; perl_unpack
   below delegates to this for scalar context too, taking just element 0). */
PerlArray *perl_unpack_to_array(PerlValue *fmt_pv, PerlValue *str_pv) {
    char *fmt = perl_to_string_dup(fmt_pv);
    /* D85: the read-side counterpart to pack's fix above — strlen(str)
       would silently truncate an unpack() call reading back exactly the
       kind of NUL-containing binary data pack() itself now correctly
       produces (e.g. unpack("N", pack("N", 1234567))). */
    long long slen_ll;
    char *str = perl_to_string_dup_len(str_pv, &slen_ll);
    size_t slen = (size_t)slen_ll;
    size_t strpos = 0;

    PerlArray *result = perl_array_new();

    for (const char *p = fmt; *p; ) {
        if (*p == '\\') { p++; if (!*p) break; continue; }

        /* read type char first */
        char type = *p++;
        if (!type) break;

        /* read count multiplier: a plain digit string, or '*' meaning "all
           remaining bytes" (a/A/h/H/b/B: as one field) or "as many whole
           elements as fit" (numeric types) — D67's original wiring omitted
           '*' entirely, silently defaulting to a count of 1. */
        long long count = 1;
        int starCount = 0;
        if (*p == '*') { starCount = 1; p++; }
        else if (*p && *p >= '0' && *p <= '9') {
            count = 0;
            while (*p && *p >= '0' && *p <= '9') { count = count * 10 + (*p - '0'); p++; }
        }
        /* p already points past the digits/'*' or at the next type char */

        int little_endian = 0;
        int size_bytes = 0;

        switch (type) {
        case 'a': case 'A': case 'h': case 'H': case 'b': case 'B': {
            /* D67: count/'*' is a FIELD WIDTH (one element), same as the
               pack side. 'A' additionally strips trailing spaces and NUL
               bytes from the extracted field (real Perl's ASCII-string
               semantics); 'a' (raw bytes) and h/H/b/B (still just raw byte
               copies, not real nibble/bit encoding — see TESTS.md D85) do
               not strip anything. */
            size_t remain = slen - strpos;
            size_t fieldWidth = starCount ? remain : (size_t)count;
            if (fieldWidth > remain) fieldWidth = remain;
            size_t end = fieldWidth;
            if (type == 'A') {
                while (end > 0 && (str[strpos + end - 1] == ' ' || str[strpos + end - 1] == '\0'))
                    end--;
            }
            char *s = malloc(end + 1);
            memcpy(s, str + strpos, end);
            s[end] = '\0';
            strpos += fieldWidth;
            /* D85: 'a' (raw bytes) extraction may itself contain embedded
               NULs — perl_alloc_string(s) would silently re-truncate what
               was just correctly extracted above via perl_alloc_string_len. */
            perl_array_push(result, perl_alloc_string_len(s, (long long)end));
            free(s);
            continue;
        }
        case 'x':
            strpos += count;
            continue;
        case 'X':
            if (strpos > 0) strpos -= count;
            continue;
        case '@':
            strpos = count;
            continue;
        case 'c': case 'C':
            size_bytes = 1;
            break;
        case 's': case 'S':
            size_bytes = 2;
            if (type == 'S') little_endian = 1;
            break;
        case 'n':
            size_bytes = 2; little_endian = 0;
            break;
        case 'N':
            size_bytes = 4; little_endian = 0;
            break;
        case 'v':
            size_bytes = 2; little_endian = 1;
            break;
        case 'V':
            size_bytes = 4; little_endian = 1;
            break;
        case 'l': case 'L':
            size_bytes = 4;
            if (type == 'L') little_endian = 1;
            break;
        case 'i': case 'I':
            size_bytes = (int)sizeof(int);
            if (type == 'I') little_endian = 1;
            break;
        case 'q': case 'Q':
            size_bytes = 8;
            if (type == 'Q') little_endian = 1;
            break;
        case 'f':
            size_bytes = 4;
            if (starCount) count = (long long)((slen - strpos) / size_bytes);
            for (long long c = 0; c < count; c++) {
                if (strpos + 4 > slen) { perl_array_push(result, perl_alloc_undef()); continue; }
                float fv;
                memcpy(&fv, str + strpos, 4);
                perl_array_push(result, perl_alloc_float((double)fv));
                strpos += 4;
            }
            continue;
        case 'd':
            size_bytes = 8;
            if (starCount) count = (long long)((slen - strpos) / size_bytes);
            for (long long c = 0; c < count; c++) {
                if (strpos + 8 > slen) { perl_array_push(result, perl_alloc_undef()); continue; }
                double dv;
                memcpy(&dv, str + strpos, 8);
                perl_array_push(result, perl_alloc_float(dv));
                strpos += 8;
            }
            continue;
        default:
            continue;
        }

        if (starCount) count = (size_bytes > 0) ? (long long)((slen - strpos) / size_bytes) : 0;
        for (long long c = 0; c < count; c++) {
            if (strpos + size_bytes > slen) {
                perl_array_push(result, perl_alloc_undef());
                continue;
            }
            unsigned char *bp = (unsigned char *)(str + strpos);
            long long val = read_int_from_buf(bp, size_bytes, little_endian);
            /* D67: read_int_from_buf() just ORs raw bytes together with no
               notion of signedness, so a narrower-than-64-bit field always
               comes back as its unsigned interpretation (e.g. pack("c",-100)
               unpacked as 156, pack("l",-1094861636) unpacked as
               3200105660 — confirmed both wrong before this fix). Signed
               types need explicit sign-extension when the field's own top
               bit is set; unsigned types need the opposite — masking off
               any stray high bits `read_int_from_buf`'s OR-based accumulator
               could have inherited (there shouldn't be any given `val`
               starts at 0, but this mirrors the pre-existing unsigned-mask
               intent explicitly rather than relying on that). Both are
               skipped for size_bytes==8 — a full 8 bytes already occupies
               all of `long long`'s width (no bits to extend or mask), and
               `1LL << 64` is undefined behavior (shift-by-width-of-type;
               on x86/ARM it silently wraps to `1LL << 0`, which previously
               zeroed every unsigned 8-byte unpack via `val &= 0`). */
            if (size_bytes < 8) {
                int isSignedType = (type == 'c' || type == 's' || type == 'l' ||
                                     type == 'i' || type == 'q');
                long long signBit = 1LL << (size_bytes * 8 - 1);
                if (isSignedType) {
                    if (val & signBit) val |= ~(signBit | (signBit - 1));
                } else {
                    val &= ((1LL << (size_bytes * 8)) - 1);
                }
            }
            perl_array_push(result, perl_alloc_int(val));
            strpos += size_bytes;
        }
    }

    free(fmt);
    free(str);
    return result;
}

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
    char *s = perl_to_string_dup(v);
    for (char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p);
    PerlValue *r = perl_alloc_string(s); free(s); return r;
}

PerlValue *perl_lc_str(PerlValue *v) {
    char *s = perl_to_string_dup(v);
    for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p);
    PerlValue *r = perl_alloc_string(s); free(s); return r;
}

PerlValue *perl_ucfirst_str(PerlValue *v) {
    char *s = perl_to_string_dup(v);
    if (s[0]) s[0] = (char)toupper((unsigned char)s[0]);
    PerlValue *r = perl_alloc_string(s); free(s); return r;
}

PerlValue *perl_lcfirst_str(PerlValue *v) {
    char *s = perl_to_string_dup(v);
    if (s[0]) s[0] = (char)tolower((unsigned char)s[0]);
    PerlValue *r = perl_alloc_string(s); free(s); return r;
}

/* ── index / rindex ──────────────────────────────────────────────────────── */
PerlValue *perl_index_str(PerlValue *str_pv, PerlValue *sub_pv, PerlValue *pos_pv) {
    char *s = perl_to_string_dup(str_pv);
    char *sub = perl_to_string_dup(sub_pv);
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
    char *s = perl_to_string_dup(str_pv);
    char *sub = perl_to_string_dup(sub_pv);
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

/* UTF-8 encode: write code point to buf, return bytes written (1-4) */
static int utf8_encode(unsigned char *buf, long long cp) {
    if (cp < 0x80) {
        buf[0] = (unsigned char)cp;
        return 1;
    } else if (cp < 0x800) {
        buf[0] = (unsigned char)(0xC0 | (cp >> 6));
        buf[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        buf[0] = (unsigned char)(0xE0 | (cp >> 12));
        buf[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp < 0x110000) {
        buf[0] = (unsigned char)(0xF0 | (cp >> 18));
        buf[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (unsigned char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* UTF-8 decode: read code point from buf, return bytes consumed. Sets *out to code point. */
static int utf8_decode(const unsigned char *buf, long long *out) {
    if (buf[0] < 0x80) {
        *out = buf[0];
        return 1;
    } else if ((buf[0] & 0xE0) == 0xC0) {
        if (buf[1] & 0xC0) {
            *out = ((buf[0] & 0x1F) << 6) | (buf[1] & 0x3F);
            return 2;
        }
    } else if ((buf[0] & 0xF0) == 0xE0) {
        if ((buf[1] & 0xC0) == 0x80 && (buf[2] & 0xC0) == 0x80) {
            *out = ((buf[0] & 0x0F) << 12) | ((buf[1] & 0x3F) << 6) | (buf[2] & 0x3F);
            return 3;
        }
    } else if ((buf[0] & 0xF8) == 0xF0) {
        if ((buf[1] & 0xC0) == 0x80 && (buf[2] & 0xC0) == 0x80 && (buf[3] & 0xC0) == 0x80) {
            *out = ((buf[0] & 0x07) << 18) | ((buf[1] & 0x3F) << 12) | ((buf[2] & 0x3F) << 6) | (buf[3] & 0x3F);
            return 4;
        }
    }
    *out = 0;
    return 1; /* invalid sequence, consume 1 byte */
}

/* Count UTF-8 code points in string */
static long long utf8_strlen(const char *s) {
    long long count = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        count++;
        if (*p < 0x80) p++;
        else if ((*p & 0xE0) == 0xC0) p += 2;
        else if ((*p & 0xF0) == 0xE0) p += 3;
        else if ((*p & 0xF8) == 0xF0) p += 4;
        else p++;
    }
    return count;
}

/* D85: bounded counterpart to utf8_strlen — scans exactly `n` bytes instead
   of stopping at the first NUL, so length() is correct for a string with
   embedded NUL bytes (each counts as its own 1-byte code point, matching
   real Perl's byte-is-a-character-when-not-UTF8-flagged behavior). */
static long long utf8_strlen_n(const char *s, long long n) {
    long long count = 0;
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *end = p + n;
    while (p < end) {
        count++;
        if (*p < 0x80) p++;
        else if ((*p & 0xE0) == 0xC0) p += 2;
        else if ((*p & 0xF0) == 0xE0) p += 3;
        else if ((*p & 0xF8) == 0xF0) p += 4;
        else p++;
    }
    return count;
}

/* Get byte offset of the nth UTF-8 code point */
static long long utf8_char_to_byte(const char *s, long long n) {
    long long count = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && count < n) {
        if (*p < 0x80) p++;
        else if ((*p & 0xE0) == 0xC0) p += 2;
        else if ((*p & 0xF0) == 0xE0) p += 3;
        else if ((*p & 0xF8) == 0xF0) p += 4;
        else p++;
        count++;
    }
    return (long long)(p - (const unsigned char *)s);
}

PerlValue *perl_chr_val(PerlValue *v) {
    long long n = perl_to_int(v);
    if (n < 0) n = 0;
    if (n > 0x10FFFF) n = 0xFFFD; /* replacement char */
    unsigned char buf[4];
    int len = utf8_encode(buf, n);
    buf[len] = '\0';
    /* D85: chr(0) encodes to a single NUL byte — perl_alloc_string(buf)
       would compute its length via strlen(buf), which is 0 for a buffer
       starting with 0x00, silently discarding the character entirely.
       utf8_encode's own return value is already the exact byte length. */
    return perl_alloc_string_len((char *)buf, len);
}

PerlValue *perl_ord_val(PerlValue *v) {
    char *s = perl_to_string_dup(v);
    long long cp;
    utf8_decode((const unsigned char *)s, &cp);
    free(s);
    return perl_alloc_int(cp);
}

PerlValue *perl_hex_val(PerlValue *v) {
    char *s = perl_to_string_dup(v);
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    long long r = (long long)strtoll(p, NULL, 16);
    free(s);
    return perl_alloc_int(r);
}

PerlValue *perl_oct_val(PerlValue *v) {
    char *s = perl_to_string_dup(v);
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
    char *s = perl_to_string_dup(v);
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
    /* D85: NUL-safe. */
    long long la, lb;
    char *sa = perl_to_string_dup_len(*(PerlValue**)a, &la);
    char *sb = perl_to_string_dup_len(*(PerlValue**)b, &lb);
    int r = perl_strcmp_len(sa, la, sb, lb); free(sa); free(sb); return r;
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
    /* D85: NUL-safe. */
    long long la, lb;
    char *sa = perl_to_string_dup_len(a, &la), *sb = perl_to_string_dup_len(b, &lb);
    int r = perl_strcmp_len(sa, la, sb, lb);
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

  uint32_t name_count, name_entry_size;
  pcre2_pattern_info(re, PCRE2_INFO_NAMECOUNT,     &name_count);
  if (name_count == 0) return;
  pcre2_pattern_info(re, PCRE2_INFO_NAMEENTRYSIZE, &name_entry_size);

  PCRE2_SPTR name_table;
  pcre2_pattern_info(re, PCRE2_INFO_NAMETABLE, &name_table);
  PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);

  for (uint32_t i = 0; i < name_count; i++) {
    PCRE2_SPTR entry = name_table + name_entry_size * i;
    /* first 2 bytes are group number (big-endian uint16) */
    uint32_t group_num = (entry[0] << 8) | entry[1];
    const char *name_str = (const char *)(entry + 2);
    PCRE2_SIZE cstart = ov[2 * group_num], cend = ov[2 * group_num + 1];
    if (cstart != PCRE2_UNSET && cstart < cend) {
      char *cap = malloc(cend - cstart + 1);
      memcpy(cap, s + cstart, cend - cstart);
      cap[cend - cstart] = '\0';
      PerlValue *key = perl_alloc_string(name_str);
      PerlValue *val = perl_alloc_string(cap);
      perl_hash_set_sv(perl_plus_hash, key, val);
      perl_free(key);
      perl_free(val);
      free(cap);
    }
  }
}

/* ── PCRE2 compiled-pattern cache (per-thread LRU, max 256 entries) ─────────
 * Keyed by (pattern ‖ flags) — read-only pcre2_code* after compilation,
 * so no locking is needed.  LRU eviction via access-order array.             */

#define REGEX_CACHE_CAP 256

typedef struct RegexCacheEntry {
    char key[128];          /* pattern ‖ flags (NUL-terminated) */
    pcre2_code *compiled;   /* read-only after pcre2_compile */
} RegexCacheEntry;

static __thread RegexCacheEntry regex_cache_[REGEX_CACHE_CAP];
static __thread int regex_cache_len_ = 0;

static pcre2_code *regex_cache_lookup(const char *pattern, const char *flags) {
    char key[128];
    int ki = 0;
    for (const char *p = pattern; *p && ki < 127; ki++, p++) key[ki] = *p;
    key[ki++] = '\x1f';   /* unit separator */
    for (const char *f = flags; *f && ki < 127; ki++, f++) key[ki] = *f;
    key[ki] = '\0';

    for (int i = 0; i < regex_cache_len_; i++) {
        if (regex_cache_[i].compiled && strcmp(regex_cache_[i].key, key) == 0) {
            /* promote to front (LRU) */
            RegexCacheEntry tmp = regex_cache_[i];
            for (int j = i; j > 0; j--) regex_cache_[j] = regex_cache_[j-1];
            regex_cache_[0] = tmp;
            return tmp.compiled;
        }
    }
    return NULL;
}

static void regex_cache_insert(const char *pattern, const char *flags, pcre2_code *compiled) {
    if (!compiled) return;
    char key[128];
    int ki = 0;
    for (const char *p = pattern; *p && ki < 127; ki++, p++) key[ki] = *p;
    key[ki++] = '\x1f';
    for (const char *f = flags; *f && ki < 127; ki++, f++) key[ki] = *f;
    key[ki] = '\0';

    /* check if already in cache (shouldn't be, but handle it) */
    for (int i = 0; i < regex_cache_len_; i++) {
        if (strcmp(regex_cache_[i].key, key) == 0) {
            regex_cache_[i].compiled = compiled;
            RegexCacheEntry tmp = regex_cache_[i];
            for (int j = i; j > 0; j--) regex_cache_[j] = regex_cache_[j-1];
            regex_cache_[0] = tmp;
            return;
        }
    }

    /* evict LRU (last entry) if full */
    if (regex_cache_len_ >= REGEX_CACHE_CAP) {
        pcre2_code_free(regex_cache_[regex_cache_len_ - 1].compiled);
        regex_cache_[regex_cache_len_ - 1].compiled = NULL;
        regex_cache_[regex_cache_len_ - 1].key[0] = '\0';
    } else {
        regex_cache_len_++;
    }

    /* insert at front */
    RegexCacheEntry *dst = &regex_cache_[0];
    for (int j = regex_cache_len_ - 1; j > 0; j--) regex_cache_[j] = regex_cache_[j-1];
    memcpy(dst->key, key, ki + 1);
    dst->compiled = compiled;
}

PerlValue *perl_regex_match(PerlValue *str, const char *pattern, const char *flags) {
    int errcode; PCRE2_SIZE erroffset;
    pcre2_code *re = regex_cache_lookup(pattern, flags);
    if (!re) {
        re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                           pcre_flags(flags), &errcode, &erroffset, NULL);
        if (re) regex_cache_insert(pattern, flags, re);
    }
    if (!re) return perl_alloc_int(0);

    char *s = perl_to_string_dup(str);
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
          s_dollar_amp.tag = PERL_STRING; s_dollar_amp.sval = ms_str;
          s_dollar_amp.slen = (long long)(me - ms); }
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
    /* Do NOT free re — it comes from the shared cache. */
    return perl_alloc_int(rc > 0 ? 1 : 0);
}

long long perl_regex_subst(PerlValue *str, const char *pattern, const char *repl, const char *flags) {
    /* separate /g /e from PCRE options */
    int global = 0, eval_repl = 0;
    char clean[64]; int ci = 0;
    for (const char *fp = flags; *fp; fp++) {
        if      (*fp == 'g') global = 1;
        else if (*fp == 'e') eval_repl = 1;
        else if (ci < 63)    clean[ci++] = *fp;
    }
    clean[ci] = '\0';

    pcre2_code *re = regex_cache_lookup(pattern, clean);
    int errcode; PCRE2_SIZE erroffset;
    if (!re) {
        re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                           pcre_flags(clean), &errcode, &erroffset, NULL);
        if (re) regex_cache_insert(pattern, clean, re);
    }

    char *s = perl_to_string_dup(str);
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
        /* build expanded replacement string */
        char *expanded = NULL; size_t exp_len = 0, exp_cap = 128;
        expanded = malloc(exp_cap);
#define EXPENSURE(n) do { while(exp_len+(n)+1>exp_cap){exp_cap*=2;expanded=realloc(expanded,exp_cap);} } while(0)
        for (const char *rp = repl; *rp; ) {
            if (*rp == '$' && isdigit((unsigned char)rp[1])) {
                int n = rp[1] - '0'; rp += 2;
                size_t cstart = (n == 0) ? mstart : (n < rc ? ov[2*n]   : 0);
                size_t cend   = (n == 0) ? mend   : (n < rc ? ov[2*n+1] : 0);
                size_t caplen = (cstart < cend) ? cend - cstart : 0;
                EXPENSURE(caplen); memcpy(expanded + exp_len, s + cstart, caplen); exp_len += caplen;
            } else {
                EXPENSURE(1); expanded[exp_len++] = *rp++;
            }
        }
        expanded[exp_len] = '\0';
        /* /e: evaluate expanded string as Perl expression — disabled (no JIT) */
        const char *rep_text = expanded;
        size_t replen = exp_len;
        ENSURE(replen); memcpy(out + out_len, rep_text, replen); out_len += replen;
        free(expanded);
#undef EXPENSURE
        count++;

        if (mend == mstart) {
            /* Zero-length match: copy the char at the match point (mstart),
               not the old search-start `pos` — an anchor like $ can match
               ahead of pos (e.g. at end-of-string), and the "text before
               match" copy above already flushed s[pos..mstart) into out, so
               re-copying s[pos] here would duplicate it. Copying the wrong
               character also let `pos` end up as mstart+1 = slen+1 when the
               match was exactly at end-of-string, underflowing the `slen -
               pos` below (size_t wraps to ~SIZE_MAX) and corrupting the
               heap via the resulting "huge remaining length" memcpy. */
            if (mstart < slen) { ENSURE(1); out[out_len++] = s[mstart]; }
            pos = mstart + 1;
        } else {
            pos = mend;
        }

        if (!global) {
            size_t rem = (pos < slen) ? slen - pos : 0;
            ENSURE(rem); memcpy(out + out_len, s + pos, rem); out_len += rem; break;
        }
    }
    out[out_len] = '\0';
#undef ENSURE

    /* update PerlValue in-place */
    if (str->tag == PERL_STRING && str->sval) free(str->sval);
    str->tag  = PERL_STRING;
    str->sval = out;
    str->slen = (long long)out_len; /* D85: out_len is already correctly tracked above */

    free(s);
    pcre2_match_data_free(md);
    /* Do NOT free re — it comes from the shared cache. */
    return count;
}

PerlArray *perl_split_regex(const char *pattern, const char *flags, PerlValue *str) {
    PerlArray *arr = perl_array_new();
    char *s = perl_to_string_dup(str);
    size_t slen = strlen(s);

    /* empty pattern: split into individual characters (Perl semantics) */
    if (pattern[0] == '\0') {
        for (size_t i = 0; i < slen; i++) {
            char ch[2] = {s[i], '\0'};
            PerlValue *v = perl_alloc_string(ch);
            perl_array_push(arr, v); perl_free(v);
        }
        free(s);
        return arr;
    }

    int errcode; PCRE2_SIZE erroffset;
    pcre2_code *re = regex_cache_lookup(pattern, flags);
    if (!re) {
        re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                           pcre_flags(flags), &errcode, &erroffset, NULL);
        if (re) regex_cache_insert(pattern, flags, re);
    }
    if (!re) { free(s); return arr; }
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
    /* Do NOT free re — it comes from the shared cache. */
    return arr;
}

PerlValue *perl_get_plus_hash(void) {
  return perl_plus_hash ? perl_ref_hash(perl_plus_hash) : perl_alloc_undef();
}

PerlArray *perl_plus_hash_keys(void) {
  return perl_plus_hash ? perl_hash_keys(perl_plus_hash) : perl_array_new();
}

PerlValue *perl_plus_hash_get(PerlValue *key) {
  if (!perl_plus_hash) return perl_alloc_undef();
  PerlValue *v = perl_hash_get_sv(perl_plus_hash, key);
  return v ? perl_clone(v) : perl_alloc_undef();
}

void perl_clear_named_captures(void) {
  if (perl_plus_hash) {
    perl_hash_free(perl_plus_hash);
    perl_plus_hash = NULL;
  }
}

PerlValue *perl_regex_match_g(PerlValue *str, const char *pattern, const char *flags) {
    int errcode; PCRE2_SIZE erroffset;
    pcre2_code *re = regex_cache_lookup(pattern, flags);
    if (!re) {
        re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                           pcre_flags(flags), &errcode, &erroffset, NULL);
        if (re) regex_cache_insert(pattern, flags, re);
    }
    if (!re) { str->matchpos = 0; return perl_alloc_int(0); }

    char *s = perl_to_string_dup(str);
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
        free(s); pcre2_match_data_free(md);
        /* Do NOT free re — it comes from the shared cache. */
        return perl_alloc_int(1);
    } else {
        str->matchpos = 0;
        free(s); pcre2_match_data_free(md);
        /* Do NOT free re — it comes from the shared cache. */
        return perl_alloc_int(0);
    }
}

PerlArray *perl_regex_match_all(PerlValue *str, const char *pattern, const char *flags) {
    int errcode; PCRE2_SIZE erroffset;
    pcre2_code *re = regex_cache_lookup(pattern, flags);
    if (!re) {
        re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                           pcre_flags(flags), &errcode, &erroffset, NULL);
        if (re) regex_cache_insert(pattern, flags, re);
    }
    PerlArray *arr = perl_array_new();
    if (!re) return arr;

    uint32_t capturecount = 0;
    pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &capturecount);

    char *s = perl_to_string_dup(str);
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

    free(s); pcre2_match_data_free(md);
    /* Do NOT free re — it comes from the shared cache. */
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
    char *k = perl_to_string_dup(key);
    const char *val = getenv(k);
    free(k);
    return val ? perl_alloc_string(val) : perl_alloc_undef();
}

void perl_env_set(PerlValue *key, PerlValue *val) {
    char *k = perl_to_string_dup(key);
    char *v = perl_to_string_dup(val);
    setenv(k, v, 1);
    free(k); free(v);
}

void perl_warn(PerlValue *msg) {
    char *s = perl_to_string_dup(msg);
    size_t len = strlen(s);
    fputs(s, stderr);
    if (len == 0 || s[len - 1] != '\n') fputc('\n', stderr);
    free(s);
}

PerlValue *perl_system(PerlValue *cmd) {
    char *s = perl_to_string_dup(cmd);
    int ret = system(s);
    free(s);
    /* Real Perl's system() returns the raw wait(2) status word, not the
       unwrapped exit code — the documented idiom is `$rc >> 8` to recover
       the exit code (matching $?'s convention, though $? itself is not
       yet implemented). Returning WEXITSTATUS(ret) directly here silently
       broke that idiom for any caller following it. -1 (system() itself
       failed to launch a child at all) is returned as-is, matching Perl. */
    if (ret == -1) return perl_alloc_int(-1);
    return perl_alloc_int(ret);
}

PerlValue *perl_backtick(PerlValue *cmd) {
    char *s = perl_to_string_dup(cmd);
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

    /* D85: NUL-safe input read — tr/// is a pure byte-for-byte lookup-table
       mapping with no UTF-8/regex involvement, so it's cheap to make fully
       NUL-safe end to end (unlike substr/regex, which stay a documented
       remaining gap). */
    long long in_len_ll;
    char *s = perl_to_string_dup_len(str, &in_len_ll);
    size_t in_len = (size_t)in_len_ll;
    char *out = malloc(in_len + 1);
    size_t out_len = 0;
    long long count = 0;
    char last_out = 0;
    int has_last = 0;

    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)s[i];
        int mapped = table[c];
        if (mapped == -1) {
            /* no translation — always pass through; /s never squeezes untranslated chars */
            out[out_len++] = (char)c;
            has_last = 0;  /* break any in-progress squeeze run */
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
    str->slen = (long long)out_len;
    free(s);
    return count;
}

/* ── command-line arguments ─────────────────────────────────────────────── */

static PerlArray *perl_argv_arr = NULL;
static PerlValue  perl_dollar0_val = { .tag = PERL_STRING };

PerlArray *perl_init_argv(int argc, char **argv) {
    /* Prevent glibc from returning heap pages to OS between frees (reduces page
       faults from re-allocation in tight loops with many short-lived objects). */
    mallopt(M_TRIM_THRESHOLD, -1);
    mallopt(M_MMAP_THRESHOLD, 64 * 1024 * 1024); /* only mmap for >64MB allocs */
    perl_argv_arr = perl_array_new();
    /* $0 = script name (argv[0]) */
    perl_dollar0_val.sval = strdup(argc > 0 ? argv[0] : "");
    perl_dollar0_val.slen = (long long)strlen(perl_dollar0_val.sval);
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
    char *path = perl_to_string_dup(path_pv);
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

/* ── Time::HiRes (D30) ───────────────────────────────────────────────────────
   No real Time::HiRes.pm exists — this is a built-in implementation, like
   POSIX/Scalar::Util. `time`/`sleep`'s higher-precision override is gated
   by explicit import (see parser.cpp's KW_TIME/KW_SLEEP handling and
   main.cpp's inlineModules()); gettimeofday/usleep/tv_interval have no
   pre-existing builtin meaning so are always available unqualified. */

PerlValue *perl_hires_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return perl_alloc_float((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
}

/* list context: (seconds, microseconds) */
PerlArray *perl_hires_gettimeofday_list(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    PerlArray *a = perl_array_new();
    perl_array_push(a, perl_alloc_int((long long)ts.tv_sec));
    perl_array_push(a, perl_alloc_int((long long)(ts.tv_nsec / 1000)));
    return a;
}

/* scalar context: floating-point seconds (same as perl_hires_time) */
PerlValue *perl_hires_gettimeofday_scalar(void) {
    return perl_hires_time();
}

/* sleep(SECONDS), fractional seconds allowed; returns actual seconds slept */
PerlValue *perl_hires_sleep(PerlValue *secs) {
    double s = secs ? perl_to_float(secs) : 0.0;
    struct timespec req;
    req.tv_sec  = (time_t)s;
    req.tv_nsec = (long)((s - (double)req.tv_sec) * 1e9);
    struct timespec start, end;
    clock_gettime(CLOCK_REALTIME, &start);
    nanosleep(&req, NULL);
    clock_gettime(CLOCK_REALTIME, &end);
    double elapsed = ((double)end.tv_sec - (double)start.tv_sec) +
                      ((double)end.tv_nsec - (double)start.tv_nsec) / 1e9;
    return perl_alloc_float(elapsed);
}

/* usleep(MICROSECONDS); returns actual microseconds slept */
PerlValue *perl_hires_usleep(PerlValue *usecs) {
    long long u = usecs ? perl_to_int(usecs) : 0;
    struct timespec req;
    req.tv_sec  = (time_t)(u / 1000000);
    req.tv_nsec = (long)((u % 1000000) * 1000);
    struct timespec start, end;
    clock_gettime(CLOCK_REALTIME, &start);
    nanosleep(&req, NULL);
    clock_gettime(CLOCK_REALTIME, &end);
    long long elapsed_us = (long long)(((double)end.tv_sec - (double)start.tv_sec) * 1000000.0 +
                                        ((double)end.tv_nsec - (double)start.tv_nsec) / 1000.0);
    return perl_alloc_int(elapsed_us);
}

/* tv_interval([$sec,$usec], [$sec,$usec] or omitted => now) => elapsed seconds */
PerlValue *perl_hires_tv_interval(PerlValue *t0ref, PerlValue *t1ref) {
    double t0 = 0.0, t1;
    if (t0ref && t0ref->tag == PERL_REF_ARRAY && t0ref->pval) {
        PerlArray *a = (PerlArray *)t0ref->pval;
        double sec  = a->len > 0 ? perl_to_float(a->elems[0]) : 0.0;
        double usec = a->len > 1 ? perl_to_float(a->elems[1]) : 0.0;
        t0 = sec + usec / 1e6;
    }
    if (t1ref && t1ref->tag == PERL_REF_ARRAY && t1ref->pval) {
        PerlArray *a = (PerlArray *)t1ref->pval;
        double sec  = a->len > 0 ? perl_to_float(a->elems[0]) : 0.0;
        double usec = a->len > 1 ? perl_to_float(a->elems[1]) : 0.0;
        t1 = sec + usec / 1e6;
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        t1 = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    }
    return perl_alloc_float(t1 - t0);
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
    /* D69: real List::Util::uniq keeps the first occurrence of each
       distinct value *anywhere* in the list, not just consecutive runs
       (that was this function's original, wrong behavior — it only
       stripped adjacent duplicates, Unix `uniq`-command style, so e.g.
       (3,1,4,1,5,9,1) with no adjacent repeats passed through completely
       unchanged instead of dropping the repeated 1s). Uses a hash-based
       "seen" set keyed by each element's stringification, matching how
       real Perl's uniq compares values (stringified equality). */
    PerlArray *res = perl_array_new();
    if (!a) return res;
    PerlHash *seen = perl_hash_new();
    PerlValue *marker = perl_alloc_int(1);
    for (long long i = 0; i < a->len; i++) {
        char *s = perl_to_string_dup(a->elems[i]);
        if (!perl_hash_exists_str(seen, s)) {
            /* perl_hash_set_str clones marker internally, so the same
               marker PerlValue* can be reused across every insertion here
               without being consumed or invalidated. */
            perl_hash_set_str(seen, s, marker);
            perl_array_push(res, perl_clone(a->elems[i]));
        }
        free(s);
    }
    perl_free(marker);
    perl_hash_free(seen);
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

/* D62: release this comparator's captures — symmetric to the increment
   perl_array_push_capture already did at the call site. There's no
   PerlClosure/teardown event for sort's captures (unlike an AnonSub), so
   this is the only place that can release them; without it, every scalar
   any sort{} comparator captures would be incremented once and never
   decremented, permanently pinning (leaking) it. */
static void perl_sort_release_captures(PerlArray *captures) {
    if (!captures) return;
    for (long long i = 0; i < captures->len; i++)
        perl_release_capture(captures->elems[i]);
    perl_array_free_nc(captures);  /* wrapper only — elements were just
                                       released above, or are :shared and
                                       untouched either way */
}

PerlArray *perl_sort_custom(PerlArray *a, PerlSortCmpFn cmp, PerlArray *captures) {
    PerlArray *res = perl_array_new();
    if (!a) { perl_sort_release_captures(captures); return res; }
    /* copy element pointers (shallow) */
    for (long long i = 0; i < a->len; i++)
        perl_array_push(res, perl_clone(a->elems[i]));
    /* D61: install the comparator's closure captures (if any) via the same
       s_current_captures context a closure call already uses, so the
       compiled comparator can read them with perl_get_capture(idx) — save
       and restore rather than blindly clearing, so a comparator that
       itself triggers a nested sort{}/closure call doesn't corrupt the
       outer one's context. */
    PerlSortCmpFn saved_cmp  = sort_custom_cmp_;
    PerlValue    **saved_caps = s_current_captures;
    int            saved_n    = s_ncaptures;
    sort_custom_cmp_    = cmp;
    s_current_captures  = (captures && captures->len > 0) ? captures->elems : NULL;
    s_ncaptures         = captures ? (int)captures->len : 0;
    qsort(res->elems, (size_t)res->len, sizeof(PerlValue *), sort_qsort_wrap_);
    sort_custom_cmp_    = saved_cmp;
    s_current_captures  = saved_caps;
    s_ncaptures         = saved_n;
    perl_sort_release_captures(captures);  /* D62 */
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
    char *fmt = perl_to_string_dup(args->elems[0]);
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
    case PERL_XS_PTR:     return perl_alloc_string("PTR");
    default:              return perl_alloc_undef();
    }
}

PerlValue *perl_su_looks_like_number(PerlValue *v) {
    /* Real Perl's looks_like_number returns 1 for true but "" (empty
       string, not 0) for false — logically equivalent in boolean context,
       but the string value differs if printed/interpolated directly. */
    if (!v || v->tag == PERL_UNDEF) return perl_alloc_string("");
    if (v->tag == PERL_INT || v->tag == PERL_FLOAT) return perl_alloc_int(1);
    if (v->tag != PERL_STRING || !v->sval) return perl_alloc_string("");
    const char *s = v->sval;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '+' || *s == '-') s++;
    if (*s == '\0') return perl_alloc_string("");
    int has_digit = 0;
    while (*s >= '0' && *s <= '9') { has_digit = 1; s++; }
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { has_digit = 1; s++; } }
    if (!has_digit) return perl_alloc_string("");
    if (*s == 'e' || *s == 'E') {
        s++;
        if (*s == '+' || *s == '-') s++;
        if (*s < '0' || *s > '9') return perl_alloc_string("");
        while (*s >= '0' && *s <= '9') s++;
    }
    while (*s == ' ' || *s == '\t') s++;
    return *s == '\0' ? perl_alloc_int(1) : perl_alloc_string("");
}

/* ── Carp ─────────────────────────────────────────────────────────────────── */

void perl_carp_croak(PerlArray *args) {
     /* Route through perl_die (not fprintf+exit) so `eval { croak(...) }` —
        the standard Carp usage pattern for turning a library error into a
        catchable exception — actually catches it instead of killing the
        whole process. perl_die already has the correct dual behavior:
        longjmp to the nearest eval (setting $@) if one is active, or print
        to stderr and exit(1) at top level if not. perl_die only reads its
        argument (via perl_assign / perl_to_string_dup, both of which clone
        rather than take ownership), so passing a borrowed array element or
        a stack-local PerlValue here is safe either way. */
     if (args && args->len > 0) {
         perl_die(args->elems[0], NULL, 0);
     } else {
         /* D85: .slen must be set explicitly — see perl_set_dollar_at_cstr's
            comment for why an unset .slen on a designated-initializer
            literal silently truncates to "". */
         PerlValue died = { .tag = PERL_STRING, .sval = (char *)"Died", .slen = 4 };
         perl_die(&died, NULL, 0);
    }
}

void perl_carp_carp(PerlArray *args) {
    char *msg = (args && args->len > 0) ? perl_to_string_dup(args->elems[0]) : strdup("Warning: something's wrong");
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
    char *path = perl_to_string_dup(v);
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
    char *path = perl_to_string_dup(v);
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
    char *pat = perl_to_string_dup(pattern);
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

/* D75: depth-first search matching perl_find_method_dfs's traversal —
   returns 1 if `class_name` itself, or any class reachable through its
   full @ISA tree (all parents, not just the first), equals `want`. */
static int perl_isa_dfs_match(const char *class_name, const char *want, int depth) {
    if (!class_name || depth > 32) return 0;
    if (strcmp(class_name, want) == 0) return 1;
    const char *parents[ISA_MAX_DIRECT_PARENTS];
    int np = perl_isa_direct_parents(class_name, parents, ISA_MAX_DIRECT_PARENTS);
    for (int i = 0; i < np; i++)
        if (perl_isa_dfs_match(parents[i], want, depth + 1)) return 1;
    return 0;
}

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
        case PERL_XS_PTR:     tname = "PTR"; break;
        default: break;
        }
        if (tname && strcmp(tname, want) == 0) return perl_alloc_int(1);
        return perl_alloc_int(0);
    }
    /* D75: walk the full @ISA tree (multiple inheritance), not just a
       single linear chain — a class with 2+ parents (e.g. our @ISA =
       ('B','C')) previously could only ever match against whichever
       parent perl_get_parent's single-entry lookup happened to return. */
    return perl_alloc_int(perl_isa_dfs_match(got, want, 0));
}

PerlValue *perl_can_check(PerlValue *obj, PerlValue *method_pv) {
    if (!obj || !method_pv) return perl_alloc_undef();
    char *method = perl_to_string_dup(method_pv);
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
            /* append at offset — D85: raw fread() output legitimately may
               contain embedded NUL bytes, so use buf_pv->slen (not
               strlen()) for the existing-buffer length. */
            char *cur = (buf_pv->tag == PERL_STRING && buf_pv->sval) ? buf_pv->sval : (char *)"";
            long long curlen = (buf_pv->tag == PERL_STRING) ? buf_pv->slen : 0;
            if (off > curlen) off = curlen;
            char *newbuf = (char *)malloc(off + got + 1);
            memcpy(newbuf, cur, (size_t)off);
            memcpy(newbuf + off, tmp, got);
            newbuf[off + got] = '\0';
            if (buf_pv->tag == PERL_STRING && buf_pv->sval) free(buf_pv->sval);
            buf_pv->tag = PERL_STRING;
            buf_pv->sval = newbuf;
            buf_pv->slen = off + (long long)got;
        } else {
            if (buf_pv->tag == PERL_STRING && buf_pv->sval) free(buf_pv->sval);
            buf_pv->tag = PERL_STRING;
            buf_pv->sval = tmp;
            buf_pv->slen = (long long)got;
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
        char *path = perl_to_string_dup(fh_or_path);
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

void perl_set_pos_str(PerlValue *pv, PerlValue *pos) {
    if (!pv) return;
    if (!pos || pos->tag == PERL_UNDEF) { pv->matchpos = 0; return; }
    long long n = perl_to_int(pos);
    pv->matchpos = (n < 0) ? 0 : n;
}

PerlValue *perl_getpid(void) {
    return perl_alloc_int((long long)getpid());
}

PerlValue *perl_get_os_name(void) {
    return perl_alloc_string("linux");
}

/* ── XS / FFI support ───────────────────────────────────────────────────── */

typedef struct PerlXSModuleInfo {
    char *name;
    void *handle;
    struct PerlXSModuleInfo *next;
} PerlXSModuleInfo;

typedef enum {
    XS_TYPE_INVALID = 0,
    XS_TYPE_VOID,
    XS_TYPE_LONG,
    XS_TYPE_DOUBLE,
    XS_TYPE_STRING,
    XS_TYPE_PTR,
} PerlXSType;

typedef struct {
    PerlXSType ret_type;
    PerlXSType arg_types[4];
    int arg_count;
} PerlXSSignature;

static __thread PerlXSModuleInfo *s_xs_module_list = NULL;

static void perl_xs_set_error(const char *msg) {
    perl_set_dollar_at_cstr(msg ? msg : "XS call failed");
}

static PerlXSModuleInfo *perl_xs_find_module(const char *libname) {
    PerlXSModuleInfo *module = s_xs_module_list;
    while (module) {
        if (strcmp(module->name, libname) == 0) return module;
        module = module->next;
    }
    return NULL;
}

static PerlXSModuleInfo *perl_xs_find_or_load_module(const char *libname) {
    PerlXSModuleInfo *module;
    void *handle;

    if (!libname || !*libname) {
        perl_xs_set_error("XS::load_library: missing library name");
        return NULL;
    }

    module = perl_xs_find_module(libname);
    if (module) return module;

    handle = dlopen(libname, RTLD_LAZY);
    if (!handle) {
        perl_xs_set_error(dlerror());
        return NULL;
    }

    module = calloc(1, sizeof(*module));
    if (!module) {
        dlclose(handle);
        perl_xs_set_error("XS::load_library: out of memory");
        return NULL;
    }

    module->name = strdup(libname);
    module->handle = handle;
    module->next = s_xs_module_list;
    s_xs_module_list = module;
    return module;
}

static PerlXSType perl_xs_parse_type_name(const char *name) {
    if (strcmp(name, "void") == 0) return XS_TYPE_VOID;
    if (strcmp(name, "long") == 0 || strcmp(name, "int") == 0 ||
        strcmp(name, "size_t") == 0 || strcmp(name, "ssize_t") == 0) return XS_TYPE_LONG;
    if (strcmp(name, "double") == 0) return XS_TYPE_DOUBLE;
    if (strcmp(name, "string") == 0 || strcmp(name, "str") == 0) return XS_TYPE_STRING;
    if (strcmp(name, "ptr") == 0 || strcmp(name, "pointer") == 0) return XS_TYPE_PTR;
    return XS_TYPE_INVALID;
}

static void perl_xs_trim_token(char *s) {
    char *start = s;
    char *end;
    size_t len;

    if (!s) return;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    end = s + len;
    (void)end;
}

static int perl_xs_parse_signature(const char *sig, PerlXSSignature *out) {
    char retbuf[32];
    char argbuf[128];
    const char *lp;
    const char *rp;
    size_t retlen;
    size_t arglen;
    char *cursor;

    if (!sig || !out) return 0;
    memset(out, 0, sizeof(*out));

    lp = strchr(sig, '(');
    rp = strrchr(sig, ')');
    if (!lp || !rp || rp < lp) return 0;

    retlen = (size_t)(lp - sig);
    arglen = (size_t)(rp - lp - 1);
    if (retlen == 0 || retlen >= sizeof(retbuf) || arglen >= sizeof(argbuf)) return 0;

    memcpy(retbuf, sig, retlen);
    retbuf[retlen] = '\0';
    memcpy(argbuf, lp + 1, arglen);
    argbuf[arglen] = '\0';
    perl_xs_trim_token(retbuf);
    perl_xs_trim_token(argbuf);

    out->ret_type = perl_xs_parse_type_name(retbuf);
    if (out->ret_type == XS_TYPE_INVALID) return 0;

    cursor = argbuf;
    while (*cursor) {
        char *comma;
        PerlXSType arg_type;

        if (out->arg_count >= 4) return 0;
        comma = strchr(cursor, ',');
        if (comma) *comma = '\0';
        perl_xs_trim_token(cursor);
        arg_type = perl_xs_parse_type_name(cursor);
        if (arg_type == XS_TYPE_INVALID) return 0;
        out->arg_types[out->arg_count++] = arg_type;
        if (!comma) break;
        cursor = comma + 1;
    }
    return 1;
}

PerlValue *perl_xs_load_library(PerlValue *libname_pv) {
    char *libname;
    PerlXSModuleInfo *module;
    PerlValue empty = { .tag = PERL_UNDEF };

    if (!libname_pv || libname_pv->tag == PERL_UNDEF) {
        perl_xs_set_error("XS::load_library: missing library name");
        return perl_alloc_undef();
    }

    libname = perl_to_string_dup(libname_pv);
    if (!libname) {
        perl_xs_set_error("XS::load_library: invalid library name");
        return perl_alloc_undef();
    }

    module = perl_xs_find_or_load_module(libname);
    free(libname);
    if (!module) return perl_alloc_undef();

    perl_assign(&s_dollar_at, &empty);
    return perl_alloc_string(module->name);
}

static int perl_xs_signature_code(const PerlXSSignature *sig) {
    int code = 0;
    for (int i = 0; i < sig->arg_count; i++)
        code |= ((int)sig->arg_types[i] << (i * 4));
    return code;
}

#define XS_CODE1(a0) ((int)(a0))
#define XS_CODE2(a0, a1) (XS_CODE1(a0) | ((int)(a1) << 4))
#define XS_CODE3(a0, a1, a2) (XS_CODE2(a0, a1) | ((int)(a2) << 8))
#define XS_CODE4(a0, a1, a2, a3) (XS_CODE3(a0, a1, a2) | ((int)(a3) << 12))

#define XS_TYPES_1(M) \
    M(XS_TYPE_LONG, long long, long_args[0]); \
    M(XS_TYPE_DOUBLE, double, double_args[0]); \
    M(XS_TYPE_STRING, const char *, string_args[0]); \
    M(XS_TYPE_PTR, void *, ptr_args[0]);

#define XS_TYPES_2(M) \
    XS_TYPES_2_A(M, XS_TYPE_LONG, long long, long_args[0]); \
    XS_TYPES_2_A(M, XS_TYPE_DOUBLE, double, double_args[0]); \
    XS_TYPES_2_A(M, XS_TYPE_STRING, const char *, string_args[0]); \
    XS_TYPES_2_A(M, XS_TYPE_PTR, void *, ptr_args[0]);

#define XS_TYPES_2_A(M, E0, T0, A0) \
    M(E0, T0, A0, XS_TYPE_LONG, long long, long_args[1]); \
    M(E0, T0, A0, XS_TYPE_DOUBLE, double, double_args[1]); \
    M(E0, T0, A0, XS_TYPE_STRING, const char *, string_args[1]); \
    M(E0, T0, A0, XS_TYPE_PTR, void *, ptr_args[1]);

#define XS_TYPES_3(M) \
    XS_TYPES_3_A(M, XS_TYPE_LONG, long long, long_args[0]); \
    XS_TYPES_3_A(M, XS_TYPE_DOUBLE, double, double_args[0]); \
    XS_TYPES_3_A(M, XS_TYPE_STRING, const char *, string_args[0]); \
    XS_TYPES_3_A(M, XS_TYPE_PTR, void *, ptr_args[0]);

#define XS_TYPES_3_A(M, E0, T0, A0) \
    XS_TYPES_3_B(M, E0, T0, A0, XS_TYPE_LONG, long long, long_args[1]); \
    XS_TYPES_3_B(M, E0, T0, A0, XS_TYPE_DOUBLE, double, double_args[1]); \
    XS_TYPES_3_B(M, E0, T0, A0, XS_TYPE_STRING, const char *, string_args[1]);

#define XS_TYPES_3_B(M, E0, T0, A0, E1, T1, A1) \
    M(E0, T0, A0, E1, T1, A1, XS_TYPE_LONG, long long, long_args[2]); \
    M(E0, T0, A0, E1, T1, A1, XS_TYPE_DOUBLE, double, double_args[2]); \
    M(E0, T0, A0, E1, T1, A1, XS_TYPE_STRING, const char *, string_args[2]); \
    M(E0, T0, A0, E1, T1, A1, XS_TYPE_PTR, void *, ptr_args[2]);

#define XS_TYPES_4(M) \
    XS_TYPES_4_A(M, XS_TYPE_LONG, long long, long_args[0]); \
    XS_TYPES_4_A(M, XS_TYPE_DOUBLE, double, double_args[0]); \
    XS_TYPES_4_A(M, XS_TYPE_STRING, const char *, string_args[0]); \
    XS_TYPES_4_A(M, XS_TYPE_PTR, void *, ptr_args[0]);

#define XS_TYPES_4_A(M, E0, T0, A0) \
    XS_TYPES_4_B(M, E0, T0, A0, XS_TYPE_LONG, long long, long_args[1]); \
    XS_TYPES_4_B(M, E0, T0, A0, XS_TYPE_DOUBLE, double, double_args[1]); \
    XS_TYPES_4_B(M, E0, T0, A0, XS_TYPE_STRING, const char *, string_args[1]);

#define XS_TYPES_4_B(M, E0, T0, A0, E1, T1, A1) \
    XS_TYPES_4_C(M, E0, T0, A0, E1, T1, A1, XS_TYPE_LONG, long long, long_args[2]); \
    XS_TYPES_4_C(M, E0, T0, A0, E1, T1, A1, XS_TYPE_DOUBLE, double, double_args[2]); \
    XS_TYPES_4_C(M, E0, T0, A0, E1, T1, A1, XS_TYPE_STRING, const char *, string_args[2]);

#define XS_TYPES_4_C(M, E0, T0, A0, E1, T1, A1, E2, T2, A2) \
    M(E0, T0, A0, E1, T1, A1, E2, T2, A2, XS_TYPE_LONG, long long, long_args[3]); \
    M(E0, T0, A0, E1, T1, A1, E2, T2, A2, XS_TYPE_DOUBLE, double, double_args[3]); \
    M(E0, T0, A0, E1, T1, A1, E2, T2, A2, XS_TYPE_STRING, const char *, string_args[3]); \
    M(E0, T0, A0, E1, T1, A1, E2, T2, A2, XS_TYPE_PTR, void *, ptr_args[3]);

#define XS_CASE_VOID_1(E0, T0, A0) \
    case XS_CODE1(E0): ((void (*)(T0))sym)(A0); result = perl_alloc_undef(); break
#define XS_CASE_VOID_2(E0, T0, A0, E1, T1, A1) \
    case XS_CODE2(E0, E1): ((void (*)(T0, T1))sym)(A0, A1); result = perl_alloc_undef(); break
#define XS_CASE_VOID_3(E0, T0, A0, E1, T1, A1, E2, T2, A2) \
    case XS_CODE3(E0, E1, E2): ((void (*)(T0, T1, T2))sym)(A0, A1, A2); result = perl_alloc_undef(); break
#define XS_CASE_VOID_4(E0, T0, A0, E1, T1, A1, E2, T2, A2, E3, T3, A3) \
    case XS_CODE4(E0, E1, E2, E3): ((void (*)(T0, T1, T2, T3))sym)(A0, A1, A2, A3); result = perl_alloc_undef(); break

#define XS_CASE_LONG_1(E0, T0, A0) \
    case XS_CODE1(E0): result = perl_alloc_int(((long long (*)(T0))sym)(A0)); break
#define XS_CASE_LONG_2(E0, T0, A0, E1, T1, A1) \
    case XS_CODE2(E0, E1): result = perl_alloc_int(((long long (*)(T0, T1))sym)(A0, A1)); break
#define XS_CASE_LONG_3(E0, T0, A0, E1, T1, A1, E2, T2, A2) \
    case XS_CODE3(E0, E1, E2): result = perl_alloc_int(((long long (*)(T0, T1, T2))sym)(A0, A1, A2)); break
#define XS_CASE_LONG_4(E0, T0, A0, E1, T1, A1, E2, T2, A2, E3, T3, A3) \
    case XS_CODE4(E0, E1, E2, E3): result = perl_alloc_int(((long long (*)(T0, T1, T2, T3))sym)(A0, A1, A2, A3)); break

#define XS_CASE_DOUBLE_1(E0, T0, A0) \
    case XS_CODE1(E0): result = perl_alloc_float(((double (*)(T0))sym)(A0)); break
#define XS_CASE_DOUBLE_2(E0, T0, A0, E1, T1, A1) \
    case XS_CODE2(E0, E1): result = perl_alloc_float(((double (*)(T0, T1))sym)(A0, A1)); break
#define XS_CASE_DOUBLE_3(E0, T0, A0, E1, T1, A1, E2, T2, A2) \
    case XS_CODE3(E0, E1, E2): result = perl_alloc_float(((double (*)(T0, T1, T2))sym)(A0, A1, A2)); break
#define XS_CASE_DOUBLE_4(E0, T0, A0, E1, T1, A1, E2, T2, A2, E3, T3, A3) \
    case XS_CODE4(E0, E1, E2, E3): result = perl_alloc_float(((double (*)(T0, T1, T2, T3))sym)(A0, A1, A2, A3)); break

#define XS_CASE_STRING_1(E0, T0, A0) \
    case XS_CODE1(E0): do { const char *raw = ((const char *(*)(T0))sym)(A0); result = raw ? perl_alloc_string(raw) : perl_alloc_undef(); } while (0); break
#define XS_CASE_STRING_2(E0, T0, A0, E1, T1, A1) \
    case XS_CODE2(E0, E1): do { const char *raw = ((const char *(*)(T0, T1))sym)(A0, A1); result = raw ? perl_alloc_string(raw) : perl_alloc_undef(); } while (0); break
#define XS_CASE_STRING_3(E0, T0, A0, E1, T1, A1, E2, T2, A2) \
    case XS_CODE3(E0, E1, E2): do { const char *raw = ((const char *(*)(T0, T1, T2))sym)(A0, A1, A2); result = raw ? perl_alloc_string(raw) : perl_alloc_undef(); } while (0); break
#define XS_CASE_STRING_4(E0, T0, A0, E1, T1, A1, E2, T2, A2, E3, T3, A3) \
    case XS_CODE4(E0, E1, E2, E3): do { const char *raw = ((const char *(*)(T0, T1, T2, T3))sym)(A0, A1, A2, A3); result = raw ? perl_alloc_string(raw) : perl_alloc_undef(); } while (0); break

#define XS_CASE_PTR_1(E0, T0, A0) \
    case XS_CODE1(E0): do { void *raw = ((void *(*)(T0))sym)(A0); result = raw ? perl_alloc_xs_ptr(raw) : perl_alloc_undef(); } while (0); break
#define XS_CASE_PTR_2(E0, T0, A0, E1, T1, A1) \
    case XS_CODE2(E0, E1): do { void *raw = ((void *(*)(T0, T1))sym)(A0, A1); result = raw ? perl_alloc_xs_ptr(raw) : perl_alloc_undef(); } while (0); break
#define XS_CASE_PTR_3(E0, T0, A0, E1, T1, A1, E2, T2, A2) \
    case XS_CODE3(E0, E1, E2): do { void *raw = ((void *(*)(T0, T1, T2))sym)(A0, A1, A2); result = raw ? perl_alloc_xs_ptr(raw) : perl_alloc_undef(); } while (0); break
#define XS_CASE_PTR_4(E0, T0, A0, E1, T1, A1, E2, T2, A2, E3, T3, A3) \
    case XS_CODE4(E0, E1, E2, E3): do { void *raw = ((void *(*)(T0, T1, T2, T3))sym)(A0, A1, A2, A3); result = raw ? perl_alloc_xs_ptr(raw) : perl_alloc_undef(); } while (0); break

PerlValue *perl_xs_call_dynamic(PerlValue *libname_pv, PerlValue *funcname_pv,
                                PerlValue *signature_pv, PerlArray *args) {
    char *libname = NULL;
    char *funcname = NULL;
    char *signature = NULL;
    PerlXSSignature parsed;
    int sig_code;
    PerlXSModuleInfo *module;
    void *sym;
    long long long_args[4] = {0, 0, 0, 0};
    double double_args[4] = {0.0, 0.0, 0.0, 0.0};
    char *string_args[4] = {NULL, NULL, NULL, NULL};
    void *ptr_args[4] = {NULL, NULL, NULL, NULL};
    PerlValue *result = NULL;
    PerlValue empty = { .tag = PERL_UNDEF };

    if (!libname_pv || !funcname_pv || !signature_pv) {
        perl_xs_set_error("XS::call: expected library, symbol, and signature");
        return perl_alloc_undef();
    }

    libname = perl_to_string_dup(libname_pv);
    funcname = perl_to_string_dup(funcname_pv);
    signature = perl_to_string_dup(signature_pv);
    if (!libname || !funcname || !signature) {
        perl_xs_set_error("XS::call: invalid arguments");
        goto fail;
    }

    if (!perl_xs_parse_signature(signature, &parsed)) {
        perl_xs_set_error("XS::call: unsupported signature");
        goto fail;
    }

    if ((args ? args->len : 0) != parsed.arg_count) {
        perl_xs_set_error("XS::call: argument count does not match signature");
        goto fail;
    }
    sig_code = perl_xs_signature_code(&parsed);

    module = perl_xs_find_or_load_module(libname);
    if (!module) goto fail;

    dlerror();
    sym = dlsym(module->handle, funcname);
    if (!sym) {
        const char *errmsg = dlerror();
        perl_xs_set_error(errmsg ? errmsg : "XS::call: symbol lookup failed");
        goto fail;
    }

    for (int i = 0; i < parsed.arg_count; i++) {
        PerlValue *arg = args->elems[i];
        switch (parsed.arg_types[i]) {
            case XS_TYPE_LONG:
                long_args[i] = perl_to_int(arg);
                break;
            case XS_TYPE_DOUBLE:
                double_args[i] = perl_to_float(arg);
                break;
            case XS_TYPE_STRING:
                string_args[i] = perl_to_string_dup(arg);
                if (!string_args[i]) {
                    perl_xs_set_error("XS::call: failed to convert string argument");
                    goto fail;
                }
                break;
            case XS_TYPE_PTR:
                if (!arg || arg->tag == PERL_UNDEF) {
                    ptr_args[i] = NULL;
                } else if (arg->tag == PERL_XS_PTR) {
                    ptr_args[i] = arg->pval;
                } else if (arg->tag == PERL_STRING) {
                    string_args[i] = perl_to_string_dup(arg);
                    if (!string_args[i]) {
                        perl_xs_set_error("XS::call: failed to convert pointer argument");
                        goto fail;
                    }
                    ptr_args[i] = string_args[i];
                } else {
                    ptr_args[i] = (void *)(uintptr_t)perl_to_int(arg);
                }
                break;
            default:
                perl_xs_set_error("XS::call: unsupported argument type");
                goto fail;
        }
    }

    switch (parsed.ret_type) {
        case XS_TYPE_VOID:
            if (parsed.arg_count == 0) {
                ((void (*)(void))sym)();
                result = perl_alloc_undef();
            } else switch (sig_code) {
                XS_TYPES_1(XS_CASE_VOID_1);
                XS_TYPES_2(XS_CASE_VOID_2);
                XS_TYPES_3(XS_CASE_VOID_3);
                XS_TYPES_4(XS_CASE_VOID_4);
                default:
                    perl_xs_set_error("XS::call: unsupported void signature");
                    goto fail;
            }
            break;
        case XS_TYPE_LONG:
            if (parsed.arg_count == 0) result = perl_alloc_int(((long long (*)(void))sym)());
            else switch (sig_code) {
                XS_TYPES_1(XS_CASE_LONG_1);
                XS_TYPES_2(XS_CASE_LONG_2);
                XS_TYPES_3(XS_CASE_LONG_3);
                XS_TYPES_4(XS_CASE_LONG_4);
                default:
                    perl_xs_set_error("XS::call: unsupported long signature");
                    goto fail;
            }
            break;
        case XS_TYPE_DOUBLE:
            if (parsed.arg_count == 0) result = perl_alloc_float(((double (*)(void))sym)());
            else switch (sig_code) {
                XS_TYPES_1(XS_CASE_DOUBLE_1);
                XS_TYPES_2(XS_CASE_DOUBLE_2);
                XS_TYPES_3(XS_CASE_DOUBLE_3);
                XS_TYPES_4(XS_CASE_DOUBLE_4);
                default:
                    perl_xs_set_error("XS::call: unsupported double signature");
                    goto fail;
            }
            break;
        case XS_TYPE_STRING:
            if (parsed.arg_count == 0) {
                const char *raw = ((const char *(*)(void))sym)();
                result = raw ? perl_alloc_string(raw) : perl_alloc_undef();
            } else switch (sig_code) {
                XS_TYPES_1(XS_CASE_STRING_1);
                XS_TYPES_2(XS_CASE_STRING_2);
                XS_TYPES_3(XS_CASE_STRING_3);
                XS_TYPES_4(XS_CASE_STRING_4);
                default:
                    perl_xs_set_error("XS::call: unsupported string signature");
                    goto fail;
            }
            break;
        case XS_TYPE_PTR:
            if (parsed.arg_count == 0) {
                void *raw = ((void *(*)(void))sym)();
                result = raw ? perl_alloc_xs_ptr(raw) : perl_alloc_undef();
            } else switch (sig_code) {
                XS_TYPES_1(XS_CASE_PTR_1);
                XS_TYPES_2(XS_CASE_PTR_2);
                XS_TYPES_3(XS_CASE_PTR_3);
                XS_TYPES_4(XS_CASE_PTR_4);
                default:
                    perl_xs_set_error("XS::call: unsupported ptr signature");
                    goto fail;
            }
            break;
        default:
            perl_xs_set_error("XS::call: unsupported return type");
            goto fail;
    }

    perl_assign(&s_dollar_at, &empty);
    goto done;

fail:
    if (!result) result = perl_alloc_undef();

done:
    for (int i = 0; i < 4; i++) {
        if (string_args[i]) free(string_args[i]);
    }
    if (libname) free(libname);
    if (funcname) free(funcname);
    if (signature) free(signature);
    return result;
}

void perl_xs_cleanup(void) {
    PerlXSModuleInfo *module = s_xs_module_list;
    while (module) {
        PerlXSModuleInfo *next = module->next;
        if (module->handle) dlclose(module->handle);
        if (module->name) free(module->name);
        free(module);
        module = next;
    }
    s_xs_module_list = NULL;
}

/* ── program cleanup ────────────────────────────────────────────────────────
 * Free all program-lifetime runtime state so valgrind reports zero leaks.
 * Called at exit via atexit() from main.cpp.                                                    */
void perl_cleanup(void) {
    /* 1. Shared-mutex side-table: destroy mutex/condvar, free entries and
     *    the SharedMutex structs they point to. */
    for (int b = 0; b < SHARED_MUTEX_TABLE_BUCKETS; b++) {
        SharedMutexEntry *e = s_mutex_table[b];
        while (e) {
            SharedMutexEntry *next = e->next;
            SharedMutex *mu = e->mu;
            if (mu) {
                pthread_mutex_destroy(&mu->mu);
                pthread_cond_destroy(&mu->cond);
                free(mu);
            }
            free(e);
            e = next;
        }
        s_mutex_table[b] = NULL;
    }

    /* 2. Named-captures hash ($+) — may still hold the last match. */
    if (perl_plus_hash) {
        perl_hash_free(perl_plus_hash);
        perl_plus_hash = NULL;
    }

    /* 3. XS module list (reload of existing cleanup). */
    perl_xs_cleanup();

    /* 3b. do-lib list (D24: dlopen()ed `do FILE` shared libraries). */
    perl_do_lib_cleanup();

    /* 4. PV slabs: free all allocated slabs so valgrind reports zero leaks.
     * The PVs inside the slabs are either in the freelist or in use, but
     * at exit we just free the slab memory itself (calloc'd blocks). */
    for (int s = 0; s < pv_slab_count_; s++) {
        free(pv_slabs_[s]);
    }
    pv_slab_count_ = 0;

    /* 5. Per-thread PCRE2 compiled-pattern cache (main thread only). */
    for (int i = 0; i < regex_cache_len_; i++) {
        if (regex_cache_[i].compiled) {
            pcre2_code_free(regex_cache_[i].compiled);
            regex_cache_[i].compiled = NULL;
            regex_cache_[i].key[0] = '\0';
        }
    }
    regex_cache_len_ = 0;

#ifdef PERL_ALLOC_DEBUG
    /* 4. PV leak check: iterate all slabs, report PVs with sentinel set. */
    int leak_count = 0;
    for (int s = 0; s < pv_slab_count_; s++) {
        PerlValue *slab = pv_slabs_[s];
        for (int i = 0; i < PV_SLAB; i++) {
            if (slab[i].ival == PV_LEAK_SENTINEL) {
                if (leak_count < 100)
                    fprintf(stderr, "perlc PV leak: slab %d offset %d tag=%d\n",
                            s, i, (int)slab[i].tag);
                leak_count++;
            }
        }
    }
    if (leak_count > 0) {
        if (leak_count > 100)
            fprintf(stderr, "perlc PV leak: ... and %d more\n", leak_count - 100);
        fprintf(stderr, "perlc PV leak: total %d unfreed PVs (compile with -DPERL_ALLOC_DEBUG)\n", leak_count);
    }
#endif
}

/* ── DBI/SQLite integration ────────────────────────────────────────────────── */

/* SQLite integration - DBI-like interface */
#include <sqlite3.h>

static void perl_dbi_set_error(PerlDBIHandle *dbh, const char *msg) {
    if (!dbh) return;
    if (dbh->last_error) free(dbh->last_error);
    dbh->last_error = strdup(msg ? msg : "");
}

static void perl_dbi_stmt_set_error(PerlDBIStatement *sth, const char *msg) {
    if (!sth) return;
    if (sth->last_error) free(sth->last_error);
    sth->last_error = strdup(msg ? msg : "");
    if (sth->dbh) perl_dbi_set_error(sth->dbh, msg);
}

static const char *perl_dbi_sqlite_path(const char *dsn) {
    static const char prefix[] = "dbi:SQLite:dbname=";
    return (dsn && strncmp(dsn, prefix, sizeof(prefix) - 1) == 0)
        ? dsn + sizeof(prefix) - 1
        : dsn;
}

static PerlValue *perl_dbi_wrap_dbh(PerlDBIHandle *dbh) {
    PerlValue *pv = pv_alloc();
    pv->tag = PERL_DBI_DBH;
    pv->flags = 0;
    pv->pval = dbh;
    pv->matchpos = 0;
    pv->blessed_class = strdup("DBI::db");
    return pv;
}

static PerlValue *perl_dbi_wrap_sth(PerlDBIStatement *sth) {
    PerlValue *pv = pv_alloc();
    pv->tag = PERL_DBI_STH;
    pv->flags = 0;
    pv->pval = sth;
    pv->matchpos = 0;
    pv->blessed_class = strdup("DBI::st");
    return pv;
}

static PerlDBIHandle *perl_dbi_get_dbh(PerlValue *pv) {
    return (pv && pv->tag == PERL_DBI_DBH) ? (PerlDBIHandle *)pv->pval : NULL;
}

static PerlDBIStatement *perl_dbi_get_sth(PerlValue *pv) {
    return (pv && pv->tag == PERL_DBI_STH) ? (PerlDBIStatement *)pv->pval : NULL;
}

static void perl_dbi_handle_release(PerlDBIHandle *dbh) {
    if (!dbh) return;
    if (--dbh->refcount > 0) return;
    if (dbh->db) sqlite3_close((sqlite3 *)dbh->db);
    if (dbh->dbname) free(dbh->dbname);
    if (dbh->last_error) free(dbh->last_error);
    free(dbh);
}

static void perl_dbi_statement_release(PerlDBIStatement *sth) {
    if (!sth) return;
    if (--sth->refcount > 0) return;
    if (sth->stmt) sqlite3_finalize((sqlite3_stmt *)sth->stmt);
    if (sth->dbh) perl_dbi_handle_release(sth->dbh);
    if (sth->last_error) free(sth->last_error);
    free(sth);
}

static int perl_dbi_bind_params(sqlite3_stmt *stmt, PerlArray *params) {
    long long i;
    if (!params) return SQLITE_OK;
    for (i = 0; i < params->len; i++) {
        PerlValue *pv = params->elems[i];
        int idx = (int)i + 1;
        int rc = SQLITE_OK;
        if (!pv || pv->tag == PERL_UNDEF) {
            rc = sqlite3_bind_null(stmt, idx);
        } else if (pv->tag == PERL_INT) {
            rc = sqlite3_bind_int64(stmt, idx, pv->ival);
        } else if (pv->tag == PERL_FLOAT) {
            rc = sqlite3_bind_double(stmt, idx, pv->fval);
        } else {
            char *s = perl_to_string_dup(pv);
            rc = sqlite3_bind_text(stmt, idx, s, -1, SQLITE_TRANSIENT);
            free(s);
        }
        if (rc != SQLITE_OK) return rc;
    }
    return SQLITE_OK;
}

static PerlValue *perl_dbi_column_value(sqlite3_stmt *stmt, int col) {
    switch (sqlite3_column_type(stmt, col)) {
        case SQLITE_INTEGER:
            return perl_alloc_int(sqlite3_column_int64(stmt, col));
        case SQLITE_FLOAT:
            return perl_alloc_float(sqlite3_column_double(stmt, col));
        case SQLITE_TEXT:
            return perl_alloc_string((const char *)sqlite3_column_text(stmt, col));
        case SQLITE_NULL:
            return perl_alloc_undef();
        default:
            return perl_alloc_string((const char *)sqlite3_column_text(stmt, col));
    }
}

static PerlArray *perl_dbi_fetch_row(sqlite3_stmt *stmt) {
    int cols = sqlite3_column_count(stmt);
    int i;
    PerlArray *row = perl_array_new();
    for (i = 0; i < cols; i++) {
        PerlValue *pv = perl_dbi_column_value(stmt, i);
        perl_array_push(row, pv);
        perl_free(pv);
    }
    return row;
}

/* DBI connection - returns a reference to a database handle */
PerlValue *perl_dbi_connect(PerlValue *dsn_pv, PerlValue *username_pv, PerlValue *password_pv) {
    char *dsn;
    const char *path;
    sqlite3 *db = NULL;
    PerlDBIHandle *handle;
    int rc;
    (void)username_pv;
    (void)password_pv;

    if (!dsn_pv || dsn_pv->tag == PERL_UNDEF) return perl_alloc_undef();
    dsn = perl_to_string_dup(dsn_pv);
    if (!dsn) return perl_alloc_undef();
    path = perl_dbi_sqlite_path(dsn);

    rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        perl_set_dollar_at_cstr(sqlite3_errmsg(db));
        sqlite3_close(db);
        free(dsn);
        return perl_alloc_undef();
    }

    handle = calloc(1, sizeof(PerlDBIHandle));
    handle->db = db;
    handle->dbname = strdup(path ? path : "");
    handle->is_connected = 1;
    handle->refcount = 1;
    handle->last_error = strdup("");
    free(dsn);
    return perl_dbi_wrap_dbh(handle);
}

/* Disconnect from database */
PerlValue *perl_dbi_disconnect(PerlValue *dbh_pv) {
    PerlDBIHandle *dbh = perl_dbi_get_dbh(dbh_pv);
    if (!dbh) return perl_alloc_undef();
    if (dbh->db) {
        sqlite3_close((sqlite3 *)dbh->db);
        dbh->db = NULL;
    }
    dbh->is_connected = 0;
    return perl_alloc_int(1);
}

/* Prepare SQL statement */
PerlValue *perl_dbi_prepare(PerlValue *dbh_pv, PerlValue *sql_pv) {
    PerlDBIHandle *dbh = perl_dbi_get_dbh(dbh_pv);
    PerlDBIStatement *sth;
    sqlite3_stmt *stmt = NULL;
    char *sql;
    int rc;

    if (!dbh || !sql_pv || sql_pv->tag == PERL_UNDEF) return perl_alloc_undef();
    sql = perl_to_string_dup(sql_pv);
    rc = sqlite3_prepare_v2((sqlite3 *)dbh->db, sql, -1, &stmt, NULL);
    free(sql);
    if (rc != SQLITE_OK) {
        perl_dbi_set_error(dbh, sqlite3_errmsg((sqlite3 *)dbh->db));
        return perl_alloc_undef();
    }

    sth = calloc(1, sizeof(PerlDBIStatement));
    sth->stmt = stmt;
    sth->dbh = dbh;
    sth->done = 0;
    sth->rows_affected = 0;
    sth->refcount = 1;
    sth->last_error = strdup("");
    dbh->refcount++;
    return perl_dbi_wrap_sth(sth);
}

/* Execute SQL statement */
PerlValue *perl_dbi_execute(PerlValue *sth_pv, PerlArray *params) {
    PerlDBIStatement *sth = perl_dbi_get_sth(sth_pv);
    sqlite3_stmt *stmt;
    int rc;
    if (!sth) return perl_alloc_undef();
    stmt = (sqlite3_stmt *)sth->stmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    rc = perl_dbi_bind_params(stmt, params);
    if (rc != SQLITE_OK) {
        perl_dbi_stmt_set_error(sth, sqlite3_errmsg((sqlite3 *)sth->dbh->db));
        return perl_alloc_undef();
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        sth->done = 0;
        return perl_alloc_int(1);
    }
    if (rc == SQLITE_DONE) {
        sth->done = 1;
        sth->rows_affected = sqlite3_changes((sqlite3 *)sth->dbh->db);
        return perl_alloc_int(sth->rows_affected >= 0 ? sth->rows_affected : 1);
    }
    perl_dbi_stmt_set_error(sth, sqlite3_errmsg((sqlite3 *)sth->dbh->db));
    return perl_alloc_undef();
}

/* Fetch row as array ref */
PerlValue *perl_dbi_fetch(PerlValue *sth_pv) {
    PerlDBIStatement *sth = perl_dbi_get_sth(sth_pv);
    sqlite3_stmt *stmt;
    int rc;
    PerlArray *row;
    if (!sth) return perl_alloc_undef();
    stmt = (sqlite3_stmt *)sth->stmt;
    rc = sqlite3_data_count(stmt) > 0 ? SQLITE_ROW : sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        sth->done = 1;
        return perl_alloc_undef();
    }
    if (rc != SQLITE_ROW) {
        perl_dbi_stmt_set_error(sth, sqlite3_errmsg((sqlite3 *)sth->dbh->db));
        return perl_alloc_undef();
    }
    row = perl_dbi_fetch_row(stmt);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) sth->done = 1;
    return perl_ref_array(row);
}

/* Fetch all rows as array-ref of array-refs */
PerlValue *perl_dbi_fetchall(PerlValue *sth_pv) {
    PerlDBIStatement *sth = perl_dbi_get_sth(sth_pv);
    sqlite3_stmt *stmt;
    PerlArray *rows;
    int rc;
    if (!sth) return perl_alloc_undef();
    stmt = (sqlite3_stmt *)sth->stmt;
    rows = perl_array_new();
    rc = sqlite3_data_count(stmt) > 0 ? SQLITE_ROW : sqlite3_step(stmt);
    while (rc == SQLITE_ROW) {
        PerlArray *row = perl_dbi_fetch_row(stmt);
        PerlValue *row_ref = perl_ref_array(row);
        perl_array_push(rows, row_ref);
        perl_free(row_ref);
        rc = sqlite3_step(stmt);
    }
    if (rc == SQLITE_DONE) sth->done = 1;
    if (rc != SQLITE_DONE) {
        perl_array_free(rows);
        perl_dbi_stmt_set_error(sth, sqlite3_errmsg((sqlite3 *)sth->dbh->db));
        return perl_alloc_undef();
    }
    return perl_ref_array(rows);
}

/* Get number of rows affected */
PerlValue *perl_dbi_rows(PerlValue *sth_pv) {
    PerlDBIStatement *sth = perl_dbi_get_sth(sth_pv);
    if (!sth) return perl_alloc_undef();
    return perl_alloc_int(sth->rows_affected);
}

/* Commit transaction */
PerlValue *perl_dbi_commit(PerlValue *dbh_pv) {
    PerlDBIHandle *dbh = perl_dbi_get_dbh(dbh_pv);
    char *errmsg = NULL;
    int rc;
    if (!dbh) return perl_alloc_undef();
    rc = sqlite3_exec((sqlite3 *)dbh->db, "COMMIT", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        perl_dbi_set_error(dbh, errmsg ? errmsg : "commit failed");
        sqlite3_free(errmsg);
        return perl_alloc_undef();
    }
    return perl_alloc_int(1);
}

/* Rollback transaction */
PerlValue *perl_dbi_rollback(PerlValue *dbh_pv) {
    PerlDBIHandle *dbh = perl_dbi_get_dbh(dbh_pv);
    char *errmsg = NULL;
    int rc;
    if (!dbh) return perl_alloc_undef();
    rc = sqlite3_exec((sqlite3 *)dbh->db, "ROLLBACK", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        perl_dbi_set_error(dbh, errmsg ? errmsg : "rollback failed");
        sqlite3_free(errmsg);
        return perl_alloc_undef();
    }
    return perl_alloc_int(1);
}

/* Get error information */
PerlValue *perl_dbi_error(PerlValue *dbh_pv) {
    PerlDBIHandle *dbh = perl_dbi_get_dbh(dbh_pv);
    if (!dbh) return perl_alloc_undef();
    return perl_alloc_string(dbh->last_error ? dbh->last_error : "");
}
