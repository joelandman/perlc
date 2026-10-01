#include "lexer.h"
#include <stdexcept>
#include <unordered_map>
#include <deque>
#include <string>

static const std::unordered_map<std::string, TK> KEYWORDS = {
    {"my",       TK::KW_MY},    {"our",      TK::KW_OUR},
    {"local",    TK::KW_LOCAL}, {"if",       TK::KW_IF},
    {"elsif",    TK::KW_ELSIF}, {"else",     TK::KW_ELSE},
    {"unless",   TK::KW_UNLESS},{"while",    TK::KW_WHILE},
    {"until",    TK::KW_UNTIL}, {"for",      TK::KW_FOR},
    {"foreach",  TK::KW_FOREACH},{"do",      TK::KW_DO},
    {"last",     TK::KW_LAST},  {"next",     TK::KW_NEXT},
    {"redo",     TK::KW_REDO},  {"return",   TK::KW_RETURN},
    {"continue", TK::KW_CONTINUE},
    {"goto",     TK::KW_GOTO},
    {"sub",      TK::KW_SUB},   {"use",      TK::KW_USE},   {"require",  TK::KW_REQUIRE},
    {"strict",   TK::KW_STRICT},{"warnings", TK::KW_WARNINGS},
    {"print",    TK::KW_PRINT},  {"say",      TK::KW_SAY},
    {"printf",   TK::KW_PRINTF}, {"sprintf",  TK::KW_SPRINTF},
    {"open",     TK::KW_OPEN},   {"close",    TK::KW_CLOSE},
    {"eof",      TK::KW_EOF},    {"die",      TK::KW_DIE},   {"unlink",   TK::KW_UNLINK},
    {"push",     TK::KW_PUSH},  {"pop",      TK::KW_POP},
    {"shift",    TK::KW_SHIFT}, {"unshift",  TK::KW_UNSHIFT},
    {"scalar",   TK::KW_SCALAR},{"defined",  TK::KW_DEFINED},
    {"undef",    TK::KW_UNDEF},
    {"and",      TK::KW_AND},   {"or",       TK::KW_OR},
    {"not",      TK::KW_NOT},
    {"keys",     TK::KW_KEYS},  {"values",   TK::KW_VALUES},
    {"exists",   TK::KW_EXISTS},{"delete",   TK::KW_DELETE},
    {"each",     TK::KW_EACH},   {"sort",     TK::KW_SORT},
    {"chomp",    TK::KW_CHOMP},  {"chop",     TK::KW_CHOP},
    {"length",   TK::KW_LENGTH}, {"substr",   TK::KW_SUBSTR},
    {"join",     TK::KW_JOIN},   {"split",    TK::KW_SPLIT},
    {"index",    TK::KW_INDEX},  {"rindex",   TK::KW_RINDEX},
    {"uc",       TK::KW_UC},     {"lc",       TK::KW_LC},
    {"ucfirst",  TK::KW_UCFIRST},{"lcfirst",  TK::KW_LCFIRST},
    {"reverse",  TK::KW_REVERSE},{"splice",   TK::KW_SPLICE},
    {"ref",      TK::KW_REF},
    {"abs",      TK::KW_ABS},   {"int",      TK::KW_INT},
    {"sqrt",     TK::KW_SQRT},
    {"chr",      TK::KW_CHR},   {"ord",      TK::KW_ORD},
    {"hex",      TK::KW_HEX},   {"oct",      TK::KW_OCT},
    {"map",      TK::KW_MAP},   {"grep",     TK::KW_GREP},
    {"warn",     TK::KW_WARN},  {"system",   TK::KW_SYSTEM}, {"eval",    TK::KW_EVAL},
    {"bless",    TK::KW_BLESS}, {"package",  TK::KW_PACKAGE},
    {"wantarray",TK::KW_WANTARRAY}, {"caller", TK::KW_CALLER},
    {"state",    TK::KW_STATE},
    {"BEGIN",    TK::KW_BEGIN},     {"END",    TK::KW_END},
    {"chdir",    TK::KW_CHDIR},     {"mkdir",  TK::KW_MKDIR},
    {"rmdir",    TK::KW_RMDIR},     {"rename", TK::KW_RENAME},
    {"chmod",    TK::KW_CHMOD},
    {"opendir",  TK::KW_OPENDIR},   {"readdir",TK::KW_READDIR},
    {"closedir", TK::KW_CLOSEDIR},
    {"rand",     TK::KW_RAND},      {"srand",  TK::KW_SRAND},
    {"time",     TK::KW_TIME},      {"localtime",TK::KW_LOCALTIME}, {"gmtime", TK::KW_GMTIME},
    {"sleep",    TK::KW_SLEEP},     {"alarm",  TK::KW_ALARM},
    {"seek",     TK::KW_SEEK},      {"tell",   TK::KW_TELL},
    {"binmode",  TK::KW_BINMODE},   {"stat",   TK::KW_STAT},
    {"lstat",    TK::KW_LSTAT},     {"glob",   TK::KW_GLOB},
    {"read",     TK::KW_READ},      {"fileno", TK::KW_FILENO},
    {"truncate", TK::KW_TRUNCATE},
    {"pos",            TK::KW_POS},
    {"lock",           TK::KW_LOCK},
    {"cond_wait",      TK::KW_COND_WAIT},
    {"cond_signal",    TK::KW_COND_SIGNAL},
    {"cond_broadcast", TK::KW_COND_BROADCAST},
    {"sum",      TK::KW_SUM},       {"min",    TK::KW_MIN},    {"max",    TK::KW_MAX},
    {"first",    TK::KW_FIRST},     {"any",    TK::KW_ANY},    {"all",    TK::KW_ALL},
    {"none",     TK::KW_NONE},      {"uniq",   TK::KW_UNIQ},   {"reduce", TK::KW_REDUCE},
    {"tie",    TK::KW_TIE},     {"untie",    TK::KW_UNTIE},
    {"pack",   TK::KW_PACK},    {"unpack",   TK::KW_UNPACK},
};

/* D128: process-wide registry of source-file names. Token::file points at
   an element's heap storage, and tokens are copied around by value and
   outlive the Lexer that made them, so the pointed-at std::string must
   never move or die. A std::deque guarantees element-address stability
   across push_back (unlike std::vector, which reallocates), and this
   registry only grows (one entry per lexed source file per process —
   the driver lexes the main script plus a handful of inlined modules,
   so size is trivial). The registry is only ever touched from the single-
   threaded compile path (no thread ever shares Token streams), so no
   synchronization is needed. */
static std::deque<std::string> s_token_files;

const char *lexer_register_source_file(const std::string &name) {
    s_token_files.push_back(name);
    return s_token_files.back().c_str();
}

Lexer::Lexer(std::string src, const std::string &sourceName) : src_(std::move(src)) {
    if (!sourceName.empty())
        fileTag_ = lexer_register_source_file(sourceName);
}

char Lexer::peek(int offset) const {
    size_t p = pos_ + offset;
    return p < src_.size() ? src_[p] : '\0';
}

char Lexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') line_++;
    return c;
}

void Lexer::skipLineComment() {
    while (pos_ < src_.size() && src_[pos_] != '\n') pos_++;
}

Token Lexer::readNumber() {
    size_t start = pos_;
    bool   isFloat = false;
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        pos_ += 2;
        while (isxdigit(peek())) pos_++;
        return {TK::INT, src_.substr(start, pos_ - start), line_};
    }
    while (isdigit(peek()) || peek() == '_') pos_++;
    if (peek() == '.' && isdigit(peek(1))) {
        isFloat = true; pos_++;
        while (isdigit(peek()) || peek() == '_') pos_++;
    }
    if (peek() == 'e' || peek() == 'E') {
        isFloat = true; pos_++;
        if (peek() == '+' || peek() == '-') pos_++;
        while (isdigit(peek())) pos_++;
    }
    std::string text = src_.substr(start, pos_ - start);
    /* strip underscores from numeric literals */
    std::string clean; for (char c : text) if (c != '_') clean += c;
    return {isFloat ? TK::FLOAT : TK::INT, clean, line_};
}

Token Lexer::readString(char delim, bool interpolates) {
    pos_++; /* skip opening delimiter */
    std::string raw;
    bool wide = false;
    while (pos_ < src_.size()) {
        char c = src_[pos_];
        if (c == delim) { pos_++; break; }
        if (c == '\\' && pos_ + 1 < src_.size()) {
            pos_++;
            char esc = src_[pos_++];
            if (!interpolates) {
                /* Non-interpolating (single-quoted-style) string: real Perl
                   only recognizes \\ (literal backslash) and \<delim>
                   (literal delimiter, e.g. \' inside '...') as escapes —
                   every other backslash sequence stays literal, as both
                   characters (\n in 'a\nb' is the two characters '\' 'n',
                   NOT a newline). */
                if (esc == '\\' || esc == delim) raw += esc;
                else { raw += '\\'; raw += esc; }
                continue;
            }
            appendEscape(esc, raw, wide);
            continue;
        }
        if (c == '\n') line_++;
        raw += c;
        pos_++;
    }
    /* For double-quoted strings, we store the raw content with $ intact —
       the parser handles interpolation by calling splitInterp() */
    Token t{TK::STRING, raw, line_};
    t.wide = wide;
    return t;
}

/* Double-quoted escape sequences, shared by readString and the qq{...}
   balanced-brace scanner (which previously had its own, smaller switch
   with no \x at all). D154 adds \x{HEX}, one-digit \xH, bare \x (NUL)
   and octal \NNN; a \x{...} above 0xFF is UTF-8 encoded and marks the
   literal wide (a character string). */
void Lexer::appendEscape(char esc, std::string &raw, bool &wide) {
    auto hexv = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
    };
    auto putCode = [&](unsigned long cp) {
        if (cp < 0x100) { raw += (char)cp; return; }
        wide = true;
        if (cp < 0x800) {
            raw += (char)(0xC0 | (cp >> 6));
            raw += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            raw += (char)(0xE0 | (cp >> 12));
            raw += (char)(0x80 | ((cp >> 6) & 0x3F));
            raw += (char)(0x80 | (cp & 0x3F));
        } else {
            raw += (char)(0xF0 | (cp >> 18));
            raw += (char)(0x80 | ((cp >> 12) & 0x3F));
            raw += (char)(0x80 | ((cp >> 6) & 0x3F));
            raw += (char)(0x80 | (cp & 0x3F));
        }
    };
    switch (esc) {
        case 'n':  raw += '\n'; break;
        case 't':  raw += '\t'; break;
        case 'r':  raw += '\r'; break;
        case 'f':  raw += '\f'; break;
        case 'b':  raw += '\b'; break;
        case 'a':  raw += '\a'; break;
        case 'e':  raw += '\x1b'; break;
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            /* octal: up to 3 digits including this one */
            unsigned long v = (unsigned long)(esc - '0');
            for (int k = 0; k < 2 && peek() >= '0' && peek() <= '7'; k++) {
                v = v * 8 + (unsigned long)(peek() - '0');
                pos_++;
            }
            putCode(v);
            break;
        }
        case 'x': {
            if (peek() == '{') {
                size_t close = src_.find('}', pos_);
                if (close != std::string::npos) {
                    unsigned long v = 0;
                    for (size_t k = pos_ + 1; k < close; k++) {
                        int d = hexv(src_[k]);
                        if (d < 0) { if (src_[k] == '_' || src_[k] == ' ') continue; break; }
                        v = v * 16 + (unsigned long)d;
                    }
                    pos_ = close + 1;
                    putCode(v);
                    break;
                }
            }
            unsigned long v = 0;
            int n = 0;
            while (n < 2 && hexv(peek()) >= 0) { v = v * 16 + (unsigned long)hexv(peek()); pos_++; n++; }
            putCode(v);   /* bare \x (no digits) is NUL, like perl */
            break;
        }
        case '\\': raw += '\\'; break;
        case '\'': raw += '\''; break;
        case '"':  raw += '"';  break;
        /* D51: \$ and \@ keep a \x02 marker so parseStringInterp can tell
           an escaped literal from a real interpolation trigger. */
        case '$':  raw += '\x02'; raw += '$'; break;
        case '@':  raw += '\x02'; raw += '@'; break;
        default:   raw += '\\'; raw += esc; break;
    }
}

Token Lexer::readIdent() {
    /* D46: do NOT swallow a lone ':' (ternary / labels). Only absorb
       package separators as the two-char sequence '::'. Previously
       `while (... || peek()==':')` turned `$y:"str"` into IDENT "y:"
       and ate the ternary colon. */
    size_t start = pos_;
    while (isalnum((unsigned char)peek()) || peek() == '_') pos_++;
    while (peek() == ':' && peek(1) == ':') {
        pos_ += 2;
        while (isalnum((unsigned char)peek()) || peek() == '_') pos_++;
    }
    std::string text = src_.substr(start, pos_ - start);
    auto it = KEYWORDS.find(text);
    if (it != KEYWORDS.end()) return {it->second, text, line_};
    return {TK::IDENT, text, line_};
}

Token Lexer::readRegex() {
    return readRegexDelim('/', '/', false);
}

Token Lexer::readRegexDelim(char open, char close, bool paired) {
    /* pos_ is just past the opening delimiter. */
    std::string pattern;
    int depth = 1;
    while (pos_ < src_.size()) {
        char c = src_[pos_];
        if (c == '\\' && pos_ + 1 < src_.size()) {
            /* Pass every backslash-escaped sequence through to PCRE2
               unchanged (both characters) — \\ (literal backslash), \d
               (digit class), \s, \/ (escaped delimiter — redundant but
               harmless for PCRE2, which allows backslash-escaping any
               non-alphanumeric character), etc. */
            pattern += c;
            pattern += src_[++pos_];
            pos_++;
            continue;
        }
        if (paired) {
            if (c == open) depth++;
            else if (c == close) {
                if (--depth == 0) { pos_++; break; }
            }
        } else if (c == close) { pos_++; break; }
        if (c == '\n') line_++;
        pattern += c;
        pos_++;
    }
    std::string flags;
    while (pos_ < src_.size() && isalpha(src_[pos_])) flags += src_[pos_++];
    return {TK::REGEX, pattern + "\x01" + flags, line_};
}

Token Lexer::readHeredoc() {
    /* pos_ is just past the second '<'. Handle <<IDENT, <<"IDENT", <<'IDENT',
       and the D104 indented forms <<~IDENT / <<~"IDENT" / <<~'IDENT'
       (Perl 5.26+) — the '~' must be checked before the quote/identifier
       scan below, since it appears immediately after the second '<'. */
    bool indented = false;
    if (pos_ < src_.size() && src_[pos_] == '~') { indented = true; pos_++; }
    /* D165: `<< "EOT"` / `<<~ "EOT"` — real Perl allows (and tolerates
       in practice) whitespace between `<<`/`<<~` and a QUOTED
       delimiter (verified: `print <<   "QQ";` works). It does NOT
       allow this for the bare-identifier form (`<< EOT` with a space
       is a hard "Use of bare << to mean <<"" is forbidden" error in
       real Perl) — so only skip the whitespace here when it's
       followed by a quote char; otherwise leave it untouched so the
       bareword-delimiter scan below behaves exactly as before. */
    {
        size_t wsSave = pos_;
        while (pos_ < src_.size() && (src_[pos_] == ' ' || src_[pos_] == '\t')) pos_++;
        if (!(pos_ < src_.size() && (src_[pos_] == '"' || src_[pos_] == '\'')))
            pos_ = wsSave;
    }
    bool interp = true;
    char quote  = 0;
    if (pos_ < src_.size() && (src_[pos_] == '"' || src_[pos_] == '\'')) {
        quote  = src_[pos_++];
        interp = (quote == '"');
    }
    /* read delimiter name */
    std::string delim;
    while (pos_ < src_.size() && (isalnum((unsigned char)src_[pos_]) || src_[pos_] == '_'))
        delim += src_[pos_++];
    if (delim.empty())
        return {TK::EOF_TOK, "", line_}; /* not a heredoc — caller handles */
    /* A digit-leading unquoted delimiter (`1<<5`) is the left-shift
       operator in real Perl (verified: `print 1<<5` → 32), not a heredoc
       with terminator "5" — heredocs need a bareword identifier or a
       quoted string. Rewind to just after '<<' and let the caller emit
       LSHIFT. Only bareword letters/underscore continue as heredocs. */
    if (!quote && (isdigit((unsigned char)delim[0]))) {
        pos_ -= delim.size();
        return {TK::EOF_TOK, "", line_}; /* not a heredoc — caller handles */
    }
    if (quote && pos_ < src_.size() && src_[pos_] == quote)
        pos_++; /* consume closing quote */

    /* locate end of current line */
    size_t lineEnd = pos_;
    while (lineEnd < src_.size() && src_[lineEnd] != '\n') lineEnd++;

    /* collect heredoc body from subsequent lines. For the indented form,
       the terminator line may itself be preceded by whitespace — match it
       with that leading whitespace stripped, and remember the stripped
       prefix so it can be removed from every body line below (real Perl's
       `<<~IDENT` strips the terminator line's own indentation from the
       whole body). */
    size_t p = (lineEnd < src_.size()) ? lineEnd + 1 : lineEnd;
    std::string body;
    std::string indentPrefix;
    int extraLines = 0;
    while (p < src_.size()) {
        size_t ls = p;
        while (p < src_.size() && src_[p] != '\n') p++;
        std::string ln = src_.substr(ls, p - ls);
        if (p < src_.size()) p++; /* skip \n */
        extraLines++;
        if (indented) {
            size_t ws = 0;
            while (ws < ln.size() && (ln[ws] == ' ' || ln[ws] == '\t')) ws++;
            if (ln.substr(ws) == delim) { indentPrefix = ln.substr(0, ws); break; }
        } else if (ln == delim) {
            break; /* terminator line */
        }
        body += ln + "\n";
    }

    if (indented && !indentPrefix.empty()) {
        /* strip indentPrefix (or as much of it as a given line actually
           has, permissively — real Perl fatal-errors on a body line less
           indented than the terminator; we just strip what matches) from
           every body line. */
        std::string stripped;
        size_t bp = 0;
        while (bp < body.size()) {
            size_t nl = body.find('\n', bp);
            if (nl == std::string::npos) nl = body.size();
            std::string ln = body.substr(bp, nl - bp);
            size_t k = 0;
            while (k < indentPrefix.size() && k < ln.size() && ln[k] == indentPrefix[k]) k++;
            stripped += ln.substr(k);
            stripped += '\n';
            bp = nl + 1;
        }
        body = stripped;
    }

    /* after the current line's \n is consumed, jump past the heredoc body */
    pendingHeredocPos_   = p;
    pendingHeredocLines_ = extraLines;

    if (interp) {
        /* D154: <<"EOT" / <<EOT bodies are double-quoted strings — process
           their escapes (\t, \n, \$, \x{..}, ...); they used to stay
           literal (a\tb printed as a backslash-t). */
        bool wide = false;
        body = processEscapes(body, wide);
        Token t{TK::STRING, "\x01" + body, line_};
        t.wide = wide;
        return t;
    }
    return {TK::STRING, body, line_};
}

std::string Lexer::processEscapes(const std::string &in, bool &wide) {
    Lexer tmp(in);
    std::string out;
    while (tmp.pos_ < tmp.src_.size()) {
        char c = tmp.src_[tmp.pos_];
        if (c == '\\' && tmp.pos_ + 1 < tmp.src_.size()) {
            tmp.pos_++;
            char esc = tmp.src_[tmp.pos_++];
            tmp.appendEscape(esc, out, wide);
            continue;
        }
        out += c;
        tmp.pos_++;
    }
    return out;
}

Token Lexer::readSubst() {
    /* pos_ is just past 's' and the opening delimiter. Supports:
       s/pat/repl/flags, s!pat!repl!, s{pat}{repl}, s(pat)(repl), etc. */
    char open = src_[pos_ - 1]; /* delimiter just consumed by caller */
    char close = open;
    bool paired = false;
    if      (open == '{') { close = '}'; paired = true; }
    else if (open == '(') { close = ')'; paired = true; }
    else if (open == '[') { close = ']'; paired = true; }
    else if (open == '<') { close = '>'; paired = true; }

    auto readSection = [&](char delim) {
        std::string s;
        int depth = 1;
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == '\\' && pos_ + 1 < src_.size()) {
                s += c; s += src_[++pos_]; pos_++; continue;
            }
            if (paired) {
                if (c == open) depth++;
                else if (c == delim) {
                    if (--depth == 0) { pos_++; break; }
                }
            } else if (c == delim) { pos_++; break; }
            if (c == '\n') line_++;
            s += c; pos_++;
        }
        return s;
    };
    std::string pattern = readSection(close);
    /* skip optional whitespace between sections for paired delims */
    if (paired) {
        while (pos_ < src_.size() && (src_[pos_] == ' ' || src_[pos_] == '\t' || src_[pos_] == '\n')) {
            if (src_[pos_] == '\n') line_++;
            pos_++;
        }
        if (pos_ < src_.size()) {
            char o2 = src_[pos_++];
            char c2 = o2;
            if      (o2 == '{') c2 = '}';
            else if (o2 == '(') c2 = ')';
            else if (o2 == '[') c2 = ']';
            else if (o2 == '<') c2 = '>';
            open = o2; close = c2;
            paired = (c2 != o2);
        }
    }
    std::string repl = readSection(close);
    std::string flags;
    while (pos_ < src_.size() && isalpha(src_[pos_])) flags += src_[pos_++];
    return {TK::SUBST, pattern + "\x01" + repl + "\x01" + flags, line_};
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> toks;

    /* D128: stamp every token with this lexer's source-file registry tag.
       Done at the two exits (rather than at each of the ~80 push sites)
       so no token — current or future — can escape untagged; stamping an
       already-stamped token is a no-op. Tokens are only stamped when this
       lexer has a source name (fileTag_ != nullptr); nameless lexers (the
       synthetic-fragment ones in codegen) leave tokens untagged so errors
       keep the legacy main-script format. */
    auto stampFile = [&](std::vector<Token> &v) {
        if (fileTag_)
            for (auto &t : v) t.file = fileTag_;
    };

    /* D121: $::name / @::arr / %::hash — Perl's shorthand for an explicit
       main::name package-qualified variable (a bare :: prefix meaning
       "main"). Only readIdent()'s own :: handling existed before this,
       and that only fires once an identifier has already started with a
       letter/underscore — a leading :: right after the sigil never
       reaches it at all. Synthesizes the same IDENT token a written-out
       "main::name" would produce, so every downstream consumer (parser,
       codegen) needs no changes. */
    auto tryReadMainColonIdent = [&]() -> bool {
        if (pos_ + 2 < src_.size() && src_[pos_] == ':' && src_[pos_ + 1] == ':' &&
            (isalpha((unsigned char)src_[pos_ + 2]) || src_[pos_ + 2] == '_')) {
            pos_ += 2;
            std::string name = "main::";
            while (pos_ < src_.size() && (isalnum((unsigned char)src_[pos_]) || src_[pos_] == '_'))
                name += src_[pos_++];
            while (pos_ + 1 < src_.size() && src_[pos_] == ':' && src_[pos_ + 1] == ':') {
                pos_ += 2;
                name += "::";
                while (pos_ < src_.size() && (isalnum((unsigned char)src_[pos_]) || src_[pos_] == '_'))
                    name += src_[pos_++];
            }
            /* D121: $::name / %::hash — D110: @::arr (the sigil branch's
               tryReadMainColonIdent call used to be $-only in practice) */
            toks.push_back({TK::IDENT, name, line_});
            return true;
        }
        return false;
    };

    while (pos_ < src_.size()) {
        char c = peek();

        /* whitespace */
        if (c == ' ' || c == '\t' || c == '\r') { advance(); continue; }
        if (c == '\n') {
            advance();
            if (pendingHeredocPos_) {
                pos_   = pendingHeredocPos_;
                line_ += pendingHeredocLines_;
                pendingHeredocPos_   = 0;
                pendingHeredocLines_ = 0;
            }
            continue;
        }

        /* POD: =pod / =head1 / … at column 0 until =cut */
        if (c == '=' && (pos_ == 0 || src_[pos_ - 1] == '\n') &&
            isalpha((unsigned char)peek(1))) {
            while (pos_ < src_.size()) {
                if (src_[pos_] == '\n') line_++;
                /* look for =cut at start of a line */
                if ((pos_ == 0 || src_[pos_ - 1] == '\n') &&
                    pos_ + 4 <= src_.size() &&
                    src_[pos_] == '=' && src_[pos_+1] == 'c' &&
                    src_[pos_+2] == 'u' && src_[pos_+3] == 't' &&
                    (pos_ + 4 == src_.size() ||
                     !isalnum((unsigned char)src_[pos_+4]))) {
                    skipLineComment(); /* rest of =cut line */
                    break;
                }
                pos_++;
            }
            continue;
        }

        /* shebang line */
        if (c == '#' && line_ == 1 && pos_ == 0) {
            skipLineComment(); continue;
        }

        /* comments */
        if (c == '#') { skipLineComment(); continue; }

        /* numbers */
        if (isdigit(c)) { toks.push_back(readNumber()); continue; }

        /* strings */
        if (c == '"') {
            /* mark as double-quoted so parser can interpolate */
            Token t = readString('"', /*interpolates=*/true);
            t.kind = TK::STRING;
            /* prefix with \x01 to distinguish dq from sq in parser */
            t.text = "\x01" + t.text;
            toks.push_back(t);
            continue;
        }
        if (c == '\'') { toks.push_back(readString('\'', /*interpolates=*/false)); continue; }

        /* backtick command `cmd` */
        if (c == '`') {
            /* D161: this used to be a separate raw character-copy loop
               with ZERO escape processing — \$/\@ (meant to protect a
               literal $/@ from Perl's own interpolation before the
               text reaches the shell) passed through completely
               unhandled, so `` `echo "\$x hello"` `` both interpolated
               $x to its value AND left a stray literal backslash in
               front of it, instead of real Perl's `\$x` → literal
               `$x` for the shell to see. Now reuses readString's
               shared escape/appendEscape handling — the exact same
               path "..." literals already use, including \$/\@'s
               \x02 marker that parseStringInterp recognizes. */
            Token t = readString('`', /*interpolates=*/true);
            t.kind = TK::BACKTICK;
            /* prefix \x01 so parser treats content like a dq string (interpolation) */
            t.text = "\x01" + t.text;
            toks.push_back(t);
            continue;
        }

        /* D65: a bare `$`/`@`/`%` sigil is tokenized as its own, separate
           single-character token (just "$", "@", or "%") without
           consuming the identifier that follows — the *next* loop
           iteration reads that identifier via the generic isalpha(c)
           path further down. This means a variable *name* that happens
           to start with "q"/"qq"/"qw" (e.g. $qqfoo — a plain variable,
           not a quote operator; $q{key} for a variable literally named
           "q"; $qw{key}/%qw for one named "qw") would otherwise be
           caught by the q/qq/qw checks below — which exist for fresh
           expression-start quote-like operators — silently
           misinterpreting the variable name (or part of it) as a
           quote-like operator instead. Confirmed this is what was
           happening: $qqfoo produced spurious parse errors, and
           $q{key}/$qw{key}/%qw (bare, not string-interpolated) silently
           evaluated to the wrong value entirely (including the hash
           *declaration* `my %qw = (...)` itself, whose "qw" was mistaken
           for a qw()-list operator). Guard: a name immediately following
           a bare sigil token is always a plain identifier, never a
           quote-like operator. */
        bool afterSigil = !toks.empty() &&
            (toks.back().kind == TK::SCALAR || toks.back().kind == TK::ARRAY ||
             toks.back().kind == TK::HASH) &&
            toks.back().text.size() == 1;

        /* qw(...) – quote-word list. Real Perl allows whitespace
           (including newlines) between 'qw' and the delimiter
           (`qw (...)` / `qw(\n  a\n  b\n)` — Encode/Alias.pm's exact
           shape); previously the delimiter had to be adjacent, so a
           space made close=' ' and swallowed the rest of the file. */
        if (!afterSigil && c == 'q' && peek(1) == 'w' && !isalnum(peek(2)) && peek(2) != '_') {
            pos_ += 2; /* skip 'qw' */
            while (pos_ < src_.size() && (src_[pos_] == ' ' || src_[pos_] == '\t' || src_[pos_] == '\n')) {
                if (src_[pos_] == '\n') line_++;
                pos_++;
            }
            char open = peek();
            char close = (open == '(') ? ')' : (open == '[') ? ']' :
                         (open == '{') ? '}' : (open == '<') ? '>' : open;
            pos_++; /* skip opening delimiter */
            std::string words;
            while (pos_ < src_.size() && src_[pos_] != close) {
                if (src_[pos_] == '\n') line_++;
                words += src_[pos_++];
            }
            if (pos_ < src_.size()) pos_++; /* skip closing delimiter */
            toks.push_back({TK::QWORDS, words, line_}); continue;
        }

        /* q{} qq{} — with balanced-brace support.
           D60: also recognize bare q/qq with a non-brace delimiter —
           `q(...)`, `q[...]`, `q<...>`, `q/.../` were previously only
           reachable via qq(...) et al; a bare single-q with one of these
           delimiters fell through to ordinary bareword lexing entirely.
           Deliberately restricted to this small, unambiguous bracket/
           slash set rather than "any non-alnum punctuation": `q`, being
           a single short letter, appears immediately before many other
           punctuation characters in entirely unrelated contexts — most
           importantly a hash-subscript bareword key (`$h{q}`, where
           peek(1) is '}') — and treating those as quote-delimiter starts
           would silently break such existing, unrelated code. Bracket/
           slash delimiters carry no such risk: '(' '[' '<' '/' never
           legitimately follow a bareword `q` in valid Perl outside the
           quote-like-operator meaning. */
        auto isQDelimStart = [](char ch) {
            /* Real Perl accepts almost any non-alphanumeric, non-whitespace
               character as a q/qq delimiter (q!..!, q%..%, q#..#, q$..$,
               even q,..,). Only the commonest ones used to be accepted,
               which broke real-world modules (Encode/Alias.pm's q$...$).
               Closers } ] > ) and = are excluded exactly like qw()'s own
               guard (D60's regression: `$h{q}` must stay a bareword key —
               real Perl also refuses `q}`-style openers). */
            return ch && !isalnum((unsigned char)ch) && ch != '_' &&
                   ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' &&
                   ch != '}' && ch != ']' && ch != ')' && ch != '>' &&
                   ch != '='; /* q=..= conflicts with POD/directives */
        };
        /* D65: unlike qw's existing `!isalnum(peek(2)) && peek(2) != '_'`
           guard above, the "qq" branch here had NO check at all that the
           character after "qq" looks like a genuine delimiter — it
           unconditionally treated whatever followed as the delimiter,
           even an ordinary alphanumeric/underscore character. That's how
           an identifier like "qqfoo" (peek(2)=='f') or "qq_slash"
           (peek(2)=='_') got misparsed as qq with a bizarre single-letter
           delimiter, scanning far ahead for another occurrence of that
           same letter and corrupting the token stream. Mirrors qw's own
           guard now. */
        auto isQQDelim = [](char ch) {
            return ch != '\0' && !isalnum((unsigned char)ch) && ch != '_';
        };
        if (!afterSigil && c == 'q' &&
            (peek(1) == '{' || (peek(1) == 'q' && isQQDelim(peek(2))) || isQDelimStart(peek(1)))) {
            bool dq = (peek(1) == 'q');
            /* Only 'q' itself is skipped here when the delimiter is a
               real punctuation character (not part of "qq") — the
               delimiter itself must stay in place for the peek()=='{'
               check and the qopen/qclose fallback just below to see it.
               For genuine "qq", both letters are skipped, landing on the
               delimiter that follows them instead. */
            pos_ += (dq ? 2 : 1);
            if (peek() == '{') {
                pos_++; /* skip '{' */
                /* balanced brace scan */
                std::string raw;
                bool qqWide = false;
                int depth = 1;
                while (pos_ < src_.size() && depth > 0) {
                    char ch = src_[pos_];
                    if (ch == '\\' && pos_+1 < src_.size()) {
                        char esc = src_[pos_+1]; pos_ += 2;
                        if (!dq) {
                            /* q{...}: only \\ and escaped braces are special */
                            if (esc == '\\' || esc == '{' || esc == '}') raw += esc;
                            else { raw += '\\'; raw += esc; }
                            continue;
                        }
                        /* D154: qq{...} shares readString's escape set */
                        if (esc == '{' || esc == '}') raw += esc;
                        else appendEscape(esc, raw, qqWide);
                        continue;
                    }
                    if (ch == '{') depth++;
                    else if (ch == '}') { if (--depth == 0) { pos_++; break; } }
                    if (ch == '\n') line_++;
                    raw += ch; pos_++;
                }
                Token t{TK::STRING, dq ? "\x01" + raw : raw, line_};
                t.wide = qqWide;
                toks.push_back(t); continue;
            }
            /* q<delim>...<delim> / qq<delim>...<delim> with a non-'{'
               delimiter, e.g. qq(...), q[...], qq/.../ — the closing
               delimiter must match the actual opening one (mirroring
               qw()'s open->close mapping above); this previously
               hardcoded '}' regardless, silently corrupting/losing
               qq(...)-style strings entirely (confirmed: qq(hello world)
               produced no output at all, since the scan for a literal
               '}' that never appears ran off past the intended end). */
            char qopen  = peek();
            char qclose = (qopen == '(') ? ')' : (qopen == '[') ? ']' :
                          (qopen == '{') ? '}' : (qopen == '<') ? '>' : qopen;
            Token t = readString(qclose, /*interpolates=*/dq);
            if (dq) t.text = "\x01" + t.text;
            toks.push_back(t); continue;
        }

        /* qx(CMD) / qx{CMD} / qx/CMD/ — command execution, the exact
           same semantics as backticks `` `CMD` `` (real Perl treats
           them as two spellings of one operator). D161: previously
           not recognized AT ALL (a hard parse error, "String found
           where operator expected") — qx(...) just fell through to
           being read as a bareword `qx` call followed by a parenthesized
           expression. Mirrors qq's own delimiter handling one level up
           (including the `{...}` nested-brace-depth scan) since qx is
           semantically "qq-style interpolating text whose result
           executes as a shell command", not a separate feature —
           producing a TK::BACKTICK token so it reuses every bit of the
           backtick codegen/interpolation path unchanged (including the
           \$/\@ escape fix just above). `qx` itself is never a valid
           bareword sigil/arrow target, so no afterSigil/afterArrow
           guard is needed the way q/qq/qr/m/s/tr all carry one for
           their single-letter names ($q, ->q, etc.) — "qx" as an
           identifier prefix is already a vanishingly rare, and this
           still only fires when a genuine delimiter character follows. */
        if (c == 'q' && peek(1) == 'x') {
            size_t lookQx = 2;
            while (peek(lookQx) == ' ' || peek(lookQx) == '\t') lookQx++;
            char dQx = peek(lookQx);
            bool closerDelimQx = (dQx == '}' || dQx == ']' || dQx == ')' || dQx == '>');
            bool fatCommaQx = (dQx == '=' && peek(lookQx + 1) == '>');
            if (!closerDelimQx && !fatCommaQx &&
                dQx != '\0' && !isalnum((unsigned char)dQx) && dQx != '_' &&
                dQx != ' ' && dQx != '\t' && dQx != '\n' && dQx != '\r') {
                pos_ += lookQx; /* skip 'qx' and optional whitespace */
                if (peek() == '{') {
                    pos_++; /* skip '{' */
                    std::string raw;
                    bool qxWide = false;
                    int depth = 1;
                    while (pos_ < src_.size() && depth > 0) {
                        char ch = src_[pos_];
                        if (ch == '\\' && pos_ + 1 < src_.size()) {
                            char esc = src_[pos_ + 1]; pos_ += 2;
                            if (esc == '{' || esc == '}') raw += esc;
                            else appendEscape(esc, raw, qxWide);
                            continue;
                        }
                        if (ch == '{') depth++;
                        else if (ch == '}') { if (--depth == 0) { pos_++; break; } }
                        if (ch == '\n') line_++;
                        raw += ch; pos_++;
                    }
                    toks.push_back({TK::BACKTICK, "\x01" + raw, line_});
                    continue;
                }
                char qxOpen  = peek();
                char qxClose = (qxOpen == '(') ? ')' : (qxOpen == '[') ? ']' :
                               (qxOpen == '<') ? '>' : qxOpen;
                Token t = readString(qxClose, /*interpolates=*/true);
                toks.push_back({TK::BACKTICK, "\x01" + t.text, line_});
                continue;
            }
        }

        /* m/pattern/flags or m{pat}x — any non-word delimiter.
           Not after a bare sigil ($m) or `->` (method named m).
           Closers collide with `$h{m}`. Optional space: `m {pat}`. */
        if (c == 'm' && pos_ + 1 < src_.size()) {
            bool afterBareSigil = !toks.empty() &&
                (toks.back().kind == TK::SCALAR || toks.back().kind == TK::ARRAY ||
                 toks.back().kind == TK::HASH) &&
                toks.back().text.size() <= 1;
            bool afterArrow = !toks.empty() && toks.back().kind == TK::ARROW;
            size_t look = 1;
            while (peek(look) == ' ' || peek(look) == '\t') look++;
            char d = peek(look);
            bool closerDelim = (d == '}' || d == ']' || d == ')');
            /* `m =>` / `m=>` is a fat-comma-quoted bareword hash key, not
               m=pattern=. d76: `bless { m => ... }`. */
            bool fatComma = (d == '=' && peek(look + 1) == '>');
            if (!afterBareSigil && !afterArrow && !closerDelim && !fatComma &&
                d != '\0' && !isalnum((unsigned char)d) && d != '_') {
                pos_ += look; /* skip 'm' and optional whitespace */
                pos_++;       /* skip opening delimiter */
                char close = (d == '{') ? '}' : (d == '(') ? ')' :
                             (d == '[') ? ']' : (d == '<') ? '>' : d;
                bool paired = (close != d);
                toks.push_back(readRegexDelim(d, close, paired));
                continue;
            }
        }

        /* qr/pattern/flags — compiled-regex VALUE (real perl's qr//).
           Any non-word delimiter like m//, with the same collision
           guards: not after a bare sigil, not after `->` (a method named
           qr), not a closer delimiter ($h{qr}), and `qr =>` is a
           fat-comma key. */
        if (c == 'q' && pos_ + 1 < src_.size() && peek(1) == 'r' &&
            pos_ + 2 < src_.size()) {
            bool afterBareSigil = !toks.empty() &&
                (toks.back().kind == TK::SCALAR || toks.back().kind == TK::ARRAY ||
                 toks.back().kind == TK::HASH) &&
                toks.back().text.size() <= 1;
            bool afterArrow = !toks.empty() && toks.back().kind == TK::ARROW;
            size_t look = 2;
            while (peek(look) == ' ' || peek(look) == '\t') look++;
            char d = peek(look);
            bool closerDelim = (d == '}' || d == ']' || d == ')');
            bool fatComma = (d == '=' && peek(look + 1) == '>');
            if (!afterBareSigil && !afterArrow && !closerDelim && !fatComma &&
                d != '\0' && !isalnum((unsigned char)d) && d != '_') {
                pos_ += look; /* skip 'qr' and optional whitespace */
                pos_++;       /* skip opening delimiter */
                char close = (d == '{') ? '}' : (d == '(') ? ')' :
                             (d == '[') ? ']' : (d == '<') ? '>' : d;
                bool paired = (close != d);
                Token t = readRegexDelim(d, close, paired);
                toks.push_back({TK::QR, t.text, t.line});
                continue;
            }
        }

        /* s/pattern/replacement/flags — any non-word delimiter.
           Not after a bare sigil ($s / @s / %s are variable names).
           Closers `}` `]` `)` are legal-but-vanishingly-rare s/// openers
           and collide with the common hash-key form `$h{s}` (D66 remainder). */
        if (c == 's' && pos_ + 1 < src_.size()) {
            char d = peek(1);
            bool afterBareSigil = !toks.empty() &&
                (toks.back().kind == TK::SCALAR || toks.back().kind == TK::ARRAY ||
                 toks.back().kind == TK::HASH) &&
                toks.back().text.size() <= 1;
            bool closerDelim = (d == '}' || d == ']' || d == ')');
            if (!afterBareSigil && !closerDelim &&
                !isalnum((unsigned char)d) && d != '_' && d != '\0' &&
                d != ' ' && d != '\t' && d != '\n' && d != '\r') {
                pos_ += 2; /* skip 's' and opening delim */
                toks.push_back(readSubst());
                continue;
            }
        }

        /* tr/search/replace/flags  or  y/search/replace/flags —
           any non-word delimiter, matching s/// (real Perl: tr|/|_,
           tr{/}{_}, tr!..! all work; previously only '/' was accepted,
           which made update-xmlcatalog's `$x =~ tr|/|_|;` a hard parse
           error). Same guard as s///: not after a bare sigil, closers
           excluded ($h{tr} collision). */
        {
            bool afterBareSigilTr = !toks.empty() &&
                (toks.back().kind == TK::SCALAR || toks.back().kind == TK::ARRAY ||
                 toks.back().kind == TK::HASH) &&
                toks.back().text.size() <= 1;
            /* {y=>1} / {tr=>1}: a bareword hash KEY spelled y/tr — the
               '=>' fatarrow disqualifies the y/// / tr/// operator (real
               perl's tokenizer treats '=>' as the key separator, not a
               delimiter) */
            bool isTr = !afterBareSigilTr &&
                        (c == 't' && peek(1) == 'r' &&
                         peek(2) && !isalnum((unsigned char)peek(2)) &&
                         peek(2) != '_' && peek(2) != ' ' && peek(2) != '\t' &&
                         peek(2) != '\n' && peek(2) != '\r' &&
                         peek(2) != '}' && peek(2) != ']' && peek(2) != ')' &&
                         !(peek(2) == '=' && peek(3) == '>'));
            bool isY  = !afterBareSigilTr &&
                        (c == 'y' && peek(1) && !isalnum((unsigned char)peek(1)) &&
                         peek(1) != '_' && peek(1) != ' ' && peek(1) != '\t' &&
                         peek(1) != '\n' && peek(1) != '\r' &&
                         peek(1) != '}' && peek(1) != ']' && peek(1) != ')' &&
                         !(peek(1) == '=' && peek(2) == '>'));
            if (isTr || isY) {
                char delim = isTr ? peek(2) : peek(1);
                size_t skip = isTr ? 3 : 2;
                pos_ += skip;
                char close = delim;
                if      (delim == '{') close = '}';
                else if (delim == '(') close = ')';
                else if (delim == '[') close = ']';
                else if (delim == '<') close = '>';
                auto readSec = [&](char d) {
                    std::string s;
                    int depth = (close != delim) ? 1 : 0;
                    while (pos_ < src_.size()) {
                        char ch = src_[pos_];
                        if (ch == delim && close != delim) depth++;
                        if (ch == close) {
                            if (close != delim) {
                                if (--depth == 0) { pos_++; break; }
                            } else { pos_++; break; }
                        }
                        if (ch == '\\' && pos_ + 1 < src_.size()) { s += ch; s += src_[++pos_]; pos_++; continue; }
                        s += ch; pos_++;
                    }
                    return s;
                };
                std::string search = readSec(close);
                /* skip optional whitespace between sections for paired delims */
                if (close != delim) {
                    while (pos_ < src_.size() && (src_[pos_] == ' ' || src_[pos_] == '\t' || src_[pos_] == '\n')) {
                        if (src_[pos_] == '\n') line_++;
                        pos_++;
                    }
                    if (pos_ < src_.size()) {
                        char o2 = src_[pos_++];
                        close = o2;
                        if      (o2 == '{') close = '}';
                        else if (o2 == '(') close = ')';
                        else if (o2 == '[') close = ']';
                        else if (o2 == '<') close = '>';
                    }
                }
                std::string repl = readSec(close);
                std::string flags;
                while (pos_ < src_.size() && isalpha(src_[pos_])) flags += src_[pos_++];
                toks.push_back({TK::TR, search + "\x01" + repl + "\x01" + flags, line_});
                continue;
            }
        }

        /* identifiers and keywords */
        if (isalpha(c) || c == '_') {
            Token t = readIdent();
            /* __END__ / __DATA__: stop the program; remaining text is <DATA>.
               In the main file both tokens create the DATA handle (perlmod). */
            if (t.text == "__END__" || t.text == "__DATA__") {
                skipLineComment(); /* rest of this line */
                if (pos_ < src_.size() && src_[pos_] == '\n') {
                    pos_++;
                    line_++;
                }
                dataSection_ = src_.substr(pos_);
                hasDataSection_ = true;
                toks.push_back({TK::EOF_TOK, "", line_});
                stampFile(toks);
                return toks;
            }
            toks.push_back(t);
            continue;
        }

        /* sigils — but % after an expression-ending token is modulo */
        if (c == '$' || c == '@') {
            TK k = (c == '$') ? TK::SCALAR : TK::ARRAY;
            pos_++;
            toks.push_back({k, std::string(1, c), line_});
            if (tryReadMainColonIdent()) continue; /* D121: $::name / @::arr — D110: also @::arr (previously only $ and %) */
            /* $#arr — last index of array. D110: the name may be
               package-qualified ($#Other::arr); without the :: handling
               the scan stopped at the bare segment and left "::arr" for
               the parser (hard parse error downstream). Mirrors
               readIdent()'s own :: handling. */
            if (c == '$' && pos_ < src_.size() && src_[pos_] == '#') {
                char nxt = (pos_+1 < src_.size()) ? src_[pos_+1] : 0;
                if (isalpha((unsigned char)nxt) || nxt == '_') {
                    pos_++; /* skip '#' */
                    std::string arrname;
                    while (pos_ < src_.size() && (isalnum((unsigned char)src_[pos_]) || src_[pos_] == '_'))
                        arrname += src_[pos_++];
                    while (pos_ + 1 < src_.size() && src_[pos_] == ':' && src_[pos_ + 1] == ':') {
                        pos_ += 2;
                        arrname += "::";
                        while (pos_ < src_.size() && (isalnum((unsigned char)src_[pos_]) || src_[pos_] == '_'))
                            arrname += src_[pos_++];
                    }
                    toks.back().text = "#" + arrname;
                    continue;
                }
            }
    if (c == '$' && pos_ < src_.size() && src_[pos_] == '+') {
      pos_++;
      toks.back().text = "+";
      continue;
    }
            /* $^X control variables: encode as "^X" in the SCALAR token text */
            if (c == '$' && pos_ < src_.size() && src_[pos_] == '^'
                    && pos_+1 < src_.size() && isupper((unsigned char)src_[pos_+1])) {
                char ctrl = src_[pos_+1];
                pos_ += 2;
                toks.back().text = std::string("^") + ctrl;
                continue;
            }
            /* $/ — force SLASH (not regex) immediately after $ sigil */
            if (c == '$' && pos_ < src_.size() && src_[pos_] == '/') {
                pos_++;
                toks.push_back({TK::SLASH, "/", line_});
            }
            continue;
        }
        if (c == '%') {
            /* heuristic: % is modulo if previous token ends an expression */
            bool afterValue = !toks.empty() && [&]{
                switch (toks.back().kind) {
                    case TK::INT: case TK::FLOAT: case TK::STRING:
                    case TK::IDENT: case TK::RPAREN: case TK::RBRACKET:
                    case TK::PLUS_PLUS: case TK::MINUS_MINUS:
                        return true;
                    default: return false;
                }
            }();
            if (afterValue) {
                pos_++;
                if (peek() == '=') { pos_++; toks.push_back({TK::PERCENT_ASSIGN, "%=", line_}); }
                else toks.push_back({TK::PERCENT, "%", line_});
            } else {
                pos_++;
                toks.push_back({TK::HASH, "%", line_});
                if (tryReadMainColonIdent()) continue; /* D121: %::hash */
            }
            continue;
        }

        /* '/' is division after a value, regex otherwise */
        if (c == '/') {
            bool afterValue = !toks.empty() && [&]{
                switch (toks.back().kind) {
                    case TK::INT: case TK::FLOAT: case TK::STRING: case TK::REGEX:
                    case TK::IDENT: case TK::RPAREN: case TK::RBRACKET:
                    case TK::RBRACE:   /* } // default — hash/block result */
                    case TK::KW_UNDEF: /* undef // default */
                    case TK::PLUS_PLUS: case TK::MINUS_MINUS:
                        return true;
                    default: return false;
                }
            }();
            pos_++; /* consume '/' */
            if (afterValue) {
                if (peek() == '/') {
                    pos_++;
                    if (peek() == '=') { pos_++; toks.push_back({TK::DEFINED_OR_ASSIGN, "//=", line_}); }
                    else toks.push_back({TK::DEFINED_OR, "//", line_});
                }
                else if (peek() == '=') { pos_++; toks.push_back({TK::SLASH_ASSIGN, "/=", line_}); }
                else toks.push_back({TK::SLASH, "/", line_});
            } else {
                toks.push_back(readRegex());
            }
            continue;
        }

        pos_++; /* consume c */

        switch (c) {
            case '(':  toks.push_back({TK::LPAREN,   "(", line_}); break;
            case ')':  toks.push_back({TK::RPAREN,   ")", line_}); break;
            case '{':  toks.push_back({TK::LBRACE,   "{", line_}); break;
            case '}':  toks.push_back({TK::RBRACE,   "}", line_}); break;
            case '[':  toks.push_back({TK::LBRACKET, "[", line_}); break;
            case ']':  toks.push_back({TK::RBRACKET, "]", line_}); break;
            case ';':  toks.push_back({TK::SEMI,     ";", line_}); break;
            case ',':  toks.push_back({TK::COMMA,    ",", line_}); break;
            case '\\': toks.push_back({TK::BACKSLASH,"\\",line_}); break;
            case '?':  toks.push_back({TK::QUESTION, "?", line_}); break;
            case ':':  toks.push_back({TK::COLON,    ":", line_}); break;
            case '+':
                if (peek() == '+') { pos_++; toks.push_back({TK::PLUS_PLUS, "++", line_}); }
                else if (peek() == '=') { pos_++; toks.push_back({TK::PLUS_ASSIGN, "+=", line_}); }
                else toks.push_back({TK::PLUS, "+", line_});
                break;
            case '-':
                if (peek() == '-') { pos_++; toks.push_back({TK::MINUS_MINUS, "--", line_}); }
                else if (peek() == '=') { pos_++; toks.push_back({TK::MINUS_ASSIGN, "-=", line_}); }
                else if (peek() == '>') { pos_++; toks.push_back({TK::ARROW, "->", line_}); }
                else {
                    /* file test: -e/-f/-d/-r/-w/-x/-z/-s/-l/-p only at expression start */
                    static const std::string ftOps = "efdrzswxoltpSTMABC";
                    bool afterVal = !toks.empty() && [&]{
                        switch (toks.back().kind) {
                            case TK::INT: case TK::FLOAT: case TK::STRING:
                            case TK::IDENT: case TK::RPAREN: case TK::RBRACKET:
                            case TK::PLUS_PLUS: case TK::MINUS_MINUS: return true;
                            default: return false;
                        }
                    }();
                    char nxt = peek();
                    if (!afterVal && isalpha(nxt) && ftOps.find(nxt) != std::string::npos
                            && (pos_ + 1 >= src_.size() || (!isalnum(src_[pos_+1]) && src_[pos_+1] != '_'))) {
                        pos_++;
                        toks.push_back({TK::FILETEST, std::string(1, nxt), line_});
                    } else {
                        toks.push_back({TK::MINUS, "-", line_});
                    }
                }
                break;
            case '*':
                if (peek() == '*') {
                    pos_++;
                    if (peek() == '=') { pos_++; toks.push_back({TK::POW_ASSIGN, "**=", line_}); }
                    else toks.push_back({TK::STAR_STAR, "**", line_});
                }
                else if (peek() == '=') { pos_++; toks.push_back({TK::STAR_ASSIGN, "*=", line_}); }
                else toks.push_back({TK::STAR, "*", line_});
                break;
            case '%':
                toks.push_back({TK::PERCENT, "%", line_}); break;
            case '.':
                if (peek() == '.') { pos_++; toks.push_back({TK::DOTDOT, "..", line_}); }
                else if (peek() == '=') { pos_++; toks.push_back({TK::DOT_ASSIGN, ".=", line_}); }
                else toks.push_back({TK::DOT, ".", line_});
                break;
            case '=':
                if (peek() == '=') { pos_++; toks.push_back({TK::EQ, "==", line_}); }
                else if (peek() == '>') { pos_++; toks.push_back({TK::FATARROW, "=>", line_}); }
                else if (peek() == '~') { pos_++; toks.push_back({TK::BIND, "=~", line_}); }
                else toks.push_back({TK::ASSIGN, "=", line_});
                break;
            case '!':
                if (peek() == '=') { pos_++; toks.push_back({TK::NE, "!=", line_}); }
                else if (peek() == '~') { pos_++; toks.push_back({TK::NBIND, "!~", line_}); }
                else toks.push_back({TK::NOT, "!", line_});
                break;
            case '<': {
                if (peek() == '<') {
                    pos_++; /* consume second < */
                    Token t = readHeredoc();
                    if (t.kind != TK::EOF_TOK) { toks.push_back(t); break; }
                    /* not a heredoc — emit shift operator */
                    if (peek() == '=') { pos_++; toks.push_back({TK::LSHIFT_ASSIGN, "<<=", line_}); }
                    else toks.push_back({TK::LSHIFT, "<<", line_});
                    break;
                }
                if (peek() == '=' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '>') {
                    pos_ += 2; toks.push_back({TK::SPACESHIP, "<=>", line_}); break;
                }
                if (peek() == '=') { pos_++; toks.push_back({TK::LE, "<=", line_}); break; }
                /* readline: <$ident>, <STDIN>, <STDERR>, <STDOUT>, <> */
                {
                    size_t save = pos_;
                    std::string rl;
                    if (pos_ < src_.size() && src_[pos_] == '$') { pos_++; }
                    while (pos_ < src_.size() && (isalnum((unsigned char)src_[pos_]) || src_[pos_] == '_'))
                        rl += src_[pos_++];
                    if (pos_ < src_.size() && src_[pos_] == '>') {
                        pos_++;  /* consume '>' */
                        toks.push_back({TK::READLINE, rl, line_});
                        break;
                    }
                    pos_ = save;
                }
                /* D164: diamond-glob <PATTERN> — real Perl treats <...>
                   as glob(...) whenever its contents aren't a bare
                   filehandle-shaped token (pure identifier, optionally
                   `$`-prefixed) — e.g. <*.c>, <path/*.txt>, <~/.bashrc>.
                   Single-line only (bail to plain '<' on newline/EOF),
                   matching real Perl. Emits the same TK::READLINE token
                   the identifier case uses; codegen's case NK::Readline
                   distinguishes a glob pattern from a filehandle name by
                   checking whether the text is a pure identifier. Note:
                   the pattern text is NOT variable-interpolated (unlike
                   real Perl's <$dir/*.txt> form) — scoped out; a literal
                   pattern (the common real-world shape) works.

                   Only attempted where a *term* is expected, not after a
                   value — a genuine less-than ($a < $b, foo() < 5, ...)
                   always has a value token immediately before '<', so
                   this guard is both necessary and sufficient to avoid
                   misparsing `$a < $b > $c`-shaped comparisons as a
                   diamond-glob scan swallowing everything up to the
                   first unrelated later '>' on the same line. */
                bool ltAfterVal = !toks.empty() && [&]{
                    switch (toks.back().kind) {
                        case TK::INT: case TK::FLOAT: case TK::STRING:
                        case TK::IDENT: case TK::RPAREN: case TK::RBRACKET:
                        case TK::PLUS_PLUS: case TK::MINUS_MINUS:
                            return true;
                        default: return false;
                    }
                }();
                if (!ltAfterVal) {
                    /* Extra safety belt beyond ltAfterVal: only scan
                       through characters that plausibly belong in a
                       filename/glob pattern. Any other character (a
                       space, &&, =, etc.) aborts the scan immediately
                       rather than continuing to hunt for a '>' — this
                       keeps something like `$h{k} < 5 && $x > 2`
                       (where RBRACE isn't in the afterVal set above)
                       from being swallowed as a bogus glob pattern. */
                    size_t save = pos_;
                    std::string rl;
                    static const std::string globSafeChars =
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                        "0123456789_./\\-*?[]~@:+,$";
                    while (pos_ < src_.size() && src_[pos_] != '>' &&
                           globSafeChars.find(src_[pos_]) != std::string::npos)
                        rl += src_[pos_++];
                    if (!rl.empty() && pos_ < src_.size() && src_[pos_] == '>') {
                        pos_++;  /* consume '>' */
                        toks.push_back({TK::READLINE, rl, line_});
                        break;
                    }
                    pos_ = save;
                }
                toks.push_back({TK::LT, "<", line_});
                break;
            }
            case '>':
                if (peek() == '>') {
                    pos_++;
                    if (peek() == '=') { pos_++; toks.push_back({TK::RSHIFT_ASSIGN, ">>=", line_}); }
                    else toks.push_back({TK::RSHIFT, ">>", line_});
                }
                else if (peek() == '=') { pos_++; toks.push_back({TK::GE, ">=", line_}); }
                else toks.push_back({TK::GT, ">", line_});
                break;
            case '&':
                if (peek() == '&') {
                    pos_++;
                    if (peek() == '=') { pos_++; toks.push_back({TK::AND_ASSIGN, "&&=", line_}); }
                    else toks.push_back({TK::AND2, "&&", line_});
                }
                else if (peek() == '=') { pos_++; toks.push_back({TK::BITAND_ASSIGN, "&=", line_}); }
                else toks.push_back({TK::AND, "&", line_});
                break;
            case '|':
                if (peek() == '|') {
                    pos_++;
                    if (peek() == '=') { pos_++; toks.push_back({TK::OR_ASSIGN, "||=", line_}); }
                    else toks.push_back({TK::OR2, "||", line_});
                }
                else if (peek() == '=') { pos_++; toks.push_back({TK::BITOR_ASSIGN, "|=", line_}); }
                else toks.push_back({TK::OR, "|", line_});
                break;
            case '^':
                if (peek() == '=') { pos_++; toks.push_back({TK::BITXOR_ASSIGN, "^=", line_}); }
                else toks.push_back({TK::CARET, "^", line_});
                break;
            case '~':
                toks.push_back({TK::TILDE, "~", line_});
                break;
            default:
                /* skip unknown */
                break;
        }
    }

    /* handle string-comparison operators that look like barewords */
    /* (eq ne lt gt le ge) — already handled as IDENT; parser must promote */

    toks.push_back({TK::EOF_TOK, "", line_});
    stampFile(toks);
    return toks;
}
