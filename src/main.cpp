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

        /* parse */
        Parser parser(std::move(tokens));
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
            " -o " + outputFile + " -lm 2>&1";
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
