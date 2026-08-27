#pragma once
#include "lexer.h"
#include "ast.h"
#include <vector>
#include <map>
#include <string>

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    /* importMap: short_name → qualified Module::name for re-exported symbols */
    void setImportMap(std::map<std::string, std::string> m) { importMap_ = std::move(m); }
    void setConstMap(std::map<std::string, NodePtr> m)      { constMap_  = std::move(m); }
    static NodePtr parseExprFromTokens(std::vector<Token> tokens);  /* pre-parse const value expr */
    static NodeList parseExprListFromTokens(std::vector<Token> tokens);  /* comma-separated list, e.g. slice indices/keys */
    NodePtr parseProgram();   /* returns a Block */
    /* D56: warnings state accessors for codegen */
    bool getWarningsEnabled() const      { return warningsEnabled_; }
    bool getWarningsUninitialized() const { return warningsUninitialized_; }

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

    Token &cur();
    Token &peek(int offset = 1);
    bool   check(TK k) const;
    bool   checkIdent(const std::string &s) const;
    Token  consume(TK k, const char *msg = nullptr);
    Token  advance();
    bool   match(TK k);

    NodePtr parseStmt();
    NodePtr parseBlock();
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
    NodePtr parseBareCall(std::string name, int line); /* D36: foo "arg" without () */
    std::string parsePrototype();
    void        rememberProto(const std::string &name, const std::string &proto);
    const std::string *lookupProto(const std::string &name) const;
    void        checkProtoArity(const std::string &name, const std::string &proto,
                                int nargs, int line);
    NodePtr     parseAmpBlockCall(std::string name, const std::string &proto, int line);
    NodePtr     parseAnonSubBody(int line, const std::string &proto);
    NodePtr parseStringInterp(const std::string &raw, int line);
    NodePtr parseSubscript(NodePtr base, int line); /* chains ->[]/->{}  */

    NodeList parseArgList();
};
