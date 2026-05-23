#include "codegen.h"
#include "runtime.h"
#include <llvm/IR/Verifier.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <stdexcept>
#include <sstream>
#include <unordered_set>

using namespace llvm;

/* collect all ScalarVar names referenced in a node (skip nested AnonSub/SubDef) */
static void collectAllScalarNames(const Node &n, std::set<std::string> &names) {
    switch (n.kind) {
    case NK::ScalarVar: names.insert(n.name); return;
    case NK::AnonSub:   return;  /* don't recurse — nested closure handles its own captures */
    case NK::SubDef:    return;
    default: break;
    }
    if (n.left)  collectAllScalarNames(*n.left,  names);
    if (n.right) collectAllScalarNames(*n.right, names);
    if (n.cond)  collectAllScalarNames(*n.cond,  names);
    if (n.body)  collectAllScalarNames(*n.body,  names);
    if (n.init)  collectAllScalarNames(*n.init,  names);
    if (n.step)  collectAllScalarNames(*n.step,  names);
    for (auto &b : n.branches) {
        if (b.cond) collectAllScalarNames(*b.cond, names);
        if (b.body) collectAllScalarNames(*b.body, names);
    }
    for (auto &a : n.args) collectAllScalarNames(*a, names);
}

/* mangle Foo::bar → perlsub_Foo__bar for valid LLVM identifiers */
static std::string subLLVMName(const std::string &name) {
    std::string result = name;
    size_t pos;
    while ((pos = result.find("::")) != std::string::npos)
        result.replace(pos, 2, "__");
    return "perlsub_" + result;
}

/* ── construction ────────────────────────────────────────────────────────── */

CodeGen::CodeGen(bool debug, int optLevel)
    : debug_(debug),
      optLevel_(optLevel),
      mod_(std::make_unique<Module>("perlc", ctx_)),
      builder_(ctx_) {
    perlPtrTy_  = PointerType::getUnqual(ctx_);
    arrayPtrTy_ = PointerType::getUnqual(ctx_);
    declareRuntime();
    if (debug_) {
        dib_ = std::make_unique<DIBuilder>(*mod_);
    }
}

/* ── runtime declarations ────────────────────────────────────────────────── */

static FunctionType *makeRT(LLVMContext &ctx, Type *ret,
                             std::initializer_list<Type *> params) {
    return FunctionType::get(ret, SmallVector<Type*>(params), false);
}

void CodeGen::declareRuntime() {
    auto  voidTy = Type::getVoidTy(ctx_);
    auto  i64    = Type::getInt64Ty(ctx_);
    auto  i32    = Type::getInt32Ty(ctx_);
    auto  i8p    = PointerType::getUnqual(ctx_);
    auto  pv     = perlPtrTy_;   /* PerlValue* */
    auto  av     = arrayPtrTy_;  /* PerlArray* */

#define RT(nm, ret, ...) \
    rtFuncs_[nm] = Function::Create( \
        makeRT(ctx_, ret, {__VA_ARGS__}), \
        Function::ExternalLinkage, nm, mod_.get())

    RT("perl_alloc_undef",   pv);
    RT("perl_alloc_int",     pv,  i64);
    RT("perl_alloc_float",   pv,  Type::getDoubleTy(ctx_));
    RT("perl_alloc_string",  pv,  i8p);
    RT("perl_clone",         pv,  pv);
    RT("perl_free",          voidTy, pv);
    RT("perl_assign",        voidTy, pv, pv);
    RT("perl_to_int",        i64, pv);
    RT("perl_to_float",      Type::getDoubleTy(ctx_), pv);
    RT("perl_is_true",       Type::getInt32Ty(ctx_), pv);
    RT("perl_print",         voidTy, pv);
    RT("perl_say",           voidTy, pv);
    RT("perl_print_string",  voidTy, i8p);
    RT("perl_add",           pv,  pv, pv);
    RT("perl_sub",           pv,  pv, pv);
    RT("perl_mul",           pv,  pv, pv);
    RT("perl_div",           pv,  pv, pv);
    RT("perl_mod",           pv,  pv, pv);
    RT("perl_pow",           pv,  pv, pv);
    RT("perl_negate",        pv,  pv);
    RT("perl_concat",        pv,  pv, pv);
    RT("perl_repeat_str",    pv,  pv, pv);
    RT("perl_num_eq",        pv,  pv, pv);
    RT("perl_num_ne",        pv,  pv, pv);
    RT("perl_num_lt",        pv,  pv, pv);
    RT("perl_num_gt",        pv,  pv, pv);
    RT("perl_num_le",        pv,  pv, pv);
    RT("perl_num_ge",        pv,  pv, pv);
    RT("perl_str_eq",        pv,  pv, pv);
    RT("perl_str_ne",        pv,  pv, pv);
    RT("perl_str_lt",        pv,  pv, pv);
    RT("perl_str_gt",        pv,  pv, pv);
    RT("perl_str_le",        pv,  pv, pv);
    RT("perl_str_ge",        pv,  pv, pv);
    RT("perl_not",           pv,  pv);
    RT("perl_and",           pv,  pv, pv);
    RT("perl_or",            pv,  pv, pv);
    RT("perl_inc",           pv,  pv);
    RT("perl_dec",           pv,  pv);
    RT("perl_array_new",     av);
    RT("perl_array_free",    voidTy, av);
    RT("perl_array_push",    voidTy, av, pv);
    RT("perl_array_pop",     pv,  av);
    RT("perl_array_get",     pv,  av, i64);
    RT("perl_array_get_ref", pv,  av, i64);
    RT("perl_array_set",     voidTy, av, i64, pv);
    RT("perl_array_len",     pv,  av);
    /* hash */
    RT("perl_hash_new",      av);   /* reuse av as opaque ptr */
    RT("perl_hash_get_sv",       pv,  av, pv);
    RT("perl_hash_get_sv_ref",   pv,  av, pv);
    RT("perl_hash_set_sv",       voidTy, av, pv, pv);
    RT("perl_hash_exists_sv",    Type::getInt32Ty(ctx_), av, pv);
    RT("perl_hash_delete_sv",    pv,  av, pv);
    /* constant C-string key variants — no strdup overhead */
    auto *strPtrTy = PointerType::get(Type::getInt8Ty(ctx_), 0);
    RT("perl_hash_get_str_ref",  pv,  av, strPtrTy);
    RT("perl_hash_set_str",      voidTy, av, strPtrTy, pv);
    RT("perl_hash_exists_str",   Type::getInt32Ty(ctx_), av, strPtrTy);
    RT("perl_hash_delete_str",   pv,  av, strPtrTy);
    RT("perl_hash_keys",     av,  av);
    RT("perl_hash_values",   av,  av);
    RT("perl_hash_size",     pv,  av);
    RT("perl_hash_from_list",voidTy, av, av);
    RT("perl_array_sort_str",   voidTy, av);
    RT("perl_array_extend",     voidTy, av, av);
    RT("perl_array_extend_hash",voidTy, av, av);
    RT("perl_array_shift",      pv,  av);
    RT("perl_array_unshift",    voidTy, av, pv);
    /* string builtins */
    RT("perl_chomp",       i64,  pv);
    RT("perl_chomp_array", i64,  av);
    RT("perl_length",   pv,   pv);
    RT("perl_substr2",  pv,   pv, pv);
    RT("perl_substr3",  pv,   pv, pv, pv);
    RT("perl_join",     pv,   pv, av);
    RT("perl_split",    av,   pv, pv);
    /* references */
    RT("perl_ref_scalar",   pv, pv);
    RT("perl_ref_array",    pv, av);
    RT("perl_ref_hash",     pv, av);  /* PerlHash* treated as opaque av */
    RT("perl_deref_scalar", pv, pv);
    RT("perl_deref_array",  av, pv);
    RT("perl_deref_hash",   av, pv);  /* returns PerlHash* as opaque av */
    RT("perl_ref_type",     pv, pv);
    /* file I/O */
    RT("perl_open_fh",          pv,     pv, pv, pv);
    RT("perl_open2_fh",         pv,     pv, pv);
    RT("perl_close_fh",         voidTy, pv);
    RT("perl_readline",         pv,     pv);
    RT("perl_readline_all",     av,     pv);
    RT("perl_readline_stdin",   pv);
    RT("perl_readline_all_stdin", av);
    RT("perl_print_fh",         voidTy, pv, pv);
    RT("perl_say_fh",           voidTy, pv, pv);
    RT("perl_printf_fh",        voidTy, pv, pv, av);
    RT("perl_eof_fh",           pv,     pv);
    RT("perl_die",              voidTy, pv);
    RT("perl_unlink_files",     pv,     av);
    RT("perl_get_stderr",       pv);
    RT("perl_get_stdout",       pv);
    RT("perl_get_stdin",        pv);
    /* sprintf / printf */
    RT("perl_sprintf",      pv, pv, av);
    RT("perl_printf",       voidTy, pv, av);
    /* range */
    RT("perl_range",        av, pv, pv);
    /* math builtins */
    RT("perl_abs_val",      pv, pv);
    RT("perl_int_trunc",    pv, pv);
    RT("perl_sqrt_val",     pv, pv);
    /* string case */
    RT("perl_uc_str",       pv, pv);
    RT("perl_lc_str",       pv, pv);
    RT("perl_ucfirst_str",  pv, pv);
    RT("perl_lcfirst_str",  pv, pv);
    /* string search */
    RT("perl_index_str",    pv, pv, pv, pv);
    RT("perl_rindex_str",   pv, pv, pv, pv);
    /* character conversion */
    RT("perl_chr_val",      pv, pv);
    RT("perl_ord_val",      pv, pv);
    RT("perl_hex_val",      pv, pv);
    RT("perl_oct_val",      pv, pv);
    /* list ops */
    RT("perl_reverse_array",  av, av);
    RT("perl_reverse_str",    pv, pv);
    RT("perl_sort_num_asc",   av, av);
    RT("perl_sort_num_desc",  av, av);
    RT("perl_sort_str_asc",   av, av);
    RT("perl_sort_str_desc",  av, av);
    /* spaceship / cmp */
    RT("perl_spaceship",      pv, pv, pv);
    RT("perl_str_spaceship",  pv, pv, pv);
    /* new builtins */
    RT("perl_chop",       pv, pv);
    RT("perl_warn",       voidTy, pv);
    RT("perl_splice",     av, av, pv, pv, av);
    RT("perl_filetest",   pv, Type::getInt32Ty(ctx_), pv);
    RT("perl_env_get",    pv, pv);
    RT("perl_env_set",    voidTy, pv, pv);
    RT("perl_system",     pv, pv);
    RT("perl_backtick",   pv, pv);
    RT("perl_init_argv",   av, Type::getInt32Ty(ctx_), i8p);
    RT("perl_get_dollar0", pv);
    RT("perl_make_code_ref",  pv, i8p);
    RT("perl_call_code_ref",  pv, pv, av);
    RT("perl_eval_pop",        voidTy);
    RT("perl_get_dollar_at",   pv);
    RT("perl_eval_push",       voidTy, i8p);
    RT("perl_tr",              i64, pv, i8p, i8p, i8p);
    /* setjmp called directly from generated code — must be returns_twice */
    {
        auto *ft = makeRT(ctx_, Type::getInt32Ty(ctx_), {i8p});
        auto *fn = Function::Create(ft, Function::ExternalLinkage,
                                    "setjmp", mod_.get());
        fn->addFnAttr(Attribute::ReturnsTwice);
        rtFuncs_["setjmp"] = fn;
    }
    /* regex */
    RT("perl_regex_match",     pv,  pv, i8p, i8p);
    RT("perl_regex_match_g",   pv,  pv, i8p, i8p);
    RT("perl_regex_match_all", av,  pv, i8p, i8p);
    RT("perl_regex_subst",     i64, pv, i8p, i8p, i8p);
    RT("perl_capture",         pv,  i64);
    RT("perl_split_regex",     av,  i8p, i8p, pv);
    /* OOP */
    RT("perl_bless",                   pv,     pv, pv);
    RT("perl_register_method",         voidTy, i8p, i8p);
    RT("perl_dispatch_method",         pv,     pv, i8p, av);
    RT("perl_dispatch_method_super",   pv,     pv, i8p, i8p, av);
    RT("perl_set_isa",                 voidTy, i8p, i8p);
    /* closures */
    RT("perl_make_closure",  pv, i8p, av);
    RT("perl_get_capture",   pv, i64);
    /* local() */
    RT("perl_local_save_depth", Type::getInt32Ty(ctx_));
    RT("perl_local_save",       voidTy, pv);
    RT("perl_local_restore_to", voidTy, Type::getInt32Ty(ctx_));
    /* special globals */
    RT("perl_get_input_sep",    pv);
    RT("perl_get_dollar_bang",  pv);
    RT("perl_push_wantarray", i32, i32);
RT("perl_pop_wantarray",  i32);
RT("perl_wantarray",      pv);
RT("perl_threads_create", pv, i8p, av);
RT("perl_threads_join",   voidTy, pv);
    RT("perl_caller",           av);
RT("perl_get_plus_hash",     av);
RT("perl_clear_named_captures", voidTy);
    RT("perl_defined",          Type::getInt32Ty(ctx_), pv);
    /* filesystem */
    RT("perl_chdir",            pv, pv);
    RT("perl_mkdir_op",         pv, pv, pv);
    RT("perl_rmdir_op",         pv, pv);
    RT("perl_rename_op",        pv, pv, pv);
    RT("perl_chmod_op",         pv, pv, av);
    /* directory I/O */
    RT("perl_opendir_fh",       pv, pv, pv);
    RT("perl_readdir",          pv, pv);
    RT("perl_readdir_all",      av, pv);
    RT("perl_closedir_fh",      voidTy, pv);
#undef RT
}

Function *CodeGen::getRTFunc(const std::string &nm) {
    auto it = rtFuncs_.find(nm);
    if (it == rtFuncs_.end())
        throw std::runtime_error("Unknown runtime function: " + nm);
    return it->second;
}

/* ── scope management ────────────────────────────────────────────────────── */

void CodeGen::pushScope()  {
    scopes_.emplace_back(); arrayScopes_.emplace_back();
    hashScopes_.emplace_back(); pvScopes_.emplace_back();
    floatScopes_.emplace_back(); intScopes_.emplace_back();
}
void CodeGen::popScope() {
    /* free stable PerlValue*s for my-vars going out of scope, unless in dead block */
    auto *bb = builder_.GetInsertBlock();
    if (bb && !bb->getTerminator() && !pvScopes_.empty()) {
        for (Value *pv : pvScopes_.back())
            callRT("perl_free", {pv});
    }
    scopes_.pop_back(); arrayScopes_.pop_back();
    hashScopes_.pop_back(); pvScopes_.pop_back();
    floatScopes_.pop_back(); intScopes_.pop_back();
}

Value *CodeGen::lookupVar(const std::string &nm) {
    for (int i = (int)scopes_.size() - 1; i >= 0; i--) {
        auto it = scopes_[i].find(nm);
        if (it != scopes_[i].end()) return it->second;
    }
    auto git = fileScalarGlobals_.find(nm);
    if (git != fileScalarGlobals_.end()) return git->second;
    return nullptr;
}

void CodeGen::declareVar(const std::string &nm, Value *a) {
    scopes_.back()[nm] = a;
}

void CodeGen::trackPv(Value *pv) {
    if (!pvScopes_.empty()) pvScopes_.back().push_back(pv);
}

void CodeGen::emitScopeCleanup() {
    for (int i = (int)pvScopes_.size() - 1; i >= 0; i--)
        for (Value *pv : pvScopes_[i])
            callRT("perl_free", {pv});
}

Value *CodeGen::lookupArray(const std::string &nm) {
    for (int i = (int)arrayScopes_.size() - 1; i >= 0; i--) {
        auto it = arrayScopes_[i].find(nm);
        if (it != arrayScopes_[i].end()) return it->second;
    }
    auto git = fileArrayGlobals_.find(nm);
    if (git != fileArrayGlobals_.end())
        return builder_.CreateLoad(perlPtrTy_, git->second, nm);
    return nullptr;
}

void CodeGen::declareArray(const std::string &nm, Value *ptr) {
    arrayScopes_.back()[nm] = ptr;
}

Value *CodeGen::lookupHash(const std::string &nm) {
    for (int i = (int)hashScopes_.size() - 1; i >= 0; i--) {
        auto it = hashScopes_[i].find(nm);
        if (it != hashScopes_[i].end()) return it->second;
    }
    auto git = fileHashGlobals_.find(nm);
    if (git != fileHashGlobals_.end())
        return builder_.CreateLoad(perlPtrTy_, git->second, nm);
    return nullptr;
}

void CodeGen::declareHash(const std::string &nm, Value *ptr) {
    hashScopes_.back()[nm] = ptr;
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

Value *CodeGen::callRT(const std::string &nm,
                       std::initializer_list<Value *> args) {
    auto *fn = getRTFunc(nm);
    SmallVector<Value*> av(args);
    return builder_.CreateCall(fn, av);
}

Value *CodeGen::perlUndef() { return callRT("perl_alloc_undef", {}); }

Value *CodeGen::perlInt(long long v) {
    return callRT("perl_alloc_int",
        {ConstantInt::get(Type::getInt64Ty(ctx_), v, true)});
}

Value *CodeGen::perlFloat(double v) {
    return callRT("perl_alloc_float",
        {ConstantFP::get(Type::getDoubleTy(ctx_), v)});
}

/* Returns a PerlArray* Value for expressions that produce arrays.
   Used by foreach and array-context assignments. */
Value *CodeGen::emitArrayPtr(const Node &n) {
    if (n.kind == NK::ArrayVar) {
        return lookupArray(n.name);
    }
    if (n.kind == NK::KeysFunc) {
        Value *h = lookupHash(n.name);
        if (!h) return callRT("perl_array_new", {});
        Value *av = callRT("perl_hash_keys", {h});
        if (!n.sval.empty()) callRT("perl_array_sort_str", {av}); /* "sort" flag */
        return av;
    }
    if (n.kind == NK::ValuesFunc) {
        Value *h = lookupHash(n.name);
        return h ? callRT("perl_hash_values", {h}) : callRT("perl_array_new", {});
    }
    if (n.kind == NK::SplitFunc) {
        Value *str = n.right ? emitExpr(*n.right) : perlUndef();
        if (n.ival) {
            Value *pat = builder_.CreateGlobalStringPtr(n.sval, "sp_pat");
            Value *flg = builder_.CreateGlobalStringPtr(n.name, "sp_flg");
            return callRT("perl_split_regex", {pat, flg, str});
        }
        Value *sep = n.left  ? emitExpr(*n.left)  : perlStr(" ");
        return callRT("perl_split", {sep, str});
    }
    if (n.kind == NK::SortFunc) {
        /* collect input array */
        Value *av = nullptr;
        if (n.left) av = emitArrayPtr(*n.left);
        if (!av && !n.args.empty()) {
            if (n.args.size() == 1) {
                av = emitArrayPtr(*n.args[0]);
            }
            if (!av) {
                av = callRT("perl_array_new", {});
                for (auto &a : n.args) {
                    Value *sub = emitArrayPtr(*a);
                    if (sub) callRT("perl_array_extend", {av, sub});
                    else     callRT("perl_array_push",   {av, emitExpr(*a)});
                }
            }
        }
        if (!av) av = callRT("perl_array_new", {});
        /* dispatch on sort mode */
        const std::string &mode = n.sval;
        if      (mode == "num_asc")  return callRT("perl_sort_num_asc",  {av});
        else if (mode == "num_desc") return callRT("perl_sort_num_desc", {av});
        else if (mode == "str_asc")  return callRT("perl_sort_str_asc",  {av});
        else if (mode == "str_desc") return callRT("perl_sort_str_desc", {av});
        else { /* default: sort a copy lexicographically */
            Value *copy = callRT("perl_sort_str_asc", {av}); return copy;
        }
    }
    if (n.kind == NK::DerefArray) {
        Value *ref = emitExpr(*n.left);
        return callRT("perl_deref_array", {ref});
    }
    /* NK::AnonArray is a scalar (array ref) — do not flatten it as a list.
       Callers that get nullptr will use emitExpr() which returns perl_ref_array(). */
    if (n.kind == NK::Readline) {
        if (n.sval == "STDIN" || n.sval.empty())
            return callRT("perl_readline_all_stdin", {});
        if (auto *slot = lookupVar(n.sval)) {
            Value *fh = builder_.CreateLoad(perlPtrTy_, slot);
            return callRT("perl_readline_all", {fh});
        }
        return callRT("perl_array_new", {});
    }
    if (n.kind == NK::Range) {
        Value *lo = emitExpr(*n.left);
        Value *hi = emitExpr(*n.right);
        Value *arr = callRT("perl_range", {lo, hi});
        freeIfOwned(lo);
        freeIfOwned(hi);
        return arr;
    }
    if (n.kind == NK::ReverseFunc) {
        /* reverse @arr or reverse LIST — return new reversed array */
        Value *av = nullptr;
        if (n.args.size() == 1) av = emitArrayPtr(*n.args[0]);
        if (!av) {
            av = callRT("perl_array_new", {});
            for (auto &a : n.args) {
                Value *sub = emitArrayPtr(*a);
                if (sub) callRT("perl_array_extend", {av, sub});
                else     callRT("perl_array_push",   {av, emitExpr(*a)});
            }
        }
        return callRT("perl_reverse_array", {av});
    }
    if (n.kind == NK::MapFunc || n.kind == NK::GrepFunc) {
        bool isMap = (n.kind == NK::MapFunc);
        auto *fn   = builder_.GetInsertBlock()->getParent();
        auto *i64  = Type::getInt64Ty(ctx_);
        auto *i32  = Type::getInt32Ty(ctx_);

        /* build input array from args */
        Value *inputArr = nullptr;
        if (n.args.size() == 1) {
            inputArr = emitArrayPtr(*n.args[0]);
            if (!inputArr) {
                inputArr = callRT("perl_array_new", {});
                callRT("perl_array_push", {inputArr, emitExpr(*n.args[0])});
            }
        } else {
            inputArr = callRT("perl_array_new", {});
            for (auto &a : n.args) {
                Value *sub = emitArrayPtr(*a);
                if (sub) callRT("perl_array_extend", {inputArr, sub});
                else     callRT("perl_array_push",   {inputArr, emitExpr(*a)});
            }
        }

        Value *resultArr = callRT("perl_array_new", {});
        Value *lenPv = callRT("perl_array_len", {inputArr});
        Value *len   = callRT("perl_to_int", {lenPv});

        /* $_ alloca (hoisted before loop) */
        auto *udAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, "$_");
        Value *udPv    = perlUndef();
        builder_.CreateStore(udPv, udAlloca);

        auto *iAlloca = builder_.CreateAlloca(i64, nullptr, "mg.i");
        builder_.CreateStore(ConstantInt::get(i64, 0), iAlloca);

        auto *condBB = BasicBlock::Create(ctx_, isMap ? "map.cond" : "grep.cond", fn);
        auto *bodyBB = BasicBlock::Create(ctx_, isMap ? "map.body" : "grep.body", fn);
        auto *exitBB = BasicBlock::Create(ctx_, isMap ? "map.end"  : "grep.end",  fn);

        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        Value *i     = builder_.CreateLoad(i64, iAlloca);
        Value *done  = builder_.CreateICmpSGE(i, len);
        builder_.CreateCondBr(done, exitBB, bodyBB);

        builder_.SetInsertPoint(bodyBB);
        Value *elem = callRT("perl_array_get_ref", {inputArr, i});
        callRT("perl_assign", {udPv, elem});

        /* emit block / expr with $_ in scope */
        pushScope();
        declareVar("_", udAlloca);
        Value *blockResult;
        if (n.body)       blockResult = emitBlockLast(*n.body);
        else if (n.left)  blockResult = emitExpr(*n.left);
        else              blockResult = perlUndef();
        popScope();

        if (isMap) {
            callRT("perl_array_push", {resultArr, blockResult});
            Value *i2 = builder_.CreateAdd(i, ConstantInt::get(i64, 1));
            builder_.CreateStore(i2, iAlloca);
            builder_.CreateBr(condBB);
        } else {
            /* grep: push element if block result is true */
            Value *tv    = callRT("perl_is_true", {blockResult});
            Value *cond  = builder_.CreateICmpNE(tv, ConstantInt::get(i32, 0));
            auto *pushBB = BasicBlock::Create(ctx_, "grep.push", fn);
            auto *nextBB = BasicBlock::Create(ctx_, "grep.next", fn);
            builder_.CreateCondBr(cond, pushBB, nextBB);

            builder_.SetInsertPoint(pushBB);
            callRT("perl_array_push", {resultArr, elem});
            builder_.CreateBr(nextBB);

            builder_.SetInsertPoint(nextBB);
            Value *i2 = builder_.CreateAdd(i, ConstantInt::get(i64, 1));
            builder_.CreateStore(i2, iAlloca);
            builder_.CreateBr(condBB);
        }

        builder_.SetInsertPoint(exitBB);
        return resultArr;
    }
    if (n.kind == NK::RegexMatch && n.name.find('g') != std::string::npos) {
        Value *str = emitExpr(*n.left);
        Value *pat = builder_.CreateGlobalStringPtr(n.sval, "ra_pat");
        Value *flg = builder_.CreateGlobalStringPtr(n.name, "ra_flg");
        return callRT("perl_regex_match_all", {str, pat, flg});
    }
    if (n.kind == NK::SpliceFunc) {
        Value *av = lookupArray(n.name);
        if (!av) return callRT("perl_array_new", {});
        Value *off = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *len = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        /* build replacement array from remaining args */
        Value *repl = callRT("perl_array_new", {});
        for (size_t i = 2; i < n.args.size(); i++)
            callRT("perl_array_push", {repl, emitExpr(*n.args[i])});
        return callRT("perl_splice", {av, off, len, repl});
    }
    if (n.kind == NK::ArraySlice) {
        Value *av  = lookupArray(n.name);
        Value *res = callRT("perl_array_new", {});
        for (auto &idxNode : n.args) {
            Value *elem = av ? callRT("perl_array_get_ref", {av, emitIdx(*idxNode)}) : perlUndef();
            callRT("perl_array_push", {res, elem});
        }
        return res;
    }
    if (n.kind == NK::HashSlice) {
        Value *hv  = lookupHash(n.name);
        Value *res = callRT("perl_array_new", {});
        /* args may be an ArrayLit (from qw() or (list)) — flatten */
        auto pushHashKey = [&](const Node &keyNode) {
            if (keyNode.kind == NK::ArrayLit) {
                for (auto &k : keyNode.args) {
                    Value *elem = hv ? emitHashGetRef(hv, *k) : perlUndef();
                    callRT("perl_array_push", {res, elem});
                }
            } else {
                Value *elem = hv ? emitHashGetRef(hv, keyNode) : perlUndef();
                callRT("perl_array_push", {res, elem});
            }
        };
        for (auto &keyNode : n.args) pushHashKey(*keyNode);
        return res;
    }
    if (n.kind == NK::ArrayLit) {
        Value *res = callRT("perl_array_new", {});
        for (auto &elem : n.args) {
            Value *sub = emitArrayPtr(*elem);
            if (sub) callRT("perl_array_extend", {res, sub});
            else     callRT("perl_array_push",   {res, emitExpr(*elem)});
        }
        return res;
    }
    if (n.kind == NK::CallerFunc) {
        return callRT("perl_caller", {});
    }
    if (n.kind == NK::ReaddirFunc) {
        Value *slot = lookupVar(n.name);
        if (!slot) return callRT("perl_array_new", {});
        Value *dh = builder_.CreateLoad(perlPtrTy_, slot);
        return callRT("perl_readdir_all", {dh});
    }
    return nullptr;
}

Value *CodeGen::perlStr(const std::string &s) {
    auto *gv = builder_.CreateGlobalString(s, ".str");
    return callRT("perl_alloc_string", {gv});
}

/* Returns an i8* global string constant if n is a compile-time string literal,
 * otherwise nullptr. Used to bypass PerlValue key creation for hash ops. */
static llvm::Value *constKeyPtr(const Node &n, llvm::IRBuilder<> &builder) {
    if (n.kind == NK::StringLit || n.kind == NK::IntLit)
        return builder.CreateGlobalStringPtr(
            n.kind == NK::IntLit ? std::to_string(n.ival) : n.sval, ".hk");
    return nullptr;
}

Value *CodeGen::emitHashGetRef(Value *hv, const Node &keyNode) {
    if (Value *kp = constKeyPtr(keyNode, builder_))
        return callRT("perl_hash_get_str_ref", {hv, kp});
    Value *key = emitExpr(keyNode);
    Value *r   = callRT("perl_hash_get_sv_ref", {hv, key});
    freeIfOwned(key);
    return r;
}

void CodeGen::emitHashSet(Value *hv, const Node &keyNode, Value *val) {
    if (Value *kp = constKeyPtr(keyNode, builder_)) {
        callRT("perl_hash_set_str", {hv, kp, val});
        return;
    }
    Value *key = emitExpr(keyNode);
    callRT("perl_hash_set_sv", {hv, key, val});
    freeIfOwned(key);
}

Value *CodeGen::emitHashExists(Value *hv, const Node &keyNode) {
    if (Value *kp = constKeyPtr(keyNode, builder_))
        return callRT("perl_hash_exists_str", {hv, kp});
    Value *key = emitExpr(keyNode);
    Value *r   = callRT("perl_hash_exists_sv", {hv, key});
    freeIfOwned(key);
    return r;
}

Value *CodeGen::emitHashDelete(Value *hv, const Node &keyNode) {
    if (Value *kp = constKeyPtr(keyNode, builder_))
        return callRT("perl_hash_delete_str", {hv, kp});
    Value *key = emitExpr(keyNode);
    Value *r   = callRT("perl_hash_delete_sv", {hv, key});
    freeIfOwned(key);
    return r;
}

bool CodeGen::isOwnedTemp(llvm::Value *v) {
    auto *ci = llvm::dyn_cast<llvm::CallInst>(v);
    if (!ci) return false;
    auto *fn = ci->getCalledFunction();
    if (!fn) return false;
    llvm::StringRef nm = fn->getName();
    /* user-defined subs always return a freshly cloned PerlValue* */
    if (nm.starts_with("perlsub_")) return true;
    static const std::unordered_set<std::string> owned = {
        "perl_alloc_int", "perl_alloc_float", "perl_alloc_string", "perl_alloc_undef",
        "perl_add",    "perl_sub",    "perl_mul",    "perl_div",    "perl_mod",
        "perl_pow",    "perl_negate", "perl_not",    "perl_concat", "perl_repeat_str",
        "perl_num_eq", "perl_num_ne", "perl_num_lt", "perl_num_gt",
        "perl_num_le", "perl_num_ge",
        "perl_str_eq", "perl_str_ne", "perl_str_lt", "perl_str_gt",
        "perl_str_le", "perl_str_ge",
        "perl_spaceship", "perl_str_spaceship",
        "perl_array_get", "perl_hash_get_sv",
        "perl_ref_type", "perl_ref_array", "perl_ref_scalar",
        "perl_clone", "perl_sprintf", "perl_array_len",
        /* single-arg math/string builtins */
        "perl_abs_val", "perl_int_trunc", "perl_sqrt_val",
        "perl_uc_str", "perl_lc_str", "perl_ucfirst_str", "perl_lcfirst_str",
        "perl_chr_val", "perl_ord_val",
        "perl_length", "perl_substr2", "perl_substr3",
        "perl_chop", "perl_index_str", "perl_rindex_str",
        "perl_array_pop", "perl_array_shift",
        "perl_hex_val", "perl_oct_val",
        "perl_defined", "perl_ref_type",
        /* hash/array access */
        "perl_hash_delete_sv",
    };
    return owned.count(nm.str()) > 0;
}

void CodeGen::freeIfOwned(llvm::Value *v) {
    if (isOwnedTemp(v)) callRT("perl_free", {v});
}

/* ── unboxed float helpers ───────────────────────────────────────────────── */

Value *CodeGen::lookupFloatVar(const std::string &name) {
    for (int i = (int)floatScopes_.size() - 1; i >= 0; i--) {
        auto it = floatScopes_[i].find(name);
        if (it != floatScopes_[i].end()) return it->second;
    }
    return nullptr;
}

void CodeGen::declareFloatVar(const std::string &name, Value *alloca) {
    if (!floatScopes_.empty()) floatScopes_.back()[name] = alloca;
}

Value *CodeGen::boxF64(Value *dbl) {
    return callRT("perl_alloc_float", {dbl});
}

/* ── unboxed integer helpers ─────────────────────────────────────────────── */

Value *CodeGen::lookupIntVar(const std::string &name) {
    for (int i = (int)intScopes_.size() - 1; i >= 0; i--) {
        auto it = intScopes_[i].find(name);
        if (it != intScopes_[i].end()) return it->second;
    }
    return nullptr;
}

void CodeGen::declareIntVar(const std::string &name, Value *alloca) {
    if (!intScopes_.empty()) intScopes_.back()[name] = alloca;
}

Value *CodeGen::boxI64(Value *iv) {
    return callRT("perl_alloc_int", {iv});
}

bool CodeGen::canEmitI64(const Node &n) {
    switch (n.kind) {
    case NK::IntLit: return true;
    case NK::ScalarVar: {
        std::string nm = n.name;
        if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
        return lookupIntVar(nm) != nullptr;
    }
    case NK::BinOp: {
        static const char *intOps[] = {"+", "-", "*", "%", nullptr};
        for (auto *p = intOps; *p; p++) if (n.sval == *p)
            return n.left && n.right && canEmitI64(*n.left) && canEmitI64(*n.right);
        return false;
    }
    case NK::UnaryOp:
        return n.sval == "-" && n.left && canEmitI64(*n.left);
    default: return false;
    }
}

Value *CodeGen::emitExprI64(const Node &n) {
    auto *i64 = Type::getInt64Ty(ctx_);
    switch (n.kind) {
    case NK::IntLit:
        return ConstantInt::get(i64, n.ival);
    case NK::ScalarVar: {
        std::string nm = n.name;
        if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
        if (Value *ia = lookupIntVar(nm))
            return builder_.CreateLoad(i64, ia, nm + ".i");
        return nullptr;
    }
    case NK::BinOp: {
        static const char *intOps[] = {"+", "-", "*", "%", nullptr};
        bool isInt = false;
        for (auto *p = intOps; *p; p++) if (n.sval == *p) { isInt = true; break; }
        if (!isInt || !canEmitI64(*n.left) || !canEmitI64(*n.right)) return nullptr;
        Value *lv = emitExprI64(*n.left);
        Value *rv = emitExprI64(*n.right);
        if (!lv || !rv) return nullptr;
        if (n.sval == "+") return builder_.CreateAdd(lv, rv, "iadd");
        if (n.sval == "-") return builder_.CreateSub(lv, rv, "isub");
        if (n.sval == "*") return builder_.CreateMul(lv, rv, "imul");
        return builder_.CreateSRem(lv, rv, "irem");
    }
    case NK::UnaryOp:
        if (n.sval == "-" && n.left && canEmitI64(*n.left)) {
            Value *v = emitExprI64(*n.left);
            return v ? builder_.CreateNeg(v, "ineg") : nullptr;
        }
        return nullptr;
    default: return nullptr;
    }
}

/* Returns an i1 for integer comparisons, nullptr if not applicable. */
Value *CodeGen::tryEmitI1Cond(const Node &n) {
    if (n.kind != NK::BinOp || !n.left || !n.right) return nullptr;
    if (!canEmitI64(*n.left) || !canEmitI64(*n.right)) return nullptr;
    using P = llvm::CmpInst::Predicate;
    P pred;
    if      (n.sval == "<")  pred = P::ICMP_SLT;
    else if (n.sval == "<=") pred = P::ICMP_SLE;
    else if (n.sval == ">")  pred = P::ICMP_SGT;
    else if (n.sval == ">=") pred = P::ICMP_SGE;
    else if (n.sval == "==") pred = P::ICMP_EQ;
    else if (n.sval == "!=") pred = P::ICMP_NE;
    else return nullptr;
    Value *lv = emitExprI64(*n.left);
    Value *rv = emitExprI64(*n.right);
    if (!lv || !rv) return nullptr;
    return builder_.CreateICmp(pred, lv, rv, "icmp");
}

/* Emit an array index as a bare i64, bypassing PerlValue boxing.
   - IntLit → ConstantInt (zero allocation)
   - int var → load from alloca (zero allocation)
   - float var → FPToSI (zero allocation)
   - anything else → emitExpr + perl_to_int + freeIfOwned */
Value *CodeGen::emitIdx(const Node &n) {
    auto *i64 = Type::getInt64Ty(ctx_);
    if (n.kind == NK::IntLit)
        return ConstantInt::get(i64, n.ival);
    if (n.kind == NK::ScalarVar) {
        std::string nm = n.name;
        if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
        if (Value *ia = lookupIntVar(nm))
            return builder_.CreateLoad(i64, ia, nm + ".i");
        if (Value *fa = lookupFloatVar(nm)) {
            Value *d = builder_.CreateLoad(Type::getDoubleTy(ctx_), fa, nm + ".f");
            return builder_.CreateFPToSI(d, i64, nm + ".i");
        }
    }
    Value *pv = emitExpr(n);
    Value *i  = callRT("perl_to_int", {pv});
    freeIfOwned(pv);
    return i;
}

/* Returns true if 'nm' only ever appears in numeric-safe contexts within 'n'.
   Used to decide whether a @_ scalar arg can be promoted to a float alloca.
   'inNum' = the parent node is a numeric expression (so nm here is safe). */
static bool floatSafe(const Node &n, const std::string &nm, bool inNum) {
    if (n.kind == NK::ScalarVar && n.name == nm)
        return inNum;

    switch (n.kind) {
    case NK::BinOp: {
        bool isNum = (n.sval=="+"||n.sval=="-"||n.sval=="*"||n.sval=="/"||
                      n.sval=="**"||n.sval=="%"||n.sval=="<"||n.sval=="<="||
                      n.sval==">"||n.sval==">="||n.sval=="=="||n.sval=="!=");
        bool ok = true;
        if (n.left)  ok = ok && floatSafe(*n.left,  nm, isNum);
        if (n.right) ok = ok && floatSafe(*n.right, nm, isNum);
        return ok;
    }
    case NK::UnaryOp: {
        bool isNum = (n.sval=="-"||n.sval=="pre++"||n.sval=="post++"||
                      n.sval=="pre--"||n.sval=="post--");
        return !n.left || floatSafe(*n.left, nm, isNum);
    }
    case NK::SqrtFunc:
        return !n.left || floatSafe(*n.left, nm, true);
    case NK::CompoundAssign: {
        bool isNum = (n.sval=="+="||n.sval=="-="||n.sval=="*="||n.sval=="/="||
                      n.sval=="**="||n.sval=="%=");
        bool ok = true;
        if (n.left) {
            /* if nm is directly the LHS target, that's a numeric update — fine */
            if (!(n.left->kind == NK::ScalarVar && n.left->name == nm))
                ok = floatSafe(*n.left, nm, false);
        }
        if (n.right) ok = ok && floatSafe(*n.right, nm, isNum);
        return ok;
    }
    case NK::Assign:
        if (n.left && n.left->kind == NK::ArrayLit) {
            /* LHS elements are write targets — skip them; check RHS only */
            return !n.right || floatSafe(*n.right, nm, false);
        }
        if (n.left && n.left->kind == NK::ScalarVar && n.left->name == nm) {
            return !n.right || floatSafe(*n.right, nm, false);
        }
        {
            bool ok = true;
            if (n.left)  ok = ok && floatSafe(*n.left,  nm, false);
            if (n.right) ok = ok && floatSafe(*n.right, nm, false);
            return ok;
        }
    default: {
        bool ok = true;
        if (n.left)  ok = ok && floatSafe(*n.left,  nm, false);
        if (n.right) ok = ok && floatSafe(*n.right, nm, false);
        for (auto &a : n.args) ok = ok && floatSafe(*a, nm, false);
        if (n.body)  ok = ok && floatSafe(*n.body,  nm, false);
        if (n.init)  ok = ok && floatSafe(*n.init,  nm, false);
        if (n.cond)  ok = ok && floatSafe(*n.cond,  nm, false);
        if (n.step)  ok = ok && floatSafe(*n.step,  nm, false);
        return ok;
    }
    }
}

/* Returns true if 'nm' appears anywhere as a ScalarVar in 'n'. */
static bool hasVar(const Node &n, const std::string &nm) {
    if (n.kind == NK::ScalarVar && n.name == nm) return true;
    bool r = false;
    if (n.left)  r = r || hasVar(*n.left,  nm);
    if (n.right) r = r || hasVar(*n.right, nm);
    for (auto &a : n.args) r = r || hasVar(*a, nm);
    if (n.body)  r = r || hasVar(*n.body,  nm);
    if (n.init)  r = r || hasVar(*n.init,  nm);
    if (n.cond)  r = r || hasVar(*n.cond,  nm);
    if (n.step)  r = r || hasVar(*n.step,  nm);
    return r;
}

/* Returns true if 'nm' is ever used in a float-precision context (/, **, sqrt).
   Ensures we only promote vars that actually need floating-point, not integer
   counters that happen to appear in arithmetic. */
static bool needsFloatPrec(const Node &n, const std::string &nm) {
    if (n.kind == NK::BinOp && (n.sval == "/" || n.sval == "**")) {
        if ((n.left  && hasVar(*n.left,  nm)) ||
            (n.right && hasVar(*n.right, nm))) return true;
    }
    if (n.kind == NK::SqrtFunc && n.left && hasVar(*n.left, nm)) return true;
    bool r = false;
    if (n.left)  r = r || needsFloatPrec(*n.left,  nm);
    if (n.right) r = r || needsFloatPrec(*n.right, nm);
    for (auto &a : n.args) r = r || needsFloatPrec(*a, nm);
    if (n.body)  r = r || needsFloatPrec(*n.body,  nm);
    if (n.init)  r = r || needsFloatPrec(*n.init,  nm);
    if (n.cond)  r = r || needsFloatPrec(*n.cond,  nm);
    if (n.step)  r = r || needsFloatPrec(*n.step,  nm);
    return r;
}

/* Pure predicate — can this expression be computed as a bare LLVM double?
   Never emits any IR. Returns true iff emitExprF64 will succeed. */
bool CodeGen::canEmitF64(const Node &n) {
    switch (n.kind) {
    case NK::FloatLit:
    case NK::IntLit:
        return true;
    case NK::ScalarVar: {
        std::string nm = n.name;
        if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
        return lookupFloatVar(nm) != nullptr;
    }
    case NK::BinOp: {
        static const char *arithOps[] = {"+", "-", "*", "/", nullptr};
        for (auto *p = arithOps; *p; p++) if (n.sval == *p) {
            return n.left && n.right && canEmitF64(*n.left) && canEmitF64(*n.right);
        }
        return false;
    }
    case NK::UnaryOp:
        return n.sval == "-" && n.left && canEmitF64(*n.left);
    case NK::SqrtFunc:
        return n.left && canEmitF64(*n.left);
    /* Array/hash element lookups can always be converted to double via perl_to_float,
       but they require emitting emitExpr calls internally — always allowed. */
    case NK::ArrayElem:
        return lookupArray(n.name) != nullptr;
    case NK::ArrowDeref:
        /* $ref->[$i] or $ref->{key}: always convertible to float */
        return true;
    case NK::HashElem:
        return lookupHash(n.name) != nullptr;
    default:
        return false;
    }
}

Value *CodeGen::emitExprF64(const Node &n) {
    auto *f64 = Type::getDoubleTy(ctx_);
    switch (n.kind) {
    case NK::FloatLit:
        return ConstantFP::get(f64, n.fval);
    case NK::IntLit:
        return ConstantFP::get(f64, (double)n.ival);
    case NK::ScalarVar: {
        std::string nm = n.name;
        if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
        if (Value *fa = lookupFloatVar(nm))
            return builder_.CreateLoad(f64, fa, nm + ".f");
        return nullptr;
    }
    case NK::BinOp: {
        static const char *arithOps[] = {"+", "-", "*", "/", nullptr};
        bool isArith = false;
        for (auto *p = arithOps; *p; p++) if (n.sval == *p) { isArith = true; break; }
        if (!isArith) return nullptr;
        /* Check both children before emitting any IR to avoid double-emission */
        if (!canEmitF64(*n.left) || !canEmitF64(*n.right)) return nullptr;
        Value *lv = emitExprF64(*n.left);
        Value *rv = emitExprF64(*n.right);
        if (!lv || !rv) return nullptr;
        if (n.sval == "+") return builder_.CreateFAdd(lv, rv, "fadd");
        if (n.sval == "-") return builder_.CreateFSub(lv, rv, "fsub");
        if (n.sval == "*") return builder_.CreateFMul(lv, rv, "fmul");
        /* "/" — no div-by-zero check for unboxed (same as C) */
        return builder_.CreateFDiv(lv, rv, "fdiv");
    }
    case NK::UnaryOp:
        if (n.sval == "-") {
            if (!canEmitF64(*n.left)) return nullptr;
            Value *v = emitExprF64(*n.left);
            return v ? builder_.CreateFNeg(v, "fneg") : nullptr;
        }
        return nullptr;
    case NK::SqrtFunc: {
        if (!canEmitF64(*n.left)) return nullptr;
        Value *v = emitExprF64(*n.left);
        if (!v) return nullptr;
        auto *sqrtFn = llvm::Intrinsic::getDeclaration(mod_.get(),
            llvm::Intrinsic::sqrt, {f64});
        return builder_.CreateCall(sqrtFn, {v}, "sqrt");
    }
    case NK::ArrayElem: {
        Value *av = lookupArray(n.name);
        if (!av) return nullptr;
        Value *elem = callRT("perl_array_get_ref", {av, emitIdx(*n.left)});
        return callRT("perl_to_float", {elem});
    }
    case NK::ArrowDeref: {
        /* $ref->[$i] or $ref->{k} read — unbox the element */
        Value *base = emitExpr(*n.left);
        if (n.sval == "array") {
            Value *av  = callRT("perl_deref_array", {base});
            freeIfOwned(base);
            Value *elem = callRT("perl_array_get_ref", {av, emitIdx(*n.right)});
            return callRT("perl_to_float", {elem});
        } else {
            Value *hv = callRT("perl_deref_hash", {base});
            freeIfOwned(base);
            Value *elem = emitHashGetRef(hv, *n.right);
            return callRT("perl_to_float", {elem});
        }
    }
    case NK::HashElem: {
        Value *hv = lookupHash(n.name);
        if (!hv) return nullptr;
        Value *elem = emitHashGetRef(hv, *n.left);
        return callRT("perl_to_float", {elem});
    }
    default:
        return nullptr;
    }
}

/* ── top-level compile ───────────────────────────────────────────────────── */

void CodeGen::compile(const Node &program, const std::string &modName) {
    mod_->setModuleIdentifier(modName);
    if (debug_) initializeDebugInfo(modName);

    /* collect sub definitions first so forward calls work */
    std::vector<const Node *> subs;
    for (auto &stmt : program.args)
        if (stmt->kind == NK::SubDef)
            subs.push_back(stmt.get());

    /* pre-declare all subs as Functions */
    for (auto *s : subs) {
        auto *ft = FunctionType::get(perlPtrTy_,
                        {arrayPtrTy_, Type::getInt32Ty(ctx_)},  /* PerlArray* args, int ctx */
                        false);
        Function::Create(ft, Function::ExternalLinkage,
                         subLLVMName(s->name), mod_.get());
    }

    /* emit main(int argc, char **argv) */
    auto *i32  = Type::getInt32Ty(ctx_);
    auto *i8p  = PointerType::getUnqual(ctx_);
    auto *mainFT = FunctionType::get(i32, {i32, i8p}, false);
    auto *mainFn = Function::Create(mainFT, Function::ExternalLinkage,
                                    "main", mod_.get());
    mainFn->getArg(0)->setName("argc");
    mainFn->getArg(1)->setName("argv");
    auto *entry = BasicBlock::Create(ctx_, "entry", mainFn);
    builder_.SetInsertPoint(entry);

    if (debug_) {
        mainFn->setSubprogram(currentSP_);
        builder_.SetCurrentDebugLocation(getDebugLoc(1, currentSP_));
    }

    currentFn_ = mainFn;
    pushScope();
    /* emitBlock(program) will push one more scope; file-scope my vars live at that depth */
    fileScopeDepth_ = (int)scopes_.size() + 1;
    inMainBody_ = true;

    /* register all subs in the method dispatch table (before user code runs) */
    for (auto *s : subs) {
        if (s->name.find("::") != std::string::npos) {
            Value *keyStr = builder_.CreateGlobalStringPtr(s->name);
            auto *fn = mod_->getFunction(subLLVMName(s->name));
            callRT("perl_register_method", {keyStr, fn});
        }
    }

    /* set up @ARGV and $0 from command-line arguments */
    {
        Value *argc_v = mainFn->getArg(0);
        Value *argv_v = mainFn->getArg(1);
        Value *argvArr = callRT("perl_init_argv", {argc_v, argv_v});
        declareArray("ARGV", argvArr);

        Value *dollar0 = callRT("perl_get_dollar0", {});
        auto *slot0 = builder_.CreateAlloca(perlPtrTy_, nullptr, "$0");
        builder_.CreateStore(dollar0, slot0);
        declareVar("0", slot0);

        Value *underscoreVal = callRT("perl_alloc_undef", {});
        auto *slotUs = builder_.CreateAlloca(perlPtrTy_, nullptr, "$_");
        builder_.CreateStore(underscoreVal, slotUs);
        declareVar("_", slotUs);
    }

    /* capture local() save depth at function entry */
    auto *i32Ty = Type::getInt32Ty(ctx_);
    localDepthAlloca_ = builder_.CreateAlloca(i32Ty, nullptr, "local.depth");
    builder_.CreateStore(callRT("perl_local_save_depth", {}), localDepthAlloca_);

    emitBlock(program);
    popScope();

    callRT("perl_pop_wantarray", {});
    /* restore any local()s before returning */
    {
        Value *depth = builder_.CreateLoad(i32Ty, localDepthAlloca_);
        callRT("perl_local_restore_to", {depth});
    }
    builder_.CreateRet(ConstantInt::get(Type::getInt32Ty(ctx_), 0));

    inMainBody_ = false;
    /* emit sub bodies */
    for (auto *s : subs) emitSub(*s);

    if (debug_) {
        dib_->finalize();
    }

    std::string err;
    raw_string_ostream es(err);
    if (verifyModule(*mod_, &es))
        throw std::runtime_error("LLVM verify error: " + err);
}

/* ── sub definition ──────────────────────────────────────────────────────── */

void CodeGen::emitSub(const Node &n) {
    auto *fn = mod_->getFunction(subLLVMName(n.name));
    if (!fn) return;

    auto *entry = BasicBlock::Create(ctx_, "entry", fn);
    builder_.SetInsertPoint(entry);

    if (debug_) {
        builder_.SetCurrentDebugLocation(getDebugLoc(n.line, currentSP_));
    }

    auto *savedFn = currentFn_;
    currentFn_ = fn;
    pushScope();

    /* @_ is the first argument (PerlArray*) */
    Value *argsArr = fn->getArg(0);
    argsArr->setName("args");
    Value *ctxArg = fn->getArg(1);
    callRT("perl_push_wantarray", {ctxArg});
    declareArray("_", argsArr);

    /* pre-declare $_ so it's available for default-arg builtins and while(<FH>) */
    {
        Value *udv  = callRT("perl_alloc_undef", {});
        auto *slotUs = builder_.CreateAlloca(perlPtrTy_, nullptr, "$_");
        builder_.CreateStore(udv, slotUs);
        declareVar("_", slotUs);
        trackPv(udv);  /* ensure $_ stable pv is freed on scope exit */
    }

    /* capture local() save depth at function entry */
    auto *i32Ty = Type::getInt32Ty(ctx_);
    auto *savedLocalDepth = localDepthAlloca_;
    localDepthAlloca_ = builder_.CreateAlloca(i32Ty, nullptr, "local.depth");
    builder_.CreateStore(callRT("perl_local_save_depth", {}), localDepthAlloca_);

    /* forward declaration: emit empty body that returns undef */
    if (!n.body) {
        callRT("perl_pop_wantarray", {});
        Value *depth = builder_.CreateLoad(i32Ty, localDepthAlloca_);
        callRT("perl_local_restore_to", {depth});
        popScope();  /* free $_ */
        builder_.CreateRet(perlUndef());
        localDepthAlloca_ = savedLocalDepth;
        currentFn_ = savedFn;
        return;
    }

    auto *savedSubBody = currentSubBody_;
    currentSubBody_ = n.body.get();
    Value *lastVal = emitBlockLast(*n.body);
    currentSubBody_ = savedSubBody;

    /* implicit return from last expression (Perl: last expr is the return value) */
    if (!builder_.GetInsertBlock()->getTerminator()) {
        callRT("perl_pop_wantarray", {});
        Value *depth = builder_.CreateLoad(i32Ty, localDepthAlloca_);
        callRT("perl_local_restore_to", {depth});
        popScope();  /* free $_ and other function-scope pvs before ret */
        builder_.CreateRet(lastVal);
    } else {
        popScope();  /* explicit return: emitScopeCleanup already freed pvs; skip due to terminator */
    }
    localDepthAlloca_ = savedLocalDepth;

    currentFn_ = savedFn;

    /* restore insert point to end of main (for any remaining stmts) */
    /* caller will set insert point back */
}

/* ── statement emission ──────────────────────────────────────────────────── */

Value *CodeGen::emitBlock(const Node &n) {
    auto *i32Ty = Type::getInt32Ty(ctx_);
    auto *bdAlloca = builder_.CreateAlloca(i32Ty, nullptr, "block.ldepth");
    builder_.CreateStore(callRT("perl_local_save_depth", {}), bdAlloca);
    pushScope();
    for (auto &stmt : n.args) {
        emitStmt(*stmt);
        if (builder_.GetInsertBlock()->getTerminator()) break;
    }
    popScope();
    if (!builder_.GetInsertBlock()->getTerminator())
        callRT("perl_local_restore_to", {builder_.CreateLoad(i32Ty, bdAlloca)});
    return nullptr;
}

/* Emit a block and return the PerlValue* of its last expression statement. */
Value *CodeGen::emitBlockLast(const Node &n) {
    auto *i32Ty = Type::getInt32Ty(ctx_);
    auto *bdAlloca = builder_.CreateAlloca(i32Ty, nullptr, "block.ldepth");
    builder_.CreateStore(callRT("perl_local_save_depth", {}), bdAlloca);
    pushScope();
    Value *result = perlUndef();
    for (size_t i = 0; i < n.args.size(); i++) {
        const Node &stmt = *n.args[i];
        bool isLast = (i + 1 == n.args.size());
        if (isLast && stmt.kind == NK::ExprStmt && stmt.left) {
            result = emitExpr(*stmt.left);
        } else {
            emitStmt(stmt);
        }
        if (builder_.GetInsertBlock()->getTerminator()) { break; }
    }
    popScope();
    if (!builder_.GetInsertBlock()->getTerminator())
        callRT("perl_local_restore_to", {builder_.CreateLoad(i32Ty, bdAlloca)});
    return result;
}

void CodeGen::emitStmt(const Node &n) {
    if (builder_.GetInsertBlock()->getTerminator()) return;
    if (debug_ && n.line > 0) {
        builder_.SetCurrentDebugLocation(getDebugLoc(n.line, currentSP_));
    }
    switch (n.kind) {
    case NK::Block:
        emitBlock(n); break;

    case NK::FlatBlock:
        /* emit contents in the current scope, no new scope push */
        for (auto &stmt : n.args) {
            emitStmt(*stmt);
            if (builder_.GetInsertBlock()->getTerminator()) break;
        }
        break;

    case NK::ExprStmt:
        freeIfOwned(emitExpr(*n.left)); break;

    case NK::My: {
        if (n.name.empty()) break;
        bool isArr  = n.name[0] == '@';
        bool isHash = n.name[0] == '%';
        bool atFileScope = inMainBody_ && (int)scopes_.size() == fileScopeDepth_;

        if (isHash) {
            std::string nm = n.name.substr(1);
            Value *hv = callRT("perl_hash_new", {});
            if (atFileScope) {
                auto *gv = new GlobalVariable(*mod_, perlPtrTy_, false,
                    GlobalValue::InternalLinkage,
                    Constant::getNullValue(perlPtrTy_), "g.hash." + nm);
                builder_.CreateStore(hv, gv);
                fileHashGlobals_[nm] = gv;
            }
            declareHash(nm, hv);
            if (n.right) {
                Value *listArr = emitArrayPtr(*n.right);
                if (!listArr) {
                    listArr = callRT("perl_array_new", {});
                    callRT("perl_array_push", {listArr, emitExpr(*n.right)});
                }
                callRT("perl_hash_from_list", {hv, listArr});
            }
        } else if (isArr) {
            std::string nm = n.name.substr(1);
            Value *av = nullptr;
            if (n.right) av = emitArrayPtr(*n.right);
            if (!av) {
                av = callRT("perl_array_new", {});
                /* scalar RHS (e.g. my @arr = $ref  or  my @arr = [1,2,3]) —
                   push the value as a single element */
                if (n.right) callRT("perl_array_push", {av, emitExpr(*n.right)});
            }
            if (atFileScope) {
                auto *gv = new GlobalVariable(*mod_, perlPtrTy_, false,
                    GlobalValue::InternalLinkage,
                    Constant::getNullValue(perlPtrTy_), "g.arr." + nm);
                builder_.CreateStore(av, gv);
                fileArrayGlobals_[nm] = gv;
            }
            declareArray(nm, av);
        } else {
            /* n.name may carry a '$' prefix when parsed in expression context */
            std::string nm = n.name;
            if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
            if (atFileScope) {
                /* use a global variable so subroutines can access this file-scope var */
                auto *gv = new GlobalVariable(*mod_, perlPtrTy_, false,
                    GlobalValue::InternalLinkage,
                    Constant::getNullValue(perlPtrTy_), "g." + nm);
                Value *pv = perlUndef();
                builder_.CreateStore(pv, gv);
                if (n.right) {
                    Value *init = emitExpr(*n.right);
                    callRT("perl_assign", {pv, init});
                    freeIfOwned(init);
                }
                fileScalarGlobals_[nm] = gv;
                declareVar(nm, gv);
            } else {
                /* Unbox numeric scalars: skip PerlValue* alloca entirely */
                if (n.right && !atFileScope) {
                    if (Value *ival = emitExprI64(*n.right)) {
                        auto *ialloca = builder_.CreateAlloca(Type::getInt64Ty(ctx_), nullptr, n.name + ".i");
                        builder_.CreateStore(ival, ialloca);
                        declareIntVar(nm, ialloca);
                        break;
                    }
                    if (Value *fval = emitExprF64(*n.right)) {
                        auto *falloca = builder_.CreateAlloca(Type::getDoubleTy(ctx_), nullptr, n.name + ".f");
                        builder_.CreateStore(fval, falloca);
                        declareFloatVar(nm, falloca);
                        break;
                    }
                }
                auto *alloca = builder_.CreateAlloca(perlPtrTy_, nullptr, n.name);
                /* allocate a stable PerlValue* that lives for this variable's lifetime */
                Value *pv = perlUndef();
                builder_.CreateStore(pv, alloca);
                trackPv(pv);
                if (n.right) {
                    Value *init = emitExpr(*n.right);
                    callRT("perl_assign", {pv, init});
                    freeIfOwned(init);
                }
                declareVar(nm, alloca);
            }
        }
        break;
    }

    case NK::SubDef:
        /* bodies already emitted in compile() */
        break;

    case NK::PrintStmt:
    case NK::SayStmt: {
        bool isSay = (n.kind == NK::SayStmt);
        /* resolve filehandle (n.name: "", "STDOUT", "STDERR", or scalar varname) */
        Value *fh = nullptr;
        if (n.name == "STDERR")       fh = callRT("perl_get_stderr", {});
        else if (n.name == "STDOUT")  fh = callRT("perl_get_stdout", {});
        else if (!n.name.empty()) {
            if (auto *slot = lookupVar(n.name))
                fh = builder_.CreateLoad(perlPtrTy_, slot);
        }
        if (fh) {
            /* print/say to filehandle */
            if (n.args.empty()) {
                if (auto *slot = lookupVar("_")) {
                    Value *v = builder_.CreateLoad(perlPtrTy_, slot);
                    callRT(isSay ? "perl_say_fh" : "perl_print_fh", {fh, v});
                }
            } else {
                for (size_t i = 0; i < n.args.size(); i++)
                    callRT(isSay && i + 1 == n.args.size() ? "perl_say_fh" : "perl_print_fh",
                           {fh, emitExpr(*n.args[i])});
            }
        } else {
            /* print/say to stdout */
            if (n.args.empty()) {
                if (auto *slot = lookupVar("_")) {
                    Value *v = builder_.CreateLoad(perlPtrTy_, slot);
                    callRT(isSay ? "perl_say" : "perl_print", {v});
                }
            } else if (n.args.size() == 1) {
                Value *v = emitExpr(*n.args[0]);
                callRT(isSay ? "perl_say" : "perl_print", {v});
            } else {
                for (size_t i = 0; i < n.args.size(); i++)
                    callRT("perl_print", {emitExpr(*n.args[i])});
                if (isSay) {
                    auto *nl = builder_.CreateGlobalString("\n", ".nl");
                    callRT("perl_print_string", {nl});
                }
            }
        }
        break;
    }

    case NK::PrintfStmt: {
        Value *fmt = emitExpr(*n.left);
        Value *av  = callRT("perl_array_new", {});
        for (auto &a : n.args) callRT("perl_array_push", {av, emitExpr(*a)});
        if (n.name == "STDERR") {
            callRT("perl_printf_fh", {callRT("perl_get_stderr", {}), fmt, av});
        } else if (!n.name.empty() && n.name != "STDOUT") {
            Value *fh = nullptr;
            if (auto *slot = lookupVar(n.name)) fh = builder_.CreateLoad(perlPtrTy_, slot);
            if (fh) callRT("perl_printf_fh", {fh, fmt, av});
            else    callRT("perl_printf",    {fmt, av});
        } else {
            callRT("perl_printf", {fmt, av});
        }
        break;
    }

    case NK::If: {
        auto *fn   = builder_.GetInsertBlock()->getParent();
        auto *merge = BasicBlock::Create(ctx_, "if.end", fn);

        for (size_t i = 0; i < n.branches.size(); i++) {
            auto &br = n.branches[i];
            if (!br.cond) {
                /* else */
                emitBlock(*br.body);
                if (!builder_.GetInsertBlock()->getTerminator())
                    builder_.CreateBr(merge);
                break;
            }
            Value *cond = emitExpr(*br.cond);
            Value *b    = callRT("perl_is_true", {cond});
            freeIfOwned(cond);
            Value *bv   = builder_.CreateICmpNE(b,
                            ConstantInt::get(Type::getInt32Ty(ctx_), 0));
            auto *thenBB = BasicBlock::Create(ctx_, "if.then", fn);
            auto *elseBB = BasicBlock::Create(ctx_, "if.else", fn);
            builder_.CreateCondBr(bv, thenBB, elseBB);

            builder_.SetInsertPoint(thenBB);
            emitBlock(*br.body);
            if (!builder_.GetInsertBlock()->getTerminator())
                builder_.CreateBr(merge);

            builder_.SetInsertPoint(elseBB);
            if (i + 1 == n.branches.size()) {
                builder_.CreateBr(merge);
            }
            /* else: loop continues into next branch from elseBB */
        }

        builder_.SetInsertPoint(merge);
        break;
    }

    case NK::While: {
        auto *fn    = builder_.GetInsertBlock()->getParent();
        auto *cond  = BasicBlock::Create(ctx_, "while.cond", fn);
        auto *body  = BasicBlock::Create(ctx_, "while.body", fn);
        auto *exit  = BasicBlock::Create(ctx_, "while.end",  fn);

        loopExits_.push_back(exit);
        loopContinues_.push_back(cond);

        /* If condition is 'my $var = rhs', hoist the variable allocation before
         * the loop so the alloca and stable PerlValue* are created exactly once.
         * In while.cond we only do the assignment + truth-test each iteration. */
        Value *myCondPv   = nullptr;
        Node  *myCondRhs  = nullptr;
        if (n.cond && n.cond->kind == NK::My &&
            !n.cond->name.empty() && n.cond->name[0] == '$') {
            std::string nm = n.cond->name.substr(1);
            auto *alloca = builder_.CreateAlloca(perlPtrTy_, nullptr, n.cond->name);
            myCondPv = perlUndef();
            builder_.CreateStore(myCondPv, alloca);
            declareVar(nm, alloca);
            myCondRhs = n.cond->right.get();
        }

        builder_.CreateBr(cond);
        builder_.SetInsertPoint(cond);

        Value *b = nullptr;
        /* integer comparison fast path: skip boxing entirely */
        if (!myCondPv && n.cond) b = tryEmitI1Cond(*n.cond);
        if (!b) {
            Value *cv;
            if (myCondPv) {
                Value *rhs = myCondRhs ? emitExpr(*myCondRhs) : perlUndef();
                callRT("perl_assign", {myCondPv, rhs});
                freeIfOwned(rhs);
                cv = myCondPv;
            } else {
                cv = emitExpr(*n.cond);
            }
            Value *bv = callRT("perl_is_true", {cv});
            if (!myCondPv) freeIfOwned(cv);
            b = builder_.CreateICmpNE(bv,
                    ConstantInt::get(Type::getInt32Ty(ctx_), 0));
        }
        builder_.CreateCondBr(b, body, exit);

        builder_.SetInsertPoint(body);
        emitBlock(*n.body);
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateBr(cond);

        loopExits_.pop_back();
        loopContinues_.pop_back();
        builder_.SetInsertPoint(exit);
        break;
    }

    case NK::DoWhile: {
        auto *fn   = builder_.GetInsertBlock()->getParent();
        auto *body = BasicBlock::Create(ctx_, "dowhile.body", fn);
        auto *cond = BasicBlock::Create(ctx_, "dowhile.cond", fn);
        auto *exit = BasicBlock::Create(ctx_, "dowhile.end",  fn);

        loopExits_.push_back(exit);
        loopContinues_.push_back(cond);

        builder_.CreateBr(body);
        builder_.SetInsertPoint(body);
        emitBlock(*n.body);
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateBr(cond);

        builder_.SetInsertPoint(cond);
        Value *cv = emitExpr(*n.cond);
        Value *bv = callRT("perl_is_true", {cv});
        freeIfOwned(cv);
        Value *b  = builder_.CreateICmpNE(bv,
                        ConstantInt::get(Type::getInt32Ty(ctx_), 0));
        builder_.CreateCondBr(b, body, exit);

        loopExits_.pop_back();
        loopContinues_.pop_back();
        builder_.SetInsertPoint(exit);
        break;
    }

    case NK::For: {
        auto *fn   = builder_.GetInsertBlock()->getParent();
        auto *condBB = BasicBlock::Create(ctx_, "for.cond", fn);
        auto *bodyBB = BasicBlock::Create(ctx_, "for.body", fn);
        auto *stepBB = BasicBlock::Create(ctx_, "for.step", fn);
        auto *exit   = BasicBlock::Create(ctx_, "for.end",  fn);

        loopExits_.push_back(exit);
        loopContinues_.push_back(stepBB);

        pushScope();
        if (n.init) emitStmt(*n.init);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(condBB);
        if (n.cond) {
            Value *b = tryEmitI1Cond(*n.cond);
            if (!b) {
                Value *cv = emitExpr(*n.cond);
                Value *bv = callRT("perl_is_true", {cv});
                freeIfOwned(cv);
                b = builder_.CreateICmpNE(bv,
                        ConstantInt::get(Type::getInt32Ty(ctx_), 0));
            }
            builder_.CreateCondBr(b, bodyBB, exit);
        } else {
            builder_.CreateBr(bodyBB);
        }

        builder_.SetInsertPoint(bodyBB);
        emitBlock(*n.body);
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateBr(stepBB);

        builder_.SetInsertPoint(stepBB);
        if (n.step) freeIfOwned(emitExpr(*n.step));
        builder_.CreateBr(condBB);

        loopExits_.pop_back();
        loopContinues_.pop_back();
        builder_.SetInsertPoint(exit);
        popScope();  /* free for-init pvs (e.g. my $i) at loop exit */
        break;
    }

    case NK::Foreach: {
        auto *fn    = builder_.GetInsertBlock()->getParent();
        auto *exit  = BasicBlock::Create(ctx_, "foreach.end",  fn);
        auto *bodyBB = BasicBlock::Create(ctx_, "foreach.body", fn);
        auto *stepBB = BasicBlock::Create(ctx_, "foreach.step", fn);
        auto *i64   = Type::getInt64Ty(ctx_);

        /* Fast path: integer-range foreach — for my $VAR (LO .. HI)
           Emits a counted i64 loop with the loop var as an int alloca.
           Eliminates perl_range alloc, per-iter perl_array_get/perl_assign,
           and all perl_to_int($VAR) calls in the body via emitIdx. */
        bool isIntRange = (n.args.size() == 1 &&
                           n.args[0]->kind == NK::Range);
        if (isIntRange) {
            /* Compute lo and hi as bare i64 — use I64 path if possible,
               fall back to emitExpr + perl_to_int otherwise. */
            auto emitBound = [&](const Node &bound) -> Value * {
                if (Value *iv = emitExprI64(bound)) return iv;
                Value *pv = emitExpr(bound);
                Value *i  = callRT("perl_to_int", {pv});
                freeIfOwned(pv);
                return i;
            };
            Value *lo = emitBound(*n.args[0]->left);
            Value *hi = emitBound(*n.args[0]->right);

            std::string loopNm = n.name;
            if (!loopNm.empty() && loopNm[0] == '$') loopNm = loopNm.substr(1);
            auto *iterAlloca = builder_.CreateAlloca(i64, nullptr, loopNm + ".i");
            builder_.CreateStore(lo, iterAlloca);

            auto *condBB2 = BasicBlock::Create(ctx_, "foreach.cond", fn);
            loopExits_.push_back(exit);
            loopContinues_.push_back(stepBB);

            builder_.CreateBr(condBB2);
            builder_.SetInsertPoint(condBB2);
            Value *cur = builder_.CreateLoad(i64, iterAlloca);
            builder_.CreateCondBr(builder_.CreateICmpSLE(cur, hi), bodyBB, exit);

            builder_.SetInsertPoint(bodyBB);
            pushScope();
            declareIntVar(loopNm, iterAlloca);
            emitBlock(*n.body);
            popScope();
            if (!builder_.GetInsertBlock()->getTerminator())
                builder_.CreateBr(stepBB);

            builder_.SetInsertPoint(stepBB);
            Value *cur2 = builder_.CreateLoad(i64, iterAlloca);
            builder_.CreateStore(builder_.CreateAdd(cur2, ConstantInt::get(i64, 1)),
                                 iterAlloca);
            builder_.CreateBr(condBB2);

            loopExits_.pop_back();
            loopContinues_.pop_back();
            builder_.SetInsertPoint(exit);
            break;
        }

        /* General foreach: build iteration array */
        Value *tmpArr = nullptr;
        bool ownsTmpArr = false;
        if (n.args.size() == 1) {
            tmpArr = emitArrayPtr(*n.args[0]);
            NK k = n.args[0]->kind;
            ownsTmpArr = tmpArr && (k != NK::ArrayVar && k != NK::DerefArray);
        }
        if (!tmpArr) {
            tmpArr = callRT("perl_array_new", {});
            ownsTmpArr = true;
            for (auto &elem : n.args) {
                Value *v = emitExpr(*elem);
                callRT("perl_array_push", {tmpArr, v});
            }
        }

        /* loop variable — stable PerlValue* in alloca */
        auto *loopVar = builder_.CreateAlloca(perlPtrTy_, nullptr, n.name);
        Value *loopPv = perlUndef();
        builder_.CreateStore(loopPv, loopVar);

        /* index counter */
        auto *idxAlloca = builder_.CreateAlloca(i64, nullptr, "foreach.idx");
        builder_.CreateStore(ConstantInt::get(i64, 0), idxAlloca);

        auto *condBB = BasicBlock::Create(ctx_, "foreach.cond", fn);

        loopExits_.push_back(exit);
        loopContinues_.push_back(stepBB);

        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);

        Value *idx  = builder_.CreateLoad(i64, idxAlloca);
        Value *lenV = callRT("perl_array_len", {tmpArr});
        Value *len  = callRT("perl_to_int", {lenV});
        callRT("perl_free", {lenV});
        Value *cmp  = builder_.CreateICmpSLT(idx, len);
        builder_.CreateCondBr(cmp, bodyBB, exit);

        builder_.SetInsertPoint(bodyBB);
        pushScope();
        declareVar(n.name, loopVar);
        Value *elem = callRT("perl_array_get", {tmpArr, idx});
        callRT("perl_assign", {loopPv, elem});
        callRT("perl_free", {elem});

        emitBlock(*n.body);
        popScope();
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateBr(stepBB);

        builder_.SetInsertPoint(stepBB);
        Value *idx2 = builder_.CreateLoad(i64, idxAlloca);
        builder_.CreateStore(
            builder_.CreateAdd(idx2, ConstantInt::get(i64, 1)), idxAlloca);
        builder_.CreateBr(condBB);
        loopExits_.pop_back();
        loopContinues_.pop_back();
        builder_.SetInsertPoint(exit);
        callRT("perl_free", {loopPv});
        if (ownsTmpArr) callRT("perl_array_free", {tmpArr});
        break;
    }

    case NK::Last:
        if (!loopExits_.empty())
            builder_.CreateBr(loopExits_.back());
        break;

    case NK::Next:
        if (!loopContinues_.empty())
            builder_.CreateBr(loopContinues_.back());
        break;

    case NK::Return: {
        Value *v = n.left ? emitExpr(*n.left) : perlUndef();
        /* restore any local()s before returning; clone retval first so
           restore doesn't clobber the in-place PerlValue we're returning */
        if (localDepthAlloca_) {
            auto *i32Ty = Type::getInt32Ty(ctx_);
            Value *depth = builder_.CreateLoad(i32Ty, localDepthAlloca_);
            Value *cloned = callRT("perl_clone", {v});
            freeIfOwned(v);
            callRT("perl_pop_wantarray", {});
            callRT("perl_local_restore_to", {depth});
            v = cloned;
        }
        emitScopeCleanup();  /* free tracked my-var pvs in all active scopes */
        builder_.CreateRet(v);
        break;
    }

    case NK::LocalStmt: {
        /* save current value, optionally assign new one */
        Value *pv;
        if (n.name == "/") {
            pv = callRT("perl_get_input_sep", {});
        } else if (n.name == "!") {
            pv = callRT("perl_get_dollar_bang", {});
        } else {
            Value *slot = lookupVar(n.name);
            if (!slot) {
                Value *uv = callRT("perl_alloc_undef", {});
                slot = builder_.CreateAlloca(perlPtrTy_, nullptr, ("$" + n.name).c_str());
                builder_.CreateStore(uv, slot);
                declareVar(n.name, slot);
            }
            pv = builder_.CreateLoad(perlPtrTy_, slot);
        }
        callRT("perl_local_save", {pv});
        if (n.left) {
            Value *rhs = emitExpr(*n.left);
            callRT("perl_assign", {pv, rhs});
        }
        break;
    }

    case NK::StateDecl: {
        auto *fn  = builder_.GetInsertBlock()->getParent();
        auto *ptrTy = perlPtrTy_;
        auto *i8Ty  = Type::getInt8Ty(ctx_);
        /* module-level globals: the PerlValue* and an init flag */
        static int stateSeq = 0;
        std::string gname = "state.ptr." + std::to_string(stateSeq);
        std::string gflag = "state.init." + std::to_string(stateSeq++);
        auto *gptr = new GlobalVariable(*mod_, ptrTy, false,
            GlobalValue::InternalLinkage, ConstantPointerNull::get(ptrTy), gname);
        auto *ginit = new GlobalVariable(*mod_, i8Ty, false,
            GlobalValue::InternalLinkage, ConstantInt::get(i8Ty, 0), gflag);
        /* local alloca holds the same PerlValue* as the global */
        auto *slot = builder_.CreateAlloca(ptrTy, nullptr, ("$" + n.name).c_str());
        declareVar(n.name, slot);
        auto *initBB = BasicBlock::Create(ctx_, "state.init", fn);
        auto *doneBB = BasicBlock::Create(ctx_, "state.done", fn);
        Value *flag = builder_.CreateLoad(i8Ty, ginit);
        Value *isInited = builder_.CreateICmpNE(flag, ConstantInt::get(i8Ty, 0));
        builder_.CreateCondBr(isInited, doneBB, initBB);
        builder_.SetInsertPoint(initBB);
        Value *initVal = n.left ? emitExpr(*n.left) : callRT("perl_alloc_undef", {});
        builder_.CreateStore(initVal, gptr);
        builder_.CreateStore(ConstantInt::get(i8Ty, 1), ginit);
        builder_.CreateBr(doneBB);
        builder_.SetInsertPoint(doneBB);
        Value *finalPtr = builder_.CreateLoad(ptrTy, gptr);
        builder_.CreateStore(finalPtr, slot);
        break;
    }

    case NK::BeginBlock: {
        /* emit as inline code called immediately (at start of main) */
        emitBlock(*n.body);
        break;
    }

    case NK::EndBlock: {
        /* compile END body as a function and register via atexit */
        auto *fn = builder_.GetInsertBlock()->getParent();
        auto *savedBB = builder_.GetInsertBlock();
        static int endSeq = 0;
        std::string endName = "perl_end_" + std::to_string(endSeq++);
        auto *endFnTy = FunctionType::get(Type::getVoidTy(ctx_), false);
        auto *endFn = Function::Create(endFnTy, Function::InternalLinkage, endName, mod_.get());
        auto *entryBB = BasicBlock::Create(ctx_, "entry", endFn);
        builder_.SetInsertPoint(entryBB);
        emitBlock(*n.body);
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateRetVoid();
        builder_.SetInsertPoint(savedBB);
        /* call atexit with the END function */
        auto *atexitFnTy = FunctionType::get(Type::getInt32Ty(ctx_),
            {PointerType::get(endFnTy, 0)}, false);
        auto *atexitFn = mod_->getOrInsertFunction("atexit", atexitFnTy).getCallee();
        builder_.CreateCall(cast<Function>(atexitFn), {endFn});
        break;
    }

    case NK::PushStmt: {
        Value *av;
        if (n.left) {
            Value *ref = emitExpr(*n.left);
            av = callRT("perl_deref_array", {ref});
        } else {
            av = lookupArray(n.name);
            if (!av) { av = callRT("perl_array_new", {}); declareArray(n.name, av); }
        }
        for (auto &arg : n.args) {
            Value *src = emitArrayPtr(*arg);
            if (src) callRT("perl_array_extend", {av, src});
            else     callRT("perl_array_push",   {av, emitExpr(*arg)});
        }
        break;
    }

    case NK::UnshiftStmt2: {
        Value *av;
        if (n.left) {
            Value *ref = emitExpr(*n.left);
            av = callRT("perl_deref_array", {ref});
        } else {
            av = lookupArray(n.name);
            if (!av) { av = callRT("perl_array_new", {}); declareArray(n.name, av); }
        }
        /* build a temp array in order then extend from front */
        Value *tmp = callRT("perl_array_new", {});
        for (auto &arg : n.args) {
            Value *src = emitArrayPtr(*arg);
            if (src) callRT("perl_array_extend", {tmp, src});
            else     callRT("perl_array_push",   {tmp, emitExpr(*arg)});
        }
        /* unshift tmp elements into av in reverse order */
        Value *tmpLen = callRT("perl_to_int", {callRT("perl_array_len", {tmp})});
        /* emit a simple C-style loop: for (i = len-1; i >= 0; i--) */
        auto *fn    = builder_.GetInsertBlock()->getParent();
        auto *i64   = Type::getInt64Ty(ctx_);
        auto *iA    = builder_.CreateAlloca(i64, nullptr, "us.i");
        builder_.CreateStore(builder_.CreateSub(tmpLen, ConstantInt::get(i64, 1)), iA);
        auto *condBB = BasicBlock::Create(ctx_, "us.cond", fn);
        auto *bodyBB = BasicBlock::Create(ctx_, "us.body", fn);
        auto *exitBB = BasicBlock::Create(ctx_, "us.exit", fn);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        Value *i = builder_.CreateLoad(i64, iA);
        builder_.CreateCondBr(builder_.CreateICmpSGE(i, ConstantInt::get(i64, 0)), bodyBB, exitBB);
        builder_.SetInsertPoint(bodyBB);
        Value *elem = callRT("perl_array_get_ref", {tmp, i});
        callRT("perl_array_unshift", {av, elem});
        builder_.CreateStore(builder_.CreateSub(i, ConstantInt::get(i64, 1)), iA);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(exitBB);
        break;
    }

    default:
        emitExpr(n);
    }
}

/* ── expression emission ─────────────────────────────────────────────────── */

Value *CodeGen::emitExpr(const Node &n) {
    if (debug_ && n.line > 0) {
        builder_.SetCurrentDebugLocation(getDebugLoc(n.line, currentSP_));
    }
    switch (n.kind) {
    case NK::UndefLit:  return perlUndef();
    case NK::IntLit:    return perlInt(n.ival);
    case NK::FloatLit:  return perlFloat(n.fval);
    case NK::StringLit: return perlStr(n.sval);

    case NK::ScalarVar: {
        if (n.name == "!")  return callRT("perl_get_dollar_bang", {});
        if (n.name == "/")  return callRT("perl_get_input_sep",   {});
        {
            std::string nm = n.name;
            if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
            /* int var: box on demand */
            if (Value *ia = lookupIntVar(nm)) {
                Value *iv = builder_.CreateLoad(Type::getInt64Ty(ctx_), ia, nm + ".i");
                return boxI64(iv);
            }
            /* float var: box on demand */
            if (Value *fa = lookupFloatVar(nm)) {
                Value *dbl = builder_.CreateLoad(Type::getDoubleTy(ctx_), fa, nm + ".f");
                return boxF64(dbl);
            }
        }
        auto *slot = lookupVar(n.name);
        if (!slot) return perlUndef();
        return builder_.CreateLoad(perlPtrTy_, slot, n.name);
    }

    case NK::ArrayElem: {
        Value *av = lookupArray(n.name);
        if (!av) return perlUndef();
        return callRT("perl_array_get_ref", {av, emitIdx(*n.left)});
    }

    case NK::ArrayVar: {
        /* @arr in scalar/expr context — return the PerlArray* as opaque ptr */
        /* (used as rhs of list assignment) */
        Value *av = lookupArray(n.name);
        return av ? av : perlUndef();
    }

    case NK::ArrayLit: {
        /* build temp PerlArray from the element list */
        Value *av = callRT("perl_array_new", {});
        for (auto &elem : n.args) {
            Value *v = emitExpr(*elem);
            callRT("perl_array_push", {av, v});
        }
        return av;
    }

    case NK::Range: {
        /* in scalar context, return the element count */
        Value *lo = emitExpr(*n.left), *hi = emitExpr(*n.right);
        Value *av  = callRT("perl_range", {lo, hi});
        freeIfOwned(lo); freeIfOwned(hi);
        Value *len = callRT("perl_array_len", {av});
        callRT("perl_array_free", {av});
        return len;
    }

    case NK::Readline: {
        if (n.sval == "STDIN" || n.sval.empty())
            return callRT("perl_readline_stdin", {});
        if (auto *slot = lookupVar(n.sval)) {
            Value *fh = builder_.CreateLoad(perlPtrTy_, slot);
            return callRT("perl_readline", {fh});
        }
        return perlUndef();
    }

    case NK::OpenFunc: {
        Value *slot = nullptr;
        if (n.sval == "my") {
            slot = builder_.CreateAlloca(perlPtrTy_, nullptr, n.name);
            Value *pv = perlUndef();
            builder_.CreateStore(pv, slot);
            declareVar(n.name, slot);
        } else {
            slot = lookupVar(n.name);
            if (!slot) return perlUndef();
        }
        Value *fh_pv = builder_.CreateLoad(perlPtrTy_, slot);
        if (n.args.size() >= 2)
            return callRT("perl_open_fh",  {fh_pv, emitExpr(*n.args[0]), emitExpr(*n.args[1])});
        if (n.args.size() == 1)
            return callRT("perl_open2_fh", {fh_pv, emitExpr(*n.args[0])});
        return perlUndef();
    }

    case NK::CloseFunc: {
        Value *fh = n.left ? emitExpr(*n.left) : perlUndef();
        callRT("perl_close_fh", {fh});
        return perlInt(1);
    }

    case NK::EofFunc: {
        Value *fh = n.left ? emitExpr(*n.left) : callRT("perl_get_stdin", {});
        return callRT("perl_eof_fh", {fh});
    }

    case NK::DieStmt: {
        Value *msg = n.left ? emitExpr(*n.left) : perlStr("Died");
        callRT("perl_die", {msg});
        builder_.CreateUnreachable();
        /* move to a dead block so surrounding codegen stays well-formed */
        auto *fn     = builder_.GetInsertBlock()->getParent();
        auto *deadBB = BasicBlock::Create(ctx_, "die.dead", fn);
        builder_.SetInsertPoint(deadBB);
        return perlUndef();
    }

    case NK::UnlinkFunc: {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) callRT("perl_array_push", {av, emitExpr(*a)});
        return callRT("perl_unlink_files", {av});
    }

    case NK::My: {
        /* 'my $var = expr' in expression context */
        emitStmt(n);
        if (!n.name.empty() && n.name[0] == '$') {
            std::string nm = n.name.substr(1);
            if (Value *ia = lookupIntVar(nm)) {
                Value *iv = builder_.CreateLoad(Type::getInt64Ty(ctx_), ia, nm + ".i");
                return boxI64(iv);
            }
            if (Value *fa = lookupFloatVar(nm)) {
                Value *dbl = builder_.CreateLoad(Type::getDoubleTy(ctx_), fa, nm + ".f");
                return boxF64(dbl);
            }
            if (auto *slot = lookupVar(nm))
                return builder_.CreateLoad(perlPtrTy_, slot);
        }
        return perlUndef();
    }

    case NK::BinOp:     return emitBinOp(n);

    case NK::UnaryOp: {
        if (n.sval == "-") {
            Value *operand = emitExpr(*n.left);
            Value *result  = callRT("perl_negate", {operand});
            freeIfOwned(operand);
            return result;
        }
        if (n.sval == "!") {
            Value *operand = emitExpr(*n.left);
            Value *result  = callRT("perl_not", {operand});
            freeIfOwned(operand);
            return result;
        }
        if (n.sval == "pre++" || n.sval == "pre--" ||
            n.sval == "post++" || n.sval == "post--") {
            /* fast path: unboxed integer variable */
            if (n.left && n.left->kind == NK::ScalarVar) {
                std::string nm = n.left->name;
                if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
                if (Value *ia = lookupIntVar(nm)) {
                    auto *i64 = Type::getInt64Ty(ctx_);
                    Value *cur = builder_.CreateLoad(i64, ia);
                    bool isInc = (n.sval == "pre++" || n.sval == "post++");
                    Value *delta = ConstantInt::get(i64, isInc ? 1 : -1);
                    Value *next = isInc ? builder_.CreateAdd(cur, delta, "preinc")
                                       : builder_.CreateSub(cur, delta, "predec");
                    builder_.CreateStore(next, ia);
                    bool isPre = (n.sval == "pre++" || n.sval == "pre--");
                    return boxI64(isPre ? next : cur);
                }
            }
        }
        if (n.sval == "pre++") {
            Value *v = emitExpr(*n.left); callRT("perl_inc", {v}); return v;
        }
        if (n.sval == "pre--") {
            Value *v = emitExpr(*n.left); callRT("perl_dec", {v}); return v;
        }
        if (n.sval == "post++") {
            Value *orig = emitExpr(*n.left);
            Value *copy = callRT("perl_clone", {orig});
            callRT("perl_inc", {orig});
            return copy;
        }
        if (n.sval == "post--") {
            Value *orig = emitExpr(*n.left);
            Value *copy = callRT("perl_clone", {orig});
            callRT("perl_dec", {orig});
            return copy;
        }
        return perlUndef();
    }

    case NK::Assign: {
        /* ($a,$b,...) = list */
        if (n.left->kind == NK::ArrayLit) {
            Value *rhsArr = emitArrayPtr(*n.right);
            if (!rhsArr) rhsArr = emitExpr(*n.right);
            bool fromUnderbar = (n.right->kind == NK::ArrayVar && n.right->name == "_");
            for (size_t i = 0; i < n.left->args.size(); i++) {
                auto *slot = emitLValue(*n.left->args[i]);
                if (!slot) continue;
                Value *pv  = builder_.CreateLoad(perlPtrTy_, slot);
                Value *idx = ConstantInt::get(Type::getInt64Ty(ctx_), (long long)i);
                Value *elem = callRT("perl_array_get_ref", {rhsArr, idx});
                callRT("perl_assign", {pv, elem});
                /* promote @_ scalar args to float allocas when only used numerically */
                if (fromUnderbar && currentSubBody_) {
                    auto *varNode = n.left->args[i].get();
                    if (varNode->kind == NK::ScalarVar) {
                        const std::string &nm = varNode->name;
                        if (!lookupFloatVar(nm) && !lookupIntVar(nm) &&
                            needsFloatPrec(*currentSubBody_, nm) &&
                            floatSafe(*currentSubBody_, nm, false)) {
                            auto *f64 = Type::getDoubleTy(ctx_);
                            auto *fa  = builder_.CreateAlloca(f64, nullptr, nm + ".f");
                            Value *dbl = callRT("perl_to_float", {pv});
                            builder_.CreateStore(dbl, fa);
                            declareFloatVar(nm, fa);
                        }
                    }
                }
                freeIfOwned(elem);
            }
            return perlUndef();
        }
        /* $h{key} = val */
        if (n.left->kind == NK::HashElem) {
            if (n.left->name == "ENV") {
                Value *key = emitExpr(*n.left->left);
                Value *val = emitExpr(*n.right);
                callRT("perl_env_set", {key, val});
                freeIfOwned(key);
                return val;
            }
            Value *hv = lookupHash(n.left->name);
            if (!hv) return perlUndef();
            Value *val = emitExpr(*n.right);
            emitHashSet(hv, *n.left->left, val);
            return val;
        }
        /* %h = (list) */
        if (n.left->kind == NK::HashVar) {
            Value *hv = lookupHash(n.left->name);
            if (!hv) return perlUndef();
            Value *listArr = callRT("perl_array_new", {});
            if (n.right->kind == NK::ArrayLit) {
                for (auto &elem : n.right->args)
                    callRT("perl_array_push", {listArr, emitExpr(*elem)});
            }
            callRT("perl_hash_from_list", {hv, listArr});
            return perlUndef();
        }
        /* $$ref = val */
        if (n.left->kind == NK::DerefScalar) {
            Value *ref = emitExpr(*n.left->left);
            Value *target = callRT("perl_deref_scalar", {ref});
            Value *rhs = emitExpr(*n.right);
            callRT("perl_assign", {target, rhs});
            return rhs;
        }
        /* $ref->[i] = val  or  $ref->{k} = val */
        if (n.left->kind == NK::ArrowDeref) {
            Value *base = emitExpr(*n.left->left);
            Value *rhs  = emitExpr(*n.right);
            if (n.left->sval == "array") {
                Value *av = callRT("perl_deref_array", {base});
                freeIfOwned(base);
                callRT("perl_array_set", {av, emitIdx(*n.left->right), rhs});
            } else {
                Value *hv = callRT("perl_deref_hash", {base});
                freeIfOwned(base);
                emitHashSet(hv, *n.left->right, rhs);
            }
            return rhs;
        }
        /* $arr[i] = val */
        if (n.left->kind == NK::ArrayElem) {
            Value *av = lookupArray(n.left->name);
            if (!av) return perlUndef();
            Value *rhs = emitExpr(*n.right);
            callRT("perl_array_set", {av, emitIdx(*n.left->left), rhs});
            return rhs;
        }
        /* opendir assignment: opendir(my $dh, path) stores DIRHANDLE into $dh */
        if (n.left->kind == NK::ScalarVar && n.right &&
            n.right->kind == NK::OpendirFunc) {
            /* The OpendirFunc node stores result into the named var */
            emitStmt(*n.right);   /* side-effect: fills the dh slot */
            return perlUndef();
        }
        /* special globals: $/ and $! assigned through stable pointer */
        if (n.left->kind == NK::ScalarVar &&
            (n.left->name == "/" || n.left->name == "!")) {
            Value *rhs = emitExpr(*n.right);
            Value *pv  = (n.left->name == "/") ? callRT("perl_get_input_sep",  {})
                                                : callRT("perl_get_dollar_bang", {});
            callRT("perl_assign", {pv, rhs});
            return rhs;
        }
        /* int/float var assignment */
        if (n.left->kind == NK::ScalarVar) {
            std::string nm = n.left->name;
            if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
            if (Value *ia = lookupIntVar(nm)) {
                Value *rhs = emitExprI64(*n.right);
                if (rhs) {
                    builder_.CreateStore(rhs, ia);
                    return boxI64(rhs);
                }
                /* RHS not purely integer — extract int from boxed value */
                Value *rv = emitExpr(*n.right);
                Value *iv = callRT("perl_to_int", {rv});
                builder_.CreateStore(iv, ia);
                freeIfOwned(rv);
                return rv;
            }
            if (Value *fa = lookupFloatVar(nm)) {
                Value *rhs = emitExprF64(*n.right);
                if (rhs) {
                    builder_.CreateStore(rhs, fa);
                    return boxF64(rhs);
                }
                /* RHS not purely numeric — box and store */
                Value *rv = emitExpr(*n.right);
                Value *dbl = callRT("perl_to_float", {rv});
                builder_.CreateStore(dbl, fa);
                freeIfOwned(rv);
                return rv;
            }
        }
        {
        Value *rhs = emitExpr(*n.right);
        Value *lhs = emitLValue(*n.left);
        if (lhs) {
            /* perl_assign model: mutate the stable PerlValue* in-place */
            Value *lhsVal = builder_.CreateLoad(perlPtrTy_, lhs);
            callRT("perl_assign", {lhsVal, rhs});
        }
        return rhs;
        }
    }

    case NK::CompoundAssign: {
        auto applyOp = [&](Value *lv, Value *rv) -> Value * {
            if      (n.sval == "+") return callRT("perl_add",    {lv, rv});
            else if (n.sval == "-") return callRT("perl_sub",    {lv, rv});
            else if (n.sval == "*") return callRT("perl_mul",    {lv, rv});
            else if (n.sval == "/") return callRT("perl_div",    {lv, rv});
            else if (n.sval == ".") return callRT("perl_concat", {lv, rv});
            else return perlUndef();
        };
        /* int/float var: $x op= rhs */
        if (n.left->kind == NK::ScalarVar) {
            std::string nm = n.left->name;
            if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
            if (Value *ia = lookupIntVar(nm)) {
                auto applyI64 = [&](Value *lv, Value *rv) -> Value * {
                    if (n.sval == "+") return builder_.CreateAdd(lv, rv);
                    if (n.sval == "-") return builder_.CreateSub(lv, rv);
                    if (n.sval == "*") return builder_.CreateMul(lv, rv);
                    if (n.sval == "%") return builder_.CreateSRem(lv, rv);
                    return nullptr;
                };
                Value *lv = builder_.CreateLoad(Type::getInt64Ty(ctx_), ia);
                Value *rv = emitExprI64(*n.right);
                if (rv) {
                    Value *res = applyI64(lv, rv);
                    if (res) {
                        builder_.CreateStore(res, ia);
                        return boxI64(res);
                    }
                }
                /* fallback: box current int, compute boxed op, unbox result */
                {
                    Value *lhsBox = boxI64(lv);
                    Value *rhsVal = emitExpr(*n.right);
                    Value *result = applyOp(lhsBox, rhsVal);
                    Value *newInt = callRT("perl_to_int", {result});
                    builder_.CreateStore(newInt, ia);
                    callRT("perl_free", {lhsBox});
                    freeIfOwned(rhsVal);
                    return result;
                }
            }
            if (Value *fa = lookupFloatVar(nm)) {
                auto applyF64 = [&](Value *lv, Value *rv) -> Value * {
                    if (n.sval == "+") return builder_.CreateFAdd(lv, rv);
                    if (n.sval == "-") return builder_.CreateFSub(lv, rv);
                    if (n.sval == "*") return builder_.CreateFMul(lv, rv);
                    if (n.sval == "/") return builder_.CreateFDiv(lv, rv);
                    return nullptr;
                };
                Value *lv = builder_.CreateLoad(Type::getDoubleTy(ctx_), fa);
                Value *rv = emitExprF64(*n.right);
                if (rv) {
                    Value *res = applyF64(lv, rv);
                    if (res) {
                        builder_.CreateStore(res, fa);
                        return boxF64(res);
                    }
                }
                /* fallback: box current float, compute boxed op, unbox result */
                {
                    Value *lhsBox = boxF64(lv);
                    Value *rhsVal = emitExpr(*n.right);
                    Value *result = applyOp(lhsBox, rhsVal);
                    Value *newDbl = callRT("perl_to_float", {result});
                    builder_.CreateStore(newDbl, fa);
                    callRT("perl_free", {lhsBox});
                    freeIfOwned(rhsVal);
                    return result;
                }
            }
        }
        /* $arr[$i] op= rhs */
        if (n.left->kind == NK::ArrayElem) {
            Value *av = lookupArray(n.left->name);
            if (!av) return perlUndef();
            Value *idx    = emitIdx(*n.left->left);
            Value *lhsVal = callRT("perl_array_get_ref", {av, idx});
            Value *rhsVal = emitExpr(*n.right);
            Value *result = applyOp(lhsVal, rhsVal);
            freeIfOwned(lhsVal);
            freeIfOwned(rhsVal);
            callRT("perl_array_set", {av, idx, result});
            return result;
        }
        /* $hash{key} op= rhs */
        if (n.left->kind == NK::HashElem) {
            Value *hv = lookupHash(n.left->name);
            if (!hv) return perlUndef();
            Value *lhsVal = emitHashGetRef(hv, *n.left->left);
            Value *rhsVal = emitExpr(*n.right);
            Value *result = applyOp(lhsVal, rhsVal);
            freeIfOwned(lhsVal);
            freeIfOwned(rhsVal);
            emitHashSet(hv, *n.left->left, result);
            return result;
        }
        /* $ref->[$i] op= rhs  or  $ref->{k} op= rhs */
        if (n.left->kind == NK::ArrowDeref) {
            Value *base = emitExpr(*n.left->left);
            Value *lhsVal, *rhsVal, *result;
            if (n.left->sval == "array") {
                Value *av  = callRT("perl_deref_array", {base});
                freeIfOwned(base);
                Value *idx = emitIdx(*n.left->right);
                lhsVal = callRT("perl_array_get_ref", {av, idx});
                rhsVal = emitExpr(*n.right);
                result = applyOp(lhsVal, rhsVal);
                freeIfOwned(lhsVal);
                freeIfOwned(rhsVal);
                callRT("perl_array_set", {av, idx, result});
            } else {
                Value *hv = callRT("perl_deref_hash", {base});
                freeIfOwned(base);
                lhsVal = emitHashGetRef(hv, *n.left->right);
                rhsVal = emitExpr(*n.right);
                result = applyOp(lhsVal, rhsVal);
                freeIfOwned(lhsVal);
                freeIfOwned(rhsVal);
                emitHashSet(hv, *n.left->right, result);
            }
            return result;
        }
        /* scalar: $var op= rhs */
        Value *lhsPtr = emitLValue(*n.left);
        if (!lhsPtr) return perlUndef();
        Value *lhsVal = builder_.CreateLoad(perlPtrTy_, lhsPtr);
        Value *rhsVal = emitExpr(*n.right);
        Value *result = applyOp(lhsVal, rhsVal);
        freeIfOwned(rhsVal);
        callRT("perl_assign", {lhsVal, result});
        freeIfOwned(result);
        return lhsVal;
    }

    case NK::Call: return emitCall(n);

    case NK::ScalarFunc: {
        Value *av = lookupArray(n.name);
        if (!av) return perlInt(0);
        return callRT("perl_array_len", {av});
    }

    case NK::DefinedFunc: {
        Value *v    = emitExpr(*n.left);
        Value *i32  = callRT("perl_defined", {v});
        Value *bit  = builder_.CreateICmpNE(i32, ConstantInt::get(Type::getInt32Ty(ctx_), 0));
        Value *i64  = builder_.CreateZExt(bit, Type::getInt64Ty(ctx_));
        return callRT("perl_alloc_int", {i64});
    }

    case NK::PopExpr: {
        Value *av = lookupArray(n.name);
        if (!av) return perlUndef();
        return callRT("perl_array_pop", {av});
    }

    case NK::ShiftExpr: {
        Value *av = lookupArray(n.name);
        if (!av) return perlUndef();
        return callRT("perl_array_shift", {av});
    }

    case NK::UnshiftStmt2: {
        Value *av;
        if (n.left) {
            av = callRT("perl_deref_array", {emitExpr(*n.left)});
        } else {
            av = lookupArray(n.name);
            if (!av) { av = callRT("perl_array_new", {}); declareArray(n.name, av); }
        }
        Value *tmp = callRT("perl_array_new", {});
        for (auto &arg : n.args) {
            Value *src = emitArrayPtr(*arg);
            if (src) callRT("perl_array_extend", {tmp, src});
            else     callRT("perl_array_push",   {tmp, emitExpr(*arg)});
        }
        Value *tmpLen = callRT("perl_to_int", {callRT("perl_array_len", {tmp})});
        auto *fn    = builder_.GetInsertBlock()->getParent();
        auto *i64   = Type::getInt64Ty(ctx_);
        auto *iA    = builder_.CreateAlloca(i64, nullptr, "us2.i");
        builder_.CreateStore(builder_.CreateSub(tmpLen, ConstantInt::get(i64, 1)), iA);
        auto *condBB = BasicBlock::Create(ctx_, "us2.cond", fn);
        auto *bodyBB = BasicBlock::Create(ctx_, "us2.body", fn);
        auto *exitBB = BasicBlock::Create(ctx_, "us2.exit", fn);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        Value *i = builder_.CreateLoad(i64, iA);
        builder_.CreateCondBr(builder_.CreateICmpSGE(i, ConstantInt::get(i64, 0)), bodyBB, exitBB);
        builder_.SetInsertPoint(bodyBB);
        Value *elem = callRT("perl_array_get_ref", {tmp, i});
        callRT("perl_array_unshift", {av, elem});
        builder_.CreateStore(builder_.CreateSub(i, ConstantInt::get(i64, 1)), iA);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(exitBB);
        return callRT("perl_array_len", {av});
    }

    case NK::ChompFunc: {
        bool isChop = (n.sval == "chop");
        /* chomp/chop on array: apply to every element */
        if (n.left->kind == NK::ArrayVar) {
            Value *av = lookupArray(n.left->name);
            if (av) {
                if (isChop) { /* chop each element, return last removed char */
                    Value *lastChar = perlStr("");
                    Value *len = callRT("perl_to_int", {callRT("perl_array_len", {av})});
                    /* simple loop would need LLVM loop; for now just chop each element */
                    /* Actually call a helper — same as chomp_array for simplicity */
                    callRT("perl_chomp_array", {av}); return perlInt(0);
                }
                callRT("perl_chomp_array", {av}); return perlInt(0);
            }
        }
        /* chomp/chop on scalar */
        Value *v = emitExpr(*n.left);
        if (isChop) return callRT("perl_chop", {v});
        callRT("perl_chomp", {v});
        return v;
    }

    case NK::LengthFunc:
        return callRT("perl_length", {emitExpr(*n.left)});

    case NK::SubstrFunc: {
        if (n.args.size() < 2) return perlUndef();
        Value *str = emitExpr(*n.args[0]);
        Value *off = emitExpr(*n.args[1]);
        if (n.args.size() >= 3) {
            Value *len = emitExpr(*n.args[2]);
            return callRT("perl_substr3", {str, off, len});
        }
        return callRT("perl_substr2", {str, off});
    }

    case NK::SprintfFunc: {
        Value *fmt = emitExpr(*n.left);
        Value *av  = callRT("perl_array_new", {});
        for (auto &a : n.args) callRT("perl_array_push", {av, emitExpr(*a)});
        return callRT("perl_sprintf", {fmt, av});
    }

    case NK::JoinFunc: {
        Value *sep = emitExpr(*n.left);
        /* build a temp array from the rest of the args */
        Value *av = nullptr;
        if (n.args.size() == 1) {
            av = emitArrayPtr(*n.args[0]);
        }
        if (!av) {
            av = callRT("perl_array_new", {});
            for (auto &a : n.args) {
                Value *src = emitArrayPtr(*a);
                if (src) callRT("perl_array_extend", {av, src});
                else     callRT("perl_array_push",   {av, emitExpr(*a)});
            }
        }
        return callRT("perl_join", {sep, av});
    }

    case NK::SplitFunc: {
        Value *str = n.right ? emitExpr(*n.right) : perlUndef();
        if (n.ival) {  /* regex split */
            Value *pat = builder_.CreateGlobalStringPtr(n.sval, "sp_pat");
            Value *flg = builder_.CreateGlobalStringPtr(n.name, "sp_flg");
            return callRT("perl_split_regex", {pat, flg, str});
        }
        Value *sep = n.left  ? emitExpr(*n.left)  : perlStr(" ");
        return callRT("perl_split", {sep, str});
    }

    case NK::HashVar: {
        /* %hash in scalar context — return key count */
        Value *hv = lookupHash(n.name);
        return hv ? callRT("perl_hash_size", {hv}) : perlInt(0);
    }

    case NK::HashElem: {
        if (n.name == "ENV") {
            Value *key = emitExpr(*n.left);
            return callRT("perl_env_get", {key});
        }
        Value *hv = lookupHash(n.name);
        if (!hv) return perlUndef();
        return emitHashGetRef(hv, *n.left);
    }

    case NK::KeysFunc: {
        Value *hv = lookupHash(n.name);
        if (!hv) return perlInt(0);
        /* in scalar context return count; sort flag handled by emitArrayPtr */
        return callRT("perl_hash_size", {hv});
    }

    case NK::ValuesFunc: {
        Value *hv = lookupHash(n.name);
        return hv ? callRT("perl_hash_size", {hv}) : perlInt(0);
    }

    case NK::ExistsFunc: {
        Value *hv = lookupHash(n.name);
        if (!hv) return perlInt(0);
        Value *r = emitHashExists(hv, *n.left);
        return callRT("perl_alloc_int",
            {builder_.CreateSExt(r, Type::getInt64Ty(ctx_))});
    }

    case NK::DeleteFunc: {
        Value *hv = lookupHash(n.name);
        if (!hv) return perlUndef();
        return emitHashDelete(hv, *n.left);
    }

    case NK::SortFunc:
    case NK::MapFunc:
    case NK::GrepFunc:
        /* array-producing: scalar context returns element count */
        {
            Value *av = emitArrayPtr(n);
            if (!av) return perlUndef();
            return callRT("perl_array_len", {av});
        }
    case NK::ReverseFunc: {
        /* scalar EXPR or single scalar arg → reverse string */
        bool hasScalarCtx = (n.sval == "scalar_ctx");
        bool hasArrayArg = false;
        for (auto &a : n.args)
            if (a->kind == NK::ArrayVar || a->kind == NK::DerefArray) { hasArrayArg = true; break; }
        if (hasScalarCtx || (!hasArrayArg && n.args.size() == 1)) {
            return callRT("perl_reverse_str", {emitExpr(*n.args[0])});
        }
        Value *av = emitArrayPtr(n);
        if (!av) return perlUndef();
        return callRT("perl_array_len", {av});
    }

    /* ── math builtins ───────────────────────────────────────────────────── */
    case NK::AbsFunc:  { Value *a=emitExpr(*n.left); Value *r=callRT("perl_abs_val",  {a}); freeIfOwned(a); return r; }
    case NK::IntFunc:  { Value *a=emitExpr(*n.left); Value *r=callRT("perl_int_trunc",{a}); freeIfOwned(a); return r; }
    case NK::SqrtFunc: { Value *a=emitExpr(*n.left); Value *r=callRT("perl_sqrt_val", {a}); freeIfOwned(a); return r; }

    /* ── string case ─────────────────────────────────────────────────────── */
    case NK::UcFunc:      { Value *a=emitExpr(*n.left); Value *r=callRT("perl_uc_str",      {a}); freeIfOwned(a); return r; }
    case NK::LcFunc:      { Value *a=emitExpr(*n.left); Value *r=callRT("perl_lc_str",      {a}); freeIfOwned(a); return r; }
    case NK::UcfirstFunc: { Value *a=emitExpr(*n.left); Value *r=callRT("perl_ucfirst_str", {a}); freeIfOwned(a); return r; }
    case NK::LcfirstFunc: { Value *a=emitExpr(*n.left); Value *r=callRT("perl_lcfirst_str", {a}); freeIfOwned(a); return r; }

    /* ── chr / ord / hex / oct ───────────────────────────────────────────── */
    case NK::ChrFunc: { Value *a=emitExpr(*n.left); Value *r=callRT("perl_chr_val", {a}); freeIfOwned(a); return r; }
    case NK::OrdFunc: { Value *a=emitExpr(*n.left); Value *r=callRT("perl_ord_val", {a}); freeIfOwned(a); return r; }
    case NK::HexFunc: { Value *a=emitExpr(*n.left); Value *r=callRT("perl_hex_val", {a}); freeIfOwned(a); return r; }
    case NK::OctFunc: { Value *a=emitExpr(*n.left); Value *r=callRT("perl_oct_val", {a}); freeIfOwned(a); return r; }

    /* ── index / rindex ──────────────────────────────────────────────────── */
    case NK::IndexFunc:
    case NK::RindexFunc: {
        bool isR = (n.kind == NK::RindexFunc);
        Value *str = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *sub = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        Value *pos = n.args.size() > 2 ? emitExpr(*n.args[2]) : perlUndef();
        return callRT(isR ? "perl_rindex_str" : "perl_index_str", {str, sub, pos});
    }

    /* ── reverse in scalar context = reverse string ──────────────────────── */
    /* (array context is handled in emitArrayPtr) */

    /* ── references ─────────────────────────────────────────────────────── */

    case NK::RefScalar: {
        /* \$x — capture the stable PerlValue* */
        Value *pv = emitExpr(*n.left);
        return callRT("perl_ref_scalar", {pv});
    }

    case NK::RefArray: {
        Value *av = lookupArray(n.name);
        if (!av) av = callRT("perl_array_new", {});
        return callRT("perl_ref_array", {av});
    }

    case NK::RefHash: {
        Value *hv = lookupHash(n.name);
        if (!hv) hv = callRT("perl_hash_new", {});
        return callRT("perl_ref_hash", {hv});
    }

    case NK::AnonArray: {
        Value *av = callRT("perl_array_new", {});
        for (auto &elem : n.args)
            callRT("perl_array_push", {av, emitExpr(*elem)});
        return callRT("perl_ref_array", {av});
    }

    case NK::AnonHash: {
        Value *hv = callRT("perl_hash_new", {});
        Value *listArr = callRT("perl_array_new", {});
        for (auto &elem : n.args)
            callRT("perl_array_push", {listArr, emitExpr(*elem)});
        callRT("perl_hash_from_list", {hv, listArr});
        return callRT("perl_ref_hash", {hv});
    }

    case NK::DerefScalar: {
        Value *ref = emitExpr(*n.left);
        return callRT("perl_deref_scalar", {ref});
    }

    case NK::DerefArray: {
        Value *ref = emitExpr(*n.left);
        return callRT("perl_deref_array", {ref});
    }

    case NK::DerefHash: {
        Value *ref = emitExpr(*n.left);
        return callRT("perl_deref_hash", {ref});
    }

    case NK::ArrowDeref: {
        Value *base = emitExpr(*n.left);
        if (n.sval == "array") {
            Value *av = callRT("perl_deref_array", {base});
            freeIfOwned(base);
            return callRT("perl_array_get_ref", {av, emitIdx(*n.right)});
        } else {
            Value *hv = callRT("perl_deref_hash", {base});
            freeIfOwned(base);
            return emitHashGetRef(hv, *n.right);
        }
    }

    case NK::RefFunc: {
        Value *v = emitExpr(*n.left);
        return callRT("perl_ref_type", {v});
    }

    case NK::RegexMatch: {
        Value *str = emitExpr(*n.left);
        Value *pat = builder_.CreateGlobalStringPtr(n.sval, "re_pat");
        Value *flg = builder_.CreateGlobalStringPtr(n.name, "re_flg");
        bool isG   = n.name.find('g') != std::string::npos;
        Value *res = callRT(isG ? "perl_regex_match_g" : "perl_regex_match", {str, pat, flg});
        return n.ival ? callRT("perl_not", {res}) : res;
    }

    case NK::RegexSubst: {
        Value *str = emitExpr(*n.left);
        size_t sep = n.name.find('\x01');
        std::string repl  = n.name.substr(0, sep);
        std::string flags = n.name.substr(sep + 1);
        Value *pat  = builder_.CreateGlobalStringPtr(n.sval, "rs_pat");
        Value *rep  = builder_.CreateGlobalStringPtr(repl,   "rs_rep");
        Value *flg  = builder_.CreateGlobalStringPtr(flags,  "rs_flg");
        Value *cnt  = callRT("perl_regex_subst", {str, pat, rep, flg});
        return callRT("perl_alloc_int", {cnt});
    }

    case NK::CaptureVar: {
        return callRT("perl_capture", {builder_.getInt64(n.ival)});
    }

    case NK::WarnStmt: {
        Value *msg = n.left ? emitExpr(*n.left) : perlStr("Warning: something's wrong");
        callRT("perl_warn", {msg});
        return perlUndef();
    }

    case NK::SystemFunc: {
        Value *cmd = n.left ? emitExpr(*n.left) : perlStr("");
        return callRT("perl_system", {cmd});
    }

    case NK::BacktickExpr: {
        Value *cmd = n.left ? emitExpr(*n.left) : perlStr("");
        return callRT("perl_backtick", {cmd});
    }

    case NK::FileTestOp: {
        int op = (unsigned char)n.sval[0];
        Value *path = n.left ? emitExpr(*n.left) : perlStr("");
        Value *opv  = ConstantInt::get(Type::getInt32Ty(ctx_), op);
        return callRT("perl_filetest", {opv, path});
    }

    case NK::ArraySlice: {
        Value *av  = lookupArray(n.name);
        Value *res = callRT("perl_array_new", {});
        for (auto &idxNode : n.args) {
            Value *elem = av ? callRT("perl_array_get_ref", {av, emitIdx(*idxNode)}) : perlUndef();
            callRT("perl_array_push", {res, elem});
        }
        return callRT("perl_ref_array", {res});
    }

    case NK::HashSlice: {
        Value *hv  = lookupHash(n.name);
        Value *res = callRT("perl_array_new", {});
        auto pushHashKey2 = [&](const Node &keyNode) {
            if (keyNode.kind == NK::ArrayLit) {
                for (auto &k : keyNode.args) {
                    Value *elem = hv ? emitHashGetRef(hv, *k) : perlUndef();
                    callRT("perl_array_push", {res, elem});
                }
            } else {
                Value *elem = hv ? emitHashGetRef(hv, keyNode) : perlUndef();
                callRT("perl_array_push", {res, elem});
            }
        };
        for (auto &keyNode : n.args) pushHashKey2(*keyNode);
        return callRT("perl_ref_array", {res});
    }

    case NK::SpliceFunc: {
        /* scalar context: return count of removed elements */
        Value *av = emitArrayPtr(n);
        if (!av) return perlUndef();
        return callRT("perl_array_len", {av});
    }

    case NK::DollarAt:
        return callRT("perl_get_dollar_at", {});

    case NK::WantarrayFunc:
        return callRT("perl_wantarray", {});

    case NK::CallerFunc: {
        Value *av = callRT("perl_caller", {});
        return callRT("perl_array_get", {av, ConstantInt::get(Type::getInt64Ty(ctx_), 0)});
    }

    case NK::ChdirFunc:
        return callRT("perl_chdir", {n.left ? emitExpr(*n.left) : perlStr(".")});

    case NK::MkdirFunc: {
        Value *path = n.left ? emitExpr(*n.left) : perlStr(".");
        Value *mode = n.right ? emitExpr(*n.right) : perlUndef();
        return callRT("perl_mkdir_op", {path, mode});
    }

    case NK::RmdirFunc:
        return callRT("perl_rmdir_op", {n.left ? emitExpr(*n.left) : perlStr(".")});

    case NK::RenameFunc: {
        Value *oldp = n.left  ? emitExpr(*n.left)  : perlUndef();
        Value *newp = n.right ? emitExpr(*n.right) : perlUndef();
        return callRT("perl_rename_op", {oldp, newp});
    }

    case NK::ChmodFunc: {
        Value *mode = n.left ? emitExpr(*n.left) : perlUndef();
        Value *av   = callRT("perl_array_new", {});
        for (auto &a : n.args) callRT("perl_array_push", {av, emitExpr(*a)});
        return callRT("perl_chmod_op", {mode, av});
    }

    case NK::OpendirFunc: {
        /* opendir(my $dh, path) — declare/find $dh, call opendir_fh */
        Value *slot = lookupVar(n.name);
        if (!slot) {
            Value *uv = callRT("perl_alloc_undef", {});
            slot = builder_.CreateAlloca(perlPtrTy_, nullptr, ("$" + n.name).c_str());
            builder_.CreateStore(uv, slot);
            declareVar(n.name, slot);
        }
        Value *dh   = builder_.CreateLoad(perlPtrTy_, slot);
        Value *path = n.left ? emitExpr(*n.left) : perlStr(".");
        return callRT("perl_opendir_fh", {dh, path});
    }

    case NK::ReaddirFunc: {
        Value *slot = lookupVar(n.name);
        if (!slot) return perlUndef();
        Value *dh = builder_.CreateLoad(perlPtrTy_, slot);
        return callRT("perl_readdir", {dh});
    }

    case NK::ClosedirFunc: {
        Value *slot = lookupVar(n.name);
        if (!slot) return perlUndef();
        Value *dh = builder_.CreateLoad(perlPtrTy_, slot);
        callRT("perl_closedir_fh", {dh});
        return perlUndef();
    }

    case NK::TrOp: {
        Value *str = emitExpr(*n.left);
        size_t s1 = n.sval.find('\x01'), s2 = n.sval.find('\x01', s1 + 1);
        std::string search = n.sval.substr(0, s1);
        std::string repl   = n.sval.substr(s1 + 1, s2 - s1 - 1);
        std::string flags  = n.sval.substr(s2 + 1);
        Value *sv = builder_.CreateGlobalStringPtr(search, "tr_s");
        Value *rv = builder_.CreateGlobalStringPtr(repl,   "tr_r");
        Value *fv = builder_.CreateGlobalStringPtr(flags,  "tr_f");
        Value *cnt = callRT("perl_tr", {str, sv, rv, fv});
        return callRT("perl_alloc_int", {cnt});
    }

    case NK::EvalBlock: {
        /* $@ = "" before eval */
        Value *emptyAt = perlStr("");
        callRT("perl_assign", {callRT("perl_get_dollar_at", {}), emptyAt});

        /* allocate jmp_buf on stack (256 bytes, enough for any platform) */
        auto *i8Arr  = ArrayType::get(Type::getInt8Ty(ctx_), 256);
        auto *jbAlloca = builder_.CreateAlloca(i8Arr, nullptr, "jmp_buf");
        /* cast to ptr for setjmp/perl_eval_push */
        Value *jbPtr = builder_.CreateBitCast(jbAlloca, PointerType::getUnqual(ctx_));
        /* perl_eval_push(jbPtr) — register this jmp_buf */
        callRT("perl_eval_push", {jbPtr});

        /* int caught = setjmp(jbPtr) */
        auto *i32     = Type::getInt32Ty(ctx_);
        Value *caught = callRT("setjmp", {jbPtr});
        auto *fn      = builder_.GetInsertBlock()->getParent();
        Value *isCaught = builder_.CreateICmpNE(caught, ConstantInt::get(i32, 0));
        auto *bodyBB  = BasicBlock::Create(ctx_, "eval.body", fn);
        auto *endBB   = BasicBlock::Create(ctx_, "eval.end",  fn);
        builder_.CreateCondBr(isCaught, endBB, bodyBB);

        builder_.SetInsertPoint(bodyBB);
        if (n.body) emitBlock(*n.body);
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        callRT("perl_eval_pop", {});
        return perlUndef();
    }

    case NK::AnonSub: {
        /* Phase 1: collect captures from outer scopes */
        std::set<std::string> usedNames;
        collectAllScalarNames(*n.body, usedNames);
        std::vector<std::string> captureNames;
        std::vector<Value*>      captureVals;   /* PerlValue* loaded from outer alloca */
        for (auto &nm : usedNames) {
            if (nm == "_") continue;
            if (auto *slot = lookupVar(nm)) {
                captureNames.push_back(nm);
                captureVals.push_back(builder_.CreateLoad(perlPtrTy_, slot));
            }
        }

        /* Phase 2: emit the closure as an internal LLVM function */
        auto *subFT = FunctionType::get(perlPtrTy_,
                          {PointerType::getUnqual(ctx_), Type::getInt32Ty(ctx_)}, false);
        auto *subFn = Function::Create(subFT, Function::InternalLinkage,
                                       subLLVMName(n.name), mod_.get());
        /* save codegen state */
        auto *savedFn          = currentFn_;
        auto *savedBB          = builder_.GetInsertBlock();
        auto  savedScopes      = scopes_;
        auto  savedArrScopes   = arrayScopes_;
        auto  savedHashScopes  = hashScopes_;
        auto  savedPvScopes    = pvScopes_;
        auto  savedFloatScopes = floatScopes_;
        auto  savedIntScopes   = intScopes_;
        auto *savedLocalDepth  = localDepthAlloca_;
        auto *savedSubBody     = currentSubBody_;
        /* emit sub entry */
        auto *subEntry = BasicBlock::Create(ctx_, "entry", subFn);
        builder_.SetInsertPoint(subEntry);
        currentFn_ = subFn;
        scopes_ = {}; arrayScopes_ = {}; hashScopes_ = {}; pvScopes_ = {}; floatScopes_ = {}; intScopes_ = {};
        pushScope();
    Value *argsArr = subFn->getArg(0);
    argsArr->setName("args");
    Value *ctxArg = subFn->getArg(1);
    callRT("perl_push_wantarray", {ctxArg});
    declareArray("_", argsArr);
        /* fresh local() depth for this closure */
        {
            auto *i32Ty = Type::getInt32Ty(ctx_);
            localDepthAlloca_ = builder_.CreateAlloca(i32Ty, nullptr, "local.depth");
            builder_.CreateStore(callRT("perl_local_save_depth", {}), localDepthAlloca_);
        }
        /* Phase 3: initialise captured variables as local allocas */
        auto i64Ty = Type::getInt64Ty(ctx_);
        for (size_t i = 0; i < captureNames.size(); i++) {
            Value *pv = callRT("perl_get_capture",
                               {ConstantInt::get(i64Ty, (long long)i)});
            auto *alloca = builder_.CreateAlloca(perlPtrTy_, nullptr, captureNames[i]);
            builder_.CreateStore(pv, alloca);
            declareVar(captureNames[i], alloca);
        }
        currentSubBody_ = n.body.get();
        emitBlock(*n.body);
        currentSubBody_ = savedSubBody;
        if (!builder_.GetInsertBlock()->getTerminator()) {
            auto *i32Ty = Type::getInt32Ty(ctx_);
            Value *depth = builder_.CreateLoad(i32Ty, localDepthAlloca_);
            callRT("perl_pop_wantarray", {});
            callRT("perl_local_restore_to", {depth});
            builder_.CreateRet(perlUndef());
        }
        popScope();
        /* restore state */
        currentFn_        = savedFn;
        builder_.SetInsertPoint(savedBB);
        scopes_           = std::move(savedScopes);
        arrayScopes_      = std::move(savedArrScopes);
        hashScopes_       = std::move(savedHashScopes);
        pvScopes_         = std::move(savedPvScopes);
        floatScopes_      = std::move(savedFloatScopes);
        intScopes_        = std::move(savedIntScopes);
        localDepthAlloca_ = savedLocalDepth;
        currentSubBody_   = savedSubBody;

        /* Phase 4: build captures array and return closure (or plain code ref) */
        Value *fnPtr = ConstantExpr::getPointerCast(subFn, PointerType::getUnqual(ctx_));
        if (captureNames.empty())
            return callRT("perl_make_code_ref", {fnPtr});
        Value *capsAv = callRT("perl_array_new", {});
        for (auto *pv : captureVals)
            callRT("perl_array_push", {capsAv, pv});
        return callRT("perl_make_closure", {fnPtr, capsAv});
    }

    case NK::RefSub: {
        auto *subFn = mod_->getFunction(subLLVMName(n.name));
        if (!subFn) return perlUndef();
        Value *fnPtr = ConstantExpr::getPointerCast(subFn, PointerType::getUnqual(ctx_));
        return callRT("perl_make_code_ref", {fnPtr});
    }

    case NK::CallCodeRef: {
        Value *ref = emitExpr(*n.left);
        Value *av  = callRT("perl_array_new", {});
        for (auto &arg : n.args) {
            Value *src = emitArrayPtr(*arg);
            if (src) callRT("perl_array_extend", {av, src});
            else     callRT("perl_array_push",   {av, emitExpr(*arg)});
        }
        return callRT("perl_call_code_ref", {ref, av});
    }

    case NK::PackageStmt:
        return perlUndef();

    case NK::BlessFunc: {
        Value *ref = emitExpr(*n.left);
        Value *cls = emitExpr(*n.right);
        return callRT("perl_bless", {ref, cls});
    }

    case NK::SetIsa: {
        Value *child  = builder_.CreateGlobalStringPtr(n.name);
        Value *parent = builder_.CreateGlobalStringPtr(n.sval);
        callRT("perl_set_isa", {child, parent});
        return perlUndef();
    }

    case NK::MethodCall: {
        Value *obj = emitExpr(*n.left);
        Value *argsArr = callRT("perl_array_new", {});
        for (auto &arg : n.args) {
            if (arg->kind == NK::ArrayVar) {
                Value *av = lookupArray(arg->name);
                if (av) { callRT("perl_array_extend", {argsArr, av}); continue; }
            }
            Value *v = emitExpr(*arg);
            callRT("perl_array_push", {argsArr, v});
        }
        /* SUPER::method — dispatch starting from parent of caller package */
        if (n.sval.size() > 7 && n.sval.substr(0, 7) == "SUPER::") {
            std::string realMethod = n.sval.substr(7);
            Value *callerPkg  = builder_.CreateGlobalStringPtr(n.name);
            Value *methodStr  = builder_.CreateGlobalStringPtr(realMethod);
            return callRT("perl_dispatch_method_super", {obj, callerPkg, methodStr, argsArr});
        }
        Value *methodStr = builder_.CreateGlobalStringPtr(n.sval);
    if (n.sval == "threads" && n.name == "create") {
      // special: threads->create(sub{...})
      Value *argsArr = callRT("perl_array_new", {});
      for (auto &arg : n.args) {
        Value *subArr = emitArrayPtr(*arg);
        if (subArr) callRT("perl_array_extend", {argsArr, subArr});
        else callRT("perl_array_push", {argsArr, emitExpr(*arg)});
      }
      return callRT("perl_threads_create", {methodStr, argsArr});
    }
        return callRT("perl_dispatch_method", {obj, methodStr, argsArr});
    }

    default:
        return perlUndef();
    }
}

Value *CodeGen::emitLValue(const Node &n) {
    switch (n.kind) {
    case NK::ScalarVar: {
        auto *slot = lookupVar(n.name);
        if (!slot) {
            /* auto-vivify global-ish variable in current scope */
            auto *alloca = builder_.CreateAlloca(perlPtrTy_, nullptr, n.name);
            builder_.CreateStore(perlUndef(), alloca);
            declareVar(n.name, alloca);
            return alloca;
        }
        return slot;
    }
    case NK::ArrayElem: {
        /* returns nullptr — array element assignment handled separately */
        return nullptr;
    }
    default: return nullptr;
    }
}

Value *CodeGen::emitBinOp(const Node &n) {
    /* Fast path: stay unboxed for integer arithmetic */
    if (n.sval == "+" || n.sval == "-" || n.sval == "*" || n.sval == "%") {
        if (Value *iv = emitExprI64(n))
            return boxI64(iv);
    }
    /* Fast path: if both operands can be expressed as doubles, stay unboxed */
    if (n.sval == "+" || n.sval == "-" || n.sval == "*" || n.sval == "/") {
        if (Value *fv = emitExprF64(n))
            return boxF64(fv);
    }
    /* short-circuit ops */
    if (n.sval == "&&") {
        auto *fn   = builder_.GetInsertBlock()->getParent();
        auto *rhsBB  = BasicBlock::Create(ctx_, "and.rhs",  fn);
        auto *endBB  = BasicBlock::Create(ctx_, "and.end",  fn);
        Value *lv  = emitExpr(*n.left);
        Value *lb  = callRT("perl_is_true", {lv});
        Value *ltrue = builder_.CreateICmpNE(lb, ConstantInt::get(Type::getInt32Ty(ctx_), 0));
        auto *lBB  = builder_.GetInsertBlock();
        builder_.CreateCondBr(ltrue, rhsBB, endBB);

        builder_.SetInsertPoint(rhsBB);
        Value *rv = emitExpr(*n.right);
        auto *rBB = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        auto *phi = builder_.CreatePHI(perlPtrTy_, 2, "and.result");
        phi->addIncoming(lv, lBB);
        phi->addIncoming(rv, rBB);
        return phi;
    }
    if (n.sval == "||") {
        auto *fn   = builder_.GetInsertBlock()->getParent();
        auto *rhsBB  = BasicBlock::Create(ctx_, "or.rhs", fn);
        auto *endBB  = BasicBlock::Create(ctx_, "or.end", fn);
        Value *lv  = emitExpr(*n.left);
        Value *lb  = callRT("perl_is_true", {lv});
        Value *ltrue = builder_.CreateICmpNE(lb, ConstantInt::get(Type::getInt32Ty(ctx_), 0));
        auto *lBB  = builder_.GetInsertBlock();
        builder_.CreateCondBr(ltrue, endBB, rhsBB);

        builder_.SetInsertPoint(rhsBB);
        Value *rv = emitExpr(*n.right);
        auto *rBB = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        auto *phi = builder_.CreatePHI(perlPtrTy_, 2, "or.result");
        phi->addIncoming(lv, lBB);
        phi->addIncoming(rv, rBB);
        return phi;
    }
    /* defined-or: $a // $b — return $a if defined, else $b */
    if (n.sval == "//") {
        auto *fn    = builder_.GetInsertBlock()->getParent();
        auto *rhsBB = BasicBlock::Create(ctx_, "defor.rhs", fn);
        auto *endBB = BasicBlock::Create(ctx_, "defor.end", fn);
        Value *lv   = emitExpr(*n.left);
        Value *lb   = callRT("perl_defined", {lv});
        Value *ldef = builder_.CreateICmpNE(lb, ConstantInt::get(Type::getInt32Ty(ctx_), 0));
        auto *lBB   = builder_.GetInsertBlock();
        builder_.CreateCondBr(ldef, endBB, rhsBB);

        builder_.SetInsertPoint(rhsBB);
        Value *rv = emitExpr(*n.right);
        auto *rBB = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        auto *phi = builder_.CreatePHI(perlPtrTy_, 2, "defor.result");
        phi->addIncoming(lv, lBB);
        phi->addIncoming(rv, rBB);
        return phi;
    }
    /* ternary */
    if (n.sval == "?:") {
        auto *fn    = builder_.GetInsertBlock()->getParent();
        auto *thenBB = BasicBlock::Create(ctx_, "tern.then", fn);
        auto *elseBB = BasicBlock::Create(ctx_, "tern.else", fn);
        auto *endBB  = BasicBlock::Create(ctx_, "tern.end",  fn);
        Value *cv   = emitExpr(*n.cond);
        Value *cb   = callRT("perl_is_true", {cv});
        Value *ctrue = builder_.CreateICmpNE(cb, ConstantInt::get(Type::getInt32Ty(ctx_), 0));
        builder_.CreateCondBr(ctrue, thenBB, elseBB);

        builder_.SetInsertPoint(thenBB);
        Value *tv = emitExpr(*n.left);
        auto *tb  = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(elseBB);
        Value *ev = emitExpr(*n.right);
        auto *eb  = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        auto *phi = builder_.CreatePHI(perlPtrTy_, 2, "tern.result");
        phi->addIncoming(tv, tb);
        phi->addIncoming(ev, eb);
        return phi;
    }

    Value *lv = emitExpr(*n.left);
    Value *rv = emitExpr(*n.right);

    static const struct { const char *op; const char *rt; } OPS[] = {
        {"+",  "perl_add"   }, {"-",  "perl_sub"   }, {"*",  "perl_mul"   },
        {"/",  "perl_div"   }, {"%",  "perl_mod"   }, {"**", "perl_pow"   },
        {".",  "perl_concat"},
        {"==", "perl_num_eq"}, {"!=", "perl_num_ne"},
        {"<",  "perl_num_lt"}, {">",  "perl_num_gt"},
        {"<=", "perl_num_le"}, {">=", "perl_num_ge"},
        {"eq", "perl_str_eq"}, {"ne", "perl_str_ne"},
        {"lt", "perl_str_lt"}, {"gt", "perl_str_gt"},
        {"le", "perl_str_le"}, {"ge", "perl_str_ge"},
        {"x",  "perl_repeat_str"},
        {"<=>","perl_spaceship"}, {"cmp","perl_str_spaceship"},
        {nullptr, nullptr}
    };
    for (auto *p = OPS; p->op; p++) {
        if (n.sval == p->op) {
            Value *result = callRT(p->rt, {lv, rv});
            freeIfOwned(lv);
            freeIfOwned(rv);
            return result;
        }
    }

    return perlUndef();
}

Value *CodeGen::emitCall(const Node &n) {
    /* eval EXPR — string eval stub: returns undef */
    if (n.name == "eval") {
        /* allocate jmp_buf on stack (256 bytes) */
        auto *i8Arr  = ArrayType::get(Type::getInt8Ty(ctx_), 256);
        auto *jbAlloca = builder_.CreateAlloca(i8Arr, nullptr, "jmp_buf");
        Value *jbPtr = builder_.CreateBitCast(jbAlloca, PointerType::getUnqual(ctx_));
        callRT("perl_eval_push", {jbPtr});
        auto *i32 = Type::getInt32Ty(ctx_);
        Value *caught = callRT("setjmp", {jbPtr});
        Value *isCaught = builder_.CreateICmpNE(caught, ConstantInt::get(i32, 0));
        auto *fn = builder_.GetInsertBlock()->getParent();
        auto *bodyBB = BasicBlock::Create(ctx_, "eval.body", fn);
        auto *endBB = BasicBlock::Create(ctx_, "eval.end", fn);
        builder_.CreateCondBr(isCaught, endBB, bodyBB);
        builder_.SetInsertPoint(bodyBB);
        for (auto &arg : n.args) emitExpr(*arg);  /* evaluate string arg */
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        callRT("perl_eval_pop", {});
        return perlUndef();
    }
    if (auto *fn = mod_->getFunction(subLLVMName(n.name))) {
        Value *argsArr = callRT("perl_array_new", {});
        for (auto &arg : n.args) {
            /* @arr and %hash are splatted into @_ (flattened) */
            if (arg->kind == NK::ArrayVar) {
                Value *av = lookupArray(arg->name);
                if (av) { callRT("perl_array_extend", {argsArr, av}); continue; }
            }
            if (arg->kind == NK::HashVar) {
                Value *hv = lookupHash(arg->name);
                if (hv) { callRT("perl_array_extend_hash", {argsArr, hv}); continue; }
            }
            Value *v = emitExpr(*arg);
            callRT("perl_array_push", {argsArr, v});
            freeIfOwned(v);
        }
        auto *i32Ty = Type::getInt32Ty(ctx_);
        Value *zero = ConstantInt::get(i32Ty, 0);
        Value *retVal = builder_.CreateCall(fn, {argsArr, zero});
        callRT("perl_array_free", {argsArr});
        return retVal;
    }
    return perlUndef();
}

/* ── optimization ────────────────────────────────────────────────────────── */

void CodeGen::runOptimization() {
    if (optLevel_ <= 0) return;

    if (verifyModule(*mod_, &errs())) {
        errs() << "IR verification failed before optimization\n";
        return;
    }

    PassBuilder PB;
    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    OptimizationLevel level = OptimizationLevel::O1;
    if (optLevel_ >= 2) level = OptimizationLevel::O2;
    if (optLevel_ >= 3) level = OptimizationLevel::O3;

    ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(level);
    MPM.run(*mod_, MAM);
}

/* ── output ──────────────────────────────────────────────────────────────── */

std::unique_ptr<Module> CodeGen::releaseModule() {
    return std::move(mod_);
}

void CodeGen::dumpIR() {
    mod_->print(outs(), nullptr);
}

void CodeGen::writeIR(const std::string &path) {
    runOptimization();
    std::error_code ec;
    raw_fd_ostream out(path, ec);
    if (ec) throw std::runtime_error("Cannot write " + path + ": " + ec.message());
    mod_->print(out, nullptr);
}

void CodeGen::writeBC(const std::string &path) {
    runOptimization();
    std::error_code ec;
    raw_fd_ostream out(path, ec);
    if (ec) throw std::runtime_error("Cannot write " + path + ": " + ec.message());
    WriteBitcodeToFile(*mod_, out);
}

void CodeGen::initializeDebugInfo(const std::string &sourceFile) {
    std::string dir = ".";
    size_t slash = sourceFile.rfind('/');
    if (slash != std::string::npos) {
        dir = sourceFile.substr(0, slash);
    }
    std::string filename = (slash != std::string::npos) ? sourceFile.substr(slash + 1) : sourceFile;
    file_ = dib_->createFile(filename, dir);

    cu_ = dib_->createCompileUnit(
        llvm::dwarf::DW_LANG_C, file_, "perlc", false, "", 0);

    auto *intTy = dib_->createBasicType("int", 32, llvm::dwarf::DW_ATE_signed);
    auto *subTy = dib_->createSubroutineType(dib_->getOrCreateTypeArray(intTy));
    currentSP_ = dib_->createFunction(
        cu_, "main", "main", file_, 1, subTy, 1,
        llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);
}

llvm::DILocation *CodeGen::getDebugLoc(int line, llvm::DIScope *scope) {
    if (!scope) scope = currentSP_;
    return llvm::DILocation::get(ctx_, line, 0, scope);
}
