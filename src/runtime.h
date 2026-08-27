#ifndef PERLC_RUNTIME_H
#define PERLC_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

/* Perl scalar: tagged union holding undef / integer / float / string */
typedef enum {
    PERL_UNDEF      = 0,
    PERL_INT        = 1,
    PERL_FLOAT      = 2,
    PERL_STRING     = 3,
    PERL_REF_SCALAR = 4,
    PERL_REF_ARRAY  = 5,
    PERL_REF_HASH   = 6,
    PERL_FILEHANDLE = 7,
    PERL_CODE_REF   = 8,
    PERL_DIRHANDLE  = 9,
    PERL_FLAT_ARRAY   = 10, /* flat double[] — pval=double*, matchpos=len */
    PERL_THREAD       = 11, /* thread object  — pval=PerlThread* */
    PERL_LIST_RESULT  = 12, /* list-context sub return — pval=PerlArray*; spread by perl_unwrap_list_return */
    PERL_FLOAT_PAIR   = 13, /* 2-elem float array inline: fval=elem[0], matchpos bits=elem[1]; no inner PerlArray */
    PERL_DBI_DBH      = 14, /* DBI database handle — pval=PerlDBIHandle* */
    PERL_DBI_STH      = 15, /* DBI statement handle — pval=PerlDBIStatement* */
    PERL_XS_PTR       = 16, /* opaque native pointer — pval=void* */
    PERL_BIGINT       = 17, /* Math::BigInt — pval=mpz_t* (GMP or mini-gmp), blessed_class="Math::BigInt" */
} PerlTag;

/* PV_FLAG_SHARED: cell is a threads::shared variable (see SharedMutex below). */
#define PV_FLAG_SHARED 1u

/* D62: closure-capture reference counting, packed into spare `flags` bits
   (no PerlValue struct growth — see the size/alignment note on `flags`
   below). A PV captured by a closure/sort-comparator is no longer cloned;
   instead its capture count is bumped, and perl_free() defers the real
   free until every capturing closure AND the declaring scope have both
   released their share. PV_FLAG_CAPTURE_RELEASED records that the
   declaring scope's own perl_free() call already happened while captures
   were still outstanding, so the capture-side release that brings the
   count to 0 knows it's safe to actually free. See perl_array_push_capture/
   perl_free/perl_release_capture/perl_closure_release in runtime.c. */
#define PV_FLAG_CAPTURE_RELEASED  2u
#define PV_CAPTURE_SHIFT          2
#define PV_CAPTURE_BITS           20
#define PV_CAPTURE_MASK   (((1u << PV_CAPTURE_BITS) - 1) << PV_CAPTURE_SHIFT)
#define PV_CAPTURE_MAX     ((1u << PV_CAPTURE_BITS) - 1)   /* pin-forever sentinel on overflow */
/* D90: string holds Unicode characters (UTF-8 encoded). length()/substr
   count code points when set; otherwise they count raw bytes (binary /
   pack output, byte strings). Bits 0-21 used by shared/capture above. */
#define PV_FLAG_UTF8              (1u << 22)

typedef struct PerlValue {
    PerlTag      tag;
    unsigned int flags;       /* PV_FLAG_SHARED etc.; fits existing padding slot */
    union {
        long long ival;
        double    fval;
        char     *sval;   /* heap-allocated; NUL-terminated for legacy/strlen()-
                              based consumers, but may contain embedded NUL
                              bytes before that terminator — `slen` below is
                              the authoritative byte length (see D85). */
        void     *pval;   /* for reference types */
    };
    long long matchpos;      /* current /g match offset; 0 = start of string */
    char     *blessed_class; /* NULL unless bless'd */
    /* D85: PerlValue previously had no explicit length field, so any string
       containing an embedded NUL byte (e.g. pack("N", 1234567), a common
       4-byte network-order pack) was silently truncated wherever the
       runtime derived a string's length via strlen() instead of tracking
       it explicitly — which was nearly everywhere. `slen` is the
       authoritative byte length of `sval` for PERL_STRING-tagged values
       (meaningless for other tags); `sval[slen]` is still always a NUL
       terminator for backward compatibility with any remaining
       strlen()-based consumer, but bytes *before* that position may
       legitimately include 0x00. `_pad_reserved` exists solely to keep
       sizeof(PerlValue) a multiple of 16 — the lock-free 16-byte CAS
       (cmpxchg16b/ldxp+stxp, see perl_atomic_* and PerlValueAtomic16)
       requires every slab-allocated PerlValue to stay 16-byte aligned,
       which only holds if the struct's own size is a 16-byte multiple. */
    long long slen;
    long long _pad_reserved;
} PerlValue;

/* PV_FLAG_SHARED: variable is a threads::shared variable.  With the new
   layout (Phase 2 of THREADS_SHARED_ATOMIC_PLAN.md) the cell *is* the
   PerlValue; the SharedMutex is allocated lazily on first lock()/cond_wait()
   and kept in a process-wide side-table keyed by the cell address.  The
   old `PerlSharedVar` wrapper has been removed — see git log for the
   one-shot ABI break that landed with this change. */
#include <pthread.h>
#include <stdatomic.h>

typedef struct SharedMutex {
    pthread_mutex_t mu;
    pthread_cond_t  cond;
} SharedMutex;

/* allocation */
PerlValue *perl_alloc_undef(void);
PerlValue *perl_alloc_int(long long v);
PerlValue *perl_alloc_float(double v);

PerlValue *perl_alloc_string(const char *s);
PerlValue *perl_alloc_string_len(const char *s, long long len); /* D85: NUL-safe — s may contain embedded NUL bytes within the first len */
PerlValue *perl_alloc_bool(long long b); /* W1: "1" (IV) or "" — real Perl comparison-true/false values */
char      *perl_to_string_dup_len(const PerlValue *v, long long *out_len); /* D85: NUL-safe perl_to_string_dup, also reports true byte length */
PerlValue *perl_alloc_flat_array(long long n); /* alloc PV with pval=double[n] */
PerlValue *perl_alloc_float_array(long long n); /* alloc FLAT_ARRAY with n zero doubles */
PerlValue *perl_alloc_float_pair(double re, double im); /* PERL_FLOAT_PAIR: inline 2-float */
PerlValue *perl_alloc_xs_ptr(void *p);
PerlValue *perl_clone(const PerlValue *v);
void       perl_free(PerlValue *v);
PerlValue *perl_make_shared_scalar(void); /* threads::shared — alloc bare PV with PV_FLAG_SHARED, no mutex yet */

/* threads::shared — lock/cond */
void perl_lock_shared(PerlValue *pv);          /* lock scalar shared var + push auto-unlock */
void perl_unlock_shared(PerlValue *pv);        /* explicit unlock */
int  perl_cond_timedwait(PerlValue *pv, long long timeout_ms); /* timed wait */
void perl_cond_broadcast_shared(PerlValue *pv); /* broadcast to all */
void perl_cond_wait(PerlValue *pv);            /* cond_wait on shared var's condvar */
void perl_cond_signal(PerlValue *pv);          /* cond_signal */
void perl_cond_broadcast(PerlValue *pv);       /* cond_broadcast */

/* atomic operations for threads::shared */
PerlValue *perl_atomic_load(PerlValue *pv);
PerlValue *perl_atomic_store(PerlValue *pv, PerlValue *v);
PerlValue *perl_atomic_swap(PerlValue *pv, PerlValue *v);
PerlValue *perl_atomic_inc(PerlValue *pv);
PerlValue *perl_atomic_dec(PerlValue *pv);
PerlValue *perl_atomic_add(PerlValue *pv, PerlValue *delta);
PerlValue *perl_atomic_rmw(PerlValue *pv, PerlValue *rhs, int op);  /* *, /, % */

 /* tie/untie */
 PerlValue *perl_tie(PerlValue *args_arr);
 void perl_untie(PerlValue *var_pv);

 /* coercions */
long long  perl_to_int(const PerlValue *v);
double     perl_to_float(const PerlValue *v);
const char *perl_to_string(const PerlValue *v);   /* stable for PERL_STRING/undef, heap for others */
char       *perl_to_string_dup(const PerlValue *v); /* always heap-allocated (caller must free) */
int        perl_defined(const PerlValue *v);
int        perl_is_true(const PerlValue *v);

/* assignment: dst = src  (manages dst's old string if any) */
void perl_assign(PerlValue *dst, const PerlValue *src);

/* arithmetic */
PerlValue *perl_add(const PerlValue *a, const PerlValue *b);
PerlValue *perl_sub(const PerlValue *a, const PerlValue *b);
PerlValue *perl_mul(const PerlValue *a, const PerlValue *b);
PerlValue *perl_div(const PerlValue *a, const PerlValue *b);
PerlValue *perl_mod(const PerlValue *a, const PerlValue *b);
/* D84: i64 fast-path modulo with eval-catchable zero-divisor check */
long long perl_mod_i64(long long a, long long b);
PerlValue *perl_pow(const PerlValue *a, const PerlValue *b);
PerlValue *perl_negate(const PerlValue *a);
PerlValue *perl_bitand(const PerlValue *a, const PerlValue *b);
PerlValue *perl_bitor(const PerlValue *a, const PerlValue *b);
PerlValue *perl_bitxor(const PerlValue *a, const PerlValue *b);
PerlValue *perl_bitnot(const PerlValue *a);
PerlValue *perl_lshift(const PerlValue *a, const PerlValue *b);
PerlValue *perl_rshift(const PerlValue *a, const PerlValue *b);

/* string ops */
PerlValue *perl_concat(const PerlValue *a, const PerlValue *b);
PerlValue *perl_repeat_str(const PerlValue *str, const PerlValue *n);

/* comparison – return PerlValue int 1/0 */
PerlValue *perl_num_eq(const PerlValue *a, const PerlValue *b);
PerlValue *perl_num_ne(const PerlValue *a, const PerlValue *b);
PerlValue *perl_num_lt(const PerlValue *a, const PerlValue *b);
PerlValue *perl_num_gt(const PerlValue *a, const PerlValue *b);
PerlValue *perl_num_le(const PerlValue *a, const PerlValue *b);
PerlValue *perl_num_ge(const PerlValue *a, const PerlValue *b);
PerlValue *perl_str_eq(const PerlValue *a, const PerlValue *b);
PerlValue *perl_str_ne(const PerlValue *a, const PerlValue *b);
PerlValue *perl_str_lt(const PerlValue *a, const PerlValue *b);
PerlValue *perl_str_gt(const PerlValue *a, const PerlValue *b);
PerlValue *perl_str_le(const PerlValue *a, const PerlValue *b);
PerlValue *perl_str_ge(const PerlValue *a, const PerlValue *b);

/* logical */
PerlValue *perl_not(const PerlValue *a);
PerlValue *perl_and(const PerlValue *a, const PerlValue *b);
PerlValue *perl_or(const PerlValue *a, const PerlValue *b);

/* I/O */
void perl_print(const PerlValue *v);
void perl_say(const PerlValue *v);
void perl_print_string(const char *s);
/* wantarray context peek */
int perl_current_wantarray_ctx(void);

/* increment / decrement (in-place, returns v) */
PerlValue *perl_inc(PerlValue *v);
PerlValue *perl_dec(PerlValue *v);

/* array support */
typedef struct PerlArray {
    PerlValue      **elems;
    long long        len;
    long long        cap;
    int              refcount; /* 0 = scope-managed (named @arr), >0 = anonymous refcounted ([]) */
    pthread_mutex_t *mu;       /* non-NULL when declared : shared */
} PerlArray;

/* list-context return helpers for wantarray */
PerlValue *perl_array_to_list_return(PerlArray *av); /* list ctx→LIST_RESULT; scalar→last elem */
PerlValue *perl_array_to_count_or_list(PerlArray *av); /* map/grep: list→LIST_RESULT; scalar→count */
PerlValue *perl_array_to_sort_return(PerlArray *av);   /* sort: list→LIST_RESULT; scalar→undef */
PerlArray *perl_unwrap_list_return(PerlValue *pv);   /* caller side: extract array from return */

PerlArray *perl_array_new(void);
PerlArray *perl_anon_array_new(void); /* like perl_array_new but refcount=1 (anonymous) */
long long perl_array_is_all_flat(PerlArray *av); /* 1 if all elems are FLAT_ARRAY */
void       perl_array_free(PerlArray *a);
void       perl_array_push_nc(PerlArray *a, PerlValue *v);  /* no-clone push; caller owns v */
void       perl_array_free_nc(PerlArray *a);                /* free array without freeing elements */
void       perl_array_make_shared(PerlArray *a); /* threads::shared: init mu */
void       perl_lock_array(PerlArray *a);        /* lock + push auto-unlock */
void       perl_array_push(PerlArray *a, PerlValue *v);
void       perl_array_push_capture(PerlArray *a, PerlValue *v);
PerlValue *perl_array_pop(PerlArray *a);
PerlValue *perl_array_get(PerlArray *a, long long idx);
PerlValue *perl_array_get_ref(PerlArray *a, long long idx); /* borrow: no clone, never free result */
void       perl_array_set(PerlArray *a, long long idx, PerlValue *v);
/* D81: delete $a[i] / element of delete @a[...] — returns prior value,
   sets slot to undef, then trims trailing undefs (Perl's delete-on-array). */
PerlValue *perl_array_delete(PerlArray *a, long long idx);
/* D41: writable slot for $a[i], extending with undef if needed (local lvalue). */
PerlValue *perl_array_lvalue(PerlArray *a, long long idx);
PerlValue *perl_array_len(PerlArray *a);
double perl_array_len_f64(PerlArray *a);
void perl_array_clear(PerlArray *a);
void perl_array_replace(PerlArray *dst, PerlArray *src);
PerlArray *perl_repeat_list(PerlArray *src, PerlValue *n);
void       perl_array_sort_str(PerlArray *a);
void       perl_array_extend(PerlArray *dst, PerlArray *src);
void       perl_array_extend_from(PerlArray *dst, PerlArray *src, long long start); /* dst += src[start..] */
PerlValue *perl_array_shift(PerlArray *a);
void       perl_array_unshift(PerlArray *a, PerlValue *v);
void       perl_print_array(PerlArray *a); /* print all elements with $, between them */

/* ── string builtins ─────────────────────────────────────────────────────── */
long long  perl_chomp(PerlValue *v);       /* remove trailing \n in-place, returns removed count */
long long  perl_chomp_array(PerlArray *a); /* chomp every element; returns total removed count */
PerlValue *perl_chop(PerlValue *v);        /* remove and return last character */
PerlValue *perl_chop_array(PerlArray *a);  /* chop every element; returns last removed char */
PerlValue *perl_length(PerlValue *v);
PerlValue *perl_substr2(PerlValue *str, PerlValue *off);
void perl_substr_replace(PerlValue *str, PerlValue *off, PerlValue *len, PerlValue *repl);
PerlValue *perl_substr3(PerlValue *str, PerlValue *off, PerlValue *len);
PerlValue *perl_join(PerlValue *sep, PerlArray *arr);
PerlArray *perl_split(PerlValue *sep, PerlValue *str);

/* ── hash support ────────────────────────────────────────────────────────── */

#define PERL_HASH_BUCKETS 64

typedef struct PerlHashEntry {
    char                *key;
    PerlValue           *val;
    struct PerlHashEntry *next;
} PerlHashEntry;

typedef struct PerlHash {
    PerlHashEntry   *buckets[PERL_HASH_BUCKETS];
    long long        size;
    int              refcount; /* 0 = scope-managed (named %hash), >0 = anonymous refcounted ({}) */
    pthread_mutex_t *mu;       /* non-NULL when declared : shared */
} PerlHash;

PerlHash *perl_hash_new(void);
PerlHash *perl_anon_hash_new(void); /* like perl_hash_new but refcount=1 (anonymous) */
void       perl_hash_free(PerlHash *h);
void       perl_hash_clear(PerlHash *h);
void       perl_hash_make_shared(PerlHash *h); /* threads::shared: init mu */
void       perl_lock_hash(PerlHash *h);        /* lock + push auto-unlock */

/* key is a PerlValue* — stringified internally */
PerlValue *perl_hash_get_sv(PerlHash *h, PerlValue *key);
PerlValue *perl_hash_get_sv_ref(PerlHash *h, PerlValue *key); /* borrow: no clone, never free result */
void       perl_hash_set_sv(PerlHash *h, PerlValue *key, PerlValue *val);
int        perl_hash_exists_sv(PerlHash *h, PerlValue *key);
PerlValue *perl_hash_delete_sv(PerlHash *h, PerlValue *key);
/* key is a C string literal — no strdup overhead */
PerlValue *perl_hash_get_str_ref(PerlHash *h, const char *key); /* borrow: no clone, never free result */
PerlValue *perl_hash_lvalue_str(PerlHash *h, const char *key);  /* writable: creates slot if missing */
PerlValue *perl_hash_lvalue_sv(PerlHash *h, PerlValue *key);
void       perl_hash_set_str(PerlHash *h, const char *key, PerlValue *val);
int        perl_hash_exists_str(PerlHash *h, const char *key);
PerlValue *perl_hash_delete_str(PerlHash *h, const char *key);

/* list operations */
PerlArray *perl_hash_keys(PerlHash *h);    /* returns new PerlArray* of key strings */
PerlArray *perl_hash_slice(PerlHash *h, PerlArray *keys); /* values for key list */
PerlArray *perl_hash_values(PerlHash *h);  /* returns new PerlArray* of values */
PerlValue *perl_hash_size(PerlHash *h);    /* returns int count of key-value pairs */

/* autovivification: ensure slot holds a ref of the right type, create if needed */
PerlHash  *perl_hash_autoviv_hash(PerlHash *h, const char *key);
PerlHash  *perl_hash_autoviv_hash_sv(PerlHash *h, PerlValue *key);
PerlArray *perl_hash_autoviv_array(PerlHash *h, const char *key);
PerlArray *perl_hash_autoviv_array_sv(PerlHash *h, PerlValue *key);
PerlHash  *perl_array_autoviv_hash(PerlArray *a, long long idx);
PerlArray *perl_array_autoviv_array(PerlArray *a, long long idx);
/* D50: scalar-ref-rooted autoviv — handle FLAT_ARRAY/FLOAT_PAIR safely.
 * slot points to the element pointer inside the parent container (or a
 * temporary slot the caller owns).  idx is the array index. */
PerlArray *perl_array_autoviv_array_from_scalar(PerlValue **slot, long long idx);
PerlHash  *perl_array_autoviv_hash_from_scalar(PerlValue **slot, long long idx);
/* D50: convenience helpers for intermediate chain levels. */
PerlArray *perl_array_autoviv_array_idx(PerlArray *parent, long long idx);
 PerlHash  *perl_hash_autoviv_hash_idx(PerlHash *parent, const char *key);
 /* D50: cross-type autoviv — array-index wanting hash, hash-key wanting array. */
 PerlHash  *perl_array_autoviv_hash_idx(PerlArray *parent, long long idx);
 PerlArray *perl_hash_autoviv_array_idx(PerlHash *parent, const char *key);
 /* PerlValue*-key variant. */
 PerlArray *perl_hash_autoviv_array_idx_sv(PerlHash *parent, PerlValue *key_pv);
 /* D50: read with autoviv — returns cloned element or undef. */
 PerlValue *perl_deref_array_auto(PerlValue *ref_pv, long long idx);

/* lvalue slice assignment */
void perl_hash_assign_slice(PerlHash *h, PerlArray *keys, PerlArray *vals);
void perl_array_assign_slice(PerlArray *a, PerlArray *indices, PerlArray *vals);

/* initialise hash from flat list (k1,v1,k2,v2,...) */
void       perl_hash_from_list(PerlHash *h, PerlArray *list);
void       perl_array_extend_hash(PerlArray *dst, PerlHash *h); /* append k,v pairs */
void       perl_array_push_list_or_scalar(PerlArray *dst, PerlValue *pv); /* unwrap LIST_RESULT or push scalar */

/* ── array manipulation ──────────────────────────────────────────────────── */
/* splice(@arr, off, len, repl) — removes/inserts elements, returns removed */
PerlArray *perl_splice(PerlArray *arr, PerlValue *off_pv, PerlValue *len_pv, PerlArray *repl);

/* ── environment / system ───────────────────────────────────────────────── */
PerlValue *perl_env_get(PerlValue *key);
void       perl_env_set(PerlValue *key, PerlValue *val);
/* D49: warn with optional file/line (for " at FILE line N." + $SIG{__WARN__}) */
void       perl_warn(PerlValue *msg, const char *filename, int line);
/* D49: process-wide %SIG hash ($SIG{__WARN__}, $SIG{__DIE__}, …) */
PerlHash  *perl_get_sig_hash(void);
PerlValue *perl_system(PerlValue *cmd);
PerlValue *perl_backtick(PerlValue *cmd);
/* D70: syscall() builtin */
PerlValue *perl_syscall(PerlValue *args);

/* W2: process / IPC / sockets */
PerlValue *perl_fork(void);
PerlValue *perl_wait_pid(void);
PerlValue *perl_waitpid(PerlValue *pid, PerlValue *flags);
PerlValue *perl_kill(PerlArray *args);
PerlValue *perl_exec(PerlArray *args);
void       perl_exit_n(PerlValue *code);
PerlValue *perl_getppid_val(void);
PerlValue *perl_getuid_val(void);
PerlValue *perl_getgid_val(void);
PerlValue *perl_geteuid_val(void);
PerlValue *perl_getegid_val(void);
PerlValue *perl_setsid_val(void);
PerlValue *perl_getpgrp_val(PerlValue *pid);
PerlValue *perl_setpgrp_val(PerlValue *pid, PerlValue *pgid);
PerlValue *perl_umask_val(PerlValue *mode);
PerlValue *perl_pipe_fh(PerlValue *r, PerlValue *w);
PerlValue *perl_socket_fh(PerlValue *fh, PerlValue *domain, PerlValue *type, PerlValue *proto);
PerlValue *perl_bind_fh(PerlValue *fh, PerlValue *addr);
PerlValue *perl_listen_fh(PerlValue *fh, PerlValue *backlog);
PerlValue *perl_connect_fh(PerlValue *fh, PerlValue *addr);
PerlValue *perl_accept_fh(PerlValue *new_fh, PerlValue *listen_fh);
PerlValue *perl_send_fh(PerlValue *fh, PerlValue *msg, PerlValue *flags);
PerlValue *perl_recv_fh(PerlValue *fh, PerlValue *buf, PerlValue *len, PerlValue *flags);
PerlValue *perl_shutdown_fh(PerlValue *fh, PerlValue *how);
PerlValue *perl_getsockname_fh(PerlValue *fh);
PerlValue *perl_getpeername_fh(PerlValue *fh);
PerlValue *perl_sysopen_fh(PerlValue *fh, PerlValue *path, PerlValue *mode, PerlValue *perms);
PerlValue *perl_sysread_fh(PerlValue *fh, PerlValue *buf, PerlValue *len, PerlValue *off);
PerlValue *perl_syswrite_fh(PerlValue *fh, PerlValue *buf, PerlValue *len, PerlValue *off);
PerlValue *perl_flock_fh(PerlValue *fh, PerlValue *op);
PerlValue *perl_vec_get(PerlValue *str, PerlValue *off, PerlValue *bits);
PerlValue *perl_vec_set(PerlValue *str, PerlValue *off, PerlValue *bits, PerlValue *val);
PerlValue *perl_select4(PerlValue *r, PerlValue *w, PerlValue *e, PerlValue *timeout);
PerlValue *perl_select_fh(PerlValue *newfh);
PerlValue *perl_fcntl_fh(PerlValue *fh, PerlValue *cmd, PerlValue *arg);
PerlValue *perl_ioctl_fh(PerlValue *fh, PerlValue *cmd, PerlValue *arg);
PerlValue *perl_dup_fd(PerlValue *fd);
PerlValue *perl_dup2_fd(PerlValue *oldfd, PerlValue *newfd);
void       perl_sig_poll(void);

/* ── command-line arguments ─────────────────────────────────────────────── */
PerlArray *perl_init_argv(int argc, char **argv); /* call at program start; sets $0, returns @ARGV */
PerlValue *perl_get_dollar0(void);               /* returns stable $0 PerlValue* */

/* ── file test operators ─────────────────────────────────────────────────── */
PerlValue *perl_filetest(int op, PerlValue *path);

/* ── file I/O ────────────────────────────────────────────────────────────── */
/* open($fh, mode, filename) or open($fh, "mode_and_filename") */
PerlValue *perl_open_fh(PerlValue *target, PerlValue *mode, PerlValue *filename);
PerlValue *perl_open2_fh(PerlValue *target, PerlValue *mode_file);
void       perl_close_fh(PerlValue *fh);
PerlValue *perl_readline(PerlValue *fh);
PerlArray *perl_readline_all(PerlValue *fh);
PerlValue *perl_readline_stdin(void);
PerlArray *perl_readline_all_stdin(void);
void       perl_print_fh(PerlValue *fh, PerlValue *v);
void       perl_say_fh(PerlValue *fh, PerlValue *v);
void       perl_printf_fh(PerlValue *fh, PerlValue *fmt, PerlArray *args);
PerlValue *perl_eof_fh(PerlValue *fh);
void       perl_die(PerlValue *msg, const char *filename, int line);
PerlValue *perl_unlink_files(PerlArray *files);
PerlValue *perl_get_stdin(void);    /* returns stable STDIN PerlValue*  */
PerlValue *perl_get_stderr(void);   /* returns stable STDERR PerlValue* */
PerlValue *perl_get_stdout(void);   /* returns stable STDOUT PerlValue* */

/* ── sprintf / printf ────────────────────────────────────────────────────── */
PerlValue *perl_sprintf(PerlValue *fmt, PerlArray *args);
void       perl_printf(PerlValue *fmt, PerlArray *args);

/* ── pack / unpack ───────────────────────────────────────────────────────── */
PerlValue *perl_pack(PerlValue *fmt, PerlArray *args);
PerlValue *perl_unpack(PerlValue *fmt, PerlValue *str);
PerlArray *perl_unpack_to_array(PerlValue *fmt, PerlValue *str);

/* ── math builtins ───────────────────────────────────────────────────────── */
PerlValue *perl_abs_val(PerlValue *v);
PerlValue *perl_int_trunc(PerlValue *v);
PerlValue *perl_sqrt_val(PerlValue *v);

/* ── string case/conversion builtins ────────────────────────────────────── */
PerlValue *perl_uc_str(PerlValue *v);
PerlValue *perl_lc_str(PerlValue *v);
PerlValue *perl_ucfirst_str(PerlValue *v);
PerlValue *perl_lcfirst_str(PerlValue *v);
PerlValue *perl_index_str(PerlValue *str, PerlValue *sub, PerlValue *pos);
PerlValue *perl_rindex_str(PerlValue *str, PerlValue *sub, PerlValue *pos);
PerlValue *perl_chr_val(PerlValue *n);
PerlValue *perl_ord_val(PerlValue *s);
PerlValue *perl_hex_val(PerlValue *s);
PerlValue *perl_oct_val(PerlValue *s);

/* ── list ops ────────────────────────────────────────────────────────────── */
PerlArray *perl_reverse_array(PerlArray *a);    /* new array, reversed */
PerlValue *perl_reverse_str(PerlValue *s);      /* new string, reversed */
PerlArray *perl_sort_num_asc(PerlArray *a);     /* numeric ascending */
PerlArray *perl_sort_num_desc(PerlArray *a);    /* numeric descending */
PerlArray *perl_sort_str_asc(PerlArray *a);     /* string ascending (same as sort_str) */
PerlArray *perl_sort_str_desc(PerlArray *a);    /* string descending */

/* ── comparison returning -1/0/1 ─────────────────────────────────────────── */
PerlValue *perl_spaceship(PerlValue *a, PerlValue *b);  /* <=> */
PerlValue *perl_str_spaceship(PerlValue *a, PerlValue *b); /* cmp */

/* ── range ───────────────────────────────────────────────────────────────── */
PerlArray *perl_range(PerlValue *from, PerlValue *to);

/* ── regex (PCRE2) ───────────────────────────────────────────────────────── */
PerlValue *perl_regex_match(PerlValue *str, const char *pattern, const char *flags);
PerlValue *perl_regex_match_g(PerlValue *str, const char *pattern, const char *flags);
PerlArray *perl_regex_match_all(PerlValue *str, const char *pattern, const char *flags);
long long  perl_regex_subst(PerlValue *str, const char *pattern, const char *repl, const char *flags);
/* D38c: s///e — eval_fn is called once per match with $1/$& already set;
   returns a PerlValue* whose stringification becomes the replacement.
   captures may be NULL (no outer lexical capture). */
typedef PerlValue *(*PerlSubstEvalFn)(void);
long long  perl_regex_subst_e(PerlValue *str, const char *pattern, const char *flags,
                              PerlSubstEvalFn eval_fn, PerlArray *captures);
PerlValue *perl_capture(long long n);
PerlArray *perl_split_regex(const char *pattern, const char *flags, PerlValue *str);

/* ── references ──────────────────────────────────────────────────────────── */
PerlValue *perl_ref_scalar(PerlValue *v);
PerlValue *perl_ref_array(PerlArray *a);
PerlValue *perl_ref_hash(PerlHash *h);
PerlValue *perl_deref_scalar(PerlValue *ref);   /* returns (PerlValue*)ref->pval */
PerlArray *perl_deref_array(PerlValue *ref);    /* returns (PerlArray*)ref->pval */
PerlArray *perl_deref_array_ro(PerlValue *ref); /* fast read-only variant, assumes REF_ARRAY */
PerlHash  *perl_deref_hash(PerlValue *ref);     /* returns (PerlHash*)ref->pval */
PerlValue *perl_ref_type(PerlValue *ref);       /* "SCALAR"/"ARRAY"/"HASH"/""   */

/* ── code references ─────────────────────────────────────────────────────── */
typedef PerlValue *(*PerlSubFnCtx)(PerlArray *, int ctx);

/* PerlClosure is the heap object stored in PERL_CODE_REF pval */
typedef struct PerlClosure {
    PerlSubFnCtx   fn;
    PerlValue **captures;  /* D62: refcounted PerlValue* — see PV_CAPTURE_MASK */
    int         ncaptures;
    int         refcount;  /* # of live PerlValue wrappers whose pval points
                               here (mirrors PerlArray/PerlHash's refcount
                               pattern); always >=1 while reachable */
} PerlClosure;

PerlValue *perl_make_code_ref(PerlSubFnCtx fp);                     /* no captures */
PerlValue *perl_make_closure(PerlSubFnCtx fp, PerlArray *captures); /* with captures */
PerlValue *perl_call_code_ref(PerlValue *ref, PerlArray *args);
PerlValue *perl_get_capture(long long idx);  /* returns capture[idx] during a closure call */

/* ── OOP / bless / method dispatch ──────────────────────────────────────── */
PerlValue *perl_bless(PerlValue *ref, PerlValue *class_pv);
void       perl_register_method(const char *key, PerlSubFnCtx fn);
PerlValue *perl_call_named_sub(const char *name, PerlArray *args, int ctx);
PerlValue *perl_get_or_create_global_scalar(const char *key); /* D58: process-wide package-scalar registry for --do-lib builds */
/* ── threads ─────────────────────────────────────────────────────────────── */
#include <pthread.h>
typedef struct PerlThread {
    pthread_t   pth;
    long long   tid;
    PerlValue  *result;    /* return value (set by thread before exit) */
    int         joined;
    int         detached;
} PerlThread;

PerlValue *perl_threads_create(PerlValue *code_pv, PerlArray *args);
PerlValue *perl_threads_join(PerlValue *thr_pv);    /* returns result PerlValue* */
void       perl_threads_detach(PerlValue *thr_pv);
PerlValue *perl_threads_tid(PerlValue *thr_pv);
PerlValue *perl_threads_self(void);
PerlArray *perl_threads_list(void);
void       perl_threads_yield(void);

PerlValue *perl_dispatch_method(PerlValue *obj, const char *method, PerlArray *args);
PerlValue *perl_dispatch_method_super(PerlValue *obj, const char *caller_pkg,
                                      const char *method, PerlArray *args);

/* ── inheritance ─────────────────────────────────────────────────────────── */
void       perl_set_isa(const char *child, const char *parent);

/* ── tr/// character translation ────────────────────────────────────────── */
/* returns count of characters translated (like Perl's tr return value) */
long long  perl_tr(PerlValue *str, const char *search, const char *replace, const char *flags);

/* ── local() dynamic save/restore ───────────────────────────────────────── */
int  perl_local_save_depth(void);              /* current save-stack depth */
void perl_local_save(PerlValue *pv);           /* save current state of *pv */
void perl_local_restore_to(int depth);         /* restore all saved since depth */

/* ── special global variables (Tier 2) ───────────────────────────────────── */
PerlValue *perl_get_dollar_dot(void);      /* $.  — current input line number  */
PerlValue *perl_get_dollar_comma(void);    /* $,  — output field separator     */
PerlValue *perl_get_dollar_bsl(void);      /* $\  — output record separator    */
PerlValue *perl_get_dollar_amp(void);      /* $&  — last successful regex match */
PerlValue *perl_get_dollar_question(void); /* $?  — last wait/system status    */
void       perl_print_sep(void);           /* print $, if defined              */
void       perl_print_sep_fh(PerlValue *fh);
void       perl_print_ors(void);           /* print $\ if defined              */
void       perl_print_ors_fh(PerlValue *fh);

/* ── POSIX functions ──────────────────────────────────────────────────────── */
PerlValue *perl_posix_floor(PerlValue *v);
PerlValue *perl_posix_ceil(PerlValue *v);
PerlValue *perl_posix_fmod(PerlValue *a, PerlValue *b);
PerlValue *perl_posix_strftime(PerlArray *args); /* (fmt, sec,min,hour,mday,mon,year,...) */

/* ── Scalar::Util functions ───────────────────────────────────────────────── */
PerlValue *perl_su_blessed(PerlValue *v);
PerlValue *perl_su_reftype(PerlValue *v);
PerlValue *perl_su_looks_like_number(PerlValue *v);

/* ── Carp functions ───────────────────────────────────────────────────────── */
void       perl_carp_croak(PerlArray *args);   /* die with caller location    */
void       perl_carp_carp(PerlArray *args);    /* warn with caller location   */

/* ── file I/O extras ──────────────────────────────────────────────────────── */
PerlValue *perl_seek_fh(PerlValue *fh, PerlValue *off, PerlValue *whence);
PerlValue *perl_tell_fh(PerlValue *fh);
PerlValue *perl_binmode_fh(PerlValue *fh, PerlValue *layer);

/* ── filesystem stat / glob ───────────────────────────────────────────────── */
PerlArray *perl_stat_path(PerlValue *v);
PerlArray *perl_lstat_path(PerlValue *v);
PerlArray *perl_glob_val(PerlValue *pattern);

/* ── Tier 3 file / misc builtins ────────────────────────────────────────── */
PerlValue *perl_read_fh(PerlValue *fh, PerlValue *buf_pv, PerlValue *nbytes, PerlValue *offset);
PerlValue *perl_fileno_fh(PerlValue *fh);
PerlValue *perl_truncate_fh(PerlValue *fh_or_path, PerlValue *len);
PerlArray *perl_each_hash(PerlHash *h);        /* returns [key,val] or empty [] */
PerlValue *perl_pos_str(PerlValue *pv);        /* pos($str) — last match pos */
PerlValue *perl_getpid(void);                  /* getpid() */
PerlValue *perl_get_os_name(void);             /* $^O — "linux" */

/* ── UNIVERSAL isa / can ──────────────────────────────────────────────────── */
PerlValue *perl_isa_check(PerlValue *obj, PerlValue *class_pv);
PerlValue *perl_can_check(PerlValue *obj, PerlValue *method_pv);

/* ── time / randomness / process ────────────────────────────────────────── */
PerlValue *perl_rand_val(PerlValue *max);       /* rand [max] — float [0,max) */
void       perl_srand_val(PerlValue *seed);     /* srand [seed] */
PerlValue *perl_time_val(void);                 /* time() — epoch seconds */
PerlArray *perl_localtime_val(PerlValue *t);    /* localtime — 9-element list */
PerlArray *perl_gmtime_val(PerlValue *t);       /* gmtime — 9-element list */
PerlValue *perl_sleep_val(PerlValue *secs);     /* sleep — returns actual secs slept */
PerlValue *perl_alarm_val(PerlValue *secs);     /* alarm — returns prev alarm value */

/* ── Time::HiRes (D30, built-in) ──────────────────────────────────────────── */
PerlValue *perl_hires_time(void);                       /* time() — fractional epoch seconds */
PerlArray *perl_hires_gettimeofday_list(void);           /* list ctx: (sec, usec) */
PerlValue *perl_hires_gettimeofday_scalar(void);         /* scalar ctx: fractional seconds */
PerlValue *perl_hires_sleep(PerlValue *secs);            /* fractional seconds; returns actual secs slept */
PerlValue *perl_hires_usleep(PerlValue *usecs);          /* microseconds; returns actual usecs slept */
PerlValue *perl_hires_tv_interval(PerlValue *t0ref, PerlValue *t1ref); /* elapsed seconds between two gettimeofday refs */

/* ── List::Util ───────────────────────────────────────────────────────────── */
PerlValue *perl_sum_list(PerlArray *a);         /* sum LIST — undef if empty */
PerlValue *perl_min_list(PerlArray *a);         /* min LIST — undef if empty */
PerlValue *perl_max_list(PerlArray *a);         /* max LIST — undef if empty */
PerlArray *perl_uniq_list(PerlArray *a);        /* uniq LIST — keeps first occurrence of each distinct value, anywhere in the list (D69: NOT consecutive-only, that was the bug) */

/* ── sort with custom comparator ─────────────────────────────────────────── */
typedef long long (*PerlSortCmpFn)(PerlValue *, PerlValue *);
/* captures: outer-scope variables the comparator body closes over (D61),
   installed via the same s_current_captures/perl_get_capture mechanism a
   closure's body already uses — may be NULL/empty for a comparator that
   captures nothing. */
PerlArray *perl_sort_custom(PerlArray *a, PerlSortCmpFn cmp, PerlArray *captures); /* sort copy of a */

/* ── directory I/O ───────────────────────────────────────────────────────── */
PerlValue *perl_opendir_fh(PerlValue *target, PerlValue *path);
PerlValue *perl_readdir(PerlValue *dh);      /* scalar: one entry or undef */
PerlArray *perl_readdir_all(PerlValue *dh);  /* list: all remaining entries */
void       perl_closedir_fh(PerlValue *dh);

/* ── filesystem ops ──────────────────────────────────────────────────────── */
PerlValue *perl_chdir(PerlValue *path);
PerlValue *perl_mkdir_op(PerlValue *path, PerlValue *mode);  /* mode may be undef → 0777 */
PerlValue *perl_rmdir_op(PerlValue *path);
PerlValue *perl_rename_op(PerlValue *oldp, PerlValue *newp);
PerlValue *perl_chmod_op(PerlValue *mode, PerlArray *files);

/* ── special global variables ────────────────────────────────────────────── */
PerlValue *perl_get_input_sep(void);           /* $/ — input record separator (stable ptr) */
PerlValue *perl_get_dollar_bang(void);         /* $! — errno string */
typedef PerlValue *(*PerlSubFnCtx)(PerlArray *, int); /* fn(args, ctx) */

int perl_push_wantarray(int ctx);
int perl_pop_wantarray(void);
PerlValue *perl_wantarray(void);
PerlArray *perl_caller(int level);             /* caller(N) — returns (pkg,file,line) at N levels up */
PerlValue *perl_get_plus_hash(void);
PerlValue *perl_plus_hash_get(PerlValue *key);
PerlArray *perl_plus_hash_keys(void);
void perl_clear_named_captures(void);

/* ── eval / exception handling ───────────────────────────────────────────── */
/* codegen allocates jmp_buf on stack and calls setjmp directly;
   perl_eval_push/pop manage the eval stack of jmp_buf pointers */
#include <setjmp.h>
void       perl_eval_push(jmp_buf *jb); /* push caller's jmp_buf onto eval stack */
void       perl_eval_pop(void);          /* pop after eval completes */
PerlValue *perl_get_dollar_at(void);   /* returns stable $@ PerlValue* */

/* ── caller() call stack ─────────────────────────────────────────────────── */
void perl_push_call_frame(const char *pkg, const char *file, int line);
void perl_pop_call_frame(void);
PerlArray *perl_caller(int level);

/* ── local @arr / local %hash ───────────────────────────────────────────── */
void perl_local_save_array(PerlArray **slot);
void perl_local_save_hash(PerlHash  **slot);

/* ── AUTOLOAD ────────────────────────────────────────────────────────────── */
PerlValue *perl_get_autoload_name(void);   /* returns stable $AUTOLOAD PV* */

/* ── pos() write ─────────────────────────────────────────────────────────── */
void perl_set_pos_str(PerlValue *pv, PerlValue *pos);

/* ── runtime require ─────────────────────────────────────────────────────── */
PerlValue *perl_runtime_require(const char *modname);
PerlValue *perl_do_file(PerlValue *path_pv);
void       perl_do_lib_cleanup(void);  /* D24: dlclose() do'd shared libraries */

/* ── XS / FFI ────────────────────────────────────────────────────────────── */
PerlValue *perl_xs_load_library(PerlValue *libname_pv);
PerlValue *perl_xs_call_dynamic(PerlValue *libname_pv, PerlValue *funcname_pv,
                                PerlValue *signature_pv, PerlArray *args);

/* ── DBI/SQLite integration ──────────────────────────────────────────────── */
/* SQLite database handle */
typedef struct PerlDBIHandle {
    void *db; /* SQLite database handle */
    char *dbname;
    int is_connected;
    int refcount;
    char *last_error;
} PerlDBIHandle;

typedef struct PerlDBIStatement {
    void *stmt;              /* sqlite3_stmt* */
    PerlDBIHandle *dbh;
    int done;
    int rows_affected;
    int refcount;
    char *last_error;
} PerlDBIStatement;

/* Database connection */
PerlValue *perl_dbi_connect(PerlValue *dsn, PerlValue *username, PerlValue *password);
PerlValue *perl_dbi_disconnect(PerlValue *dbh);
PerlValue *perl_dbi_prepare(PerlValue *dbh, PerlValue *sql);
PerlValue *perl_dbi_execute(PerlValue *sth, PerlArray *params);
PerlValue *perl_dbi_fetch(PerlValue *sth);
PerlValue *perl_dbi_fetchall(PerlValue *sth);
PerlValue *perl_dbi_rows(PerlValue *sth);
PerlValue *perl_dbi_commit(PerlValue *dbh);
PerlValue *perl_dbi_rollback(PerlValue *dbh);
PerlValue *perl_dbi_error(PerlValue *dbh);

/* ── program cleanup (free shared-mutex side-table, named-captures hash, etc.) */
void perl_cleanup(void);

/* string eval EXPR — compile via --do-lib (no outer lexicals for dynamic
   strings; constant strings are inlined by codegen into EvalBlock). */
PerlValue *perl_eval_string(PerlValue *code_pv);

/* D96: compound-assign on a 2D array element where the inner row may be
   FLAT_ARRAY or REF_ARRAY.  Re-reads the row PV's tag/pval to avoid using a
   stale flat-row cache (the row may have been lazy-converted by an earlier read
   in the same loop body).  op: 0=+,1=-,2=*,3=/.  Returns the boxed result. */
 PerlValue *perl_flat_row_op_assign(PerlValue *row_pv, long long idx,
                                    PerlValue *rhs_pv, int op);
 /* D98: set one element in a 2D row that may be FLAT_ARRAY or REF_ARRAY.
    Keeps FLAT_ARRAY rows flat (writes the numified value into the backing
    double[] in bounds) so the "all rows flat" read fast-path assumption stays
    valid; otherwise falls back to the normal converting set.  Clones v. */
 PerlValue *perl_array_set_row(PerlValue *row_pv, long long idx, PerlValue *v);
 /* D98: ensure parent[idx] holds an array ref, creating one only if missing/
    undef — but (unlike perl_array_autoviv_array_idx) it does NOT convert an
    existing FLAT_ARRAY/FLOAT_PAIR row to a REF_ARRAY, preserving the "all rows
    flat" invariant the 2D read fast-path relies on.  Returns void. */
 void perl_array_ensure_slot(PerlArray *parent, long long idx);

/* ── operator overloading (use overload) ─────────────────────────────────── */
/* Register an overload for a class.  op is the Perl overload key (e.g. "+",
   "*", "/", "%", "**", "-", "<=>", "cmp", "\"\"", "0+", "bool", "neg").
   method is the fully-qualified method name to call. */
void perl_register_overload(const char *class_name, const char *op,
                            const char *method);

/* ── Math::BigInt (D97) — built-in using mini-gmp, zero external dependency */
PerlValue *perl_bigint_new(PerlValue *arg);
PerlValue *perl_bigint_bmul(PerlValue *self, PerlValue *other);
PerlValue *perl_bigint_badd(PerlValue *self, PerlValue *other);
PerlValue *perl_bigint_bsub(PerlValue *self, PerlValue *other);
PerlValue *perl_bigint_bcmp(PerlValue *self, PerlValue *other);
PerlValue *perl_bigint_numify(PerlValue *self);
PerlValue *perl_bigint_ovl_add(PerlValue *lhs, PerlValue *rhs);
PerlValue *perl_bigint_ovl_sub(PerlValue *lhs, PerlValue *rhs);
PerlValue *perl_bigint_ovl_mul(PerlValue *lhs, PerlValue *rhs);
PerlValue *perl_bigint_ovl_div(PerlValue *lhs, PerlValue *rhs);
PerlValue *perl_bigint_ovl_cmp(PerlValue *lhs, PerlValue *rhs);
PerlValue *perl_bigint_ovl_str(PerlValue *self);
PerlValue *perl_bigint_ovl_neg(PerlValue *self);

/* Note: runtime require/do of dynamic files now also set $@ + return undef.
   Compile-time 'use' and static 'require' are still inlined by the driver. */

#ifdef __cplusplus
}
#endif

#endif /* PERLC_RUNTIME_H */
