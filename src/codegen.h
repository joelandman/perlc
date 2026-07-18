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
#include <unordered_set>
#include <vector>

/* A scope frame maps variable names → alloca (PerlValue*) */
using Scope = std::unordered_map<std::string, llvm::Value *>;

class CodeGen {
public:
    CodeGen(bool debug = false, int optLevel = 0);

    /* asDoLib: emit a `PerlValue *__perlc_do_run(PerlArray*, int)` entry
       point (for a `do FILE`-loadable shared library, D24) instead of a
       normal `main(int,char**)`. See compile()'s definition for details. */
    void compile(const Node &program, const std::string &moduleName, bool asDoLib = false);
    void writeIR(const std::string &path);
    void writeBC(const std::string &path);
    void dumpIR();



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

    /* Step 3/4: systematic opt stage control for diagnosis + re-architecture.
       Disable via PERLC_OPT_DISABLE="flatdouble,allflat,stage31,stage23,..."
       Only Stage 23 (allflat) and Stage 31 (flatdouble) have active gated code.
       Stage 32/33 names are accepted by the gate (for compatibility) but have no bodies.
       Non-gated foundational passes (FLAT_ARRAY 22, DerefAV, FLOAT_PAIR, F64 fastpath,
       sub inlining, TBAA) remain always-on. */
    std::unordered_set<std::string> disabledStages_;
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

    /* Helper for Step 3 diagnosis: returns false if the named stage
        (e.g. "flatdouble", "allflat", "stage31", "stage32", "stage33")
        has been disabled via PERLC_OPT_DISABLE or setDisabledStages. */
    bool isOptStageEnabled(const std::string& raw) const {
        std::string name = raw;
        for (auto &c : name) c = (char)tolower(c);
        if (disabledStages_.count(name)) return false;
        if (name == "stage31" || name == "flatdouble" || name == "flat_double") {
            if (disabledStages_.count("stage31") || disabledStages_.count("flatdouble") || disabledStages_.count("flat_double")) return false;
        }
        if (name == "stage23" || name == "allflat") {
            if (disabledStages_.count("stage23") || disabledStages_.count("allflat")) return false;
        }
        if (name == "stage32" || name == "loopderef" || name == "derefhoist") {
            if (disabledStages_.count("stage32") || disabledStages_.count("loopderef") || disabledStages_.count("derefhoist")) return false;
        }
        if (name == "stage33" || name == "knowntag") {
            if (disabledStages_.count("stage33") || disabledStages_.count("knowntag")) return false;
        }
        return true;
    }

    /* Phase 3: names of shared scalars (declared with `: shared`).  Used
       to route reads/writes through the atomic primitive helpers so the
       codegen for `$x`, `$x = v`, `$x++`, `$x += N` (on a shared scalar)
       calls perl_atomic_load / perl_atomic_store / perl_atomic_inc /
       perl_atomic_dec / perl_atomic_add instead of plain loads and the
       non-atomic perl_assign.  The set is populated in `case NK::My`
       (shared branch) and is read in `case NK::ScalarVar` (rval),
       `case NK::Assign` (ScalarVar LHS), `case NK::UnaryOp` (++/-- on
       shared ScalarVar), and `case NK::CompoundAssign` (numeric op on
       shared ScalarVar). */
    std::unordered_set<std::string> sharedScalarNames_;

    /* file-scope (top-level my) globals — accessible from subroutines.
       fileScalarGlobals_'s value is llvm::Value* (not GlobalVariable*)
       because D58's fix stores an AllocaInst* there instead, in --do-lib
       builds only (see asDoLib_) — populated at runtime from a process-
       wide registry (perl_get_or_create_global_scalar) instead of a
       plain per-compilation-unit GlobalVariable, so a package scalar's
       storage is actually shared across repeated `do` calls on the same
       file. All existing consumers only ever load through the pointer
       generically, so this is a source-compatible widening. */
     std::unordered_map<std::string, llvm::Value *> fileScalarGlobals_;
     /* D78: file-scope variables initialized with integer literals > 2^53.
        These cannot safely use the F64 fast path because converting to double
        would lose precision. Excluded from canEmitF64 for ScalarVar. */
     std::unordered_set<std::string> fileScalarLargeInt_;
    std::unordered_map<std::string, llvm::GlobalVariable *> fileArrayGlobals_;
    std::unordered_map<std::string, llvm::GlobalVariable *> fileHashGlobals_;
    int fileScopeDepth_ = -1;   /* scopes_.size() that corresponds to file scope */
    bool inMainBody_ = false;   /* true only while emitting the top-level program body */
    bool asDoLib_ = false;      /* D24/D58: compiling in --do-lib mode (see compile()) */
    /* Stage 23: when true, all 2D-array rows are known FLAT_ARRAY — skip flat/norm condBrs */
    bool inFlatOnly_ = false;

    /* current function */
    llvm::Function                *currentFn_ = nullptr;
    /* Stage 24a: true when current sub emitted perl_push_wantarray at entry */
    bool                           currentSubNeedsWantarray_ = true;
    /* D64: scalar names captured by some closure (AnonSub, or sort{}'s
       custom comparator) anywhere within the function currently being
       compiled — computed once per function entry (emitSub/AnonSub/the
       top-level program body) via collectClosureCapturedNames(). A `my
       $var = <literal>` declaration whose name is in this set must not
       use the unboxed int/float fast path (intScopes_/floatScopes_,
       which has no real PerlValue* for a closure to later share) even
       though the fast path would otherwise apply — see D64 in TESTS.md. */
    std::unordered_set<std::string> capturedNamesInCurrentFn_;
    /* 0=scalar context, 1=list context — set before emitting call, consumed by emitCall */
    int                            callCtx_ = 0;
    /* body of the currently-emitting named sub (for @_ arg promotion analysis) */
    const Node                    *currentSubBody_ = nullptr;
    /* Stage 25: promotion kind for @_ args identified before sub body emission */
    enum class PPKind { Int, Float, DerefAV };
    std::unordered_map<std::string, PPKind> prePromotedArgs_;
    /* loop control blocks */
    struct LoopLabel { std::string name; llvm::BasicBlock *exit; llvm::BasicBlock *cont; llvm::BasicBlock *redo; };
    std::vector<llvm::BasicBlock *> loopExits_;
    std::vector<llvm::BasicBlock *> loopContinues_;
    std::vector<llvm::BasicBlock *> loopRedos_;  /* redo target = body start */
    std::vector<LoopLabel>          loopLabels_; /* labeled loop support */
    /* `return` inside eval{} targets the nearest enclosing eval block (its
       value becomes the eval's result; execution resumes after the eval,
       NOT the enclosing sub) — real Perl semantics, distinct from die.
       Stack (not a single Value) to support nested eval{}; NK::AnonSub
       saves/clears/restores this the same way it does scopes_ etc., since
       an anon sub's own `return` must NOT be captured by an enclosing
       eval — it targets the anon sub itself. Named subs don't need the
       same save/restore: they're compiled as an entirely separate pass,
       never nested inside this stack's push/pop in the same call frame. */
    struct EvalReturnTarget { llvm::Value *resultAlloca; llvm::BasicBlock *endBB; };
    std::vector<EvalReturnTarget>   evalReturnTargets_;
    /* local() save depth at function entry (alloca holding i32) */
    llvm::Value *localDepthAlloca_ = nullptr;
    /* caller() support: current package name and source file */
    std::string currentPackage_ = "main";
    std::string sourceFile_;

    /* Sub-task 2 (named-sub closure capture): the AST nodes of every
       top-level named sub.  Populated in `compile()` (and the
       eval-string JIT), consumed by `case NK::RefSub` when emitting
       `\&subname` so it can build a closure that captures the
       enclosing scope's shared scalars.  The matching sub body
       emission in `emitSub()` reads `subCaptures_[name]` to know
       which captures to load with `perl_get_capture(i)`. */
    std::vector<const Node *> subs_;
    std::unordered_map<std::string, std::vector<std::string>> subCaptures_;

    /* AST-level inline subs: subs with (my (@params)=@_; return expr) body.
       At call sites these are expanded directly, bypassing @_ construction. */
    struct InlineSub {
        std::vector<std::string> params;  /* param names without $ */
        const Node *bodyExpr;            /* the single return expression */
    };
    std::unordered_map<std::string, InlineSub> inlineSubs_;
    llvm::Value *tryEmitInline(const Node &callNode);  /* returns nullptr if not inlineable */

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
    llvm::Value *emitShortCircuitRhs(const Node &rhsNode); /* ||/&& RHS: real control-flow for `or return`/`and return` (D8a) */
    bool isCallLikeForContext(const Node &n); /* D12: safe to propagate outer list context into this node's own call */
    llvm::Value *emitCall(const Node &n);
    llvm::Value *emitLValue(const Node &n); /* returns alloca */
    /* Recursively resolve (autovivifying every missing intermediate level)
       the container that `node` — a chain of HashElem/ArrayElem/ArrowDeref
       subscripts — refers to. `wantHash` says whether the container at
       node's own level should end up a PerlHash* (true) or PerlArray*
       (false), i.e. what the *next* outer subscript needs. Returns nullptr
       if the root variable isn't found. Used for $h{a}{b}{c}=val chains of
       arbitrary depth (2-level chains happened to work before because they
       bottom out directly at a HashElem/ArrayElem; 3+ levels didn't, because
       the base of the outer ArrowDeref is itself another ArrowDeref, which
       wasn't recursed into). */
    llvm::Value *emitAutovivContainer(const Node &node, bool wantHash);
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
    llvm::Value *emitFlooredMod(llvm::Value *lv, llvm::Value *rv); /* Perl % semantics, not C's truncating SRem */
    llvm::Value *tryEmitI1Cond(const Node &n);  /* i1 for int comparisons, else nullptr */
    llvm::Value *emitIdx(const Node &n);        /* i64 array index without boxing */

    /* Hash key dispatch: use _str variant for literal keys, _sv for dynamic */
    llvm::Value *emitHashGetRef(llvm::Value *hv, const Node &keyNode);
    llvm::Value *emitHashLValueRef(llvm::Value *hv, const Node &keyNode);
    void         emitHashSet(llvm::Value *hv, const Node &keyNode, llvm::Value *val);
    llvm::Value *emitHashExists(llvm::Value *hv, const Node &keyNode);
    llvm::Value *emitHashDelete(llvm::Value *hv, const Node &keyNode);
};
