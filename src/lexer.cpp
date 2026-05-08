#include "lexer.h"
#include <stdexcept>
#include <unordered_map>

static const std::unordered_map<std::string, TK> KEYWORDS = {
    {"my",       TK::KW_MY},    {"our",      TK::KW_OUR},
    {"local",    TK::KW_LOCAL}, {"if",       TK::KW_IF},
    {"elsif",    TK::KW_ELSIF}, {"else",     TK::KW_ELSE},
    {"unless",   TK::KW_UNLESS},{"while",    TK::KW_WHILE},
    {"until",    TK::KW_UNTIL}, {"for",      TK::KW_FOR},
    {"foreach",  TK::KW_FOREACH},{"do",      TK::KW_DO},
    {"last",     TK::KW_LAST},  {"next",     TK::KW_NEXT},
    {"redo",     TK::KW_REDO},  {"return",   TK::KW_RETURN},
    {"sub",      TK::KW_SUB},   {"use",      TK::KW_USE},
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
    {"warn",     TK::KW_WARN},  {"system",   TK::KW_SYSTEM}, {"eval", TK::KW_EVAL},
};

Lexer::Lexer(std::string src) : src_(std::move(src)) {}

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

Token Lexer::readString(char delim) {
    pos_++; /* skip opening delimiter */
    std::string raw;
    while (pos_ < src_.size()) {
        char c = src_[pos_];
        if (c == delim) { pos_++; break; }
        if (c == '\\' && pos_ + 1 < src_.size()) {
            pos_++;
            char esc = src_[pos_++];
            switch (esc) {
                case 'n':  raw += '\n'; break;
                case 't':  raw += '\t'; break;
                case 'r':  raw += '\r'; break;
                case '\\': raw += '\\'; break;
                case '\'': raw += '\''; break;
                case '"':  raw += '"';  break;
                case '$':  raw += '$';  break;
                case '@':  raw += '@';  break;
                default:   raw += '\\'; raw += esc; break;
            }
            continue;
        }
        if (c == '\n') line_++;
        raw += c;
        pos_++;
    }
    /* For double-quoted strings, we store the raw content with $ intact —
       the parser handles interpolation by calling splitInterp() */
    return {TK::STRING, raw, line_};
}

Token Lexer::readIdent() {
    size_t start = pos_;
    while (isalnum(peek()) || peek() == '_' || peek() == ':') pos_++;
    std::string text = src_.substr(start, pos_ - start);
    auto it = KEYWORDS.find(text);
    if (it != KEYWORDS.end()) return {it->second, text, line_};
    return {TK::IDENT, text, line_};
}

Token Lexer::readRegex() {
    /* pos_ is just past the opening '/' */
    std::string pattern;
    while (pos_ < src_.size()) {
        char c = src_[pos_];
        if (c == '/') { pos_++; break; }
        if (c == '\\' && pos_ + 1 < src_.size()) {
            pattern += c;
            pattern += src_[++pos_];
            pos_++;
            continue;
        }
        if (c == '\n') line_++;
        pattern += c;
        pos_++;
    }
    /* capture trailing flags */
    std::string flags;
    while (pos_ < src_.size() && isalpha(src_[pos_])) flags += src_[pos_++];
    return {TK::REGEX, pattern + "\x01" + flags, line_};
}

Token Lexer::readSubst() {
    /* pos_ is just past 's/' — read pattern / replacement / flags */
    auto readSection = [&](char delim) {
        std::string s;
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == delim) { pos_++; break; }
            if (c == '\\' && pos_ + 1 < src_.size()) { s += c; s += src_[++pos_]; pos_++; continue; }
            if (c == '\n') line_++;
            s += c; pos_++;
        }
        return s;
    };
    std::string pattern = readSection('/');
    std::string repl    = readSection('/');
    std::string flags;
    while (pos_ < src_.size() && isalpha(src_[pos_])) flags += src_[pos_++];
    return {TK::SUBST, pattern + "\x01" + repl + "\x01" + flags, line_};
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> toks;

    while (pos_ < src_.size()) {
        char c = peek();

        /* whitespace */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(); continue;
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
            Token t = readString('"');
            t.kind = TK::STRING;
            /* prefix with \x01 to distinguish dq from sq in parser */
            t.text = "\x01" + t.text;
            toks.push_back(t);
            continue;
        }
        if (c == '\'') { toks.push_back(readString('\'')); continue; }

        /* backtick command `cmd` */
        if (c == '`') {
            advance();
            std::string cmd;
            while (pos_ < src_.size() && src_[pos_] != '`') {
                if (src_[pos_] == '\n') line_++;
                cmd += src_[pos_++];
            }
            if (pos_ < src_.size()) pos_++; /* consume closing backtick */
            /* prefix \x01 so parser treats content like a dq string (interpolation) */
            toks.push_back({TK::BACKTICK, "\x01" + cmd, line_}); continue;
        }

        /* qw(...) – quote-word list */
        if (c == 'q' && peek(1) == 'w' && !isalnum(peek(2)) && peek(2) != '_') {
            pos_ += 2; /* skip 'qw' */
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

        /* q{} qq{} */
        if (c == 'q' && (peek(1) == '{' || peek(1) == 'q')) {
            bool dq = (peek(1) == 'q');
            pos_ += 2;
            if (peek() == '{') { pos_++; }
            Token t = readString('}');
            if (dq) t.text = "\x01" + t.text;
            toks.push_back(t); continue;
        }

        /* s/pattern/replacement/flags — substitution operator */
        if (c == 's' && peek(1) == '/') { pos_ += 2; toks.push_back(readSubst()); continue; }

        /* tr/search/replace/flags  or  y/search/replace/flags */
        if ((c == 't' && peek(1) == 'r' && peek(2) == '/') ||
            (c == 'y' && peek(1) == '/')) {
            size_t skip = (c == 't') ? 3 : 2;
            pos_ += skip;
            auto readSec = [&](char delim) {
                std::string s;
                while (pos_ < src_.size()) {
                    char ch = src_[pos_];
                    if (ch == delim) { pos_++; break; }
                    if (ch == '\\' && pos_ + 1 < src_.size()) { s += ch; s += src_[++pos_]; pos_++; continue; }
                    s += ch; pos_++;
                }
                return s;
            };
            std::string search = readSec('/');
            std::string repl   = readSec('/');
            std::string flags;
            while (pos_ < src_.size() && isalpha(src_[pos_])) flags += src_[pos_++];
            toks.push_back({TK::TR, search + "\x01" + repl + "\x01" + flags, line_});
            continue;
        }

        /* identifiers and keywords */
        if (isalpha(c) || c == '_') { toks.push_back(readIdent()); continue; }

        /* sigils — but % after an expression-ending token is modulo */
        if (c == '$' || c == '@') {
            TK k = (c == '$') ? TK::SCALAR : TK::ARRAY;
            pos_++;
            toks.push_back({k, std::string(1, c), line_});
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
                if (peek() == '=') { pos_++; toks.push_back({TK::PLUS_ASSIGN, "%=", line_}); }
                else toks.push_back({TK::PERCENT, "%", line_});
            } else {
                pos_++;
                toks.push_back({TK::HASH, "%", line_});
            }
            continue;
        }

        /* '/' is division after a value, regex otherwise */
        if (c == '/') {
            bool afterValue = !toks.empty() && [&]{
                switch (toks.back().kind) {
                    case TK::INT: case TK::FLOAT: case TK::STRING: case TK::REGEX:
                    case TK::IDENT: case TK::RPAREN: case TK::RBRACKET:
                    case TK::PLUS_PLUS: case TK::MINUS_MINUS:
                        return true;
                    default: return false;
                }
            }();
            pos_++; /* consume '/' */
            if (afterValue) {
                if (peek() == '=') { pos_++; toks.push_back({TK::SLASH_ASSIGN, "/=", line_}); }
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
                    static const std::string ftOps = "efdrzswxolpSTMABC";
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
                if (peek() == '=') { pos_++; toks.push_back({TK::STAR_ASSIGN, "*=", line_}); }
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
                if (peek() == '=' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '>') {
                    pos_ += 2; toks.push_back({TK::SPACESHIP, "<=>", line_}); break;
                }
                if (peek() == '=') { pos_++; toks.push_back({TK::LE, "<=", line_}); break; }
                /* readline: <$ident>, <STDIN>, <STDERR>, <STDOUT>, <> */
                {
                    size_t save = pos_;
                    bool hasSigil = false;
                    std::string rl;
                    if (pos_ < src_.size() && src_[pos_] == '$') { hasSigil = true; pos_++; }
                    while (pos_ < src_.size() && (isalnum((unsigned char)src_[pos_]) || src_[pos_] == '_'))
                        rl += src_[pos_++];
                    if (pos_ < src_.size() && src_[pos_] == '>') {
                        bool ok = hasSigil || rl.empty()
                               || rl == "STDIN" || rl == "STDOUT" || rl == "STDERR";
                        if (ok) {
                            pos_++;  /* consume '>' */
                            toks.push_back({TK::READLINE, rl, line_});
                            break;
                        }
                    }
                    pos_ = save;
                }
                toks.push_back({TK::LT, "<", line_});
                break;
            }
            case '>':
                if (peek() == '=') { pos_++; toks.push_back({TK::GE, ">=", line_}); }
                else toks.push_back({TK::GT, ">", line_});
                break;
            case '&':
                if (peek() == '&') { pos_++; toks.push_back({TK::AND2, "&&", line_}); }
                else toks.push_back({TK::AND, "&", line_});
                break;
            case '|':
                if (peek() == '|') { pos_++; toks.push_back({TK::OR2, "||", line_}); }
                else toks.push_back({TK::OR, "|", line_});
                break;
            default:
                /* skip unknown */
                break;
        }
    }

    /* handle string-comparison operators that look like barewords */
    /* (eq ne lt gt le ge) — already handled as IDENT; parser must promote */

    toks.push_back({TK::EOF_TOK, "", line_});
    return toks;
}
