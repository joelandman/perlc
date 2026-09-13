#pragma once
#include <string>
#include <vector>

/* D128: registers a source-file name in a process-wide, address-stable
   registry and returns a pointer that stays valid for the process's
   lifetime (Token::file points at it — token streams are copied around
   by value and outlive their Lexer, so the pointed-at storage must never
   move or be freed). */
const char *lexer_register_source_file(const std::string &name);

enum class TK {
    /* literals */
    INT, FLOAT, STRING, REGEX,
    /* identifiers / keywords */
    IDENT,
    KW_MY, KW_OUR, KW_LOCAL,
    KW_IF, KW_ELSIF, KW_ELSE, KW_UNLESS,
    KW_WHILE, KW_UNTIL, KW_FOR, KW_FOREACH, KW_DO,
    KW_LAST, KW_NEXT, KW_REDO, KW_RETURN, KW_GOTO,
    KW_SUB, KW_USE, KW_STRICT, KW_WARNINGS,
    KW_PRINT, KW_SAY, KW_PRINTF, KW_SPRINTF,
    KW_OPEN, KW_CLOSE, KW_EOF, KW_DIE, KW_UNLINK,
    KW_PUSH, KW_POP, KW_SHIFT, KW_UNSHIFT,
    KW_SCALAR, KW_DEFINED, KW_UNDEF,
    KW_AND, KW_OR, KW_NOT,
    KW_KEYS, KW_VALUES, KW_EXISTS, KW_DELETE, KW_EACH, KW_SORT,
    KW_CHOMP, KW_CHOP, KW_LENGTH, KW_SUBSTR, KW_JOIN, KW_SPLIT,
    KW_INDEX, KW_RINDEX, KW_UC, KW_LC, KW_UCFIRST, KW_LCFIRST,
    KW_REVERSE, KW_SPLICE, KW_REF,
    KW_ABS, KW_INT, KW_SQRT,
    KW_CHR, KW_ORD, KW_HEX, KW_OCT,
    KW_MAP, KW_GREP,
    KW_WARN, KW_SYSTEM, KW_EVAL,
    KW_BLESS, KW_PACKAGE,
    KW_WANTARRAY, KW_CALLER, KW_STATE,
    KW_BEGIN, KW_END,
    KW_CHDIR, KW_MKDIR, KW_RMDIR, KW_RENAME, KW_CHMOD,
    KW_OPENDIR, KW_READDIR, KW_CLOSEDIR,
    /* time / randomness / process */
    KW_RAND, KW_SRAND, KW_TIME, KW_LOCALTIME, KW_GMTIME,
    KW_SLEEP, KW_ALARM,
    /* file / filesystem */
    KW_SEEK, KW_TELL, KW_BINMODE, KW_STAT, KW_LSTAT, KW_GLOB,
    KW_READ, KW_FILENO, KW_TRUNCATE,
    /* require */
    KW_REQUIRE,
    /* misc builtins */
    KW_POS, KW_LOCK, KW_COND_WAIT, KW_COND_SIGNAL, KW_COND_BROADCAST,
    /* tie/untie */
    KW_TIE, KW_UNTIE,
    /* List::Util */
    KW_SUM, KW_MIN, KW_MAX, KW_FIRST, KW_ANY, KW_ALL, KW_NONE, KW_UNIQ, KW_REDUCE,
    /* pack/unpack */
    KW_PACK, KW_UNPACK,
    SPACESHIP,    /* <=> */
    FILETEST,  /* -e/-f/-d/-r/-w/-x/-z/-s/-l/-p – text = flag char */
    QWORDS,    /* qw(...) – text = space-separated words */
    BACKTICK,  /* `cmd`  – text = command string */
    READLINE,  /* <$fh>, <STDIN>, <> */
    /* regex binding */
    BIND,   /* =~ */
    NBIND,  /* !~ */
    SUBST,  /* s/pat/repl/flags */
    TR,     /* tr/search/replace/flags  or  y/search/replace/flags */
    /* sigils */
    SCALAR,   /* $ */
    ARRAY,    /* @ */
    HASH,     /* % */
    /* punctuation */
    LPAREN, RPAREN, LBRACE, RBRACE, LBRACKET, RBRACKET,
    SEMI, COMMA, ARROW, FATARROW, COLON,
    /* operators */
    PLUS, MINUS, STAR, SLASH, PERCENT, DOTDOT, DOT,
    STAR_STAR,                           /* ** exponentiation */
    EQ, NE, LT, GT, LE, GE,             /* numeric cmp */
    STR_EQ, STR_NE, STR_LT, STR_GT, STR_LE, STR_GE,  /* string cmp */
    AND, OR, NOT, AND2, OR2,             /* &&, ||, ! */
    DEFINED_OR,                          /* // */
    TILDE,                               /* ~ bitwise NOT */
    CARET,                               /* ^ bitwise XOR */
    LSHIFT,                              /* << */
    RSHIFT,                              /* >> */
    ASSIGN,
    PLUS_ASSIGN, MINUS_ASSIGN, STAR_ASSIGN, SLASH_ASSIGN, DOT_ASSIGN,
    PERCENT_ASSIGN, POW_ASSIGN,          /* %= **= */
    OR_ASSIGN, AND_ASSIGN,               /* ||= &&= */
    DEFINED_OR_ASSIGN,                   /* //= */
    X_ASSIGN,                            /* x= */
    BITAND_ASSIGN, BITOR_ASSIGN, BITXOR_ASSIGN,  /* &= |= ^= */
    LSHIFT_ASSIGN, RSHIFT_ASSIGN,        /* <<= >>= */
    PLUS_PLUS, MINUS_MINUS,
    QUESTION, BACKSLASH,
    /* special */
    EOF_TOK, NEWLINE,
};

struct Token {
    TK          kind;
    std::string text;
    int         line;
    /* D128: source file this token was lexed from — a stable pointer into
       the process-wide filename registry (lexer_register_source_file(),
       lexer.cpp). Token streams are copied around by value (inlineModules()
       splices module tokens into the combined stream) and outlive the
       Lexer that produced them, so a pointer into a temporary std::string
       would dangle; registry elements are guaranteed address-stable.
       nullptr means "unknown/synthetic" — tokens manufactured by the
       parser or codegen itself (e.g. const-sub splices, interpolation
       fragments) — and keeps the legacy main-script error format. */
    const char *file = nullptr;
};

class Lexer {
public:
    /* D128: `sourceName` is tagged onto every token this lexer emits (via
       the process-wide filename registry; see Token::file). The driver
       passes the main script's path; inlineModules() passes each module's
       resolved fullPath. An empty name (the default) leaves tokens
       untagged (file == nullptr) — used only for synthetic fragments
       (s///-replacement text, interpolated-string sub-expressions), which
       are not file text and should keep the legacy error format. */
    explicit Lexer(std::string src, const std::string &sourceName = "");
    std::vector<Token> tokenize();
    /* D128: the registered tag for this lexer's source file (nullptr if
       constructed with no name). */
    const char *sourceName() const { return fileTag_; }
    /* Text after __DATA__ / __END__ (the DATA filehandle). Empty if none. */
    const std::string &dataSection() const { return dataSection_; }
    bool hasDataSection() const { return hasDataSection_; }

private:
    std::string src_;
    std::string dataSection_;
    bool        hasDataSection_ = false;
    size_t      pos_  = 0;
    int         line_ = 1;
    const char *fileTag_ = nullptr; /* D128: registry pointer for this file */
    size_t      pendingHeredocPos_   = 0; /* if set, jump here after consuming the next \n */
    int         pendingHeredocLines_ = 0; /* extra line count for the heredoc body */

    char peek(int offset = 0) const;
    char advance();
    void skipLineComment();
    void skipBlockComment();
    Token readNumber();
    Token readString(char delim, bool interpolates);
    Token readHeredoc();
    Token readIdent();
    Token readRegex();
    Token readRegexDelim(char open, char close, bool paired);
    Token readSubst();
};
