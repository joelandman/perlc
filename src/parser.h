#pragma once
#include "lexer.h"
#include "ast.h"
#include <vector>
#include <map>
#include <set>
#include <string>

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    /* importMap: short_name → qualified Module::name for re-exported symbols */
    void setImportMap(std::map<std::string, std::string> m) { importMap_ = std::move(m); }
    void setConstMap(std::map<std::string, NodePtr> m)      { constMap_  = std::move(m); }
    /* W19: non-owning view for parseExprFromTokens' throwaway parser —
       constMap_ entries are cloned at each use (parsePrimary), so sharing
       the map without transferring ownership is safe for the parse's
       lifetime. */
    void setConstMapView(const std::map<std::string, NodePtr> *m) {
        for (const auto &kv : *m) constMap_[kv.first] = kv.second->clone();
    }
    static NodePtr parseExprFromTokens(std::vector<Token> tokens);  /* pre-parse const value expr */
    /* W19: same, but seed the throwaway parser with the constants already
       declared by earlier `use constant` statements — `use constant B => A + 1`
       parses its value in a fresh parser, and without the map `A` falls to
       the bareword-call heuristic (PLUS is a valid arg starter), producing
       Call(A,[1]) instead of the constant's value. main.cpp must pass its
       constMap for the chain to resolve (parser-side support only). */
    static NodePtr parseExprFromTokens(std::vector<Token> tokens,
                                       const std::map<std::string, NodePtr> *consts);
    /* D109: entry point for codegen to interpolate a raw string (e.g. an
       s///REPLACEMENT/ that isn't a plain literal) outside of a normal
       parse pass, using the same variable-interpolation scanner ordinary
       "..." literals go through. `pkg` seeds currentPackage_ so a bare
       `$Pkg::var` in the text resolves against the caller's actual
       compiling package instead of always "main". */
    static NodePtr parseInterpString(const std::string &raw, int line,
                                      const std::string &pkg = "main");
    static NodeList parseExprListFromTokens(std::vector<Token> tokens);  /* comma-separated list, e.g. slice indices/keys */
    NodePtr parseProgram();   /* returns a Block */
    /* D56: warnings state accessors for codegen */
    bool getWarningsEnabled() const      { return warningsEnabled_; }
    bool getWarningsUninitialized() const { return warningsUninitialized_; }
    /* D128: tell the parser which registered source-file tag (Token::file)
       belongs to the main script. A parse error whose current token carries
       a different (or any, when this is unset) non-null tag is reported as
       "Parse error in <file> line N:" — the module's own file and internal
       line — instead of the legacy main-script-only format. */
    void setMainFileTag(const char *t)   { mainFileTag_ = t; }

private:
    std::vector<Token>              toks_;
    size_t                          pos_ = 0;
    std::string                     currentPackage_ = "main";
    int                             subDepth_        = 0;
    int                             anonCount_       = 0; /* D10: per-parse anon sub ids */
    std::map<std::string,std::string> importMap_;  /* short → qualified call names */
    std::map<std::string,NodePtr>     constMap_;   /* constant name → parsed AST */
    std::map<std::string,std::string> protoMap_;   /* sub name → prototype string */
    bool inKeyContext_ = false;  /* true when parsing hash keys — barewords are strings */

    /* D56: warnings pragma state */
    bool warningsEnabled_     = false;
    bool warningsUninitialized_ = false;
    bool signaturesEnabled_   = false; /* use v5.20+ / use feature 'signatures' */
    bool utf8Enabled_         = false; /* use utf8 — string lits are characters */
    std::set<std::string>    knownBareFH_; /* open LOG, ... → print LOG */
    /* D128: registered Token::file tag of the main script (nullptr when
       unset — every non-null tag then reports as a foreign file). */
    const char *mainFileTag_ = nullptr;
    /* D128: the one place that decides how a parse error is prefixed.
       Token is tagged with its source file (Token::file); an erroring
       token from an inlined module reports the module's own file name and
       internal line ("Parse error in X.pm line N: msg"); a main-script or
       untagged token keeps the exact legacy "Parse error line N: msg". */
    std::string parseErrPrefix(int line) const;
    NodePtr litStr(std::string s, int line); /* StringLit; UTF-8 flag if use utf8 */

    Token &cur();
    Token &peek(int offset = 1);
    bool   check(TK k) const;
    bool   checkIdent(const std::string &s) const;
    Token  consume(TK k, const char *msg = nullptr);
    Token  advance();
    bool   match(TK k);

    NodePtr parseStmt();
    NodePtr parseBlock();
    /* D125: the whole `use MODULE`/`use pragma`/`no PRAGMA` statement
       handling, previously reachable only from parseProgram()'s file-level
       loop — so `use strict;` / `no warnings 'numeric';` inside any
       nested block or sub body was a hard parse error. Extracted verbatim
       (the caller has already consumed nothing) so both parseProgram() and
       parseStmt() share one implementation. Advances past the statement
       and returns the statement node(s) to splice into the caller's
       statement list (multiple for `use parent`), or an empty Block for
       fully-ignored pragmas. */
    NodePtr parseUseNoStmt();
    NodePtr parseIf();
    NodePtr parseWhile();
    NodePtr parseFor();
    NodePtr parseForeach();
    NodePtr parseForeachBody(int line);
    NodePtr parseSub();
    NodePtr parseMy();
    NodePtr parsePrint(bool isSay);
    NodePtr parsePush();
    NodePtr parseUnshift();
    NodePtr parseReturn();

    bool    isModifier() const;
    NodePtr parseModifier(NodePtr stmt, int line);
    void    consumeLowOrChain();  /* consume or/and/xor statement separators */

    NodePtr parseExpr();
    NodePtr parseLowOr();
    NodePtr parseLowAnd();
    NodePtr parseLowNot();
    NodePtr parseOrRhs();  /* rhs of low-precedence or/and — may be a stmt */
    NodePtr parseDieWarnBody(bool isDie, int line);  /* die/warn body without leading KW or trailing `;` */
    NodePtr parseLastNextRedoBody(NK kind, int line);  /* last/next/redo body without leading KW or trailing `;` */
    NodePtr parseAssign();
    NodePtr parseTernary();
    NodePtr parseRange();
    NodePtr parseOr();
    NodePtr parseAnd();
    NodePtr parseBitOr();
    NodePtr parseBitAnd();
    NodePtr parseNot();
    NodePtr parseCmp();
    NodePtr parseBinding();
    NodePtr parseShift();
    NodePtr parseAdd();
    NodePtr parseMul();
    NodePtr parseUnary();
    NodePtr parsePow();
    NodePtr parsePostfix();
    NodePtr parsePrimary();
    NodePtr parseCall(std::string name, int line);
    bool    looksLikeBareCallArg() const; /* D36 */
    /* W23: inside a hash-key context, is the current keyword token a
       builtin followed by an argument starter (so it is a real call,
       not a string key)?  cur() must be a KW_* token. */
    bool    inKeyBuiltinFollowedByArg() const;
    NodePtr parseBareCall(std::string name, int line); /* D36: foo "arg" without () */
    std::string parsePrototype();
    bool        looksLikeSignature() const;
    NodePtr     parseSignaturePrefix(int line); /* my ($x,$y)=@_; defaults */
    void        rememberProto(const std::string &name, const std::string &proto);
    const std::string *lookupProto(const std::string &name) const;
    void        checkProtoArity(const std::string &name, const std::string &proto,
                                int nargs, int line);
    NodePtr     parseAmpBlockCall(std::string name, const std::string &proto, int line);
    NodePtr     parseAnonSubBody(int line, const std::string &proto,
                                 NodePtr sigPrefix = nullptr);
    NodePtr parseStringInterp(const std::string &raw, int line);
    NodePtr parseSubscript(NodePtr base, int line); /* chains ->[]/->{}  */
    /* D120: subscript group(s) inside interpolated strings — see
       parseStringInterp for the node shapes. */
    NodePtr parseSubscriptGroup(const std::string &raw, size_t &i, int line,
                                const char *nameRef, bool isOpenBracket,
                                NodePtr exprRef = nullptr);

    NodeList parseArgList();
};
