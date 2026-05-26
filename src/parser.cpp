#include "parser.h"
#include <stdexcept>
#include <sstream>
#include <cstdlib>

Parser::Parser(std::vector<Token> tokens) : toks_(std::move(tokens)) {}

NodePtr Parser::parseExprFromTokens(std::vector<Token> tokens) {
    tokens.push_back({TK::EOF_TOK, "", 0});
    Parser p(std::move(tokens));
    return p.parseExpr();
}

Token &Parser::cur()                { return toks_[pos_]; }
Token &Parser::peek(int off)        { size_t p = pos_ + off; return p < toks_.size() ? toks_[p] : toks_.back(); }
bool   Parser::check(TK k)  const  { return toks_[pos_].kind == k; }
bool   Parser::checkIdent(const std::string &s) const {
    return toks_[pos_].kind == TK::IDENT && toks_[pos_].text == s;
}
Token Parser::advance()             { return toks_[pos_++]; }
bool  Parser::match(TK k)          { if (check(k)) { pos_++; return true; } return false; }
Token Parser::consume(TK k, const char *msg) {
    if (!check(k)) {
        std::ostringstream os;
        os << "Parse error line " << cur().line << ": expected " << (msg ? msg : "token")
           << " but got '" << cur().text << "'";
        throw std::runtime_error(os.str());
    }
    return advance();
}

/* ── program ─────────────────────────────────────────────────────────────── */

NodePtr Parser::parseProgram() {
    NodeList stmts;
    while (!check(TK::EOF_TOK)) {
        if (check(TK::KW_USE)) {
            int line = cur().line;
            advance(); /* consume 'use' */
            /* use parent 'Base'  or  use base 'Base'  or  use parent qw(...) */
            if (check(TK::IDENT) &&
                (cur().text == "parent" || cur().text == "base")) {
                advance();
                std::vector<std::string> parents;
                if (check(TK::QWORDS)) {
                    std::istringstream iss(cur().text); advance();
                    std::string w;
                    while (iss >> w) parents.push_back(w);
                } else if (check(TK::STRING)) {
                    parents.push_back(cur().text); advance();
                }
                match(TK::SEMI);
                for (auto &p : parents) {
                    auto n = std::make_unique<Node>();
                    n->kind = NK::SetIsa;
                    n->name = currentPackage_; /* child */
                    n->sval = p;               /* parent */
                    n->line = line;
                    stmts.push_back(std::move(n));
                }
                continue;
            }
            /* skip all other use statements */
            while (!check(TK::SEMI) && !check(TK::EOF_TOK)) advance();
            match(TK::SEMI); continue;
        }
        stmts.push_back(parseStmt());
    }
    return makeBlock(std::move(stmts), 1);
}

/* ── statements ──────────────────────────────────────────────────────────── */

NodePtr Parser::parseBlock() {
    int line = cur().line;
    consume(TK::LBRACE, "{");
    NodeList stmts;
    while (!check(TK::RBRACE) && !check(TK::EOF_TOK))
        stmts.push_back(parseStmt());
    consume(TK::RBRACE, "}");
    return makeBlock(std::move(stmts), line);
}

NodePtr Parser::parseStmt() {
    int line = cur().line;

    /* package Foo; — changes current package context for sub naming */
    if (check(TK::KW_PACKAGE)) {
        advance();
        currentPackage_ = cur().text; advance();
        match(TK::SEMI);
        auto n = std::make_unique<Node>(); n->kind = NK::PackageStmt;
        n->sval = currentPackage_; n->line = line;
        return n;
    }

    if (check(TK::KW_BEGIN)) {
        advance(); /* consume 'BEGIN' */
        auto body = parseBlock();
        auto n = std::make_unique<Node>(); n->kind = NK::BeginBlock; n->line = line;
        n->body = std::move(body);
        return n;
    }
    if (check(TK::KW_END)) {
        advance(); /* consume 'END' */
        auto body = parseBlock();
        auto n = std::make_unique<Node>(); n->kind = NK::EndBlock; n->line = line;
        n->body = std::move(body);
        return n;
    }
    if (check(TK::KW_MY) || check(TK::KW_OUR)) return parseMy();
    if (check(TK::KW_STATE)) {
        advance(); /* consume 'state' */
        consume(TK::SCALAR, "$");
        std::string varName = cur().text; advance();
        auto n = std::make_unique<Node>(); n->kind = NK::StateDecl;
        n->name = varName; n->line = line;
        if (match(TK::ASSIGN))
            n->left = parseExpr();
        return parseModifier(std::move(n), line);
    }
    if (check(TK::KW_LOCAL)) {
        advance(); /* consume 'local' */
        consume(TK::SCALAR, "$");
        std::string varName;
        if (check(TK::SLASH)) { advance(); varName = "/"; }    /* local $/ */
        else if (check(TK::NOT)) { advance(); varName = "!"; } /* local $! */
        else { varName = cur().text; advance(); }
        auto n = std::make_unique<Node>(); n->kind = NK::LocalStmt;
        n->name = varName; n->line = line;
        if (match(TK::ASSIGN))
            n->left = parseExpr();
        return parseModifier(std::move(n), line);
    }
    if (check(TK::KW_IF))      return parseIf();
    if (check(TK::KW_UNLESS)) {
        /* desugar: unless(C) B  →  if(!C) B */
        advance();
        consume(TK::LPAREN, "(");
        auto cond = makeUnary("!", parseExpr(), line);
        consume(TK::RPAREN, ")");
        auto body = parseBlock();
        auto n = std::make_unique<Node>(); n->kind = NK::If; n->line = line;
        n->branches.push_back({std::move(cond), std::move(body)});
        return n;
    }
    if (check(TK::KW_WHILE))   return parseWhile();
    if (check(TK::KW_UNTIL)) {
        advance();
        consume(TK::LPAREN, "(");
        auto cond = makeUnary("!", parseExpr(), line);
        consume(TK::RPAREN, ")");
        auto body = parseBlock();
        auto n = std::make_unique<Node>(); n->kind = NK::While; n->line = line;
        n->cond = std::move(cond); n->body = std::move(body);
        return n;
    }
    if (check(TK::KW_FOR))     return parseFor();
    if (check(TK::KW_FOREACH)) return parseForeach();
    if (check(TK::KW_DO)) {
        advance();
        auto blk = parseBlock();
        /* do { } while/until (cond) — post-condition loop */
        if (check(TK::KW_WHILE) || check(TK::KW_UNTIL)) {
            bool negate = check(TK::KW_UNTIL);
            advance();
            consume(TK::LPAREN, "(");
            auto cond = parseExpr();
            consume(TK::RPAREN, ")");
            match(TK::SEMI);
            if (negate) cond = makeUnary("!", std::move(cond), line);
            auto n = std::make_unique<Node>(); n->kind = NK::DoWhile; n->line = line;
            n->body = std::move(blk); n->cond = std::move(cond);
            return n;
        }
        match(TK::SEMI);
        return blk;
    }
    if (check(TK::KW_SUB))    return parseSub();
    if (check(TK::KW_PRINT))  { return parseModifier(parsePrint(false), line); }
    if (check(TK::KW_SAY))    { return parseModifier(parsePrint(true),  line); }
    if (check(TK::KW_PRINTF)) {
        advance();
        /* filehandle detection (same heuristic as parsePrint) */
        std::string fhname;
        if (check(TK::IDENT) && (cur().text == "STDOUT" || cur().text == "STDERR")) {
            fhname = cur().text; advance();
        } else if (check(TK::SCALAR) && peek(1).kind == TK::IDENT) {
            TK t2 = peek(2).kind;
            /* only treat $var as filehandle when the next token starts a new
             * expression — not when it's an operator or list separator */
            /* LBRACKET/LBRACE excluded: $arr[i] and $hash{k} are subscripts, not fh */
            bool isFhCtx = (t2 == TK::SCALAR || t2 == TK::ARRAY || t2 == TK::HASH ||
                            t2 == TK::STRING || t2 == TK::INT   || t2 == TK::FLOAT ||
                            t2 == TK::IDENT  || t2 == TK::LPAREN);
            if (isFhCtx) { pos_++; fhname = advance().text; }
        }
        bool hasParen = check(TK::LPAREN);
        if (hasParen) advance();
        NodePtr fmt = parseExpr();
        NodeList args;
        while (match(TK::COMMA)) {
            if (!hasParen && isModifier()) break;
            if (hasParen && check(TK::RPAREN)) break;
            if (check(TK::SEMI) || check(TK::EOF_TOK)) break;
            args.push_back(parseExpr());
        }
        if (hasParen) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::PrintfStmt; n->line = line;
        n->name = fhname; n->left = std::move(fmt); n->args = std::move(args);
        return parseModifier(std::move(n), line);
    }
    if (check(TK::KW_PUSH))   { return parseModifier(parsePush(),        line); }
    if (check(TK::KW_UNSHIFT)){ return parseModifier(parseUnshift(),     line); }
    if (check(TK::KW_RETURN)) { return parseModifier(parseReturn(),      line); }
    if (check(TK::KW_LAST)) {
        advance();
        auto n = std::make_unique<Node>(); n->kind = NK::Last; n->line = line;
        return parseModifier(std::move(n), line);
    }
    if (check(TK::KW_NEXT)) {
        advance();
        auto n = std::make_unique<Node>(); n->kind = NK::Next; n->line = line;
        return parseModifier(std::move(n), line);
    }
    if (check(TK::LBRACE))    return parseBlock();
    if (check(TK::KW_DIE) || check(TK::KW_WARN)) {
        bool isDie = check(TK::KW_DIE); advance();
        NodePtr msg;
        if (!check(TK::SEMI) && !isModifier() && !check(TK::EOF_TOK))
            msg = parseExpr();
        auto n = std::make_unique<Node>();
        n->kind = isDie ? NK::DieStmt : NK::WarnStmt; n->line = line;
        n->left = std::move(msg);
        return parseModifier(std::move(n), line);
    }

    /* expression statement */
    auto expr = parseExpr();
    auto n = std::make_unique<Node>(); n->kind = NK::ExprStmt; n->left = std::move(expr); n->line = line;
    return parseModifier(std::move(n), line);
}

NodePtr Parser::parseIf() {
    int line = cur().line;
    consume(TK::KW_IF);
    consume(TK::LPAREN, "(");
    auto cond = parseExpr();
    consume(TK::RPAREN, ")");
    auto body = parseBlock();

    auto n = std::make_unique<Node>(); n->kind = NK::If; n->line = line;
    n->branches.push_back({std::move(cond), std::move(body)});

    while (check(TK::KW_ELSIF)) {
        advance();
        consume(TK::LPAREN, "(");
        auto ec = parseExpr();
        consume(TK::RPAREN, ")");
        auto eb = parseBlock();
        n->branches.push_back({std::move(ec), std::move(eb)});
    }
    if (check(TK::KW_ELSE)) {
        advance();
        auto eb = parseBlock();
        n->branches.push_back({nullptr, std::move(eb)});
    }
    return n;
}

NodePtr Parser::parseWhile() {
    int line = cur().line;
    consume(TK::KW_WHILE);
    consume(TK::LPAREN, "(");
    auto cond = parseExpr();
    /* while (<FH>) without explicit assignment → while ($_ = <FH>) */
    if (cond->kind == NK::Readline) {
        auto assign = std::make_unique<Node>(); assign->kind = NK::Assign;
        assign->left  = makeScalar("_", line);
        assign->right = std::move(cond);
        assign->line  = line;
        cond = std::move(assign);
    }
    consume(TK::RPAREN, ")");
    auto body = parseBlock();
    auto n = std::make_unique<Node>(); n->kind = NK::While; n->line = line;
    n->cond = std::move(cond); n->body = std::move(body);
    return n;
}

NodePtr Parser::parseFor() {
    int line = cur().line;
    consume(TK::KW_FOR);

    /* peek: if next is '(' and body has ';', it's C-style */
    /* otherwise treat as foreach */
    if (check(TK::LPAREN)) {
        /* look ahead for ; inside parens to distinguish C-for from foreach */
        int depth = 0; size_t p = pos_;
        bool hasSemi = false;
        while (p < toks_.size()) {
            if (toks_[p].kind == TK::LPAREN) depth++;
            else if (toks_[p].kind == TK::RPAREN) { depth--; if (depth == 0) break; }
            else if (toks_[p].kind == TK::SEMI && depth == 1) { hasSemi = true; break; }
            p++;
        }
        if (hasSemi) {
            /* C-style: for (init; cond; step) { } */
            consume(TK::LPAREN, "(");
            NodePtr init;
            if (!check(TK::SEMI)) {
                if (check(TK::KW_MY) || check(TK::KW_OUR)) {
                    /* parse my $x = ... without consuming semi */
                    init = parseMy();
                    /* parseMy consumed the semi already — we need the second one */
                    /* Actually parseMy ends with SEMI consumed. Undo by re-inserting */
                    /* Simpler: parseMy eats ';'. For C-for, don't call parseMy. */
                    /* We handle this: parseMy below won't eat semi if we intercept */
                    /* Let's just parse as expr instead */
                } else {
                    init = parseExpr(); match(TK::SEMI);
                }
            } else match(TK::SEMI);

            NodePtr cond;
            if (!check(TK::SEMI)) { cond = parseExpr(); }
            match(TK::SEMI);

            NodePtr step;
            if (!check(TK::RPAREN)) { step = parseExpr(); }
            consume(TK::RPAREN, ")");
            auto body = parseBlock();

            auto n = std::make_unique<Node>(); n->kind = NK::For; n->line = line;
            n->init = std::move(init); n->cond = std::move(cond);
            n->step = std::move(step); n->body = std::move(body);
            return n;
        }
    }

    /* foreach form: for my $x (LIST) { } or for (LIST) { } */
    return parseForeachBody(line);
}

NodePtr Parser::parseForeach() {
    int line = cur().line;
    consume(TK::KW_FOREACH);
    return parseForeachBody(line);
}

NodePtr Parser::parseForeachBody(int line) {
    /* optional: my $var */
    std::string varName = "_";
    if (check(TK::KW_MY)) {
        advance();
        consume(TK::SCALAR, "$");
        varName = cur().text; advance();
    } else if (check(TK::SCALAR)) {
        advance(); /* skip $ */
        varName = cur().text; advance();
    }

    consume(TK::LPAREN, "(");
    NodeList elems;
    while (!check(TK::RPAREN) && !check(TK::EOF_TOK)) {
        elems.push_back(parseExpr());
        if (!match(TK::COMMA)) break;
    }
    consume(TK::RPAREN, ")");
    auto body = parseBlock();

    auto n = std::make_unique<Node>(); n->kind = NK::Foreach; n->name = varName; n->line = line;
    /* store list in args, body in body */
    n->args = std::move(elems); n->body = std::move(body);
    return n;
}

NodePtr Parser::parseSub() {
    int line = cur().line;
    consume(TK::KW_SUB);
    std::string name = cur().text; advance();
    /* qualify with current package when not already qualified and not in main */
    if (name.find("::") == std::string::npos && currentPackage_ != "main")
        name = currentPackage_ + "::" + name;
    /* forward declaration: sub name; or sub name(PROTOTYPE); */
    if (check(TK::SEMI)) { advance(); match(TK::RBRACE); auto n = std::make_unique<Node>(); n->kind = NK::SubDef; n->name = name; n->line = line; return n; }
    if (check(TK::LPAREN)) { advance(); while (!check(TK::RPAREN) && !check(TK::EOF_TOK)) advance(); consume(TK::RPAREN, ")"); if (check(TK::SEMI)) { advance(); match(TK::RBRACE); auto n = std::make_unique<Node>(); n->kind = NK::SubDef; n->name = name; n->line = line; return n; } }
    consume(TK::LBRACE, "{");

    ++subDepth_;
    NodeList stmts;
    while (!check(TK::RBRACE) && !check(TK::EOF_TOK))
        stmts.push_back(parseStmt());
    --subDepth_;
    consume(TK::RBRACE, "}");

    auto n = std::make_unique<Node>(); n->kind = NK::SubDef; n->name = name; n->line = line;
    n->body = makeBlock(std::move(stmts), line);
    return n;
}

NodePtr Parser::parseMy() {
    int line = cur().line;
    bool isOur = check(TK::KW_OUR);
    advance(); /* consume my/our */

    /* my ($a, $b) = ...  or  my (%h) = ...  or  my (@arr) = ... */
    if (check(TK::LPAREN)) {
        advance();

        /* my (%h) = rhs — single hash captures the whole list */
        if (check(TK::HASH)) {
            advance();
            std::string nm = cur().text; advance();
            consume(TK::RPAREN, ")");
            NodePtr rhs;
            if (match(TK::ASSIGN)) rhs = parseExpr();
            match(TK::SEMI);
            auto decl = std::make_unique<Node>(); decl->kind = NK::My;
            decl->name = "%" + nm; decl->line = line;
            if (rhs) decl->right = std::move(rhs);
            return decl;
        }

        /* my (@a, @b, ..., $x, ...) — mixed arrays/hashes/scalars */
        /* collect all variable declarations with their sigil types */
        struct VarDecl {
            std::string sigil;  /* "@", "%", "$" */
            std::string name;
        };
        std::vector<VarDecl> allVars;

        if (check(TK::ARRAY)) {
            advance();
            allVars.push_back({"@", cur().text}); advance();
        }
        while (!check(TK::RPAREN) && !check(TK::EOF_TOK)) {
            if (check(TK::ARRAY)) { advance(); allVars.push_back({"@", cur().text}); advance(); }
            else if (check(TK::HASH)) { advance(); allVars.push_back({"%", cur().text}); advance(); }
            else if (check(TK::SCALAR)) { advance(); allVars.push_back({"$", cur().text}); advance(); }
            else { advance(); continue; }
            if (!match(TK::COMMA)) break;
        }
        consume(TK::RPAREN, ")");
        NodePtr rhs;
        if (match(TK::ASSIGN)) rhs = parseExpr();
        match(TK::SEMI);
        /* emit as FlatBlock with multiple decls */
        NodeList stmts;
        for (auto &vd : allVars) {
            auto decl = std::make_unique<Node>(); decl->kind = NK::My;
            decl->name = vd.sigil + vd.name; decl->line = line;
            stmts.push_back(std::move(decl));
        }
        if (rhs) {
            NodeList lhsList;
            for (auto &vd : allVars) {
                /* strip sigil for LHS — same as old code using makeScalar */
                lhsList.push_back(makeScalar(vd.name, line));
            }
            auto lhsArr = std::make_unique<Node>(); lhsArr->kind = NK::ArrayLit;
            lhsArr->args = std::move(lhsList); lhsArr->line = line;
            auto asgn = std::make_unique<Node>(); asgn->kind = NK::Assign;
            asgn->left = std::move(lhsArr); asgn->right = std::move(rhs); asgn->line = line;
            auto es = std::make_unique<Node>(); es->kind = NK::ExprStmt;
            es->left = std::move(asgn); es->line = line;
            stmts.push_back(std::move(es));
        }
        auto fb = std::make_unique<Node>(); fb->kind = NK::FlatBlock;
        fb->args = std::move(stmts); fb->line = line;
        return fb;
    }

    /* my $scalar [= expr] */
    if (check(TK::SCALAR)) {
        advance(); /* skip $ */
        std::string nm = cur().text; advance();
        auto decl = std::make_unique<Node>(); decl->kind = NK::My;
        decl->name = nm; decl->line = line;
        if (match(TK::ASSIGN)) {
            decl->right = parseExpr();
        }
        match(TK::SEMI);
        return decl;
    }

    /* my @arr */
    if (check(TK::ARRAY)) {
        advance(); /* skip @ */
        std::string nm = cur().text; advance();
        auto decl = std::make_unique<Node>(); decl->kind = NK::My;
        decl->name = "@" + nm; decl->line = line;
        if (match(TK::ASSIGN)) {
            decl->right = parseExpr();
        }
        match(TK::SEMI);
        return decl;
    }

    /* my %hash */
    if (check(TK::HASH)) {
        advance(); /* skip % */
        std::string nm = cur().text; advance();
        auto decl = std::make_unique<Node>(); decl->kind = NK::My;
        decl->name = "%" + nm; decl->line = line;
        if (match(TK::ASSIGN)) {
            decl->right = parseExpr();
        }
        match(TK::SEMI);
        return decl;
    }

    /* fallback: expression statement */
    auto expr = parseExpr();
    match(TK::SEMI);
    auto n = std::make_unique<Node>(); n->kind = NK::ExprStmt; n->left = std::move(expr); n->line = line;
    return n;
}

NodePtr Parser::parsePrint(bool isSay) {
    int line = cur().line;
    advance(); /* consume print/say */

    /* filehandle detection:
       - bare STDOUT/STDERR ident
       - $fh without a following comma → it's a filehandle, not a value */
    std::string fhname;
    if (check(TK::IDENT) && (cur().text == "STDOUT" || cur().text == "STDERR")) {
        fhname = cur().text; advance();
    } else if (check(TK::SCALAR) && peek(1).kind == TK::IDENT) {
        TK t2 = peek(2).kind;
        /* LBRACKET/LBRACE excluded: $arr[i] and $hash{k} are subscripts, not fh */
        bool isFhCtx = (t2 == TK::SCALAR || t2 == TK::ARRAY || t2 == TK::HASH ||
                        t2 == TK::STRING || t2 == TK::INT   || t2 == TK::FLOAT ||
                        t2 == TK::IDENT  || t2 == TK::LPAREN);
        if (isFhCtx) {
            pos_++;  /* skip $ */
            fhname = advance().text;  /* variable name */
        }
    }

    NodeList args;
    bool hasParen = match(TK::LPAREN);
    while (!check(TK::SEMI) && !check(TK::EOF_TOK) && !isModifier()) {
        if (hasParen && check(TK::RPAREN)) break;
        args.push_back(parseExpr());
        if (!match(TK::COMMA)) break;
    }
    if (hasParen) match(TK::RPAREN);

    auto n = std::make_unique<Node>();
    n->kind = isSay ? NK::SayStmt : NK::PrintStmt;
    n->name = fhname;
    n->args = std::move(args); n->line = line;
    return n;
}

NodePtr Parser::parsePush() {
    int line = cur().line;
    consume(TK::KW_PUSH);
    bool hasParen = match(TK::LPAREN);
    consume(TK::ARRAY, "@");
    NodeList vals;
    NodePtr refExpr;
    std::string arrName;
    /* push @{EXPR}, val  or  push @$ref, val  or  push @arr, val */
    if (check(TK::LBRACE)) {
        advance(); /* skip { */
        refExpr = parseExpr();
        consume(TK::RBRACE, "}");
    } else if (check(TK::SCALAR)) {
        advance(); /* skip $ */
        std::string nm = cur().text; advance();
        refExpr = makeScalar(nm, line);
    } else {
        arrName = cur().text; advance();
    }
    match(TK::COMMA);
    while (!check(TK::SEMI) && !check(TK::EOF_TOK) && !isModifier()) {
        if (hasParen && check(TK::RPAREN)) break;
        vals.push_back(parseExpr());
        if (!match(TK::COMMA)) break;
    }
    if (hasParen) match(TK::RPAREN);
    auto n = std::make_unique<Node>(); n->kind = NK::PushStmt;
    n->name = arrName; n->args = std::move(vals); n->line = line;
    if (refExpr) n->left = std::move(refExpr); /* @$ref or @{EXPR} form */
    return n;
}

NodePtr Parser::parseUnshift() {
    int line = cur().line;
    consume(TK::KW_UNSHIFT);
    bool hasParen = match(TK::LPAREN);
    consume(TK::ARRAY, "@");
    NodePtr refExpr;
    std::string arrName;
    if (check(TK::SCALAR)) {
        advance();
        std::string nm = cur().text; advance();
        refExpr = makeScalar(nm, line);
    } else {
        arrName = cur().text; advance();
    }
    match(TK::COMMA);
    NodeList vals;
    while (!check(TK::SEMI) && !check(TK::EOF_TOK) && !isModifier()) {
        if (hasParen && check(TK::RPAREN)) break;
        vals.push_back(parseExpr());
        if (!match(TK::COMMA)) break;
    }
    if (hasParen) match(TK::RPAREN);
    auto n = std::make_unique<Node>(); n->kind = NK::UnshiftStmt2;
    n->name = arrName; n->args = std::move(vals); n->line = line;
    if (refExpr) n->left = std::move(refExpr);
    return n;
}

NodePtr Parser::parseReturn() {
    int line = cur().line;
    consume(TK::KW_RETURN);
    NodePtr val;
    if (!check(TK::SEMI) && !isModifier()) val = parseExpr();
    auto n = std::make_unique<Node>(); n->kind = NK::Return; n->line = line;
    n->left = std::move(val);
    return n;
}

/* ── statement modifiers ─────────────────────────────────────────────────── */

bool Parser::isModifier() const {
    TK k = toks_[pos_].kind;
    return k == TK::KW_IF || k == TK::KW_UNLESS ||
           k == TK::KW_WHILE || k == TK::KW_UNTIL ||
           k == TK::KW_FOR   || k == TK::KW_FOREACH;
}

/* Wrap stmt in an if/while/foreach node if a modifier keyword follows.
   Always consumes the trailing semicolon. */
NodePtr Parser::parseModifier(NodePtr stmt, int line) {
    if (check(TK::KW_IF) || check(TK::KW_UNLESS)) {
        bool neg = check(TK::KW_UNLESS); advance();
        bool hasParen = match(TK::LPAREN);
        auto cond = parseExpr();
        if (hasParen) consume(TK::RPAREN, ")");
        if (neg) cond = makeUnary("!", std::move(cond), line);
        NodeList body; body.push_back(std::move(stmt));
        auto n = std::make_unique<Node>(); n->kind = NK::If; n->line = line;
        n->branches.push_back({std::move(cond), makeBlock(std::move(body), line)});
        match(TK::SEMI);
        return n;
    }
    if (check(TK::KW_WHILE) || check(TK::KW_UNTIL)) {
        bool neg = check(TK::KW_UNTIL); advance();
        bool hasParen = match(TK::LPAREN);
        auto cond = parseExpr();
        if (hasParen) consume(TK::RPAREN, ")");
        if (neg) cond = makeUnary("!", std::move(cond), line);
        NodeList body; body.push_back(std::move(stmt));
        auto n = std::make_unique<Node>(); n->kind = NK::While; n->line = line;
        n->cond = std::move(cond); n->body = makeBlock(std::move(body), line);
        match(TK::SEMI);
        return n;
    }
    if (check(TK::KW_FOR) || check(TK::KW_FOREACH)) {
        advance();
        bool hasParen = match(TK::LPAREN);
        NodeList elems;
        while (!check(TK::SEMI) && !check(TK::EOF_TOK)) {
            if (hasParen && check(TK::RPAREN)) break;
            elems.push_back(parseExpr());
            if (!match(TK::COMMA)) break;
        }
        if (hasParen) match(TK::RPAREN);
        NodeList body; body.push_back(std::move(stmt));
        auto n = std::make_unique<Node>(); n->kind = NK::Foreach; n->line = line;
        n->name = "_"; n->args = std::move(elems);
        n->body = makeBlock(std::move(body), line);
        match(TK::SEMI);
        return n;
    }
    match(TK::SEMI);
    return stmt;
}

/* ── expressions ─────────────────────────────────────────────────────────── */

NodePtr Parser::parseExpr()    { return parseAssign(); }

NodePtr Parser::parseAssign() {
    auto lhs = parseTernary();
    int line = cur().line;
    if (check(TK::ASSIGN)) {
        advance();
        auto rhs = parseAssign();
        auto n = std::make_unique<Node>(); n->kind = NK::Assign;
        n->left = std::move(lhs); n->right = std::move(rhs); n->line = line;
        return n;
    }
    /* compound assignment */
    TK k = cur().kind;
    if (k == TK::PLUS_ASSIGN || k == TK::MINUS_ASSIGN ||
        k == TK::STAR_ASSIGN || k == TK::SLASH_ASSIGN || k == TK::DOT_ASSIGN) {
        std::string op; advance();
        if (k == TK::PLUS_ASSIGN)  op = "+";
        else if (k == TK::MINUS_ASSIGN) op = "-";
        else if (k == TK::STAR_ASSIGN)  op = "*";
        else if (k == TK::SLASH_ASSIGN) op = "/";
        else op = ".";
        auto rhs = parseAssign();
        /* desugar lhs op= rhs → lhs = lhs op rhs */
        /* need a clone of lhs — since we already moved it, build assign directly */
        auto n = std::make_unique<Node>(); n->kind = NK::CompoundAssign;
        n->sval = op; n->left = std::move(lhs); n->right = std::move(rhs); n->line = line;
        return n;
    }
    return lhs;
}

NodePtr Parser::parseRange() {
    auto lhs = parseOr();
    if (!check(TK::DOTDOT)) return lhs;
    int line = cur().line;
    advance();
    auto rhs = parseOr();
    auto n = std::make_unique<Node>();
    n->kind = NK::Range; n->line = line;
    n->left = std::move(lhs); n->right = std::move(rhs);
    return n;
}

NodePtr Parser::parseTernary() {
    auto cond = parseRange();
    if (!match(TK::QUESTION)) return cond;
    int line = cur().line;
    auto then = parseExpr();
    consume(TK::COLON, ":");
    auto els = parseExpr();
    /* represent as if-expression; use BinOp "?:" */
    auto n = std::make_unique<Node>(); n->kind = NK::BinOp; n->sval = "?:"; n->line = line;
    n->cond = std::move(cond); n->left = std::move(then); n->right = std::move(els);
    return n;
}

NodePtr Parser::parseOr() {
    auto lhs = parseAnd();
    while (check(TK::OR2) || check(TK::KW_OR) || check(TK::DEFINED_OR)) {
        int line = cur().line;
        std::string op = check(TK::DEFINED_OR) ? "//" : "||";
        advance();
        auto rhs = parseAnd();
        lhs = makeBin(op, std::move(lhs), std::move(rhs), line);
    }
    return lhs;
}

NodePtr Parser::parseAnd() {
    auto lhs = parseNot();
    while (check(TK::AND2) || check(TK::KW_AND)) {
        int line = cur().line; advance();
        auto rhs = parseNot();
        lhs = makeBin("&&", std::move(lhs), std::move(rhs), line);
    }
    return lhs;
}

NodePtr Parser::parseNot() {
    if (check(TK::NOT) || check(TK::KW_NOT)) {
        int line = cur().line; advance();
        return makeUnary("!", parseNot(), line);
    }
    return parseCmp();
}

NodePtr Parser::parseBinding() {
    auto lhs = parseAdd();
    while (check(TK::BIND) || check(TK::NBIND)) {
        bool negated = check(TK::NBIND);
        int line = cur().line; advance();
        if (check(TK::SUBST)) {
            if (negated) throw std::runtime_error("!~ s/// doesn't make sense");
            std::string txt = cur().text; advance();
            size_t s1 = txt.find('\x01'), s2 = txt.find('\x01', s1 + 1);
            auto n = std::make_unique<Node>(); n->kind = NK::RegexSubst; n->line = line;
            n->left  = std::move(lhs);
            n->sval  = txt.substr(0, s1);                       /* pattern */
            n->name  = txt.substr(s1 + 1, s2 - s1 - 1)         /* replacement */
                     + "\x01" + txt.substr(s2 + 1);             /* flags */
            lhs = std::move(n);
        } else if (check(TK::REGEX)) {
            std::string txt = cur().text; advance();
            size_t sep = txt.find('\x01');
            auto n = std::make_unique<Node>(); n->kind = NK::RegexMatch; n->line = line;
            n->left = std::move(lhs);
            n->sval = txt.substr(0, sep);
            n->name = (sep != std::string::npos) ? txt.substr(sep + 1) : "";
            n->ival = negated ? 1 : 0;
            lhs = std::move(n);
        } else if (check(TK::TR)) {
            if (negated) throw std::runtime_error("!~ tr/// doesn't make sense");
            std::string txt = cur().text; advance();
            auto n = std::make_unique<Node>(); n->kind = NK::TrOp; n->line = line;
            n->left = std::move(lhs);
            n->sval = txt; /* search\x01replace\x01flags */
            lhs = std::move(n);
        } else {
            throw std::runtime_error("Expected /regex/ or s/// or tr/// after =~");
        }
    }
    return lhs;
}

NodePtr Parser::parseCmp() {
    auto lhs = parseBinding();
    int line = cur().line;
    TK k = cur().kind;
    std::string ident = (k == TK::IDENT) ? cur().text : "";

    auto isCmpOp = [&]() {
        switch (k) {
            case TK::EQ: case TK::NE: case TK::LT: case TK::GT: case TK::LE: case TK::GE:
            case TK::SPACESHIP:
                return true;
            case TK::IDENT:
                return ident == "eq" || ident == "ne" || ident == "lt" ||
                       ident == "gt" || ident == "le" || ident == "ge" ||
                       ident == "cmp";
            default: return false;
        }
    };

    while (isCmpOp()) {
        std::string op;
        if (k == TK::IDENT) op = ident;
        else {
            switch (k) {
                case TK::EQ:       op = "==";  break; case TK::NE:  op = "!="; break;
                case TK::LT:       op = "<";   break; case TK::GT:  op = ">";  break;
                case TK::LE:       op = "<=";  break; case TK::GE:  op = ">="; break;
                case TK::SPACESHIP:op = "<=>";  break;
                default: break;
            }
        }
        advance();
        auto rhs = parseBinding();
        lhs = makeBin(op, std::move(lhs), std::move(rhs), line);
        line = cur().line; k = cur().kind;
        ident = (k == TK::IDENT) ? cur().text : "";
    }
    return lhs;
}

NodePtr Parser::parseAdd() {
    auto lhs = parseMul();
    while (check(TK::PLUS) || check(TK::MINUS) || check(TK::DOT)) {
        int line = cur().line;
        std::string op(1, check(TK::DOT) ? '.' : (check(TK::PLUS) ? '+' : '-'));
        advance();
        auto rhs = parseMul();
        lhs = makeBin(op, std::move(lhs), std::move(rhs), line);
    }
    return lhs;
}

NodePtr Parser::parseMul() {
    auto lhs = parseUnary();
    while (check(TK::STAR) || check(TK::SLASH) || check(TK::PERCENT)) {
        int line = cur().line;
        std::string op; op += (check(TK::STAR) ? '*' : (check(TK::SLASH) ? '/' : '%'));
        advance();
        auto rhs = parseUnary();
        lhs = makeBin(op, std::move(lhs), std::move(rhs), line);
    }
    return lhs;
}

NodePtr Parser::parseUnary() {
    int line = cur().line;
    if (check(TK::MINUS)) { advance(); return makeUnary("-", parsePow(), line); }
    if (check(TK::NOT))   { advance(); return makeUnary("!", parsePow(), line); }
    if (check(TK::PLUS_PLUS)) {
        advance(); return makeUnary("pre++", parsePostfix(), line);
    }
    if (check(TK::MINUS_MINUS)) {
        advance(); return makeUnary("pre--", parsePostfix(), line);
    }
    return parsePow();
}

NodePtr Parser::parsePow() {
    auto lhs = parsePostfix();
    if (!check(TK::STAR_STAR)) return lhs;
    int line = cur().line;
    advance();
    auto rhs = parsePow(); /* right-associative */
    return makeBin("**", std::move(lhs), std::move(rhs), line);
}

NodePtr Parser::parseSubscript(NodePtr base, int line) {
    /* consume -> then [ or { */
    /* also handles adjacent [] {} (no -> needed after first subscript) */
    for (;;) {
        if (check(TK::ARROW)) {
            advance();
            if (check(TK::LBRACKET)) {
                advance();
                auto idx = parseExpr();
                consume(TK::RBRACKET, "]");
                auto n = std::make_unique<Node>(); n->kind = NK::ArrowDeref;
                n->sval = "array"; n->left = std::move(base);
                n->right = std::move(idx); n->line = line;
                base = std::move(n);
                continue;
            }
            if (check(TK::LBRACE)) {
                advance();
                auto key = parseExpr();
                consume(TK::RBRACE, "}");
                auto n = std::make_unique<Node>(); n->kind = NK::ArrowDeref;
                n->sval = "hash"; n->left = std::move(base);
                n->right = std::move(key); n->line = line;
                base = std::move(n);
                continue;
            }
            if (check(TK::LPAREN)) {
                /* $code_ref->(args) */
                advance();
                auto n = std::make_unique<Node>(); n->kind = NK::CallCodeRef;
                n->left = std::move(base); n->line = line;
                while (!check(TK::RPAREN) && !check(TK::EOF_TOK)) {
                    n->args.push_back(parseExpr());
                    if (!match(TK::COMMA) && !match(TK::FATARROW)) break;
                }
                consume(TK::RPAREN, ")");
                base = std::move(n);
                continue;
            }
            /* ->method(args), ->SUPER::method(args) — method call */
            if (check(TK::IDENT)) {
                std::string method = cur().text; advance();
                auto n = std::make_unique<Node>(); n->kind = NK::MethodCall;
                n->sval = method; n->left = std::move(base); n->line = line;
                /* store caller package for SUPER dispatch */
                if (method.substr(0, 7) == "SUPER::")
                    n->name = currentPackage_;
                if (check(TK::LPAREN)) {
                    advance();
                    while (!check(TK::RPAREN) && !check(TK::EOF_TOK)) {
                        n->args.push_back(parseExpr());
                        if (!match(TK::COMMA) && !match(TK::FATARROW)) break;
                    }
                    consume(TK::RPAREN, ")");
                }
                base = std::move(n);
                continue;
            }
            /* -> not followed by [ or { or ( or IDENT: stop */
            break;
        }
        /* adjacent subscripts after any subscriptable kind — implies implicit -> */
        auto isSubscriptableK = [](NK k) {
            return k == NK::ArrowDeref || k == NK::ArrayElem || k == NK::HashElem ||
                   k == NK::MethodCall || k == NK::CallCodeRef || k == NK::DerefScalar;
        };
        if (isSubscriptableK(base->kind) && check(TK::LBRACKET)) {
            advance();
            auto idx = parseExpr();
            consume(TK::RBRACKET, "]");
            auto n = std::make_unique<Node>(); n->kind = NK::ArrowDeref;
            n->sval = "array"; n->left = std::move(base);
            n->right = std::move(idx); n->line = line;
            base = std::move(n);
            continue;
        }
        if (isSubscriptableK(base->kind) && check(TK::LBRACE)) {
            advance();
            auto key = parseExpr();
            consume(TK::RBRACE, "}");
            auto n = std::make_unique<Node>(); n->kind = NK::ArrowDeref;
            n->sval = "hash"; n->left = std::move(base);
            n->right = std::move(key); n->line = line;
            base = std::move(n);
            continue;
        }
        break;
    }
    return base;
}

NodePtr Parser::parsePostfix() {
    auto expr = parsePrimary();
    int ln = expr->line;
    /* enter subscript chain for explicit -> or adjacent [ / { after a subscript kind */
    auto isSubscriptable = [](NK k) {
        return k == NK::ArrowDeref || k == NK::ArrayElem || k == NK::HashElem ||
               k == NK::MethodCall || k == NK::CallCodeRef || k == NK::DerefScalar;
    };
    if (check(TK::ARROW) ||
        (isSubscriptable(expr->kind) && (check(TK::LBRACKET) || check(TK::LBRACE)))) {
        expr = parseSubscript(std::move(expr), ln);
    }
    /* post++ / post-- */
    while (check(TK::PLUS_PLUS) || check(TK::MINUS_MINUS)) {
        int line = cur().line;
        std::string op = check(TK::PLUS_PLUS) ? "post++" : "post--";
        advance();
        expr = makeUnary(op, std::move(expr), line);
    }
    return expr;
}

NodePtr Parser::parsePrimary() {
    int line = cur().line;

    /* undef */
    if (check(TK::KW_UNDEF)) { advance(); auto n = std::make_unique<Node>(); n->kind = NK::UndefLit; n->line = line; return n; }

    if (check(TK::KW_WANTARRAY)) {
        advance();
        if (match(TK::LPAREN)) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::WantarrayFunc; n->line = line;
        return n;
    }
    if (check(TK::KW_CALLER)) {
        advance();
        if (match(TK::LPAREN)) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::CallerFunc; n->line = line;
        return n;
    }

    /* integer literal */
    if (check(TK::INT)) {
        long long v = std::stoll(cur().text, nullptr, 0);
        advance(); return makeInt(v, line);
    }
    /* float literal */
    if (check(TK::FLOAT)) {
        double v = std::stod(cur().text);
        advance(); return makeFloat(v, line);
    }
    /* string literal */
    if (check(TK::STRING)) {
        std::string raw = cur().text; advance();
        bool isDQ = !raw.empty() && raw[0] == '\x01';
        if (isDQ) {
            raw = raw.substr(1);
            return parseStringInterp(raw, line);
        }
        return makeStr(raw, line);
    }

    /* qw(word list) — returns ArrayLit of string literals */
    if (check(TK::QWORDS)) {
        std::string text = cur().text; advance();
        auto n = std::make_unique<Node>(); n->kind = NK::ArrayLit; n->line = line;
        std::istringstream iss(text);
        std::string word;
        while (iss >> word) n->args.push_back(makeStr(word, line));
        return n;
    }

    /* backtick command `cmd` */
    if (check(TK::BACKTICK)) {
        std::string raw = cur().text; advance();
        bool isDQ = !raw.empty() && raw[0] == '\x01';
        NodePtr cmdExpr;
        if (isDQ) { raw = raw.substr(1); cmdExpr = parseStringInterp(raw, line); }
        else       cmdExpr = makeStr(raw, line);
        auto n = std::make_unique<Node>(); n->kind = NK::BacktickExpr;
        n->left = std::move(cmdExpr); n->line = line;
        return n;
    }

    /* file test: -e $file, -f $file, etc. */
    if (check(TK::FILETEST)) {
        std::string flag = cur().text; advance();
        bool hp = match(TK::LPAREN);
        NodePtr path;
        if (!check(TK::SEMI) && !check(TK::EOF_TOK))
            path = hp ? parseExpr() : parsePostfix(); /* postfix-level: grabs $var, "str", $arr[i] */
        if (hp) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::FileTestOp;
        n->sval = flag; n->left = std::move(path); n->line = line;
        return n;
    }

    /* backslash: reference-taking  \$x  \@arr  \%h  \&sub */
    if (check(TK::BACKSLASH)) {
        advance();
        if (check(TK::SCALAR)) {
            advance();
            std::string nm = cur().text; advance();
            auto n = std::make_unique<Node>(); n->kind = NK::RefScalar;
            n->left = makeScalar(nm, line); n->line = line;
            return n;
        }
        if (check(TK::ARRAY)) {
            advance();
            std::string nm = cur().text; advance();
            auto n = std::make_unique<Node>(); n->kind = NK::RefArray;
            n->name = nm; n->line = line;
            return n;
        }
        if (check(TK::HASH)) {
            advance();
            std::string nm = cur().text; advance();
            auto n = std::make_unique<Node>(); n->kind = NK::RefHash;
            n->name = nm; n->line = line;
            return n;
        }
        if (check(TK::AND)) {
            advance();
            std::string nm = cur().text; advance();
            auto n = std::make_unique<Node>(); n->kind = NK::RefSub;
            n->name = nm; n->line = line;
            return n;
        }
        /* \(expr) — ref to expr value, treat as RefScalar of expr */
        auto inner = parsePrimary();
        auto n = std::make_unique<Node>(); n->kind = NK::RefScalar;
        n->left = std::move(inner); n->line = line;
        return n;
    }

    /* eval { BLOCK } or eval EXPR (string eval) */
    if (check(TK::KW_EVAL)) {
        advance();
        if (check(TK::LBRACE)) {
            consume(TK::LBRACE, "{");
            NodeList stmts;
            while (!check(TK::RBRACE) && !check(TK::EOF_TOK))
                stmts.push_back(parseStmt());
            consume(TK::RBRACE, "}");
            auto n = std::make_unique<Node>(); n->kind = NK::EvalBlock; n->line = line;
            n->body = makeBlock(std::move(stmts), line);
            return n;
        }
        /* eval EXPR — string eval (runtime compilation) */
        NodePtr expr = parseExpr();
        auto n = std::make_unique<Node>(); n->kind = NK::Call;
        n->name = "eval"; n->args.push_back(std::move(expr)); n->line = line;
        return n;
    }

    /* anonymous sub: sub { BLOCK } */
    if (check(TK::KW_SUB) && pos_ + 1 < toks_.size() && toks_[pos_+1].kind == TK::LBRACE) {
        advance(); /* consume 'sub' */
        consume(TK::LBRACE, "{");
        ++subDepth_;
        NodeList stmts;
        while (!check(TK::RBRACE) && !check(TK::EOF_TOK))
            stmts.push_back(parseStmt());
        --subDepth_;
        consume(TK::RBRACE, "}");
        auto n = std::make_unique<Node>(); n->kind = NK::AnonSub; n->line = line;
        /* generate unique name for this anonymous sub */
        static int anonCount = 0;
        n->name = "__anon_" + std::to_string(++anonCount);
        n->body = makeBlock(std::move(stmts), line);
        return n;
    }

    /* anonymous array ref:  [list] */
    if (check(TK::LBRACKET)) {
        advance();
        NodeList elems;
        while (!check(TK::RBRACKET) && !check(TK::EOF_TOK)) {
            elems.push_back(parseExpr());
            if (!match(TK::COMMA) && !match(TK::FATARROW)) break;
        }
        consume(TK::RBRACKET, "]");
        auto n = std::make_unique<Node>(); n->kind = NK::AnonArray;
        n->args = std::move(elems); n->line = line;
        return n;
    }

    /* scalar variable: $name  or  $$ref (scalar deref) */
    if (check(TK::SCALAR)) {
        advance(); /* skip $ */
        /* $@ — eval error variable */
        if (check(TK::ARRAY) && cur().text == "@") {
            advance();
            auto n = std::make_unique<Node>(); n->kind = NK::DollarAt; n->line = line;
            return n;
        }
        /* $! — errno string */
        if (check(TK::NOT)) {
            advance();
            return makeScalar("!", line);
        }
        /* $/ — input record separator */
        if (check(TK::SLASH)) {
            advance();
            return makeScalar("/", line);
        }
        /* $1, $2, ... capture variables; $0 = program name */
        if (check(TK::INT)) {
            long long n = std::stoll(cur().text); advance();
            if (n == 0) {
                /* $0 = program name: treat as scalar variable "0" */
                return makeScalar("0", line);
            }
            auto node = std::make_unique<Node>(); node->kind = NK::CaptureVar;
            node->ival = n; node->line = line;
            return node;
        }
        /* $$ref — deref */
        if (check(TK::SCALAR)) {
            advance();
            std::string nm = cur().text; advance();
            auto n = std::make_unique<Node>(); n->kind = NK::DerefScalar;
            n->left = makeScalar(nm, line); n->line = line;
            return n;
        }
        std::string nm = cur().text; advance();
        auto sv = makeScalar(nm, line);
        if (nm == "+" && check(TK::LBRACE)) {
          advance();
          auto key = parseExpr();
          consume(TK::RBRACE, "}");
          auto n = std::make_unique<Node>();
          n->kind = NK::HashElem;
          n->name = "+";
          n->left = std::move(key);
          n->line = line;
          return std::move(n);
        }
        /* $arr[idx] */
        if (check(TK::LBRACKET)) {
            advance();
            auto idx = parseExpr();
            consume(TK::RBRACKET, "]");
            auto n = std::make_unique<Node>(); n->kind = NK::ArrayElem;
            n->name = nm; n->left = std::move(idx); n->line = line;
            return n;
        }
        /* $hash{key} */
        if (check(TK::LBRACE)) {
            advance();
            auto key = parseExpr();
            consume(TK::RBRACE, "}");
            auto n = std::make_unique<Node>(); n->kind = NK::HashElem;
            n->name = nm; n->left = std::move(key); n->line = line;
            return n;
        }
        return sv;
    }

    /* scalar(@arr) / scalar keys %h / scalar values %h / scalar EXPR */
    if (check(TK::KW_SCALAR)) {
        advance();
        /* scalar keys %h  or  scalar values %h — no parens required */
        if (check(TK::KW_KEYS) || check(TK::KW_VALUES)) {
            bool isKeys = check(TK::KW_KEYS); advance();
            bool hasParen = match(TK::LPAREN);
            consume(TK::HASH, "%");
            std::string nm = cur().text; advance();
            if (hasParen) consume(TK::RPAREN, ")");
            auto n = std::make_unique<Node>();
            n->kind = isKeys ? NK::KeysFunc : NK::ValuesFunc;
            n->name = nm; n->sval = "scalar"; n->line = line;
            return n;
        }
        /* scalar @arr or scalar(@arr) */
        if (check(TK::ARRAY) || (check(TK::LPAREN) && pos_ + 1 < toks_.size() && toks_[pos_+1].kind == TK::ARRAY)) {
            bool hasParen = match(TK::LPAREN);
            consume(TK::ARRAY, "@");
            std::string nm = cur().text; advance();
            if (hasParen) consume(TK::RPAREN, ")");
            auto n = std::make_unique<Node>(); n->kind = NK::ScalarFunc;
            n->name = nm; n->line = line;
            return n;
        }
        /* scalar EXPR — evaluate EXPR in scalar context; just parse it */
        auto inner = parsePrimary();
        if (inner) inner->sval = "scalar_ctx";
        return inner;
    }

    if (check(TK::ARRAY)) {
        advance(); /* skip @ */
        /* @{EXPR} — block deref */
        if (check(TK::LBRACE)) {
            advance();
            auto inner = parseExpr();
            consume(TK::RBRACE, "}");
            auto n = std::make_unique<Node>(); n->kind = NK::DerefArray;
            n->left = std::move(inner); n->line = line;
            return n;
        }
        /* @$ref — array deref */
        if (check(TK::SCALAR)) {
            advance();
            std::string nm = cur().text; advance();
            auto n = std::make_unique<Node>(); n->kind = NK::DerefArray;
            n->left = makeScalar(nm, line); n->line = line;
            return n;
        }
        std::string nm = cur().text; advance();
        /* @arr[0,1,2] — array slice */
        if (check(TK::LBRACKET)) {
            advance();
            auto n = std::make_unique<Node>(); n->kind = NK::ArraySlice; n->name = nm; n->line = line;
            while (!check(TK::RBRACKET) && !check(TK::EOF_TOK)) {
                n->args.push_back(parseExpr());
                if (!match(TK::COMMA)) break;
            }
            consume(TK::RBRACKET, "]");
            return n;
        }
        /* @hash{'a','b'} — hash slice */
        if (check(TK::LBRACE)) {
            advance();
            auto n = std::make_unique<Node>(); n->kind = NK::HashSlice; n->name = nm; n->line = line;
            while (!check(TK::RBRACE) && !check(TK::EOF_TOK)) {
                n->args.push_back(parseExpr());
                if (!match(TK::COMMA)) break;
            }
            consume(TK::RBRACE, "}");
            return n;
        }
        auto n = std::make_unique<Node>(); n->kind = NK::ArrayVar;
        n->name = nm; n->line = line;
        return n;
    }

    /* %hash variable  or  %$ref */
    if (check(TK::HASH)) {
        advance(); /* skip % */
        if (check(TK::SCALAR)) {
            advance();
            std::string nm = cur().text; advance();
            auto n = std::make_unique<Node>(); n->kind = NK::DerefHash;
            n->left = makeScalar(nm, line); n->line = line;
            return n;
        }
        std::string nm = cur().text; advance();
        auto n = std::make_unique<Node>(); n->kind = NK::HashVar;
        n->name = nm; n->line = line;
        return n;
    }

    /* bless($ref [, $class])  or  bless $ref, $class */
    if (check(TK::KW_BLESS)) {
        advance();
        bool hp = match(TK::LPAREN);
        NodePtr ref = parseExpr();
        NodePtr cls;
        if (match(TK::COMMA)) cls = parseExpr();
        if (hp) consume(TK::RPAREN, ")");
        if (!cls) cls = makeStr(currentPackage_, line);
        auto n = std::make_unique<Node>(); n->kind = NK::BlessFunc; n->line = line;
        n->left = std::move(ref); n->right = std::move(cls);
        return n;
    }

    /* ref($x) */
    if (check(TK::KW_REF)) {
        advance();
        bool hasParen = match(TK::LPAREN);
        auto inner = parseExpr();
        if (hasParen) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::RefFunc;
        n->left = std::move(inner); n->line = line;
        return n;
    }

    /* defined($x) */
    if (check(TK::KW_DEFINED)) {
        advance();
        consume(TK::LPAREN, "(");
        auto inner = parseExpr();
        consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::DefinedFunc;
        n->left = std::move(inner); n->line = line;
        return n;
    }

    /* exists $h{key} */
    if (check(TK::KW_EXISTS)) {
        advance();
        consume(TK::SCALAR, "$");
        std::string nm = cur().text; advance();
        consume(TK::LBRACE, "{");
        auto key = parseExpr();
        consume(TK::RBRACE, "}");
        auto n = std::make_unique<Node>(); n->kind = NK::ExistsFunc;
        n->name = nm; n->left = std::move(key); n->line = line;
        return n;
    }

    /* delete $h{key} */
    if (check(TK::KW_DELETE)) {
        advance();
        consume(TK::SCALAR, "$");
        std::string nm = cur().text; advance();
        consume(TK::LBRACE, "{");
        auto key = parseExpr();
        consume(TK::RBRACE, "}");
        auto n = std::make_unique<Node>(); n->kind = NK::DeleteFunc;
        n->name = nm; n->left = std::move(key); n->line = line;
        return n;
    }

    /* keys %h  /  values %h */
    if (check(TK::KW_KEYS) || check(TK::KW_VALUES)) {
        bool isKeys = check(TK::KW_KEYS); advance();
        bool hasParen = match(TK::LPAREN);
        consume(TK::HASH, "%");
        std::string nm = cur().text; advance();
        if (hasParen) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>();
        n->kind = isKeys ? NK::KeysFunc : NK::ValuesFunc;
        n->name = nm; n->line = line;
        return n;
    }

    /* sort LIST  or  sort { CMP } LIST  or  sort keys %h  or  sort @arr */
    if (check(TK::KW_SORT)) {
        advance();
        /* detect sort { $a <=> $b } or { $b <=> $a } or sort { BLOCK } */
        std::string sortMode;
        NodePtr sortBlock;
        if (check(TK::LBRACE)) {
            size_t save = pos_;
            advance(); // {
            std::string first, op, second;
            if (check(TK::SCALAR)) { advance(); first = cur().text; advance(); }
            if (check(TK::SPACESHIP))                { op = "num"; advance(); }
            else if (check(TK::IDENT) && cur().text == "cmp") { op = "str"; advance(); }
            if (check(TK::SCALAR)) { advance(); second = cur().text; advance(); }
            if (!op.empty() && check(TK::RBRACE) && !first.empty() && !second.empty()) {
                advance(); // }
                if      (first == "a" && second == "b") sortMode = op + "_asc";
                else if (first == "b" && second == "a") sortMode = op + "_desc";
            }
            if (sortMode.empty()) {
                pos_ = save; /* restore — parse as arbitrary block */
                sortBlock = parseBlock();
                sortMode = "custom";
            }
        }
        NodeList elems;
        /* sort (list) or sort list-expr */
        if (check(TK::KW_KEYS) || check(TK::KW_VALUES)) {
            auto inner = parsePrimary();
            inner->sval = "sort";
            auto n = std::make_unique<Node>(); n->kind = NK::SortFunc;
            n->left = std::move(inner); n->sval = sortMode; n->line = line;
            n->body = std::move(sortBlock);
            return n;
        }
        if (check(TK::ARRAY)) {
            auto inner = parsePrimary();
            auto n = std::make_unique<Node>(); n->kind = NK::SortFunc;
            n->left = std::move(inner); n->sval = sortMode; n->line = line;
            n->body = std::move(sortBlock);
            return n;
        }
        if (check(TK::LPAREN)) {
            advance();
            while (!check(TK::RPAREN) && !check(TK::EOF_TOK)) {
                elems.push_back(parseExpr());
                if (!match(TK::COMMA) && !match(TK::FATARROW)) break;
            }
            consume(TK::RPAREN, ")");
        }
        auto n = std::make_unique<Node>(); n->kind = NK::SortFunc;
        n->args = std::move(elems); n->sval = sortMode; n->line = line;
        n->body = std::move(sortBlock);
        return n;
    }

    /* pop / shift */
    if (check(TK::KW_POP) || check(TK::KW_SHIFT)) {
        bool isPop = check(TK::KW_POP); advance();
        bool hasParen = match(TK::LPAREN);
        std::string nm = subDepth_ > 0 ? "_" : "ARGV"; /* default: @_ in sub, @ARGV at top level */
        if (check(TK::ARRAY)) {
            advance();
            nm = cur().text; advance();
        }
        if (hasParen) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>();
        n->kind = isPop ? NK::PopExpr : NK::ShiftExpr;
        n->name = nm; n->line = line;
        return n;
    }

    /* chomp / chop */
    if (check(TK::KW_CHOMP) || check(TK::KW_CHOP)) {
        bool isChomp = check(TK::KW_CHOMP); advance();
        bool hasParen = match(TK::LPAREN);
        NodePtr inner;
        if (hasParen) {
            inner = parseExpr();
            consume(TK::RPAREN, ")");
        } else if (check(TK::SEMI) || check(TK::RBRACE) || check(TK::EOF_TOK) || isModifier()) {
            inner = makeScalar("_", line); /* default to $_ */
        } else {
            inner = parseExpr();
        }
        auto n = std::make_unique<Node>(); n->kind = NK::ChompFunc;
        n->left = std::move(inner); n->sval = isChomp ? "chomp" : "chop";
        n->line = line; return n;
    }

    /* length */
    if (check(TK::KW_LENGTH)) {
        advance();
        bool hasParen = match(TK::LPAREN);
        auto inner = parseExpr();
        if (hasParen) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::LengthFunc;
        n->left = std::move(inner); n->line = line; return n;
    }

    /* substr($str, $off [, $len]) */
    if (check(TK::KW_SUBSTR)) {
        advance();
        bool hasParen = match(TK::LPAREN);
        NodeList args;
        while (args.size() < 4 && !check(TK::RPAREN) && !check(TK::EOF_TOK)) {
            args.push_back(parseExpr());
            if (!match(TK::COMMA)) break;
        }
        if (hasParen) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::SubstrFunc;
        n->args = std::move(args); n->line = line; return n;
    }

    /* join($sep, @arr / list) */
    if (check(TK::KW_JOIN)) {
        advance();
        bool hasParen = match(TK::LPAREN);
        NodePtr sep = parseExpr();
        match(TK::COMMA);
        NodeList rest;
        while (!check(TK::RPAREN) && !check(TK::SEMI) && !check(TK::EOF_TOK)) {
            rest.push_back(parseExpr());
            if (!match(TK::COMMA)) break;
        }
        if (hasParen) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::JoinFunc;
        n->left = std::move(sep); n->args = std::move(rest); n->line = line;
        return n;
    }

    /* readline: <$fh>, <STDIN>, <> */
    if (check(TK::READLINE)) {
        std::string fhname = cur().text; advance();
        auto n = std::make_unique<Node>(); n->kind = NK::Readline; n->sval = fhname; n->line = line;
        return n;
    }

    /* open([my] $fh, mode, filename) or open([my] $fh, "mode_file") */
    if (check(TK::KW_OPEN)) {
        advance();
        bool hasParen = match(TK::LPAREN);
        bool isMy = match(TK::KW_MY);
        consume(TK::SCALAR, "$");
        std::string fhname = consume(TK::IDENT, "filehandle name").text;
        match(TK::COMMA);
        NodeList args;
        while (true) {
            if (hasParen && check(TK::RPAREN)) break;
            if (check(TK::SEMI) || check(TK::EOF_TOK)) break;
            args.push_back(parseExpr());
            if (!match(TK::COMMA)) break;
        }
        if (hasParen) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::OpenFunc; n->line = line;
        n->name = fhname; n->sval = isMy ? "my" : ""; n->args = std::move(args);
        return n;
    }

    /* close($fh) */
    if (check(TK::KW_CLOSE)) {
        advance();
        bool hasParen = match(TK::LPAREN);
        auto fh = parseExpr();
        if (hasParen) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::CloseFunc; n->line = line;
        n->left = std::move(fh);
        return n;
    }

    /* eof($fh) */
    if (check(TK::KW_EOF)) {
        advance();
        bool hasParen = match(TK::LPAREN);
        NodePtr fh;
        if (!check(TK::RPAREN) && !check(TK::SEMI) && !check(TK::EOF_TOK))
            fh = parseExpr();
        if (hasParen) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::EofFunc; n->line = line;
        n->left = std::move(fh);
        return n;
    }

    /* unlink LIST */
    if (check(TK::KW_UNLINK)) {
        advance();
        bool hasParen = match(TK::LPAREN);
        auto n = std::make_unique<Node>(); n->kind = NK::UnlinkFunc; n->line = line;
        while (!check(TK::SEMI) && !check(TK::EOF_TOK) &&
               !(hasParen && check(TK::RPAREN))) {
            n->args.push_back(parseExpr());
            if (!match(TK::COMMA)) break;
        }
        if (hasParen) consume(TK::RPAREN, ")");
        return n;
    }

    /* die [EXPR] — also usable in expression context, e.g. open(...) or die "..." */
    if (check(TK::KW_DIE)) {
        advance();
        NodePtr msg;
        if (!check(TK::SEMI) && !isModifier() && !check(TK::EOF_TOK) && !check(TK::RPAREN))
            msg = parseExpr();
        auto n = std::make_unique<Node>(); n->kind = NK::DieStmt; n->line = line;
        n->left = std::move(msg);
        return n;
    }

    /* 'my $var [= expr]' in expression context (e.g. while (my $line = <$fh>)) */
    if (check(TK::KW_MY) && peek(1).kind == TK::SCALAR) {
        advance();  /* my */
        pos_++;     /* $ */
        std::string vname = advance().text;
        auto n = std::make_unique<Node>(); n->kind = NK::My; n->line = line;
        n->name = "$" + vname;
        if (check(TK::ASSIGN)) { advance(); n->right = parseAssign(); }
        return n;
    }

    /* sprintf($fmt, args...) */
    if (check(TK::KW_SPRINTF)) {
        advance();
        bool hasParen = match(TK::LPAREN);
        NodePtr fmt = parseExpr();
        NodeList args;
        while (match(TK::COMMA)) {
            if (!hasParen && isModifier()) break;
            if (hasParen && check(TK::RPAREN)) break;
            if (check(TK::SEMI) || check(TK::EOF_TOK)) break;
            args.push_back(parseExpr());
        }
        if (hasParen) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::SprintfFunc; n->line = line;
        n->left = std::move(fmt); n->args = std::move(args);
        return n;
    }

    /* split(/pat/, $str) or split($sep, $str) */
    if (check(TK::KW_SPLIT)) {
        advance();
        bool hasParen = match(TK::LPAREN);
        NodePtr sep;
        /* first arg: regex literal or string */
        bool regexSplit = false;
        std::string splitPat, splitFlags;
        if (check(TK::REGEX)) {
            std::string txt = cur().text; advance();
            size_t sp = txt.find('\x01');
            splitPat   = txt.substr(0, sp);
            splitFlags = (sp != std::string::npos) ? txt.substr(sp + 1) : "";
            regexSplit = true;
            sep = makeStr(splitPat, line); /* placeholder, not used for regex split */
        } else {
            sep = parseExpr();
        }
        match(TK::COMMA);
        NodePtr str;
        if (!check(TK::RPAREN) && !check(TK::SEMI) && !check(TK::EOF_TOK))
            str = parseExpr();
        if (hasParen) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::SplitFunc;
        n->left = std::move(sep); n->right = std::move(str); n->line = line;
        if (regexSplit) { n->ival = 1; n->sval = splitPat; n->name = splitFlags; }
        return n;
    }

    /* ── math builtins: abs, int, sqrt ─────────────────────────────────────── */
    {
        NK kind = NK::IntLit; /* placeholder */
        if      (check(TK::KW_ABS))  { kind = NK::AbsFunc;  }
        else if (check(TK::KW_INT))  { kind = NK::IntFunc;   }
        else if (check(TK::KW_SQRT)) { kind = NK::SqrtFunc;  }
        if (kind != NK::IntLit) {
            advance();
            bool hp = match(TK::LPAREN);
            auto inner = parseExpr();
            if (hp) consume(TK::RPAREN, ")");
            auto n = std::make_unique<Node>(); n->kind = kind; n->line = line;
            n->left = std::move(inner); return n;
        }
    }

    /* ── string case: uc, lc, ucfirst, lcfirst ──────────────────────────── */
    {
        NK kind = NK::IntLit;
        if      (check(TK::KW_UC))      { kind = NK::UcFunc;      }
        else if (check(TK::KW_LC))      { kind = NK::LcFunc;      }
        else if (check(TK::KW_UCFIRST)) { kind = NK::UcfirstFunc; }
        else if (check(TK::KW_LCFIRST)) { kind = NK::LcfirstFunc; }
        if (kind != NK::IntLit) {
            advance();
            bool hp = match(TK::LPAREN);
            auto inner = parseExpr();
            if (hp) consume(TK::RPAREN, ")");
            auto n = std::make_unique<Node>(); n->kind = kind; n->line = line;
            n->left = std::move(inner); return n;
        }
    }

    /* ── chr, ord, hex, oct ──────────────────────────────────────────────── */
    {
        NK kind = NK::IntLit;
        if      (check(TK::KW_CHR)) { kind = NK::ChrFunc; }
        else if (check(TK::KW_ORD)) { kind = NK::OrdFunc; }
        else if (check(TK::KW_HEX)) { kind = NK::HexFunc; }
        else if (check(TK::KW_OCT)) { kind = NK::OctFunc; }
        if (kind != NK::IntLit) {
            advance();
            bool hp = match(TK::LPAREN);
            auto inner = parseExpr();
            if (hp) consume(TK::RPAREN, ")");
            auto n = std::make_unique<Node>(); n->kind = kind; n->line = line;
            n->left = std::move(inner); return n;
        }
    }

    /* ── rand, srand ─────────────────────────────────────────────────────── */
    if (check(TK::KW_RAND) || check(TK::KW_SRAND)) {
        bool isRand = check(TK::KW_RAND); advance();
        auto n = std::make_unique<Node>();
        n->kind = isRand ? NK::RandFunc : NK::SrandFunc; n->line = line;
        bool hp = match(TK::LPAREN);
        if (!check(TK::RPAREN) && !check(TK::SEMI) && !check(TK::EOF_TOK) && !isModifier())
            n->left = parseExpr();
        if (hp) consume(TK::RPAREN, ")");
        return n;
    }

    /* ── time, localtime, gmtime ─────────────────────────────────────────── */
    if (check(TK::KW_TIME)) {
        advance();
        /* optional empty parens */
        if (match(TK::LPAREN)) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::TimeFunc; n->line = line;
        return n;
    }
    if (check(TK::KW_LOCALTIME) || check(TK::KW_GMTIME)) {
        bool isGm = check(TK::KW_GMTIME); advance();
        auto n = std::make_unique<Node>();
        n->kind = isGm ? NK::GmtimeFunc : NK::LocaltimeFunc; n->line = line;
        bool hp = match(TK::LPAREN);
        if (!check(TK::RPAREN) && !check(TK::SEMI) && !check(TK::EOF_TOK) && !isModifier())
            n->left = parseExpr();
        if (hp) consume(TK::RPAREN, ")");
        return n;
    }

    /* ── sleep, alarm ────────────────────────────────────────────────────── */
    if (check(TK::KW_SLEEP) || check(TK::KW_ALARM)) {
        bool isSleep = check(TK::KW_SLEEP); advance();
        auto n = std::make_unique<Node>();
        n->kind = isSleep ? NK::SleepFunc : NK::AlarmFunc; n->line = line;
        bool hp = match(TK::LPAREN);
        if (!check(TK::RPAREN) && !check(TK::SEMI) && !check(TK::EOF_TOK) && !isModifier())
            n->left = parseExpr();
        if (hp) consume(TK::RPAREN, ")");
        return n;
    }

    /* ── List::Util: sum, min, max, uniq ────────────────────────────────── */
    if (check(TK::KW_SUM) || check(TK::KW_MIN) || check(TK::KW_MAX) || check(TK::KW_UNIQ)) {
        NK kind = check(TK::KW_SUM) ? NK::SumFunc
                : check(TK::KW_MIN) ? NK::MinFunc
                : check(TK::KW_UNIQ)? NK::UniqFunc : NK::MaxFunc;
        advance();
        auto n = std::make_unique<Node>(); n->kind = kind; n->line = line;
        bool hp = match(TK::LPAREN);
        while (!check(TK::SEMI) && !check(TK::EOF_TOK) && !isModifier()) {
            if (hp && check(TK::RPAREN)) break;
            n->args.push_back(parseExpr());
            if (!match(TK::COMMA)) break;
        }
        if (hp) consume(TK::RPAREN, ")");
        return n;
    }

    /* ── List::Util: first, any, all, none — block form ─────────────────── */
    if (check(TK::KW_FIRST) || check(TK::KW_ANY) || check(TK::KW_ALL) || check(TK::KW_NONE)) {
        NK kind = check(TK::KW_FIRST) ? NK::FirstFunc
                : check(TK::KW_ANY)   ? NK::AnyFunc
                : check(TK::KW_NONE)  ? NK::NoneFunc : NK::AllFunc;
        advance();
        auto n = std::make_unique<Node>(); n->kind = kind; n->line = line;
        bool hp = match(TK::LPAREN);
        if (check(TK::LBRACE)) n->body = parseBlock();
        match(TK::COMMA);
        while (!check(TK::SEMI) && !check(TK::EOF_TOK) && !isModifier()) {
            if (hp && check(TK::RPAREN)) break;
            n->args.push_back(parseExpr());
            if (!match(TK::COMMA)) break;
        }
        if (hp) consume(TK::RPAREN, ")");
        return n;
    }

    /* ── List::Util: reduce ──────────────────────────────────────────────── */
    if (check(TK::KW_REDUCE)) {
        advance();
        auto n = std::make_unique<Node>(); n->kind = NK::ReduceFunc; n->line = line;
        bool hp = match(TK::LPAREN);
        if (check(TK::LBRACE)) n->body = parseBlock();
        match(TK::COMMA);
        while (!check(TK::SEMI) && !check(TK::EOF_TOK) && !isModifier()) {
            if (hp && check(TK::RPAREN)) break;
            n->args.push_back(parseExpr());
            if (!match(TK::COMMA)) break;
        }
        if (hp) consume(TK::RPAREN, ")");
        return n;
    }

    /* ── index, rindex ───────────────────────────────────────────────────── */
    if (check(TK::KW_INDEX) || check(TK::KW_RINDEX)) {
        bool isR = check(TK::KW_RINDEX); advance();
        bool hp = match(TK::LPAREN);
        NodeList args;
        while (args.size() < 3 && !check(TK::RPAREN) && !check(TK::EOF_TOK) && !check(TK::SEMI)) {
            args.push_back(parseExpr());
            if (!match(TK::COMMA)) break;
        }
        if (hp) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>();
        n->kind = isR ? NK::RindexFunc : NK::IndexFunc;
        n->args = std::move(args); n->line = line; return n;
    }

    /* ── reverse ─────────────────────────────────────────────────────────── */
    if (check(TK::KW_REVERSE)) {
        advance();
        bool hp = match(TK::LPAREN);
        auto n = std::make_unique<Node>(); n->kind = NK::ReverseFunc; n->line = line;
        /* collect all args (array var, list, or scalar) */
        while (!check(TK::SEMI) && !check(TK::EOF_TOK) && !isModifier()) {
            if (hp && check(TK::RPAREN)) break;
            n->args.push_back(parseExpr());
            if (!match(TK::COMMA)) break;
        }
        if (hp) consume(TK::RPAREN, ")");
        return n;
    }

    /* ── map { BLOCK } LIST  or  map EXPR, LIST ─────────────────────────── */
    if (check(TK::KW_MAP) || check(TK::KW_GREP)) {
        bool isMap = check(TK::KW_MAP); advance();
        auto n = std::make_unique<Node>();
        n->kind = isMap ? NK::MapFunc : NK::GrepFunc; n->line = line;
        bool hp = match(TK::LPAREN);
        if (check(TK::LBRACE)) {
            n->body = parseBlock(); /* block form: map { BLOCK } LIST */
        } else {
            n->left = parseExpr(); /* expr form: map EXPR, LIST */
        }
        match(TK::COMMA);
        /* parse the input list (array, range, or explicit list) */
        while (!check(TK::SEMI) && !check(TK::EOF_TOK) && !isModifier()) {
            if (hp && check(TK::RPAREN)) break;
            n->args.push_back(parseExpr());
            if (!match(TK::COMMA)) break;
        }
        if (hp) consume(TK::RPAREN, ")");
        return n;
    }

    /* Bare /regex/ outside split/binding — match against $_ */
    if (check(TK::REGEX)) {
        std::string txt = cur().text; advance();
        size_t sep = txt.find('\x01');
        auto n = std::make_unique<Node>(); n->kind = NK::RegexMatch; n->line = line;
        n->left = makeScalar("_", line);
        n->sval = txt.substr(0, sep);
        n->name = (sep != std::string::npos) ? txt.substr(sep + 1) : "";
        n->ival = 0;
        return n;
    }
    /* Bare s/// — substitute on $_ */
    if (check(TK::SUBST)) {
        std::string txt = cur().text; advance();
        size_t s1 = txt.find('\x01'), s2 = txt.find('\x01', s1 + 1);
        auto n = std::make_unique<Node>(); n->kind = NK::RegexSubst; n->line = line;
        n->left = makeScalar("_", line);
        n->sval = txt.substr(0, s1);
        n->name = txt.substr(s1 + 1, s2 - s1 - 1) + "\x01" + txt.substr(s2 + 1);
        return n;
    }
    /* Bare tr/// — translate on $_ */
    if (check(TK::TR)) {
        std::string txt = cur().text; advance();
        auto n = std::make_unique<Node>(); n->kind = NK::TrOp; n->line = line;
        n->left = makeScalar("_", line);
        n->sval = txt;
        return n;
    }

    /* splice(@arr, off[, len[, repl...]]) */
    if (check(TK::KW_SPLICE)) {
        advance();
        bool hp = match(TK::LPAREN);
        consume(TK::ARRAY, "@");
        std::string nm = cur().text; advance();
        auto n = std::make_unique<Node>(); n->kind = NK::SpliceFunc; n->name = nm; n->line = line;
        while (!check(TK::SEMI) && !check(TK::EOF_TOK) && !isModifier()) {
            if (!match(TK::COMMA)) break;
            if ((hp && check(TK::RPAREN)) || check(TK::SEMI) || check(TK::EOF_TOK)) break;
            n->args.push_back(parseExpr());
        }
        if (hp) consume(TK::RPAREN, ")");
        return n;
    }

    /* system("cmd") */
    if (check(TK::KW_SYSTEM)) {
        advance();
        bool hp = match(TK::LPAREN);
        auto cmd = parseExpr();
        if (hp) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::SystemFunc;
        n->left = std::move(cmd); n->line = line;
        return n;
    }

    /* die / warn in expression context (e.g. open(...) or die "msg") */
    if (check(TK::KW_DIE) || check(TK::KW_WARN)) {
        bool isDie = check(TK::KW_DIE); advance();
        bool hp = match(TK::LPAREN);
        NodePtr msg;
        if (!check(TK::SEMI) && !check(TK::EOF_TOK) && !(hp && check(TK::RPAREN)))
            msg = parseExpr();
        if (hp) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>();
        n->kind = isDie ? NK::DieStmt : NK::WarnStmt; n->line = line;
        n->left = std::move(msg);
        return n;
    }

    /* ── filesystem ops ─────────────────────────────────────────────────── */
    if (check(TK::KW_CHDIR)) {
        advance(); bool hp = match(TK::LPAREN);
        auto path = parseExpr(); if (hp) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::ChdirFunc; n->line = line;
        n->left = std::move(path); return n;
    }
    if (check(TK::KW_MKDIR)) {
        advance(); bool hp = match(TK::LPAREN);
        auto path = parseExpr();
        NodePtr mode;
        if (match(TK::COMMA) && !check(TK::RPAREN)) mode = parseExpr();
        if (hp) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::MkdirFunc; n->line = line;
        n->left = std::move(path); n->right = std::move(mode); return n;
    }
    if (check(TK::KW_RMDIR)) {
        advance(); bool hp = match(TK::LPAREN);
        auto path = parseExpr(); if (hp) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::RmdirFunc; n->line = line;
        n->left = std::move(path); return n;
    }
    if (check(TK::KW_RENAME)) {
        advance(); bool hp = match(TK::LPAREN);
        auto oldp = parseExpr(); match(TK::COMMA); auto newp = parseExpr();
        if (hp) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::RenameFunc; n->line = line;
        n->left = std::move(oldp); n->right = std::move(newp); return n;
    }
    if (check(TK::KW_CHMOD)) {
        advance(); bool hp = match(TK::LPAREN);
        auto mode = parseExpr();
        auto n = std::make_unique<Node>(); n->kind = NK::ChmodFunc; n->line = line;
        n->left = std::move(mode);
        while (match(TK::COMMA)) {
            if (hp && check(TK::RPAREN)) break;
            if (check(TK::SEMI) || check(TK::EOF_TOK)) break;
            n->args.push_back(parseExpr());
        }
        if (hp) consume(TK::RPAREN, ")");
        return n;
    }

    /* ── directory I/O ───────────────────────────────────────────────────── */
    if (check(TK::KW_OPENDIR)) {
        advance(); bool hp = match(TK::LPAREN);
        /* opendir(my $dh, path) or opendir(DH, path) */
        std::string dhVar;
        if (check(TK::KW_MY)) { advance(); consume(TK::SCALAR, "$"); dhVar = cur().text; advance(); }
        else if (check(TK::SCALAR)) { advance(); dhVar = cur().text; advance(); }
        else { dhVar = cur().text; advance(); } /* bare DH ident */
        match(TK::COMMA);
        auto path = parseExpr(); if (hp) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::OpendirFunc; n->line = line;
        n->name = dhVar; n->left = std::move(path); return n;
    }
    if (check(TK::KW_READDIR)) {
        advance(); bool hp = match(TK::LPAREN);
        std::string dhVar;
        if (check(TK::SCALAR)) { advance(); dhVar = cur().text; advance(); }
        else { dhVar = cur().text; advance(); }
        if (hp) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::ReaddirFunc; n->line = line;
        n->name = dhVar; return n;
    }
    if (check(TK::KW_CLOSEDIR)) {
        advance(); bool hp = match(TK::LPAREN);
        std::string dhVar;
        if (check(TK::SCALAR)) { advance(); dhVar = cur().text; advance(); }
        else { dhVar = cur().text; advance(); }
        if (hp) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::ClosedirFunc; n->line = line;
        n->name = dhVar; return n;
    }

    /* unshift as expression (returns new count) */
    if (check(TK::KW_UNSHIFT)) {
        advance();
        bool hasParen = match(TK::LPAREN);
        consume(TK::ARRAY, "@");
        std::string nm = cur().text; advance();
        match(TK::COMMA);
        NodeList vals;
        while (!check(TK::RPAREN) && !check(TK::SEMI) && !check(TK::EOF_TOK)) {
            vals.push_back(parseExpr());
            if (!match(TK::COMMA)) break;
        }
        if (hasParen) consume(TK::RPAREN, ")");
        auto n = std::make_unique<Node>(); n->kind = NK::UnshiftStmt2;
        n->name = nm; n->args = std::move(vals); n->line = line;
        return n;
    }

    /* anonymous hash ref:  { key => val, ... } */
    if (check(TK::LBRACE)) {
        advance();
        NodeList elems;
        while (!check(TK::RBRACE) && !check(TK::EOF_TOK)) {
            elems.push_back(parseExpr());
            if (!match(TK::COMMA) && !match(TK::FATARROW)) break;
        }
        consume(TK::RBRACE, "}");
        auto n = std::make_unique<Node>(); n->kind = NK::AnonHash;
        n->args = std::move(elems); n->line = line;
        return n;
    }

    /* parenthesised expression or list — FATARROW (=>) acts as COMMA */
    if (check(TK::LPAREN)) {
        advance();
        if (check(TK::RPAREN)) { advance(); auto n = std::make_unique<Node>(); n->kind = NK::ArrayLit; n->line = line; return n; }
        auto inner = parseExpr();
        if (check(TK::COMMA) || check(TK::FATARROW)) {
            NodeList elems; elems.push_back(std::move(inner));
            while (match(TK::COMMA) || match(TK::FATARROW)) {
                if (check(TK::RPAREN)) break;
                elems.push_back(parseExpr());
            }
            consume(TK::RPAREN, ")");
            auto n = std::make_unique<Node>(); n->kind = NK::ArrayLit;
            n->args = std::move(elems); n->line = line;
            return n;
        }
        consume(TK::RPAREN, ")");
        return inner;
    }

    /* function call or bare identifier */
    if (check(TK::IDENT)) {
        std::string nm = cur().text; advance();
        /* bare word string comparison ops handled in parseCmp */
        if (check(TK::LPAREN)) return parseCall(nm, line);
        /* bareword — if followed by FATARROW it's an auto-quoted string */
        if (check(TK::FATARROW)) return makeStr(nm, line);
        /* check constant map so bare NAME resolves without parens */
        {
            auto cit = constMap_.find(nm);
            if (cit != constMap_.end()) {
                return cit->second->clone();  /* pre-parsed AST node (cloned for reuse) */
            }
        }
        /* bareword string */
        return makeStr(nm, line);
    }

    throw std::runtime_error("Parse error line " + std::to_string(line) +
        ": unexpected token '" + cur().text + "' (this may be due to advanced Perl syntax in an imported module)");
}

NodePtr Parser::parseCall(std::string name, int line) {
    /* remap imported short names to their qualified Module::name */
    auto it = importMap_.find(name);
    if (it != importMap_.end()) name = it->second;
    consume(TK::LPAREN, "(");
    NodeList args;
    while (!check(TK::RPAREN) && !check(TK::EOF_TOK)) {
        args.push_back(parseExpr());
        if (!match(TK::COMMA) && !match(TK::FATARROW)) break;
    }
    consume(TK::RPAREN, ")");
    auto n = std::make_unique<Node>(); n->kind = NK::Call;
    n->name = name; n->args = std::move(args); n->line = line;
    return n;
}

/* ── string interpolation ────────────────────────────────────────────────── */

NodePtr Parser::parseStringInterp(const std::string &raw, int line) {
    /* scan raw for $var / ${var} / $1-$9 / $@ / $0 / $arr[i] / $hash{k}
       and @arr and split into string + expression fragments */
    std::vector<NodePtr> parts;
    std::string cur_s;

    auto flush = [&]{ if (!cur_s.empty()) { parts.push_back(makeStr(cur_s, line)); cur_s.clear(); } };

    size_t i = 0;
    while (i < raw.size()) {
        /* $@ — eval error */
        if (raw[i] == '$' && i + 1 < raw.size() && raw[i+1] == '@') {
            flush();
            auto n = std::make_unique<Node>(); n->kind = NK::DollarAt; n->line = line;
            parts.push_back(std::move(n));
            i += 2; continue;
        }
        /* $0 — program name */
        if (raw[i] == '$' && i + 1 < raw.size() && raw[i+1] == '0') {
            flush();
            parts.push_back(makeScalar("0", line));
            i += 2; continue;
        }
        /* $1-$9 — capture vars */
        if (raw[i] == '$' && i + 1 < raw.size() && raw[i+1] >= '1' && raw[i+1] <= '9') {
            flush();
            long long n = raw[i+1] - '0'; i += 2;
            auto cv = std::make_unique<Node>(); cv->kind = NK::CaptureVar;
            cv->ival = n; cv->line = line;
            parts.push_back(std::move(cv));
            continue;
        }
        /* ${varname} */
        if (raw[i] == '$' && i + 1 < raw.size() && raw[i+1] == '{') {
            flush(); i += 2;
            std::string vname;
            while (i < raw.size() && raw[i] != '}') vname += raw[i++];
            if (i < raw.size()) i++; /* skip } */
            parts.push_back(makeScalar(vname, line));
            continue;
        }
        /* $varname possibly followed by [idx] or {key} */
        if (raw[i] == '$' && i + 1 < raw.size() && (isalpha(raw[i+1]) || raw[i+1] == '_')) {
            flush(); i++;
            std::string vname;
            while (i < raw.size() && (isalnum(raw[i]) || raw[i] == '_')) vname += raw[i++];
            /* $arr[idx] */
            if (i < raw.size() && raw[i] == '[') {
                i++;
                std::string idx_s;
                while (i < raw.size() && raw[i] != ']') idx_s += raw[i++];
                if (i < raw.size()) i++;
                auto n = std::make_unique<Node>(); n->kind = NK::ArrayElem;
                n->name = vname;
                long long idx = idx_s.empty() ? 0 : std::stoll(idx_s);
                n->left = makeInt(idx, line);
                n->line = line;
                parts.push_back(std::move(n));
            /* $hash{key} */
            } else if (i < raw.size() && raw[i] == '{') {
                i++;
                std::string key_s;
                while (i < raw.size() && raw[i] != '}') key_s += raw[i++];
                if (i < raw.size()) i++;
                auto n = std::make_unique<Node>(); n->kind = NK::HashElem;
                n->name = vname;
                n->left = makeStr(key_s, line);
                n->line = line;
                parts.push_back(std::move(n));
            } else {
                parts.push_back(makeScalar(vname, line));
            }
            continue;
        }
        /* @arr — interpolate entire array joined by $" (default space) */
        if (raw[i] == '@' && i + 1 < raw.size() && (isalpha(raw[i+1]) || raw[i+1] == '_')) {
            flush(); i++;
            std::string vname;
            while (i < raw.size() && (isalnum(raw[i]) || raw[i] == '_')) vname += raw[i++];
            /* join array with space */
            auto arrNode = std::make_unique<Node>(); arrNode->kind = NK::ArrayVar;
            arrNode->name = vname; arrNode->line = line;
            auto sepNode  = makeStr(" ", line);
            auto joinNode = std::make_unique<Node>(); joinNode->kind = NK::JoinFunc;
            joinNode->left = std::move(sepNode);
            joinNode->args.push_back(std::move(arrNode));
            joinNode->line = line;
            parts.push_back(std::move(joinNode));
            continue;
        }
        cur_s += raw[i++];
    }
    flush();

    if (parts.empty()) return makeStr("", line);
    if (parts.size() == 1) return std::move(parts[0]);

    /* fold into left-associative concat chain */
    auto result = std::move(parts[0]);
    for (size_t j = 1; j < parts.size(); j++)
        result = makeBin(".", std::move(result), std::move(parts[j]), line);
    return result;
}

NodeList Parser::parseArgList() {
    NodeList args;
    consume(TK::LPAREN, "(");
    while (!check(TK::RPAREN) && !check(TK::EOF_TOK)) {
        args.push_back(parseExpr());
        if (!match(TK::COMMA)) break;
    }
    consume(TK::RPAREN, ")");
    return args;
}

