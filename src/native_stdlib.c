/* Native FindBin, Symbol, IPC::Open2/Open3, IO::Handle/File/Socket,
   Socket, MIME::Base64, Digest::MD5, Digest::SHA. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <limits.h>
#include <libgen.h>
#include <sys/time.h>
#include <sys/select.h>
#include <ctype.h>
#include <openssl/evp.h>

#define PV_FLAG_AUTOFLUSH (1u << 25)

PerlValue *perl_stdlib_method(PerlValue *obj, const char *method, PerlArray *args);

static FILE *nst_fp(PerlValue *pv) {
    if (!pv) return NULL;
    if (pv->tag == PERL_FILEHANDLE) return (FILE *)pv->pval;
    if (pv->tag == PERL_REF_SCALAR && pv->pval)
        return nst_fp((PerlValue *)pv->pval);
    return NULL;
}

static int nst_is_io_class(const char *c) {
    if (!c) return 0;
    return strcmp(c, "IO::Handle") == 0 || strcmp(c, "IO::File") == 0 ||
           strcmp(c, "IO::Socket") == 0 || strcmp(c, "IO::Socket::INET") == 0 ||
           strcmp(c, "IO::Socket::IP") == 0 || strcmp(c, "IO::Socket::UNIX") == 0 ||
           strcmp(c, "FileHandle") == 0 || strcmp(c, "IO::Seekable") == 0;
}

static void nst_bless_fh(PerlValue *fh, const char *cls) {
    if (!fh) return;
    if (fh->blessed_class) free(fh->blessed_class);
    fh->blessed_class = strdup(cls);
}

static PerlValue *nst_new_fh(FILE *fp, const char *cls) {
    PerlValue *fh = perl_alloc_undef();
    fh->tag = PERL_FILEHANDLE;
    fh->pval = fp;
    if (cls) nst_bless_fh(fh, cls);
    return fh;
}

static const char *nst_fopen_mode(const char *m) {
    if (!m || !*m) return "r";
    if (m[0] == 'r' || m[0] == '<') {
        if (strchr(m, '+') || (m[0] == '+' && m[1] == '<')) return "r+";
        return "r";
    }
    if (m[0] == 'w' || m[0] == '>') {
        if (m[0] == '>' && m[1] == '>') return "a";
        if (strchr(m, '+')) return "w+";
        return "w";
    }
    if (m[0] == 'a') return strchr(m, '+') ? "a+" : "a";
    if (m[0] == '+') {
        if (m[1] == '<') return "r+";
        if (m[1] == '>') return "w+";
    }
    return "r";
}

/* ── FindBin ─────────────────────────────────────────────────────────────── */

static char s_fb_bin[PATH_MAX];
static char s_fb_script[PATH_MAX];
static char s_fb_realbin[PATH_MAX];
static char s_fb_realscript[PATH_MAX];
static int  s_fb_inited;

static void nst_set_pkg_str(const char *qual, const char *val) {
    /* Same glob registry D110's $FindBin::Bin read path uses. */
    PerlValue *cell = perl_glob_get_scalar(qual);
    PerlValue *sv = perl_alloc_string(val ? val : "");
    perl_assign(cell, sv);
    perl_free(sv);
}

void perl_findbin_init(const char *script_path) {
    char resolved[PATH_MAX];
    char copy[PATH_MAX];
    const char *path = script_path && script_path[0] ? script_path : "";
    const char *base;
    s_fb_inited = 1;
    if (!path[0]) path = ".";
    if (realpath(path, resolved)) {
        strncpy(s_fb_realscript, resolved, sizeof(s_fb_realscript) - 1);
        s_fb_realscript[sizeof(s_fb_realscript) - 1] = '\0';
    } else {
        strncpy(s_fb_realscript, path, sizeof(s_fb_realscript) - 1);
        s_fb_realscript[sizeof(s_fb_realscript) - 1] = '\0';
    }
    strncpy(copy, s_fb_realscript, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    {
        char *dir = dirname(copy);
        strncpy(s_fb_realbin, dir, sizeof(s_fb_realbin) - 1);
        s_fb_realbin[sizeof(s_fb_realbin) - 1] = '\0';
    }
    strncpy(s_fb_bin, s_fb_realbin, sizeof(s_fb_bin) - 1);
    base = strrchr(s_fb_realscript, '/');
    strncpy(s_fb_script, base ? base + 1 : s_fb_realscript, sizeof(s_fb_script) - 1);
    s_fb_script[sizeof(s_fb_script) - 1] = '\0';
    nst_set_pkg_str("FindBin::Bin", s_fb_bin);
    nst_set_pkg_str("FindBin::Dir", s_fb_bin);
    nst_set_pkg_str("FindBin::Script", s_fb_script);
    nst_set_pkg_str("FindBin::RealBin", s_fb_realbin);
    nst_set_pkg_str("FindBin::RealScript", s_fb_realscript);
}

PerlValue *perl_findbin_get(const char *which) {
    if (!s_fb_inited) perl_findbin_init("");
    if (!which) return perl_alloc_string("");
    if (strcmp(which, "Bin") == 0 || strcmp(which, "Dir") == 0)
        return perl_alloc_string(s_fb_bin);
    if (strcmp(which, "Script") == 0) return perl_alloc_string(s_fb_script);
    if (strcmp(which, "RealBin") == 0) return perl_alloc_string(s_fb_realbin);
    if (strcmp(which, "RealScript") == 0) return perl_alloc_string(s_fb_realscript);
    return perl_alloc_string("");
}

/* ── Symbol ──────────────────────────────────────────────────────────────── */

static int s_gensym_n;

PerlValue *perl_symbol_gensym(void) {
    char name[64];
    snprintf(name, sizeof name, "Symbol::GEN%04d", ++s_gensym_n);
    PerlValue *cell = perl_glob_get_scalar(name);
    cell->tag = PERL_FILEHANDLE;
    cell->pval = NULL;
    perl_glob_set_io(name, cell);
    return cell;
}

PerlValue *perl_symbol_qualify(PerlValue *name, PerlValue *pkg) {
    char *n = perl_to_string_dup(name);
    char *p = pkg ? perl_to_string_dup(pkg) : strdup("main");
    PerlValue *out;
    if (!n) n = strdup("");
    if (!p || !p[0]) { free(p); p = strdup("main"); }
    if (strstr(n, "::")) {
        out = perl_alloc_string(n);
    } else {
        char buf[1024];
        snprintf(buf, sizeof buf, "%s::%s", p, n);
        out = perl_alloc_string(buf);
    }
    free(n); free(p);
    return out;
}

/* ── IPC::Open2 / Open3 ──────────────────────────────────────────────────── */

static void nst_assign_fp(PerlValue *slot, FILE *fp) {
    if (!slot) { if (fp) fclose(fp); return; }
    if (slot->tag == PERL_FILEHANDLE && slot->pval && slot->pval != fp)
        fclose((FILE *)slot->pval);
    slot->tag = PERL_FILEHANDLE;
    slot->pval = fp;
    slot->matchpos = 0;
}

PerlValue *perl_open3(PerlValue *to_chld, PerlValue *from_chld, PerlValue *err_chld,
                      PerlArray *cmd, int with_err) {
    int pin[2] = {-1,-1}, pout[2] = {-1,-1}, perr[2] = {-1,-1};
    pid_t pid;
    int need_in = 1, need_out = 1, need_err = with_err;
    if (nst_fp(to_chld)) need_in = 0;
    if (nst_fp(from_chld)) need_out = 0;
    if (with_err && nst_fp(err_chld)) need_err = 0;
    if (need_in && pipe(pin) != 0) return perl_alloc_undef();
    if (need_out && pipe(pout) != 0) return perl_alloc_undef();
    if (need_err && pipe(perr) != 0) return perl_alloc_undef();
    pid = fork();
    if (pid < 0) return perl_alloc_undef();
    if (pid == 0) {
        if (need_in) {
            dup2(pin[0], STDIN_FILENO);
            close(pin[0]); close(pin[1]);
        }
        if (need_out) {
            dup2(pout[1], STDOUT_FILENO);
            close(pout[0]); close(pout[1]);
        }
        if (need_err) {
            dup2(perr[1], STDERR_FILENO);
            close(perr[0]); close(perr[1]);
        } else if (with_err && nst_fp(err_chld)) {
            int fd = fileno(nst_fp(err_chld));
            if (fd >= 0) dup2(fd, STDERR_FILENO);
        }
        if (!cmd || cmd->len < 1) _exit(127);
        if (cmd->len == 1) {
            char *line = perl_to_string_dup(cmd->elems[0]);
            execl("/bin/sh", "sh", "-c", line ? line : "", (char *)NULL);
            _exit(127);
        } else {
            char **argv = calloc((size_t)cmd->len + 1, sizeof(char *));
            long long i;
            for (i = 0; i < cmd->len; i++)
                argv[i] = perl_to_string_dup(cmd->elems[i]);
            execvp(argv[0], argv);
            _exit(127);
        }
    }
    if (need_in) {
        close(pin[0]);
        nst_assign_fp(to_chld, fdopen(pin[1], "w"));
    }
    if (need_out) {
        close(pout[1]);
        nst_assign_fp(from_chld, fdopen(pout[0], "r"));
    }
    if (need_err) {
        close(perr[1]);
        nst_assign_fp(err_chld, fdopen(perr[0], "r"));
    }
    return perl_alloc_int((long long)pid);
}

/* ── MIME::Base64 ────────────────────────────────────────────────────────── */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static PerlValue *nst_b64_enc(const unsigned char *src, size_t n,
                              const char *tbl, int pad, const char *eol, int wrap) {
    size_t outn = ((n + 2) / 3) * 4;
    size_t extra = 0, col = 0, o = 0;
    char *out;
    size_t i;
    size_t eollen = eol ? strlen(eol) : 0;
    if (wrap && eollen) extra = (outn / 76) * eollen + eollen;
    out = malloc(outn + extra + 1);
    for (i = 0; i < n; i += 3) {
        unsigned int v = (unsigned int)src[i] << 16;
        if (i + 1 < n) v |= (unsigned int)src[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned int)src[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = tbl[(v >> 6) & 63];
        else if (pad) out[o++] = '=';
        if (i + 2 < n) out[o++] = tbl[v & 63];
        else if (pad) out[o++] = '=';
        col += 4;
        if (wrap && eollen && col >= 76) {
            memcpy(out + o, eol, eollen); o += eollen; col = 0;
        }
    }
    if (wrap && eollen && o > 0 && (o < eollen || memcmp(out + o - eollen, eol, eollen) != 0)) {
        memcpy(out + o, eol, eollen); o += eollen;
    }
    out[o] = '\0';
    {
        PerlValue *pv = perl_alloc_string_len(out, (long long)o);
        free(out);
        return pv;
    }
}

static int nst_b64_val(int c, int url) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (!url && c == '+') return 62;
    if (!url && c == '/') return 63;
    if (url && c == '-') return 62;
    if (url && c == '_') return 63;
    return -1;
}

static PerlValue *nst_b64_dec(const char *s, long long slen, int url) {
    unsigned char *out = malloc((size_t)slen + 4);
    size_t o = 0;
    int acc = 0, nbits = 0;
    long long i;
    for (i = 0; i < slen; i++) {
        int v = nst_b64_val((unsigned char)s[i], url);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out[o++] = (unsigned char)((acc >> nbits) & 0xff);
        }
    }
    {
        PerlValue *pv = perl_alloc_string_len((char *)out, (long long)o);
        free(out);
        return pv;
    }
}

PerlValue *perl_b64_encode(PerlValue *data, PerlValue *eol) {
    long long n = 0;
    char *s = perl_to_string_dup_len(data, &n);
    const char *e = "\n";
    int wrap = 1;
    PerlValue *r;
    if (eol) {
        if (eol->tag == PERL_UNDEF) { e = "\n"; wrap = 1; }
        else {
            e = perl_to_string(eol);
            wrap = e && e[0];
        }
    }
    r = nst_b64_enc((unsigned char *)(s ? s : ""), (size_t)n, B64, 1, e, wrap);
    free(s);
    return r;
}

PerlValue *perl_b64_decode(PerlValue *data) {
    long long n = 0;
    char *s = perl_to_string_dup_len(data, &n);
    PerlValue *r = nst_b64_dec(s ? s : "", n, 0);
    free(s);
    return r;
}

PerlValue *perl_b64_encode_url(PerlValue *data) {
    long long n = 0;
    char *s = perl_to_string_dup_len(data, &n);
    PerlValue *r = nst_b64_enc((unsigned char *)(s ? s : ""), (size_t)n, B64URL, 0, "", 0);
    free(s);
    return r;
}

PerlValue *perl_b64_decode_url(PerlValue *data) {
    long long n = 0;
    char *s = perl_to_string_dup_len(data, &n);
    PerlValue *r = nst_b64_dec(s ? s : "", n, 1);
    free(s);
    return r;
}

/* ── Digest ──────────────────────────────────────────────────────────────── */

typedef struct {
    const EVP_MD *md;
    EVP_MD_CTX *ctx;
    int refcount;
} PerlDigest;

static PerlValue *nst_digest_new(const EVP_MD *md, const char *cls) {
    PerlDigest *d = calloc(1, sizeof *d);
    PerlValue *pv;
    d->md = md;
    d->refcount = 1;
    d->ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(d->ctx, md, NULL);
    pv = perl_alloc_xs_ptr(d);
    pv->blessed_class = strdup(cls);
    return pv;
}

static PerlDigest *nst_digest(PerlValue *obj) {
    if (!obj || obj->tag != PERL_XS_PTR || !obj->pval) return NULL;
    if (!obj->blessed_class) return NULL;
    if (strncmp(obj->blessed_class, "Digest::", 8) != 0) return NULL;
    return (PerlDigest *)obj->pval;
}

void perl_digest_retain(PerlValue *v) {
    PerlDigest *d = nst_digest(v);
    if (d) d->refcount++;
}

void perl_digest_free_pv(PerlValue *v) {
    PerlDigest *d = nst_digest(v);
    if (!d) return;
    v->pval = NULL;
    if (--d->refcount > 0) return;
    if (d->ctx) EVP_MD_CTX_free(d->ctx);
    free(d);
}

static PerlValue *nst_digest_bytes(const EVP_MD *md, PerlArray *args) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    long long i;
    EVP_DigestInit_ex(ctx, md, NULL);
    for (i = 0; args && i < args->len; i++) {
        long long ln = 0;
        char *s = perl_to_string_dup_len(args->elems[i], &ln);
        if (s && ln) EVP_DigestUpdate(ctx, s, (size_t)ln);
        free(s);
    }
    EVP_DigestFinal_ex(ctx, out, &n);
    EVP_MD_CTX_free(ctx);
    return perl_alloc_string_len((char *)out, (long long)n);
}

static PerlValue *nst_hex(PerlValue *raw) {
    long long n = 0;
    char *s = perl_to_string_dup_len(raw, &n);
    char *hex = malloc((size_t)n * 2 + 1);
    long long i;
    for (i = 0; i < n; i++)
        sprintf(hex + i * 2, "%02x", (unsigned char)s[i]);
    hex[n * 2] = '\0';
    {
        PerlValue *pv = perl_alloc_string_len(hex, n * 2);
        free(hex); free(s); perl_free(raw);
        return pv;
    }
}

static PerlValue *nst_b64_nopad(PerlValue *raw) {
    long long n = 0;
    char *s = perl_to_string_dup_len(raw, &n);
    PerlValue *enc = nst_b64_enc((unsigned char *)(s ? s : ""), (size_t)n, B64, 0, "", 0);
    free(s); perl_free(raw);
    return enc;
}

static const EVP_MD *nst_md_by_name(const char *n) {
    if (!n) return EVP_sha256();
    if (strcmp(n, "MD5") == 0 || strcmp(n, "md5") == 0) return EVP_md5();
    if (strcmp(n, "SHA1") == 0 || strcmp(n, "SHA-1") == 0 || strcmp(n, "1") == 0)
        return EVP_sha1();
    if (strcmp(n, "SHA224") == 0 || strcmp(n, "SHA-224") == 0 || strcmp(n, "224") == 0)
        return EVP_sha224();
    if (strcmp(n, "SHA256") == 0 || strcmp(n, "SHA-256") == 0 || strcmp(n, "256") == 0)
        return EVP_sha256();
    if (strcmp(n, "SHA384") == 0 || strcmp(n, "SHA-384") == 0 || strcmp(n, "384") == 0)
        return EVP_sha384();
    if (strcmp(n, "SHA512") == 0 || strcmp(n, "SHA-512") == 0 || strcmp(n, "512") == 0)
        return EVP_sha512();
    return EVP_sha256();
}

PerlValue *perl_digest_call(const char *name, PerlArray *args) {
    const char *n = name ? name : "";
    const char *bare = strrchr(n, ':');
    if (bare) bare++; else bare = n;
    if (strcmp(bare, "md5") == 0) return nst_digest_bytes(EVP_md5(), args);
    if (strcmp(bare, "md5_hex") == 0) return nst_hex(nst_digest_bytes(EVP_md5(), args));
    if (strcmp(bare, "md5_base64") == 0) return nst_b64_nopad(nst_digest_bytes(EVP_md5(), args));
    if (strcmp(bare, "sha1") == 0) return nst_digest_bytes(EVP_sha1(), args);
    if (strcmp(bare, "sha1_hex") == 0) return nst_hex(nst_digest_bytes(EVP_sha1(), args));
    if (strcmp(bare, "sha1_base64") == 0) return nst_b64_nopad(nst_digest_bytes(EVP_sha1(), args));
    if (strcmp(bare, "sha256") == 0) return nst_digest_bytes(EVP_sha256(), args);
    if (strcmp(bare, "sha256_hex") == 0) return nst_hex(nst_digest_bytes(EVP_sha256(), args));
    if (strcmp(bare, "sha256_base64") == 0) return nst_b64_nopad(nst_digest_bytes(EVP_sha256(), args));
    if (strcmp(bare, "sha384") == 0) return nst_digest_bytes(EVP_sha384(), args);
    if (strcmp(bare, "sha384_hex") == 0) return nst_hex(nst_digest_bytes(EVP_sha384(), args));
    if (strcmp(bare, "sha512") == 0) return nst_digest_bytes(EVP_sha512(), args);
    if (strcmp(bare, "sha512_hex") == 0) return nst_hex(nst_digest_bytes(EVP_sha512(), args));
    if (strcmp(bare, "sha512_base64") == 0) return nst_b64_nopad(nst_digest_bytes(EVP_sha512(), args));
    return perl_alloc_undef();
}

PerlValue *perl_digest_method(PerlValue *obj, const char *method, PerlArray *args) {
    PerlDigest *d;
    if (obj && obj->tag == PERL_STRING && obj->sval) {
        if (strcmp(method, "new") == 0) {
            if (strcmp(obj->sval, "Digest::MD5") == 0)
                return nst_digest_new(EVP_md5(), "Digest::MD5");
            if (strcmp(obj->sval, "Digest::SHA") == 0) {
                const char *alg = "256";
                if (args && args->len > 0) alg = perl_to_string(args->elems[0]);
                return nst_digest_new(nst_md_by_name(alg), "Digest::SHA");
            }
            if (strcmp(obj->sval, "Digest") == 0) {
                const char *alg = args && args->len ? perl_to_string(args->elems[0]) : "SHA-256";
                if (strcmp(alg, "MD5") == 0 || strcmp(alg, "md5") == 0)
                    return nst_digest_new(EVP_md5(), "Digest::MD5");
                return nst_digest_new(nst_md_by_name(alg), "Digest::SHA");
            }
        }
    }
    d = nst_digest(obj);
    if (!d) return NULL;
    if (strcmp(method, "add") == 0) {
        long long i;
        for (i = 0; args && i < args->len; i++) {
            long long ln = 0;
            char *s = perl_to_string_dup_len(args->elems[i], &ln);
            if (s && ln) EVP_DigestUpdate(d->ctx, s, (size_t)ln);
            free(s);
        }
        return perl_clone(obj);
    }
    if (strcmp(method, "addfile") == 0) {
        FILE *fp = args && args->len ? nst_fp(args->elems[0]) : NULL;
        char buf[4096];
        size_t n;
        int close_fp = 0;
        if (!fp && args && args->len) {
            char *path = perl_to_string_dup(args->elems[0]);
            fp = fopen(path ? path : "", "rb");
            close_fp = 1;
            free(path);
        }
        if (!fp) return perl_clone(obj);
        while ((n = fread(buf, 1, sizeof buf, fp)) > 0)
            EVP_DigestUpdate(d->ctx, buf, n);
        if (close_fp) fclose(fp);
        return perl_clone(obj);
    }
    if (strcmp(method, "digest") == 0 || strcmp(method, "hexdigest") == 0 ||
        strcmp(method, "b64digest") == 0) {
        unsigned char out[EVP_MAX_MD_SIZE];
        unsigned int n = 0;
        PerlValue *raw;
        EVP_DigestFinal_ex(d->ctx, out, &n);
        EVP_DigestInit_ex(d->ctx, d->md, NULL);
        raw = perl_alloc_string_len((char *)out, (long long)n);
        if (strcmp(method, "hexdigest") == 0) return nst_hex(raw);
        if (strcmp(method, "b64digest") == 0) return nst_b64_nopad(raw);
        return raw;
    }
    if (strcmp(method, "reset") == 0 || strcmp(method, "new") == 0) {
        EVP_DigestInit_ex(d->ctx, d->md, NULL);
        return perl_clone(obj);
    }
    if (strcmp(method, "clone") == 0) {
        PerlDigest *c = calloc(1, sizeof *c);
        PerlValue *pv;
        c->md = d->md;
        c->refcount = 1;
        c->ctx = EVP_MD_CTX_new();
        EVP_MD_CTX_copy_ex(c->ctx, d->ctx);
        pv = perl_alloc_xs_ptr(c);
        pv->blessed_class = strdup(obj->blessed_class);
        return pv;
    }
    if (strcmp(method, "DESTROY") == 0) {
        perl_digest_free_pv(obj);
        return perl_alloc_undef();
    }
    return perl_alloc_undef();
}

/* ── Socket ──────────────────────────────────────────────────────────────── */

PerlValue *perl_socket_const(const char *name) {
    const char *n = name ? name : "";
    const char *bare = strrchr(n, ':');
    if (bare && bare[1]) n = bare + 1;
#define C(sym) do { if (strcmp(n, #sym) == 0) return perl_alloc_int((long long)sym); } while (0)
    C(AF_INET); C(AF_INET6); C(AF_UNIX); C(AF_UNSPEC);
    C(PF_INET); C(PF_INET6); C(PF_UNIX);
    C(SOCK_STREAM); C(SOCK_DGRAM); C(SOCK_RAW);
#ifdef SOCK_SEQPACKET
    C(SOCK_SEQPACKET);
#endif
    C(SOL_SOCKET); C(SO_REUSEADDR); C(SO_KEEPALIVE); C(SO_LINGER);
    C(SO_BROADCAST); C(SO_OOBINLINE); C(SO_SNDBUF); C(SO_RCVBUF);
    C(SO_ERROR); C(SO_TYPE);
#ifdef SO_REUSEPORT
    C(SO_REUSEPORT);
#endif
    C(SHUT_RD); C(SHUT_WR); C(SHUT_RDWR);
    C(SOMAXCONN);
#ifdef MSG_NOSIGNAL
    C(MSG_NOSIGNAL);
#endif
    C(MSG_OOB); C(MSG_PEEK); C(MSG_DONTROUTE);
    C(IPPROTO_TCP); C(IPPROTO_UDP); C(IPPROTO_IP);
#undef C
    if (strcmp(n, "INADDR_ANY") == 0)
        return perl_alloc_string_len("\0\0\0\0", 4);
    if (strcmp(n, "INADDR_BROADCAST") == 0)
        return perl_alloc_string_len("\xff\xff\xff\xff", 4);
    if (strcmp(n, "INADDR_LOOPBACK") == 0)
        return perl_alloc_string_len("\x7f\0\0\x01", 4);
    if (strcmp(n, "INADDR_NONE") == 0)
        return perl_alloc_string_len("\xff\xff\xff\xff", 4);
    return perl_alloc_undef();
}

static PerlValue *nst_pack4(const void *p) {
    return perl_alloc_string_len((const char *)p, 4);
}

PerlValue *perl_socket_call(const char *name, PerlArray *args) {
    const char *n = name ? name : "";
    const char *bare = strrchr(n, ':');
    if (bare && bare[1]) n = bare + 1;
    if (strcmp(n, "inet_aton") == 0) {
        char *s = args && args->len ? perl_to_string_dup(args->elems[0]) : strdup("");
        struct in_addr a;
        PerlValue *r;
        if (inet_aton(s ? s : "", &a))
            r = nst_pack4(&a);
        else {
            struct hostent *he = gethostbyname(s ? s : "");
            if (he && he->h_addr_list && he->h_addr_list[0])
                r = nst_pack4(he->h_addr_list[0]);
            else
                r = perl_alloc_undef();
        }
        free(s);
        return r;
    }
    if (strcmp(n, "inet_ntoa") == 0) {
        long long ln = 0;
        char *s = args && args->len ? perl_to_string_dup_len(args->elems[0], &ln) : NULL;
        struct in_addr a;
        PerlValue *r;
        memset(&a, 0, sizeof a);
        if (s && ln >= 4) memcpy(&a, s, 4);
        r = perl_alloc_string(inet_ntoa(a));
        free(s);
        return r;
    }
    if (strcmp(n, "pack_sockaddr_in") == 0 ||
        (strcmp(n, "sockaddr_in") == 0 && args && args->len >= 2)) {
        long long port = args && args->len ? perl_to_int(args->elems[0]) : 0;
        long long ln = 0;
        char *addr = args && args->len > 1 ? perl_to_string_dup_len(args->elems[1], &ln) : NULL;
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof sin);
        sin.sin_family = AF_INET;
        sin.sin_port = htons((uint16_t)port);
        if (addr && ln >= 4) memcpy(&sin.sin_addr, addr, 4);
        free(addr);
        return perl_alloc_string_len((char *)&sin, sizeof sin);
    }
    if (strcmp(n, "sockaddr_in") == 0) {
        /* 1-arg form unpacks; scalar context yields the address (last). */
        PerlArray *av = perl_socket_unpack_in(args && args->len ? args->elems[0] : NULL);
        PerlValue *r = perl_array_get(av, 1);
        perl_array_free(av);
        return r;
    }
    if (strcmp(n, "sockaddr_family") == 0) {
        long long ln = 0;
        char *s = args && args->len ? perl_to_string_dup_len(args->elems[0], &ln) : NULL;
        int fam = 0;
        if (s && ln >= (long long)sizeof(sa_family_t))
            memcpy(&fam, s, sizeof(sa_family_t));
        free(s);
        return perl_alloc_int(fam);
    }
    if (strcmp(n, "pack_sockaddr_un") == 0 ||
        (strcmp(n, "sockaddr_un") == 0 && args && args->len != 1)) {
        char *path = args && args->len ? perl_to_string_dup(args->elems[0]) : strdup("");
        struct sockaddr_un un;
        memset(&un, 0, sizeof un);
        un.sun_family = AF_UNIX;
        strncpy(un.sun_path, path ? path : "", sizeof(un.sun_path) - 1);
        free(path);
        return perl_alloc_string_len((char *)&un, sizeof un);
    }
    if (strcmp(n, "sockaddr_un") == 0) {
        PerlArray *av = perl_socket_unpack_un(args && args->len ? args->elems[0] : NULL);
        PerlValue *r = perl_array_get(av, 0);
        perl_array_free(av);
        return r;
    }
    if (strcmp(n, "inet_pton") == 0) {
        int af = args && args->len ? (int)perl_to_int(args->elems[0]) : AF_INET;
        char *s = args && args->len > 1 ? perl_to_string_dup(args->elems[1]) : strdup("");
        unsigned char buf[16];
        PerlValue *r;
        if (inet_pton(af, s ? s : "", buf) == 1)
            r = perl_alloc_string_len((char *)buf, af == AF_INET6 ? 16 : 4);
        else
            r = perl_alloc_undef();
        free(s);
        return r;
    }
    if (strcmp(n, "inet_ntop") == 0) {
        int af = args && args->len ? (int)perl_to_int(args->elems[0]) : AF_INET;
        long long ln = 0;
        char *s = args && args->len > 1 ? perl_to_string_dup_len(args->elems[1], &ln) : NULL;
        char out[INET6_ADDRSTRLEN];
        PerlValue *r;
        if (s && inet_ntop(af, s, out, sizeof out))
            r = perl_alloc_string(out);
        else
            r = perl_alloc_undef();
        free(s);
        return r;
    }
    return perl_socket_const(n);
}

PerlArray *perl_socket_unpack_in(PerlValue *sa) {
    long long ln = 0;
    char *s = perl_to_string_dup_len(sa, &ln);
    struct sockaddr_in sin;
    PerlArray *av = perl_array_new();
    memset(&sin, 0, sizeof sin);
    if (s && ln >= (long long)sizeof(sin.sin_family) + 2 + 4)
        memcpy(&sin, s, (size_t)(ln < (long long)sizeof sin ? ln : (long long)sizeof sin));
    perl_array_push(av, perl_alloc_int(ntohs(sin.sin_port)));
    perl_array_push(av, nst_pack4(&sin.sin_addr));
    free(s);
    return av;
}

PerlArray *perl_socket_unpack_un(PerlValue *sa) {
    long long ln = 0;
    char *s = perl_to_string_dup_len(sa, &ln);
    struct sockaddr_un un;
    PerlArray *av = perl_array_new();
    memset(&un, 0, sizeof un);
    if (s && ln > 0)
        memcpy(&un, s, (size_t)(ln < (long long)sizeof un ? ln : (long long)sizeof un));
    perl_array_push(av, perl_alloc_string(un.sun_path));
    free(s);
    return av;
}

/* ── IO::Socket::INET / IP ───────────────────────────────────────────────── */

static PerlValue *nst_opt(PerlArray *args, const char *key) {
    long long i;
    if (!args) return NULL;
    for (i = 0; i + 1 < args->len; i += 2) {
        char *k = perl_to_string_dup(args->elems[i]);
        int hit = k && strcasecmp(k, key) == 0;
        free(k);
        if (hit) return args->elems[i + 1];
    }
    return NULL;
}

static int nst_truth(PerlValue *pv) {
    if (!pv || pv->tag == PERL_UNDEF) return 0;
    if (pv->tag == PERL_INT) return pv->ival != 0;
    if (pv->tag == PERL_STRING) return pv->slen > 0;
    return 1;
}

static void nst_parse_hostport(const char *s, char *host, size_t hostn, int *port) {
    const char *colon;
    host[0] = '\0';
    if (!s) return;
    colon = strrchr(s, ':');
    if (colon && colon != s) {
        size_t n = (size_t)(colon - s);
        if (n >= hostn) n = hostn - 1;
        memcpy(host, s, n); host[n] = '\0';
        *port = atoi(colon + 1);
    } else {
        strncpy(host, s, hostn - 1);
        host[hostn - 1] = '\0';
    }
}

static PerlValue *nst_sock_new(PerlArray *args, const char *cls) {
    PerlValue *peer = nst_opt(args, "PeerAddr");
    if (!peer) peer = nst_opt(args, "PeerHost");
    PerlValue *pport = nst_opt(args, "PeerPort");
    PerlValue *local = nst_opt(args, "LocalAddr");
    if (!local) local = nst_opt(args, "LocalHost");
    PerlValue *lport = nst_opt(args, "LocalPort");
    PerlValue *listen_pv = nst_opt(args, "Listen");
    PerlValue *reuse = nst_opt(args, "Reuse");
    if (!reuse) reuse = nst_opt(args, "ReuseAddr");
    PerlValue *proto = nst_opt(args, "Proto");
    int socktype = SOCK_STREAM;
    int fd, on = 1;
    struct sockaddr_in sin;
    char host[256] = "";
    int port = 0;
    FILE *fp;
    PerlValue *fh;
    if (proto) {
        char *ps = perl_to_string_dup(proto);
        if (ps && (strcmp(ps, "udp") == 0 || strcmp(ps, "UDP") == 0))
            socktype = SOCK_DGRAM;
        free(ps);
    }
    if (peer) {
        char *ps = perl_to_string_dup(peer);
        nst_parse_hostport(ps, host, sizeof host, &port);
        free(ps);
    }
    if (pport) {
        char *ps = perl_to_string_dup(pport);
        if (ps && ps[0]) port = atoi(ps);
        free(ps);
    }
    fd = socket(AF_INET, socktype, 0);
    if (fd < 0) return perl_alloc_undef();
    if (nst_truth(reuse))
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    if (local || lport || listen_pv) {
        if (local) {
            char *ls = perl_to_string_dup(local);
            inet_aton(ls && ls[0] ? ls : "0.0.0.0", &sin.sin_addr);
            free(ls);
        } else {
            sin.sin_addr.s_addr = htonl(INADDR_ANY);
        }
        if (lport) sin.sin_port = htons((uint16_t)perl_to_int(lport));
        if (bind(fd, (struct sockaddr *)&sin, sizeof sin) != 0) {
            close(fd); return perl_alloc_undef();
        }
    }
    if (listen_pv) {
        int bl = (int)perl_to_int(listen_pv);
        if (bl <= 0) bl = SOMAXCONN;
        if (listen(fd, bl) != 0) { close(fd); return perl_alloc_undef(); }
    }
    if (host[0] && port > 0 && socktype == SOCK_STREAM) {
        struct in_addr a;
        memset(&sin, 0, sizeof sin);
        sin.sin_family = AF_INET;
        sin.sin_port = htons((uint16_t)port);
        if (!inet_aton(host, &a)) {
            struct hostent *he = gethostbyname(host);
            if (!he || !he->h_addr_list[0]) { close(fd); return perl_alloc_undef(); }
            memcpy(&a, he->h_addr_list[0], 4);
        }
        sin.sin_addr = a;
        if (connect(fd, (struct sockaddr *)&sin, sizeof sin) != 0) {
            close(fd); return perl_alloc_undef();
        }
    }
    fp = fdopen(fd, "r+");
    if (!fp) { close(fd); return perl_alloc_undef(); }
    setvbuf(fp, NULL, _IONBF, 0);
    fh = nst_new_fh(fp, cls);
    fh->flags |= PV_FLAG_AUTOFLUSH;
    return fh;
}

static PerlValue *nst_unix_new(PerlArray *args) {
    PerlValue *local = nst_opt(args, "Local");
    PerlValue *peer = nst_opt(args, "Peer");
    PerlValue *listen_pv = nst_opt(args, "Listen");
    int fd, type = SOCK_STREAM;
    struct sockaddr_un un;
    FILE *fp;
    PerlValue *fh;
    PerlValue *tp = nst_opt(args, "Type");
    if (tp) type = (int)perl_to_int(tp);
    fd = socket(AF_UNIX, type, 0);
    if (fd < 0) return perl_alloc_undef();
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    if (local) {
        char *p = perl_to_string_dup(local);
        strncpy(un.sun_path, p ? p : "", sizeof(un.sun_path) - 1);
        free(p);
        unlink(un.sun_path);
        if (bind(fd, (struct sockaddr *)&un, sizeof un) != 0) { close(fd); return perl_alloc_undef(); }
        if (listen_pv) {
            int bl = (int)perl_to_int(listen_pv);
            if (bl <= 0) bl = SOMAXCONN;
            if (listen(fd, bl) != 0) { close(fd); return perl_alloc_undef(); }
        }
    }
    if (peer) {
        char *p = perl_to_string_dup(peer);
        memset(&un, 0, sizeof un);
        un.sun_family = AF_UNIX;
        strncpy(un.sun_path, p ? p : "", sizeof(un.sun_path) - 1);
        free(p);
        if (connect(fd, (struct sockaddr *)&un, sizeof un) != 0) { close(fd); return perl_alloc_undef(); }
    }
    fp = fdopen(fd, "r+");
    if (!fp) { close(fd); return perl_alloc_undef(); }
    setvbuf(fp, NULL, _IONBF, 0);
    fh = nst_new_fh(fp, "IO::Socket::UNIX");
    fh->flags |= PV_FLAG_AUTOFLUSH;
    return fh;
}

static PerlValue *nst_pipe_new(void) {
    int fds[2];
    PerlHash *h;
    PerlValue *obj, *r, *w;
    FILE *rf, *wf;
    if (pipe(fds) != 0) return perl_alloc_undef();
    rf = fdopen(fds[0], "r");
    wf = fdopen(fds[1], "w");
    if (!rf || !wf) return perl_alloc_undef();
    r = nst_new_fh(rf, "IO::Handle");
    w = nst_new_fh(wf, "IO::Handle");
    h = perl_anon_hash_new();
    perl_hash_set_str(h, "r", r); perl_hash_set_str(h, "w", w);
    perl_free(r); perl_free(w);
    obj = perl_ref_hash(h);
    if (obj->blessed_class) free(obj->blessed_class);
    obj->blessed_class = strdup("IO::Pipe");
    return obj;
}

static PerlValue *nst_sockname(PerlValue *obj, int peer, int want_port) {
    FILE *fp = nst_fp(obj);
    struct sockaddr_in sin;
    socklen_t sl = sizeof sin;
    int fd;
    if (!fp) return perl_alloc_undef();
    fd = fileno(fp);
    memset(&sin, 0, sizeof sin);
    if (peer) {
        if (getpeername(fd, (struct sockaddr *)&sin, &sl) != 0)
            return perl_alloc_undef();
    } else {
        if (getsockname(fd, (struct sockaddr *)&sin, &sl) != 0)
            return perl_alloc_undef();
    }
    if (want_port) return perl_alloc_int(ntohs(sin.sin_port));
    return perl_alloc_string(inet_ntoa(sin.sin_addr));
}

/* ── IO::Handle / IO::File ───────────────────────────────────────────────── */

static PerlValue *nst_io_open(PerlArray *args, const char *cls) {
    char *path = NULL, *mode = NULL;
    FILE *fp;
    PerlValue *fh;
    if (args && args->len >= 1) path = perl_to_string_dup(args->elems[0]);
    if (args && args->len >= 2) mode = perl_to_string_dup(args->elems[1]);
    if (!path) { free(mode); return perl_alloc_undef(); }
    fp = fopen(path, nst_fopen_mode(mode));
    free(path); free(mode);
    if (!fp) return perl_alloc_undef();
    fh = nst_new_fh(fp, cls ? cls : "IO::File");
    return fh;
}

PerlValue *perl_io_method(PerlValue *obj, const char *method, PerlArray *args) {
    const char *cls = NULL;
    FILE *fp;
    if (!method) return NULL;
    if (obj && obj->tag == PERL_STRING && obj->sval)
        cls = obj->sval;
    else if (obj && obj->blessed_class)
        cls = obj->blessed_class;
    /* Do not steal Text::CSV->print / getline etc. */
    if (!nst_is_io_class(cls) && !(obj && obj->tag == PERL_FILEHANDLE))
        return NULL;

    if (cls && nst_is_io_class(cls) && strcmp(method, "new") == 0) {
        if (strcmp(cls, "IO::Socket::INET") == 0 || strcmp(cls, "IO::Socket::IP") == 0 ||
            strcmp(cls, "IO::Socket") == 0)
            return nst_sock_new(args, cls[11] ? cls : "IO::Socket::INET");
        if (strcmp(cls, "IO::Socket::UNIX") == 0)
            return nst_unix_new(args);
        if (strcmp(cls, "IO::File") == 0 || strcmp(cls, "IO::Handle") == 0 ||
            strcmp(cls, "FileHandle") == 0) {
            if (args && args->len >= 1)
                return nst_io_open(args, strcmp(cls, "FileHandle") == 0 ? "FileHandle" : "IO::File");
            return nst_new_fh(NULL, strcmp(cls, "FileHandle") == 0 ? "FileHandle" : "IO::File");
        }
        /* IO::Pipe is a hash object; handled in perl_wave45_method */
    }
    if (cls && strcmp(cls, "IO::File") == 0 && strcmp(method, "new_tmpfile") == 0) {
        FILE *t = tmpfile();
        if (!t) return perl_alloc_undef();
        return nst_new_fh(t, "IO::File");
    }

    fp = nst_fp(obj);

    if (strcmp(method, "open") == 0) {
        PerlValue *opened = nst_io_open(args, cls && nst_is_io_class(cls) ? cls : "IO::File");
        if (opened->tag == PERL_FILEHANDLE && obj) {
            if (obj->tag == PERL_FILEHANDLE && obj->pval && obj->pval != opened->pval)
                fclose((FILE *)obj->pval);
            obj->tag = PERL_FILEHANDLE;
            obj->pval = opened->pval;
            nst_bless_fh(obj, "IO::File");
            opened->pval = NULL;
            perl_free(opened);
            return perl_alloc_int(1);
        }
        perl_free(opened);
        return perl_alloc_undef();
    }
    if (strcmp(method, "fdopen") == 0) {
        int fd = args && args->len ? (int)perl_to_int(args->elems[0]) : -1;
        char *mode = args && args->len > 1 ? perl_to_string_dup(args->elems[1]) : strdup("r");
        FILE *nfp;
        if (fd < 0) { free(mode); return perl_alloc_undef(); }
        nfp = fdopen(fd, nst_fopen_mode(mode));
        free(mode);
        if (!nfp) return perl_alloc_undef();
        if (obj) {
            obj->tag = PERL_FILEHANDLE;
            obj->pval = nfp;
            nst_bless_fh(obj, cls && nst_is_io_class(cls) ? cls : "IO::Handle");
        }
        return perl_alloc_int(1);
    }
    if (strcmp(method, "getline") == 0) return perl_readline(obj);
    if (strcmp(method, "getlines") == 0) {
        PerlArray *av = perl_readline_all(obj);
        return perl_array_to_list_return(av);
    }
    if (strcmp(method, "autoflush") == 0) {
        long long old = obj && (obj->flags & PV_FLAG_AUTOFLUSH) ? 1 : 0;
        if (args && args->len) {
            if (nst_truth(args->elems[0])) {
                if (obj) obj->flags |= PV_FLAG_AUTOFLUSH;
                if (fp) setvbuf(fp, NULL, _IONBF, 0);
            } else {
                if (obj) obj->flags &= ~PV_FLAG_AUTOFLUSH;
            }
        }
        return perl_alloc_int(old);
    }
    if (strcmp(method, "flush") == 0) {
        if (fp) fflush(fp);
        return perl_alloc_int(0);
    }
    if (strcmp(method, "sync") == 0) {
        if (fp) { fflush(fp); fsync(fileno(fp)); }
        return perl_alloc_int(0);
    }
    if (strcmp(method, "print") == 0 || strcmp(method, "say") == 0) {
        long long i;
        for (i = 0; args && i < args->len; i++)
            perl_print_fh(obj, args->elems[i]);
        if (strcmp(method, "say") == 0) {
            PerlValue *nl = perl_alloc_string("\n");
            perl_print_fh(obj, nl);
            perl_free(nl);
        }
        if (obj && (obj->flags & PV_FLAG_AUTOFLUSH) && fp) fflush(fp);
        return perl_alloc_int(1);
    }
    if (strcmp(method, "printf") == 0) {
        /* fall back to concatenating sprintf via print of first+rest as raw */
        if (args && args->len) {
            PerlValue *fmt = args->elems[0];
            perl_print_fh(obj, fmt);
            long long i;
            for (i = 1; i < args->len; i++)
                perl_print_fh(obj, args->elems[i]);
        }
        if (obj && (obj->flags & PV_FLAG_AUTOFLUSH) && fp) fflush(fp);
        return perl_alloc_int(1);
    }
    if (strcmp(method, "close") == 0) {
        perl_close_fh(obj);
        return perl_alloc_int(1);
    }
    if (strcmp(method, "eof") == 0) {
        if (!fp) return perl_alloc_int(1);
        return perl_alloc_int(feof(fp) ? 1 : 0);
    }
    if (strcmp(method, "fileno") == 0) {
        if (!fp) return perl_alloc_undef();
        return perl_alloc_int(fileno(fp));
    }
    if (strcmp(method, "opened") == 0)
        return perl_alloc_bool(fp ? 1 : 0);
    if (strcmp(method, "error") == 0)
        return perl_alloc_int(fp && ferror(fp) ? 1 : 0);
    if (strcmp(method, "clearerr") == 0) {
        if (fp) clearerr(fp);
        return perl_alloc_undef();
    }
    if (strcmp(method, "binmode") == 0)
        return perl_alloc_int(1);
    if (strcmp(method, "seek") == 0) {
        long long off = args && args->len ? perl_to_int(args->elems[0]) : 0;
        int wh = args && args->len > 1 ? (int)perl_to_int(args->elems[1]) : SEEK_SET;
        if (!fp) return perl_alloc_undef();
        return perl_alloc_int(fseek(fp, (long)off, wh) == 0 ? 1 : 0);
    }
    if (strcmp(method, "tell") == 0) {
        if (!fp) return perl_alloc_int(-1);
        return perl_alloc_int((long long)ftell(fp));
    }
    if (strcmp(method, "truncate") == 0) {
        long long off = args && args->len ? perl_to_int(args->elems[0]) : 0;
        if (!fp) return perl_alloc_undef();
        fflush(fp);
        return perl_alloc_int(ftruncate(fileno(fp), (off_t)off) == 0 ? 1 : 0);
    }
    if (strcmp(method, "read") == 0 || strcmp(method, "sysread") == 0) {
        PerlValue *buf = args && args->len ? args->elems[0] : NULL;
        long long want = args && args->len > 1 ? perl_to_int(args->elems[1]) : 0;
        long long off = args && args->len > 2 ? perl_to_int(args->elems[2]) : 0;
        char *tmp;
        size_t got;
        if (!fp || !buf || want < 0) return perl_alloc_undef();
        tmp = malloc((size_t)want);
        got = fread(tmp, 1, (size_t)want, fp);
        {
            long long oldn = 0;
            char *old = perl_to_string_dup_len(buf, &oldn);
            long long newn = off + (long long)got;
            char *nbuf;
            if (off < 0) off = 0;
            if (oldn < off) {
                nbuf = calloc((size_t)newn + 1, 1);
                if (old && oldn > 0) memcpy(nbuf, old, (size_t)oldn);
            } else {
                nbuf = malloc((size_t)newn + 1);
                if (old && off > 0) memcpy(nbuf, old, (size_t)off);
            }
            memcpy(nbuf + off, tmp, got);
            nbuf[newn] = '\0';
            if (buf->tag == PERL_STRING && buf->sval) free(buf->sval);
            buf->tag = PERL_STRING;
            buf->sval = nbuf;
            buf->slen = newn;
            free(old);
        }
        free(tmp);
        return perl_alloc_int((long long)got);
    }
    if (strcmp(method, "syswrite") == 0 || strcmp(method, "write") == 0) {
        PerlValue *buf = args && args->len ? args->elems[0] : NULL;
        long long ln = 0;
        char *s = buf ? perl_to_string_dup_len(buf, &ln) : NULL;
        long long want = args && args->len > 1 ? perl_to_int(args->elems[1]) : ln;
        long long off = args && args->len > 2 ? perl_to_int(args->elems[2]) : 0;
        size_t w;
        if (!fp || !s) { free(s); return perl_alloc_undef(); }
        if (off > ln) off = ln;
        if (want > ln - off) want = ln - off;
        w = fwrite(s + off, 1, (size_t)want, fp);
        if (obj && (obj->flags & PV_FLAG_AUTOFLUSH)) fflush(fp);
        free(s);
        return perl_alloc_int((long long)w);
    }
    if (strcmp(method, "blocking") == 0) {
        int fd, fl;
        if (!fp) return perl_alloc_undef();
        fd = fileno(fp);
        fl = fcntl(fd, F_GETFL, 0);
        if (args && args->len) {
            if (nst_truth(args->elems[0])) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
            else fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        }
        return perl_alloc_int((fl & O_NONBLOCK) ? 0 : 1);
    }
    if (strcmp(method, "accept") == 0) {
        struct sockaddr_in sin;
        socklen_t sl = sizeof sin;
        int nfd, fd;
        FILE *nfp;
        PerlValue *nfh;
        if (!fp) return perl_alloc_undef();
        fd = fileno(fp);
        nfd = accept(fd, (struct sockaddr *)&sin, &sl);
        if (nfd < 0) return perl_alloc_undef();
        nfp = fdopen(nfd, "r+");
        if (!nfp) { close(nfd); return perl_alloc_undef(); }
        setvbuf(nfp, NULL, _IONBF, 0);
        nfh = nst_new_fh(nfp, cls ? cls : "IO::Socket::INET");
        nfh->flags |= PV_FLAG_AUTOFLUSH;
        return nfh;
    }
    if (strcmp(method, "peerhost") == 0) return nst_sockname(obj, 1, 0);
    if (strcmp(method, "peerport") == 0) return nst_sockname(obj, 1, 1);
    if (strcmp(method, "sockhost") == 0) return nst_sockname(obj, 0, 0);
    if (strcmp(method, "sockport") == 0) return nst_sockname(obj, 0, 1);
    if (strcmp(method, "connected") == 0) {
        PerlValue *p = nst_sockname(obj, 1, 0);
        int ok = p && p->tag != PERL_UNDEF;
        perl_free(p);
        return perl_alloc_int(ok ? 1 : 0);
    }
    if (strcmp(method, "sockdomain") == 0) return perl_alloc_int(AF_INET);
    if (strcmp(method, "socktype") == 0) return perl_alloc_int(SOCK_STREAM);
    if (strcmp(method, "protocol") == 0) return perl_alloc_int(IPPROTO_TCP);
    if (strcmp(method, "shutdown") == 0) {
        int how = args && args->len ? (int)perl_to_int(args->elems[0]) : SHUT_RDWR;
        if (!fp) return perl_alloc_undef();
        return perl_alloc_int(shutdown(fileno(fp), how) == 0 ? 1 : 0);
    }
    return NULL; /* not handled */
}

/* ── Wave 4/5: Getopt::Std, ParseWords, File::Compare/stat, version,
   HTTP::Tiny, autodie ─────────────────────────────────────────────────── */

static int s_autodie = 0;
void perl_autodie_enable(long long on) { s_autodie = on ? 1 : 0; }
int  perl_autodie_enabled(void) { return s_autodie; }

static PerlHash *nst_href(PerlValue *v) {
    if (!v) return NULL;
    if (v->tag == PERL_REF_HASH) return (PerlHash *)v->pval;
    return NULL;
}

static PerlValue *nst_hget(PerlValue *obj, const char *k) {
    PerlHash *h = nst_href(obj);
    if (!h) return perl_alloc_undef();
    PerlValue *r = perl_hash_get_str_ref(h, k);
    return r ? perl_clone(r) : perl_alloc_undef();
}

static void nst_hset(PerlHash *h, const char *k, PerlValue *v) {
    perl_hash_set_str(h, k, v);
}

static void nst_opt_store(PerlValue *dest, const char *pkg, char letter, PerlValue *val) {
    char key[8];
    snprintf(key, sizeof key, "%c", letter);
    if (dest && dest->tag == PERL_REF_HASH) {
        nst_hset((PerlHash *)dest->pval, key, val);
        return;
    }
    char glob[256];
    snprintf(glob, sizeof glob, "%s::opt_%c", pkg && pkg[0] ? pkg : "main", letter);
    PerlValue *cell = perl_glob_get_scalar(glob);
    perl_assign(cell, val);
}

PerlValue *perl_getopt_std(PerlValue *spec_pv, PerlValue *dest, PerlArray *argv,
                           PerlValue *pkg_pv, int is_getopts) {
    char *spec = spec_pv ? perl_to_string_dup(spec_pv) : strdup("");
    char *pkg = pkg_pv ? perl_to_string_dup(pkg_pv) : strdup("main");
    int ok = 1;
    long long idx = 0, n = argv ? argv->len : 0;
    (void)is_getopts;
    while (idx < n) {
        char *tok = perl_to_string_dup(argv->elems[idx]);
        if (!tok || tok[0] != '-' || tok[1] == '\0') { free(tok); break; }
        if (strcmp(tok, "--") == 0) {
            perl_free(argv->elems[idx]);
            for (long long j = idx; j + 1 < n; j++) argv->elems[j] = argv->elems[j + 1];
            argv->len--;
            free(tok);
            break;
        }
        const char *p = tok + 1;
        int consumed_tok = 1, consumed_next = 0;
        PerlValue *nextv = NULL;
        while (*p) {
            char letter = *p++;
            const char *sp = strchr(spec, letter);
            int needs_arg = 0;
            if (sp && sp[1] == ':') needs_arg = 1;
            else if (!sp) {
                fprintf(stderr, "Unknown option: %c\n", letter);
                ok = 0;
                continue;
            }
            if (needs_arg) {
                const char *raw = NULL;
                if (*p) { raw = p; p += strlen(p); }
                else if (idx + 1 < n) {
                    raw = perl_to_string(argv->elems[idx + 1]);
                    consumed_next = 1;
                } else { ok = 0; break; }
                PerlValue *v = perl_alloc_string(raw ? raw : "");
                nst_opt_store(dest, pkg, letter, v);
                perl_free(v);
            } else {
                PerlValue *v = perl_alloc_int(1);
                nst_opt_store(dest, pkg, letter, v);
                perl_free(v);
            }
        }
        free(tok);
        long long drop = consumed_tok + consumed_next;
        for (long long k = 0; k < drop; k++) {
            perl_free(argv->elems[idx]);
            for (long long j = idx; j + 1 < n; j++) argv->elems[j] = argv->elems[j + 1];
            argv->len--;
            n = argv->len;
        }
        (void)nextv;
    }
    free(spec); free(pkg);
    return perl_alloc_int(ok ? 1 : 0);
}

/* Text::ParseWords — whitespace or single-char delimiter, quoted fields. */
static void nst_push_field(PerlArray *out, const char *s, size_t n, int keep) {
    (void)keep;
    char *buf = malloc(n + 1);
    memcpy(buf, s, n); buf[n] = '\0';
    PerlValue *v = perl_alloc_string_len(buf, (long long)n);
    perl_array_push(out, v);
    perl_free(v);
    free(buf);
}

static int nst_is_delim(char c, const char *delim, int ws) {
    if (ws) return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    return delim && strchr(delim, c) != NULL;
}

PerlArray *perl_parsewords(PerlValue *delim_pv, PerlValue *keep_pv, PerlArray *texts) {
    char *delim = delim_pv ? perl_to_string_dup(delim_pv) : strdup(" ");
    int keep = keep_pv ? (int)perl_to_int(keep_pv) : 0;
    int ws = 0;
    if (!delim[0] || strcmp(delim, " ") == 0 || strcmp(delim, "\\s+") == 0) ws = 1;
    PerlArray *out = perl_array_new();
    long long ti;
    for (ti = 0; texts && ti < texts->len; ti++) {
        long long ln = 0;
        char *s = perl_to_string_dup_len(texts->elems[ti], &ln);
        long long i = 0;
        while (i < ln) {
            while (i < ln && nst_is_delim(s[i], delim, ws)) i++;
            if (i >= ln) break;
            char quote = 0;
            if (s[i] == '"' || s[i] == '\'') { quote = s[i]; i++; }
            char buf[8192];
            size_t b = 0;
            if (keep && quote && b < sizeof(buf) - 1) buf[b++] = quote;
            while (i < ln) {
                if (quote) {
                    if (s[i] == '\\' && i + 1 < ln) {
                        i++;
                        if (b < sizeof(buf) - 1) buf[b++] = s[i++];
                        continue;
                    }
                    if (s[i] == quote) {
                        if (keep && b < sizeof(buf) - 1) buf[b++] = s[i];
                        i++;
                        break;
                    }
                    if (b < sizeof(buf) - 1) buf[b++] = s[i++];
                } else {
                    if (nst_is_delim(s[i], delim, ws)) break;
                    if (s[i] == '"' || s[i] == '\'') { quote = s[i]; i++; 
                        if (keep && b < sizeof(buf) - 1) buf[b++] = quote;
                        continue; }
                    if (b < sizeof(buf) - 1) buf[b++] = s[i++];
                }
            }
            nst_push_field(out, buf, b, keep);
        }
        free(s);
    }
    free(delim);
    return out;
}

PerlValue *perl_file_compare(PerlValue *a, PerlValue *b) {
    char *pa = perl_to_string_dup(a), *pb = perl_to_string_dup(b);
    FILE *fa = fopen(pa, "rb"), *fb = fopen(pb, "rb");
    int rc;
    if (!fa || !fb) { rc = -1; goto done; }
    for (;;) {
        int ca = fgetc(fa), cb = fgetc(fb);
        if (ca != cb) { rc = 1; goto done; }
        if (ca == EOF) { rc = 0; goto done; }
    }
done:
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    free(pa); free(pb);
    return perl_alloc_int(rc);
}

static PerlValue *nst_stat_obj(const char *path, int do_lstat) {
    struct stat st;
    PerlHash *h;
    PerlValue *obj;
    int r = do_lstat ? lstat(path, &st) : stat(path, &st);
    if (r != 0) return perl_alloc_undef();
    h = perl_anon_hash_new();
#define STSET(k, v) do { PerlValue *_x = perl_alloc_int((long long)(v)); nst_hset(h, k, _x); perl_free(_x); } while (0)
    STSET("dev", st.st_dev);
    STSET("ino", st.st_ino);
    STSET("mode", st.st_mode);
    STSET("nlink", st.st_nlink);
    STSET("uid", st.st_uid);
    STSET("gid", st.st_gid);
    STSET("rdev", st.st_rdev);
    STSET("size", st.st_size);
    STSET("atime", st.st_atime);
    STSET("mtime", st.st_mtime);
    STSET("ctime", st.st_ctime);
    STSET("blksize", st.st_blksize);
    STSET("blocks", st.st_blocks);
#undef STSET
    obj = perl_ref_hash(h);
    if (obj->blessed_class) free(obj->blessed_class);
    obj->blessed_class = strdup("File::stat");
    return obj;
}

PerlValue *perl_file_stat(PerlValue *path, int do_lstat) {
    char *p = perl_to_string_dup(path);
    PerlValue *r = nst_stat_obj(p ? p : "", do_lstat);
    free(p);
    return r;
}

static PerlValue *nst_ver_from_str(const char *s) {
    int qv = 0, a = 0, b = 0, c = 0;
    const char *p = s ? s : "";
    char orig[128], norm[64], numbuf[64];
    PerlHash *h;
    PerlValue *obj, *sv;
    double num;
    snprintf(orig, sizeof orig, "%s", p);
    if (*p == 'v') { qv = 1; p++; }
    sscanf(p, "%d.%d.%d", &a, &b, &c);
    num = (double)a + (double)b / 1000.0 + (double)c / 1000000.0;
    snprintf(norm, sizeof norm, "v%d.%d.%d", a, b, c);
    snprintf(numbuf, sizeof numbuf, "%.6f", num);
    h = perl_anon_hash_new();
    sv = perl_alloc_string(orig); nst_hset(h, "s", sv); perl_free(sv);
    sv = perl_alloc_string(norm); nst_hset(h, "nstr", sv); perl_free(sv);
    sv = perl_alloc_float(num); nst_hset(h, "num", sv); perl_free(sv);
    sv = perl_alloc_int(qv); nst_hset(h, "qv", sv); perl_free(sv);
    obj = perl_ref_hash(h);
    if (obj->blessed_class) free(obj->blessed_class);
    obj->blessed_class = strdup("version");
    return obj;
}

PerlValue *perl_version_parse(PerlValue *s) {
    char *p = perl_to_string_dup(s);
    PerlValue *r = nst_ver_from_str(p);
    free(p);
    return r;
}

PerlValue *perl_ver_ovl_str(PerlValue *self) {
    return nst_hget(self, "s");
}

PerlValue *perl_ver_ovl_cmp(PerlValue *a, PerlValue *b) {
    double na, nb;
    PerlValue *va, *vb;
    va = nst_hget(a, "num");
    na = perl_to_float(va); perl_free(va);
    if (b && b->blessed_class && strcmp(b->blessed_class, "version") == 0) {
        vb = nst_hget(b, "num");
        nb = perl_to_float(vb); perl_free(vb);
    } else {
        char *s = perl_to_string_dup(b);
        PerlValue *tmp = nst_ver_from_str(s);
        free(s);
        vb = nst_hget(tmp, "num");
        nb = perl_to_float(vb); perl_free(vb); perl_free(tmp);
    }
    if (na < nb) return perl_alloc_int(-1);
    if (na > nb) return perl_alloc_int(1);
    return perl_alloc_int(0);
}

/* HTTP::Tiny — HTTP/1.0 GET/HEAD/POST, no TLS. */
static int nst_parse_url(const char *url, char *host, int *port, char *path, int *ssl) {
    const char *p = url ? url : "";
    *ssl = 0; *port = 80;
    snprintf(path, 1024, "/");
    host[0] = 0;
    if (strncmp(p, "https://", 8) == 0) { *ssl = 1; *port = 443; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) p += 7;
    {
        const char *slash = strchr(p, '/');
        const char *colon;
        size_t hl;
        if (!slash) slash = p + strlen(p);
        colon = memchr(p, ':', (size_t)(slash - p));
        if (colon) {
            hl = (size_t)(colon - p);
            *port = atoi(colon + 1);
        } else hl = (size_t)(slash - p);
        if (hl >= 255) hl = 254;
        memcpy(host, p, hl); host[hl] = 0;
        if (*slash) snprintf(path, 1024, "%s", slash);
    }
    return host[0] ? 1 : 0;
}

static PerlValue *nst_http_resp(int status, const char *reason, const char *content,
                                const char *url) {
    PerlHash *h = perl_anon_hash_new();
    PerlValue *v, *obj;
    PerlHash *hdr;
    v = perl_alloc_int(status); nst_hset(h, "status", v); perl_free(v);
    v = perl_alloc_string(reason ? reason : ""); nst_hset(h, "reason", v); perl_free(v);
    v = perl_alloc_bool(status >= 200 && status < 300); nst_hset(h, "success", v); perl_free(v);
    v = perl_alloc_string(content ? content : ""); nst_hset(h, "content", v); perl_free(v);
    v = perl_alloc_string(url ? url : ""); nst_hset(h, "url", v); perl_free(v);
    hdr = perl_anon_hash_new();
    v = perl_ref_hash(hdr); nst_hset(h, "headers", v); perl_free(v);
    obj = perl_ref_hash(h);
    return obj;
}

static PerlValue *nst_http_request(PerlHash *self, const char *method, const char *url,
                                   PerlValue *content) {
    char host[256], path[1024], req[4096], buf[8192];
    int port, ssl, fd, n, total = 0;
    struct sockaddr_in sin;
    struct hostent *he;
    FILE *fp;
    char *body = NULL;
    int status = 599;
    char reason[128] = "Internal Exception";
    char resp[65536];
    size_t resp_n = 0;
    double timeout = 60;
    PerlValue *tv;
    (void)self;
    if (!nst_parse_url(url, host, &port, path, &ssl) || ssl)
        return nst_http_resp(599, "Internal Exception", "", url);
    tv = self ? perl_hash_get_str_ref(self, "timeout") : NULL;
    if (tv) timeout = perl_to_float(tv);
    he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0])
        return nst_http_resp(599, "Internal Exception", "", url);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return nst_http_resp(599, "Internal Exception", "", url);
    {
        struct timeval tvt;
        tvt.tv_sec = (long)timeout; tvt.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tvt, sizeof tvt);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tvt, sizeof tvt);
    }
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    memcpy(&sin.sin_addr, he->h_addr_list[0], 4);
    if (connect(fd, (struct sockaddr *)&sin, sizeof sin) != 0) {
        close(fd);
        return nst_http_resp(599, "Internal Exception", "", url);
    }
    {
        long long clen = 0;
        char *cdata = NULL;
        if (content) cdata = perl_to_string_dup_len(content, &clen);
        if (cdata && clen > 0)
            snprintf(req, sizeof req,
                     "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: HTTP-Tiny/0.096\r\n"
                     "Content-Length: %lld\r\nConnection: close\r\n\r\n",
                     method, path, host, clen);
        else
            snprintf(req, sizeof req,
                     "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: HTTP-Tiny/0.096\r\n"
                     "Connection: close\r\n\r\n",
                     method, path, host);
        send(fd, req, strlen(req), 0);
        if (cdata && clen > 0) send(fd, cdata, (size_t)clen, 0);
        free(cdata);
    }
    while ((n = (int)recv(fd, buf, sizeof buf, 0)) > 0) {
        if (resp_n + (size_t)n > sizeof(resp) - 1) n = (int)(sizeof(resp) - 1 - resp_n);
        if (n <= 0) break;
        memcpy(resp + resp_n, buf, (size_t)n);
        resp_n += (size_t)n;
        if (resp_n >= sizeof(resp) - 1) break;
        total += n;
    }
    close(fd);
    resp[resp_n] = 0;
    {
        char *hdr_end = strstr(resp, "\r\n\r\n");
        char *line = resp;
        if (sscanf(line, "HTTP/%*s %d %127[^\r\n]", &status, reason) < 1)
            status = 599;
        if (hdr_end) body = hdr_end + 4;
        else body = resp;
    }
    return nst_http_resp(status, reason, body ? body : "", url);
}

PerlValue *perl_http_tiny_new(PerlArray *args) {
    PerlHash *h = perl_anon_hash_new();
    PerlValue *v, *obj;
    long long i;
    v = perl_alloc_string("HTTP-Tiny/0.096"); nst_hset(h, "agent", v); perl_free(v);
    v = perl_alloc_int(60); nst_hset(h, "timeout", v); perl_free(v);
    if (args) {
        for (i = 0; i + 1 < args->len; i += 2) {
            char *k = perl_to_string_dup(args->elems[i]);
            nst_hset(h, k, args->elems[i + 1]);
            free(k);
        }
    }
    obj = perl_ref_hash(h);
    if (obj->blessed_class) free(obj->blessed_class);
    obj->blessed_class = strdup("HTTP::Tiny");
    return obj;
}

PerlValue *perl_wave45_method(PerlValue *obj, const char *method, PerlArray *args) {
    const char *cls = NULL;
    if (!method) return NULL;
    if (obj && obj->tag == PERL_STRING && obj->sval) cls = obj->sval;
    else if (obj && obj->blessed_class) cls = obj->blessed_class;
    if (!cls) return NULL;

    if (strcmp(cls, "File::stat") == 0) {
        static const char *fields[] = {
            "dev","ino","mode","nlink","uid","gid","rdev","size",
            "atime","mtime","ctime","blksize","blocks", NULL
        };
        int i;
        if (obj->tag == PERL_STRING) return NULL;
        for (i = 0; fields[i]; i++)
            if (strcmp(method, fields[i]) == 0) return nst_hget(obj, method);
        return NULL;
    }
    if (strcmp(cls, "version") == 0) {
        if (obj->tag == PERL_STRING &&
            (strcmp(method, "parse") == 0 || strcmp(method, "new") == 0 ||
             strcmp(method, "declare") == 0 || strcmp(method, "qv") == 0)) {
            PerlValue *s = args && args->len ? args->elems[0] : perl_alloc_string("0");
            return perl_version_parse(s);
        }
        if (obj->tag != PERL_STRING) {
            if (strcmp(method, "numify") == 0) return nst_hget(obj, "num");
            if (strcmp(method, "normal") == 0) return nst_hget(obj, "nstr");
            if (strcmp(method, "stringify") == 0) return nst_hget(obj, "s");
            if (strcmp(method, "is_qv") == 0) return nst_hget(obj, "qv");
        }
        return NULL;
    }
    if (strcmp(cls, "HTTP::Tiny") == 0) {
        if (obj->tag == PERL_STRING && strcmp(method, "new") == 0)
            return perl_http_tiny_new(args);
        if (obj->tag == PERL_STRING && strcmp(method, "can_ssl") == 0)
            return perl_alloc_bool(0);
        if (obj->tag != PERL_STRING) {
            PerlHash *self = nst_href(obj);
            const char *mth = method;
            char *url;
            PerlValue *content = NULL;
            if (strcmp(method, "can_ssl") == 0) return perl_alloc_bool(0);
            if (strcmp(method, "get") == 0 || strcmp(method, "head") == 0 ||
                strcmp(method, "post") == 0 || strcmp(method, "put") == 0 ||
                strcmp(method, "delete") == 0) {
                if (strcmp(method, "get") == 0) mth = "GET";
                else if (strcmp(method, "head") == 0) mth = "HEAD";
                else if (strcmp(method, "post") == 0) mth = "POST";
                else if (strcmp(method, "put") == 0) mth = "PUT";
                else mth = "DELETE";
                url = args && args->len ? perl_to_string_dup(args->elems[0]) : strdup("/");
                if (args && args->len > 1 && nst_href(args->elems[1])) {
                    PerlValue *c = perl_hash_get_str_ref(nst_href(args->elems[1]), "content");
                    if (c) content = c;
                }
                {
                    PerlValue *r = nst_http_request(self, mth, url, content);
                    free(url);
                    return r;
                }
            }
            if (strcmp(method, "request") == 0) {
                char *meth = args && args->len ? perl_to_string_dup(args->elems[0]) : strdup("GET");
                url = args && args->len > 1 ? perl_to_string_dup(args->elems[1]) : strdup("/");
                if (args && args->len > 2 && nst_href(args->elems[2])) {
                    PerlValue *c = perl_hash_get_str_ref(nst_href(args->elems[2]), "content");
                    if (c) content = c;
                }
                {
                    PerlValue *r = nst_http_request(self, meth, url, content);
                    free(meth); free(url);
                    return r;
                }
            }
        }
        return NULL;
    }
    {
        PerlValue *extra = perl_stdlib_method(obj, method, args);
        if (extra) return extra;
    }
    return NULL;
}

/* ── MIME::QuotedPrint ───────────────────────────────────────────────────── */

PerlValue *perl_encode_qp(PerlValue *data, PerlValue *eol) {
    long long ln = 0;
    char *s = data ? perl_to_string_dup_len(data, &ln) : strdup("");
    const char *nl = "\n";
    size_t cap = (size_t)ln * 3 + 8, o = 0, col = 0;
    char *out = malloc(cap ? cap : 8);
    long long i;
    if (eol && eol->tag != PERL_UNDEF) {
        const char *e = perl_to_string(eol);
        if (e && e[0]) nl = e;
    }
    for (i = 0; i < ln; i++) {
        unsigned char c = (unsigned char)s[i];
        int enc = 0;
        if (c == '\n') {
            out[o++] = '\n'; col = 0;
            continue;
        }
        if (c == '=' || c < 32 || c > 126 || c == 127) enc = 1;
        else if (c == ' ' || c == '\t') {
            /* Trailing whitespace at line end (rest of the input line up to
               the next \n or EOF is all spaces/tabs) is encoded — real QP
               does this even at EOF, where the appended final soft break
               creates the line end. An interior space stays literal. */
            long long j = i + 1;
            while (j < ln && s[j] != '\n' &&
                   ((s[j] == ' ') || (s[j] == '\t'))) j++;
            if (j >= ln || s[j] == '\n') enc = 1;
        }
        {
            int need = enc ? 3 : 1;
            if (col + need > 75 && col > 0) {
                out[o++] = '=';
                {
                    const char *p;
                    for (p = nl; *p; p++) out[o++] = *p;
                }
                col = 0;
            }
        }
        if (enc) {
            snprintf(out + o, 4, "=%02X", c);
            o += 3; col += 3;
        } else {
            out[o++] = (char)c; col++;
        }
        if (o + 8 >= cap) { cap *= 2; out = realloc(out, cap); }
    }
    /* Real MIME::QuotedPrint appends a trailing soft break ("=" + eol)
       when the input is non-empty and does not end in a newline
       (encode_qp("c") → "c=\n"; encode_qp("") → ""). */
    if (ln > 0 && s[ln - 1] != '\n') {
        if (o + 8 >= cap) { cap *= 2; out = realloc(out, cap); }
        out[o++] = '=';
        for (const char *p = nl; *p; p++) out[o++] = *p;
    }
    out[o] = 0;
    {
        PerlValue *r = perl_alloc_string_len(out, (long long)o);
        free(out); free(s);
        return r;
    }
}

PerlValue *perl_decode_qp(PerlValue *data) {
    long long ln = 0;
    char *s = data ? perl_to_string_dup_len(data, &ln) : strdup("");
    char *out = malloc((size_t)ln + 1);
    long long i, o = 0;
    for (i = 0; i < ln; i++) {
        if (s[i] == '=' && i + 1 < ln && (s[i+1] == '\n' || s[i+1] == '\r')) {
            i++;
            if (i < ln && s[i] == '\r') i++;
            if (i < ln && s[i] == '\n') { /* skip */ }
            continue;
        }
        if (s[i] == '=' && i + 2 < ln && isxdigit((unsigned char)s[i+1]) &&
            isxdigit((unsigned char)s[i+2])) {
            int v = 0;
            sscanf(s + i + 1, "%2x", &v);
            out[o++] = (char)v;
            i += 2;
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = 0;
    {
        PerlValue *r = perl_alloc_string_len(out, o);
        free(out); free(s);
        return r;
    }
}

/* ── Text::Tabs ──────────────────────────────────────────────────────────── */

static int nst_tabstop(void) {
    PerlValue *c = perl_glob_get_scalar("Text::Tabs::tabstop");
    long long n = c ? perl_to_int(c) : 8;
    if (n <= 0) n = 8;
    return (int)n;
}

PerlValue *perl_tabs_expand(PerlArray *texts) {
    int ts = nst_tabstop();
    PerlArray *out = perl_array_new();
    long long ti;
    for (ti = 0; texts && ti < texts->len; ti++) {
        long long ln = 0, i, col = 0;
        char *s = perl_to_string_dup_len(texts->elems[ti], &ln);
        size_t cap = (size_t)ln * (size_t)ts + 8, o = 0;
        char *b = malloc(cap);
        for (i = 0; i < ln; i++) {
            if (s[i] == '\n') { b[o++] = '\n'; col = 0; }
            else if (s[i] == '\t') {
                int n = ts - (int)(col % ts);
                if (n <= 0) n = ts;
                while (n--) { b[o++] = ' '; col++; if (o + 2 >= cap) { cap *= 2; b = realloc(b, cap); } }
            } else { b[o++] = s[i]; col++; }
            if (o + 8 >= cap) { cap *= 2; b = realloc(b, cap); }
        }
        {
            PerlValue *v = perl_alloc_string_len(b, (long long)o);
            perl_array_push(out, v); perl_free(v);
        }
        free(b); free(s);
    }
    return perl_array_to_list_return(out);
}

PerlValue *perl_tabs_unexpand(PerlArray *texts) {
    /* keep simple: return expand inverse is hard; pass through spaces of tabstop */
    int ts = nst_tabstop();
    PerlArray *out = perl_array_new();
    long long ti;
    for (ti = 0; texts && ti < texts->len; ti++) {
        long long ln = 0, i, col = 0, sp = 0;
        char *s = perl_to_string_dup_len(texts->elems[ti], &ln);
        size_t cap = (size_t)ln + 8, o = 0;
        char *b = malloc(cap);
        for (i = 0; i < ln; i++) {
            if (s[i] == ' ') { sp++; col++;
                if (col % ts == 0 && sp > 1) { b[o++] = '\t'; sp = 0; }
            } else {
                while (sp--) b[o++] = ' ';
                sp = 0;
                b[o++] = s[i];
                if (s[i] == '\n') col = 0; else col++;
            }
            if (o + 8 >= cap) { cap *= 2; b = realloc(b, cap); }
        }
        while (sp--) b[o++] = ' ';
        {
            PerlValue *v = perl_alloc_string_len(b, (long long)o);
            perl_array_push(out, v); perl_free(v);
        }
        free(b); free(s);
    }
    return perl_array_to_list_return(out);
}

void perl_open_pragma_std_utf8(void) {
    PerlValue *in = perl_get_stdin();
    PerlValue *out = perl_get_stdout();
    PerlValue *err = perl_get_stderr();
    if (in) in->flags |= PV_FLAG_UTF8;
    if (out) out->flags |= PV_FLAG_UTF8;
    if (err) err->flags |= PV_FLAG_UTF8;
}

/* ── CGI ─────────────────────────────────────────────────────────────────── */

static PerlValue *s_cgi_default = NULL;

static int cgi_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static char *cgi_unescape(const char *s) {
    size_t n = s ? strlen(s) : 0, o = 0, i;
    char *out = malloc(n + 1);
    for (i = 0; i < n; i++) {
        if (s[i] == '+') out[o++] = ' ';
        else if (s[i] == '%' && i + 2 < n) {
            out[o++] = (char)((cgi_hex(s[i+1]) << 4) | cgi_hex(s[i+2]));
            i += 2;
        } else out[o++] = s[i];
    }
    out[o] = 0;
    return out;
}

static char *cgi_escape(const char *s, long long ln) {
    size_t cap = (size_t)ln * 3 + 1, o = 0;
    long long i;
    char *out = malloc(cap);
    for (i = 0; i < ln; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out[o++] = (char)c;
        else {
            snprintf(out + o, 4, "%%%02X", c);
            o += 3;
        }
    }
    out[o] = 0;
    return out;
}

static void cgi_add_param(PerlHash *h, PerlArray *names, const char *k, const char *v) {
    PerlValue *slot = perl_hash_get_str_ref(h, k);
    PerlArray *av;
    PerlValue *sv, *r;
    if (slot && slot->tag == PERL_REF_ARRAY) av = (PerlArray *)slot->pval;
    else {
        av = perl_anon_array_new();
        r = perl_ref_array(av);
        perl_hash_set_str(h, k, r);
        perl_free(r);
        if (names) {
            PerlValue *nk = perl_alloc_string(k);
            perl_array_push(names, nk);
            perl_free(nk);
        }
    }
    sv = perl_alloc_string(v ? v : "");
    perl_array_push(av, sv);
    perl_free(sv);
}

static void cgi_parse_qs(PerlHash *h, PerlArray *names, const char *qs) {
    char *dup, *save, *tok;
    if (!qs || !qs[0]) return;
    dup = strdup(qs);
    for (tok = strtok_r(dup, "&;", &save); tok; tok = strtok_r(NULL, "&;", &save)) {
        char *eq = strchr(tok, '=');
        char *k, *v;
        if (eq) { *eq = 0; k = cgi_unescape(tok); v = cgi_unescape(eq + 1); }
        else { k = cgi_unescape(tok); v = strdup(""); }
        cgi_add_param(h, names, k, v);
        free(k); free(v);
    }
    free(dup);
}

static PerlValue *cgi_opt(PerlArray *args, const char *key) {
    long long i;
    if (!args) return NULL;
    for (i = 0; i + 1 < args->len; i += 2) {
        char *k = perl_to_string_dup(args->elems[i]);
        int hit = 0;
        if (k) {
            const char *p = k;
            if (*p == '-') p++;
            hit = strcasecmp(p, key) == 0;
        }
        free(k);
        if (hit) return args->elems[i + 1];
    }
    return NULL;
}

static PerlValue *nst_cgi_new(PerlArray *args) {
    PerlHash *h = perl_anon_hash_new();
    PerlHash *params = perl_anon_hash_new();
    PerlArray *names = perl_anon_array_new();
    PerlValue *obj, *pr, *nr, *v;
    const char *qs = getenv("QUERY_STRING");
    const char *rm = getenv("REQUEST_METHOD");
    if (args && args->len == 1 && args->elems[0]->tag == PERL_STRING) {
        char *s = perl_to_string_dup(args->elems[0]);
        cgi_parse_qs(params, names, s);
        free(s);
    } else if (qs) {
        cgi_parse_qs(params, names, qs);
    }
    pr = perl_ref_hash(params);
    perl_hash_set_str(h, "params", pr);
    perl_free(pr);
    nr = perl_ref_array(names);
    perl_hash_set_str(h, "names", nr);
    perl_free(nr);
    v = perl_alloc_string(rm ? rm : "");
    perl_hash_set_str(h, "method", v); perl_free(v);
    obj = perl_ref_hash(h);
    if (obj->blessed_class) free(obj->blessed_class);
    obj->blessed_class = strdup("CGI");
    s_cgi_default = obj;
    return obj;
}

static PerlHash *cgi_params(PerlValue *obj) {
    PerlHash *h = nst_href(obj);
    PerlValue *p;
    if (!h) return NULL;
    p = perl_hash_get_str_ref(h, "params");
    return nst_href(p);
}

static PerlValue *cgi_header(PerlArray *args) {
    const char *type = "text/html";
    const char *charset = "ISO-8859-1";
    PerlValue *tv = cgi_opt(args, "type");
    PerlValue *cv = cgi_opt(args, "charset");
    PerlValue *cookie = cgi_opt(args, "cookie");
    char buf[1024];
    size_t n = 0;
    if (!tv && args && args->len == 1 && args->elems[0]->tag == PERL_STRING)
        tv = args->elems[0];
    if (tv) type = perl_to_string(tv);
    if (cv) charset = perl_to_string(cv);
    if (cookie) {
        char *cs = perl_to_string_dup(cookie);
        n += (size_t)snprintf(buf + n, sizeof buf - n, "Set-Cookie: %s\r\n", cs);
        free(cs);
    }
    n += (size_t)snprintf(buf + n, sizeof buf - n,
                          "Content-Type: %s; charset=%s\r\n\r\n", type, charset);
    return perl_alloc_string_len(buf, (long long)n);
}

static PerlValue *cgi_tag(const char *tag, PerlArray *args, int empty) {
    char buf[4096];
    size_t n = 0;
    long long i = 0;
    n += (size_t)snprintf(buf + n, sizeof buf - n, "<%s", tag);
    if (args) {
        while (i + 1 < args->len) {
            char *k = perl_to_string_dup(args->elems[i]);
            if (k && k[0] == '-') {
                char *v = perl_to_string_dup(args->elems[i + 1]);
                n += (size_t)snprintf(buf + n, sizeof buf - n, " %s=\"%s\"", k + 1, v ? v : "");
                free(v);
                i += 2;
                free(k);
                continue;
            }
            free(k);
            break;
        }
    }
    if (empty) {
        n += (size_t)snprintf(buf + n, sizeof buf - n, " />");
        return perl_alloc_string_len(buf, (long long)n);
    }
    n += (size_t)snprintf(buf + n, sizeof buf - n, ">");
    for (; args && i < args->len; i++) {
        char *v = perl_to_string_dup(args->elems[i]);
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%s", v ? v : "");
        free(v);
    }
    n += (size_t)snprintf(buf + n, sizeof buf - n, "</%s>", tag);
    return perl_alloc_string_len(buf, (long long)n);
}

static PerlValue *nst_cgi_method(PerlValue *obj, const char *method, PerlArray *args) {
    PerlHash *ph;
    if (!obj) obj = s_cgi_default;
    if (!obj && strcmp(method, "new") != 0) {
        PerlArray *empty = perl_array_new();
        obj = nst_cgi_new(empty);
        perl_array_free_nc(empty);
    }
    ph = cgi_params(obj);
    if (strcmp(method, "param") == 0 || strcmp(method, "multi_param") == 0) {
        int wa = perl_current_wantarray_ctx();
        if (!args || args->len == 0) {
            /* names */
            PerlArray *names = perl_anon_array_new();
            /* iterate keys via perl_hash - use a simple known approach */
            if (ph) {
                /* fall back: we don't have a public keys iterator in a small API;
                   store names as we parse... use perl_hash keys if available */
            }
            (void)names;
            /* Use perl_env? Walk isn't available. Keep names in "names" array on obj. */
            {
                PerlValue *nv = perl_hash_get_str_ref(nst_href(obj), "names");
                if (nv && nv->tag == PERL_REF_ARRAY) {
                    if (wa) return perl_array_to_list_return((PerlArray *)nv->pval);
                    {
                        PerlArray *av = (PerlArray *)nv->pval;
                        return av->len ? perl_clone(av->elems[av->len - 1]) : perl_alloc_undef();
                    }
                }
            }
            return wa ? perl_array_to_list_return(perl_anon_array_new()) : perl_alloc_undef();
        }
        {
            char *k = perl_to_string_dup(args->elems[0]);
            PerlValue *slot = ph ? perl_hash_get_str_ref(ph, k) : NULL;
            free(k);
            if (slot && slot->tag == PERL_REF_ARRAY) {
                PerlArray *av = (PerlArray *)slot->pval;
                if (wa) return perl_array_to_list_return(av);
                return av->len ? perl_clone(av->elems[0]) : perl_alloc_undef();
            }
            return perl_alloc_undef();
        }
    }
    if (strcmp(method, "header") == 0) return cgi_header(args);
    if (strcmp(method, "request_method") == 0) return nst_hget(obj, "method");
    if (strcmp(method, "query_string") == 0) {
        /* rebuild with ; */
        PerlValue *nv = perl_hash_get_str_ref(nst_href(obj), "names");
        char buf[4096];
        size_t n = 0;
        if (nv && nv->tag == PERL_REF_ARRAY && ph) {
            PerlArray *names = (PerlArray *)nv->pval;
            long long i;
            for (i = 0; i < names->len; i++) {
                char *k = perl_to_string_dup(names->elems[i]);
                PerlValue *slot = perl_hash_get_str_ref(ph, k);
                if (slot && slot->tag == PERL_REF_ARRAY) {
                    PerlArray *av = (PerlArray *)slot->pval;
                    long long j;
                    for (j = 0; j < av->len; j++) {
                        char *v = perl_to_string_dup(av->elems[j]);
                        char *ek = cgi_escape(k, (long long)strlen(k));
                        char *ev = cgi_escape(v, (long long)strlen(v));
                        n += (size_t)snprintf(buf + n, sizeof buf - n, "%s%s=%s",
                                              n ? ";" : "", ek, ev);
                        free(ek); free(ev); free(v);
                    }
                }
                free(k);
            }
        }
        buf[n] = 0;
        return perl_alloc_string_len(buf, (long long)n);
    }
    if (strcmp(method, "escape") == 0) {
        long long ln = 0;
        char *s = args && args->len ? perl_to_string_dup_len(args->elems[0], &ln) : strdup("");
        char *e = cgi_escape(s, ln);
        PerlValue *r = perl_alloc_string(e);
        free(s); free(e);
        return r;
    }
    if (strcmp(method, "unescape") == 0) {
        char *s = args && args->len ? perl_to_string_dup(args->elems[0]) : strdup("");
        char *u = cgi_unescape(s);
        PerlValue *r = perl_alloc_string(u);
        free(s); free(u);
        return r;
    }
    if (strcmp(method, "escapeHTML") == 0) {
        long long ln = 0, i;
        char *s = args && args->len ? perl_to_string_dup_len(args->elems[0], &ln) : strdup("");
        size_t cap = (size_t)ln * 6 + 1, o = 0;
        char *b = malloc(cap);
        for (i = 0; i < ln; i++) {
            if (s[i] == '&') { memcpy(b+o, "&amp;", 5); o += 5; }
            else if (s[i] == '<') { memcpy(b+o, "&lt;", 4); o += 4; }
            else if (s[i] == '>') { memcpy(b+o, "&gt;", 4); o += 4; }
            else if (s[i] == '"') { memcpy(b+o, "&quot;", 6); o += 6; }
            else b[o++] = s[i];
        }
        b[o] = 0;
        {
            PerlValue *r = perl_alloc_string_len(b, (long long)o);
            free(b); free(s);
            return r;
        }
    }
    if (strcmp(method, "cookie") == 0) {
        PerlValue *name = cgi_opt(args, "name");
        PerlValue *val = cgi_opt(args, "value");
        char buf[256];
        char *n = name ? perl_to_string_dup(name) : strdup("");
        char *v = val ? perl_to_string_dup(val) : strdup("");
        snprintf(buf, sizeof buf, "%s=%s; path=/", n, v);
        free(n); free(v);
        return perl_alloc_string(buf);
    }
    if (strcmp(method, "redirect") == 0) {
        PerlValue *uri = cgi_opt(args, "uri");
        if (!uri && args && args->len) uri = args->elems[0];
        char *u = uri ? perl_to_string_dup(uri) : strdup("/");
        char buf[512];
        snprintf(buf, sizeof buf, "Status: 302 Found\r\nLocation: %s\r\n\r\n", u);
        free(u);
        return perl_alloc_string(buf);
    }
    if (strcmp(method, "url") == 0 || strcmp(method, "self_url") == 0) {
        const char *host = getenv("SERVER_NAME");
        const char *script = getenv("SCRIPT_NAME");
        const char *port = getenv("SERVER_PORT");
        char buf[512];
        int abs = cgi_opt(args, "absolute") || cgi_opt(args, "full") ? 1 : 0;
        (void)abs;
        {
            char pb[16] = "";
            if (port && strcmp(port, "80") && strcmp(port, "443"))
                snprintf(pb, sizeof pb, ":%s", port);
            snprintf(buf, sizeof buf, "http://%s%s%s",
                     host ? host : "localhost", pb, script ? script : "");
        }
        if (strcmp(method, "self_url") == 0) {
            PerlValue *qs = nst_cgi_method(obj, "query_string", NULL);
            char *q = perl_to_string_dup(qs);
            if (q && q[0]) { strcat(buf, "?"); strcat(buf, q); }
            free(q); perl_free(qs);
        }
        return perl_alloc_string(buf);
    }
    if (strcmp(method, "script_name") == 0) {
        const char *s = getenv("SCRIPT_NAME");
        return perl_alloc_string(s ? s : "");
    }
    if (strcmp(method, "path_info") == 0) {
        const char *s = getenv("PATH_INFO");
        return perl_alloc_string(s ? s : "");
    }
    {
        static const char *tags[] = {
            "h1","h2","h3","h4","h5","h6","p","b","i","u","em","strong","tt","pre",
            "div","span","li","ul","ol","tr","td","th","table","a","blockquote",
            "address","html","head","title","body","style", NULL
        };
        static const char *emptyt[] = { "br","hr","img","meta", NULL };
        int t;
        for (t = 0; tags[t]; t++)
            if (strcmp(method, tags[t]) == 0) return cgi_tag(method, args, 0);
        for (t = 0; emptyt[t]; t++)
            if (strcmp(method, emptyt[t]) == 0) return cgi_tag(method, args, 1);
    }
    if (strcmp(method, "start_html") == 0) {
        PerlValue *title = cgi_opt(args, "title");
        char *t = title ? perl_to_string_dup(title) : strdup("Untitled");
        char buf[512];
        snprintf(buf, sizeof buf,
                 "<!DOCTYPE html>\n<html><head><title>%s</title></head><body>", t);
        free(t);
        return perl_alloc_string(buf);
    }
    if (strcmp(method, "end_html") == 0)
        return perl_alloc_string("</body></html>");
    if (strcmp(method, "start_form") == 0) {
        PerlValue *act = cgi_opt(args, "action");
        PerlValue *m = cgi_opt(args, "method");
        char *a = act ? perl_to_string_dup(act) : strdup("");
        char *mm = m ? perl_to_string_dup(m) : strdup("POST");
        char buf[256];
        snprintf(buf, sizeof buf, "<form method=\"%s\" action=\"%s\">", mm, a);
        free(a); free(mm);
        return perl_alloc_string(buf);
    }
    if (strcmp(method, "end_form") == 0) return perl_alloc_string("</form>");
    if (strcmp(method, "textfield") == 0 || strcmp(method, "password_field") == 0 ||
        strcmp(method, "hidden") == 0 || strcmp(method, "submit") == 0 ||
        strcmp(method, "textarea") == 0) {
        PerlValue *name = cgi_opt(args, "name");
        PerlValue *val = cgi_opt(args, "value");
        PerlValue *sz = cgi_opt(args, "size");
        char *n = name ? perl_to_string_dup(name) : strdup("");
        char *v = val ? perl_to_string_dup(val) : strdup("");
        char buf[512];
        const char *typ = "text";
        if (strcmp(method, "password_field") == 0) typ = "password";
        if (strcmp(method, "hidden") == 0) typ = "hidden";
        if (strcmp(method, "submit") == 0) typ = "submit";
        if (strcmp(method, "textarea") == 0)
            snprintf(buf, sizeof buf, "<textarea name=\"%s\">%s</textarea>", n, v);
        else if (sz)
            snprintf(buf, sizeof buf, "<input type=\"%s\" name=\"%s\" value=\"%s\" size=\"%lld\" />",
                     typ, n, v, perl_to_int(sz));
        else
            snprintf(buf, sizeof buf, "<input type=\"%s\" name=\"%s\" value=\"%s\" />", typ, n, v);
        free(n); free(v);
        return perl_alloc_string(buf);
    }
    return NULL;
}

PerlValue *perl_cgi_new(PerlArray *args) {
    return nst_cgi_new(args);
}

/* Fix names: wrap cgi_add_param to also push unique names. Redefine via
   recording at parse time. We'll patch cgi_parse to also fill names — see
   nst_cgi_new: after parse, we don't have keys. Add names array during parse. */

PerlValue *perl_cgi_call(const char *name, PerlArray *args) {
    const char *bare = name ? strrchr(name, ':') : NULL;
    if (bare && bare[1]) name = bare + 1;
    if (strcmp(name, "new") == 0) return perl_cgi_new(args);
    if (!s_cgi_default) {
        PerlArray *empty = perl_array_new();
        perl_cgi_new(empty);
        perl_array_free_nc(empty);
    }
    return nst_cgi_method(s_cgi_default, name, args);
}

/* ── Term::ReadLine / IO::Select / SelectSaver / IO::Pipe ─────────────── */

static PerlValue *nst_readline_new(PerlArray *args) {
    PerlHash *h = perl_anon_hash_new();
    PerlValue *obj, *v, *hist;
    const char *nm = args && args->len ? perl_to_string(args->elems[0]) : "perl";
    v = perl_alloc_string(nm); perl_hash_set_str(h, "name", v); perl_free(v);
    v = perl_alloc_string("Term::ReadLine::Stub");
    perl_hash_set_str(h, "impl", v); perl_free(v);
    hist = perl_ref_array(perl_anon_array_new());
    perl_hash_set_str(h, "hist", hist); perl_free(hist);
    obj = perl_ref_hash(h);
    if (obj->blessed_class) free(obj->blessed_class);
    obj->blessed_class = strdup("Term::ReadLine::Stub");
    return obj;
}

static PerlValue *nst_select_new(PerlArray *args) {
    PerlHash *h = perl_anon_hash_new();
    PerlArray *hs = perl_anon_array_new();
    PerlValue *obj, *hr;
    long long i;
    for (i = 0; args && i < args->len; i++)
        perl_array_push(hs, args->elems[i]);
    hr = perl_ref_array(hs);
    perl_hash_set_str(h, "h", hr); perl_free(hr);
    obj = perl_ref_hash(h);
    if (obj->blessed_class) free(obj->blessed_class);
    obj->blessed_class = strdup("IO::Select");
    return obj;
}

PerlValue *perl_stdlib_method(PerlValue *obj, const char *method, PerlArray *args) {
    const char *cls = NULL;
    if (!method) return NULL;
    if (obj && obj->tag == PERL_STRING && obj->sval) cls = obj->sval;
    else if (obj && obj->blessed_class) cls = obj->blessed_class;

    if (cls && strcmp(cls, "CGI") == 0) {
        if (obj && obj->tag == PERL_STRING && strcmp(method, "new") == 0)
            return perl_cgi_new(args);
        return nst_cgi_method(obj && obj->tag == PERL_STRING ? s_cgi_default : obj, method, args);
    }
    if (cls && (strcmp(cls, "Term::ReadLine") == 0 ||
                strcmp(cls, "Term::ReadLine::Stub") == 0)) {
        if (obj && obj->tag == PERL_STRING && strcmp(method, "new") == 0)
            return nst_readline_new(args);
        if (strcmp(method, "ReadLine") == 0)
            return perl_alloc_string("Term::ReadLine::Stub");
        if (strcmp(method, "readline") == 0) {
            {
                PerlValue *pr = args && args->len ? args->elems[0] : NULL;
                char *ps = pr ? perl_to_string_dup(pr) : strdup("");
                /* Term::ReadLine writes cursor-positioning codes around the
                   prompt even when it returns immediately in non-tty mode:
                   "\e[4m<ps>\e[24m\e[1m\e[0m\e[0m". Match that so the prompt
                   bytes (on stderr) match real perl. */
                char buf[4200];
                int bp = snprintf(buf, sizeof buf, "\033[4m%s\033[24m\033[1m\033[0m\033[0m", ps);
                if (bp < 0) { free(ps); return perl_alloc_undef(); }
                if (bp >= (int)sizeof buf) bp = (int)sizeof buf - 1;
                buf[bp] = '\0';
                fputs(buf, stderr);
                fflush(stderr);
                free(ps);
                if (!isatty(0)) return perl_alloc_undef();
                if (!fgets(buf, sizeof buf, stdin)) return perl_alloc_undef();
                {
                    size_t n = strlen(buf);
                    if (n && buf[n-1] == '\n') buf[--n] = 0;
                    if (n && buf[n-1] == '\r') buf[--n] = 0;
                    return perl_alloc_string_len(buf, (long long)n);
                }
            }
        }
        if (strcmp(method, "addhistory") == 0) return perl_alloc_int(0);
        if (strcmp(method, "IN") == 0) return perl_get_stdin();
        if (strcmp(method, "OUT") == 0) return perl_get_stdout();
        if (strcmp(method, "MinLine") == 0) return perl_alloc_int(1);
        if (strcmp(method, "Attribs") == 0) {
            PerlHash *h = perl_anon_hash_new();
            return perl_ref_hash(h);
        }
        return NULL;
    }
    if (cls && strcmp(cls, "IO::Select") == 0) {
        if (obj && obj->tag == PERL_STRING && strcmp(method, "new") == 0)
            return nst_select_new(args);
        {
            PerlValue *hv = nst_hget(obj, "h");
            PerlArray *hs = NULL;
            if (hv && hv->tag == PERL_REF_ARRAY) hs = (PerlArray *)hv->pval;
            perl_free(hv);
            if (strcmp(method, "add") == 0) {
                long long i;
                for (i = 0; args && i < args->len; i++)
                    if (hs) perl_array_push(hs, args->elems[i]);
                return perl_alloc_int(1);
            }
            if (strcmp(method, "remove") == 0) return obj;
            if (strcmp(method, "count") == 0)
                return perl_alloc_int(hs ? hs->len : 0);
            if (strcmp(method, "handles") == 0)
                return hs ? perl_array_to_list_return(hs) : perl_array_to_list_return(perl_anon_array_new());
            if (strcmp(method, "can_read") == 0 || strcmp(method, "can_write") == 0 ||
                strcmp(method, "can_error") == 0) {
                fd_set fds;
                int maxfd = -1;
                long long i;
                struct timeval tv, *tvp = NULL;
                PerlArray *ready = perl_anon_array_new();
                FD_ZERO(&fds);
                for (i = 0; hs && i < hs->len; i++) {
                    FILE *fp = nst_fp(hs->elems[i]);
                    int fd = fp ? fileno(fp) : -1;
                    if (fd >= 0) { FD_SET(fd, &fds); if (fd > maxfd) maxfd = fd; }
                }
                if (args && args->len) {
                    double t = perl_to_float(args->elems[0]);
                    tv.tv_sec = (long)t;
                    tv.tv_usec = (long)((t - (double)tv.tv_sec) * 1e6);
                    tvp = &tv;
                }
                {
                    int w = 0;
                    if (strcmp(method, "can_read") == 0)
                        w = select(maxfd + 1, &fds, NULL, NULL, tvp);
                    else if (strcmp(method, "can_write") == 0)
                        w = select(maxfd + 1, NULL, &fds, NULL, tvp);
                    else
                        w = select(maxfd + 1, NULL, NULL, &fds, tvp);
                    (void)w;
                }
                for (i = 0; hs && i < hs->len; i++) {
                    FILE *fp = nst_fp(hs->elems[i]);
                    int fd = fp ? fileno(fp) : -1;
                    if (fd >= 0 && FD_ISSET(fd, &fds))
                        perl_array_push(ready, hs->elems[i]);
                }
                return perl_array_to_list_return(ready);
            }
        }
        return NULL;
    }
    if (cls && strcmp(cls, "SelectSaver") == 0) {
        if (strcmp(method, "new") == 0) {
            PerlValue *fh = args && args->len ? args->elems[0] : NULL;
            PerlValue *old = perl_select_fh(fh);
            PerlHash *h = perl_anon_hash_new();
            PerlValue *obj2;
            perl_hash_set_str(h, "old", old);
            perl_free(old);
            obj2 = perl_ref_hash(h);
            if (obj2->blessed_class) free(obj2->blessed_class);
            obj2->blessed_class = strdup("SelectSaver");
            return obj2;
        }
        if (strcmp(method, "DESTROY") == 0) {
            PerlValue *old = nst_hget(obj, "old");
            if (old) { perl_select_fh(old); perl_free(old); }
            return perl_alloc_undef();
        }
        return NULL;
    }
    if (cls && strcmp(cls, "IO::Pipe") == 0) {
        if (obj && obj->tag == PERL_STRING && strcmp(method, "new") == 0)
            return nst_pipe_new();
        if (strcmp(method, "reader") == 0) return nst_hget(obj, "r");
        if (strcmp(method, "writer") == 0) return nst_hget(obj, "w");
        return NULL;
    }
    /* MIME::QuotedPrint functional form (encode_qp/decode_qp are @EXPORT). */
    if (cls && strcmp(cls, "MIME::QuotedPrint") == 0) {
        if (strcmp(method, "encode_qp") == 0) {
            PerlValue *d = args && args->len > 0 ? args->elems[0] : NULL;
            PerlValue *e = args && args->len > 1 ? args->elems[1] : NULL;
            return perl_encode_qp(d, e);
        }
        if (strcmp(method, "decode_qp") == 0) {
            PerlValue *d = args && args->len > 0 ? args->elems[0] : NULL;
            return perl_decode_qp(d);
        }
        return NULL;
    }
    /* Text::Tabs functional form (expand/unexpand are @EXPORT). */
    if (cls && strcmp(cls, "Text::Tabs") == 0) {
        if (strcmp(method, "expand") == 0)
            return perl_tabs_expand(args);
        if (strcmp(method, "unexpand") == 0)
            return perl_tabs_unexpand(args);
        return NULL;
    }
    /* IO::Seekable SEEK_* constants (@EXPORT'd barewords). */
    if (cls && strcmp(cls, "IO::Seekable") == 0) {
        if (strcmp(method, "SEEK_SET") == 0) return perl_alloc_int(SEEK_SET);
        if (strcmp(method, "SEEK_CUR") == 0) return perl_alloc_int(SEEK_CUR);
        if (strcmp(method, "SEEK_END") == 0) return perl_alloc_int(SEEK_END);
        return NULL;
    }
    return NULL;
}




