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

/* Inline `use Module` by prepending module tokens.
   Pragmas (strict/warnings/feature/parent/base/constant/Exporter/Carp/POSIX/Scalar::Util)
   are left as-is for the parser to handle or skip.
   Returns combined token list: [module tokens...] [main tokens...] */
static std::vector<Token> inlineModules(
        const std::vector<Token> &tokens,
        const std::string &baseDir,
        std::set<std::string> &loaded)
{
    /* pragmas that are not files to load */
    static const std::set<std::string> PRAGMAS = {
        "strict","warnings","feature","parent","base",
        "constant","Exporter","Carp","POSIX","Scalar::Util",
        "List::Util","Data::Dumper","Storable","overload",
    };

    std::vector<Token> modTokens;  /* tokens from all inlined modules */

    for (size_t i = 0; i < tokens.size(); ) {
        if (tokens[i].kind != TK::KW_USE ||
            i + 1 >= tokens.size() ||
            tokens[i+1].kind != TK::IDENT) {
            i++;
            continue;
        }

        std::string modName = tokens[i+1].text;

        /* skip to semicolon */
        size_t j = i + 2;
        while (j < tokens.size() && tokens[j].kind != TK::SEMI) j++;
        i = j < tokens.size() ? j + 1 : j;  /* advance past semicolon */

        if (PRAGMAS.count(modName) || loaded.count(modName)) continue;

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
            auto expanded = inlineModules(modToks, dirOf(fullPath), loaded);
            /* strip any EOF_TOK from expanded result too */
            if (!expanded.empty() && expanded.back().kind == TK::EOF_TOK)
                expanded.pop_back();
            modTokens.insert(modTokens.end(), expanded.begin(), expanded.end());
            break;
        }
    }

    if (modTokens.empty()) return tokens;

    /* combined: [module tokens] + synthetic "package main;" + [main tokens] */
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

        /* inline any 'use Module' files before parsing */
        std::set<std::string> loaded;
        auto expanded = inlineModules(tokens, dirOf(inputFile), loaded);

        /* parse */
        Parser parser(std::move(expanded));
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
