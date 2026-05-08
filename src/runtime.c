#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <setjmp.h>

/* ── eval / $@ support ───────────────────────────────────────────────────── */

/* jmp_buf pointers are pushed by callers (codegen allocates jmp_buf on stack) */
#define EVAL_STACK_MAX 64
static jmp_buf *s_eval_stack[EVAL_STACK_MAX];
static int      s_eval_depth = 0;
static PerlValue s_dollar_at = { PERL_STRING, {0}, 0 }; /* $@ */

void perl_eval_push(jmp_buf *jb) {
    if (s_eval_depth < EVAL_STACK_MAX)
        s_eval_stack[s_eval_depth++] = jb;
}

void perl_eval_pop(void) {
    if (s_eval_depth > 0) s_eval_depth--;
}

PerlValue *perl_get_dollar_at(void) { return &s_dollar_at; }

/* ── allocation ──────────────────────────────────────────────────────────── */

PerlValue *perl_alloc_undef(void) {
    PerlValue *v = malloc(sizeof *v);
    v->tag = PERL_UNDEF;
    v->ival = 0;
    return v;
}

PerlValue *perl_alloc_int(long long n) {
    PerlValue *v = malloc(sizeof *v);
    v->tag = PERL_INT;
    v->ival = n;
    return v;
}

PerlValue *perl_alloc_float(double f) {
    PerlValue *v = malloc(sizeof *v);
    v->tag = PERL_FLOAT;
    v->fval = f;
    return v;
}

PerlValue *perl_alloc_string(const char *s) {
    PerlValue *v = malloc(sizeof *v);
    v->tag = PERL_STRING;
    v->sval = strdup(s ? s : "");
    return v;
}

PerlValue *perl_clone(const PerlValue *src) {
    if (!src) return perl_alloc_undef();
    if (src->tag == PERL_STRING) return perl_alloc_string(src->sval);
    PerlValue *v = malloc(sizeof *v);
    *v = *src;
    v->matchpos = 0;   /* fresh clone starts at beginning */
    return v;
}

void perl_free(PerlValue *v) {
    if (!v) return;
    if (v->tag == PERL_STRING) free(v->sval);
    free(v);
}

/* ── coercions ───────────────────────────────────────────────────────────── */

long long perl_to_int(const PerlValue *v) {
    if (!v) return 0;
    switch (v->tag) {
        case PERL_INT:    return v->ival;
        case PERL_FLOAT:  return (long long)v->fval;
        case PERL_STRING: return atoll(v->sval);
        default:          return 0;
    }
}

double perl_to_float(const PerlValue *v) {
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
            snprintf(buf, sizeof buf, "SCALAR(0x%llx)", (unsigned long long)(uintptr_t)v->pval);
            return strdup(buf);
        case PERL_REF_ARRAY:
            snprintf(buf, sizeof buf, "ARRAY(0x%llx)", (unsigned long long)(uintptr_t)v->pval);
            return strdup(buf);
        case PERL_REF_HASH:
            snprintf(buf, sizeof buf, "HASH(0x%llx)", (unsigned long long)(uintptr_t)v->pval);
            return strdup(buf);
        case PERL_FILEHANDLE:
            snprintf(buf, sizeof buf, "GLOB(0x%llx)", (unsigned long long)(uintptr_t)v->pval);
            return strdup(buf);
        default:
            return strdup("");
    }
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
            return 1;
        case PERL_FILEHANDLE:
            return v->pval != NULL;
        default: return 0;
    }
}

void perl_assign(PerlValue *dst, const PerlValue *src) {
    if (!dst) return;
    if (dst->tag == PERL_STRING) { free(dst->sval); dst->sval = NULL; }
    if (!src) { dst->tag = PERL_UNDEF; dst->ival = 0; dst->matchpos = 0; return; }
    *dst = *src;
    if (src->tag == PERL_STRING) dst->sval = strdup(src->sval);
    dst->matchpos = 0;   /* assignment resets /g position */
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

static int both_int(const PerlValue *a, const PerlValue *b) {
    return a->tag == PERL_INT && b->tag == PERL_INT;
}

/* ── arithmetic ──────────────────────────────────────────────────────────── */

PerlValue *perl_add(const PerlValue *a, const PerlValue *b) {
    if (both_int(a, b)) return perl_alloc_int(a->ival + b->ival);
    return perl_alloc_float(perl_to_float(a) + perl_to_float(b));
}

PerlValue *perl_sub(const PerlValue *a, const PerlValue *b) {
    if (both_int(a, b)) return perl_alloc_int(a->ival - b->ival);
    return perl_alloc_float(perl_to_float(a) - perl_to_float(b));
}

PerlValue *perl_mul(const PerlValue *a, const PerlValue *b) {
    if (both_int(a, b)) return perl_alloc_int(a->ival * b->ival);
    return perl_alloc_float(perl_to_float(a) * perl_to_float(b));
}

PerlValue *perl_div(const PerlValue *a, const PerlValue *b) {
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

PerlValue *perl_num_eq(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_to_float(a) == perl_to_float(b));
}
PerlValue *perl_num_ne(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_to_float(a) != perl_to_float(b));
}
PerlValue *perl_num_lt(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_to_float(a) <  perl_to_float(b));
}
PerlValue *perl_num_gt(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_to_float(a) >  perl_to_float(b));
}
PerlValue *perl_num_le(const PerlValue *a, const PerlValue *b) {
    return perl_alloc_int(perl_to_float(a) <= perl_to_float(b));
}
PerlValue *perl_num_ge(const PerlValue *a, const PerlValue *b) {
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
    a->len = 0; a->cap = 8;
    a->elems = malloc(a->cap * sizeof(PerlValue *));
    return a;
}

void perl_array_push(PerlArray *a, PerlValue *v) {
    if (a->len == a->cap) {
        a->cap *= 2;
        a->elems = realloc(a->elems, a->cap * sizeof(PerlValue *));
    }
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
    free(h);
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

int perl_hash_exists_sv(PerlHash *h, PerlValue *key) {
    char *ks = perl_to_string(key);
    int r = hash_find(h, ks) != NULL;
    free(ks);
    return r;
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
    PerlValue *r = malloc(sizeof *r);
    r->tag = PERL_REF_SCALAR;
    r->pval = v;
    return r;
}

PerlValue *perl_ref_array(PerlArray *a) {
    PerlValue *r = malloc(sizeof *r);
    r->tag = PERL_REF_ARRAY;
    r->pval = a;
    return r;
}

PerlValue *perl_ref_hash(PerlHash *h) {
    PerlValue *r = malloc(sizeof *r);
    r->tag = PERL_REF_HASH;
    r->pval = h;
    return r;
}

PerlValue *perl_deref_scalar(PerlValue *ref) {
    if (!ref || ref->tag != PERL_REF_SCALAR) return perl_alloc_undef();
    return (PerlValue *)ref->pval;
}

PerlArray *perl_deref_array(PerlValue *ref) {
    if (!ref || ref->tag != PERL_REF_ARRAY) return perl_array_new();
    return (PerlArray *)ref->pval;
}

PerlHash *perl_deref_hash(PerlValue *ref) {
    if (!ref || ref->tag != PERL_REF_HASH) return perl_hash_new();
    return (PerlHash *)ref->pval;
}

PerlValue *perl_ref_type(PerlValue *ref) {
    if (!ref) return perl_alloc_string("");
    switch (ref->tag) {
        case PERL_REF_SCALAR: return perl_alloc_string("SCALAR");
        case PERL_REF_ARRAY:  return perl_alloc_string("ARRAY");
        case PERL_REF_HASH:   return perl_alloc_string("HASH");
        case PERL_CODE_REF:   return perl_alloc_string("CODE");
        default:              return perl_alloc_string("");
    }
}

/* ── code references ─────────────────────────────────────────────────────── */

PerlValue *perl_make_code_ref(PerlSubFn fp) {
    PerlValue *v = malloc(sizeof *v);
    v->tag = PERL_CODE_REF;
    v->pval = (void *)fp;
    v->matchpos = 0;
    return v;
}

PerlValue *perl_call_code_ref(PerlValue *ref, PerlArray *args) {
    if (!ref || ref->tag != PERL_CODE_REF || !ref->pval)
        return perl_alloc_undef();
    PerlSubFn fn = (PerlSubFn)ref->pval;
    return fn(args);
}

/* ── file I/O ────────────────────────────────────────────────────────────── */

static PerlValue s_stdin_pv  = { PERL_UNDEF, {0}, 0 };
static PerlValue s_stdout_pv = { PERL_UNDEF, {0}, 0 };
static PerlValue s_stderr_pv = { PERL_UNDEF, {0}, 0 };

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
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = (char)c;
        if (c == '\n') break;
    }
    if (len == 0) { free(buf); return perl_alloc_undef(); }
    buf[len] = '\0';
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
    PerlValue tmp = { PERL_FILEHANDLE, {.pval = NULL}, 0 };
    tmp.pval = stdin;
    return perl_readline(&tmp);
}

PerlArray *perl_readline_all_stdin(void) {
    PerlValue tmp = { PERL_FILEHANDLE, {.pval = NULL}, 0 };
    tmp.pval = stdin;
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
    char *s = msg ? perl_to_string(msg) : strdup("Died");
    if (s_eval_depth > 0) {
        /* inside eval: set $@ and longjmp back to setjmp in calling frame */
        if (s_dollar_at.sval) free(s_dollar_at.sval);
        s_dollar_at.tag  = PERL_STRING;
        s_dollar_at.sval = s;
        longjmp(*s_eval_stack[s_eval_depth - 1], 1);
    }
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
    for (long long i = lo; i <= hi; i++)
        perl_array_push(a, perl_alloc_int(i));
    return a;
}

/* ── regex (PCRE2) ───────────────────────────────────────────────────────── */

#define PERL_MAX_CAPTURES 10
static PerlValue *perl_captures_[PERL_MAX_CAPTURES + 1];  /* $1..$10 */

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

PerlValue *perl_regex_match(PerlValue *str, const char *pattern, const char *flags) {
    int errcode; PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                   pcre_flags(flags), &errcode, &erroffset, NULL);
    if (!re) return perl_alloc_int(0);

    char *s = perl_to_string(str);
    size_t slen = strlen(s);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    int rc = pcre2_match(re, (PCRE2_SPTR)s, slen, 0, 0, md, NULL);

    if (rc > 0) {
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
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
static PerlValue  perl_dollar0_val = { PERL_STRING, {0}, 0 };

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
