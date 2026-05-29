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
    PERL_FLAT_ARRAY = 10, /* flat double[] — pval=double*, matchpos=len */
    PERL_THREAD     = 11, /* thread object  — pval=PerlThread* */
} PerlTag;

/* PV_FLAG_SHARED: variable is a threads::shared variable (PerlSharedVar). */
#define PV_FLAG_SHARED 1u

typedef struct PerlValue {
    PerlTag      tag;
    unsigned int flags;       /* PV_FLAG_SHARED etc.; fits existing padding slot */
    union {
        long long ival;
        double    fval;
        char     *sval;   /* heap-allocated, NUL-terminated */
        void     *pval;   /* for reference types */
    };
    long long matchpos;      /* current /g match offset; 0 = start of string */
    char     *blessed_class; /* NULL unless bless'd */
} PerlValue;

/* PerlSharedVar: a PerlValue with an embedded mutex+condvar for threads::shared.
   The PerlValue MUST be the first member so that (PerlValue*) == (PerlSharedVar*). */
#include <pthread.h>
typedef struct {
    PerlValue       pv;
    pthread_mutex_t mu;
    pthread_cond_t  cond;
} PerlSharedVar;

/* allocation */
PerlValue *perl_alloc_undef(void);
PerlValue *perl_alloc_int(long long v);
PerlValue *perl_alloc_float(double v);
PerlValue *perl_alloc_string(const char *s);
PerlValue *perl_alloc_flat_array(long long n); /* alloc PV with pval=double[n] */
PerlValue *perl_clone(const PerlValue *v);
void       perl_free(PerlValue *v);
PerlValue *perl_make_shared_scalar(void); /* threads::shared — returns PerlSharedVar->pv */

/* threads::shared — lock/cond */
void perl_lock_shared(PerlValue *pv);          /* lock scalar shared var + push auto-unlock */
void perl_cond_wait(PerlValue *pv);            /* cond_wait on shared var's condvar */
void perl_cond_signal(PerlValue *pv);          /* cond_signal */
void perl_cond_broadcast(PerlValue *pv);       /* cond_broadcast */

/* coercions */
long long  perl_to_int(const PerlValue *v);
double     perl_to_float(const PerlValue *v);
char      *perl_to_string(const PerlValue *v);   /* caller must free */
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
PerlValue *perl_array_to_list_return(PerlArray *av); /* wrap for return in list/scalar ctx */
PerlArray *perl_unwrap_list_return(PerlValue *pv);   /* caller side: extract array from return */

PerlArray *perl_array_new(void);
PerlArray *perl_anon_array_new(void); /* like perl_array_new but refcount=1 (anonymous) */
long long perl_array_is_all_flat(PerlArray *av); /* 1 if all elems are FLAT_ARRAY */
void       perl_array_free(PerlArray *a);
void       perl_array_make_shared(PerlArray *a); /* threads::shared: init mu */
void       perl_lock_array(PerlArray *a);        /* lock + push auto-unlock */
void       perl_array_push(PerlArray *a, PerlValue *v);
void       perl_array_push_capture(PerlArray *a, PerlValue *v);
PerlValue *perl_array_pop(PerlArray *a);
PerlValue *perl_array_get(PerlArray *a, long long idx);
PerlValue *perl_array_get_ref(PerlArray *a, long long idx); /* borrow: no clone, never free result */
void       perl_array_set(PerlArray *a, long long idx, PerlValue *v);
PerlValue *perl_array_len(PerlArray *a);
void perl_array_clear(PerlArray *a);
void perl_array_replace(PerlArray *dst, PerlArray *src);
void       perl_array_sort_str(PerlArray *a);
void       perl_array_extend(PerlArray *dst, PerlArray *src);
PerlValue *perl_array_shift(PerlArray *a);
void       perl_array_unshift(PerlArray *a, PerlValue *v);

/* ── string builtins ─────────────────────────────────────────────────────── */
long long  perl_chomp(PerlValue *v);       /* remove trailing \n in-place, returns removed count */
long long  perl_chomp_array(PerlArray *a); /* chomp every element; returns total removed count */
PerlValue *perl_chop(PerlValue *v);        /* remove and return last character */
PerlValue *perl_length(PerlValue *v);
PerlValue *perl_substr2(PerlValue *str, PerlValue *off);
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
void       perl_hash_set_str(PerlHash *h, const char *key, PerlValue *val);
int        perl_hash_exists_str(PerlHash *h, const char *key);
PerlValue *perl_hash_delete_str(PerlHash *h, const char *key);

/* list operations */
PerlArray *perl_hash_keys(PerlHash *h);    /* returns new PerlArray* of key strings */
PerlArray *perl_hash_values(PerlHash *h);  /* returns new PerlArray* of values */
PerlValue *perl_hash_size(PerlHash *h);    /* returns int count of key-value pairs */

/* initialise hash from flat list (k1,v1,k2,v2,...) */
void       perl_hash_from_list(PerlHash *h, PerlArray *list);
void       perl_array_extend_hash(PerlArray *dst, PerlHash *h); /* append k,v pairs */

/* ── array manipulation ──────────────────────────────────────────────────── */
/* splice(@arr, off, len, repl) — removes/inserts elements, returns removed */
PerlArray *perl_splice(PerlArray *arr, PerlValue *off_pv, PerlValue *len_pv, PerlArray *repl);

/* ── environment / system ───────────────────────────────────────────────── */
PerlValue *perl_env_get(PerlValue *key);
void       perl_env_set(PerlValue *key, PerlValue *val);
void       perl_warn(PerlValue *msg);
PerlValue *perl_system(PerlValue *cmd);
PerlValue *perl_backtick(PerlValue *cmd);

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
void       perl_die(PerlValue *msg);
PerlValue *perl_unlink_files(PerlArray *files);
PerlValue *perl_get_stdin(void);    /* returns stable STDIN PerlValue*  */
PerlValue *perl_get_stderr(void);   /* returns stable STDERR PerlValue* */
PerlValue *perl_get_stdout(void);   /* returns stable STDOUT PerlValue* */

/* ── sprintf / printf ────────────────────────────────────────────────────── */
PerlValue *perl_sprintf(PerlValue *fmt, PerlArray *args);
void       perl_printf(PerlValue *fmt, PerlArray *args);

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
    PerlValue **captures;  /* borrowed PerlValue* — not owned */
    int         ncaptures;
} PerlClosure;

PerlValue *perl_make_code_ref(PerlSubFnCtx fp);                     /* no captures */
PerlValue *perl_make_closure(PerlSubFnCtx fp, PerlArray *captures); /* with captures */
PerlValue *perl_call_code_ref(PerlValue *ref, PerlArray *args);
PerlValue *perl_get_capture(long long idx);  /* returns capture[idx] during a closure call */

/* ── OOP / bless / method dispatch ──────────────────────────────────────── */
PerlValue *perl_bless(PerlValue *ref, PerlValue *class_pv);
void       perl_register_method(const char *key, PerlSubFnCtx fn);
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

/* ── List::Util ───────────────────────────────────────────────────────────── */
PerlValue *perl_sum_list(PerlArray *a);         /* sum LIST — undef if empty */
PerlValue *perl_min_list(PerlArray *a);         /* min LIST — undef if empty */
PerlValue *perl_max_list(PerlArray *a);         /* max LIST — undef if empty */
PerlArray *perl_uniq_list(PerlArray *a);        /* uniq LIST — remove consecutive dups */

/* ── sort with custom comparator ─────────────────────────────────────────── */
typedef long long (*PerlSortCmpFn)(PerlValue *, PerlValue *);
PerlArray *perl_sort_custom(PerlArray *a, PerlSortCmpFn cmp); /* sort copy of a */

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

/* ── DBI/SQLite integration ──────────────────────────────────────────────── */
/* SQLite database handle */
typedef struct PerlDBIHandle {
    void *db; /* SQLite database handle */
    char *dbname;
    int is_connected;
} PerlDBIHandle;

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

/* ── string eval hook ────────────────────────────────────────────────────── */
/* Set by eval_jit.cpp (linked only when program uses eval EXPR).
   NULL → perl_eval_string sets $@ and returns undef. */
typedef PerlValue *(*PerlEvalStringFn)(const char *code);
extern PerlEvalStringFn perl_eval_string_fn;
PerlValue *perl_eval_string(PerlValue *code_pv);  /* called from JIT'd code */

#ifdef __cplusplus
}
#endif

#endif /* PERLC_RUNTIME_H */
