#include "jit.h"
#include "runtime.h"
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <iostream>

using namespace llvm;
using namespace orc;

static void handleError(Error err) {
    if (err) {
        std::string errMsg;
        raw_string_ostream rso(errMsg);
        rso << err;
        std::cerr << "JIT Error: " << errMsg << "\n";
    }
}

PerlJIT::PerlJIT() {
    /* Create LLJIT using the builder */
    auto jitBuilder = LLJITBuilder();

    /* Configure the JIT */
    auto tmOrErr = JITTargetMachineBuilder::detectHost();
    if (!tmOrErr) {
        std::cerr << "Could not detect host JIT target\n";
        return;
    }

    jitBuilder.setJITTargetMachineBuilder(std::move(*tmOrErr));

    auto jitOrErr = jitBuilder.create();
    if (!jitOrErr) {
        handleError(jitOrErr.takeError());
        return;
    }

    jit_ = std::move(*jitOrErr);

    /* Expose process symbols (perl_*, pcre2_*, etc.) to JIT-compiled modules */
    auto gen = DynamicLibrarySearchGenerator::GetForCurrentProcess(
        jit_->getDataLayout().getGlobalPrefix());
    if (gen)
        jit_->getMainJITDylib().addGenerator(std::move(*gen));

    std::cerr << "JIT initialized successfully\n";
}

PerlJIT::~PerlJIT() = default;

void PerlJIT::addModule(std::unique_ptr<Module> mod) {
    addModuleWithContext(std::move(mod), std::make_unique<LLVMContext>());
}

void PerlJIT::addModuleWithContext(std::unique_ptr<Module> mod,
                                   std::unique_ptr<LLVMContext> ctx) {
    if (!jit_) {
        std::cerr << "JIT not initialized\n";
        return;
    }

    /* Verify using the module's actual context (must still be alive) */
    if (verifyModule(*mod, &errs())) {
        std::cerr << "Module verification failed\n";
        return;
    }

    /* Hand both module and context to ThreadSafeModule */
    ThreadSafeModule tsm(std::move(mod), std::move(ctx));

    auto err = jit_->addIRModule(std::move(tsm));
    if (err) {
        handleError(std::move(err));
    }
}

void *PerlJIT::getSymbolAddress(const std::string &name) {
    if (!jit_) return nullptr;

    auto addrOrErr = jit_->lookup(name);
    if (!addrOrErr) {
        handleError(addrOrErr.takeError());
        return nullptr;
    }

    return (void *)(uintptr_t)addrOrErr->getValue();
}

uint64_t PerlJIT::compileAndRun(std::unique_ptr<Module> mod, const std::string &entryFunc) {
    if (!jit_) {
        std::cerr << "JIT not initialized\n";
        return 0;
    }

    /* Verify the module */
    if (verifyModule(*mod, &errs())) {
        std::cerr << "Module verification failed\n";
        return 0;
    }

    /* Wrap module in ThreadSafeModule */
    ThreadSafeModule tsm(std::move(mod), std::make_unique<LLVMContext>());

    /* Add the module */
    auto err = jit_->addIRModule(std::move(tsm));
    if (err) {
        handleError(std::move(err));
        return 0;
    }

    /* Look up the entry function */
    auto addrOrErr = jit_->lookup(entryFunc);
    if (!addrOrErr) {
        handleError(addrOrErr.takeError());
        return 0;
    }

    uint64_t entryAddr = addrOrErr->getValue();

    /* Call the entry function (main) */
    using MainFunc = int(*)(int, char**);
    auto mainFunc = (MainFunc)(void *)(uintptr_t)entryAddr;
    int argc = 0;
    char *argv[] = { nullptr };
    mainFunc(argc, argv);

    return entryAddr;
}