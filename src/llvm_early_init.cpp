/* llvm_early_init.cpp
 * Provides perlc_llvm_early_init using the real LLVM headers.
 * This file is compiled with full LLVM cxxflags (via g++ now).
 */
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/TargetSelect.h>

#include <memory>

static std::unique_ptr<llvm::InitLLVM> g_init;

extern "C" {

void perlc_llvm_early_init(int *pargc, char ***pargv) {
    if (!g_init) {
        g_init = std::make_unique<llvm::InitLLVM>(*pargc, *pargv);
    }
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
}

}
