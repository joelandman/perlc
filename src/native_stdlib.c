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
#include <openssl/evp.h>

#define PV_FLAG_AUTOFLUSH (1u << 25)

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
           strcmp(c, "IO::Socket::IP") == 0;
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
        if (strcmp(cls, "IO::File") == 0 || strcmp(cls, "IO::Handle") == 0) {
            if (args && args->len >= 1)
                return nst_io_open(args, "IO::File");
            return nst_new_fh(NULL, "IO::File");
        }
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
