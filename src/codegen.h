#pragma once
#include "ast.h"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/MDBuilder.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

/* A scope frame maps variable names → alloca (PerlValue*) */
using Scope = std::unordered_map<std::string, llvm::Value *>;

class CodeGen {
public:
    CodeGen(bool debug = false, int optLevel = 0);

    void compile(const Node &program, const std::string &moduleName);
    /* Compile a Perl snippet for JIT string eval: emits PerlValue *funcName()
       with no init calls; caller handles die via perl_eval_push/setjmp. */
    void compileForEval(const Node &program, const std::string &funcName);
    bool hasStringEval() const { return hasStringEval_; }
    void writeIR(const std::string &path);
    void writeBC(const std::string &path);
    void dumpIR();

    /* Release the module for use with JIT - transfers ownership to caller */
    std::unique_ptr<llvm::Module> releaseModule();
    /* For JIT use: release the LLVMContext so it can be owned by ThreadSafeModule */
    std::unique_ptr<llvm::LLVMContext> releaseContext();

    void initializeDebugInfo(const std::string &sourceFile);
    llvm::DILocation *getDebugLoc(int line, llvm::DIScope *scope = nullptr);

private:
    /* ctx_ is heap-allocated so it can be released to ThreadSafeModule
       while all ctx_ references in this class remain valid. */
    std::unique_ptr<llvm::LLVMContext> ctx_owned_;
    llvm::LLVMContext                 &ctx_;       /* = *ctx_owned_ */
    std::unique_ptr<llvm::Module>      mod_;
    llvm::IRBuilder<>                  builder_;

    bool                           debug_ = false;
    int                            optLevel_ = 0;
    std::unique_ptr<llvm::DIBuilder> dib_;
    llvm::DICompileUnit           *cu_ = nullptr;
    llvm::DIFile                  *file_ = nullptr;
    llvm::DISubprogram            *currentSP_ = nullptr;

    /* runtime type: opaque pointer (PerlValue*) */
    llvm::PointerType *perlPtrTy_;
    llvm::PointerType *arrayPtrTy_;

    /* TBAA access tags — attached to inline GEP+load/store so LLVM can prove
       PerlValue tag/fval stores don't alias PerlArray.elems loads */
    llvm::MDNode *tbaaAvElemsTag_ = nullptr;  /* PerlArray.elems  (ptr  @ offset 0) */
    llvm::MDNode *tbaaPvTagTag_   = nullptr;  /* PerlValue.tag    (i32  @ offset 0) */
    llvm::MDNode *tbaaPvFvalTag_  = nullptr;  /* PerlValue.fval   (f64  @ offset 8) */
    llvm::MDNode *tbaaAvElemTag_      = nullptr;  /* PerlValue* element of elems[] array */
    llvm::MDNode *tbaaFlatDoubleTag_  = nullptr;  /* double element of flat double[] array */
    void setTBAA(llvm::Value *v, llvm::MDNode *tag);

    /* scope stack */
    std::vector<Scope>             scopes_;
    /* array variable scope */
    std::vector<std::unordered_map<std::string, llvm::Value *>> arrayScopes_;
    /* hash variable scope */
    std::vector<std::unordered_map<std::string, llvm::Value *>> hashScopes_;
    /* stable PerlValue* for each my-variable, freed on scope exit */
    std::vector<std::vector<llvm::Value *>> pvScopes_;
    /* unboxed numeric variables: name → double alloca */
    std::vector<std::unordered_map<std::string, llvm::Value *>> floatScopes_;
    /* unboxed integer variables: name → i64 alloca */
    std::vector<std::unordered_map<std::string, llvm::Value *>> intScopes_;
    /* cached PerlArray* for @_ array-ref args: name → ptr alloca (PerlArray*) */
    std::vector<std::unordered_map<std::string, llvm::Value *>> derefAVScopes_;
    /* per-loop row cache: "outerVar\x01indexVar" → ptr alloca (inner PerlArray*) */
    std::vector<std::unordered_map<std::string, llvm::Value *>> rowAVScopes_;
    /* per-loop flat row cache: "outerVar\x01indexVar" → ptr alloca (double*, null if not flat) */
    std::vector<std::unordered_map<std::string, llvm::Value *>> flatRowScopes_;
    /* Stage 23: per-outer-loop allflat pre-check: outerVar → i1 alloca (1 if all rows flat) */
    std::unordered_map<std::string, llvm::Value *> avAllflatSlots_;
    /* Stage 30: for float vars assigned via sqrt(x), track x so that
       v*v can be replaced by x and v*v*v can be replaced by x*v (1 fewer fmul on critical path) */
    llvm::Value *lastSqrtInput_ = nullptr;
    std::unordered_map<std::string, llvm::Value *> floatSqrtOf_;
    /* Stage 31: flat-double read cache: (outerNm\x01idxNm\x01elemIdx) → f64 Value*.
       Eliminates redundant loads like body[j][6] appearing 3× in the velocity-update
       block; invalidated only when the exact (outerNm, idxNm, elemIdx) is written. */
    std::unordered_map<std::string, llvm::Value *> flatDoubleCache_;

    /* file-scope (top-level my) globals — accessible from subroutines */
    std::unordered_map<std::string, llvm::GlobalVariable *> fileScalarGlobals_;
    std::unordered_map<std::string, llvm::GlobalVariable *> fileArrayGlobals_;
    std::unordered_map<std::string, llvm::GlobalVariable *> fileHashGlobals_;
    int fileScopeDepth_ = -1;   /* scopes_.size() that corresponds to file scope */
    bool inMainBody_ = false;   /* true only while emitting the top-level program body */
    /* Stage 23: when true, all 2D-array rows are known FLAT_ARRAY — skip flat/norm condBrs */
    bool inFlatOnly_ = false;
    bool hasStringEval_ = false;

    /* current function */
    llvm::Function                *currentFn_ = nullptr;
    /* Stage 24a: true when current sub emitted perl_push_wantarray at entry */
    bool                           currentSubNeedsWantarray_ = true;
    /* 0=scalar context, 1=list context — set before emitting call, consumed by emitCall */
    int                            callCtx_ = 0;
    /* body of the currently-emitting named sub (for @_ arg promotion analysis) */
    const Node                    *currentSubBody_ = nullptr;
    /* Stage 25: promotion kind for @_ args identified before sub body emission */
    enum class PPKind { Int, Float, DerefAV };
    std::unordered_map<std::string, PPKind> prePromotedArgs_;
    /* loop control blocks */
    std::vector<llvm::BasicBlock *> loopExits_;
    std::vector<llvm::BasicBlock *> loopContinues_;
    std::vector<llvm::BasicBlock *> loopRedos_;  /* redo target = body start */
    /* local() save depth at function entry (alloca holding i32) */
    llvm::Value *localDepthAlloca_ = nullptr;

    /* runtime function declarations */
    std::unordered_map<std::string, llvm::Function *> rtFuncs_;

    void declareRuntime();
    void runOptimization();
    llvm::Function *getRTFunc(const std::string &name);

    void pushScope();
    void popScope();
    llvm::Value *lookupVar(const std::string &name);
    void declareVar(const std::string &name, llvm::Value *alloca);
    llvm::Value *lookupArray(const std::string &name);
    void declareArray(const std::string &name, llvm::Value *ptr);
    llvm::Value *lookupHash(const std::string &name);
    void declareHash(const std::string &name, llvm::Value *ptr);
    void trackPv(llvm::Value *pv);
    void emitScopeCleanup();  /* free all tracked pvs in all active scopes */
    /* returns a PerlArray* Value for things that produce arrays */
    llvm::Value *emitArrayPtr(const Node &n);

    void   emitStmt(const Node &n);
    llvm::Value *emitExpr(const Node &n);
    llvm::Value *emitBlock(const Node &n);
    llvm::Value *emitBlockLast(const Node &n); /* emits block, returns last expr value */
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

    bool isOwnedTemp(llvm::Value *v);
    void freeIfOwned(llvm::Value *v);

    llvm::Value *lookupFloatVar(const std::string &name);
    void         declareFloatVar(const std::string &name, llvm::Value *alloca);
    bool         canEmitF64(const Node &n);
    llvm::Value *emitExprF64(const Node &n);
    llvm::Value *boxF64(llvm::Value *dbl);

    llvm::Value *lookupIntVar(const std::string &name);
    void         declareIntVar(const std::string &name, llvm::Value *alloca);
    llvm::Value *lookupDerefAV(const std::string &name);   /* cached PerlArray* for array-ref @_ args */
    void         declareDerefAV(const std::string &name, llvm::Value *alloca);
    llvm::Value *lookupRowAV(const std::string &outerVar, const std::string &idxVar);
    void         declareRowAV(const std::string &outerVar, const std::string &idxVar, llvm::Value *alloca);
    llvm::Value *lookupFlatRow(const std::string &outerVar, const std::string &idxVar);
    void         declareFlatRow(const std::string &outerVar, const std::string &idxVar, llvm::Value *alloca);
    bool         canEmitI64(const Node &n);
    llvm::Value *emitExprI64(const Node &n);
    llvm::Value *boxI64(llvm::Value *iv);
    llvm::Value *tryEmitI1Cond(const Node &n);  /* i1 for int comparisons, else nullptr */
    llvm::Value *emitIdx(const Node &n);        /* i64 array index without boxing */

    /* Hash key dispatch: use _str variant for literal keys, _sv for dynamic */
    llvm::Value *emitHashGetRef(llvm::Value *hv, const Node &keyNode);
    void         emitHashSet(llvm::Value *hv, const Node &keyNode, llvm::Value *val);
    llvm::Value *emitHashExists(llvm::Value *hv, const Node &keyNode);
    llvm::Value *emitHashDelete(llvm::Value *hv, const Node &keyNode);
};
