#pragma once
#include <cstdint>
#include <memory>
#include <string>

// Forward declarations to avoid pulling in LLVM headers that conflict with GCC 15/16
namespace llvm {
    class Module;
    class LLVMContext;
    class orc;
}

class PerlJIT {
public:
    PerlJIT();
    ~PerlJIT();

    void addModuleWithContext(std::unique_ptr<llvm::Module> mod,
                              std::unique_ptr<llvm::LLVMContext> ctx);
    void addModule(std::unique_ptr<llvm::Module> mod);

    void *getSymbolAddress(const std::string &name);

    uint64_t compileAndRun(
        std::unique_ptr<llvm::Module> mod,
        const std::string &entryFunc = "main");

    llvm::LLVMContext &getContext();

    bool isReady() const;

    void *getJIT();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

using JITFunc = void(*)(int, char**);
