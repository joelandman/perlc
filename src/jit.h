#pragma once
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/Module.h>
#include <memory>
#include <string>

class PerlJIT {
public:
    PerlJIT();
    ~PerlJIT();

    /* Add a module; caller also provides the context it was built in */
    void addModuleWithContext(std::unique_ptr<llvm::Module> mod,
                              std::unique_ptr<llvm::LLVMContext> ctx);
    /* Add a module with a fresh context (for REPL use) */
    void addModule(std::unique_ptr<llvm::Module> mod);

    /* Look up a function by name and get its address */
    void *getSymbolAddress(const std::string &name);

    /* Compile and run a module, returning the result of the last expression */
    uint64_t compileAndRun(
        std::unique_ptr<llvm::Module> mod,
        const std::string &entryFunc = "main");

    /* Get the LLVM context for module construction */
    llvm::LLVMContext &getContext() { return ctx_; }

    /* Check if JIT is initialized */
    bool isReady() const { return jit_ != nullptr; }

    /* Get the LLJIT instance for advanced use */
    llvm::orc::LLJIT *getJIT() { return jit_.get(); }

private:
    llvm::LLVMContext ctx_;
    std::unique_ptr<llvm::orc::LLJIT> jit_;
};

/* Type alias for JIT-compiled function */
using JITFunc = void(*)(int, char**);