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
} PerlTag;

typedef struct PerlValue {
    PerlTag tag;
    union {
        long long ival;
        double    fval;
        char     *sval;   /* heap-allocated, NUL-terminated */
        void     *pval;   /* for reference types */
    };
    long long matchpos;   /* current /g match offset; 0 = start of string */
} PerlValue;

/* allocation */
PerlValue *perl_alloc_undef(void);
PerlValue *perl_alloc_int(long long v);
PerlValue *perl_alloc_float(double v);
PerlValue *perl_alloc_string(const char *s);
PerlValue *perl_clone(const PerlValue *v);
void       perl_free(PerlValue *v);

/* coercions */
long long  perl_to_int(const PerlValue *v);
double     perl_to_float(const PerlValue *v);
char      *perl_to_string(const PerlValue *v);   /* caller must free */
int        perl_is_true(const PerlValue *v);

/* assignment: dst = src  (manages dst's old string if any) */
void perl_assign(PerlValue *dst, const PerlValue *src);

/* arithmetic */
PerlValue *perl_add(const PerlValue *a, const PerlValue *b);
PerlValue *perl_sub(const PerlValue *a, const PerlValue *b);
PerlValue *perl_mul(const PerlValue *a, const PerlValue *b);
PerlValue *perl_div(const PerlValue *a, const PerlValue *b);
PerlValue *perl_mod(const PerlValue *a, const PerlValue *b);
PerlValue *perl_negate(const PerlValue *a);

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
    PerlValue **elems;
    long long   len;
    long long   cap;
} PerlArray;

PerlArray *perl_array_new(void);
void       perl_array_push(PerlArray *a, PerlValue *v);
PerlValue *perl_array_pop(PerlArray *a);
PerlValue *perl_array_get(PerlArray *a, long long idx);
void       perl_array_set(PerlArray *a, long long idx, PerlValue *v);
PerlValue *perl_array_len(PerlArray *a);
void       perl_array_sort_str(PerlArray *a);
void       perl_array_extend(PerlArray *dst, PerlArray *src);
PerlValue *perl_array_shift(PerlArray *a);
void       perl_array_unshift(PerlArray *a, PerlValue *v);

/* ── string builtins ─────────────────────────────────────────────────────── */
long long  perl_chomp(PerlValue *v);       /* remove trailing \n in-place, returns removed count */
long long  perl_chomp_array(PerlArray *a); /* chomp every element; returns total removed count */
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
    PerlHashEntry  *buckets[PERL_HASH_BUCKETS];
    long long       size;
} PerlHash;

PerlHash *perl_hash_new(void);
void       perl_hash_free(PerlHash *h);

/* key is a PerlValue* — stringified internally */
PerlValue *perl_hash_get_sv(PerlHash *h, PerlValue *key);
void       perl_hash_set_sv(PerlHash *h, PerlValue *key, PerlValue *val);
int        perl_hash_exists_sv(PerlHash *h, PerlValue *key);
PerlValue *perl_hash_delete_sv(PerlHash *h, PerlValue *key);

/* list operations */
PerlArray *perl_hash_keys(PerlHash *h);    /* returns new PerlArray* of key strings */
PerlArray *perl_hash_values(PerlHash *h);  /* returns new PerlArray* of values */
PerlValue *perl_hash_size(PerlHash *h);    /* returns int count of key-value pairs */

/* initialise hash from flat list (k1,v1,k2,v2,...) */
void       perl_hash_from_list(PerlHash *h, PerlArray *list);
void       perl_array_extend_hash(PerlArray *dst, PerlHash *h); /* append k,v pairs */

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
PerlHash  *perl_deref_hash(PerlValue *ref);     /* returns (PerlHash*)ref->pval */
PerlValue *perl_ref_type(PerlValue *ref);       /* "SCALAR"/"ARRAY"/"HASH"/""   */

#ifdef __cplusplus
}
#endif

#endif /* PERLC_RUNTIME_H */
