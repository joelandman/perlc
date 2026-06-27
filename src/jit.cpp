#include "jit.h"
#include "runtime.h"
#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>

struct PerlJIT::Impl {
    Impl() = default;
};

PerlJIT::PerlJIT() : impl_(std::make_unique<Impl>()) {}
PerlJIT::~PerlJIT() = default;

void PerlJIT::addModule(std::unique_ptr<llvm::Module> mod) {
    addModuleWithContext(std::move(mod), std::make_unique<llvm::LLVMContext>());
}

void PerlJIT::addModuleWithContext(std::unique_ptr<llvm::Module> mod,
                                   std::unique_ptr<llvm::LLVMContext> ctx) {}

void *PerlJIT::getSymbolAddress(const std::string &name) { return nullptr; }

uint64_t PerlJIT::compileAndRun(std::unique_ptr<llvm::Module> mod, const std::string &entryFunc) { return 0; }

llvm::LLVMContext &PerlJIT::getContext() {
    static llvm::LLVMContext dummy;
    return dummy;
}

bool PerlJIT::isReady() const { return false; }

void *PerlJIT::getJIT() { return nullptr; }
