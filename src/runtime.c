#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdint.h>

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
    if (v->tag == PERL_FLOAT) { v->fval += 1.0; }
    else { if (v->tag != PERL_INT) { v->tag = PERL_INT; v->ival = 0; } v->ival++; }
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
        default:              return perl_alloc_string("");
    }
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
