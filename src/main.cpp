/* === BUILD FIX: pull in every standard header we use, in full, at the
   very start of the TU.  This guarantees complete definitions for
   std::string, std::vector, std::ifstream, iterators etc. before any
   project header that might transitively pull LLVM headers.

   The LLVM 18 Orc/ADT headers on this clang-18 + gcc-16 libstdc++ host
   otherwise leave __gnu_cxx::__normal_iterator incomplete for containers.
*/
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <iterator>
#include <utility>
#include <functional>
#include <array>
#include <deque>
#include <list>
#include <queue>
#include <stack>
#include <bitset>
#include <limits>
#include <sstream>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <type_traits>
#include <new>
#include <exception>
#include <stdexcept>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "lexer.h"
#include "parser.h"
#include "codegen.h"   /* real CodeGen - now safe because we build with g++ + force_complete_std.h */
#include "runtime.h"
#include "llvm_early_init.h"

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
        "constant",
        "Math::BigInt","Math::BigInt::GMP","Math::BigFloat",
        "Math::BigRat","bignum","bigint","Math::BigInt::Calc",
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
         Parser *parser = nullptr,
         bool isMainScript = true,
         const std::vector<std::string> &explicitImportNames = {})
{
    /* pragmas that are not files to load */
    static const std::set<std::string> PRAGMAS = {
        "strict","warnings","feature","parent","base",
        "Exporter","Carp","POSIX","Scalar::Util",
        "List::Util","Data::Dumper","Storable","overload",
        "Math::BigInt","Math::BigInt::GMP","Math::BigFloat",
        "Math::BigRat","bignum","bigint","Math::BigInt::Calc",
    };

    std::vector<Token> modTokens;   /* tokens from all inlined modules */
    std::vector<Token> constToks;   /* synthetic constant sub definitions */

    std::vector<std::string> searchDirsBase = {
        baseDir, baseDir + "/lib", "lib", "lib/lib/perl5", "."
    };

    /* D26: `use constant` declared inside an inlined module must not leak
       as a bareword-global sub the way it does for the main script's own
       constants — real Perl only exposes it unqualified when actually
       exported. ownExports/visibleUnqualified capture this module's own
       @EXPORT (default-export set); explicitImportNames (passed down from
       the `use Module qw(...)` call site that pulled this file in)
       overrides the default when non-empty, matching Exporter semantics
       (an explicit import list replaces @EXPORT, it doesn't add to it).
       currentPackage tracks `package NAME;` as we scan, so the always-
       created qualified sub (Package::NAME) names the right package. */
    auto ownExports = scanExports(tokens);
    std::set<std::string> visibleUnqualified;
    if (!explicitImportNames.empty())
        visibleUnqualified.insert(explicitImportNames.begin(), explicitImportNames.end());
    else if (ownExports.count("EXPORT"))
        visibleUnqualified.insert(ownExports["EXPORT"].begin(), ownExports["EXPORT"].end());
    std::string currentPackage = "main";

    for (size_t i = 0; i < tokens.size(); ) {
        if (tokens[i].kind == TK::KW_PACKAGE && i + 1 < tokens.size() &&
            tokens[i+1].kind == TK::IDENT) {
            currentPackage = tokens[i+1].text;
        }
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
                auto expanded = inlineModules(modToks, dirOf(fullPath), loaded, importMap, constMap, parser,
                                               /*isMainScript=*/false, /*explicitImportNames=*/{});
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

            auto emitOneConstSub = [&](const std::string &subName, const std::vector<Token> &valTokens) {
                /* inject: sub SUBNAME { return VALUE; } */
                constToks.push_back({TK::KW_SUB,    "sub",    0});
                constToks.push_back({TK::IDENT,     subName,  0});
                constToks.push_back({TK::LBRACE,    "{",      0});
                constToks.push_back({TK::KW_RETURN, "return", 0});
                for (const auto &vt : valTokens) constToks.push_back(vt);
                constToks.push_back({TK::SEMI,      ";",      0});
                constToks.push_back({TK::RBRACE,    "}",      0});
            };
            auto emitConstSub = [&](const std::string &cname, const std::vector<Token> &valTokens) {
                if (isMainScript) {
                    /* No cross-package boundary — always create the
                       bareword-global sub (unchanged pre-D26 behavior). */
                    emitOneConstSub(cname, valTokens);
                    if (constMap && !valTokens.empty() && parser) {
                        auto parsed = Parser::parseExprFromTokens(valTokens);
                        if (parsed) (*constMap)[cname] = std::move(parsed);
                    }
                    return;
                }
                /* D26: constant declared inside an inlined module. Always
                   create the fully-qualified sub so explicit qualification
                   (Package::NAME) works, matching real Perl. Only ALSO
                   create the unqualified bareword-global sub when the name
                   is actually exported — real Perl constants are not
                   auto-exported just by being declared. */
                emitOneConstSub(currentPackage + "::" + cname, valTokens);
                if (visibleUnqualified.count(cname)) {
                    emitOneConstSub(cname, valTokens);
                    /* also record in constMap so bare NAME (without parens) resolves */
                    if (constMap && !valTokens.empty() && parser) {
                        auto parsed = Parser::parseExprFromTokens(valTokens);
                        if (parsed) (*constMap)[cname] = std::move(parsed);
                    }
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

        /* D30: Time::HiRes — built-in (no .pm file), like POSIX/
           Scalar::Util/Carp. Unlike those, `time`/`sleep` are lexer
           keywords with pre-existing (integer-only) builtin behavior, so
           overriding them must be opt-in per real Perl semantics — a
           bare `use Time::HiRes;` with no import list does NOT override
           the builtin time()/sleep() (confirmed against real Perl).
           Populate importMap only for explicitly requested names,
           exactly mirroring the qw(...) list, with no default-export
           fallback; the parser separately checks importMap_ at its
           KW_TIME/KW_SLEEP sites, and codegen recognizes the qualified
           "Time::HiRes::*" call names for all of them (including
           gettimeofday/usleep, which aren't keywords and so work via the
           same always-available bareword dispatch POSIX::floor uses). */
        if (modName == "Time::HiRes") {
            for (auto &name : explicitImports)
                importMap[name] = "Time::HiRes::" + name;
            continue;
        }

        if (PRAGMAS.count(modName)) continue;

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

        /* if module already loaded, only process explicit import list —
           still needs D26-follow-up validation (a second `use Module
           qw(...)` of an already-loaded module with a bogus name must
           fail exactly like the first-load path below does; re-locate
           and re-scan the file for its exports since they weren't
           cached from the first load). */
        if (loaded.count(modName)) {
            if (!explicitImports.empty()) {
                for (auto &dir : searchDirs) {
                    std::string fullPath = dir + "/" + modPath;
                    if (access(fullPath.c_str(), R_OK) != 0) continue;
                    auto exports = scanExports(Lexer(readFile(fullPath)).tokenize());
                    auto exportedElsewhere = [&](const std::string &name) {
                        for (auto &tag : {"EXPORT", "EXPORT_OK"}) {
                            auto it = exports.find(tag);
                            if (it == exports.end()) continue;
                            for (auto &n : it->second) if (n == name) return true;
                        }
                        return false;
                    };
                    for (auto &name : explicitImports) {
                        if (!exportedElsewhere(name) && !constMap->count(name)) {
                            throw std::runtime_error("\"" + name + "\" is not exported by the " +
                                                      modName + " module");
                        }
                    }
                    break;
                }
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
            auto expanded = inlineModules(modToks, dirOf(fullPath), loaded, importMap, constMap, parser,
                                           /*isMainScript=*/false, explicitImports);
            /* strip any EOF_TOK from expanded result too */
            if (!expanded.empty() && expanded.back().kind == TK::EOF_TOK)
                expanded.pop_back();
            modTokens.insert(modTokens.end(), expanded.begin(), expanded.end());

            /* build import map from @EXPORT / explicit list */
            auto exports = scanExports(modToks);
            std::vector<std::string> importList;
            if (!explicitImports.empty()) {
                /* explicit: use Module qw(a b) — import those names.
                   D26 follow-up: validate each requested name is actually
                   in @EXPORT or @EXPORT_OK, matching real Perl's Exporter
                   (confirmed directly: requesting a name that's in
                   neither is a fatal compile-time error, "NAME" is not
                   exported by the Module module — not a runtime warning,
                   since Exporter's import() runs at BEGIN/compile time).
                   Constant names declared via `use constant` inside the
                   module aren't found by scanExports() (it only scans for
                   literal `our @EXPORT[_OK] = ...` arrays) — those are
                   validated separately, inside the module's own recursive
                   inlineModules() call, via the D26 fix's
                   visibleUnqualified/explicitImportNames mechanism, so
                   they're deliberately exempted from this check here to
                   avoid rejecting a valid constant import. */
                auto exportedElsewhere = [&](const std::string &name) {
                    for (auto &tag : {"EXPORT", "EXPORT_OK"}) {
                        auto it = exports.find(tag);
                        if (it == exports.end()) continue;
                        for (auto &n : it->second) if (n == name) return true;
                    }
                    return false;
                };
                for (auto &name : explicitImports) {
                    if (!exportedElsewhere(name) && !constMap->count(name)) {
                        throw std::runtime_error("\"" + name + "\" is not exported by the " +
                                                  modName + " module");
                    }
                }
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

/* REPL removed. */


static void usage(const char *prog) {
    std::cerr << "Usage: " << prog << " [options] <file.pl>\n"
              << "Options:\n"
              << "  -o <out>    Output file (default: a.out)\n"
              << "  --emit-ir   Emit LLVM IR (.ll) instead of compiling\n"
              << "  --emit-bc   Emit LLVM bitcode (.bc)\n"
              << "  --do-lib    Emit a do-FILE-loadable shared library instead of an\n"
              << "              executable (internal use — invoked by perl_do_file() at\n"
              << "              runtime to implement `do FILE`, D24)\n"
              << "  -O[level]   Optimization level 0-5 (default: 1)\n"
              << "  -v          Verbose\n"
              << "  -pm         Download and install missing Perl modules via cpanm\n"
              << "  -g          Generate debugging symbols\n"
              << "  --mini-gmp  Confirm mini-gmp is used for Math::BigInt (default; no external GMP)\n";
}

int main(int argc, char **argv) {
    /* LLVM initialization is done in a separate TU (llvm_support.cpp)
       that is compiled with full LLVM headers.  This keeps the main driver
       TU from seeing Orc/ADT headers directly, avoiding the incomplete
       iterator errors on this host. */
    perlc_llvm_early_init(&argc, &argv);

    std::string inputFile;
    std::string outputFile = "a.out";
    bool emitIR = false, emitBC = false, verbose = false, installPM = false, debugSymbols = false;
    bool doLib = false;
    int optLevel = 2;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--emit-ir"))      emitIR = true;
        else if (!strcmp(argv[i], "--emit-bc")) emitBC = true;
        else if (!strcmp(argv[i], "--do-lib"))  doLib = true;
        else if (!strcmp(argv[i], "--mini-gmp")) {
            /* D97: mini-gmp is always compiled into the runtime — this flag
               is a no-op (mini-gmp is the only backend), accepted for
               compatibility and to document the choice.  System GMP is not
               linked, so Math::BigInt always uses mini-gmp. */
            verbose = true;
        }
        else if (!strcmp(argv[i], "-v"))        verbose = true;
        else if (!strcmp(argv[i], "-pm"))       installPM = true;
        else if (!strcmp(argv[i], "-g"))         debugSymbols = true;
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

    if (inputFile.empty()) { usage(argv[0]); return 1; }

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
        cg.compile(*ast, inputFile, doLib);

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

        /* find runtime.c relative to the compiler binary; also record the
           compiler binary's own absolute path (D24: baked into every
           compiled executable via PERLC_SELF_PATH so perl_do_file() can
           re-invoke this same perlc binary at runtime to compile a
           do-FILE target into a loadable shared library). */
        std::string rtSrc, selfPath;
        {
            /* try to find runtime.c next to the perlc binary */
            char self[1024] = {};
            ssize_t len = readlink("/proc/self/exe", self, sizeof(self)-1);
            if (len > 0) {
                selfPath = std::string(self, len);
                std::string dir = selfPath;
                auto sl = dir.rfind('/');
                if (sl != std::string::npos) dir = dir.substr(0, sl);
                rtSrc = dir + "/src/runtime.c";
            }
        }
        if (rtSrc.empty() || access(rtSrc.c_str(), R_OK) != 0)
            rtSrc = "src/runtime.c";  /* fallback: CWD */
        /* D97: mini-gmp source — compiled alongside runtime.c for Math::BigInt.
           Look for it in the same directory as runtime.c. */
        std::string mgmpSrc;
        {
            auto sl = rtSrc.rfind('/');
            std::string dir = (sl != std::string::npos) ? rtSrc.substr(0, sl) : ".";
            mgmpSrc = dir + "/mini-gmp.c";
        }
        if (mgmpSrc.empty() || access(mgmpSrc.c_str(), R_OK) != 0)
            mgmpSrc = "src/mini-gmp.c";  /* fallback: CWD */
        if (selfPath.empty()) selfPath = "perlc";  /* fallback: hope it's on $PATH */

        if (doLib) {
            /* D24: `do FILE`-loadable shared library. Deliberately does NOT
               compile/link runtime.c at all — its perl_* symbol references
               stay undefined in this .so and are resolved at dlopen() time
               against the *loading* process's own already-linked runtime
               (which must have been compiled with -rdynamic, see below).
               This is what makes the do'd file share the same runtime
               state (method dispatch table, $@, PV allocator, etc.) as
               the program that do'd it, instead of getting an isolated
               second copy of every runtime global. */
            std::string cmd = "clang-18 -O" + std::to_string(optLevel) +
                               " -march=native -Wno-atomic-alignment -shared -fPIC";
            if (debugSymbols) cmd += " -g";
            cmd += " " + tmpIR;
            cmd += " -o " + outputFile + " 2>&1";
            if (verbose) std::cerr << "[link] " << cmd << "\n";
            int rc = system(cmd.c_str());
            unlink(tmpIR.c_str());
            if (rc != 0) { std::cerr << "Link failed\n"; return 1; }
            if (verbose) std::cerr << "do-lib written to " << outputFile << "\n";
            return 0;
        }

        /* String-eval (JIT) removed; eval EXPR sets $@ and returns undef. */
        std::string cmd = "clang-18 -O" + std::to_string(optLevel) + " -march=native"
                            " -Wno-atomic-alignment -rdynamic"
                            " -DPERLC_SELF_PATH=\"\\\"" + selfPath + "\\\"\"";
         if (debugSymbols) cmd += " -g";
         cmd += " " + tmpIR + " " + rtSrc + " " + mgmpSrc;
         cmd += " -o " + outputFile + " -lm -lpcre2-8 -lsqlite3 -latomic -ldl 2>&1";
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
