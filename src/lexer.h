#pragma once
#include <string>
#include <vector>

enum class TK {
    /* literals */
    INT, FLOAT, STRING, REGEX,
    /* identifiers / keywords */
    IDENT,
    KW_MY, KW_OUR, KW_LOCAL,
    KW_IF, KW_ELSIF, KW_ELSE, KW_UNLESS,
    KW_WHILE, KW_UNTIL, KW_FOR, KW_FOREACH, KW_DO,
    KW_LAST, KW_NEXT, KW_REDO, KW_RETURN,
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
    EQ, NE, LT, GT, LE, GE,             /* numeric cmp */
    STR_EQ, STR_NE, STR_LT, STR_GT, STR_LE, STR_GE,  /* string cmp */
    AND, OR, NOT, AND2, OR2,             /* &&, ||, ! */
    ASSIGN,
    PLUS_ASSIGN, MINUS_ASSIGN, STAR_ASSIGN, SLASH_ASSIGN, DOT_ASSIGN,
    PLUS_PLUS, MINUS_MINUS,
    QUESTION, BACKSLASH,
    /* special */
    EOF_TOK, NEWLINE,
};

struct Token {
    TK          kind;
    std::string text;
    int         line;
};

class Lexer {
public:
    explicit Lexer(std::string src);
    std::vector<Token> tokenize();

private:
    std::string src_;
    size_t      pos_  = 0;
    int         line_ = 1;
    size_t      pendingHeredocPos_   = 0; /* if set, jump here after consuming the next \n */
    int         pendingHeredocLines_ = 0; /* extra line count for the heredoc body */

    char peek(int offset = 0) const;
    char advance();
    void skipLineComment();
    void skipBlockComment();
    Token readNumber();
    Token readString(char delim);
    Token readHeredoc();
    Token readIdent();
    Token readRegex();
    Token readSubst();
};
