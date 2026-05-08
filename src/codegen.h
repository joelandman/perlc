#pragma once
#include "ast.h"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

/* A scope frame maps variable names → alloca (PerlValue*) */
using Scope = std::unordered_map<std::string, llvm::Value *>;

class CodeGen {
public:
    CodeGen();

    void compile(const Node &program, const std::string &moduleName);
    void writeIR(const std::string &path);
    void writeBC(const std::string &path);
    void dumpIR();

private:
    llvm::LLVMContext              ctx_;
    std::unique_ptr<llvm::Module>  mod_;
    llvm::IRBuilder<>              builder_;

    /* runtime type: opaque pointer (PerlValue*) */
    llvm::PointerType *perlPtrTy_;
    llvm::PointerType *arrayPtrTy_;

    /* scope stack */
    std::vector<Scope>             scopes_;
    /* array variable scope */
    std::vector<std::unordered_map<std::string, llvm::Value *>> arrayScopes_;
    /* hash variable scope */
    std::vector<std::unordered_map<std::string, llvm::Value *>> hashScopes_;
    /* current function */
    llvm::Function                *currentFn_ = nullptr;
    /* loop control blocks */
    std::vector<llvm::BasicBlock *> loopExits_;
    std::vector<llvm::BasicBlock *> loopContinues_;

    /* runtime function declarations */
    std::unordered_map<std::string, llvm::Function *> rtFuncs_;

    void declareRuntime();
    llvm::Function *getRTFunc(const std::string &name);

    void pushScope();
    void popScope();
    llvm::Value *lookupVar(const std::string &name);
    void declareVar(const std::string &name, llvm::Value *alloca);
    llvm::Value *lookupArray(const std::string &name);
    void declareArray(const std::string &name, llvm::Value *ptr);
    llvm::Value *lookupHash(const std::string &name);
    void declareHash(const std::string &name, llvm::Value *ptr);
    /* returns a PerlArray* Value for things that produce arrays */
    llvm::Value *emitArrayPtr(const Node &n);

    void   emitStmt(const Node &n);
    llvm::Value *emitExpr(const Node &n);
    llvm::Value *emitBlock(const Node &n);
    llvm::Value *emitBinOp(const Node &n);
    llvm::Value *emitCall(const Node &n);
    llvm::Value *emitLValue(const Node &n); /* returns alloca */
    void   emitSub(const Node &n);

    llvm::Value *callRT(const std::string &name,
                        std::initializer_list<llvm::Value *> args);
    llvm::Value *perlUndef();
    llvm::Value *perlInt(long long v);
    llvm::Value *perlFloat(double v);
    llvm::Value *perlStr(const std::string &s);
};
