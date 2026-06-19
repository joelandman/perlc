#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "jit.h"
#include "runtime.h"
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/TargetSelect.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <set>
#include <map>
#include <vector>

static std::string readFile(const std::string &path) {
    std::ifstream f(path);
    if (!f) return "";
    std::ostringstream buf; buf << f.rdbuf();
    return buf.str();
}

static std::string dirOf(const std::string &path) {
    auto p = path.rfind('/');
    return p == std::string::npos ? "." : path.substr(0, p);
}

/* Extract qw(...) word list from token stream starting after current position.
   Advances *pos past the closing ')'.  Returns list of words. */
static std::vector<std::string> extractQw(
        const std::vector<Token> &toks, size_t pos, size_t end)
{
    std::vector<std::string> words;
    /* look for QWORDS token or bare LPAREN IDENT... RPAREN */
    while (pos < end) {
        if (toks[pos].kind == TK::QWORDS) {
            /* text is space-separated words */
            std::istringstream ss(toks[pos].text);
            std::string w; while (ss >> w) words.push_back(w);
            return words;
        }
        if (toks[pos].kind == TK::LPAREN) {
            pos++;
            while (pos < end && toks[pos].kind != TK::RPAREN) {
                if (toks[pos].kind == TK::IDENT || toks[pos].kind == TK::STRING)
                    words.push_back(toks[pos].text);
                pos++;
            }
            return words;
        }
        if (toks[pos].kind == TK::IDENT || toks[pos].kind == TK::STRING) {
            words.push_back(toks[pos].text);
            return words;  /* single unparenthesised name */
        }
        pos++;
    }
    return words;
}

/* Install missing Perl modules using cpanm into a local lib/ directory.
   Returns true if all modules were successfully installed or were already present. */
static bool installMissingModules(const std::vector<Token> &tokens,
                                  const std::string &baseDir)
{
    static const std::set<std::string> PRAGMAS = {
        "strict","warnings","feature","parent","base",
        "Exporter","Carp","POSIX","Scalar::Util",
        "List::Util","Data::Dumper","Storable","overload",
        "constant"
    };

    std::set<std::string> modulesToInstall;
    std::vector<std::string> searchDirs = {
        baseDir, baseDir + "/lib", "lib", "lib/lib/perl5", "."
    };

    /* Scan for use statements */
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i].kind == TK::KW_USE && tokens[i+1].kind == TK::IDENT) {
            std::string modName = tokens[i+1].text;
            if (PRAGMAS.count(modName)) continue;

            /* Check if module already exists in search path */
            bool found = false;
            std::string modPath = modName;
            for (char &c : modPath) if (c == ':') c = '/';
            while (modPath.find("//") != std::string::npos)
                modPath.replace(modPath.find("//"), 2, "/");
            modPath += ".pm";

            for (const auto &dir : searchDirs) {
                std::string fullPath = dir + "/" + modPath;
                if (access(fullPath.c_str(), R_OK) == 0) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                modulesToInstall.insert(modName);
            }
        }
    }

    if (modulesToInstall.empty()) {
        std::cout << "All Perl modules are already available.\n";
        return true;
    }

    std::cout << "Installing " << modulesToInstall.size()
              << " missing Perl module(s) using cpanm...\n";

    /* Create lib directory if it doesn't exist */
    std::string libDir = "lib";
    if (access(libDir.c_str(), F_OK) != 0) {
        if (system("mkdir -p lib") != 0) {
            std::cerr << "Failed to create lib/ directory\n";
            return false;
        }
    }

    /* Install each missing module */
    for (const auto &mod : modulesToInstall) {
        std::cout << "  Installing " << mod << "...\n";
        std::string cmd = "cpanm --quiet --notest --local-lib lib " + mod;
        int rc = system(cmd.c_str());
        if (rc != 0) {
            std::cerr << "Failed to install module: " << mod << "\n";
            std::cerr << "You may need to install cpanm first: sudo apt install cpanminus\n";
            return false;
        }
    }

    std::cout << "All modules installed successfully to lib/\n";
    return true;
}

/* Scan module tokens for  our @EXPORT = qw(...)  and  our @EXPORT_OK = qw(...)
   Returns map "EXPORT" → [names] and "EXPORT_OK" → [names]. */
static std::map<std::string, std::vector<std::string>>
scanExports(const std::vector<Token> &toks)
{
    std::map<std::string, std::vector<std::string>> result;
    for (size_t i = 0; i + 3 < toks.size(); i++) {
        /* pattern: our @EXPORT [_OK] = qw(...) */
        if (toks[i].kind != TK::KW_OUR) continue;
        if (i+1 >= toks.size() || toks[i+1].kind != TK::ARRAY) continue;
        if (i+2 >= toks.size()) continue;
        std::string arrName = toks[i+2].text;
        if (arrName != "EXPORT" && arrName != "EXPORT_OK") continue;
        /* find = */
        size_t j = i + 3;
        while (j < toks.size() && toks[j].kind != TK::ASSIGN && toks[j].kind != TK::SEMI) j++;
        if (j >= toks.size() || toks[j].kind != TK::ASSIGN) continue;
        j++;
        /* find semicolon as end bound */
        size_t end = j;
        while (end < toks.size() && toks[end].kind != TK::SEMI) end++;
        result[arrName] = extractQw(toks, j, end);
    }
    return result;
}

/* Inline `use Module` by prepending module tokens.
   Also builds importMap (short → qualified) from @EXPORT and explicit import lists.
   Pragmas (strict/warnings/feature/parent/base/Exporter/Carp/POSIX/Scalar::Util etc)
   are handled or skipped.  Returns combined token list. */
static std::vector<Token> inlineModules(
         const std::vector<Token> &tokens,
         const std::string &baseDir,
         std::set<std::string> &loaded,
         std::map<std::string,std::string> &importMap,
         std::map<std::string,NodePtr> *constMap = nullptr,
         Parser *parser = nullptr)
{
    /* pragmas that are not files to load */
    static const std::set<std::string> PRAGMAS = {
        "strict","warnings","feature","parent","base",
        "Exporter","Carp","POSIX","Scalar::Util",
        "List::Util","Data::Dumper","Storable","overload",
    };

    std::vector<Token> modTokens;   /* tokens from all inlined modules */
    std::vector<Token> constToks;   /* synthetic constant sub definitions */

    std::vector<std::string> searchDirsBase = {
        baseDir, baseDir + "/lib", "lib", "lib/lib/perl5", "."
    };

    for (size_t i = 0; i < tokens.size(); ) {
        /* ── require "file.pm" or require Module::Name ── */
        if (tokens[i].kind == TK::KW_REQUIRE &&
            i + 1 < tokens.size() &&
            (tokens[i+1].kind == TK::IDENT || tokens[i+1].kind == TK::STRING)) {
            std::string rawName = tokens[i+1].text;
            /* double-quoted STRING tokens have a leading \x01 marker — strip it */
            if (!rawName.empty() && rawName[0] == '\x01') rawName = rawName.substr(1);
            /* advance past: require <name> ; */
            size_t j = i + 2;
            while (j < tokens.size() && tokens[j].kind != TK::SEMI) j++;
            i = (j < tokens.size()) ? j + 1 : j;

            /* convert to module name (for loaded tracking) and file path */
            std::string modName = rawName, modPath = rawName;
            if (rawName.find('/') != std::string::npos ||
                (rawName.size() > 3 && rawName.substr(rawName.size()-3) == ".pm")) {
                /* file path form: "Foo/Bar.pm" */
                modPath = rawName;
                /* strip .pm and convert / to :: for the loaded key */
                if (modPath.size() > 3 && modPath.substr(modPath.size()-3) == ".pm")
                    modName = modPath.substr(0, modPath.size()-3);
                for (char &c : modName) if (c == '/') c = ':';
            } else {
                /* module name form: Foo::Bar */
                modPath = rawName;
                for (char &c : modPath) if (c == ':') c = '/';
                while (modPath.find("//") != std::string::npos)
                    modPath.replace(modPath.find("//"), 2, "/");
                modPath += ".pm";
            }
            if (PRAGMAS.count(modName) || loaded.count(modName)) continue;
            /* helper: load and inline one file into modTokens */
            auto tryInlineFile = [&](const std::string &fullPath) -> bool {
                if (access(fullPath.c_str(), R_OK) != 0) return false;
                loaded.insert(modName);
                std::string src = readFile(fullPath);
                Lexer modLexer(src);
                auto modToks = modLexer.tokenize();
                if (!modToks.empty() && modToks.back().kind == TK::EOF_TOK) modToks.pop_back();
                auto expanded = inlineModules(modToks, dirOf(fullPath), loaded, importMap, constMap, parser);
                if (!expanded.empty() && expanded.back().kind == TK::EOF_TOK) expanded.pop_back();
                modTokens.insert(modTokens.end(), expanded.begin(), expanded.end());
                return true;
            };
            /* absolute path: use directly */
            if (!modPath.empty() && modPath[0] == '/') { tryInlineFile(modPath); continue; }
            for (auto &dir : searchDirsBase) {
                if (tryInlineFile(dir + "/" + modPath)) break;
            }
            continue;
        }

        if (tokens[i].kind != TK::KW_USE ||
            i + 1 >= tokens.size() ||
            tokens[i+1].kind != TK::IDENT) {
            i++;
            continue;
        }

        std::string modName = tokens[i+1].text;

        /* find semicolon end of this use statement */
        size_t j = i + 2;
        while (j < tokens.size() && tokens[j].kind != TK::SEMI) j++;
        size_t useEnd = j;  /* index of SEMI */
        i = j < tokens.size() ? j + 1 : j;  /* advance past semicolon */

        /* ── use constant NAME => VALUE  or  use constant { NAME => V, ... } */
        if (modName == "constant") {
            size_t k = /* skip past 'use constant' */ (tokens[i-1-1].text == "constant" ? i-1-1 : 2);
            /* find position right after 'constant' ident */
            k = /* tokens[i-1] is SEMI, so look between IDENT('constant') and SEMI */
                0; /* re-scan */
            /* find 'use constant' index */
            size_t useIdx = i - (useEnd - (/* from original i */ 0)) - 1;
            /* simpler: re-scan forward from useEnd-1 backwards... just scan constToks area */
            /* Actually: we already have useEnd (SEMI) and we know use starts at ~i-something.
               The tokens between 'constant' (tokens[useIdx+1]) and SEMI are the definition. */
            /* Re-derive: walk backwards from useEnd to find 'use' token */
            size_t ui = useEnd;
            while (ui > 0 && tokens[ui].kind != TK::KW_USE) ui--;
            /* tokens[ui] = 'use', tokens[ui+1] = 'constant', tokens[ui+2..useEnd-1] = definition */
            size_t defStart = ui + 2;
            size_t defEnd   = useEnd; /* exclusive */

            auto emitConstSub = [&](const std::string &cname, const std::vector<Token> &valTokens) {
                /* inject: sub CNAME { return VALUE; } */
                constToks.push_back({TK::KW_SUB,    "sub",    0});
                constToks.push_back({TK::IDENT,     cname,    0});
                constToks.push_back({TK::LBRACE,    "{",      0});
                constToks.push_back({TK::KW_RETURN, "return", 0});
                for (const auto &vt : valTokens) constToks.push_back(vt);
                constToks.push_back({TK::SEMI,      ";",      0});
                constToks.push_back({TK::RBRACE,    "}",      0});
                /* also record in constMap so bare NAME (without parens) resolves */
                if (constMap && !valTokens.empty() && parser) {
                    /* pre-parse the value expression to store an AST node */
                    auto parsed = Parser::parseExprFromTokens(valTokens);
                    if (parsed) (*constMap)[cname] = std::move(parsed);
                }
            };

            auto extractValueTokens = [&](size_t start, size_t end) -> std::vector<Token> {
                if (start >= end) return {};
                /* if value starts with '(', scan forward to find the statement-ending ';'
                   then use paren-matching to capture the full expression */
                if (tokens[start].kind == TK::LPAREN) {
                    /* find the ';' that ends this use statement */
                    size_t semiEnd = end;
                    for (size_t s = start; s < end; s++) {
                        if (tokens[s].kind == TK::SEMI) { semiEnd = s; break; }
                    }
                    int depth = 1;
                    size_t p = start + 1;
                    while (p < semiEnd && depth > 0) {
                        if (tokens[p].kind == TK::LPAREN) depth++;
                        else if (tokens[p].kind == TK::RPAREN) depth--;
                        p++;
                    }
                    /* p now points past the matching ')' */
                    std::vector<Token> result(tokens.begin() + start, tokens.begin() + p);
                    return result;
                }
                /* simple value: single token */
                return {tokens[start]};
            };

            if (defStart < defEnd && tokens[defStart].kind == TK::LBRACE) {
                /* use constant { NAME => VAL, NAME2 => VAL2, ... } */
                size_t p = defStart + 1;
                while (p < defEnd && tokens[p].kind != TK::RBRACE) {
                    if (tokens[p].kind == TK::IDENT && p+1 < defEnd &&
                        (tokens[p+1].kind == TK::FATARROW || tokens[p+1].kind == TK::COMMA)) {
                        std::string cname = tokens[p].text;
                        p += 2;
                        if (p < defEnd) {
                            auto vtoks = extractValueTokens(p, defEnd);
                            emitConstSub(cname, vtoks);
                            p += vtoks.size();
                            /* skip past ')' if we captured one */
                            if (vtoks.size() > 0 && vtoks.back().kind == TK::RPAREN) p++;
                        }
                    } else p++;
                    if (p < defEnd && tokens[p].kind == TK::COMMA) p++;
                }
            } else if (defStart < defEnd) {
                /* use constant NAME => VALUE  or  use constant NAME VALUE */
                std::string cname = tokens[defStart].text;
                size_t valIdx = defStart + 1;
                if (valIdx < defEnd && (tokens[valIdx].kind == TK::FATARROW ||
                                        tokens[valIdx].kind == TK::COMMA)) valIdx++;
                if (valIdx < defEnd) {
                    auto vtoks = extractValueTokens(valIdx, defEnd);
                    emitConstSub(cname, vtoks);
                }
            }
            continue;
        }

        if (PRAGMAS.count(modName)) continue;

        /* extract explicit import list: use Module qw(...) or use Module ('a','b') */
        /* tokens between modName and SEMI */
        std::vector<std::string> explicitImports;
        {
            size_t p = /* skip 'use' and modName, both already consumed */ 0;
            /* find the IDENT token for modName in the original stream — it's at useEnd-? */
            /* we need position of first token after modName in this use stmt */
            /* walk back from useEnd to find modName */
            size_t mnIdx = useEnd;
            while (mnIdx > 0 && tokens[mnIdx].text != modName) mnIdx--;
            size_t afterMod = mnIdx + 1;
            if (afterMod < useEnd)
                explicitImports = extractQw(tokens, afterMod, useEnd);
        }

        /* convert Foo::Bar → Foo/Bar.pm */
        std::string modPath = modName;
        for (char &c : modPath) if (c == ':') c = '/';
        while (modPath.find("//") != std::string::npos)
            modPath.replace(modPath.find("//"), 2, "/");
        modPath += ".pm";

        std::vector<std::string> searchDirs = {
            baseDir,
            baseDir + "/lib",
            "lib",
            "lib/lib/perl5",
            "."
        };

        /* if module already loaded, only process explicit import list */
        if (loaded.count(modName)) {
            if (!explicitImports.empty()) {
                for (auto &name : explicitImports)
                    importMap[name] = modName + "::" + name;
            }
            continue;
        }

        for (auto &dir : searchDirs) {
            std::string fullPath = dir + "/" + modPath;
            if (access(fullPath.c_str(), R_OK) != 0) continue;

            loaded.insert(modName);
            std::string src = readFile(fullPath);
            Lexer modLexer(src);
            auto modToks = modLexer.tokenize();
            /* strip EOF_TOK so it doesn't terminate the combined stream early */
            if (!modToks.empty() && modToks.back().kind == TK::EOF_TOK)
                modToks.pop_back();
            /* recursively inline modules referenced by this module */
            auto expanded = inlineModules(modToks, dirOf(fullPath), loaded, importMap, constMap, parser);
            /* strip any EOF_TOK from expanded result too */
            if (!expanded.empty() && expanded.back().kind == TK::EOF_TOK)
                expanded.pop_back();
            modTokens.insert(modTokens.end(), expanded.begin(), expanded.end());

            /* build import map from @EXPORT / explicit list */
            auto exports = scanExports(modToks);
            std::vector<std::string> importList;
            if (!explicitImports.empty()) {
                /* explicit: use Module qw(a b) — import those names */
                importList = explicitImports;
            } else {
                /* no list: use @EXPORT by default */
                auto it = exports.find("EXPORT");
                if (it != exports.end()) importList = it->second;
            }
            for (auto &name : importList)
                importMap[name] = modName + "::" + name;

            break;
        }
    }

    /* prepend constant sub definitions */
    if (!constToks.empty()) {
        modTokens.insert(modTokens.begin(), constToks.begin(), constToks.end());
    }

    if (modTokens.empty()) return tokens;

    /* combined: [module tokens + const defs] + synthetic "package main;" + [main tokens] */
    std::vector<Token> result = std::move(modTokens);
    result.push_back({TK::KW_PACKAGE, "package", 0});
    result.push_back({TK::IDENT,      "main",    0});
    result.push_back({TK::SEMI,       ";",       0});
    result.insert(result.end(), tokens.begin(), tokens.end());
    return result;
}

/* Check if the token stream represents a complete statement (ends with semicolon at depth 0) */
static bool isCompleteStatement(const std::vector<Token> &toks) {
    int depth = 0;
    for (size_t i = 0; i < toks.size(); i++) {
        if (toks[i].kind == TK::LPAREN || toks[i].kind == TK::LBRACE || toks[i].kind == TK::LBRACKET)
            depth++;
        else if (toks[i].kind == TK::RPAREN || toks[i].kind == TK::RBRACE || toks[i].kind == TK::RBRACKET)
            depth--;
        else if (depth == 0 && toks[i].kind == TK::SEMI)
            return true;
    }
    return false;
}

/* Run REPL: compile each statement with perlc, subroutines persist between
   statements, and scalar/array/hash variables do not. */
static int runRepl(bool debugSymbols, int optLevel, bool verbose, bool pauseMode) {
    std::cout << "perlc REPL - type 'quit' or 'exit' to exit, 'help' for commands\n";
    std::cout << "Enter complete statements (end with ;).\n";
    std::cout << "Note: Subroutines persist. Scalars/arrays/hashes do not persist between statements.\n\n";

    std::string accum;
    std::vector<NodePtr> savedSubs;
    int stmtCount = 0;

    while (true) {
        std::cout << "perlc> ";
        std::string line;
        if (!std::getline(std::cin, line)) {
            std::cout << "\nExiting REPL.\n";
            break;
        }

        if (line == "quit" || line == "exit" || line == "q") {
            std::cout << "Goodbye!\n";
            break;
        }
        if (line == "help" || line == "h" || line == "?") {
            std::cout << "Commands:\n"
                      << "  quit/exit/q  - exit the REPL\n"
                      << "  help/h/?     - show this help\n"
                      << "  clear        - clear accumulated subroutines\n"
                      << "  dump         - show accumulated subroutines\n"
                      << "  stats        - show REPL stats\n"
                      << "  perl <code>  - execute raw Perl code directly\n"
                      << "Enter Perl statements - subroutines persist, scalars do not.\n";
            continue;
        }
        if (line == "clear") {
            accum.clear();
            savedSubs.clear();
            stmtCount = 0;
            std::cout << "State cleared.\n";
            continue;
        }
        if (line == "dump") {
            std::cout << "=== Accumulated subroutines ===\n";
            for (auto &sub : savedSubs) {
                std::cout << "sub " << sub->name << " { ... }\n";
            }
            std::cout << "==============================\n";
            continue;
        }
        if (line == "stats") {
            std::cout << "Statements executed: " << stmtCount << "\n";
            std::cout << "Subroutines defined: " << savedSubs.size() << "\n";
            continue;
        }
        if (line.substr(0, 5) == "perl ") {
            std::string code = line.substr(5);
            std::string cmd = "perl -e '" + code + "' 2>&1";
            system(cmd.c_str());
            continue;
        }

        if (!accum.empty()) accum += "\n";
        accum += line;

        /* Check for complete statement */
        int depth = 0;
        bool complete = false;
        for (size_t i = 0; i < accum.size(); i++) {
            if (accum[i] == '(' || accum[i] == '[' || accum[i] == '{') depth++;
            else if (accum[i] == ')' || accum[i] == ']' || accum[i] == '}') depth--;
            else if (depth == 0 && accum[i] == ';') { complete = true; break; }
        }

        if (!complete) {
            std::cout << "  ... " << std::flush;
            continue;
        }

        stmtCount++;

        /* Compile and run with perlc */
        try {
            std::set<std::string> loaded;
            std::map<std::string,std::string> importMap;
            std::map<std::string,NodePtr> constMap;
            std::vector<Token> dummyTokens;
            Parser dummyParser(std::move(dummyTokens));
            auto expanded = inlineModules(
                Lexer(accum).tokenize(), ".", loaded, importMap, &constMap, &dummyParser);

            Parser parser(std::move(expanded));
            parser.setImportMap(std::move(importMap));
            parser.setConstMap(std::move(constMap));
            NodePtr ast = parser.parseProgram();

            NodeList mainStmts;
            for (auto &stmt : ast->args) {
                if (stmt->kind == NK::SubDef) {
                    bool found = false;
                    for (auto &s : savedSubs) {
                        if (s->name == stmt->name) { found = true; break; }
                    }
                    if (!found) savedSubs.push_back(std::move(stmt));
                } else {
                    mainStmts.push_back(std::move(stmt));
                }
            }

            NodeList allStmts;
            for (auto &sub : savedSubs) allStmts.push_back(std::move(sub));
            for (auto &stmt : mainStmts) allStmts.push_back(std::move(stmt));

            NodePtr fullAst = makeBlock(std::move(allStmts), 1);

            CodeGen cg(debugSymbols, optLevel);
            cg.compile(*fullAst, "repl");

            std::string tmpIR = "/tmp/_perlc_repl_" + std::to_string(getpid()) + ".ll";
            cg.writeIR(tmpIR);

            std::string rtSrc;
            {
                char self[1024] = {};
                ssize_t len = readlink("/proc/self/exe", self, sizeof(self)-1);
                if (len > 0) {
                    std::string dir(self, len);
                    auto sl = dir.rfind('/');
                    if (sl != std::string::npos) dir = dir.substr(0, sl);
                    rtSrc = dir + "/src/runtime.c";
                }
            }
            if (rtSrc.empty() || access(rtSrc.c_str(), R_OK) != 0)
                rtSrc = "src/runtime.c";

            std::string outFile = "/tmp/_perlc_repl_out_" + std::to_string(getpid());
            std::string cmd = "clang-18 -flto -O" + std::to_string(optLevel) + " -march=native"
                              " -Wno-atomic-alignment";
            if (debugSymbols) cmd += " -g";
            cmd += " " + tmpIR + " " + rtSrc + " -o " + outFile + " -lm -lpcre2-8 -latomic 2>&1";

            int rc = system(cmd.c_str());
            unlink(tmpIR.c_str());

            if (rc == 0) {
                cmd = outFile + " 2>&1";
                FILE *fp = popen(cmd.c_str(), "r");
                if (fp) {
                    char buf[4096];
                    while (fgets(buf, sizeof(buf), fp)) std::cout << buf;
                    pclose(fp);
                }
                unlink(outFile.c_str());
            } else {
                std::cerr << "Compilation failed.\n";
            }

            if (pauseMode) {
                std::cout << "\n--- Press ENTER to continue, 'q' to quit --- ";
                std::string resp;
                if (!std::getline(std::cin, resp)) break;
                if (resp == "q" || resp == "quit" || resp == "exit") {
                    std::cout << "Goodbye!\n";
                    break;
                }
            }

        } catch (const std::exception &e) {
            std::cerr << "Error: " << e.what() << "\n";
        }

        accum.clear();
    }

    return 0;
}

static void usage(const char *prog) {
    std::cerr << "Usage: " << prog << " [options] <file.pl>\n"
              << "Options:\n"
              << "  -o <out>    Output file (default: a.out)\n"
              << "  --emit-ir   Emit LLVM IR (.ll) instead of compiling\n"
              << "  --emit-bc   Emit LLVM bitcode (.bc)\n"
              << "  -O[level]   Optimization level 0-5 (default: 1)\n"
              << "  -v          Verbose\n"
              << "  -pm         Download and install missing Perl modules via cpanm\n"
              << "  -g          Generate debugging symbols\n"
              << "  -i, --repl  Interactive REPL mode (read-eval-print loop)\n"
              << "  -p, --pause Pause after each statement in REPL mode\n";
}

int main(int argc, char **argv) {
    llvm::InitLLVM init(argc, argv);
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    std::string inputFile;
    std::string outputFile = "a.out";
    bool emitIR = false, emitBC = false, verbose = false, installPM = false, debugSymbols = false;
    bool replMode = false, pauseMode = false;
    int optLevel = 2;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--emit-ir"))      emitIR = true;
        else if (!strcmp(argv[i], "--emit-bc")) emitBC = true;
        else if (!strcmp(argv[i], "-v"))        verbose = true;
        else if (!strcmp(argv[i], "-pm"))       installPM = true;
        else if (!strcmp(argv[i], "-g"))         debugSymbols = true;
        else if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "--repl")) replMode = true;
        else if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--pause")) pauseMode = true;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outputFile = argv[++i];
        else if (strncmp(argv[i], "-O", 2) == 0) {
            if (argv[i][2] == '\0') {
                optLevel = 1;
            } else {
                optLevel = atoi(argv[i] + 2);
                if (optLevel < 0 || optLevel > 5) {
                    std::cerr << "Invalid optimization level: " << optLevel << " (must be 0-5)\n";
                    return 1;
                }
            }
        }
        else if (argv[i][0] != '-')             inputFile = argv[i];
        else { usage(argv[0]); return 1; }
    }

    if (inputFile.empty() && !replMode) { usage(argv[0]); return 1; }

    /* REPL mode: interactive read-eval-print loop */
    if (replMode) {
        return runRepl(debugSymbols, optLevel, verbose, pauseMode);
    }

    /* read source */
    std::ifstream f(inputFile);
    if (!f) { std::cerr << "Cannot open: " << inputFile << "\n"; return 1; }
    std::ostringstream buf; buf << f.rdbuf();
    std::string src = buf.str();

    try {
        /* lex */
        Lexer lexer(src);
        auto tokens = lexer.tokenize();

        if (verbose) {
            std::cerr << "[tokens]\n";
            for (auto &t : tokens)
                std::cerr << "  " << t.line << "\t" << t.text << "\n";
        }

        /* install missing modules if -pm flag was specified */
        if (installPM) {
            if (!installMissingModules(tokens, dirOf(inputFile))) {
                std::cerr << "Module installation failed. Cannot continue.\n";
                return 1;
            }
            /* re-lex after installing modules (in case new files were added) */
            Lexer lexer2(src);
            tokens = lexer2.tokenize();
        }

        /* inline any 'use Module' files before parsing; build import map */
        std::set<std::string> loaded;
        std::map<std::string,std::string> importMap;
        std::map<std::string,NodePtr> constMap;
        /* create a dummy parser first (will be rebuilt after inlining) */
        std::vector<Token> dummyTokens;
        Parser parser(std::move(dummyTokens));
        auto expanded = inlineModules(tokens, dirOf(inputFile), loaded, importMap, &constMap, &parser);

        /* parse */
        parser = Parser(std::move(expanded));
        parser.setImportMap(std::move(importMap));
        parser.setConstMap(std::move(constMap));
        auto ast = parser.parseProgram();

        /* codegen */
        CodeGen cg(debugSymbols, optLevel);
        cg.compile(*ast, inputFile);

        if (emitIR) {
            std::string irFile = outputFile == "a.out"
                ? inputFile.substr(0, inputFile.rfind('.')) + ".ll"
                : outputFile;
            cg.writeIR(irFile);
            std::cerr << "IR written to " << irFile << "\n";
            return 0;
        }
        if (emitBC) {
            std::string bcFile = outputFile == "a.out"
                ? inputFile.substr(0, inputFile.rfind('.')) + ".bc"
                : outputFile;
            cg.writeBC(bcFile);
            std::cerr << "BC written to " << bcFile << "\n";
            return 0;
        }

        /* emit IR to temp file, then use clang to link */
        std::string tmpIR = "/tmp/_perlc_" + std::to_string(getpid()) + ".ll";
        std::string rtObj  = "/tmp/_perlc_rt_" + std::to_string(getpid()) + ".o";

        cg.writeIR(tmpIR);

        /* find runtime.c relative to the compiler binary */
        /* or look in same dir as this binary */
        std::string rtSrc;
        {
            /* try to find runtime.c next to the perlc binary */
            char self[1024] = {};
            ssize_t len = readlink("/proc/self/exe", self, sizeof(self)-1);
            if (len > 0) {
                std::string dir(self, len);
                auto sl = dir.rfind('/');
                if (sl != std::string::npos) dir = dir.substr(0, sl);
                rtSrc = dir + "/src/runtime.c";
            }
        }
        if (rtSrc.empty() || access(rtSrc.c_str(), R_OK) != 0)
            rtSrc = "src/runtime.c";  /* fallback: CWD */

        /* locate libperlc_eval.a (same dir as runtime.c) for string eval */
        std::string evalLib;
        if (cg.hasStringEval()) {
            std::string dir = rtSrc.substr(0, rtSrc.rfind('/'));
            std::string candidate = dir + "/libperlc_eval.a";
            if (access(candidate.c_str(), R_OK) == 0)
                evalLib = candidate;
            else
                std::cerr << "Warning: string eval used but libperlc_eval.a not found; "
                             "eval EXPR will return undef\n";
        }

        std::string cmd = "clang-18 -O" + std::to_string(optLevel) + " -march=native"
                          " -Wno-atomic-alignment";
        if (debugSymbols) cmd += " -g";
        cmd += " " + tmpIR + " " + rtSrc;
        if (!evalLib.empty())
            cmd += " -rdynamic"   /* export runtime symbols for JIT dlopen */
                   " -Wl,--whole-archive " + evalLib + " -Wl,--no-whole-archive"
                   " -L/usr/lib/llvm-18/lib -lLLVM-18 -lstdc++ -lpthread -ldl";
        cmd += " -o " + outputFile + " -lm -lpcre2-8 -lsqlite3 -latomic 2>&1";
        if (verbose) std::cerr << "[link] " << cmd << "\n";

        int rc = system(cmd.c_str());
        unlink(tmpIR.c_str());
        if (rc != 0) { std::cerr << "Link failed\n"; return 1; }

        if (verbose) std::cerr << "Binary written to " << outputFile << "\n";

    /* Register runtime cleanup so valgrind reports zero leaks. */
    std::atexit(perl_cleanup);

    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
