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

/* D113: extra module search directories from -I, PERL5LIB, and
   `use lib "...";` — previously none of these were honored at all;
   the module search path was a fixed relative-directory list. Populated
   once at startup (-I, PERL5LIB) and grown as `use lib` statements are
   seen while scanning the main script and any inlined modules.
   Front-of-list order matches Perl's own @INC semantics (most recently
   added `use lib` / earliest -I wins). */
static std::vector<std::string> g_extraLibDirs;

static std::string dirOf(const std::string &path) {
    auto p = path.rfind('/');
    return p == std::string::npos ? "." : path.substr(0, p);
}

/* Extract qw(...) word list from token stream starting after current position.
   Advances *pos past the closing ')'.  Returns list of words. */
/* D127: a qw()/list export or import name may carry a leading & (or *)
   sigil — an old-style "this is definitely a sub" marker Perl allows in
   export lists, e.g. real Pod::Usage.pm's `our @EXPORT = qw(&pod2usage);`
   — which is never part of the actual symbol name. Strip it so stored
   export names and explicit import-list names compare equal to the
   bareword form callers/importers actually use (`pod2usage`, not
   `&pod2usage`). */
static std::string stripExportSigil(const std::string &w) {
    if (!w.empty() && (w[0] == '&' || w[0] == '*')) return w.substr(1);
    return w;
}

static std::vector<std::string> extractQw(
        const std::vector<Token> &toks, size_t pos, size_t end)
{
    std::vector<std::string> words;
    /* look for QWORDS token or bare LPAREN IDENT... RPAREN */
    while (pos < end) {
        if (toks[pos].kind == TK::QWORDS) {
            /* text is space-separated words */
            std::istringstream ss(toks[pos].text);
            std::string w; while (ss >> w) words.push_back(stripExportSigil(w));
            return words;
        }
        if (toks[pos].kind == TK::LPAREN) {
            pos++;
            while (pos < end && toks[pos].kind != TK::RPAREN) {
                if (toks[pos].kind == TK::IDENT || toks[pos].kind == TK::STRING)
                    words.push_back(stripExportSigil(toks[pos].text));
                pos++;
            }
            return words;
        }
        if (toks[pos].kind == TK::IDENT || toks[pos].kind == TK::STRING) {
            words.push_back(stripExportSigil(toks[pos].text));
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
        "List::Util","Data::Dumper","overload",
        "constant",
        "Math::BigInt","Math::BigInt::GMP","Math::BigFloat",
        "Math::BigRat","bignum","bigint","Math::BigInt::Calc",
        "File::Copy","File::Path","File::Find","File::Temp","Text::Wrap",
        "Storable","JSON::PP","JSON","Time::Piece","Time::Seconds",
        "Text::CSV","Text::CSV_PP","Text::CSV_XS","Hash::Util",
        "Try::Tiny","List::MoreUtils","Term::ANSIColor","Encode",
        "Pod::Usage",
        "FindBin","Symbol","IPC::Open2","IPC::Open3",
        "IO::Handle","IO::File","IO::Socket","IO::Socket::INET","IO::Socket::IP",
        "Socket","MIME::Base64","Digest::MD5","Digest::SHA",
        "Getopt::Std","Text::ParseWords","File::Compare","File::stat",
        "English","if","experimental","PerlIO::scalar",
        "HTTP::Tiny","version","autodie",
        "Term::ReadLine","CGI","MIME::QuotedPrint","Digest","Text::Tabs",
        "FileHandle","IO::Seekable","IO::Pipe","IO::Select","IO::Socket::UNIX",
        "SelectSaver","Fatal","open",
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
   — plus (D122) the older `use vars qw(@EXPORT_OK); @EXPORT_OK = qw(...)`
   style real core modules still use (e.g. File::Path.pm ships this way,
   unmodified, with every Perl 5 install): a bare `@EXPORT[_OK] = ...`
   assignment with no `our` prefix at all, because the array was already
   declared via `use vars` rather than `our`. `our` is optional here for
   exactly that reason — this function doesn't track scope/declarations
   either way (an `our`-prefixed match anywhere in the token stream was
   already accepted regardless of nesting before this fix; the `use
   vars`-declared form is accepted the same way, for consistency).
   Returns map "EXPORT" → [names] and "EXPORT_OK" → [names]. */
static std::map<std::string, std::vector<std::string>>
scanExports(const std::vector<Token> &toks)
{
    std::map<std::string, std::vector<std::string>> result;
    for (size_t i = 0; i + 2 < toks.size(); i++) {
        /* pattern: [our] @EXPORT[_OK] = qw(...) */
        size_t base = i;
        if (toks[base].kind == TK::KW_OUR) base++;
        if (base >= toks.size() || toks[base].kind != TK::ARRAY) continue;
        if (base + 1 >= toks.size()) continue;
        std::string arrName = toks[base + 1].text;
        if (arrName != "EXPORT" && arrName != "EXPORT_OK") continue;
        /* find = */
        size_t j = base + 2;
        while (j < toks.size() && toks[j].kind != TK::ASSIGN && toks[j].kind != TK::SEMI) j++;
        if (j >= toks.size() || toks[j].kind != TK::ASSIGN) continue;
        j++;
        /* find semicolon as end bound */
        size_t end = j;
        while (end < toks.size() && toks[end].kind != TK::SEMI) end++;
        result[arrName] = extractQw(toks, j, end);
    }
    /* Exporter mechanism, phase 1b: %EXPORT_TAGS = (tag => [...], ...).
       Real modules (Pod::Usage, File::Path, ...) declare tag → ref-array
       of names; importers then `use Module qw(:tag)` and Exporter's
       import() expands the tag to that list. Stored under "TAG:<name>"
       keys so the :tag/:all expansion in inlineModules() can find them.
       Values seen in the wild: qw(...), ['...'], ["..."], \@EXPORT_OK,
       and mixed lists. Handles all of them. */
    for (size_t i = 0; i + 2 < toks.size(); i++) {
        size_t base = i;
        if (toks[base].kind == TK::KW_OUR) base++;
        if (base >= toks.size() || toks[base].kind != TK::HASH) continue;
        if (base + 1 >= toks.size()) continue;
        std::string hashName = toks[base + 1].text;
        if (hashName != "EXPORT_TAGS") continue;
        size_t j = base + 2;
        while (j < toks.size() && toks[j].kind != TK::ASSIGN && toks[j].kind != TK::SEMI) j++;
        if (j >= toks.size() || toks[j].kind != TK::ASSIGN) continue;
        j++;
        size_t end = j;
        while (end < toks.size() && toks[end].kind != TK::SEMI) end++;
        /* walk the (tag => LIST, ...) pairs; lists may be qw(...),
           [ ... ], ( ... ) nested, or \@EXPORT_OK-style (skipped — a
           bareword-ref value's names can't be known here; the :all
           expansion handles the common \@EXPORT_OK tag). */
        size_t p = j;
        /* values may reference the scanned export arrays by name:
           `\@EXPORT_OK`, `[@EXPORT_OK]`, `\@EXPORT`. Resolve those AFTER
           the array-scan pass (it runs fully first, above), since only
           then are @EXPORT/@EXPORT_OK known. */
        auto resolveNames = [&](size_t from, size_t end,
                                std::vector<std::string> &out) {
            while (from < end) {
                if (toks[from].kind == TK::QWORDS) {
                    std::istringstream ss(toks[from].text);
                    std::string w;
                    while (ss >> w) out.push_back(stripExportSigil(w));
                } else if (toks[from].kind == TK::BACKSLASH ||
                       toks[from].kind == TK::LBRACKET) {
                    /* \@ARR / [@ARR] / ["a","b"] forms: scan to the
                       matching closer, collecting bareword/STRING names,
                       and resolving a bare EXPORT_OK/EXPORT array ref to
                       the already-scanned array's contents */
                    std::vector<std::string> collected;
                    std::string refArr;
                    from++;
                    int depth = 1;
                    while (from < end && depth > 0) {
                        if (toks[from].text == "[" &&
                            toks[from].kind == TK::LBRACKET) depth++;
                        if (toks[from].text == "]" &&
                            toks[from].kind == TK::RBRACKET) {
                            depth--;
                            if (depth == 0) { from++; break; }
                        }
                        if (depth == 1) {
                            if (toks[from].kind == TK::ARRAY &&
                                from + 1 < end && toks[from + 1].kind == TK::IDENT) {
                                /* @EXPORT_OK lexes as ARRAY("@") + IDENT */
                                refArr = toks[from + 1].text;
                                from++;
                            } else if (toks[from].kind == TK::IDENT ||
                                       toks[from].kind == TK::STRING ||
                                       toks[from].kind == TK::QWORDS) {
                                if (toks[from].kind == TK::QWORDS) {
                                    std::istringstream ss(toks[from].text);
                                    std::string w;
                                    while (ss >> w) collected.push_back(stripExportSigil(w));
                                } else {
                                    collected.push_back(stripExportSigil(toks[from].text));
                                }
                            }
                        }
                        from++;
                    }
                    if (!refArr.empty()) {
                        auto it = result.find(refArr);
                        if (it != result.end())
                            out.insert(out.end(), it->second.begin(), it->second.end());
                    } else {
                        out.insert(out.end(), collected.begin(), collected.end());
                    }
                    return; /* one list value per tag */
                } else if (toks[from].kind == TK::IDENT ||
                           toks[from].kind == TK::STRING) {
                    out.push_back(stripExportSigil(toks[from].text));
                    from++;
                } else {
                    from++;
                }
            }
        };
        while (p < end) {
            /* key: bareword (IDENT or KW_* with text — D136 makes
               `all =>` parse as a string key now) */
            if (toks[p].text.empty() ||
                toks[p].kind == TK::FATARROW || toks[p].kind == TK::COMMA ||
                toks[p].kind == TK::LPAREN || toks[p].kind == TK::RPAREN ||
                toks[p].kind == TK::LBRACKET || toks[p].kind == TK::RBRACKET ||
                toks[p].kind == TK::SEMI || toks[p].kind == TK::EOF_TOK) {
                p++;
                continue;
            }
            std::string tagName = toks[p].text;
            p++;
            if (p >= end || toks[p].kind != TK::FATARROW) continue;
            p++;
            /* value: collect names. A value that references a scanned
               export array by name (\@EXPORT_OK / [@EXPORT_OK]) resolves
               to that array's already-scanned contents (the array pass
               above runs to completion first). */
            std::vector<std::string> names;
            resolveNames(p, end, names);
            while (p < end && toks[p].kind != TK::COMMA) p++;
            result["TAG:" + tagName] = names;
            if (p < end && toks[p].kind == TK::COMMA) p++;
        }
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
    /* pragmas / perlc-internal built-in modules — not files to load, and
       (D113) must never trigger the "module not found" error below even
       though none of them have a corresponding .pm file on disk. This is
       every qualified module name codegen/parser special-case dispatch on
       internally, plus the handful of one-word pragmas perlc currently
       accepts silently (accepted-but-not-fully-modeled, e.g. `integer` —
       deliberately not hard-errored; see D113's write-up in TESTS.md for
       why this stays a conservative allowlist rather than a from-scratch
       Perl-semantics pragma engine). */
    static const std::set<std::string> PRAGMAS = {
        "strict","warnings","feature","parent","base","integer","utf8",
        "vars",
        "Exporter","Carp","POSIX","Scalar::Util",
        "List::Util","Data::Dumper","overload",
        "Math::BigInt","Math::BigInt::GMP","Math::BigFloat",
        "Math::BigRat","bignum","bigint","Math::BigInt::Calc",
        "File::Basename","Getopt::Long","DBI","DBD::SQLite",
        "threads","threads::shared","UNIVERSAL","Time::HiRes",
        "DynaLoader","XSLoader",
        "Cwd","Sys::Hostname","Time::Local",
        "File::Spec","File::Spec::Unix","File::Spec::Functions",
        "File::Copy","File::Path","File::Find","File::Temp","Text::Wrap",
        "Storable","JSON::PP","JSON","Time::Piece","Time::Seconds",
        "Text::CSV","Text::CSV_PP","Text::CSV_XS","Hash::Util",
        "Try::Tiny","List::MoreUtils","Term::ANSIColor","Encode",
        "Pod::Usage",
        "FindBin","Symbol","IPC::Open2","IPC::Open3",
        "IO::Handle","IO::File","IO::Socket","IO::Socket::INET","IO::Socket::IP",
        "Socket","MIME::Base64","Digest::MD5","Digest::SHA",
        "Getopt::Std","Text::ParseWords","File::Compare","File::stat",
        "English","if","experimental","PerlIO::scalar",
        "HTTP::Tiny","version","autodie",
        "Term::ReadLine","CGI","MIME::QuotedPrint","Digest","Text::Tabs",
        "FileHandle","IO::Seekable","IO::Pipe","IO::Select","IO::Socket::UNIX",
        "SelectSaver","Fatal","open",
    };

    std::vector<Token> modTokens;   /* tokens from all inlined modules */
    std::vector<Token> constToks;   /* synthetic constant sub definitions */

    std::vector<std::string> searchDirsBase = {
        baseDir, baseDir + "/lib", "lib", "lib/lib/perl5", "."
    };
    searchDirsBase.insert(searchDirsBase.begin(),
                           g_extraLibDirs.begin(), g_extraLibDirs.end());

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
                /* D128: the module's resolved fullPath is registered and
                   stamped onto its tokens so a parse error inside an
                   inlined module reports the module file's own name and
                   line, not the main script's. Token::file is a stable
                   registry pointer, so the by-value splice below keeps
                   the tag intact. */
                Lexer modLexer(src, fullPath);
                auto modToks = modLexer.tokenize();
                if (!modToks.empty() && modToks.back().kind == TK::EOF_TOK) modToks.pop_back();
                auto expanded = inlineModules(modToks, dirOf(fullPath), loaded, importMap, constMap, parser,
                                               /*isMainScript=*/false, /*explicitImportNames=*/{});
                if (!expanded.empty() && expanded.back().kind == TK::EOF_TOK) expanded.pop_back();
                /* D112 is fixed in codegen.cpp instead (package-qualified
                   global storage keys) — a bare-block wrap here was tried
                   first but breaks named subs' visibility into their own
                   module's file-scope `my` variables, since named subs
                   resolve free variables by name through the global
                   registry, not through genuine lexical closure capture
                   over enclosing blocks. See TESTS.md D112. */
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
            (tokens[i+1].kind != TK::IDENT && tokens[i+1].kind != TK::KW_IF &&
             tokens[i+1].kind != TK::KW_OPEN)) {
            i++;
            continue;
        }

        std::string modName = tokens[i+1].text;

        /* find semicolon end of this use statement */
        size_t j = i + 2;
        while (j < tokens.size() && tokens[j].kind != TK::SEMI) j++;
        size_t useEnd = j;  /* index of SEMI */
        i = j < tokens.size() ? j + 1 : j;  /* advance past semicolon */

        /* use if COND, MODULE, ARGS — compile-time conditional use */
        if (modName == "if") {
            size_t ui = useEnd;
            while (ui > 0 && tokens[ui].kind != TK::KW_USE) ui--;
            size_t p = ui + 2;
            int truth = 0;
            if (p < useEnd) {
                if (tokens[p].kind == TK::INT)
                    truth = strtoll(tokens[p].text.c_str(), nullptr, 10) != 0;
                else if (tokens[p].kind == TK::FLOAT)
                    truth = strtod(tokens[p].text.c_str(), nullptr) != 0.0;
                else if (tokens[p].kind == TK::STRING)
                    truth = !tokens[p].text.empty();
                p++;
                if (p < useEnd && tokens[p].kind == TK::COMMA) p++;
            }
            if (!truth || p >= useEnd) continue;
            modName = tokens[p].text;
            if (!modName.empty() && modName[0] == '\x01') modName = modName.substr(1);
        }

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
            if (ui + 1 < tokens.size() && tokens[ui + 1].text == "if") {
                size_t cp = ui + 2;
                auto tname = [](const Token &t) {
                    std::string s = t.text;
                    if (!s.empty() && s[0] == '\x01') s = s.substr(1);
                    return s;
                };
                while (cp < useEnd && tname(tokens[cp]) != "constant") cp++;
                defStart = cp + 1;
                if (defStart < useEnd && tokens[defStart].kind == TK::COMMA) defStart++;
            }

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
                        auto parsed = Parser::parseExprFromTokens(valTokens, constMap);
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
                        /* W19: pass constMap so the value can reference
                           earlier constants (use constant B => A + 1). */
                        auto parsed = Parser::parseExprFromTokens(valTokens, constMap);
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
                /* value: the full multi-token expression up to `end`
                   (exclusive of the statement-ending ';'). Values such as
                   `-1`, `!!$flag`, `5 - 3`, or `$ENV{X}` are several
                   tokens; capturing only the first one truncated the
                   constant (silently wrong, e.g. `5 - 3` became `5`) or
                   produced an unparseable injected `sub NAME { return !; }`
                   — Encode.pm's `use constant DEBUG => !!$ENV{...}` hit
                   exactly this. */
                return std::vector<Token>(tokens.begin() + start, tokens.begin() + end);
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
                            /* block form: this entry's value ends at the
                               next top-level COMMA or the closing RBRACE —
                               NOT at defEnd (that's the whole block). The
                               multi-token capture below must not swallow
                               the following NAME => VAL pairs. */
                            size_t vEnd = p;
                            int bdepth = 0;
                            while (vEnd < defEnd) {
                                TK k = tokens[vEnd].kind;
                                if (k == TK::LPAREN || k == TK::LBRACKET || k == TK::LBRACE) bdepth++;
                                else if (k == TK::RPAREN || k == TK::RBRACKET || k == TK::RBRACE) {
                                    if (bdepth == 0) break;
                                    bdepth--;
                                }
                                else if (k == TK::COMMA && bdepth == 0) break;
                                vEnd++;
                            }
                            auto vtoks = extractValueTokens(p, vEnd);
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

        /* Time::Piece — unlike Time::HiRes above, real Time::Piece's
           @EXPORT unconditionally overrides localtime/gmtime the moment
           `use Time::Piece;` appears (confirmed against real Perl: no
           import list needed) — so, unlike Time::HiRes, these two map
           regardless of explicitImports. The parser's KW_LOCALTIME/
           KW_GMTIME site checks importMap_ for exactly this pair to
           decide whether scalar-context localtime/gmtime should return a
           blessed Time::Piece object instead of the ctime()-style string;
           list context is unaffected either way. Time::Seconds' 8
           ONE_* constants are @EXPORT too (real module has no
           @EXPORT_OK gate on them). */
        if (modName == "Time::Piece") {
            importMap["localtime"] = "Time::Piece::localtime";
            importMap["gmtime"] = "Time::Piece::gmtime";
            for (auto &name : explicitImports) {
                if (name == "localtime" || name == "gmtime") continue;
                importMap[name] = "Time::Piece::" + name;
            }
            continue;
        }
        if (modName == "Time::Seconds") {
            /* real @Time::Seconds::EXPORT (probed from the host perl,
               v1.41) — all 9 are default-exported, no import list needed. */
            static const char *oneConsts[] = {
                "ONE_MINUTE","ONE_HOUR","ONE_DAY","ONE_WEEK","ONE_MONTH",
                "ONE_YEAR","ONE_FINANCIAL_MONTH","LEAP_YEAR","NON_LEAP_YEAR",
            };
            for (const char *c : oneConsts) importMap[c] = std::string("Time::Seconds::") + c;
            continue;
        }

        /* Tier 1 native modules (Cwd / Sys::Hostname / Time::Local /
           File::Spec::Functions). No .pm file exists for these — the
           functionality is dispatched natively by name in codegen. Just
           map the requested short names to their qualified call names so
           parseCall/parseBareCall resolve them (same model as Time::HiRes
           above). A bare `use Cwd;` needs no importMap entries: Cwd's
           @EXPORT names (cwd/getcwd) are already in codegen's bare-name
           dispatch. */
        if (modName == "Fcntl" || modName == "POSIX" || modName == "Errno") {
            /* Native constant modules (Fcntl/POSIX/Errno): the requested
               names map to qualified constant calls (codegen resolves
               them through the generated value table). Tag specs (:seek,
               :flock, :DEFAULT, ...) expand to the module's real default
               export set — probed from real perl's @EXPORT/@EXPORT_OK and
               the survey-3 constant list; unknown bare names still die
               "not exported" like real Exporter (validated below). */
            static const std::map<std::string, std::vector<std::string>>
                nativeTagExports = {
                {"Fcntl", {
                    /* :DEFAULT = the real @Fcntl::EXPORT set (probed from
                       the host perl; note SEEK_* / LOCK_* are @EXPORT_OK,
                       NOT @EXPORT — matching real Fcntl) */
                    "FD_CLOEXEC","F_ALLOCSP","F_ALLOCSP64","F_COMPAT",
                    "F_DUP2FD","F_DUPFD","F_EXLCK","F_FREESP","F_FREESP64",
                    "F_FSYNC","F_FSYNC64","F_GETFD","F_GETFL","F_GETLK",
                    "F_GETLK64","F_GETOWN","F_NODNY","F_POSIX","F_RDACC",
                    "F_RDDNY","F_RDLCK","F_RWACC","F_RWDNY","F_SETFD",
                    "F_SETFL","F_SETLK","F_SETLK64","F_SETLKW","F_SETLKW64",
                    "F_SETOWN","F_SHARE","F_SHLCK","F_UNLCK","F_UNSHARE",
                    "F_WRACC","F_WRDNY","F_WRLCK",
                    "O_ACCMODE","O_ALIAS","O_APPEND","O_ASYNC","O_BINARY",
                    "O_CREAT","O_DEFER","O_DIRECT","O_DIRECTORY","O_DSYNC",
                    "O_EXCL","O_EXLOCK","O_LARGEFILE","O_NDELAY","O_NOCTTY",
                    "O_NOFOLLOW","O_NOINHERIT","O_NONBLOCK","O_RANDOM",
                    "O_RAW","O_RDONLY","O_RDWR","O_RSRC","O_RSYNC",
                    "O_SEQUENTIAL","O_SHLOCK","O_SYNC","O_TEMPORARY",
                    "O_TEXT","O_TRUNC","O_WRONLY",
                }},
                {"POSIX", {
                    /* real POSIX's %EXPORT tags carry the LC_* constants
                       (probed) */
                    "LC_ALL","LC_COLLATE","LC_CTYPE","LC_NUMERIC",
                    "LC_MONETARY","LC_MESSAGES",
                    "EXIT_SUCCESS","EXIT_FAILURE",
                }},
                {"Errno", {}},
            };
            /* Tags per real module: Fcntl %EXPORT_TAGS
               (:seek => SEEK_*, :flock => LOCK_*, :DEFAULT => @EXPORT,
                :ALL => everything); POSIX similar. */
            static const std::map<std::string,
                std::map<std::string, std::vector<std::string>>> nativeTags = {
                {"Fcntl", {
                    {"seek", {"SEEK_SET","SEEK_CUR","SEEK_END"}},
                    {"flock", {"LOCK_SH","LOCK_EX","LOCK_UN","LOCK_NB"}},
                    {"DEFAULT", {}},   /* overridden above by the DEFAULT branch */
                }},
                {"POSIX", {
                    {"all", {"LC_ALL","LC_COLLATE","LC_CTYPE","LC_NUMERIC",
                             "LC_MONETARY","LC_MESSAGES"}},
                }},
            };
            std::vector<std::string> names = explicitImports;
            bool hasTag = false;
            for (auto &name : names)
                if (!name.empty() && name[0] == ':') hasTag = true;
            if (hasTag) {
                std::vector<std::string> expanded;
                for (auto &name : names) {
                    if (!name.empty() && name[0] == ':') {
                        std::string tag = name.substr(1);
                        if (tag == "DEFAULT" || tag == "all" || tag == "ALL") {
                            for (auto &n2 : nativeTagExports.at(modName))
                                expanded.push_back(n2);
                        } else {
                            auto &tagmap = nativeTags.at(modName);
                            auto it = tagmap.find(tag);
                            if (it == tagmap.end()) {
                                throw std::runtime_error("\"" + name +
                                    "\" is not defined in %EXPORT_TAGS of the " +
                                    modName + " module");
                            }
                            for (auto &n2 : it->second)
                                expanded.push_back(n2);
                        }
                    } else {
                        expanded.push_back(name);
                    }
                }
                names = expanded;
            }
            for (auto &name : names) {
                if (!name.empty() && name[0] == '\x01') name = name.substr(1);
                if (name.empty() || name[0] == ':') continue;
                importMap[name] = modName + "::" + name;
            }
            if (getenv("PERLC_DEBUG_IMPORTS")) {
                fprintf(stderr, "FcntlImport: %s -> %zu names\n", modName.c_str(), names.size());
                for (auto &name : names) fprintf(stderr, "  [%s]\n", name.c_str());
            }
            continue;
        }
        if (modName == "Try::Tiny") {
            std::vector<std::string> names = explicitImports;
            if (names.empty()) names = {"try","catch","finally"};
            for (auto &name : names) {
                if (!name.empty() && name[0] == '\x01') name = name.substr(1);
                if (name.empty() || name[0] == ':') continue;
                importMap[name] = "Try::Tiny::" + name;
            }
            continue;
        }
        if (modName == "Pod::Usage") {
            std::vector<std::string> names = explicitImports;
            if (names.empty()) names = {"pod2usage"};
            for (auto &name : names) {
                if (!name.empty() && name[0] == '&') name = name.substr(1);
                if (!name.empty() && name[0] == '\x01') name = name.substr(1);
                if (name.empty() || name[0] == ':') continue;
                importMap[name] = "Pod::Usage::" + name;
            }
            continue;
        }
        if (modName == "List::MoreUtils" || modName == "Term::ANSIColor" ||
            modName == "Encode" || modName == "Hash::Util") {
            std::vector<std::string> names = explicitImports;
            if (names.empty() && modName == "Term::ANSIColor")
                names = {"color","colored"};
            if (names.empty() && modName == "Encode")
                names = {"decode","decode_utf8","encode","encode_utf8",
                         "str2bytes","bytes2str","encodings","find_encoding",
                         "find_mime_encoding","clone_encoding"};
            if (names.size() == 1 && (names[0] == ":all" || names[0] == ":DEFAULT")) {
                if (modName == "Encode")
                    names = {"decode","decode_utf8","encode","encode_utf8",
                             "encodings","find_encoding","from_to","is_utf8"};
            }
            for (auto &name : names) {
                if (!name.empty() && name[0] == '\x01') name = name.substr(1);
                if (name.empty() || name[0] == ':') continue;
                importMap[name] = modName + "::" + name;
            }
            continue;
        }
        if (modName == "Cwd" || modName == "Sys::Hostname" ||
            modName == "Time::Local" || modName == "File::Spec::Functions" ||
            modName == "File::Copy" || modName == "File::Path" ||
            modName == "File::Find" || modName == "File::Temp" ||
            modName == "Text::Wrap" || modName == "Storable" ||
            modName == "JSON::PP" || modName == "JSON") {
            /* File::Spec::Functions' real %EXPORT_TAGS defines
               ALL => [@EXPORT_OK, @EXPORT] — expand :ALL to that union.
               (Real File::Spec::Functions' %EXPORT_TAGS has only ALL.) */
            std::vector<std::string> names = explicitImports;
            if (modName == "File::Spec::Functions") {
                std::vector<std::string> expanded;
                for (auto &name : names) {
                    if (name.size() > 1 && name[0] == ':' &&
                        (name.substr(1) == "ALL" || name.substr(1) == "all")) {
                        /* @EXPORT_OK then @EXPORT, matching %EXPORT_TAGS ALL */
                        for (const char *ok : {"splitpath","splitdir","catpath",
                                               "abs2rel","rel2abs","devnull",
                                               "tmpdir","case_tolerant",
                                               "canonpath","catdir","catfile",
                                               "curdir","rootdir","updir",
                                               "no_upwards","file_name_is_absolute",
                                               "path","join"})
                            expanded.push_back(ok);
                    } else {
                        expanded.push_back(name);
                    }
                }
                names = expanded;
            }
            for (auto &name : names) {
                if (!name.empty() && name[0] == '\x01') name = name.substr(1);
                if (name.empty() || name[0] == ':') continue;
                importMap[name] = modName + "::" + name;
            }
            continue;
        }
        if (modName == "Getopt::Std" || modName == "Text::ParseWords" ||
            modName == "File::Compare" || modName == "File::stat" ||
            modName == "version" || modName == "HTTP::Tiny" ||
            modName == "English" || modName == "experimental" ||
            modName == "autodie" || modName == "PerlIO::scalar" ||
            modName == "Term::ReadLine" || modName == "CGI" ||
            modName == "MIME::QuotedPrint" || modName == "Digest" ||
            modName == "Text::Tabs" || modName == "FileHandle" ||
            modName == "IO::Seekable" || modName == "IO::Pipe" ||
            modName == "IO::Select" || modName == "IO::Socket::UNIX" ||
            modName == "SelectSaver" || modName == "Fatal" || modName == "open") {
            std::vector<std::string> names = explicitImports;
            for (auto &n0 : names)
                if (!n0.empty() && n0[0] == '\x01') n0 = n0.substr(1);
            if (names.size() == 1 && (names[0] == ":standard" || names[0] == ":html" ||
                                      names[0] == "standard" || names[0] == "html")) {
                if (modName == "CGI")
                    names = {"param","header","start_html","end_html","h1","h2","h3","p","br","hr",
                             "escapeHTML","escape","unescape","redirect","cookie","url","self_url",
                             "script_name","path_info","request_method","start_form","end_form",
                             "textfield","submit","hidden","password_field","textarea","div","span",
                             "b","i","ul","li","table","Tr","td","th","a","img"};
            }
            if (names.empty()) {
                if (modName == "Getopt::Std") names = {"getopt","getopts"};
                else if (modName == "Text::ParseWords")
                    names = {"shellwords","quotewords","parse_line"};
                else if (modName == "File::Compare") names = {"compare"};
                else if (modName == "File::stat") names = {"stat","lstat"};
                else if (modName == "version") names = {"qv"};
                else if (modName == "MIME::QuotedPrint") names = {"encode_qp","decode_qp"};
                else if (modName == "Text::Tabs") names = {"expand","unexpand"};
                else if (modName == "IO::Seekable") names = {"SEEK_SET","SEEK_CUR","SEEK_END"};
            }
            for (auto &name : names) {
                if (!name.empty() && name[0] == '\x01') name = name.substr(1);
                if (name.empty() || name[0] == ':') continue;
                importMap[name] = modName + "::" + name;
            }
            continue;
        }
        if (modName == "Symbol" || modName == "IPC::Open2" ||
            modName == "IPC::Open3" || modName == "MIME::Base64" ||
            modName == "Digest::MD5" || modName == "Digest::SHA" ||
            modName == "Socket" || modName == "FindBin") {
            std::vector<std::string> names = explicitImports;
            if (names.empty()) {
                if (modName == "Symbol")
                    names = {"gensym","qualify"};
                else if (modName == "IPC::Open2")
                    names = {"open2"};
                else if (modName == "IPC::Open3")
                    names = {"open3"};
                else if (modName == "MIME::Base64")
                    names = {"encode_base64","decode_base64"};
                else if (modName == "Socket")
                    names = {
                        "AF_INET","AF_INET6","AF_UNIX","AF_UNSPEC",
                        "PF_INET","PF_INET6","PF_UNIX",
                        "SOCK_STREAM","SOCK_DGRAM","SOCK_RAW","SOCK_SEQPACKET",
                        "SOL_SOCKET","SO_REUSEADDR","SO_KEEPALIVE","SO_LINGER",
                        "SO_BROADCAST","SO_OOBINLINE","SO_SNDBUF","SO_RCVBUF",
                        "SO_ERROR","SO_TYPE","SO_REUSEPORT",
                        "SHUT_RD","SHUT_WR","SHUT_RDWR","SOMAXCONN",
                        "MSG_NOSIGNAL","MSG_OOB","MSG_PEEK","MSG_DONTROUTE",
                        "INADDR_ANY","INADDR_BROADCAST","INADDR_LOOPBACK","INADDR_NONE",
                        "sockaddr_family","pack_sockaddr_in","unpack_sockaddr_in",
                        "sockaddr_in","pack_sockaddr_un","unpack_sockaddr_un",
                        "sockaddr_un","inet_aton","inet_ntoa",
                    };
                /* Digest::* and FindBin have empty @EXPORT */
            }
            if (names.size() == 1 && (names[0] == ":all" || names[0] == ":ALL")) {
                if (modName == "MIME::Base64")
                    names = {"encode_base64","decode_base64",
                             "encode_base64url","decode_base64url"};
                else if (modName == "Digest::MD5")
                    names = {"md5","md5_hex","md5_base64"};
                else if (modName == "Digest::SHA")
                    names = {"sha1","sha1_hex","sha1_base64",
                             "sha256","sha256_hex","sha256_base64",
                             "sha384","sha384_hex","sha384_base64",
                             "sha512","sha512_hex","sha512_base64"};
            }
            for (auto &name : names) {
                if (!name.empty() && name[0] == '\x01') name = name.substr(1);
                if (!name.empty() && name[0] == '$') name = name.substr(1);
                if (name.empty() || name[0] == ':') continue;
                importMap[name] = modName + "::" + name;
            }
            continue;
        }

        /* D113: `use lib "path";` — previously silently ignored (parsed
           as an ordinary unresolvable module `use`, then dropped). Now
           actually prepends `path` to the module search path for the
           rest of this compilation (main script + every module inlined
           from here on, including recursively). */
        if (modName == "lib") {
            for (auto imp : explicitImports) {
                if (!imp.empty() && imp[0] == '\x01') imp = imp.substr(1);
                if (imp.empty()) continue;
                bool already = false;
                for (auto &d : g_extraLibDirs) if (d == imp) { already = true; break; }
                if (!already) g_extraLibDirs.insert(g_extraLibDirs.begin(), imp);
            }
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
        searchDirs.insert(searchDirs.begin(), g_extraLibDirs.begin(), g_extraLibDirs.end());

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
                    /* D128: transient tokens for export scanning only —
                       never reach the parser, so the file tag is harmless
                       either way. */
                    auto exports =
                        scanExports(Lexer(readFile(fullPath), fullPath).tokenize());
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

        bool moduleFileFound = false;
        for (auto &dir : searchDirs) {
            std::string fullPath = dir + "/" + modPath;
            if (access(fullPath.c_str(), R_OK) != 0) continue;
            moduleFileFound = true;

            loaded.insert(modName);
            std::string src = readFile(fullPath);
            /* D128: tag this module's tokens with its own file name — see
               the `require` handler above (tryInlineFile) for why. */
            Lexer modLexer(src, fullPath);
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
            /* D112 is fixed in codegen.cpp (package-qualified global
               storage keys) — see the note in the `require` handler
               above for why a token-level block wrap was rejected. */
            modTokens.insert(modTokens.end(), expanded.begin(), expanded.end());

            /* build import map from @EXPORT / explicit list */
            auto exports = scanExports(modToks);
            std::vector<std::string> importList;
            /* Exporter mechanism phase 1b: `:tag` / `:all` import specs.
               Real Perl expands them through Exporter's import() using the
               module's %EXPORT_TAGS (and @EXPORT+@EXPORT_OK for :all). The
               expansion is done HERE, at compile time, matching the
               compile-time-import model everything else already uses. */
            std::vector<std::string> expandedImports;
            bool usedTagExpansion = false;
            {
                auto itTags = exports.find("TAG:all");
                std::vector<std::string> allNames;
                {
                    auto e1 = exports.find("EXPORT");
                    if (e1 != exports.end())
                        allNames.insert(allNames.end(), e1->second.begin(), e1->second.end());
                    auto e2 = exports.find("EXPORT_OK");
                    if (e2 != exports.end())
                        for (auto &n : e2->second)
                            if (std::find(allNames.begin(), allNames.end(), n) == allNames.end())
                                allNames.push_back(n);
                }
                bool allTagDefined = exports.count("TAG:all") != 0;
                for (auto &name : explicitImports) {
                    if (!name.empty() && name[0] == ':') {
                        usedTagExpansion = true;
                        std::string tag = name.substr(1);
                        if (tag == "all") {
                            /* Real Exporter: :all is ONLY special when the
                               module's own %EXPORT_TAGS defines it (verified:
                               a module with @EXPORT but a :all tag over just
                               @EXPORT_OK does NOT pull in @EXPORT names —
                               probed directly against real perl). When
                               :all is NOT defined in %EXPORT_TAGS but the
                               module HAS some %EXPORT_TAGS, real Exporter
                               dies ("is not defined in %EXPORT_TAGS");
                               fall back to the @EXPORT+@EXPORT_OK union
                               ONLY when the module declares no
                               %EXPORT_TAGS at all — the sloppy-module
                               case where real-world scripts expect :all
                               to work anyway. */
                            auto tagIt = exports.find("TAG:all");
                            if (tagIt != exports.end()) {
                                for (auto &n : tagIt->second)
                                    expandedImports.push_back(n);
                            } else if (allTagDefined ||
                                       exports.count("EXPORT") ||
                                       exports.count("EXPORT_OK")) {
                                bool anyTags = false;
                                for (auto &kv : exports)
                                    if (kv.first.rfind("TAG:", 0) == 0) anyTags = true;
                                if (!anyTags) {
                                    for (auto &n : allNames)
                                        expandedImports.push_back(n);
                                } else {
                                    throw std::runtime_error("\"" + name +
                                        "\" is not defined in %EXPORT_TAGS of the " +
                                        modName + " module");
                                }
                            } else {
                                throw std::runtime_error("\"" + name +
                                    "\" is not defined in %EXPORT_TAGS of the " +
                                    modName + " module");
                            }
                        } else {
                            auto it = exports.find("TAG:" + tag);
                            if (it == exports.end())
                                throw std::runtime_error("\"" + name +
                                    "\" is not defined in %EXPORT_TAGS of the " +
                                    modName + " module");
                            for (auto &n : it->second) expandedImports.push_back(n);
                        }
                    } else if (!name.empty() && name[0] == '&') {
                        /* &name form (real Pod::Usage exports qw(&pod2usage)) */
                        expandedImports.push_back(name.substr(1));
                    } else {
                        expandedImports.push_back(name);
                    }
                }
            }
            const std::vector<std::string> &effectiveImports =
                usedTagExpansion ? expandedImports : explicitImports;
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
                   avoid rejecting a valid constant import. Names pulled
                   in by a :tag/:all expansion are validated against the
                   tag's own contents at expansion time above. */
                auto exportedElsewhere = [&](const std::string &name) {
                    for (auto &tag : {"EXPORT", "EXPORT_OK"}) {
                        auto it = exports.find(tag);
                        if (it == exports.end()) continue;
                        for (auto &n : it->second) if (n == name) return true;
                    }
                    if (std::find(effectiveImports.begin(),
                                  effectiveImports.end(), name) !=
                        effectiveImports.end())
                        return true;
                    return false;
                };
                for (auto &name : effectiveImports) {
                    if (!exportedElsewhere(name) && !constMap->count(name)) {
                        throw std::runtime_error("\"" + name + "\" is not exported by the " +
                                                  modName + " module");
                    }
                }
                importList = effectiveImports;
            } else {
                /* no list: use @EXPORT by default */
                auto it = exports.find("EXPORT");
                if (it != exports.end()) importList = it->second;
            }
            for (auto &name : importList)
                importMap[name] = modName + "::" + name;

            break;
        }

        /* D113: a `use Some::Module;` that resolves to neither a known
           perlc built-in (PRAGMAS, checked above) nor an actual .pm file
           anywhere in the search path used to be silently dropped —
           the exact mechanism that let three completely-unimplemented
           modules (Getopt::Long/Data::Dumper/File::Basename) go
           unnoticed until a human manually diffed output. Now a hard
           compile error, matching real Perl's "Can't locate Foo/Bar.pm
           in @INC". Deliberately scoped to "::"-qualified names only —
           a handful of one-word pragmas (see PRAGMAS above) are still
           accepted-but-not-fully-modeled rather than risking false
           positives on pragma-shaped names this pass doesn't know about. */
        if (!moduleFileFound && modName.find("::") != std::string::npos) {
            std::string incList;
            for (auto &d : searchDirs) incList += (incList.empty() ? "" : " ") + d;
            throw std::runtime_error("Can't locate " + modPath +
                                      " in @INC (searched: " + incList + ")");
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
              << "  --eval-lib  Like --do-lib, plus bind outer `my` cells from the eval pad\n"
              << "              (internal use — invoked by perl_eval_string())\n"
              << "  -O[level]   Optimization level 0-5 (default: 1)\n"
              << "  -v          Verbose\n"
              << "  -pm         Download and install missing Perl modules via cpanm\n"
              << "  -I<dir>     Add <dir> to the module search path (also: PERL5LIB, use lib)\n"
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
    bool doLib = false, evalLib = false;
    int optLevel = 2;

    /* D113: PERL5LIB (colon-separated) is honored the same way real perl
       honors it — prepended to the module search path. */
    if (const char *p5lib = getenv("PERL5LIB")) {
        std::string s(p5lib);
        size_t start = 0;
        while (start <= s.size()) {
            size_t colon = s.find(':', start);
            std::string dir = (colon == std::string::npos) ? s.substr(start)
                                                             : s.substr(start, colon - start);
            if (!dir.empty()) g_extraLibDirs.push_back(dir);
            if (colon == std::string::npos) break;
            start = colon + 1;
        }
    }

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--emit-ir"))      emitIR = true;
        else if (!strcmp(argv[i], "--emit-bc")) emitBC = true;
        else if (!strcmp(argv[i], "--do-lib"))  doLib = true;
        else if (!strcmp(argv[i], "--eval-lib")) { doLib = true; evalLib = true; }
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
        else if (!strcmp(argv[i], "-I") && i + 1 < argc) g_extraLibDirs.push_back(argv[++i]);
        else if (strncmp(argv[i], "-I", 2) == 0 && argv[i][2] != '\0')
            g_extraLibDirs.push_back(argv[i] + 2);
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
        /* D128: the main file's name is registered and stamped onto its
           tokens so a parse error can distinguish "in the main script"
           from "inside an inlined module" (whose tokens carry their own
           registered names from inlineModules()). */
        Lexer lexer(src, inputFile);
        auto tokens = lexer.tokenize();
        /* D128: the main file's registered tag — main-script parse errors
           keep the legacy "Parse error line N:" format by comparing the
           erroring token's tag against this pointer. */
        const char *mainFileTag = lexer.sourceName();

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
            Lexer lexer2(src, inputFile);
            tokens = lexer2.tokenize();
            mainFileTag = lexer2.sourceName();
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
        {
            std::map<std::string,std::string> protoMap;
            auto seedProto = [&](const std::string &shortN, const std::string &qual) {
                auto amp = [](const std::string &b) {
                    static const std::set<std::string> a = {
                        "try","catch","finally",
                        "firstidx","first_index","lastidx","last_index",
                        "onlyidx","only_index","indexes","apply",
                        "after","after_incl","before","before_incl",
                        "firstval","first_value","lastval","last_value",
                        "firstres","first_result","lastres","last_result",
                        "onlyval","only_value","part","any","all","none",
                        "notall","one","pairwise","insert_after","true","false",
                    };
                    return a.count(b);
                };
                std::string bare = shortN;
                if (amp(bare)) {
                    protoMap[shortN] = (bare == "try" || bare == "catch" || bare == "finally") ? "&;@" : "&@";
                    protoMap[qual] = protoMap[shortN];
                }
            };
            for (auto &kv : importMap) seedProto(kv.first, kv.second);
            static const char *ansiConsts[] = {
                "CLEAR","RESET","BOLD","DARK","FAINT","ITALIC","UNDERLINE",
                "BLACK","RED","GREEN","YELLOW","BLUE","MAGENTA","CYAN","WHITE",
                "BRIGHT_RED","BRIGHT_GREEN","BRIGHT_YELLOW","BRIGHT_BLUE",
                "ON_BLACK","ON_RED","ON_GREEN","ON_YELLOW","ON_BLUE","ON_MAGENTA",
                "ON_CYAN","ON_WHITE", NULL
            };
            for (int i = 0; ansiConsts[i]; i++) {
                auto it = importMap.find(ansiConsts[i]);
                if (it != importMap.end()) {
                    protoMap[it->first] = "";
                    protoMap[it->second] = "";
                }
            }
            parser.setProtoMap(std::move(protoMap));
        }
        parser.setImportMap(std::move(importMap));
        parser.setConstMap(std::move(constMap));
        /* D128: parse errors keep the legacy main-script format only when
           the erroring token belongs to the main file; tokens tagged with
           any other registered file report that file's own name+line. */
        parser.setMainFileTag(mainFileTag);
        auto ast = parser.parseProgram();

        /* codegen */
        CodeGen cg(debugSymbols, optLevel);
        if (lexer.hasDataSection()) cg.setDataSection(lexer.dataSection());
        cg.setEnglishEnabled(parser.getEnglishEnabled());
        cg.setAutodieEnabled(parser.getAutodieEnabled());
        cg.setOpenStdUtf8(parser.getOpenStdUtf8());
        cg.compile(*ast, inputFile, doLib, evalLib);

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
        std::string nstSrc;
        {
            auto sl = rtSrc.rfind('/');
            std::string dir = (sl != std::string::npos) ? rtSrc.substr(0, sl) : ".";
            nstSrc = dir + "/native_stdlib.c";
        }
        if (nstSrc.empty() || access(nstSrc.c_str(), R_OK) != 0)
            nstSrc = "src/native_stdlib.c";
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
         cmd += " " + tmpIR + " " + rtSrc + " " + mgmpSrc + " " + nstSrc;
         cmd += " -o " + outputFile + " -lm -lpcre2-8 -lsqlite3 -latomic -ldl -lcrypto 2>&1";
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
