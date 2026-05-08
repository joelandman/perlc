#pragma once
#include "lexer.h"
#include "ast.h"
#include <vector>

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    NodePtr parseProgram();   /* returns a Block */

private:
    std::vector<Token> toks_;
    size_t             pos_ = 0;

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

    NodePtr parseExpr();
    NodePtr parseAssign();
    NodePtr parseTernary();
    NodePtr parseOr();
    NodePtr parseAnd();
    NodePtr parseNot();
    NodePtr parseCmp();
    NodePtr parseBinding();
    NodePtr parseAdd();
    NodePtr parseMul();
    NodePtr parseUnary();
    NodePtr parsePostfix();
    NodePtr parsePrimary();
    NodePtr parseCall(std::string name, int line);
    NodePtr parseStringInterp(const std::string &raw, int line);
    NodePtr parseSubscript(NodePtr base, int line); /* chains ->[]/->{}  */

    NodeList parseArgList();
};
