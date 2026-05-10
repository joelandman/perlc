#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/TargetSelect.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
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
        std::map<std::string,Token> *constMap = nullptr)
{
    /* pragmas that are not files to load */
    static const std::set<std::string> PRAGMAS = {
        "strict","warnings","feature","parent","base",
        "Exporter","Carp","POSIX","Scalar::Util",
        "List::Util","Data::Dumper","Storable","overload",
    };

    std::vector<Token> modTokens;   /* tokens from all inlined modules */
    std::vector<Token> constToks;   /* synthetic constant sub definitions */

    for (size_t i = 0; i < tokens.size(); ) {
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

            auto emitConstSub = [&](const std::string &cname, const Token &valTok) {
                /* inject: sub CNAME { return VALUE; } */
                constToks.push_back({TK::KW_SUB,    "sub",    0});
                constToks.push_back({TK::IDENT,     cname,    0});
                constToks.push_back({TK::LBRACE,    "{",      0});
                constToks.push_back({TK::KW_RETURN, "return", 0});
                constToks.push_back(valTok);
                constToks.push_back({TK::SEMI,      ";",      0});
                constToks.push_back({TK::RBRACE,    "}",      0});
                /* also record in constMap so bare NAME (without parens) resolves */
                if (constMap) (*constMap)[cname] = valTok;
            };

            if (defStart < defEnd && tokens[defStart].kind == TK::LBRACE) {
                /* use constant { NAME => VAL, NAME2 => VAL2, ... } */
                size_t p = defStart + 1;
                while (p < defEnd && tokens[p].kind != TK::RBRACE) {
                    if (tokens[p].kind == TK::IDENT && p+1 < defEnd &&
                        (tokens[p+1].kind == TK::FATARROW || tokens[p+1].kind == TK::COMMA)) {
                        std::string cname = tokens[p].text;
                        p += 2;
                        if (p < defEnd) { emitConstSub(cname, tokens[p]); p++; }
                    } else p++;
                    if (p < defEnd && tokens[p].kind == TK::COMMA) p++;
                }
            } else if (defStart < defEnd) {
                /* use constant NAME => VALUE  or  use constant NAME VALUE */
                std::string cname = tokens[defStart].text;
                size_t valIdx = defStart + 1;
                if (valIdx < defEnd && (tokens[valIdx].kind == TK::FATARROW ||
                                        tokens[valIdx].kind == TK::COMMA)) valIdx++;
                if (valIdx < defEnd) emitConstSub(cname, tokens[valIdx]);
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
            auto expanded = inlineModules(modToks, dirOf(fullPath), loaded, importMap, constMap);
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

static void usage(const char *prog) {
    std::cerr << "Usage: " << prog << " [options] <file.pl>\n"
              << "Options:\n"
              << "  -o <out>    Output file (default: a.out)\n"
              << "  --emit-ir   Emit LLVM IR (.ll) instead of compiling\n"
              << "  --emit-bc   Emit LLVM bitcode (.bc)\n"
              << "  -v          Verbose\n";
}

int main(int argc, char **argv) {
    llvm::InitLLVM init(argc, argv);
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    std::string inputFile;
    std::string outputFile = "a.out";
    bool emitIR = false, emitBC = false, verbose = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--emit-ir"))      emitIR = true;
        else if (!strcmp(argv[i], "--emit-bc")) emitBC = true;
        else if (!strcmp(argv[i], "-v"))        verbose = true;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outputFile = argv[++i];
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

        /* inline any 'use Module' files before parsing; build import map */
        std::set<std::string> loaded;
        std::map<std::string,std::string> importMap;
        std::map<std::string,Token> constMap;
        auto expanded = inlineModules(tokens, dirOf(inputFile), loaded, importMap, &constMap);

        /* parse */
        Parser parser(std::move(expanded));
        parser.setImportMap(std::move(importMap));
        parser.setConstMap(std::move(constMap));
        auto ast = parser.parseProgram();

        /* codegen */
        CodeGen cg;
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

        std::string cmd =
            "clang-18 -O1 " + tmpIR + " " + rtSrc +
            " -o " + outputFile + " -lm -lpcre2-8 2>&1";
        if (verbose) std::cerr << "[link] " << cmd << "\n";

        int rc = system(cmd.c_str());
        unlink(tmpIR.c_str());
        if (rc != 0) { std::cerr << "Link failed\n"; return 1; }

        if (verbose) std::cerr << "Binary written to " << outputFile << "\n";

    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
