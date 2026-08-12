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
#include <set>
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
    : ctx_owned_(std::make_unique<LLVMContext>()),
      ctx_(*ctx_owned_),
      debug_(debug),
      optLevel_(optLevel),
      mod_(std::make_unique<Module>("perlc", ctx_)),
      builder_(ctx_) {
    perlPtrTy_  = PointerType::getUnqual(ctx_);
    arrayPtrTy_ = PointerType::getUnqual(ctx_);
    declareRuntime();

    /* Build TBAA type hierarchy so LLVM can prove PerlValue tag/fval stores
       don't alias PerlArray.elems loads — enables CSE of the elems pointer
       across velocity update stores in the nbody inner loop.
       createTBAAStructTypeNode requires the access type to appear as a field
       in the base struct at the specified offset; using scalar nodes as the
       base would cause the LLVM verifier to reject the metadata. */
    MDBuilder mdb(ctx_);
    MDNode *tbaaRoot = mdb.createTBAARoot("PerlTBAA");
    MDNode *ptrLeaf  = mdb.createTBAAScalarTypeNode("pointer", tbaaRoot);
    MDNode *i32Leaf  = mdb.createTBAAScalarTypeNode("int32",   tbaaRoot);
    MDNode *f64Leaf  = mdb.createTBAAScalarTypeNode("float64", tbaaRoot);
    MDNode *i64Leaf  = mdb.createTBAAScalarTypeNode("int64",   tbaaRoot);
    /* PerlArray: { PerlValue **elems (ptr,0), long long len (i64,8), cap (i64,16) } */
    MDNode *avStruct = mdb.createTBAAStructTypeNode("PerlArray",
        {{ptrLeaf, 0}, {i64Leaf, 8}, {i64Leaf, 16}});
    /* PerlValue: { PerlTag tag (i32,0), [pad 4], union fval/pval (f64,8), ... }
       Use f64Leaf for offset 8; pval loads at offset 8 are not tagged (null). */
    MDNode *pvStruct = mdb.createTBAAStructTypeNode("PerlValue",
        {{i32Leaf, 0}, {f64Leaf, 8}});
    tbaaAvElemsTag_ = mdb.createTBAAStructTagNode(avStruct, ptrLeaf, 0);
    tbaaPvTagTag_   = mdb.createTBAAStructTagNode(pvStruct, i32Leaf, 0);
    tbaaPvFvalTag_  = mdb.createTBAAStructTagNode(pvStruct, f64Leaf, 8);
    /* Scalar TBAA type for PerlValue* pointers stored in elems[].
       Sibling of i32/f64 under PerlTBAA root — no aliasing with PerlValue
       tag/fval stores, so LLVM can keep PV* values (mass, velocity slot) in
       registers across velocity update stores. */
    MDNode *pvPtrLeaf = mdb.createTBAAScalarTypeNode("PerlValuePtr", tbaaRoot);
    tbaaAvElemTag_  = mdb.createTBAAStructTagNode(pvPtrLeaf, pvPtrLeaf, 0);
    /* Flat double array elements — sibling of PerlValue fields under root,
       so flat double loads don't alias PV tag/fval stores. LLVM can then
       hoist constant-index flat-row loads out of the inner j loop. */
    MDNode *flatDblLeaf = mdb.createTBAAScalarTypeNode("flat_double", tbaaRoot);
    tbaaFlatDoubleTag_  = mdb.createTBAAStructTagNode(flatDblLeaf, flatDblLeaf, 0);

    if (debug_) {
        dib_ = std::make_unique<DIBuilder>(*mod_);
    }
}

void CodeGen::setTBAA(Value *v, MDNode *tag) {
    if (auto *inst = dyn_cast<Instruction>(v))
        inst->setMetadata(LLVMContext::MD_tbaa, tag);
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
    RT("perl_print_array",   voidTy, av);
    RT("perl_current_wantarray_ctx", Type::getInt32Ty(ctx_));
    RT("perl_add",           pv,  pv, pv);
    RT("perl_sub",           pv,  pv, pv);
    RT("perl_mul",           pv,  pv, pv);
    RT("perl_div",           pv,  pv, pv);
    RT("perl_mod",           pv,  pv, pv);
    RT("perl_pow",           pv,  pv, pv);
    RT("perl_negate",        pv,  pv);
    RT("perl_bitand",        pv,  pv, pv);
    RT("perl_bitor",         pv,  pv, pv);
    RT("perl_bitxor",        pv,  pv, pv);
    RT("perl_bitnot",        pv,  pv);
    RT("perl_lshift",        pv,  pv, pv);
    RT("perl_rshift",        pv,  pv, pv);
    RT("perl_concat",        pv,  pv, pv);
    RT("perl_repeat_str",    pv,  pv, pv);
    RT("perl_substr_replace",voidTy, pv, pv, pv, pv);
    RT("perl_repeat_list",   av,  av, pv);
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
    RT("perl_array_new",      av);
    RT("perl_anon_array_new", av);
    RT("perl_array_free",    voidTy, av);
    RT("perl_array_free_nc", voidTy, av);
    RT("perl_array_push",    voidTy, av, pv);
    RT("perl_array_push_nc", voidTy, av, pv);
    RT("perl_array_push_capture", voidTy, av, pv);
    RT("perl_array_pop",     pv,  av);
    RT("perl_array_get",     pv,  av, i64);
    RT("perl_array_get_ref",      pv,     av, i64);
    RT("perl_array_set",          voidTy, av, i64, pv);
    RT("perl_array_update_float", voidTy, av, i64, Type::getDoubleTy(ctx_));
    RT("perl_array_is_all_flat", i64, av);  /* Stage 23: 1 if every elem is FLAT_ARRAY */
    RT("perl_array_len",     pv,  av);
    RT("perl_array_clear",   voidTy, av);
    RT("perl_array_replace", voidTy, av, av);
    RT("perl_hash_clear",    voidTy, av);
    /* hash */
    RT("perl_hash_new",      av);   /* reuse av as opaque ptr */
    RT("perl_anon_hash_new", av);
    RT("perl_hash_get_sv",       pv,  av, pv);
    RT("perl_hash_get_sv_ref",   pv,  av, pv);
    RT("perl_hash_set_sv",       voidTy, av, pv, pv);
    RT("perl_hash_exists_sv",    Type::getInt32Ty(ctx_), av, pv);
    RT("perl_hash_delete_sv",    pv,  av, pv);
    /* constant C-string key variants — no strdup overhead */
    auto *strPtrTy = PointerType::get(Type::getInt8Ty(ctx_), 0);
    RT("perl_hash_get_str_ref",  pv,  av, strPtrTy);
    RT("perl_hash_lvalue_str",   pv,  av, strPtrTy);
    RT("perl_hash_lvalue_sv",    pv,  av, pv);
    RT("perl_hash_set_str",      voidTy, av, strPtrTy, pv);
    RT("perl_hash_exists_str",   Type::getInt32Ty(ctx_), av, strPtrTy);
    RT("perl_hash_delete_str",   pv,  av, strPtrTy);
    RT("perl_hash_keys",     av,  av);
    RT("perl_hash_slice",    av,  av, av);
    RT("perl_hash_values",   av,  av);
    RT("perl_hash_size",     pv,  av);
    RT("perl_hash_from_list",voidTy, av, av);
    RT("perl_hash_autoviv_hash",    av, av, strPtrTy);
    RT("perl_hash_autoviv_hash_sv", av, av, pv);
    RT("perl_hash_autoviv_array",   av, av, strPtrTy);
    RT("perl_hash_autoviv_array_sv",av, av, pv);
    RT("perl_array_autoviv_hash",   av, av, i64);
    RT("perl_array_autoviv_array",  av, av, i64);
    RT("perl_hash_assign_slice",    voidTy, av, av, av);
    RT("perl_array_assign_slice",   voidTy, av, av, av);
    RT("perl_array_sort_str",   voidTy, av);
    RT("perl_array_extend",     voidTy, av, av);
    RT("perl_array_extend_hash",voidTy, av, av);
    RT("perl_array_push_list_or_scalar", voidTy, av, pv);
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
    RT("perl_alloc_flat_array", pv, i64);
    RT("perl_alloc_float_array", pv, i64);
    RT("perl_alloc_float_pair", pv, Type::getDoubleTy(ctx_), Type::getDoubleTy(ctx_));
    RT("perl_ref_scalar",   pv, pv);
    RT("perl_ref_array",    pv, av);
    RT("perl_ref_hash",     pv, av);  /* PerlHash* treated as opaque av */
    RT("perl_deref_scalar", pv, pv);
    RT("perl_deref_array",    av, pv);
    RT("perl_deref_array_ro", av, pv);
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
    RT("perl_eval_string",     pv,  pv);
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
    RT("perl_local_save_array", voidTy, PointerType::getUnqual(ctx_));
    RT("perl_local_save_hash",  voidTy, PointerType::getUnqual(ctx_));
    RT("perl_get_autoload_name", pv);
    RT("perl_set_pos_str",      voidTy, pv, pv);
    RT("perl_runtime_require",  pv, i8p);
    RT("perl_do_file",          pv, pv);
    RT("perl_call_named_sub",   pv, i8p, av, Type::getInt32Ty(ctx_));
    RT("perl_xs_load_library",  pv, pv);
    RT("perl_xs_call_dynamic",  pv, pv, pv, pv, av);
    RT("perl_dbi_connect",      pv, pv, pv, pv);
    RT("perl_local_restore_to", voidTy, Type::getInt32Ty(ctx_));
    /* special globals */
    RT("perl_get_input_sep",    pv);
    RT("perl_get_dollar_bang",  pv);
    RT("perl_push_wantarray", i32, i32);
RT("perl_pop_wantarray",  i32);
RT("perl_wantarray",             pv);
    RT("perl_array_to_list_return",  pv, av);
    RT("perl_grep_list_return",      pv, av);
    RT("perl_map_list_return",       pv, av);
    RT("perl_sort_list_return",      pv, av);
RT("perl_unwrap_list_return",    av, pv);
RT("perl_threads_join",   voidTy, pv);
    RT("perl_caller",           av,     Type::getInt32Ty(ctx_));
    RT("perl_push_call_frame",  voidTy, i8p, i8p, Type::getInt32Ty(ctx_));
    RT("perl_pop_call_frame",   voidTy);
RT("perl_get_plus_hash",     av);
    RT("perl_plus_hash_get",     pv, pv);
    RT("perl_plus_hash_keys",    av);
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
    /* time / randomness / sleep */
    RT("perl_rand_val",     pv,     pv);
    RT("perl_srand_val",    voidTy, pv);
    RT("perl_time_val",     pv);
    RT("perl_localtime_val",av,     pv);
    RT("perl_gmtime_val",   av,     pv);
    RT("perl_sleep_val",    pv,     pv);
    RT("perl_alarm_val",    pv,     pv);
    /* List::Util */
    RT("perl_sum_list",     pv,  av);
    RT("perl_min_list",     pv,  av);
    RT("perl_max_list",     pv,  av);
    RT("perl_uniq_list",    av,  av);
    /* sort with custom comparator — fn ptr passed as i8p (opaque pointer) */
    RT("perl_sort_custom",  av,  av, i8p);
    /* special globals (Tier 2) */
    RT("perl_get_dollar_dot",   pv);
    RT("perl_get_dollar_comma", pv);
    RT("perl_get_dollar_bsl",   pv);
    RT("perl_get_dollar_amp",   pv);
    RT("perl_print_sep",        voidTy);
    RT("perl_print_sep_fh",     voidTy, pv);
    RT("perl_print_ors",        voidTy);
    RT("perl_print_ors_fh",     voidTy, pv);
    /* POSIX */
    RT("perl_posix_floor",      pv, pv);
    RT("perl_posix_ceil",       pv, pv);
    RT("perl_posix_fmod",       pv, pv, pv);
    RT("perl_posix_strftime",   pv, av);
    /* Scalar::Util */
    RT("perl_su_blessed",              pv, pv);
    RT("perl_su_reftype",              pv, pv);
    RT("perl_su_looks_like_number",    pv, pv);
    /* Carp */
    RT("perl_carp_croak",       voidTy, av);
    RT("perl_carp_carp",        voidTy, av);
    /* File I/O (Tier 2) */
    RT("perl_seek_fh",          pv, pv, pv, pv);
    RT("perl_tell_fh",          pv, pv);
    RT("perl_binmode_fh",       pv, pv, pv);
    /* Filesystem (Tier 2) */
    RT("perl_stat_path",        av, pv);
    RT("perl_lstat_path",       av, pv);
    RT("perl_glob_val",         av, pv);
    /* UNIVERSAL */
    RT("perl_isa_check",        pv, pv, pv);
    RT("perl_can_check",        pv, pv, pv);
    /* threads::shared */
    RT("perl_make_shared_scalar", pv);
    RT("perl_lock_shared",        voidTy, pv);
    RT("perl_lock_array",         voidTy, av);
    RT("perl_lock_hash",          voidTy, av);
    RT("perl_array_make_shared",  voidTy, av);
    RT("perl_hash_make_shared",   voidTy, av);
    RT("perl_cond_wait",          voidTy, pv);
    RT("perl_cond_signal",        voidTy, pv);
    RT("perl_cond_broadcast",     voidTy, pv);
    /* Phase 3: atomic primitives for shared scalars.  perl_atomic_load
       takes a pv and returns the same pointer (the load is the
       cell-pointer itself; the function is essentially an acquire fence
       around the codegen's plain load).  perl_atomic_store/inc/dec/add
       do refcount + write with a release fence.  perl_atomic_add is
       called for `$shared += N`; it takes the lazy-installed SharedMutex
       to make the RMW atomic. */
    RT("perl_atomic_load",        pv,   pv);
    RT("perl_atomic_store",       pv,   pv, pv);
    RT("perl_atomic_inc",         pv,   pv);
    RT("perl_atomic_dec",         pv,   pv);
    RT("perl_atomic_add",         pv,   pv, pv);
    RT("perl_atomic_rmw",         pv,   pv, pv, i32);
    /* threads */
    RT("perl_threads_create",   pv, pv, av);
    RT("perl_threads_join",     pv, pv);
    RT("perl_threads_detach",   voidTy, pv);
    RT("perl_threads_tid",      pv, pv);
    RT("perl_threads_self",     pv);
    RT("perl_threads_list",     av);
    RT("perl_threads_yield",    voidTy);
    /* Tier 3 */
    RT("perl_read_fh",          pv, pv, pv, pv, pv);
    RT("perl_fileno_fh",        pv, pv);
    RT("perl_truncate_fh",      pv, pv, pv);
    RT("perl_each_hash",        av, av);
    RT("perl_pos_str",          pv, pv);
    RT("perl_getpid",           pv);
    RT("perl_get_os_name",      pv);
#undef RT

    /* Mark pure read-only functions so GVN/LICM can eliminate redundant calls */
    for (const char *nm : {"perl_array_get_ref", "perl_to_float", "perl_to_int",
                            "perl_array_len", "perl_deref_array_ro"}) {
        auto *F = getRTFunc(nm);
        F->setMemoryEffects(MemoryEffects::readOnly());
        F->addFnAttr(Attribute::NoUnwind);
        F->addFnAttr(Attribute::WillReturn);
    }
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
    derefAVScopes_.emplace_back(); rowAVScopes_.emplace_back();
    flatRowScopes_.emplace_back();
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
    derefAVScopes_.pop_back(); rowAVScopes_.pop_back();
    flatRowScopes_.pop_back();
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
    /* Stage 32: inside a loop, defer free for loop-invariant PVs.
       undef PVs (perl_alloc_undef) are always loop-invariant since the
       allocation is loop-invariant.  Deref results from loop-invariant
       variables are also loop-invariant.  By deferring the free, LLVM's
       LICM can hoist the alloc_undef out of the loop (no free blocks it). */
    if (!loopExits_.empty() && pv) {
        auto *ci = dyn_cast<CallInst>(pv);
        if (ci) {
            auto *fn = ci->getCalledFunction();
            if (fn) {
                StringRef nm = fn->getName();
                /* undef PV from perl_alloc_undef — always loop-invariant */
                if (nm == "perl_alloc_undef") {
                    trackLoopInvariantPV(pv);
                    return;
                }
                /* Deref result from perl_deref_array on a loop-invariant var —
                   the PV itself is loop-invariant when cached via emitHoistedDerefs.
                   Defer the free to let LICM hoist the call. */
                if (nm == "perl_deref_array") {
                    trackLoopInvariantPV(pv);
                    return;
                }
            }
        }
    }
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
    /* LO..HI range in list context → perl_range returns PerlArray* */
    if (n.kind == NK::Range) {
        Value *lo = emitExpr(*n.left);
        Value *hi = emitExpr(*n.right);
        return callRT("perl_range", {lo, hi});
    }
    /* (LIST) x N — list repetition in array context */
    if (n.kind == NK::BinOp && n.sval == "x" && n.left) {
        Value *srcArr = emitArrayPtr(*n.left);
        if (!srcArr) {
            /* scalar or single-element expr — wrap in 1-element array */
            srcArr = callRT("perl_array_new", {});
            Value *v = emitExpr(*n.left);
            callRT("perl_array_push", {srcArr, v});
            freeIfOwned(v);
        }
        Value *nv = emitExpr(*n.right);
        return callRT("perl_repeat_list", {srcArr, nv});
    }
    if (n.kind == NK::ArrayVar) {
        return lookupArray(n.name);
    }
    if (n.kind == NK::KeysFunc) {
        Value *av;
        if (n.left) {                      /* keys %{$ref} or keys %$ref */
            Value *ref = emitExpr(*n.left);
            Value *h   = callRT("perl_deref_hash", {ref});
            freeIfOwned(ref);
            av = callRT("perl_hash_keys", {h});
        } else if (n.name == "+") {
            av = callRT("perl_plus_hash_keys", {});
        } else {
            Value *h = lookupHash(n.name);
            if (!h) return callRT("perl_array_new", {});
            av = callRT("perl_hash_keys", {h});
        }
        if (!n.sval.empty()) callRT("perl_array_sort_str", {av}); /* "sort" flag */
        return av;
    }
    if (n.kind == NK::ValuesFunc) {
        if (n.left) {                      /* values %{$ref} or values %$ref */
            Value *ref = emitExpr(*n.left);
            Value *h   = callRT("perl_deref_hash", {ref});
            freeIfOwned(ref);
            return callRT("perl_hash_values", {h});
        }
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
    /* stat / lstat in list context → 13-element array */
    if (n.kind == NK::StatFunc) {
        Value *path = n.left ? emitExpr(*n.left) : perlUndef();
        return callRT("perl_stat_path", {path});
    }
    if (n.kind == NK::LstatFunc) {
        Value *path = n.left ? emitExpr(*n.left) : perlUndef();
        return callRT("perl_lstat_path", {path});
    }
    /* glob in list context */
    if (n.kind == NK::GlobFunc) {
        Value *pat = n.left ? emitExpr(*n.left) : perlUndef();
        return callRT("perl_glob_val", {pat});
    }
    /* each %hash in list context → (key, val) pair */
    if (n.kind == NK::EachFunc) {
        Value *hv = lookupHash(n.name);
        if (!hv) return callRT("perl_array_new", {});
        return callRT("perl_each_hash", {hv});
    }
    /* localtime / gmtime in list context → 9-element array */
    if (n.kind == NK::LocaltimeFunc) {
        Value *t = n.left ? emitExpr(*n.left) : perlUndef();
        return callRT("perl_localtime_val", {t});
    }
    if (n.kind == NK::GmtimeFunc) {
        Value *t = n.left ? emitExpr(*n.left) : perlUndef();
        return callRT("perl_gmtime_val", {t});
    }
    /* uniq in list context */
    if (n.kind == NK::UniqFunc) {
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
        return callRT("perl_uniq_list", {av});
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
        else if (mode == "custom" && n.body) {
            /* Generate a comparison function: long long cmp(PerlValue* a, PerlValue* b) */
            static int sortCmpCounter = 0;
            std::string cmpName = "__sort_cmp_" + std::to_string(sortCmpCounter++);

            auto *i64Ty = Type::getInt64Ty(ctx_);
            auto *cmpFT = FunctionType::get(i64Ty, {perlPtrTy_, perlPtrTy_}, false);
            auto *cmpFn = Function::Create(cmpFT, Function::InternalLinkage,
                                           cmpName, mod_.get());
            /* save codegen state */
            auto *savedFn      = currentFn_;
            auto *savedBB      = builder_.GetInsertBlock();
            auto  savedScopes  = scopes_;
            auto  savedArr     = arrayScopes_;
            auto  savedHash    = hashScopes_;
            auto  savedPv      = pvScopes_;
            auto  savedFloat   = floatScopes_;
            auto  savedInt     = intScopes_;
            auto *savedLDep    = localDepthAlloca_;
            auto *savedBody    = currentSubBody_;

            auto *cmpEntry = BasicBlock::Create(ctx_, "entry", cmpFn);
            builder_.SetInsertPoint(cmpEntry);
            currentFn_ = cmpFn;
            scopes_ = {}; arrayScopes_ = {}; hashScopes_ = {}; pvScopes_ = {};
            floatScopes_ = {}; intScopes_ = {};
            pushScope();

            auto *i32Ty = Type::getInt32Ty(ctx_);
            localDepthAlloca_ = builder_.CreateAlloca(i32Ty, nullptr, "local.depth");
            builder_.CreateStore(callRT("perl_local_save_depth", {}), localDepthAlloca_);

            /* bind $a and $b to the function parameters */
            Value *argA = cmpFn->getArg(0); argA->setName("a");
            Value *argB = cmpFn->getArg(1); argB->setName("b");
            auto *aAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, "a");
            auto *bAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, "b");
            builder_.CreateStore(argA, aAlloca);
            builder_.CreateStore(argB, bAlloca);
            declareVar("a", aAlloca);
            declareVar("b", bAlloca);

            currentSubBody_ = n.body.get();
            Value *cmpResult = emitBlockLast(*n.body);
            currentSubBody_ = savedBody;

            if (!builder_.GetInsertBlock()->getTerminator()) {
                Value *depth = builder_.CreateLoad(i32Ty, localDepthAlloca_);
                callRT("perl_local_restore_to", {depth});
                Value *rv = callRT("perl_to_int", {cmpResult});
                builder_.CreateRet(rv);
            }
            popScope();

            /* restore state */
            currentFn_        = savedFn;
            builder_.SetInsertPoint(savedBB);
            scopes_           = std::move(savedScopes);
            arrayScopes_      = std::move(savedArr);
            hashScopes_       = std::move(savedHash);
            pvScopes_         = std::move(savedPv);
            floatScopes_      = std::move(savedFloat);
            intScopes_        = std::move(savedInt);
            localDepthAlloca_ = savedLDep;
            currentSubBody_   = savedBody;

            /* call perl_sort_custom(av, cmpFn) */
            Value *fnPtr = builder_.CreateBitCast(cmpFn,
                              PointerType::getUnqual(ctx_));
            return callRT("perl_sort_custom", {av, fnPtr});
        }
        else { /* default: sort a copy lexicographically */
            Value *copy = callRT("perl_sort_str_asc", {av}); return copy;
        }
    }
    if (n.kind == NK::DerefArray) {
        Value *ref = emitExpr(*n.left);
        return callRT("perl_deref_array", {ref});
    }
    if (n.kind == NK::PostfixDeref && n.sval == "all_array") {
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

        if (isMap) {
            /* For map, try array path first so `map { @$_ } @aoa` flattens.
               Emit all-but-last stmts normally, then handle the last expression
               via emitArrayPtr (extend) or emitExpr (push). */
            Value *mapAv = nullptr;
            Value *mapPv = nullptr;
            if (n.body && !n.body->args.empty()) {
                const auto &stmts = n.body->args;
                for (size_t si = 0; si + 1 < stmts.size(); si++)
                    emitStmt(*stmts[si]);
                const Node &last = *stmts.back();
                const Node *e = (last.kind == NK::ExprStmt && last.left)
                                ? last.left.get() : nullptr;
                if (e) { mapAv = emitArrayPtr(*e); if (!mapAv) mapPv = emitExpr(*e); }
                else   emitStmt(last);
            } else if (n.left) {
                mapAv = emitArrayPtr(*n.left);
                if (!mapAv) mapPv = emitExpr(*n.left);
            }
            /* Clone scalar result before popScope() frees scope variables it may
               reference (e.g. last expr is a ScalarVar whose alloca is being freed). */
            if (mapPv && !llvm::isa<llvm::ConstantPointerNull>(mapPv)) {
                Value *orig = mapPv;
                mapPv = callRT("perl_clone", {mapPv});
                freeIfOwned(orig);
            }
            popScope();
            if (mapAv)      callRT("perl_array_extend", {resultArr, mapAv});
            else if (mapPv) callRT("perl_array_push",   {resultArr, mapPv});
        } else {
            /* grep: push element if block result is true */
            Value *blockResult;
            if (n.body)       blockResult = emitBlockLast(*n.body);
            else if (n.left)  blockResult = emitExpr(*n.left);
            else              blockResult = perlUndef();
            popScope();
            Value *tv    = callRT("perl_is_true", {blockResult});
            Value *cond  = builder_.CreateICmpNE(tv, ConstantInt::get(i32, 0));
            auto *pushBB = BasicBlock::Create(ctx_, "grep.push", fn);
            auto *nextBB = BasicBlock::Create(ctx_, "grep.next", fn);
            builder_.CreateCondBr(cond, pushBB, nextBB);

            builder_.SetInsertPoint(pushBB);
            callRT("perl_array_push", {resultArr, elem});
            builder_.CreateBr(nextBB);
            builder_.SetInsertPoint(nextBB);
        }

        Value *i2 = builder_.CreateAdd(i, ConstantInt::get(i64, 1));
        builder_.CreateStore(i2, iAlloca);
        builder_.CreateBr(condBB);

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
        Value *av;
        if (n.left) {              /* @{$aref}[...] or @$aref[...] */
            Value *ref = emitExpr(*n.left);
            av = callRT("perl_deref_array", {ref});
            freeIfOwned(ref);
        } else {
            av = lookupArray(n.name);
        }
        Value *res = callRT("perl_array_new", {});
        for (auto &idxNode : n.args) {
            Value *elem = av ? callRT("perl_array_get_ref", {av, emitIdx(*idxNode)}) : perlUndef();
            callRT("perl_array_push", {res, elem});
        }
        return res;
    }
    if (n.kind == NK::HashSlice) {
        Value *hv;
        if (n.left) {              /* @{$href}{...} or @$href{...} */
            Value *ref = emitExpr(*n.left);
            hv = callRT("perl_deref_hash", {ref});
            freeIfOwned(ref);
        } else {
            hv = lookupHash(n.name);
        }
        Value *res = callRT("perl_array_new", {});
        auto pushHashKey = [&](const Node &keyNode) {
            if (keyNode.kind == NK::ArrayLit) {
                for (auto &k : keyNode.args) {
                    Value *elem = hv ? emitHashGetRef(hv, *k) : perlUndef();
                    callRT("perl_array_push", {res, elem});
                }
            } else if (Value *kav = emitArrayPtr(keyNode)) {
                /* dynamic array of keys: @h{@arr} */
                Value *slice = hv ? callRT("perl_hash_slice", {hv, kav})
                                  : callRT("perl_array_new", {});
                callRT("perl_array_extend", {res, slice});
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
        auto *i32Ty = Type::getInt32Ty(ctx_);
        Value *level;
        if (n.left) {
            Value *lv64 = callRT("perl_to_int", {emitExpr(*n.left)});
            level = builder_.CreateTrunc(lv64, i32Ty);
        } else {
            level = ConstantInt::get(i32Ty, 0);
        }
        return callRT("perl_caller", {level});
    }
    if (n.kind == NK::ReaddirFunc) {
        Value *slot = lookupVar(n.name);
        if (!slot) return callRT("perl_array_new", {});
        Value *dh = builder_.CreateLoad(perlPtrTy_, slot);
        return callRT("perl_readdir_all", {dh});
    }
    /* user-defined sub call in list context: call with ctx=1, unwrap result */
    if (n.kind == NK::Call) {
        if (auto *fn = mod_->getFunction(subLLVMName(n.name))) {
            Value *argsArr = callRT("perl_array_new", {});
            for (auto &arg : n.args) {
                if (arg->kind == NK::ArrayVar) {
                    Value *av = lookupArray(arg->name);
                    if (av) { callRT("perl_array_extend", {argsArr, av}); continue; }
                }
                if (arg->kind == NK::HashVar) {
                    Value *hv = lookupHash(arg->name);
                    if (hv) { callRT("perl_array_extend_hash", {argsArr, hv}); continue; }
                }
                if (Value *av = emitArrayPtr(*arg)) {
                    callRT("perl_array_extend", {argsArr, av}); continue;
                }
                Value *v = emitExpr(*arg);
                callRT("perl_array_push", {argsArr, v});
                freeIfOwned(v);
            }
            auto *i32Ty = Type::getInt32Ty(ctx_);
            Value *one = ConstantInt::get(i32Ty, 1);
            Value *pv = builder_.CreateCall(fn, {argsArr, one});
            callRT("perl_array_free", {argsArr});
            return callRT("perl_unwrap_list_return", {pv});
        }
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

/* Like emitHashGetRef but returns a writable slot (creates undef if missing).
   Use for ++ / -- / compound-assign targets to avoid mutating the sentinel. */
Value *CodeGen::emitHashLValueRef(Value *hv, const Node &keyNode) {
    if (Value *kp = constKeyPtr(keyNode, builder_))
        return callRT("perl_hash_lvalue_str", {hv, kp});
    Value *key = emitExpr(keyNode);
    Value *r   = callRT("perl_hash_lvalue_sv", {hv, key});
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
        "perl_clone", "perl_sprintf", "perl_array_len", "perl_array_len_f64",
        /* single-arg math/string builtins */
        "perl_alloc_flat_array", "perl_alloc_float_pair",
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
        /* method/sub dispatch always returns a freshly cloned PerlValue* */
        "perl_dispatch_method",
        /* reference constructors: each returns a freshly allocated PerlValue* */
        "perl_ref_hash", "perl_ref_array", "perl_ref_scalar",
    };
    if (owned.count(nm.str()) > 0) return true;
    /* perl_bless returns its first argument unchanged.  It is owned (and therefore
       safe to free after cloning for return) ONLY when the argument is itself an
       owned temp (e.g. a freshly-allocated perl_ref_hash result).  When bless is
       applied to a stable PV loaded from an alloca, the argument is a LoadInst, not
       a CallInst, so the recursive check returns false — preventing a double-free. */
    if (nm == "perl_bless") return isOwnedTemp(ci->getArgOperand(0));
    return false;
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

/* ── cached PerlArray* for array-ref @_ args (Stage 15) ─────────────────── */

Value *CodeGen::lookupDerefAV(const std::string &name) {
    for (int i = (int)derefAVScopes_.size() - 1; i >= 0; i--) {
        auto it = derefAVScopes_[i].find(name);
        if (it != derefAVScopes_[i].end()) return it->second;
    }
    return nullptr;
}

void CodeGen::declareDerefAV(const std::string &name, Value *alloca) {
    if (!derefAVScopes_.empty()) derefAVScopes_.back()[name] = alloca;
}

Value *CodeGen::lookupRowAV(const std::string &outerVar, const std::string &idxVar) {
    std::string key = outerVar + "\x01" + idxVar;
    for (int i = (int)rowAVScopes_.size() - 1; i >= 0; i--) {
        auto it = rowAVScopes_[i].find(key);
        if (it != rowAVScopes_[i].end()) return it->second;
    }
    return nullptr;
}

void CodeGen::declareRowAV(const std::string &outerVar, const std::string &idxVar, Value *alloca) {
    if (!rowAVScopes_.empty())
        rowAVScopes_.back()[outerVar + "\x01" + idxVar] = alloca;
}

Value *CodeGen::lookupFlatRow(const std::string &outerVar, const std::string &idxVar) {
    std::string key = outerVar + "\x01" + idxVar;
    for (int i = (int)flatRowScopes_.size() - 1; i >= 0; i--) {
        auto it = flatRowScopes_[i].find(key);
        if (it != flatRowScopes_[i].end()) return it->second;
    }
    return nullptr;
}

void CodeGen::declareFlatRow(const std::string &outerVar, const std::string &idxVar, Value *alloca) {
    if (!flatRowScopes_.empty())
        flatRowScopes_.back()[outerVar + "\x01" + idxVar] = alloca;
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
    using P = llvm::CmpInst::Predicate;
    P pred;
    if      (n.sval == "<")  pred = P::ICMP_SLT;
    else if (n.sval == "<=") pred = P::ICMP_SLE;
    else if (n.sval == ">")  pred = P::ICMP_SGT;
    else if (n.sval == ">=") pred = P::ICMP_SGE;
    else if (n.sval == "==") pred = P::ICMP_EQ;
    else if (n.sval == "!=") pred = P::ICMP_NE;
    else return nullptr;

    /* Standard path: both operands expressible as bare i64. */
    if (canEmitI64(*n.left) && canEmitI64(*n.right)) {
        Value *lv = emitExprI64(*n.left);
        Value *rv = emitExprI64(*n.right);
        if (lv && rv) return builder_.CreateICmp(pred, lv, rv, "icmp");
    }

    /* Stage 26a: intVar CMP fileGlobal (or vice-versa).
       File-scope globals can't be assumed to be int in general (they may be
       floats like $pi). We only trust them here, in a comparison context where
       the value is always used as an index/bound (e.g., $i <= $n). */
    auto tryFileGlobalI64 = [&](const Node &nd) -> Value* {
        if (nd.kind != NK::ScalarVar) return nullptr;
        std::string nm = nd.name;
        if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
        auto git = fileScalarGlobals_.find(nm);
        if (git == fileScalarGlobals_.end()) return nullptr;
        Value *pv = builder_.CreateLoad(perlPtrTy_, git->second, nm);
        return callRT("perl_to_int", {pv});
    };

    if (canEmitI64(*n.left)) {
        if (Value *rv = tryFileGlobalI64(*n.right)) {
            Value *lv = emitExprI64(*n.left);
            if (lv) return builder_.CreateICmp(pred, lv, rv, "icmp");
        }
    }
    if (canEmitI64(*n.right)) {
        if (Value *lv = tryFileGlobalI64(*n.left)) {
            Value *rv = emitExprI64(*n.right);
            if (rv) return builder_.CreateICmp(pred, lv, rv, "icmp");
        }
    }
    return nullptr;
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

/* ── Stage 32: loop-invariant PV tracking and deref hoisting ─────────────── */

void CodeGen::pushLoopInvariantTracking() {
    if (!loopInvariantPVs_.empty() || !loopExits_.empty())
        loopInvariantPVs_.emplace_back();
}

void CodeGen::popLoopInvariantTracking() {
    if (!loopInvariantPVs_.empty())
        loopInvariantPVs_.pop_back();
}

void CodeGen::freeLoopInvariantPVs() {
    if (loopInvariantPVs_.empty()) return;
    auto *bb = builder_.GetInsertBlock();
    if (bb && !bb->getTerminator()) {
        for (Value *pv : loopInvariantPVs_.back())
            callRT("perl_free", {pv});
    }
    loopInvariantPVs_.pop_back();
}

void CodeGen::trackLoopInvariantPV(Value *pv) {
    if (loopInvariantPVs_.empty()) return;
    loopInvariantPVs_.back().push_back(pv);
}

void CodeGen::pushDerefCache() {
    if (!loopDerefCache_.empty() || !loopExits_.empty())
        loopDerefCache_.emplace_back();
}

void CodeGen::popDerefCache() {
    if (!loopDerefCache_.empty())
        loopDerefCache_.pop_back();
}

Value *CodeGen::lookupLoopDerefCache(const std::string &varName) {
    for (int i = (int)loopDerefCache_.size() - 1; i >= 0; i--) {
        auto it = loopDerefCache_[i].find(varName);
        if (it != loopDerefCache_[i].end()) return it->second;
    }
    return nullptr;
}

void CodeGen::declareLoopDerefCache(const std::string &varName, Value *cachedPtr) {
    if (!loopDerefCache_.empty())
        loopDerefCache_.back()[varName] = cachedPtr;
}

/* Collect all ScalarVar names that are the direct argument of a
   perl_deref_array call (ArrowDeref with sval=="array" whose left is
   a ScalarVar).  Used to identify candidates for hoisting. */
static void collectDerefTargets(const Node &n, std::set<std::string> &targets) {
    if (n.kind == NK::ArrowDeref && n.sval == "array" &&
        n.left && n.left->kind == NK::ScalarVar) {
        std::string nm = n.left->name;
        if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
        targets.insert(nm);
    }
    if (n.left)  collectDerefTargets(*n.left,  targets);
    if (n.right) collectDerefTargets(*n.right, targets);
    for (auto &a : n.args) collectDerefTargets(*a, targets);
    if (n.body)  collectDerefTargets(*n.body,  targets);
}

/* Check if a ScalarVar named 'nm' is assigned (written to) anywhere in 'n'.
   Returns true if the variable is modified, false if it's only read. */
static bool isVarModified(const Node &n, const std::string &nm) {
    if (n.kind == NK::ScalarVar && n.name == nm) {
        /* Check if this is a write target (LHS of assign, etc.) */
        return false; /* bare read — not modified at this node */
    }
    /* Check for assignments where nm is the LHS */
    if (n.kind == NK::Assign && n.left &&
        n.left->kind == NK::ScalarVar && n.left->name == nm)
        return true;
    /* Check for compound assigns */
    if (n.kind == NK::CompoundAssign && n.left &&
        n.left->kind == NK::ScalarVar && n.left->name == nm)
        return true;
    /* Check for increments/decrements */
    if (n.kind == NK::UnaryOp && n.left &&
        n.left->kind == NK::ScalarVar && n.left->name == nm) {
        if (n.sval == "pre++" || n.sval == "post++" ||
            n.sval == "pre--" || n.sval == "post--")
            return true;
    }
    bool r = false;
    if (n.left)  r = r || isVarModified(*n.left,  nm);
    if (n.right) r = r || isVarModified(*n.right, nm);
    for (auto &a : n.args) { if (!r) r = r || isVarModified(*a, nm); }
    if (n.body)  r = r || isVarModified(*n.body,  nm);
    for (auto &b : n.branches) {
        if (!r && b.body) r = r || isVarModified(*b.body, nm);
    }
    return r;
}

/* Emit hoisted perl_deref_array calls for loop-invariant variables.
   For each variable in 'derefTargets' that is not modified in 'body',
   emit perl_deref_array(lookupVar(nm)) before the loop and cache the
   PerlArray* in loopDerefCache_. */
void CodeGen::emitHoistedDerefs(const Node &loopBody,
                                const std::set<std::string> &derefTargets) {
    auto *i64Ty = Type::getInt64Ty(ctx_);
    for (const auto &nm : derefTargets) {
        /* Skip if already cached */
        if (lookupLoopDerefCache(nm)) continue;
        /* Skip if variable doesn't exist in current scope */
        if (!lookupVar(nm)) continue;
        /* Skip if variable is modified inside the loop body */
        if (isVarModified(loopBody, nm)) continue;

        /* Emit perl_deref_array(lookupVar(nm)) */
        Value *slot = lookupVar(nm);
        Value *pv = builder_.CreateLoad(perlPtrTy_, slot, nm + ".pv");
        Value *arr = callRT("perl_deref_array", {pv});

        /* Cache in an alloca */
        auto *cacheAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, nm + ".deref");
        builder_.CreateStore(arr, cacheAlloca);
        declareLoopDerefCache(nm, cacheAlloca);
    }
}

/* Emit perl_deref_array(ref), checking the loop-invariant deref cache first.
   If 'cachedVarName' is provided and the variable is in the cache, load from
   the cached alloca instead of calling perl_deref_array. */
Value *CodeGen::emitDerefArray(Value *ref, const std::string *cachedVarName) {
    if (cachedVarName) {
        if (Value *cacheAlloca = lookupLoopDerefCache(*cachedVarName)) {
            return builder_.CreateLoad(perlPtrTy_, cacheAlloca, (*cachedVarName) + ".deref.load");
        }
    }
    return callRT("perl_deref_array", {ref});
}

/* ── Stage 33: known tag type tracking ───────────────────────────────────── */

void CodeGen::pushKnownTypes() {
    knownTagTypes_.emplace_back();
}

void CodeGen::popKnownTypes() {
    if (!knownTagTypes_.empty())
        knownTagTypes_.pop_back();
}

void CodeGen::setKnownTagType(const std::string &varName, int tag) {
    if (!knownTagTypes_.empty())
        knownTagTypes_.back()[varName] = tag;
}

int CodeGen::lookupKnownTagType(const std::string &varName) {
    for (int i = (int)knownTagTypes_.size() - 1; i >= 0; i--) {
        auto it = knownTagTypes_[i].find(varName);
        if (it != knownTagTypes_[i].end()) return it->second;
    }
    return 0;
}

void CodeGen::pushArrayElemTypes() {
    arrayElemTypes_.emplace_back();
}

void CodeGen::popArrayElemTypes() {
    if (!arrayElemTypes_.empty())
        arrayElemTypes_.pop_back();
}

void CodeGen::setArrayElemType(const std::string &arrName, int elemTag) {
    if (!arrayElemTypes_.empty())
        arrayElemTypes_.back()[arrName] = elemTag;
}

int CodeGen::lookupArrayElemType(const std::string &arrName) {
    for (int i = (int)arrayElemTypes_.size() - 1; i >= 0; i--) {
        auto it = arrayElemTypes_[i].find(arrName);
        if (it != arrayElemTypes_[i].end()) return it->second;
    }
    return 0;
}

void CodeGen::setFuncArgElemType(int argIdx, int elemTag) {
    funcArgElemTypes_[argIdx] = elemTag;
}

int CodeGen::lookupFuncArgElemType(int argIdx) {
    auto it = funcArgElemTypes_.find(argIdx);
    if (it != funcArgElemTypes_.end()) return it->second;
    return 0;
}

/* ── cached PerlArray* for array-ref @_ args (Stage 15) ─────────────────── */

/* Returns true if 'nm' only ever appears in numeric-safe contexts within 'n'.
   Used to decide whether a @_ scalar arg can be promoted to a float alloca.
   'inNum' = the parent node is a numeric expression (so nm here is safe). */
/* Collect unique (outerVarName, strippedIndexVarName) pairs from 2D ArrowDeref
   patterns where outerVarName is in derefAVNames (Stage 15 promoted vars).
   Used by Foreach emitter to pre-emit row derefs at loop body entry. */
static void collectRowAVPairs(
    const Node &n,
    const std::unordered_set<std::string> &derefAVNames,
    std::set<std::pair<std::string,std::string>> &out)
{
    /* 2D read or write target: $outer->[$idx][k]
       AST: ArrowDeref(left=ArrowDeref(left=ScalarVar(outer), right=ScalarVar(idx)), right=k) */
    if (n.kind == NK::ArrowDeref && n.sval == "array" &&
        n.left && n.left->kind == NK::ArrowDeref && n.left->sval == "array" &&
        n.left->left && n.left->left->kind == NK::ScalarVar &&
        derefAVNames.count(n.left->left->name) &&
        n.left->right && n.left->right->kind == NK::ScalarVar) {
        std::string idxNm = n.left->right->name;
        if (!idxNm.empty() && idxNm[0] == '$') idxNm = idxNm.substr(1);
        out.insert({n.left->left->name, idxNm});
    }

    if (n.left)  collectRowAVPairs(*n.left,  derefAVNames, out);
    if (n.right) collectRowAVPairs(*n.right, derefAVNames, out);
    for (auto &a : n.args) collectRowAVPairs(*a, derefAVNames, out);
    if (n.body)  collectRowAVPairs(*n.body,  derefAVNames, out);
    if (n.init)  collectRowAVPairs(*n.init,  derefAVNames, out);
    if (n.cond)  collectRowAVPairs(*n.cond,  derefAVNames, out);
    if (n.step)  collectRowAVPairs(*n.step,  derefAVNames, out);
    for (auto &b : n.branches) {
        if (b.cond) collectRowAVPairs(*b.cond, derefAVNames, out);
        if (b.body) collectRowAVPairs(*b.body, derefAVNames, out);
    }
}

/* Returns true if every occurrence of ScalarVar(nm) in the AST is the direct
   left-child of an ArrowDeref with sval=="array" (i.e. used only as $nm->[$i]).
   Used to decide whether to cache perl_deref_array_ro at function entry. */
static bool isOnlyArrayRefDeref(const Node &n, const std::string &nm) {
    if (n.kind == NK::ScalarVar && n.name == nm)
        return false; /* bare use — not safe */

    if (n.kind == NK::ArrowDeref && n.sval == "array") {
        bool leftOk;
        if (n.left && n.left->kind == NK::ScalarVar && n.left->name == nm)
            leftOk = true; /* nm is the direct array-ref base here — safe position */
        else
            leftOk = isOnlyArrayRefDeref(*n.left, nm);
        bool rightOk = !n.right || isOnlyArrayRefDeref(*n.right, nm);
        return leftOk && rightOk;
    }

    /* my ($x, $bodies, ...) = @_ — LHS elements are write targets, skip them */
    if (n.kind == NK::Assign && n.left && n.left->kind == NK::ArrayLit)
        return !n.right || isOnlyArrayRefDeref(*n.right, nm);

    /* $bodies = expr — reassignment means cached deref would go stale */
    if (n.kind == NK::Assign && n.left &&
        n.left->kind == NK::ScalarVar && n.left->name == nm)
        return false;

    /* my $bodies = expr or state $bodies — redeclaration */
    if ((n.kind == NK::My || n.kind == NK::StateDecl) && n.name == nm)
        return false;

    bool ok = true;
    if (n.left)  ok = ok && isOnlyArrayRefDeref(*n.left,  nm);
    if (n.right) ok = ok && isOnlyArrayRefDeref(*n.right, nm);
    for (auto &a : n.args) ok = ok && isOnlyArrayRefDeref(*a, nm);
    if (n.body)  ok = ok && isOnlyArrayRefDeref(*n.body,  nm);
    if (n.init)  ok = ok && isOnlyArrayRefDeref(*n.init,  nm);
    if (n.cond)  ok = ok && isOnlyArrayRefDeref(*n.cond,  nm);
    if (n.step)  ok = ok && isOnlyArrayRefDeref(*n.step,  nm);
    for (auto &b : n.branches) {
        if (b.cond) ok = ok && isOnlyArrayRefDeref(*b.cond, nm);
        if (b.body) ok = ok && isOnlyArrayRefDeref(*b.body, nm);
    }
    return ok;
}

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
        for (auto &b : n.branches) {
            if (b.cond) ok = ok && floatSafe(*b.cond, nm, false);
            if (b.body) ok = ok && floatSafe(*b.body, nm, false);
        }
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
    for (auto &b : n.branches) {
        if (b.cond) r = r || hasVar(*b.cond, nm);
        if (b.body) r = r || hasVar(*b.body, nm);
    }
    return r;
}

/* Returns true if 'nm' is ever used in a float-precision context (/, **, sqrt).
   Ensures we only promote vars that actually need floating-point, not integer
   counters that happen to appear in arithmetic. */
static bool needsFloatPrec(const Node &n, const std::string &nm) {
    /* Any arithmetic op (+, -, *, /, **) involving nm may lose precision if nm
       holds a float value — classify as float rather than int for @_ params. */
    if (n.kind == NK::BinOp &&
        (n.sval == "/" || n.sval == "**" ||
         n.sval == "+" || n.sval == "-"  || n.sval == "*")) {
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
    for (auto &b : n.branches) {
        if (b.cond) r = r || needsFloatPrec(*b.cond, nm);
        if (b.body) r = r || needsFloatPrec(*b.body, nm);
    }
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
        if (lookupFloatVar(nm)) return true;
        /* Only treat file-scope globals as numeric if they have no special runtime accessor */
        static const std::unordered_set<std::string> specialVars =
            {"AUTOLOAD","!","/",".","\\",",","&","0","_"};
        if (specialVars.count(nm)) return false;
        return fileScalarGlobals_.count(nm) != 0;
    }
    case NK::BinOp: {
        if (n.sval == "**" && n.right && n.right->kind == NK::IntLit && n.right->ival == 2)
            return n.left && canEmitF64(*n.left);
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
    case NK::AbsFunc:
        return n.left && canEmitF64(*n.left);
    case NK::IntFunc:
        return n.left && canEmitF64(*n.left);
    case NK::LengthFunc:
        /* length of FLAT_ARRAY: read matchpos (count) directly */
        if (n.left && n.left->kind == NK::ArrowDeref && n.left->sval == "array") {
            if (n.left->left->kind == NK::ScalarVar) {
                std::string nm = n.left->left->name;
                if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
                if (lookupDerefAV(nm)) return true;
            }
        }
        return false;
    /* Array/hash element lookups can always be converted to double via perl_to_float,
       but they require emitting emitExpr calls internally — always allowed. */
    case NK::ArrayElem:
        /* Array elements may be refs or mixed types — not safely float-promotable.
           The emitExprF64/float-var path is wrong when elements hold array refs. */
        return false;
    case NK::ArrowDeref:
        if (n.sval != "array" || !n.left) return false;
        /* 2D subscript $ref->[$i][$j]: inner array always holds scalars */
        if (n.left->kind == NK::ArrowDeref && n.left->sval == "array") return true;
        /* 1D $ref->[$i] where $ref is a DerefAV-cached param: elements are scalars */
        if (n.left->kind == NK::ScalarVar) {
            std::string nm = n.left->name;
            if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
            if (lookupDerefAV(nm)) return true;
            /* FLOAT_PAIR fast path: $z->[0] or $z->[1] where $z may be a FLOAT_PAIR PV.
               Emits a tag-check branch; branch is well-predicted so overhead is minimal. */
            if (n.right && n.right->kind == NK::IntLit &&
                (n.right->ival == 0 || n.right->ival == 1) && lookupVar(nm))
                return true;
        }
        return false;
    case NK::HashElem:
        return lookupHash(n.name) != nullptr;
    case NK::Call: {
        /* Inlineable subs whose body is purely numeric can be emitted as F64. */
        auto it = inlineSubs_.find(n.name);
        return it != inlineSubs_.end() && canEmitF64(*it->second.bodyExpr);
    }
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
        /* file-scope global: load PV* and coerce to double — skip special-accessor vars */
        {
            static const std::unordered_set<std::string> noFloat =
                {"AUTOLOAD","!","/",".","\\",",","&","0","_"};
            if (!noFloat.count(nm)) {
                auto git = fileScalarGlobals_.find(nm);
                if (git != fileScalarGlobals_.end()) {
                    Value *pv = builder_.CreateLoad(perlPtrTy_, git->second, nm + ".gpv");
                    return callRT("perl_to_float", {pv});
                }
            }
        }
        return nullptr;
    }
    case NK::BinOp: {
        /* x**2 → x*x (avoids boxing and perl_pow entirely) */
        if (n.sval == "**" && n.right && n.right->kind == NK::IntLit && n.right->ival == 2) {
            if (!canEmitF64(*n.left)) return nullptr;
            Value *v = emitExprF64(*n.left);
            return v ? builder_.CreateFMul(v, v, "sq") : nullptr;
        }
        static const char *arithOps[] = {"+", "-", "*", "/", nullptr};
        bool isArith = false;
        for (auto *p = arithOps; *p; p++) if (n.sval == *p) { isArith = true; break; }
        if (!isArith) return nullptr;
        /* Stage 30: $sqrt_var * $sqrt_var → dsq (exact, avoids fmul on the sqrt critical path).
           This fires when both sides are the SAME float variable that was assigned sqrt(x),
           giving dist*dist = x.  Together with the multiply-chain rule, dist*dist*dist = x*dist,
           shortening the critical path by one fmul (5 cycles × 10 pairs per advance() call). */
        if (n.sval == "*" && n.left->kind == NK::ScalarVar && n.right->kind == NK::ScalarVar) {
            auto lnm = n.left->name;  if (!lnm.empty() && lnm[0]=='$') lnm = lnm.substr(1);
            auto rnm = n.right->name; if (!rnm.empty() && rnm[0]=='$') rnm = rnm.substr(1);
            if (lnm == rnm) {
                auto it = floatSqrtOf_.find(lnm);
                if (it != floatSqrtOf_.end()) return it->second;  /* dist*dist = dsq */
            }
        }
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
        lastSqrtInput_ = v;  /* Stage 30: remember input so cube opts can use x*sqrt(x) */
        auto *sqrtFn = llvm::Intrinsic::getDeclaration(mod_.get(),
            llvm::Intrinsic::sqrt, {f64});
        return builder_.CreateCall(sqrtFn, {v}, "sqrt");
    }
    case NK::AbsFunc: {
        if (!canEmitF64(*n.left)) return nullptr;
        Value *v = emitExprF64(*n.left);
        if (!v) return nullptr;
        auto *absFn = llvm::Intrinsic::getDeclaration(mod_.get(),
            llvm::Intrinsic::fabs, {f64});
        return builder_.CreateCall(absFn, {v}, "fabs");
    }
    case NK::IntFunc: {
        /* int(x) on a double: truncate toward zero, convert back to double.
           LLVM's trunc only works for int->int; for float->int we use
           inttoptr+ptrtoint or the floor+ceil trick.  Simplest: round toward
           zero via conditional floor/ceil. */
        if (!canEmitF64(*n.left)) return nullptr;
        Value *v = emitExprF64(*n.left);
        if (!v) return nullptr;
        /* Truncation toward zero: if v >= 0, floor(v); else ceil(v) */
        auto *i64 = Type::getInt64Ty(ctx_);
        Value *floored = builder_.CreateCall(
            llvm::Intrinsic::getDeclaration(mod_.get(), llvm::Intrinsic::floor, {f64}),
            {v}, "floor");
        Value *ceiled = builder_.CreateCall(
            llvm::Intrinsic::getDeclaration(mod_.get(), llvm::Intrinsic::ceil, {f64}),
            {v}, "ceil");
        Value *isNeg = builder_.CreateFCmpOLT(v, ConstantFP::get(f64, 0.0), "iscmp");
        Value *truncated = builder_.CreateSelect(isNeg, ceiled, floored, "trunc");
        /* Convert i64 back to double */
        auto *intToFP = builder_.CreateSIToFP(truncated, f64, "int2fp");
        return intToFP;
    }
    case NK::LengthFunc: {
        /* length of FLAT_ARRAY: call perl_array_len_f64 for unboxed double */
        if (n.left && n.left->kind == NK::ArrowDeref && n.left->sval == "array") {
            if (n.left->left->kind == NK::ScalarVar) {
                std::string nm = n.left->left->name;
                if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
                if (Value *pa = lookupDerefAV(nm)) {
                    /* Load PerlArray* and call perl_array_len_f64 */
                    Value *arr = builder_.CreateLoad(perlPtrTy_, pa, nm + ".av");
                    return callRT("perl_array_len_f64", {arr});
                }
            }
        }
        return nullptr;
    }
    case NK::ArrayElem:
        /* Array elements may hold refs — cannot safely emit as f64.
           Callers must go through the PerlValue* path instead. */
        return nullptr;
    case NK::ArrowDeref: {
        /* $ref->[$i] or $ref->{k} read — unbox the element */
        if (n.sval == "array") {
            /* 2D pattern $arr->[$i][$k]: emit full readonly chain so GVN can CSE */
            if (n.left->kind == NK::ArrowDeref && n.left->sval == "array") {
                /* Outer deref: use cached PerlArray* if available (Stage 15) */
                Value *outerArr;
                if (n.left->left->kind == NK::ScalarVar) {
                    if (Value *pa = lookupDerefAV(n.left->left->name)) {
                        outerArr = builder_.CreateLoad(perlPtrTy_, pa, n.left->left->name + ".av");
                    } else {
                        Value *base = emitExpr(*n.left->left);
                        outerArr = callRT("perl_deref_array_ro", {base});
                        freeIfOwned(base);
                    }
                } else {
                    Value *base = emitExpr(*n.left->left);
                    outerArr = callRT("perl_deref_array_ro", {base});
                    freeIfOwned(base);
                }
                /* Inner deref: use flat/row cache if first index is a named var */
                Value *innerArr;
                if (n.left->left->kind == NK::ScalarVar &&
                    n.left->right->kind == NK::ScalarVar) {
                    std::string idxNm = n.left->right->name;
                    if (!idxNm.empty() && idxNm[0] == '$') idxNm = idxNm.substr(1);

                    /* Stage 22: flat row cache — direct double[] read via phi dispatch */
                    if (Value *fra = lookupFlatRow(n.left->left->name, idxNm)) {
                        /* Stage 31: return cached f64 if this exact element was already loaded */
                        if (n.right->kind == NK::IntLit) {
                            std::string ckey = n.left->left->name + "\x01" + idxNm + "\x01" + std::to_string(n.right->ival);
                            auto cit = flatDoubleCache_.find(ckey);
                            if (cit != flatDoubleCache_.end()) return cit->second;
                        }
                        Value *idx17    = emitIdx(*n.right);
                        auto *f64Ty17   = Type::getDoubleTy(ctx_);
                        auto *flatLoad17 = builder_.CreateLoad(perlPtrTy_, fra, "flat.ptr");
                        /* Stage 29: when the outer array was pre-checked all-flat, the fra
                           pointer is guaranteed non-null — mark it so LLVM folds the null-check
                           and eliminates the dead norm BB from the inner loop. */
                        if (avAllflatSlots_.count(n.left->left->name))
                            flatLoad17->setMetadata(LLVMContext::MD_nonnull, MDNode::get(ctx_, {}));
                        Value *flatPtr  = flatLoad17;
                        Value *isFlat17 = builder_.CreateICmpNE(flatPtr,
                            ConstantPointerNull::get(perlPtrTy_), "s17.if");
                        auto *curFn17 = builder_.GetInsertBlock()->getParent();
                        auto *fBB17   = BasicBlock::Create(ctx_, "s17.f", curFn17);
                        auto *nBB17   = BasicBlock::Create(ctx_, "s17.n", curFn17);
                        auto *mBB17   = BasicBlock::Create(ctx_, "s17.m", curFn17);
                        builder_.CreateCondBr(isFlat17, fBB17, nBB17);
                        /* flat BB: direct double load */
                        builder_.SetInsertPoint(fBB17);
                        Value *ep17  = builder_.CreateGEP(f64Ty17, flatPtr, idx17, "fe");
                        Value *fvf17 = builder_.CreateLoad(f64Ty17, ep17, "ffv");
                        setTBAA(fvf17, tbaaFlatDoubleTag_);
                        builder_.CreateBr(mBB17);
                        auto *fBB17p = builder_.GetInsertBlock();
                        /* norm BB: PV* row cache */
                        builder_.SetInsertPoint(nBB17);
                        Value *fvn17 = ConstantFP::get(f64Ty17, 0.0);
                        if (Value *ra17 = lookupRowAV(n.left->left->name, idxNm)) {
                            Value *ia17   = builder_.CreateLoad(perlPtrTy_, ra17,
                                              n.left->left->name + "." + idxNm + ".ra");
                            auto *i8Ty17n = Type::getInt8Ty(ctx_);
                            Value *el17   = builder_.CreateLoad(perlPtrTy_, ia17, "ae");
                            setTBAA(el17, tbaaAvElemsTag_);
                            Value *pp17   = builder_.CreateGEP(perlPtrTy_, el17, idx17, "pp");
                            Value *pv17   = builder_.CreateLoad(perlPtrTy_, pp17, "pv");
                            setTBAA(pv17, tbaaAvElemTag_);
                            Value *fp17   = builder_.CreateConstInBoundsGEP1_64(i8Ty17n, pv17, 8, "fp");
                            fvn17         = builder_.CreateLoad(f64Ty17, fp17, "nfv");
                            setTBAA(fvn17, tbaaPvFvalTag_);
                        }
                        builder_.CreateBr(mBB17);
                        auto *nBB17p = builder_.GetInsertBlock();
                        builder_.SetInsertPoint(mBB17);
                        auto *phi17 = builder_.CreatePHI(f64Ty17, 2, "fv");
                        phi17->addIncoming(fvf17, fBB17p);
                        phi17->addIncoming(fvn17, nBB17p);
                        /* Stage 31: cache the loaded value for repeated reads of same element */
                        if (n.right->kind == NK::IntLit) {
                            std::string ckey = n.left->left->name + "\x01" + idxNm + "\x01" + std::to_string(n.right->ival);
                            flatDoubleCache_[ckey] = phi17;
                        }
                        return phi17;
                    }

                    /* Stage 16: normal PV* row cache */
                    if (Value *ra = lookupRowAV(n.left->left->name, idxNm)) {
                        innerArr = builder_.CreateLoad(perlPtrTy_, ra,
                                                        n.left->left->name + "." + idxNm + ".ra");
                    } else {
                        /* Fallback: no row cache — inner row might be FLAT_ARRAY,
                           use perl_deref_array (handles lazy conversion) not _ro */
                        Value *innerRef = callRT("perl_array_get_ref", {outerArr, emitIdx(*n.left->right)});
                        innerArr = callRT("perl_deref_array", {innerRef});
                    }
                } else {
                    /* Uncached 2D read (non-ScalarVar index): flat/norm dispatch so
                       FLAT_ARRAY PVs are never lazy-converted by perl_deref_array. */
                    Value *innerRef = callRT("perl_array_get_ref", {outerArr, emitIdx(*n.left->right)});
                    Value *idx17u   = emitIdx(*n.right);
                    auto *i8Tu      = Type::getInt8Ty(ctx_);
                    auto *i32Tu     = Type::getInt32Ty(ctx_);
                    auto *f64Tu     = Type::getDoubleTy(ctx_);
                    Value *tagU     = builder_.CreateLoad(i32Tu, innerRef, "tagu");
                    setTBAA(tagU, tbaaPvTagTag_);
                    Value *isFlatU  = builder_.CreateICmpEQ(tagU,
                                         ConstantInt::get(i32Tu, 10), "isflatu");
                    auto *curFnU    = builder_.GetInsertBlock()->getParent();
                    auto *fBBU      = BasicBlock::Create(ctx_, "r22u.f", curFnU);
                    auto *nBBU      = BasicBlock::Create(ctx_, "r22u.n", curFnU);
                    auto *mBBU      = BasicBlock::Create(ctx_, "r22u.m", curFnU);
                    builder_.CreateCondBr(isFlatU, fBBU, nBBU);
                    /* flat: load double directly from pval[] */
                    builder_.SetInsertPoint(fBBU);
                    Value *pvalOffU = builder_.CreateConstInBoundsGEP1_64(i8Tu, innerRef, 8, "pvaloffu");
                    Value *dblPtrU  = builder_.CreateLoad(perlPtrTy_, pvalOffU, "dblpu");
                    Value *epU      = builder_.CreateGEP(f64Tu, dblPtrU, idx17u, "epu");
                    Value *fvFlatU  = builder_.CreateLoad(f64Tu, epU, "fvflatu");
                    if (tbaaFlatDoubleTag_) setTBAA(fvFlatU, tbaaFlatDoubleTag_);
                    builder_.CreateBr(mBBU);
                    auto *fBBUp = builder_.GetInsertBlock();
                    /* norm: PV* chain via perl_deref_array */
                    builder_.SetInsertPoint(nBBU);
                    Value *innerArrU = callRT("perl_deref_array", {innerRef});
                    Value *elemsU    = builder_.CreateLoad(perlPtrTy_, innerArrU, "aeu");
                    setTBAA(elemsU, tbaaAvElemsTag_);
                    Value *pvPtrU    = builder_.CreateGEP(perlPtrTy_, elemsU, idx17u, "ppu");
                    Value *pvU       = builder_.CreateLoad(perlPtrTy_, pvPtrU, "pvu");
                    setTBAA(pvU, tbaaAvElemTag_);
                    Value *fvPtrU    = builder_.CreateConstInBoundsGEP1_64(i8Tu, pvU, 8, "fpu");
                    Value *fvNormU   = builder_.CreateLoad(f64Tu, fvPtrU, "fvnu");
                    setTBAA(fvNormU, tbaaPvFvalTag_);
                    builder_.CreateBr(mBBU);
                    auto *nBBUp = builder_.GetInsertBlock();
                    builder_.SetInsertPoint(mBBU);
                    auto *phiU = builder_.CreatePHI(f64Tu, 2, "fvu");
                    phiU->addIncoming(fvFlatU, fBBUp);
                    phiU->addIncoming(fvNormU, nBBUp);
                    return phiU;
                }
                /* Stage 17: inline GEP+loads from PV* chain */
                Value *idx17 = emitIdx(*n.right);
                auto *i8Ty17  = Type::getInt8Ty(ctx_);
                auto *f64Ty17 = Type::getDoubleTy(ctx_);
                Value *elems17 = builder_.CreateLoad(perlPtrTy_, innerArr, "av.elems");
                setTBAA(elems17, tbaaAvElemsTag_);
                Value *pvPtr17 = builder_.CreateGEP(perlPtrTy_, elems17, idx17, "pv.ptr");
                Value *pv17    = builder_.CreateLoad(perlPtrTy_, pvPtr17, "pv");
                setTBAA(pv17, tbaaAvElemTag_);
                Value *fvPtr17 = builder_.CreateConstInBoundsGEP1_64(i8Ty17, pv17, 8, "fv.ptr");
                Value *fv17    = builder_.CreateLoad(f64Ty17, fvPtr17, "fv");
                setTBAA(fv17, tbaaPvFvalTag_);
                return fv17;
            }
            /* 1D with DerefAV cached param: load PerlArray* and coerce element to double */
            if (n.left && n.left->kind == NK::ScalarVar) {
                std::string nm = n.left->name;
                if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
                if (Value *pa = lookupDerefAV(nm)) {
                    Value *av   = builder_.CreateLoad(arrayPtrTy_, pa, nm + ".av");
                    Value *elem = callRT("perl_array_get_ref", {av, emitIdx(*n.right)});
                    return callRT("perl_to_float", {elem});
                }
      /* FLOAT_PAIR / FLAT_ARRAY fast path: $z->[idx] where tag may be
                FLOAT_PAIR (13), FLAT_ARRAY (10), or REF_ARRAY.
                Stage 33: if knownTagTypes_ has a known type, skip the tag dispatch. */
                   if (n.right && lookupVar(nm)) {
                       if (Value *slot = lookupVar(nm)) {
                           auto knownTag = lookupKnownTagType(nm);
                           auto *i32Ty = Type::getInt32Ty(ctx_);
                           auto *i64Ty = Type::getInt64Ty(ctx_);
                           auto *f64Ty = Type::getDoubleTy(ctx_);
                           auto *i8Ty  = Type::getInt8Ty(ctx_);
                           Value *pv   = builder_.CreateLoad(perlPtrTy_, slot, nm + ".pv");
                           auto *curFn = builder_.GetInsertBlock()->getParent();
                           /* Stage 33: use known type to skip tag dispatch */
                           if (knownTag == 13) {
                               /* Known FLOAT_PAIR: direct field load, no tag check */
                               Value *pairFv;
                               if (n.right->kind == NK::IntLit &&
                                   (n.right->ival == 0 || n.right->ival == 1)) {
                                   if (n.right->ival == 0) {
                                       Value *fvPtr = builder_.CreateConstInBoundsGEP1_64(
                                           i8Ty, pv, 8, "fp.re.ptr");
                                       pairFv = builder_.CreateLoad(f64Ty, fvPtr, "fp.re");
                                   } else {
                                       Value *mpPtr = builder_.CreateConstInBoundsGEP1_64(
                                           i8Ty, pv, 16, "fp.im.ptr");
                                       Value *mpBits = builder_.CreateLoad(i64Ty, mpPtr, "fp.im.bits");
                                       pairFv = builder_.CreateBitCast(mpBits, f64Ty, "fp.im");
                                   }
                               } else {
                                   auto *idxV = emitIdx(*n.right);
                                   Value *idx0 = builder_.CreateICmpEQ(idxV,
                                       ConstantInt::get(i64Ty, 0), "idx0");
                                   auto *idxBB = BasicBlock::Create(ctx_, "fp.idx", curFn);
                                   auto *reBB = BasicBlock::Create(ctx_, "fp.re", curFn);
                                   auto *imBB = BasicBlock::Create(ctx_, "fp.im", curFn);
                                   auto *idxM = BasicBlock::Create(ctx_, "fp.idm", curFn);
                                   builder_.CreateCondBr(idx0, reBB, imBB);
                                   builder_.SetInsertPoint(reBB);
                                   Value *fvPtr = builder_.CreateConstInBoundsGEP1_64(
                                       i8Ty, pv, 8, "fp.re.ptr");
                                   Value *reV = builder_.CreateLoad(f64Ty, fvPtr, "fp.re");
                                   builder_.CreateBr(idxM);
                                   builder_.SetInsertPoint(imBB);
                                   Value *mpPtr = builder_.CreateConstInBoundsGEP1_64(
                                       i8Ty, pv, 16, "fp.im.ptr");
                                   Value *mpBits = builder_.CreateLoad(i64Ty, mpPtr, "fp.im.bits");
                                   Value *imV = builder_.CreateBitCast(mpBits, f64Ty, "fp.im");
                                   builder_.CreateBr(idxM);
                                   builder_.SetInsertPoint(idxM);
                                   auto *phiIdx = builder_.CreatePHI(f64Ty, 2, "fp.idx");
                                   phiIdx->addIncoming(reV, reBB);
                                   phiIdx->addIncoming(imV, imBB);
                                   pairFv = phiIdx;
                               }
                               return pairFv;
                           } else if (knownTag == 10) {
                               /* Known FLAT_ARRAY: direct double[] load, no tag check */
                               Value *pvalPtr = builder_.CreateConstInBoundsGEP1_64(i8Ty, pv, 8, "fa.dp");
                               Value *dblPtr  = builder_.CreateLoad(perlPtrTy_, pvalPtr, "fa.dpp");
                               dblPtr = builder_.CreateBitCast(dblPtr, f64Ty->getPointerTo());
                               Value *idx = emitIdx(*n.right);
                               dblPtr = builder_.CreateGEP(f64Ty, dblPtr, idx, "fa.gep");
                               return builder_.CreateLoad(f64Ty, dblPtr, "fa.val");
                           }
                           /* Unknown type: emit runtime tag dispatch */
                           Value *tag  = builder_.CreateLoad(i32Ty, pv, nm + ".tag");
                           Value *isPair = builder_.CreateICmpEQ(tag,
                               ConstantInt::get(i32Ty, 13), "ispair");
                           Value *isFlat = builder_.CreateICmpEQ(tag,
                               ConstantInt::get(i32Ty, 10), "isflat");
                           auto *pBB = BasicBlock::Create(ctx_, "fp.p",  curFn);
                           auto *flBB = BasicBlock::Create(ctx_, "fl.f",  curFn);
                           auto *flatBB = BasicBlock::Create(ctx_, "fa.f",  curFn);
                           auto *nBB = BasicBlock::Create(ctx_, "fp.n",  curFn);
                           auto *mBB = BasicBlock::Create(ctx_, "fp.m",  curFn);
                           /* Branch: isPair ? pBB : flBB */
                           builder_.CreateCondBr(isPair, pBB, flBB);
                          /* FLOAT_PAIR path: direct field load.
                               For fixed 0/1: compile-time select.
                               For variable index: runtime PHI between re (offset 8) and im (offset 16). */
                          builder_.SetInsertPoint(pBB);
                          Value *pairFv;
                          if (n.right->kind == NK::IntLit &&
                              (n.right->ival == 0 || n.right->ival == 1)) {
                              if (n.right->ival == 0) {
                                  Value *fvPtr = builder_.CreateConstInBoundsGEP1_64(
                                      i8Ty, pv, 8, "fp.re.ptr");
                                  pairFv = builder_.CreateLoad(f64Ty, fvPtr, "fp.re");
                              } else {
                                  Value *mpPtr = builder_.CreateConstInBoundsGEP1_64(
                                      i8Ty, pv, 16, "fp.im.ptr");
                                  Value *mpBits = builder_.CreateLoad(i64Ty, mpPtr, "fp.im.bits");
                                  pairFv = builder_.CreateBitCast(mpBits, f64Ty, "fp.im");
                              }
                          } else {
                              /* Variable index: PHI between re (offset 8) and im (offset 16) */
                              auto *idxV = emitIdx(*n.right);
                              Value *idx0 = builder_.CreateICmpEQ(idxV,
                                  ConstantInt::get(i64Ty, 0), "idx0");
                              auto *idxBB = BasicBlock::Create(ctx_, "fp.idx", curFn);
                              auto *reBB = BasicBlock::Create(ctx_, "fp.re", curFn);
                              auto *imBB = BasicBlock::Create(ctx_, "fp.im", curFn);
                              auto *idxM = BasicBlock::Create(ctx_, "fp.idm", curFn);
                              builder_.CreateCondBr(idx0, reBB, imBB);
                              builder_.SetInsertPoint(reBB);
                              Value *fvPtr = builder_.CreateConstInBoundsGEP1_64(
                                  i8Ty, pv, 8, "fp.re.ptr");
                              Value *reV = builder_.CreateLoad(f64Ty, fvPtr, "fp.re");
                              builder_.CreateBr(idxM);
                              builder_.SetInsertPoint(imBB);
                              Value *mpPtr = builder_.CreateConstInBoundsGEP1_64(
                                  i8Ty, pv, 16, "fp.im.ptr");
                              Value *mpBits = builder_.CreateLoad(i64Ty, mpPtr, "fp.im.bits");
                              Value *imV = builder_.CreateBitCast(mpBits, f64Ty, "fp.im");
                              builder_.CreateBr(idxM);
                              builder_.SetInsertPoint(idxM);
                              auto *phiIdx = builder_.CreatePHI(f64Ty, 2, "fp.idx");
                              phiIdx->addIncoming(reV, reBB);
                              phiIdx->addIncoming(imV, imBB);
                              pairFv = phiIdx;
                          }
                          builder_.CreateBr(mBB);
                          auto *pBBp = builder_.GetInsertBlock();
                          /* FLAT_ARRAY path: check isFlat, branch to flatBB or nBB */
                          builder_.SetInsertPoint(flBB);
                          builder_.CreateCondBr(isFlat, flatBB, nBB);
                          /* FLAT_ARRAY fast path: load double* from pval (offset 8),
                               GEP by index, load double directly */
                          builder_.SetInsertPoint(flatBB);
                          Value *pvalPtr = builder_.CreateConstInBoundsGEP1_64(i8Ty, pv, 8, "fa.dp");
                          Value *dblPtr  = builder_.CreateLoad(perlPtrTy_, pvalPtr, "fa.dpp");
                          dblPtr = builder_.CreateBitCast(dblPtr, f64Ty->getPointerTo());
                          Value *idx = emitIdx(*n.right);
                          dblPtr = builder_.CreateGEP(f64Ty, dblPtr, idx, "fa.gep");
                          Value *flatFv = builder_.CreateLoad(f64Ty, dblPtr, "fa.val");
                          builder_.CreateBr(mBB);
                          auto *flatBBp = builder_.GetInsertBlock();
                          /* Normal path: fall back to deref + get_ref + to_float */
                          builder_.SetInsertPoint(nBB);
                          Value *normArr = callRT("perl_deref_array_ro", {pv});
                          Value *normElem = callRT("perl_array_get_ref", {normArr, emitIdx(*n.right)});
                          Value *normFv   = callRT("perl_to_float", {normElem});
                          builder_.CreateBr(mBB);
                          auto *nBBp = builder_.GetInsertBlock();
                          builder_.SetInsertPoint(mBB);
                          auto *phi = builder_.CreatePHI(f64Ty, 3, "fp.val");
                          phi->addIncoming(pairFv, pBBp);
                          phi->addIncoming(flatFv, flatBBp);
                          phi->addIncoming(normFv, nBBp);
                          return phi;
                      }
                  }
              }
             return nullptr;
        } else {
            Value *base = emitExpr(*n.left);
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
    case NK::Call: {
        /* Inlineable sub with a float body — emit the body directly in F64 context,
           skipping boxing entirely (e.g. cabs2($zp) in a comparison). */
        auto it = inlineSubs_.find(n.name);
        if (it == inlineSubs_.end()) return nullptr;
        const auto &is = it->second;
        if (!canEmitF64(*is.bodyExpr) || n.args.size() != is.params.size()) return nullptr;
        pushScope();
        std::vector<Value *> ownedArgs;
        auto *f64Ty = Type::getDoubleTy(ctx_);
        for (size_t i = 0; i < is.params.size(); i++) {
            Value *argVal = nullptr;
            if (n.args[i]->kind == NK::Call) argVal = tryEmitInline(*n.args[i]);
            if (!argVal) argVal = emitExpr(*n.args[i]);
            auto *slot = builder_.CreateAlloca(perlPtrTy_, nullptr, "$" + is.params[i]);
            builder_.CreateStore(argVal, slot);
            declareVar(is.params[i], slot);
            if (isOwnedTemp(argVal)) ownedArgs.push_back(argVal);
        }
        Value *f64val = emitExprF64(*is.bodyExpr);
        popScope();
        for (Value *v : ownedArgs) callRT("perl_free", {v});
        return f64val;
    }
    default:
        return nullptr;
    }
}

/* ── top-level compile ───────────────────────────────────────────────────── */

void CodeGen::compile(const Node &program, const std::string &modName) {
    mod_->setModuleIdentifier(modName);
    sourceFile_ = modName;
    if (debug_) initializeDebugInfo(modName);

    /* collect sub definitions first so forward calls work */
    subs_.clear();
    subCaptures_.clear();
    for (auto &stmt : program.args)
        if (stmt->kind == NK::SubDef)
            subs_.push_back(stmt.get());

    /* Detect inlineable subs: body = "my ($p1,..) = @_; return expr".
       These are expanded at call sites without @_ construction. */
    for (auto *s : subs_) {
        if (!s->body) continue;
        const Node &body = *s->body;
        /* Need exactly: FlatBlock(my $p1; my $p2; ...; assign-from-@_) + Return */
        if (body.args.size() != 2) continue;
        const Node &fb = *body.args[0];
        const Node &ret = *body.args[1];
        if (fb.kind != NK::FlatBlock || ret.kind != NK::Return || !ret.left) continue;
        if (fb.args.empty()) continue;
        /* Last stmt in FlatBlock: ExprStmt(Assign(ArrayLit(vars), @_)) */
        const Node &lastFb = *fb.args.back();
        if (lastFb.kind != NK::ExprStmt || !lastFb.left) continue;
        const Node &asgn = *lastFb.left;
        if (asgn.kind != NK::Assign || !asgn.right || !asgn.left) continue;
        if (asgn.right->kind != NK::ArrayVar || asgn.right->name != "_") continue;
        if (asgn.left->kind != NK::ArrayLit) continue;
        /* Extract scalar param names */
        std::vector<std::string> params;
        bool allScalar = true;
        for (auto &p : asgn.left->args) {
            if (p->kind == NK::ScalarVar) {
                std::string nm = p->name;
                if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
                params.push_back(nm);
            } else { allScalar = false; break; }
        }
        if (!allScalar || params.empty()) continue;
        inlineSubs_[s->name] = {params, ret.left.get()};
    }

    /* pre-declare all subs as Functions */
    for (auto *s : subs_) {
        auto *ft = FunctionType::get(perlPtrTy_,
                        {arrayPtrTy_, Type::getInt32Ty(ctx_)},  /* PerlArray* args, int ctx */
                        false);
        auto *fn = Function::Create(ft, Function::ExternalLinkage,
                         subLLVMName(s->name), mod_.get());
        fn->addFnAttr(Attribute::AlwaysInline);
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
    for (auto *s : subs_) {
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
        {
            auto *gvArgv = new GlobalVariable(*mod_, perlPtrTy_, false,
                GlobalValue::InternalLinkage,
                Constant::getNullValue(perlPtrTy_), "g.arr.ARGV");
            builder_.CreateStore(argvArr, gvArgv);
            fileArrayGlobals_["ARGV"] = gvArgv;
        }

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

    /* Register perl_cleanup via atexit so valgrind reports zero leaks. */
    {
        auto *atexitFnTy = FunctionType::get(Type::getInt32Ty(ctx_),
                                             {PointerType::getUnqual(ctx_)}, false);
        auto atexitFn = mod_->getOrInsertFunction("atexit", atexitFnTy);
        auto *perlCleanupFn = mod_->getFunction("perl_cleanup");
        if (perlCleanupFn)
            builder_.CreateCall(cast<Function>(atexitFn.getCallee()), {perlCleanupFn});
    }

    builder_.CreateRet(ConstantInt::get(Type::getInt32Ty(ctx_), 0));

    inMainBody_ = false;
    /* emit sub bodies */
    for (auto *s : subs_) emitSub(*s);

    if (debug_) {
        dib_->finalize();
    }

    std::string err;
    raw_string_ostream es(err);
    if (verifyModule(*mod_, &es))
        throw std::runtime_error("LLVM verify error: " + err);
}

/* forward declarations — defined in "statement emission" section */
static bool hasLocalStmt(const Node &n);
static bool hasWantarrayOrUserCall(const Node &n);
static bool hasReturnStmt(const Node &n);
static bool hasDefaultVarUse(const Node &n);

/* ── sub definition ──────────────────────────────────────────────────────── */

void CodeGen::emitSub(const Node &n) {
    auto *fn = mod_->getFunction(subLLVMName(n.name));
    if (!fn) return;

    auto *entry = BasicBlock::Create(ctx_, "entry", fn);
    builder_.SetInsertPoint(entry);

    if (debug_) {
        builder_.SetCurrentDebugLocation(getDebugLoc(n.line, currentSP_));
    }

    /* derive package from sub name for caller() tracking */
    std::string savedPackage = currentPackage_;
    {
        auto sc = n.name.rfind("::");
        currentPackage_ = (sc != std::string::npos) ? n.name.substr(0, sc) : "main";
    }

    auto *savedFn = currentFn_;
    currentFn_ = fn;
    flatDoubleCache_.clear();  /* Stage 31: SSA Values from outer fn are invalid here */
    pushScope();

    /* @_ is the first argument (PerlArray*) */
    Value *argsArr = fn->getArg(0);
    argsArr->setName("args");
    Value *ctxArg = fn->getArg(1);

    /* Stage 24a: skip push/pop wantarray for functions with no wantarray expression
       and no user sub calls (called functions read the CALLER's push, so omitting
       our push would corrupt their context if any of them use wantarray). */
    bool subNeedsWantarray = !n.body || hasWantarrayOrUserCall(*n.body);
    bool savedNeedsWantarray = currentSubNeedsWantarray_;
    currentSubNeedsWantarray_ = subNeedsWantarray;
    if (subNeedsWantarray)
        callRT("perl_push_wantarray", {ctxArg});

    declareArray("_", argsArr);

    /* Stage 27a: only pre-declare $_ when the sub body actually uses it.
       Skipping this eliminates 1 alloc_undef + 1 free per call for pure
       numeric subs like advance() that never touch $_. */
    bool subUsesDefaultVar = !n.body || hasDefaultVarUse(*n.body);
    if (subUsesDefaultVar) {
        Value *udv  = callRT("perl_alloc_undef", {});
        auto *slotUs = builder_.CreateAlloca(perlPtrTy_, nullptr, "$_");
        builder_.CreateStore(udv, slotUs);
        declareVar("_", slotUs);
        trackPv(udv);  /* ensure $_ stable pv is freed on scope exit */
    }

    /* Sub-task 2 (named-sub closure capture): if the sub has a
       capture list (built at the RefSub site when the sub was
       referenced via \&subname), install local allocas for each
       captured shared scalar and load them from
       `perl_get_capture(i)`.  This is the same pattern AnonSub
       uses.  Without this step, named subs called from
       threads->create would read whatever was in the local
       allocas (uninitialised or stale). */
    if (n.body) {
        auto it = subCaptures_.find(n.name);
        if (it != subCaptures_.end()) {
            auto i64Ty = Type::getInt64Ty(ctx_);
            const auto &caps = it->second;
            for (size_t i = 0; i < caps.size(); i++) {
                Value *pv = callRT("perl_get_capture",
                                   {ConstantInt::get(i64Ty, (long long)i)});
                auto *capSlot = builder_.CreateAlloca(perlPtrTy_, nullptr, caps[i] + ".cap");
                builder_.CreateStore(pv, capSlot);
                declareVar(caps[i], capSlot);
            }
        }
    }

    /* capture local() save depth at function entry.
       Stage 24b: skip the alloca + perl_local_save_depth entirely when the
       function has no local() AND no explicit return — in that case
       localDepthAlloca_ is never read (implicit return skips restore, and
       NK::Return is never emitted), so nullptr is safe. */
    auto *i32Ty = Type::getInt32Ty(ctx_);
    auto *savedLocalDepth = localDepthAlloca_;
    bool subNeedsLocal = n.body && hasLocalStmt(*n.body);
    bool subNeedsReturn = n.body && hasReturnStmt(*n.body);
    if (subNeedsLocal || subNeedsReturn) {
        localDepthAlloca_ = builder_.CreateAlloca(i32Ty, nullptr, "local.depth");
        builder_.CreateStore(callRT("perl_local_save_depth", {}), localDepthAlloca_);
    } else {
        localDepthAlloca_ = nullptr;
    }

    /* forward declaration: emit empty body that returns undef */
    if (!n.body) {
        if (subNeedsWantarray) callRT("perl_pop_wantarray", {});
        if (subNeedsLocal) {
            Value *depth = builder_.CreateLoad(i32Ty, localDepthAlloca_);
            callRT("perl_local_restore_to", {depth});
        }
        popScope();  /* free $_ */
        builder_.CreateRet(perlUndef());
        localDepthAlloca_ = savedLocalDepth;
        currentSubNeedsWantarray_ = savedNeedsWantarray;
        currentFn_ = savedFn;
        return;
    }

    auto *savedSubBody = currentSubBody_;
    currentSubBody_ = n.body.get();

    /* Stage 25: pre-analyze the body to find promotable @_ args.
       For my ($a, $b) = @_ patterns, skip the PV alloca entirely in NK::My
       and fill the unboxed alloca directly from the borrowed args element. */
    auto savedPrePromoted = prePromotedArgs_;
    prePromotedArgs_.clear();
    auto tryFindAtAssign = [&](const Node &blk) {
        for (auto &stmt : blk.args) {
            const Node *asgn = nullptr;
            if (stmt->kind == NK::ExprStmt && stmt->left)
                asgn = stmt->left.get();
            if (!asgn || asgn->kind != NK::Assign) continue;
            if (!asgn->left  || asgn->left->kind  != NK::ArrayLit) continue;
            if (!asgn->right || asgn->right->kind != NK::ArrayVar || asgn->right->name != "_") continue;
            for (auto &lhsElem : asgn->left->args) {
                if (lhsElem->kind != NK::ScalarVar) continue;
                const std::string &nm = lhsElem->name;
                if (prePromotedArgs_.count(nm)) continue;
                /* Stage 27c: DerefAV pre-promotion.
                   If the arg is only used as an array-ref deref base ($x->[$i]),
                   skip the PV alloca entirely — call perl_deref_array_ro directly on
                   the borrowed @_ element and cache the PerlArray* in a derefAV alloca.
                   This eliminates 3 pool ops per call (alloc + assign + free). */
                if (isOnlyArrayRefDeref(*n.body, nm)) {
                    prePromotedArgs_[nm] = PPKind::DerefAV;
                    continue;
                }
                bool safe   = floatSafe(*n.body, nm, false);
                bool needFP = needsFloatPrec(*n.body, nm);
                bool used   = hasVar(*n.body, nm);
                if (safe && needFP)
                    prePromotedArgs_[nm] = PPKind::Float;
                else if (safe && !needFP && used)
                    prePromotedArgs_[nm] = PPKind::Int;
            }
        }
    };
    tryFindAtAssign(*n.body);
    for (auto &stmt : n.body->args)
        if (stmt->kind == NK::FlatBlock) tryFindAtAssign(*stmt);

    Value *lastVal = emitBlockLast(*n.body);
    currentSubBody_ = savedSubBody;
    prePromotedArgs_ = std::move(savedPrePromoted);

    /* implicit return from last expression (Perl: last expr is the return value) */
    if (!builder_.GetInsertBlock()->getTerminator()) {
        if (subNeedsWantarray) callRT("perl_pop_wantarray", {});
        if (subNeedsLocal) {
            Value *depth = builder_.CreateLoad(i32Ty, localDepthAlloca_);
            callRT("perl_local_restore_to", {depth});
        }
        popScope();  /* free $_ and other function-scope pvs before ret */
        builder_.CreateRet(lastVal);
    } else {
        popScope();  /* explicit return: emitScopeCleanup already freed pvs; skip due to terminator */
    }
    localDepthAlloca_ = savedLocalDepth;
    currentSubNeedsWantarray_ = savedNeedsWantarray;
    currentFn_ = savedFn;
    currentPackage_ = savedPackage;

    /* restore insert point to end of main (for any remaining stmts) */
    /* caller will set insert point back */
}

/* ── statement emission ──────────────────────────────────────────────────── */

/* Stage 24a: skip push/pop wantarray for functions with no wantarray expr and
   no user sub calls (NK::Call) — called functions read from the caller's push,
   so skipping is unsafe if any nested user sub might call wantarray. */
static bool hasWantarrayOrUserCall(const Node &n) {
    if (n.kind == NK::WantarrayFunc) return true;
    if (n.kind == NK::Call)         return true;  /* any user-defined sub call */
    /* return (LIST) uses perl_array_to_list_return which reads wantarray stack */
    if (n.kind == NK::Return && n.left) {
        NK lk = n.left->kind;
        if (lk == NK::ArrayLit || lk == NK::ArrayVar || lk == NK::MapFunc ||
            lk == NK::GrepFunc || lk == NK::SortFunc || lk == NK::DerefArray ||
            lk == NK::ReverseFunc) return true;
    }
    /* implicit list return from grep/map/sort also reads wantarray stack */
    if (n.kind == NK::MapFunc || n.kind == NK::GrepFunc ||
        n.kind == NK::SortFunc) return true;
    bool r = false;
    if (n.left)  r = r || hasWantarrayOrUserCall(*n.left);
    if (n.right) r = r || hasWantarrayOrUserCall(*n.right);
    for (auto &a : n.args) { if (!r) r = hasWantarrayOrUserCall(*a); }
    if (n.body)  r = r || hasWantarrayOrUserCall(*n.body);
    if (n.init)  r = r || hasWantarrayOrUserCall(*n.init);
    if (n.cond)  r = r || hasWantarrayOrUserCall(*n.cond);
    if (n.step)  r = r || hasWantarrayOrUserCall(*n.step);
    for (auto &b : n.branches) {
        if (!r && b.cond) r = hasWantarrayOrUserCall(*b.cond);
        if (!r && b.body) r = hasWantarrayOrUserCall(*b.body);
    }
    return r;
}

/* Stage 24b: skip local-depth alloca + perl_local_save_depth for functions
   with no local() AND no explicit return — in that case localDepthAlloca_
   is never read (implicit return skips restore when !subNeedsLocal, and
   NK::Return is never emitted). */
static bool hasReturnStmt(const Node &n) {
    if (n.kind == NK::Return) return true;
    bool r = false;
    if (n.left)  r = r || hasReturnStmt(*n.left);
    if (n.right) r = r || hasReturnStmt(*n.right);
    for (auto &a : n.args) { if (!r) r = hasReturnStmt(*a); }
    if (n.body)  r = r || hasReturnStmt(*n.body);
    if (n.init)  r = r || hasReturnStmt(*n.init);
    if (n.cond)  r = r || hasReturnStmt(*n.cond);
    if (n.step)  r = r || hasReturnStmt(*n.step);
    for (auto &b : n.branches) {
        if (!r && b.cond) r = hasReturnStmt(*b.cond);
        if (!r && b.body) r = hasReturnStmt(*b.body);
    }
    return r;
}

/* Stage 23: only emit allflat pre-check for loops whose body contains a nested
   foreach — simple single-level loops don't benefit enough to pay the call cost. */
static bool hasNestedForEach(const Node &n) {
    if (n.kind == NK::Foreach) return true;
    bool r = false;
    if (n.left)  r = r || hasNestedForEach(*n.left);
    if (n.right) r = r || hasNestedForEach(*n.right);
    for (auto &a : n.args) { if (!r) r = hasNestedForEach(*a); }
    if (n.body)  r = r || hasNestedForEach(*n.body);
    if (n.init)  r = r || hasNestedForEach(*n.init);
    if (n.cond)  r = r || hasNestedForEach(*n.cond);
    if (n.step)  r = r || hasNestedForEach(*n.step);
    for (auto &b : n.branches) {
        if (!r && b.cond) r = hasNestedForEach(*b.cond);
        if (!r && b.body) r = hasNestedForEach(*b.body);
    }
    return r;
}

/* Stage 27a: returns true if the body references $_ (explicitly or implicitly).
   Conservative: also fires for foreach without explicit loop var, and for
   common builtins that default to $_ when given no arguments. */
static bool hasDefaultVarUse(const Node &n) {
    /* Explicit $_ reference */
    if (n.kind == NK::ScalarVar && (n.name == "_" || n.name == "$_")) return true;
    /* foreach/for without explicit var name — loop var defaults to $_ */
    if (n.kind == NK::Foreach && n.name.empty()) return true;
    bool r = false;
    if (n.left)  r = r || hasDefaultVarUse(*n.left);
    if (n.right) r = r || hasDefaultVarUse(*n.right);
    for (auto &a : n.args) { if (!r) r = hasDefaultVarUse(*a); }
    if (n.body)  r = r || hasDefaultVarUse(*n.body);
    if (n.init)  r = r || hasDefaultVarUse(*n.init);
    if (n.cond)  r = r || hasDefaultVarUse(*n.cond);
    if (n.step)  r = r || hasDefaultVarUse(*n.step);
    for (auto &b : n.branches) {
        if (!r && b.cond) r = hasDefaultVarUse(*b.cond);
        if (!r && b.body) r = hasDefaultVarUse(*b.body);
    }
    return r;
}

/* Stage 17: skip local save/restore for blocks that contain no local() */
static bool hasLocalStmt(const Node &n) {
    if (n.kind == NK::LocalStmt) return true;
    bool r = false;
    if (n.left)  r = r || hasLocalStmt(*n.left);
    if (n.right) r = r || hasLocalStmt(*n.right);
    for (auto &a : n.args) { if (!r) r = hasLocalStmt(*a); }
    if (n.body)  r = r || hasLocalStmt(*n.body);
    if (n.init)  r = r || hasLocalStmt(*n.init);
    if (n.cond)  r = r || hasLocalStmt(*n.cond);
    if (n.step)  r = r || hasLocalStmt(*n.step);
    for (auto &b : n.branches) {
        if (!r && b.cond) r = hasLocalStmt(*b.cond);
        if (!r && b.body) r = hasLocalStmt(*b.body);
    }
    return r;
}

Value *CodeGen::emitBlock(const Node &n) {
    auto *i32Ty = Type::getInt32Ty(ctx_);
    bool needLocal = hasLocalStmt(n);
    llvm::Value *bdAlloca = nullptr;
    if (needLocal) {
        bdAlloca = builder_.CreateAlloca(i32Ty, nullptr, "block.ldepth");
        builder_.CreateStore(callRT("perl_local_save_depth", {}), bdAlloca);
    }
    pushScope();
    for (auto &stmt : n.args) {
        emitStmt(*stmt);
        if (builder_.GetInsertBlock()->getTerminator()) break;
    }
    popScope();
    if (needLocal && !builder_.GetInsertBlock()->getTerminator())
        callRT("perl_local_restore_to", {builder_.CreateLoad(i32Ty, bdAlloca)});
    return nullptr;
}

/* Emit a block and return the PerlValue* of its last expression statement. */
Value *CodeGen::emitBlockLast(const Node &n) {
    auto *i32Ty = Type::getInt32Ty(ctx_);
    bool needLocal = hasLocalStmt(n);
    llvm::Value *bdAlloca = nullptr;
    if (needLocal) {
        bdAlloca = builder_.CreateAlloca(i32Ty, nullptr, "block.ldepth");
        builder_.CreateStore(callRT("perl_local_save_depth", {}), bdAlloca);
    }
    pushScope();
    /* Stage 27b: start with null; allocate only if no expr provides a value.
       Avoids a dead alloc_undef when the block's return value is unused (e.g.
       advance() implicit-return undef — caller always frees it, but we delay
       the alloc to the exit path so the entry path is allocation-free). */
    Value *result = nullptr;
    for (size_t i = 0; i < n.args.size(); i++) {
        const Node &stmt = *n.args[i];
        bool isLast = (i + 1 == n.args.size());
        if (isLast && stmt.kind == NK::ExprStmt && stmt.left) {
            /* If the last expr produces a list (grep/map/sort/etc.) and we're in
               a wantarray-aware sub, wrap it for list/scalar context propagation.
               Dispatch by producer kind so scalar-context semantics are correct:
               grep → count, sort → undef, map → last element, others → last elem. */
            const Node &le = *stmt.left;
            NK lk = le.kind;
            bool isListProducer = (lk == NK::MapFunc || lk == NK::GrepFunc ||
                                   lk == NK::SortFunc || lk == NK::DerefArray ||
                                   lk == NK::ReverseFunc);
            if (isListProducer && currentSubNeedsWantarray_) {
                Value *av = emitArrayPtr(le);
                if (!av) av = callRT("perl_array_new", {});
                const char *helper = "perl_array_to_list_return";
                if (lk == NK::GrepFunc)      helper = "perl_grep_list_return";
                else if (lk == NK::MapFunc)   helper = "perl_map_list_return";
                else if (lk == NK::SortFunc)  helper = "perl_sort_list_return";
                result = callRT(helper, {av});
            } else {
                result = emitExpr(le);
            }
        } else {
            emitStmt(stmt);
        }
        if (builder_.GetInsertBlock()->getTerminator()) { break; }
    }
    /* If the last statement emitted a terminator (e.g. `return` branched
       to an enclosing eval's endBB or sub's exit), the block already has
       control-flow resolved — skip post-block cleanup (clone/free/popScope/
       local_restore) which would otherwise be emitted into a terminated
       block, producing "Terminator found in the middle of a basic block!". */
    if (builder_.GetInsertBlock()->getTerminator()) {
        /* still pop the scope stack so destructors run for any my-vars
           captured by the inner expression; popScope emits no IR when the
           current block is terminated. */
        popScope();
        return nullptr;
    }
    /* Clone result before popScope() frees variables it may reference.
       Also free the original if it was an owned temp (mirrors NK::Return logic). */
    if (result && !llvm::isa<llvm::ConstantPointerNull>(result)) {
        Value *orig = result;
        result = callRT("perl_clone", {result});
        freeIfOwned(orig);
    }
    popScope();
    if (needLocal && !builder_.GetInsertBlock()->getTerminator())
        callRT("perl_local_restore_to", {builder_.CreateLoad(i32Ty, bdAlloca)});
    if (!result || llvm::isa<llvm::ConstantPointerNull>(result))
        result = llvm::ConstantPointerNull::get(perlPtrTy_);
    return result;
}

void CodeGen::emitStmt(const Node &n) {
    if (builder_.GetInsertBlock()->getTerminator()) return;
    if (debug_ && n.line > 0) {
        builder_.SetCurrentDebugLocation(getDebugLoc(n.line, currentSP_));
    }
    switch (n.kind) {
    case NK::Block: {
        /* Save local depth so lock() / local() inside bare { } blocks
           auto-restore when the block exits (Perl scope semantics). */
        auto *i32Ty = Type::getInt32Ty(ctx_);
        Value *savedDepth = callRT("perl_local_save_depth", {});
        emitBlock(n);
        if (!builder_.GetInsertBlock()->getTerminator())
            callRT("perl_local_restore_to", {savedDepth});
        break;
    }

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
                /* also register as Package::name for cross-package access */
                if (currentPackage_ != "main") fileHashGlobals_[currentPackage_ + "::" + nm] = gv;
                /* do NOT declareHash — lookupHash loads fresh from GlobalVariable */
            } else {
                declareHash(nm, hv);
            }
            if (n.ival & 1) callRT("perl_hash_make_shared", {hv});
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
            /* Save the outer callCtx_ before forcing list context for the
               RHS — the surrounding call site (e.g. a CallCodeRef inside
               `my @arr = (sub{...})->(args)`) depends on callCtx_ still
               being 1 after we return. */
            int savedCallCtx = callCtx_;
            if (n.right) { callCtx_ = 1; av = emitArrayPtr(*n.right); }
            if (!av) {
                  av = callRT("perl_array_new", {});
                  /* scalar RHS (e.g. my @arr = $ref  or  my @arr = [1,2,3]) —
                     push the value as a single element */
                  if (n.right) {
                      callCtx_ = 1;
                      Value *rhsVal = emitExpr(*n.right);
                      callRT("perl_array_push_list_or_scalar", {av, rhsVal});
                  }
              }
            callCtx_ = savedCallCtx;
            if (atFileScope) {
                auto *gv = new GlobalVariable(*mod_, perlPtrTy_, false,
                    GlobalValue::InternalLinkage,
                    Constant::getNullValue(perlPtrTy_), "g.arr." + nm);
                builder_.CreateStore(av, gv);
                fileArrayGlobals_[nm] = gv;
                /* also register as Package::name for cross-package access */
                if (currentPackage_ != "main") fileArrayGlobals_[currentPackage_ + "::" + nm] = gv;
                /* do NOT declareArray — lookupArray loads fresh from GlobalVariable */
            } else {
                declareArray(nm, av);
            }
            if (n.ival & 1) callRT("perl_array_make_shared", {av});
            /* our @ISA = ('Parent', ...) — wire up ISA chain */
            if (nm == "ISA" && n.right && !n.sval.empty()) {
                Value *child = builder_.CreateGlobalStringPtr(n.sval);
                auto processISAElem = [&](const Node &elem) {
                    if (elem.kind == NK::StringLit) {
                        Value *parent = builder_.CreateGlobalStringPtr(elem.sval);
                        callRT("perl_set_isa", {child, parent});
                    }
                };
                if (n.right->kind == NK::ArrayLit) {
                    for (auto &elem : n.right->args) processISAElem(*elem);
                } else if (n.right->kind == NK::StringLit) {
                    processISAElem(*n.right);
                }
            }
        } else {
            /* n.name may carry a '$' prefix when parsed in expression context */
            std::string nm = n.name;
            if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
            bool isShared = (n.ival & 1) != 0;
            if (isShared) {
                /* threads::shared variable: Phase-2 layout.  The cell IS the
                   PerlValue; no wrapper, no per-cell mutex at allocation.
                   The SharedMutex is lazy-installed on the first lock() or
                   cond_wait() call (see get_or_install_mutex in runtime.c).
                   Shared scalars must not be unboxed to int/float — that
                   would lose the PV_FLAG_SHARED bit and break cross-thread
                   visibility.  The "no int/float unbox" invariant is
                   upheld by storing in the regular scopes_ map and
                   skipping the intScopes_/floatScopes_ paths below.

                   Sub-task 3 (`our $x : shared`): at file scope, also
                   register the cell pointer in fileScalarGlobals_ so
                   cross-package access (e.g. `$Foo::counter` from
                   package main, or threads->create(\&Foo::inc))
                   resolves to the same cell the package-local $x
                   uses.  Without this, $Foo::counter would be undef
                   because the lookup falls through to
                   packageScalarMap_ which only knows about my $x
                   declared in main.  We register under both the bare
                   name (for in-package reads) and the qualified name
                   (for cross-package reads), matching the existing
                   `my $scalar` file-scope path at line 2606. */
                Value *pv;
                if (atFileScope) {
                    auto *gv = new GlobalVariable(*mod_, perlPtrTy_, false,
                        GlobalValue::InternalLinkage,
                        Constant::getNullValue(perlPtrTy_), "g." + nm);
                    pv = callRT("perl_make_shared_scalar", {});
                    builder_.CreateStore(pv, gv);
                    fileScalarGlobals_[nm] = gv;
                    if (currentPackage_ != "main")
                        fileScalarGlobals_[currentPackage_ + "::" + nm] = gv;
                    declareVar(nm, gv);
                } else {
                    auto *alloca = builder_.CreateAlloca(perlPtrTy_, nullptr, n.name);
                    pv = callRT("perl_make_shared_scalar", {});
                    builder_.CreateStore(pv, alloca);
                    /* no trackPv — shared vars have program lifetime */
                    declareVar(nm, alloca);
                }
                if (n.right) {
                    Value *init = emitExpr(*n.right);
                    callRT("perl_assign", {pv, init});
                    freeIfOwned(init);
                }
                sharedScalarNames_.insert(nm);  /* Phase 3: route through perl_atomic_* */
                break;
            }
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
                /* also register as Package::name for cross-package access */
                if (currentPackage_ != "main")
                    fileScalarGlobals_[currentPackage_ + "::" + nm] = gv;
            } else {
                /* Unbox numeric scalars: skip PerlValue* alloca entirely.
                   Guard: 1D ArrowDeref may return an array/hash ref, not a scalar. */
                bool rhsMayBeRef = n.right &&
                    n.right->kind == NK::ArrowDeref && n.right->sval == "array" &&
                    n.right->left && n.right->left->kind != NK::ArrowDeref;
                if (n.right && !atFileScope && !rhsMayBeRef) {
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
                        /* Stage 30: if this var was assigned sqrt(x), remember x.
                           Enables v*v → x and v*v*v → x*v (shorter sqrt critical path). */
                        if (lastSqrtInput_) { floatSqrtOf_[nm] = lastSqrtInput_; lastSqrtInput_ = nullptr; }
                        break;
                    }
                }
                /* Stage 25/27c: @_ arg pre-promoted to int/float/derefAV —
                   skip PV alloca entirely.
                   Only applies when n.right is null (bare my $var; no RHS). */
                if (!n.right) {
                    auto ppIt = prePromotedArgs_.find(nm);
                    if (ppIt != prePromotedArgs_.end()) {
                        auto *i64Ty = Type::getInt64Ty(ctx_);
                        auto *f64Ty = Type::getDoubleTy(ctx_);
                        if (ppIt->second == PPKind::Float) {
                            auto *fa = builder_.CreateAlloca(f64Ty, nullptr, nm + ".f");
                            builder_.CreateStore(ConstantFP::get(f64Ty, 0.0), fa);
                            declareFloatVar(nm, fa);
                        } else if (ppIt->second == PPKind::Int) {
                            auto *ia = builder_.CreateAlloca(i64Ty, nullptr, nm + ".i");
                            builder_.CreateStore(ConstantInt::get(i64Ty, 0), ia);
                            declareIntVar(nm, ia);
                        } else { /* PPKind::DerefAV: Stage 27c — borrow @_ elem into PV slot.
                                    Create a perlPtrTy_ alloca in scopes_ (NOT trackPv'd) so
                                    emitExpr(ScalarVar) still finds a valid PerlValue*.
                                    The derefAV alloca (PerlArray*) is filled in the Assign handler. */
                            auto *pvA = builder_.CreateAlloca(perlPtrTy_, nullptr, nm);
                            builder_.CreateStore(ConstantPointerNull::get(perlPtrTy_), pvA);
                            declareVar(nm, pvA);  /* in scopes_, NOT trackPv */
                        }
                        break; /* done — no PV alloca, no trackPv */
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
                for (size_t i = 0; i < n.args.size(); i++) {
                    if (i > 0) callRT("perl_print_sep_fh", {fh});
                    bool lastArg = (i + 1 == n.args.size());
                    callRT(isSay && lastArg ? "perl_say_fh" : "perl_print_fh",
                           {fh, emitExpr(*n.args[i])});
                }
            }
            if (!isSay) callRT("perl_print_ors_fh", {fh});
        } else {
            /* print/say to stdout */
            if (n.args.empty()) {
                if (auto *slot = lookupVar("_")) {
                    Value *v = builder_.CreateLoad(perlPtrTy_, slot);
                    callRT(isSay ? "perl_say" : "perl_print", {v});
                }
            } else if (n.args.size() == 1) {
                /* Only expand @arr / @$ref / @{expr} — not function calls which may
                   return scalars and must go through perl_say for the newline. */
                NK ak = n.args[0]->kind;
                bool isExplicitArray = (ak == NK::ArrayVar || ak == NK::DerefArray ||
                                        ak == NK::ArraySlice || ak == NK::HashSlice ||
                                        (ak == NK::PostfixDeref && n.args[0]->sval == "all_array"));
                Value *av = isExplicitArray ? emitArrayPtr(*n.args[0]) : nullptr;
                if (av) {
                    callRT("perl_print_array", {av});
                    if (isSay) callRT("perl_print_string",
                                     {builder_.CreateGlobalString("\n", ".nl")});
                } else {
                    Value *v = emitExpr(*n.args[0]);
                    callRT(isSay ? "perl_say" : "perl_print", {v});
                }
            } else {
                for (size_t i = 0; i < n.args.size(); i++) {
                    if (i > 0) callRT("perl_print_sep", {});
                    NK ak = n.args[i]->kind;
                    bool isExplicitArray = (ak == NK::ArrayVar || ak == NK::DerefArray ||
                                            ak == NK::ArraySlice || ak == NK::HashSlice ||
                                            (ak == NK::PostfixDeref && n.args[i]->sval == "all_array"));
                    Value *av = isExplicitArray ? emitArrayPtr(*n.args[i]) : nullptr;
                    if (av)
                        callRT("perl_print_array", {av});
                    else
                        callRT("perl_print", {emitExpr(*n.args[i])});
                }
                if (isSay) {
                    auto *nl = builder_.CreateGlobalString("\n", ".nl");
                    callRT("perl_print_string", {nl});
                }
            }
            if (!isSay) callRT("perl_print_ors", {});
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
        loopRedos_.push_back(body);
        if (!n.sval.empty()) loopLabels_.push_back({n.sval, exit, cond, body});

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
        loopRedos_.pop_back();
        if (!n.sval.empty()) loopLabels_.pop_back();
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
        loopRedos_.push_back(body);

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
        loopRedos_.pop_back();
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
        loopRedos_.push_back(bodyBB);

        pushScope();
        if (n.init) emitStmt(*n.init);

        /* Stage 26c: if the loop body is a single call to a named sub with
           all loop-invariant args (literals + array refs), build the args
           array once before the loop and reuse it every iteration.
           Saves: array_new + N*(alloc+push+free) + array_free per iteration. */
        auto isInvariantArg = [](const Node &a) {
            return a.kind == NK::IntLit || a.kind == NK::FloatLit ||
                   a.kind == NK::StringLit || a.kind == NK::RefArray;
        };
        const Node *hoistCallNode = nullptr;
        llvm::Function *hoistFn   = nullptr;
        if (n.body && n.body->args.size() == 1) {
            const Node *stmt = n.body->args[0].get();
            /* body may be wrapped in ExprStmt */
            const Node *call = nullptr;
            if (stmt->kind == NK::ExprStmt && stmt->left)
                call = stmt->left.get();
            else if (stmt->kind == NK::Call)
                call = stmt;
            if (call && call->kind == NK::Call && !call->name.empty()) {
                if (auto *lf = mod_->getFunction(subLLVMName(call->name))) {
                    bool allInv = true;
                    for (auto &a : call->args)
                        if (!isInvariantArg(*a)) { allInv = false; break; }
                    if (allInv) { hoistCallNode = call; hoistFn = lf; }
                }
            }
        }
        Value *hoistedArgs = nullptr;
        if (hoistCallNode) {
            hoistedArgs = callRT("perl_array_new", {});
            for (auto &arg : hoistCallNode->args) {
                Value *v = emitExpr(*arg);
                callRT("perl_array_push", {hoistedArgs, v});
                freeIfOwned(v);
            }
        }

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
        if (hoistFn && hoistedArgs) {
            /* Emit the call with pre-built args; free only the return value */
            auto *i32Ty = Type::getInt32Ty(ctx_);
            Value *ret = builder_.CreateCall(hoistFn,
                {hoistedArgs, ConstantInt::get(i32Ty, 0)});
            freeIfOwned(ret);
        } else {
            /* Stage 32: hoist perl_deref_array calls for loop-invariant variables.
               Collect deref targets (ScalarVars that are direct args of ArrowDeref),
               then for each target that is not modified in this loop body, emit
               perl_deref_array(lookupVar(nm)) before the loop and cache it.
               Inside the loop, ArrowDeref emission checks the cache first. */
            if (n.body) {
                std::set<std::string> derefTargets;
                collectDerefTargets(*n.body, derefTargets);
                emitHoistedDerefs(*n.body, derefTargets);
            }
            emitBlock(*n.body);
        }
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateBr(stepBB);

        builder_.SetInsertPoint(stepBB);
        if (n.step) {
            /* Stage 26b: post/pre ++/-- on an unboxed int var — increment
               directly, skipping the dead alloc_int(old_val)+free round-trip. */
            bool handledStep = false;
            if (n.step->kind == NK::UnaryOp && n.step->left &&
                n.step->left->kind == NK::ScalarVar) {
                const std::string &sv = n.step->sval;
                if (sv == "post++" || sv == "post--" ||
                    sv == "pre++"  || sv == "pre--") {
                    std::string nm = n.step->left->name;
                    if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
                    if (Value *ia = lookupIntVar(nm)) {
                        auto *i64Ty = Type::getInt64Ty(ctx_);
                        Value *cur = builder_.CreateLoad(i64Ty, ia, nm + ".cur");
                        bool isInc = (sv == "post++" || sv == "pre++");
                        Value *next = builder_.CreateAdd(cur,
                            ConstantInt::get(i64Ty, isInc ? 1LL : -1LL), nm + ".step");
                        builder_.CreateStore(next, ia);
                        handledStep = true;
                    }
                }
            }
            if (!handledStep) freeIfOwned(emitExpr(*n.step));
        }
        builder_.CreateBr(condBB);

        loopExits_.pop_back();
        loopContinues_.pop_back();
        loopRedos_.pop_back();
        builder_.SetInsertPoint(exit);
        if (hoistedArgs) callRT("perl_array_free", {hoistedArgs});
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
        /* Stage 28: inner loops (nested within another loop) get unroll+vectorize
           hints on their back-branch so LLVM's SLP vectorizer can combine two
           independent sqrt computations (e.g. two j-iterations) into sqrtpd. */
        bool isInnerLoop = !loopExits_.empty();
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
            /* counterAlloca: hidden loop counter — never exposed to user code.
               iterAlloca: user-visible $VAR — refreshed from counter at each body entry.
               This mirrors Perl semantics: $i++ inside foreach body does not advance
               the loop; the loop always advances its own counter by exactly 1. */
            auto *counterAlloca = builder_.CreateAlloca(i64, nullptr, loopNm + ".counter");
            auto *iterAlloca    = builder_.CreateAlloca(i64, nullptr, loopNm + ".i");
            builder_.CreateStore(lo, counterAlloca);

            auto *condBB2 = BasicBlock::Create(ctx_, "foreach.cond", fn);
            loopExits_.push_back(exit);
            loopContinues_.push_back(stepBB);
            loopRedos_.push_back(bodyBB);
            if (!n.sval.empty()) loopLabels_.push_back({n.sval, exit, stepBB, bodyBB});

            /* Stage 23: call perl_array_is_all_flat ONCE before the loop for each
               derefAV that will be row-dereffed in the body. The result is stored
               in a loop-invariant i1 alloca. At body entry, the row-deref branches
               on this flag instead of pvTag; LLVM's loop-unswitch then specializes
               the loop into a flat-only version where GVN+InstSimplify can prove
               every `fra` load is !nonnull, eliminating the per-access null-check.
               Guard: only pay the allflat call when the body has a nested foreach
               (simple single-level loops don't benefit enough to cover the call cost). */
            std::vector<std::string> newAllflatNames;
            if (n.body && hasNestedForEach(*n.body)) {
                std::unordered_set<std::string> avNamesS23;
                for (auto &scope : derefAVScopes_)
                    for (auto &[nm, _] : scope) avNamesS23.insert(nm);
                if (!avNamesS23.empty()) {
                    std::set<std::pair<std::string,std::string>> prePairs;
                    collectRowAVPairs(*n.body, avNamesS23, prePairs);
                    std::unordered_set<std::string> avChecked;
                    auto *i1Ty   = Type::getInt1Ty(ctx_);
                    auto *i64_   = Type::getInt64Ty(ctx_);
                    for (auto &[outerNm, idxNm] : prePairs) {
                        if (!avChecked.insert(outerNm).second) continue;
                        if (avAllflatSlots_.count(outerNm)) continue; /* outer loop handles it */
                        Value *outerPA = lookupDerefAV(outerNm);
                        if (!outerPA) continue;
                        Value *outerArr = builder_.CreateLoad(arrayPtrTy_, outerPA,
                                                              outerNm + ".avS23");
                        Value *af_i64 = callRT("perl_array_is_all_flat", {outerArr});
                        Value *af_i1  = builder_.CreateICmpNE(af_i64,
                                            ConstantInt::get(i64_, 0), outerNm + ".af");
                        auto *af_slot = builder_.CreateAlloca(i1Ty, nullptr,
                                                              outerNm + ".af.slot");
                        builder_.CreateStore(af_i1, af_slot);
                        avAllflatSlots_[outerNm] = af_slot;
                        newAllflatNames.push_back(outerNm);
                    }
                }
            }

            builder_.CreateBr(condBB2);
            builder_.SetInsertPoint(condBB2);
            Value *cur = builder_.CreateLoad(i64, counterAlloca);
            builder_.CreateCondBr(builder_.CreateICmpSLE(cur, hi), bodyBB, exit);

            builder_.SetInsertPoint(bodyBB);
            /* Refresh user variable from the hidden counter at each body entry. */
            builder_.CreateStore(cur, iterAlloca);
            pushScope();
            declareIntVar(loopNm, iterAlloca);

            /* Stage 16/19: pre-emit row derefs for hot 2D patterns.
               Collect (outerVar, firstIndexVar) pairs — for each we know:
               (a) outerVar is a cached PerlArray* (derefAVScopes_), and
               (b) the firstIndex is a loop variable (int alloca).
               Emitting the deref once per body entry eliminates repeated
               recomputation blocked by update_float aliasing.
               Stage 23: when avAllflatSlots_[outerNm] is set, branch on the
               loop-invariant allflat flag instead of pvTag. The flat path marks
               pval as !nonnull so GVN+InstSimplify can fold the per-access
               null-check away after LLVM loop-unswitches on the allflat flag. */
            if (n.body) {
                std::unordered_set<std::string> avNames;
                for (auto &scope : derefAVScopes_)
                    for (auto &[nm, _] : scope) avNames.insert(nm);

                if (!avNames.empty()) {
                    std::set<std::pair<std::string,std::string>> pairs;
                    collectRowAVPairs(*n.body, avNames, pairs);
                    auto *i64_ = Type::getInt64Ty(ctx_);
                    for (auto &[outerNm, idxNm] : pairs) {
                        if (lookupRowAV(outerNm, idxNm)) continue; /* already in outer scope */
                        Value *outerPA = lookupDerefAV(outerNm);
                        Value *idxIA   = lookupIntVar(idxNm);
                        if (!outerPA || !idxIA) continue;
                        Value *outerArr = builder_.CreateLoad(perlPtrTy_, outerPA, outerNm + ".av");
                        Value *idx      = builder_.CreateLoad(i64_, idxIA, idxNm + ".i");
                        auto *i8TyRD    = Type::getInt8Ty(ctx_);
                        auto *i32TyRD   = Type::getInt32Ty(ctx_);
                        Value *outerElems = builder_.CreateLoad(perlPtrTy_, outerArr, outerNm + ".oe");
                        setTBAA(outerElems, tbaaAvElemsTag_);
                        Value *rowRefPP   = builder_.CreateGEP(perlPtrTy_, outerElems, idx, outerNm + "." + idxNm + ".rpp");
                        Value *rowRef     = builder_.CreateLoad(perlPtrTy_, rowRefPP, outerNm + "." + idxNm + ".rref");
                        setTBAA(rowRef, tbaaAvElemTag_);
                        Value *pvalPtr    = builder_.CreateConstInBoundsGEP1_64(i8TyRD, rowRef, 8, outerNm + "." + idxNm + ".pp");
                        auto *fra = builder_.CreateAlloca(perlPtrTy_, nullptr, outerNm + "." + idxNm + ".fra");
                        auto *ra  = builder_.CreateAlloca(perlPtrTy_, nullptr, outerNm + "." + idxNm + ".ra");
                        builder_.CreateStore(ConstantPointerNull::get(perlPtrTy_), fra);
                        builder_.CreateStore(ConstantPointerNull::get(perlPtrTy_), ra);
                        auto *flatBBrd  = BasicBlock::Create(ctx_, outerNm + "." + idxNm + ".flat", fn);
                        auto *normBBrd  = BasicBlock::Create(ctx_, outerNm + "." + idxNm + ".norm", fn);
                        auto *mergeBBrd = BasicBlock::Create(ctx_, outerNm + "." + idxNm + ".rmerge", fn);
                        auto afIt = avAllflatSlots_.find(outerNm);
                        if (afIt != avAllflatSlots_.end()) {
                            /* Stage 23 fast path: branch on loop-invariant allflat flag.
                               LICM hoists the load; loop-unswitch specialises the loop. */
                            Value *afVal = builder_.CreateLoad(Type::getInt1Ty(ctx_),
                                              afIt->second, outerNm + ".af.v");
                            builder_.CreateCondBr(afVal, flatBBrd, normBBrd);
                            /* flat BB: we know pval is a real double* — mark !nonnull so
                               GVN+InstSimplify can fold the per-access null-check away. */
                            builder_.SetInsertPoint(flatBBrd);
                            auto *flatLoad = static_cast<LoadInst *>(
                                builder_.CreateLoad(perlPtrTy_, pvalPtr, outerNm + "." + idxNm + ".data"));
                            flatLoad->setMetadata(LLVMContext::MD_nonnull, MDNode::get(ctx_, {}));
                            builder_.CreateStore(flatLoad, fra);
                            builder_.CreateBr(mergeBBrd);
                            /* norm BB: not all flat — check each row's pvTag individually */
                            builder_.SetInsertPoint(normBBrd);
                            Value *rowDataN = builder_.CreateLoad(perlPtrTy_, pvalPtr, outerNm + "." + idxNm + ".dataN");
                            Value *pvTagN   = builder_.CreateLoad(i32TyRD, rowRef, outerNm + "." + idxNm + ".tag");
                            static_cast<LoadInst *>(pvTagN)->setMetadata(LLVMContext::MD_tbaa, tbaaPvTagTag_);
                            Value *isFlatN  = builder_.CreateICmpEQ(pvTagN,
                                ConstantInt::get(i32TyRD, 10), outerNm + "." + idxNm + ".isflat");
                            auto *nFlatBB = BasicBlock::Create(ctx_, outerNm + "." + idxNm + ".nflat", fn);
                            auto *nNormBB = BasicBlock::Create(ctx_, outerNm + "." + idxNm + ".nnorm", fn);
                            builder_.CreateCondBr(isFlatN, nFlatBB, nNormBB);
                            builder_.SetInsertPoint(nFlatBB);
                            builder_.CreateStore(rowDataN, fra);
                            builder_.CreateBr(mergeBBrd);
                            builder_.SetInsertPoint(nNormBB);
                            builder_.CreateStore(rowDataN, ra);
                            builder_.CreateBr(mergeBBrd);
                        } else {
                            /* Original Stage 19 path: dispatch on pvTag directly. */
                            Value *rowData = builder_.CreateLoad(perlPtrTy_, pvalPtr, outerNm + "." + idxNm + ".data");
                            Value *pvTag   = builder_.CreateLoad(i32TyRD, rowRef, outerNm + "." + idxNm + ".tag");
                            static_cast<LoadInst *>(pvTag)->setMetadata(LLVMContext::MD_tbaa, tbaaPvTagTag_);
                            Value *isFlat  = builder_.CreateICmpEQ(pvTag,
                                ConstantInt::get(i32TyRD, 10), outerNm + "." + idxNm + ".isflat");
                            builder_.CreateCondBr(isFlat, flatBBrd, normBBrd);
                            builder_.SetInsertPoint(flatBBrd);
                            builder_.CreateStore(rowData, fra);
                            builder_.CreateBr(mergeBBrd);
                            builder_.SetInsertPoint(normBBrd);
                            builder_.CreateStore(rowData, ra);
                            builder_.CreateBr(mergeBBrd);
                        }
                        builder_.SetInsertPoint(mergeBBrd);
                        declareFlatRow(outerNm, idxNm, fra);
                        declareRowAV(outerNm, idxNm, ra);
                    }
                }
            }

            emitBlock(*n.body);
            popScope();
            /* Clean up allflat slots added by this loop level. */
            for (auto &nm : newAllflatNames) avAllflatSlots_.erase(nm);
            if (!builder_.GetInsertBlock()->getTerminator())
                builder_.CreateBr(stepBB);

            builder_.SetInsertPoint(stepBB);
            Value *cur2 = builder_.CreateLoad(i64, counterAlloca);
            builder_.CreateStore(builder_.CreateAdd(cur2, ConstantInt::get(i64, 1)),
                                 counterAlloca);
            {
                auto *backBr = builder_.CreateBr(condBB2);
                /* Stage 28: attach unroll+vectorize loop metadata to inner loops.
                   Two unrolled j-iterations expose two independent sqrt chains so
                   LLVM's SLP vectorizer can fuse them into sqrtpd (2×throughput). */
                if (isInnerLoop) {
                    /* Stage 28: unroll 2× + interleave 2× on inner loops.
                       Two unrolled iterations expose independent sqrt chains; interleave
                       hints the scheduler to overlap the ~20-cycle sqrt latencies. */
                    auto *unrollMD = MDNode::get(ctx_, {
                        MDString::get(ctx_, "llvm.loop.unroll.count"),
                        ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(ctx_), 2))
                    });
                    auto *interleaveMD = MDNode::get(ctx_, {
                        MDString::get(ctx_, "llvm.loop.interleave.count"),
                        ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(ctx_), 2))
                    });
                    SmallVector<Metadata*, 3> loopArgs = {nullptr, unrollMD, interleaveMD};
                    auto *loopID = MDNode::getDistinct(ctx_, loopArgs);
                    loopID->replaceOperandWith(0, loopID);
                    backBr->setMetadata("llvm.loop", loopID);
                }
            }

            loopExits_.pop_back();
            loopContinues_.pop_back();
            loopRedos_.pop_back();
            if (!n.sval.empty()) loopLabels_.pop_back();
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
        loopRedos_.push_back(bodyBB);
        if (!n.sval.empty()) loopLabels_.push_back({n.sval, exit, stepBB, bodyBB});

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
        loopRedos_.pop_back();
        if (!n.sval.empty()) loopLabels_.pop_back();
        builder_.SetInsertPoint(exit);
        callRT("perl_free", {loopPv});
        if (ownsTmpArr) callRT("perl_array_free", {tmpArr});
        break;
    }

    case NK::Last:
        if (!n.sval.empty()) {
            for (auto it = loopLabels_.rbegin(); it != loopLabels_.rend(); ++it)
                if (it->name == n.sval) { builder_.CreateBr(it->exit); break; }
        } else if (!loopExits_.empty()) {
            builder_.CreateBr(loopExits_.back());
        }
        break;

    case NK::Next:
        if (!n.sval.empty()) {
            for (auto it = loopLabels_.rbegin(); it != loopLabels_.rend(); ++it)
                if (it->name == n.sval) { builder_.CreateBr(it->cont); break; }
        } else if (!loopContinues_.empty()) {
            builder_.CreateBr(loopContinues_.back());
        }
        break;

    case NK::Return: {
        Value *v;
        if (n.left && (n.left->kind == NK::ArrayLit || n.left->kind == NK::ArrayVar ||
                       n.left->kind == NK::MapFunc  || n.left->kind == NK::GrepFunc ||
                       n.left->kind == NK::SortFunc || n.left->kind == NK::DerefArray ||
                       n.left->kind == NK::ReverseFunc)) {
            /* return list-producing expr — wrap for list/scalar context at
               runtime.  Dispatch by producer kind so scalar-context semantics
               match Perl: grep → count, sort → undef, map → last element,
               other lists (anon-array/array-var/deref/reverse) → last element. */
            Value *av = emitArrayPtr(*n.left);
            if (!av) av = callRT("perl_array_new", {});
            const char *helper = "perl_array_to_list_return";
            NK lk = n.left->kind;
            if      (lk == NK::GrepFunc) helper = "perl_grep_list_return";
            else if (lk == NK::MapFunc)  helper = "perl_map_list_return";
            else if (lk == NK::SortFunc) helper = "perl_sort_list_return";
            v = callRT(helper, {av});
        } else {
            v = n.left ? emitExpr(*n.left) : perlUndef();
        }
        /* restore any local()s before returning; clone retval first so
           restore doesn't clobber the in-place PerlValue we're returning.
           Skip sub-cleanup when inside an eval — the eval's endBB handles
           its own cleanup, and we're just exiting the eval block (not a
           real sub return). */
        if (localDepthAlloca_ && inEval_ == 0) {
            auto *i32Ty = Type::getInt32Ty(ctx_);
            Value *depth = builder_.CreateLoad(i32Ty, localDepthAlloca_);
            Value *cloned = callRT("perl_clone", {v});
            freeIfOwned(v);
            if (currentSubNeedsWantarray_) callRT("perl_pop_wantarray", {});
            callRT("perl_local_restore_to", {depth});
            v = cloned;
            emitScopeCleanup();  /* free tracked my-var pvs in all active scopes */
        }
        /* `return` inside an eval { BLOCK } stores v in the eval's bodyRes
           slot and branches to the eval's endBB.  This matches Perl
           semantics: `return EXPR` inside eval sets the eval's value
           (and `die` jumps to the closest eval via longjmp). */
        if (inEval_ > 0 && evalBodyRes_ && evalEndBB_) {
            Value *cloned = callRT("perl_clone", {v});
            freeIfOwned(v);
            builder_.CreateStore(cloned, evalBodyRes_);
            builder_.CreateBr(evalEndBB_);
        }
        /* In the main function (which returns i32), we can't emit `ret ptr`.
           Inside an eval block, `return` is equivalent to `die` in Perl
           semantics (no enclosing sub to return from). Call perl_die to
           longjmp back to the eval's catch point. */
        else if (currentFn_ && currentFn_->getReturnType()->isIntegerTy() &&
            currentFn_->getName() == "main") {
            callRT("perl_die", {v});
        } else {
            builder_.CreateRet(v);
        }
        break;
    }

    case NK::LocalStmt: {
        /* save current value, optionally assign new one */
        Value *pv;
        if (n.name == "/")   pv = callRT("perl_get_input_sep",    {});
        else if (n.name == "!") pv = callRT("perl_get_dollar_bang",{});
        else if (n.name == ".") pv = callRT("perl_get_dollar_dot",  {});
        else if (n.name == ",") pv = callRT("perl_get_dollar_comma",{});
        else if (n.name == "\\") pv = callRT("perl_get_dollar_bsl", {});
        else if (n.name == "&") pv = callRT("perl_get_dollar_amp",  {});
        else {
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
        if (n.left && n.left->kind == NK::HashElem) {
            Value *hv = lookupHash(n.left->name);
            if (!hv) break;
            if (Value *kp = constKeyPtr(*n.left->left, builder_))
                av = callRT("perl_hash_autoviv_array", {hv, kp});
            else {
                Value *key = emitExpr(*n.left->left);
                av = callRT("perl_hash_autoviv_array_sv", {hv, key});  /* uses _sv fallback */
                freeIfOwned(key);
            }
        } else if (n.left && n.left->kind == NK::ArrayElem) {
            Value *outerAv = lookupArray(n.left->name);
            if (!outerAv) break;
            av = callRT("perl_array_autoviv_array", {outerAv, emitIdx(*n.left->left)});
        } else if (n.left) {
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
        if (n.left && n.left->kind == NK::HashElem) {
            Value *hv = lookupHash(n.left->name);
            if (!hv) break;
            if (Value *kp = constKeyPtr(*n.left->left, builder_))
                av = callRT("perl_hash_autoviv_array", {hv, kp});
            else {
                Value *key = emitExpr(*n.left->left);
                av = callRT("perl_hash_autoviv_array_sv", {hv, key});
                freeIfOwned(key);
            }
        } else if (n.left && n.left->kind == NK::ArrayElem) {
            Value *outerAv = lookupArray(n.left->name);
            if (!outerAv) break;
            av = callRT("perl_array_autoviv_array", {outerAv, emitIdx(*n.left->left)});
        } else if (n.left) {
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

    case NK::PackageStmt:
        currentPackage_ = n.sval;
        break;

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
    case NK::Block:     return emitBlockLast(n);   /* do { BLOCK } in expr ctx */
    case NK::UndefLit:  return perlUndef();
    case NK::IntLit:    return perlInt(n.ival);
    case NK::FloatLit:  return perlFloat(n.fval);
    case NK::StringLit: return perlStr(n.sval);

    case NK::ScalarVar: {
        if (n.name == "!")  return callRT("perl_get_dollar_bang",  {});
        if (n.name == "/")  return callRT("perl_get_input_sep",    {});
        if (n.name == ".")  return callRT("perl_get_dollar_dot",   {});
        if (n.name == ",")  return callRT("perl_get_dollar_comma", {});
        if (n.name == "\\") return callRT("perl_get_dollar_bsl",   {});
        if (n.name == "&")  return callRT("perl_get_dollar_amp",   {});
        /* $AUTOLOAD — set by dispatch when AUTOLOAD is called */
        if (n.name == "AUTOLOAD") return callRT("perl_get_autoload_name", {});
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
        /* Phase 3: shared scalars are routed through perl_atomic_load so
           the acquire fence pairs with the writer's release fence in
           perl_atomic_store / perl_atomic_inc / perl_atomic_add.  On x86
           the fence is a compiler barrier only; on aarch64 it emits ldar.
           This subsumes the old perl_shared_load (which the Phase 1
           minimal fix used) — the release fence is now on the writer. */
        {
            std::string nm = n.name;
            if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
            if (sharedScalarNames_.count(nm)) {
                Value *pv = builder_.CreateLoad(perlPtrTy_, slot, n.name);
                return callRT("perl_atomic_load", {pv});
            }
        }
        return builder_.CreateLoad(perlPtrTy_, slot, n.name);
    }

    case NK::ArrayElem: {
        Value *av = lookupArray(n.name);
        if (!av) return perlUndef();
        return callRT("perl_array_get_ref", {av, emitIdx(*n.left)});
    }

    case NK::ArrayVar: {
        /* @arr in scalar context — return the element count as PerlValue* */
        Value *av = lookupArray(n.name);
        return av ? callRT("perl_array_len", {av}) : perlInt(0);
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

    case NK::SeekFunc: {
        Value *fh = emitExpr(*n.args[0]);
        Value *off = emitExpr(*n.args[1]);
        Value *wh  = emitExpr(*n.args[2]);
        return callRT("perl_seek_fh", {fh, off, wh});
    }

    case NK::TellFunc: {
        Value *fh = n.left ? emitExpr(*n.left) : perlUndef();
        return callRT("perl_tell_fh", {fh});
    }

    case NK::BinmodeFunc: {
        Value *fh  = n.left  ? emitExpr(*n.left)  : perlUndef();
        Value *lay = n.right ? emitExpr(*n.right) : perlUndef();
        return callRT("perl_binmode_fh", {fh, lay});
    }

    case NK::StatFunc: {
        /* scalar context: return number of elements (13 or 0) */
        Value *path = n.left ? emitExpr(*n.left) : perlUndef();
        Value *av   = callRT("perl_stat_path", {path});
        Value *len  = callRT("perl_array_len", {av});
        callRT("perl_array_free", {av});
        return len;
    }

    case NK::LstatFunc: {
        Value *path = n.left ? emitExpr(*n.left) : perlUndef();
        Value *av   = callRT("perl_lstat_path", {path});
        Value *len  = callRT("perl_array_len", {av});
        callRT("perl_array_free", {av});
        return len;
    }

    case NK::GlobFunc: {
        /* scalar context: return first match */
        Value *pat = n.left ? emitExpr(*n.left) : perlUndef();
        Value *av  = callRT("perl_glob_val", {pat});
        Value *v   = callRT("perl_array_get_ref", {av, ConstantInt::get(Type::getInt64Ty(ctx_), 0)});
        Value *res = callRT("perl_clone", {v});
        callRT("perl_array_free", {av});
        return res;
    }

    case NK::ReadFunc: {
        Value *fh  = emitExpr(*n.args[0]);
        Value *buf = emitExpr(*n.args[1]);
        Value *nb  = emitExpr(*n.args[2]);
        Value *off = n.args.size() > 3 ? emitExpr(*n.args[3]) : perlUndef();
        return callRT("perl_read_fh", {fh, buf, nb, off});
    }

    case NK::FilenofFunc: {
        Value *fh = n.left ? emitExpr(*n.left) : perlUndef();
        return callRT("perl_fileno_fh", {fh});
    }

    case NK::TruncateFunc: {
        Value *fh  = n.left  ? emitExpr(*n.left)  : perlUndef();
        Value *len = n.right ? emitExpr(*n.right) : perlUndef();
        return callRT("perl_truncate_fh", {fh, len});
    }

    case NK::EachFunc: {
        /* scalar context: return undef when exhausted, else just call it */
        Value *hv = lookupHash(n.name);
        if (!hv) return perlUndef();
        Value *av = callRT("perl_each_hash", {hv});
        Value *len = callRT("perl_array_len", {av});
        callRT("perl_array_free", {av});
        return len;
    }

    case NK::PosFunc: {
        Value *str;
        if (n.left) {
            str = emitExpr(*n.left);
        } else if (auto *slot = lookupVar("_")) {
            str = builder_.CreateLoad(perlPtrTy_, slot, "pos_default");
        } else {
            str = perlUndef();
        }
        return callRT("perl_pos_str", {str});
    }

    case NK::GetpidFunc: {
        if (n.sval == "osname") return callRT("perl_get_os_name", {});
        return callRT("perl_getpid", {});
    }

    case NK::LocalArray: {
        auto git = fileArrayGlobals_.find(n.name);
        if (git != fileArrayGlobals_.end()) {
            /* file-scope global: slot is the GlobalVariable* (PerlArray**) */
            callRT("perl_local_save_array", {git->second});
            Value *newAV = callRT("perl_array_new", {});
            if (n.left) {
                /* local @arr = LIST — extend newAV with rhs */
                Value *src = emitArrayPtr(*n.left);
                if (src) callRT("perl_array_extend", {newAV, src});
                else {
                    Value *v = emitExpr(*n.left);
                    callRT("perl_array_push", {newAV, v});
                }
            }
            builder_.CreateStore(newAV, git->second);
        }
        /* function-scope arrays have no stable slot; no-op for now */
        return perlUndef();
    }

    case NK::LocalHash: {
        auto git = fileHashGlobals_.find(n.name);
        if (git != fileHashGlobals_.end()) {
            callRT("perl_local_save_hash", {git->second});
            Value *newHV = callRT("perl_hash_new", {});
            if (n.left) {
                Value *listArr = callRT("perl_array_new", {});
                for (auto &e : n.left->args)
                    callRT("perl_array_push", {listArr, emitExpr(*e)});
                callRT("perl_hash_from_list", {newHV, listArr});
            }
            builder_.CreateStore(newHV, git->second);
        }
        return perlUndef();
    }

    case NK::RequireStmt: {
        hasStringEval_ = true;
        Value *modStr = builder_.CreateGlobalStringPtr(n.sval);
        return callRT("perl_runtime_require", {modStr});
    }

    case NK::DoFile:
        hasStringEval_ = true;
        return callRT("perl_do_file", {emitExpr(*n.left)});

    case NK::Redo: {
        if (!loopRedos_.empty()) {
            builder_.CreateBr(loopRedos_.back());
            auto *fn   = builder_.GetInsertBlock()->getParent();
            auto *dead = BasicBlock::Create(ctx_, "redo.dead", fn);
            builder_.SetInsertPoint(dead);
        }
        return perlUndef();
    }

    case NK::LockStmt: {
        if (n.sval == "array") {
            Value *av = lookupArray(n.name);
            if (av) callRT("perl_lock_array", {av});
        } else if (n.sval == "hash") {
            Value *hv = lookupHash(n.name);
            if (hv) callRT("perl_lock_hash", {hv});
        } else {
            /* scalar — n.name set if bare $var, n.left set for arbitrary expr */
            Value *pv = n.left ? emitExpr(*n.left)
                               : (lookupVar(n.name)
                                      ? builder_.CreateLoad(perlPtrTy_, lookupVar(n.name))
                                      : perlUndef());
            callRT("perl_lock_shared", {pv});
        }
        return perlUndef();
    }

    case NK::CondWait:      { callRT("perl_cond_wait",      {emitExpr(*n.left)}); return perlUndef(); }
    case NK::CondSignal:    { callRT("perl_cond_signal",    {emitExpr(*n.left)}); return perlUndef(); }
    case NK::CondBcast:     { callRT("perl_cond_broadcast", {emitExpr(*n.left)}); return perlUndef(); }

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
        if (n.sval == "~") {
            Value *operand = emitExpr(*n.left);
            Value *result  = callRT("perl_bitnot", {operand});
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
        /* For hash-element targets, use lvalue accessor so missing keys get a
           writable slot instead of mutating the shared read-only undef sentinel. */
        auto emitIncTarget = [&](bool wantLValue) -> Value * {
            if (wantLValue && n.left && n.left->kind == NK::HashElem) {
                Value *hv = lookupHash(n.left->name);
                if (hv) return emitHashLValueRef(hv, *n.left->left);
            }
            return n.left ? emitExpr(*n.left) : perlUndef();
        };
        if (n.sval == "pre++") {
            Value *v = emitIncTarget(true);
            /* Phase 3: shared scalars use the atomic inc, which takes the
               lazy-installed SharedMutex and then release-fences.  Plain
               (non-shared) scalars still go through perl_inc. */
            if (n.left && n.left->kind == NK::ScalarVar) {
                std::string nm = n.left->name;
                if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
                if (sharedScalarNames_.count(nm)) {
                    callRT("perl_atomic_inc", {v});
                    return v;
                }
            }
            callRT("perl_inc", {v});
            return v;
        }
        if (n.sval == "pre--") {
            Value *v = emitIncTarget(true);
            if (n.left && n.left->kind == NK::ScalarVar) {
                std::string nm = n.left->name;
                if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
                if (sharedScalarNames_.count(nm)) {
                    callRT("perl_atomic_dec", {v});
                    return v;
                }
            }
            callRT("perl_dec", {v});
            return v;
        }
        if (n.sval == "post++") {
            Value *orig = emitIncTarget(true);
            /* Phase 3: shared scalars — clone-then-atomic-inc so post++ can
               return the old value (Perl semantics) without a race window. */
            if (n.left && n.left->kind == NK::ScalarVar) {
                std::string nm = n.left->name;
                if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
                if (sharedScalarNames_.count(nm)) {
                    Value *copy = callRT("perl_clone", {orig});
                    callRT("perl_atomic_inc", {orig});
                    return copy;
                }
            }
            Value *copy = callRT("perl_clone", {orig});
            callRT("perl_inc", {orig});
            return copy;
        }
        if (n.sval == "post--") {
            Value *orig = emitIncTarget(true);
            if (n.left && n.left->kind == NK::ScalarVar) {
                std::string nm = n.left->name;
                if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
                if (sharedScalarNames_.count(nm)) {
                    Value *copy = callRT("perl_clone", {orig});
                    callRT("perl_atomic_dec", {orig});
                    return copy;
                }
            }
            Value *copy = callRT("perl_clone", {orig});
            callRT("perl_dec", {orig});
            return copy;
        }
        return perlUndef();
    }

    case NK::Assign: {
        /* substr($str, off, len) = val — 4-arg substr as lvalue */
        if (n.left->kind == NK::SubstrFunc && n.left->args.size() >= 3) {
            Value *str = emitExpr(*n.left->args[0]);
            Value *off = emitExpr(*n.left->args[1]);
            Value *len = emitExpr(*n.left->args[2]);
            Value *val = emitExpr(*n.right);
            callRT("perl_substr_replace", {str, off, len, val});
            freeIfOwned(val);
            return str;
        }
        /* pos($str) = N — set regex match position */
        if (n.left->kind == NK::PosFunc) {
            Value *str = n.left->left ? emitExpr(*n.left->left)
                       : (lookupVar("_") ? builder_.CreateLoad(perlPtrTy_, lookupVar("_")) : perlUndef());
            Value *pos = emitExpr(*n.right);
            callRT("perl_set_pos_str", {str, pos});
            return pos;
        }
        /* ($a,$b,...) = list */
        if (n.left->kind == NK::ArrayLit) {
            int savedCallCtx = callCtx_;
            callCtx_ = 1;
            Value *rhsArr = emitArrayPtr(*n.right);
            if (!rhsArr) rhsArr = emitExpr(*n.right);
            callCtx_ = savedCallCtx;
            bool fromUnderbar = (n.right->kind == NK::ArrayVar && n.right->name == "_");
            for (size_t i = 0; i < n.left->args.size(); i++) {
                /* Stage 25: pre-promoted @_ arg — check BEFORE emitLValue to prevent
                   auto-vivification of a PV for a variable that has only an unboxed alloca. */
                if (fromUnderbar && n.left->args[i]->kind == NK::ScalarVar) {
                    const std::string &nm = n.left->args[i]->name;
                    auto ppIt = prePromotedArgs_.find(nm);
                    if (ppIt != prePromotedArgs_.end()) {
                        Value *idx2 = ConstantInt::get(Type::getInt64Ty(ctx_), (long long)i);
                        Value *elem2 = callRT("perl_array_get_ref", {rhsArr, idx2});
                        if (ppIt->second == PPKind::Float)
                            builder_.CreateStore(callRT("perl_to_float", {elem2}), lookupFloatVar(nm));
                        else if (ppIt->second == PPKind::Int)
                            builder_.CreateStore(callRT("perl_to_int", {elem2}), lookupIntVar(nm));
                        else { /* PPKind::DerefAV: Stage 27c — borrow elem into PV slot, cache PerlArray*.
                                   No alloc_undef, no perl_assign, no perl_free at exit.
                                   emitExpr(ScalarVar) finds the borrowed elem via scopes_. */
                            Value *pvSlot = lookupVar(nm);
                            if (pvSlot) builder_.CreateStore(elem2, pvSlot);
                            Value *av = callRT("perl_deref_array", {elem2});
                            auto *pa = builder_.CreateAlloca(perlPtrTy_, nullptr, nm + ".av");
                            builder_.CreateStore(av, pa);
                            declareDerefAV(nm, pa);
                        }
                        /* elem2 is borrowed (array_get_ref not in owned set) — no free needed */
                        continue; /* skip emitLValue + assign path */
                    }
                }
                auto *slot = emitLValue(*n.left->args[i]);
                if (!slot) continue;
                Value *pv  = builder_.CreateLoad(perlPtrTy_, slot);
                Value *idx = ConstantInt::get(Type::getInt64Ty(ctx_), (long long)i);
                Value *elem = callRT("perl_array_get_ref", {rhsArr, idx});
                callRT("perl_assign", {pv, elem});
                /* promote @_ scalar args to float/int allocas when only used numerically */
                if (fromUnderbar && currentSubBody_) {
                    auto *varNode = n.left->args[i].get();
                    if (varNode->kind == NK::ScalarVar) {
                        const std::string &nm = varNode->name;
                        if (!lookupFloatVar(nm) && !lookupIntVar(nm) && !lookupDerefAV(nm)) {
                            bool safe    = floatSafe(*currentSubBody_, nm, false);
                            bool needFP  = needsFloatPrec(*currentSubBody_, nm);
                            if (safe && needFP) {
                                /* float promotion: var needs fractional precision */
                                auto *f64 = Type::getDoubleTy(ctx_);
                                auto *fa  = builder_.CreateAlloca(f64, nullptr, nm + ".f");
                                Value *dbl = callRT("perl_to_float", {pv});
                                builder_.CreateStore(dbl, fa);
                                declareFloatVar(nm, fa);
                            } else if (safe && !needFP && hasVar(*currentSubBody_, nm)) {
                                /* int promotion: var only used in integer contexts */
                                auto *i64 = Type::getInt64Ty(ctx_);
                                auto *ia  = builder_.CreateAlloca(i64, nullptr, nm + ".i");
                                Value *ival = callRT("perl_to_int", {pv});
                                builder_.CreateStore(ival, ia);
                                declareIntVar(nm, ia);
                            } else if (!safe && isOnlyArrayRefDeref(*currentSubBody_, nm)) {
                                /* array-ref arg: cache PerlArray* once at entry — eliminates
                                   repeated perl_deref_array_ro calls in hot loops (Stage 15) */
                                auto *pa = builder_.CreateAlloca(perlPtrTy_, nullptr, nm + ".av");
                                Value *av = callRT("perl_deref_array_ro", {pv});
                                builder_.CreateStore(av, pa);
                                declareDerefAV(nm, pa);
                            }
                        }
                    }
                }
                freeIfOwned(elem);
            }
            /* Stage 27b: list assignment is void — return non-owned null so
               freeIfOwned (in ExprStmt) does nothing; emitBlockLast handles
               the null-result case when this is the last expression. */
            return llvm::ConstantPointerNull::get(perlPtrTy_);
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
        /* @arr = RHS — also fixes @arr = () clearing */
        if (n.left->kind == NK::ArrayVar) {
            Value *av_lhs = lookupArray(n.left->name);
            if (av_lhs) {
                int savedCallCtx = callCtx_;
                callCtx_ = 1;
                Value *av_rhs = emitArrayPtr(*n.right);
                callCtx_ = savedCallCtx;
                if (av_rhs) {
                    callRT("perl_array_replace", {av_lhs, av_rhs});
                } else if (n.right->kind == NK::ArrayLit && n.right->args.empty()) {
                    callRT("perl_array_clear", {av_lhs});
            } else {
                      callRT("perl_array_clear", {av_lhs});
                      Value *tmp = callRT("perl_array_new", {});
                      if (n.right->kind == NK::ArrayLit) {
                          for (auto &e : n.right->args)
                              callRT("perl_array_push", {tmp, emitExpr(*e)});
                      } else {
                          int savedCallCtx2 = callCtx_;
                          callCtx_ = 1;
                          Value *rhsVal = emitExpr(*n.right);
                          callCtx_ = savedCallCtx2;
                          callRT("perl_array_push_list_or_scalar", {tmp, rhsVal});
                      }
                      callRT("perl_array_replace", {av_lhs, tmp});
                  }
            }
            return perlUndef();
        }
        /* %h = (list) */
        if (n.left->kind == NK::HashVar) {
            Value *hv = lookupHash(n.left->name);
            if (!hv) return perlUndef();
            callRT("perl_hash_clear", {hv});
            Value *listArr = callRT("perl_array_new", {});
            if (n.right->kind == NK::ArrayLit) {
                for (auto &elem : n.right->args)
                    callRT("perl_array_push", {listArr, emitExpr(*elem)});
            } else {
                Value *src = emitArrayPtr(*n.right);
                if (src) callRT("perl_array_extend", {listArr, src});
                else     callRT("perl_array_push", {listArr, emitExpr(*n.right)});
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
        /* $ref->[i] = val  or  $ref->{k} = val  (with autovivification) */
        if (n.left->kind == NK::ArrowDeref) {
            if (n.left->sval == "array") {
                Value *idx = emitIdx(*n.left->right);
                Value *rhs = emitExpr(*n.right);
                /* autovivify $h{k}[i] = val */
                if (n.left->left->kind == NK::HashElem) {
                    Value *outerHv = lookupHash(n.left->left->name);
                    if (!outerHv) return perlUndef();
                    Value *innerAv;
                    if (Value *kp = constKeyPtr(*n.left->left->left, builder_))
                        innerAv = callRT("perl_hash_autoviv_array", {outerHv, kp});
                    else {
                        Value *key = emitExpr(*n.left->left->left);
                        innerAv = callRT("perl_hash_autoviv_array_sv", {outerHv, key});
                        freeIfOwned(key);
                    }
                    callRT("perl_array_set", {innerAv, idx, rhs});
                    return rhs;
                }
                /* autovivify $a[i][j] = val */
                if (n.left->left->kind == NK::ArrayElem) {
                    Value *outerAv = lookupArray(n.left->left->name);
                    if (!outerAv) return perlUndef();
                    Value *outerIdx = emitIdx(*n.left->left->left);
                    Value *innerAv  = callRT("perl_array_autoviv_array", {outerAv, outerIdx});
                    callRT("perl_array_set", {innerAv, idx, rhs});
                    return rhs;
                }
                /* regular $ref->[i] = val */
                Value *base = emitExpr(*n.left->left);
                /* Check DerefAV cache for base variable */
                std::string baseNm;
                Value *cachedAv = nullptr;
                if (n.left->left->kind == NK::ScalarVar) {
                    baseNm = n.left->left->name;
                    if (!baseNm.empty() && baseNm[0] == '$') baseNm = baseNm.substr(1);
                    if (Value *pa = lookupDerefAV(baseNm))
                        cachedAv = builder_.CreateLoad(arrayPtrTy_, pa, baseNm + ".av");
                }
                /* Stage 22: always dispatch flat/norm to avoid perl_deref_array
                   lazy-converting FLAT_ARRAY PVs (keeps all bodies flat throughout). */
                auto *i8T32  = Type::getInt8Ty(ctx_);
                auto *i32T32 = Type::getInt32Ty(ctx_);
                auto *f64T32 = Type::getDoubleTy(ctx_);
                Value *tag32    = builder_.CreateLoad(i32T32, base, "tag32");
                setTBAA(tag32, tbaaPvTagTag_);
                Value *isFlat32 = builder_.CreateICmpEQ(tag32,
                                     ConstantInt::get(i32T32, 10), "isflat32");
                auto *curFn32   = builder_.GetInsertBlock()->getParent();
                auto *fBB32     = BasicBlock::Create(ctx_, "w22.f", curFn32);
                auto *nBB32     = BasicBlock::Create(ctx_, "w22.n", curFn32);
                auto *mBB32     = BasicBlock::Create(ctx_, "w22.m", curFn32);
                builder_.CreateCondBr(isFlat32, fBB32, nBB32);
                /* flat: extract double from rhs, store directly into double[] */
                builder_.SetInsertPoint(fBB32);
                Value *rhsF32    = callRT("perl_to_float", {rhs});
                Value *pvalOff32 = builder_.CreateConstInBoundsGEP1_64(
                                       i8T32, base, 8, "pvaloff32");
                Value *dblPtr32  = builder_.CreateLoad(perlPtrTy_, pvalOff32, "dblp32");
                Value *ep32      = builder_.CreateGEP(f64T32, dblPtr32, idx, "ep32");
                auto *fst32 = builder_.CreateStore(rhsF32, ep32);
                if (tbaaFlatDoubleTag_)
                    fst32->setMetadata(LLVMContext::MD_tbaa, tbaaFlatDoubleTag_);
                freeIfOwned(base);
                builder_.CreateBr(mBB32);
                /* norm: use perl_deref_array + perl_array_set (or cached Av) */
                builder_.SetInsertPoint(nBB32);
                Value *av32;
                if (cachedAv) {
                    av32 = cachedAv;
                } else {
                    av32 = callRT("perl_deref_array", {base});
                    freeIfOwned(base);
                }
                callRT("perl_array_set", {av32, idx, rhs});
                builder_.CreateBr(mBB32);
                builder_.SetInsertPoint(mBB32);
                return rhs;
            } else {
                /* hash case — autovivify $h{k}{subk} = val or $a[i]{subk} = val */
                Value *rhs = emitExpr(*n.right);
                Value *hv;
                if (n.left->left->kind == NK::HashElem) {
                    Value *outerHv = lookupHash(n.left->left->name);
                    if (!outerHv) return perlUndef();
                    if (Value *kp = constKeyPtr(*n.left->left->left, builder_))
                        hv = callRT("perl_hash_autoviv_hash", {outerHv, kp});
                    else {
                        Value *key = emitExpr(*n.left->left->left);
                        hv = callRT("perl_hash_autoviv_hash_sv", {outerHv, key});
                        freeIfOwned(key);
                    }
                } else if (n.left->left->kind == NK::ArrayElem) {
                    Value *av = lookupArray(n.left->left->name);
                    if (!av) return perlUndef();
                    Value *outerIdx = emitIdx(*n.left->left->left);
                    hv = callRT("perl_array_autoviv_hash", {av, outerIdx});
                } else {
                    Value *base = emitExpr(*n.left->left);
                    hv = callRT("perl_deref_hash", {base});
                    freeIfOwned(base);
                }
                emitHashSet(hv, *n.left->right, rhs);
                return rhs;
            }
        }
        /* $arr[i] = val */
        if (n.left->kind == NK::ArrayElem) {
            std::string arrNm = n.left->name;
            if (!arrNm.empty() && arrNm[0] == '@') arrNm = arrNm.substr(1);
            /* Stage 33: track array element type from RHS */
            if (n.right->kind == NK::AnonArray) {
                if (n.right->args.size() == 2 &&
                    canEmitF64(*n.right->args[0]) && canEmitF64(*n.right->args[1])) {
                    /* [float, float] → element is FLOAT_PAIR (tag=13) */
                    setArrayElemType(arrNm, 13);
                } else if (n.right->args.size() >= 2) {
                    bool allF64 = true;
                    for (auto &e : n.right->args) {
                        if (!canEmitF64(*e)) { allF64 = false; break; }
                    }
                    if (allF64) {
                        /* [float, float, ...] → element is FLAT_ARRAY (tag=10) */
                        setArrayElemType(arrNm, 10);
                    }
                }
            }
            Value *av = lookupArray(n.left->name);
            if (!av) return perlUndef();
            Value *rhs = emitExpr(*n.right);
            callRT("perl_array_set", {av, emitIdx(*n.left->left), rhs});
            return rhs;
        }
        /* @arr[i,j,...] = list  (lvalue array slice) */
        if (n.left->kind == NK::ArraySlice) {
            Value *av = lookupArray(n.left->name);
            if (!av) return perlUndef();
            Value *idxArr = callRT("perl_array_new", {});
            for (auto &idxNode : n.left->args)
                callRT("perl_array_push", {idxArr, emitExpr(*idxNode)});
            int savedCallCtx = callCtx_;
            callCtx_ = 1;
            Value *rhsArr = emitArrayPtr(*n.right);
            callCtx_ = savedCallCtx;
            if (!rhsArr) {
                rhsArr = callRT("perl_array_new", {});
                if (n.right->kind == NK::ArrayLit)
                    for (auto &e : n.right->args)
                        callRT("perl_array_push", {rhsArr, emitExpr(*e)});
                else
                    callRT("perl_array_push", {rhsArr, emitExpr(*n.right)});
            }
            callRT("perl_array_assign_slice", {av, idxArr, rhsArr});
            return perlUndef();
        }
        /* @h{LIST} = list  (lvalue hash slice) */
        if (n.left->kind == NK::HashSlice) {
            Value *hv = lookupHash(n.left->name);
            if (!hv) return perlUndef();
            Value *keyArr = callRT("perl_array_new", {});
            for (auto &keyNode : n.left->args) {
                if (keyNode->kind == NK::ArrayLit)
                    for (auto &k : keyNode->args)
                        callRT("perl_array_push", {keyArr, emitExpr(*k)});
                else if (Value *kav = emitArrayPtr(*keyNode))
                    callRT("perl_array_extend", {keyArr, kav});
                else
                    callRT("perl_array_push", {keyArr, emitExpr(*keyNode)});
            }
            int savedCallCtx = callCtx_;
            callCtx_ = 1;
            Value *rhsArr = emitArrayPtr(*n.right);
            callCtx_ = savedCallCtx;
            if (!rhsArr) {
                rhsArr = callRT("perl_array_new", {});
                if (n.right->kind == NK::ArrayLit)
                    for (auto &e : n.right->args)
                        callRT("perl_array_push", {rhsArr, emitExpr(*e)});
                else
                    callRT("perl_array_push", {rhsArr, emitExpr(*n.right)});
            }
            callRT("perl_hash_assign_slice", {hv, keyArr, rhsArr});
            return perlUndef();
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
            /* Stage 33: track known tag type from AnonArray RHS */
            if (n.right->kind == NK::AnonArray) {
                if (n.right->args.size() == 2 &&
                    canEmitF64(*n.right->args[0]) && canEmitF64(*n.right->args[1])) {
                    /* [float, float] → FLOAT_PAIR (tag=13) */
                    setKnownTagType(nm, 13);
                } else if (n.right->args.size() >= 2) {
                    bool allF64 = true;
                    for (auto &e : n.right->args) {
                        if (!canEmitF64(*e)) { allF64 = false; break; }
                    }
                    if (allF64) {
                        /* [float, float, ...] → FLAT_ARRAY (tag=10) */
                        setKnownTagType(nm, 10);
                    }
                }
            }
            /* Stage 33: track known tag type from ArrowDeref RHS (array element load) */
            if (n.right->kind == NK::ArrowDeref && n.right->sval == "array") {
                /* $var = $arr->[idx] — check if $arr has known element type */
                if (n.right->left->kind == NK::ScalarVar) {
                    std::string arrNm = n.right->left->name;
                    if (!arrNm.empty() && arrNm[0] == '$') arrNm = arrNm.substr(1);
                    int elemTag = lookupArrayElemType(arrNm);
                    if (elemTag) {
                        /* Array has known element type — propagate to $var */
                        setKnownTagType(nm, elemTag);
                    }
                }
            }
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
        /* Phase 3: shared scalars with RMW-shaped RHS — pattern-match
           `$shared = $shared OP N` and translate to perl_atomic_add so
           the read-modify-write is atomic under the SharedMutex.  This
           is the same operation as `+=` etc. (which goes through
           CompoundAssign) but written out longhand; in the wild both
           forms are common (e.g. `$x = $x + 1` and `$x++`). */
        if (n.left->kind == NK::ScalarVar) {
            std::string nm = n.left->name;
            if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
            if (sharedScalarNames_.count(nm) && n.right->kind == NK::BinOp) {
                 bool isNumeric = (n.right->sval == "+" || n.right->sval == "-" ||
                                   n.right->sval == "*" || n.right->sval == "/" ||
                                   n.right->sval == "%");
                /* pattern: lhs is one of the BinOp's operands */
                bool lhsOnLeft  = n.right->left  && n.right->left->kind == NK::ScalarVar
                                && n.right->left->name  == n.left->name;
                bool lhsOnRight = n.right->right && n.right->right->kind == NK::ScalarVar
                                && n.right->right->name == n.left->name;
                if (isNumeric && (lhsOnLeft || lhsOnRight)) {
                    Value *lhs = emitLValue(*n.left);
                    if (lhs) {
                        Value *lhsVal = builder_.CreateLoad(perlPtrTy_, lhs);
                        /* evaluate the *other* operand only; the shared
                           var is read inside perl_atomic_add under the
                           mutex, so re-evaluating it here would race. */
                        const Node &other = lhsOnLeft ? *n.right->right : *n.right->left;
                         Value *rhsVal = emitExpr(other);
                         /* For subtraction, negate delta so perl_atomic_add
                            performs subtraction.  For *, /, % there is no
                            atomic RMW primitive — skip the atomic path and
                            fall through to the non-atomic perl_assign below. */
                         if (n.right->sval == "-") {
                             Value *floatVal = callRT("perl_to_float", {rhsVal});
                             Value *negFloat = builder_.CreateFNeg(floatVal);
                             freeIfOwned(floatVal);
                             Value *negBoxed = boxF64(negFloat);
                             Value *r = callRT("perl_atomic_add", {lhsVal, negBoxed});
                             freeIfOwned(negBoxed);
                             freeIfOwned(rhsVal);
                             return r;
                         } else if (n.right->sval == "+") {
                             Value *r = callRT("perl_atomic_add", {lhsVal, rhsVal});
                             freeIfOwned(rhsVal);
                             return r;
                         }
                         /* *, /, %: skip atomic path, fall through */
                         freeIfOwned(rhsVal);
                    }
                }
            }
        }
        Value *rhs = emitExpr(*n.right);
        /* DerefAV cache for local vars: when $local = $cached->[idx], cache the
           PerlArray* so inner-loop $local->[i] skips perl_deref_array_ro. */
        if (n.left->kind == NK::ScalarVar && n.right->kind == NK::ArrowDeref) {
            std::string nm = n.left->name;
            if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
            if (!lookupDerefAV(nm)) {
                std::string baseNm = n.right->left->name;
                if (!baseNm.empty() && baseNm[0] == '$') baseNm = baseNm.substr(1);
                if (Value *outerPA = lookupDerefAV(baseNm)) {
                    Value *base = emitExpr(*n.right->left);
                    auto *i8T = Type::getInt8Ty(ctx_);
                    auto *i32T = Type::getInt32Ty(ctx_);
                    auto *i64T = Type::getInt64Ty(ctx_);
                    Value *tag = builder_.CreateLoad(i32T, base, "tag");
                    Value *isFlat = builder_.CreateICmpEQ(tag,
                        ConstantInt::get(i32T, 10), "isflat");
                    auto *curFn = builder_.GetInsertBlock()->getParent();
                    auto *fBB = BasicBlock::Create(ctx_, "lva.f", curFn);
                    auto *nBB = BasicBlock::Create(ctx_, "lva.n", curFn);
                    auto *mBB = BasicBlock::Create(ctx_, "lva.m", curFn);
                    builder_.CreateCondBr(isFlat, fBB, nBB);
                    builder_.SetInsertPoint(fBB);
                    Value *pvalPtr = builder_.CreateConstInBoundsGEP1_64(i8T, base, 8, "lva.pv");
                    Value *dblPtr = builder_.CreateLoad(perlPtrTy_, pvalPtr, "lva.dp");
                    builder_.CreateBr(mBB);
                    builder_.SetInsertPoint(nBB);
                    Value *av = callRT("perl_deref_array_ro", {base});
                    builder_.CreateBr(mBB);
                    builder_.SetInsertPoint(mBB);
                    auto *phiAv = builder_.CreatePHI(perlPtrTy_, 2, "lva.av");
                    phiAv->addIncoming(dblPtr, fBB);
                    phiAv->addIncoming(av, nBB);
                    auto *pa = builder_.CreateAlloca(perlPtrTy_, nullptr, nm + ".av");
                    builder_.CreateStore(phiAv, pa);
                    declareDerefAV(nm, pa);
                    freeIfOwned(base);
                }
            }
        }
        Value *lhs = emitLValue(*n.left);
        if (lhs) {
            /* perl_assign model: mutate the stable PerlValue* in-place */
            Value *lhsVal = builder_.CreateLoad(perlPtrTy_, lhs);
            /* Phase 3: shared scalars route through perl_atomic_store so
               the write is release-fenced (pairs with perl_atomic_load's
               acquire on the reader side).  Same payload-update logic
               as perl_assign (refcount, string deep-copy, tag dispatch);
               see the implementation in runtime.c. */
            if (n.left->kind == NK::ScalarVar) {
                std::string nm = n.left->name;
                if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
                if (sharedScalarNames_.count(nm)) {
                    callRT("perl_atomic_store", {lhsVal, rhs});
                    return rhs;
                }
            }
            callRT("perl_assign", {lhsVal, rhs});
        }
        return rhs;
        }
    }

    case NK::CompoundAssign: {
        /* short-circuit compound assignments: ||= &&= //= */
        if (n.sval == "||" || n.sval == "&&" || n.sval == "//") {
            auto *fn    = builder_.GetInsertBlock()->getParent();
            auto *rhsBB = BasicBlock::Create(ctx_, "sca.rhs", fn);
            auto *endBB = BasicBlock::Create(ctx_, "sca.end", fn);
            Value *lhsPtr = emitLValue(*n.left);
            if (!lhsPtr) return perlUndef();
            Value *lhsVal = builder_.CreateLoad(perlPtrTy_, lhsPtr);
            Value *test;
            if (n.sval == "//")
                test = builder_.CreateICmpNE(callRT("perl_defined", {lhsVal}),
                                             ConstantInt::get(Type::getInt32Ty(ctx_), 0));
            else {
                Value *b = callRT("perl_is_true", {lhsVal});
                test = builder_.CreateICmpNE(b, ConstantInt::get(Type::getInt32Ty(ctx_), 0));
                if (n.sval == "&&") test = builder_.CreateNot(test); /* assign when false */
            }
            builder_.CreateCondBr(test, endBB, rhsBB);
            builder_.SetInsertPoint(rhsBB);
            Value *rhsVal = emitExpr(*n.right);
            callRT("perl_assign", {lhsVal, rhsVal});
            freeIfOwned(rhsVal);
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            return lhsVal;
        }
        auto applyOp = [&](Value *lv, Value *rv) -> Value * {
            if      (n.sval == "+")  return callRT("perl_add",        {lv, rv});
            else if (n.sval == "-")  return callRT("perl_sub",        {lv, rv});
            else if (n.sval == "*")  return callRT("perl_mul",        {lv, rv});
            else if (n.sval == "/")  return callRT("perl_div",        {lv, rv});
            else if (n.sval == ".")  return callRT("perl_concat",     {lv, rv});
            else if (n.sval == "%")  return callRT("perl_mod",        {lv, rv});
            else if (n.sval == "**") return callRT("perl_pow",        {lv, rv});
            else if (n.sval == "x")  return callRT("perl_repeat_str", {lv, rv});
            else if (n.sval == "&")  return callRT("perl_bitand",     {lv, rv});
            else if (n.sval == "|")  return callRT("perl_bitor",      {lv, rv});
            else if (n.sval == "^")  return callRT("perl_bitxor",     {lv, rv});
            else if (n.sval == "<<") return callRT("perl_lshift",     {lv, rv});
            else if (n.sval == ">>") return callRT("perl_rshift",     {lv, rv});
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
            Value *lhsVal = emitHashLValueRef(hv, *n.left->left); /* writable slot */
            Value *rhsVal = emitExpr(*n.right);
            Value *result = applyOp(lhsVal, rhsVal);
            freeIfOwned(lhsVal);
            freeIfOwned(rhsVal);
            emitHashSet(hv, *n.left->left, result);
            return result;
        }
        /* $ref->[$i] op= rhs  or  $ref->{k} op= rhs */
        if (n.left->kind == NK::ArrowDeref) {
            Value *lhsVal, *rhsVal, *result;
            if (n.left->sval == "array") {
                /* 2D pattern $arr->[$i][$k] op= rhs: all-readonly deref chain */
                Value *av;
                if (n.left->left->kind == NK::ArrowDeref && n.left->left->sval == "array") {
                    /* Outer deref: use cached PerlArray* if available (Stage 15) */
                    Value *outerArr;
                    if (n.left->left->left->kind == NK::ScalarVar) {
                        if (Value *pa = lookupDerefAV(n.left->left->left->name)) {
                            outerArr = builder_.CreateLoad(perlPtrTy_, pa,
                                                           n.left->left->left->name + ".av");
                        } else {
                            Value *base = emitExpr(*n.left->left->left);
                            outerArr = callRT("perl_deref_array_ro", {base});
                            freeIfOwned(base);
                        }
                    } else {
                        Value *outerBase = emitExpr(*n.left->left->left);
                        outerArr = callRT("perl_deref_array_ro", {outerBase});
                        freeIfOwned(outerBase);
                    }
                    /* Inner deref: use flat/row cache if first index is a named var */
                    if (n.left->left->left->kind == NK::ScalarVar &&
                        n.left->left->right->kind == NK::ScalarVar) {
                        std::string idxNm = n.left->left->right->name;
                        if (!idxNm.empty() && idxNm[0] == '$') idxNm = idxNm.substr(1);
                        const std::string &outerNm18 = n.left->left->left->name;

                        /* Stage 22: flat row fast path — RHS emitted ONCE before condBr
                           so LLVM sees a single expression, enabling CSE/hoisting. */
                        if (canEmitF64(*n.right)) {
                            if (Value *fra18 = lookupFlatRow(outerNm18, idxNm)) {
                                auto applyF64 = [&](Value *lv, Value *rv) -> Value * {
                                    if (n.sval == "+") return builder_.CreateFAdd(lv, rv);
                                    if (n.sval == "-") return builder_.CreateFSub(lv, rv);
                                    if (n.sval == "*") return builder_.CreateFMul(lv, rv);
                                    if (n.sval == "/") return builder_.CreateFDiv(lv, rv);
                                    return nullptr;
                                };
                                auto *f64Ty18 = Type::getDoubleTy(ctx_);
                                Value *idx18  = emitIdx(*n.left->right);
                                auto *flatLoad18 = builder_.CreateLoad(perlPtrTy_, fra18, "flat.ptr");
                                /* Stage 29: mark non-null when outer array is all-flat so LLVM
                                   folds the null-check and removes the dead norm write path. */
                                if (avAllflatSlots_.count(outerNm18))
                                    flatLoad18->setMetadata(LLVMContext::MD_nonnull, MDNode::get(ctx_, {}));
                                Value *flatPtr = flatLoad18;
                                Value *isFlat18 = builder_.CreateICmpNE(flatPtr,
                                    ConstantPointerNull::get(perlPtrTy_), "s18.if");
                                /* Emit RHS once here — before the branch — so both BBs reuse it.
                                   emitExprF64 may itself emit sub-branches (e.g. for reads of other
                                   flat rows); isFlat18/flatPtr/idx18 remain available via SSA. */
                                Value *rhsF = emitExprF64(*n.right);
                                if (rhsF) {
                                    auto *curFn18 = builder_.GetInsertBlock()->getParent();
                                    auto *fBB18   = BasicBlock::Create(ctx_, "s18.f", curFn18);
                                    auto *nBB18   = BasicBlock::Create(ctx_, "s18.n", curFn18);
                                    auto *mBB18   = BasicBlock::Create(ctx_, "s18.m", curFn18);
                                    builder_.CreateCondBr(isFlat18, fBB18, nBB18);
                                    /* flat BB: load double, apply op, store — no PV overhead */
                                    builder_.SetInsertPoint(fBB18);
                                    Value *ep18f  = builder_.CreateGEP(f64Ty18, flatPtr, idx18, "fe");
                                    Value *lhsF   = builder_.CreateLoad(f64Ty18, ep18f, "lhsf");
                                    setTBAA(lhsF, tbaaFlatDoubleTag_);
                                    Value *newF   = applyF64(lhsF, rhsF);
                                    Value *retFlat = ConstantPointerNull::get(perlPtrTy_);
                                    if (newF) {
                                        auto *st = builder_.CreateStore(newF, ep18f);
                                        st->setMetadata(LLVMContext::MD_tbaa, tbaaFlatDoubleTag_);
                                    }
                                    builder_.CreateBr(mBB18);
                                    auto *fBB18p = builder_.GetInsertBlock();
                                    /* norm BB: PV* chain — reuse same rhsF */
                                    builder_.SetInsertPoint(nBB18);
                                    Value *retNorm = ConstantPointerNull::get(perlPtrTy_);
                                    if (Value *ra18 = lookupRowAV(outerNm18, idxNm)) {
                                        auto *i8Ty18  = Type::getInt8Ty(ctx_);
                                        auto *i32Ty18 = Type::getInt32Ty(ctx_);
                                        Value *av18   = builder_.CreateLoad(perlPtrTy_, ra18,
                                                          outerNm18 + "." + idxNm + ".ra");
                                        Value *elems18 = builder_.CreateLoad(perlPtrTy_, av18, "ae");
                                        setTBAA(elems18, tbaaAvElemsTag_);
                                        Value *pvPtr18 = builder_.CreateGEP(perlPtrTy_, elems18, idx18, "pp");
                                        Value *pv18    = builder_.CreateLoad(perlPtrTy_, pvPtr18, "pv");
                                        setTBAA(pv18, tbaaAvElemTag_);
                                        Value *fvPtr18 = builder_.CreateConstInBoundsGEP1_64(i8Ty18, pv18, 8, "fp");
                                        Value *lhsN    = builder_.CreateLoad(f64Ty18, fvPtr18, "lhsn");
                                        setTBAA(lhsN, tbaaPvFvalTag_);
                                        Value *newN    = applyF64(lhsN, rhsF);  /* reuse rhsF */
                                        if (newN) {
                                            auto *tst = builder_.CreateStore(
                                                ConstantInt::get(i32Ty18, 2), pv18);
                                            tst->setMetadata(LLVMContext::MD_tbaa, tbaaPvTagTag_);
                                            auto *fst = builder_.CreateStore(newN, fvPtr18);
                                            fst->setMetadata(LLVMContext::MD_tbaa, tbaaPvFvalTag_);
                                            retNorm = pv18;
                                        }
                                    }
                                    builder_.CreateBr(mBB18);
                                    auto *nBB18p = builder_.GetInsertBlock();
                                    builder_.SetInsertPoint(mBB18);
                                    auto *phi18 = builder_.CreatePHI(perlPtrTy_, 2, "pv");
                                    phi18->addIncoming(retFlat, fBB18p);
                                    phi18->addIncoming(retNorm, nBB18p);
                                    /* Stage 31: invalidate flat-double cache for written element */
                                    if (n.left->right->kind == NK::IntLit) {
                                        flatDoubleCache_.erase(outerNm18 + "\x01" + idxNm + "\x01" + std::to_string(n.left->right->ival));
                                    } else {
                                        std::string pfx = outerNm18 + "\x01" + idxNm + "\x01";
                                        for (auto it = flatDoubleCache_.begin(); it != flatDoubleCache_.end(); )
                                            it = (it->first.substr(0, pfx.size()) == pfx) ? flatDoubleCache_.erase(it) : std::next(it);
                                    }
                                    return phi18;
                                }
                            }
                        }

                        /* Stage 16: normal PV* row cache */
                        if (Value *ra = lookupRowAV(outerNm18, idxNm)) {
                            av = builder_.CreateLoad(perlPtrTy_, ra,
                                                     outerNm18 + "." + idxNm + ".ra");
                        } else {
                            Value *innerRef = callRT("perl_array_get_ref",
                                                     {outerArr, emitIdx(*n.left->left->right)});
                            av = callRT("perl_deref_array", {innerRef});
                        }
                    } else {
                        Value *innerRef = callRT("perl_array_get_ref",
                                                 {outerArr, emitIdx(*n.left->left->right)});
                        av = callRT("perl_deref_array", {innerRef});
                    }
                } else {
                    Value *base = emitExpr(*n.left->left);
                    av = callRT("perl_deref_array_ro", {base});
                    freeIfOwned(base);
                }
                Value *idx = emitIdx(*n.left->right);
                /* Stage 18 PV* fast path: RHS is F64 and op is arithmetic */
                if (canEmitF64(*n.right)) {
                    auto applyF64op = [&](Value *lv, Value *rv) -> Value * {
                        if (n.sval == "+") return builder_.CreateFAdd(lv, rv);
                        if (n.sval == "-") return builder_.CreateFSub(lv, rv);
                        if (n.sval == "*") return builder_.CreateFMul(lv, rv);
                        if (n.sval == "/") return builder_.CreateFDiv(lv, rv);
                        return nullptr;
                    };
                    auto *i8Ty  = Type::getInt8Ty(ctx_);
                    auto *i32Ty = Type::getInt32Ty(ctx_);
                    auto *f64Ty = Type::getDoubleTy(ctx_);
                    Value *elems = builder_.CreateLoad(perlPtrTy_, av, "av.elems");
                    setTBAA(elems, tbaaAvElemsTag_);
                    Value *pvPtr = builder_.CreateGEP(perlPtrTy_, elems, idx, "pv.ptr");
                    Value *pv    = builder_.CreateLoad(perlPtrTy_, pvPtr, "pv");
                    setTBAA(pv, tbaaAvElemTag_);
                    Value *fvPtr = builder_.CreateConstInBoundsGEP1_64(i8Ty, pv, 8, "fv.ptr");
                    Value *lhsF  = builder_.CreateLoad(f64Ty, fvPtr, "lhsf");
                    setTBAA(lhsF, tbaaPvFvalTag_);
                    Value *rhsF  = emitExprF64(*n.right);
                    if (rhsF) {
                        Value *newF = applyF64op(lhsF, rhsF);
                        if (newF) {
                            auto *tagSt = builder_.CreateStore(ConstantInt::get(i32Ty, 2), pv);
                            tagSt->setMetadata(LLVMContext::MD_tbaa, tbaaPvTagTag_);
                            auto *fvSt = builder_.CreateStore(newF, fvPtr);
                            fvSt->setMetadata(LLVMContext::MD_tbaa, tbaaPvFvalTag_);
                            return pv;
                        }
                    }
                }
                lhsVal = callRT("perl_array_get_ref", {av, idx});
                rhsVal = emitExpr(*n.right);
                result = applyOp(lhsVal, rhsVal);
                freeIfOwned(lhsVal);
                freeIfOwned(rhsVal);
                callRT("perl_array_set", {av, idx, result});
            } else {
                Value *hashBase = emitExpr(*n.left->left);
                Value *hv = callRT("perl_deref_hash", {hashBase});
                freeIfOwned(hashBase);
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

        /* Phase 3: shared scalars route through the atomic primitive.
           For numeric ops we use perl_atomic_add, which takes the
           lazy-installed SharedMutex (correct for RMW).  For other ops
           (., x, &, |, ^, <<, >>, **) we fall back to non-atomic
           applyOp + a release-fenced perl_atomic_store — the read-modify-
           write is not atomic for these, but the write side still has
           a release fence so the result is visible to other threads. */
        if (n.left->kind == NK::ScalarVar) {
            std::string nm = n.left->name;
            if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
      if (sharedScalarNames_.count(nm)) {
                   bool isNumeric = (n.sval == "+" || n.sval == "-" ||
                                     n.sval == "*" || n.sval == "/" ||
                                     n.sval == "%");
                   if (isNumeric) {
                       /* For +, -, use perl_atomic_add (lock-free CAS + mutex fallback).
                          For *, /, %, use perl_atomic_rmw which does RMW with mutex. */
                       if (n.sval == "-") {
                           Value *floatVal = callRT("perl_to_float", {rhsVal});
                           Value *negFloat = builder_.CreateFNeg(floatVal);
                           freeIfOwned(floatVal);
                           Value *negBoxed = boxF64(negFloat);
                           Value *r = callRT("perl_atomic_add", {lhsVal, negBoxed});
                           freeIfOwned(negBoxed);
                           freeIfOwned(rhsVal);
                           return lhsVal;
                       } else if (n.sval == "+") {
                           Value *r = callRT("perl_atomic_add", {lhsVal, rhsVal});
                           freeIfOwned(rhsVal);
                           return lhsVal;
                       }
                       /* *, /, %: use perl_atomic_rmw with mutex protection */
                       int opCode = (n.sval == "*") ? 1 : (n.sval == "/") ? 2 : 3;
                       Value *r = callRT("perl_atomic_rmw", {lhsVal, rhsVal, ConstantInt::get(Type::getInt32Ty(ctx_), opCode)});
                       freeIfOwned(rhsVal);
                       return r;
                   }
                 /* non-numeric: applyOp + atomic store (release-fenced) */
                 Value *result = applyOp(lhsVal, rhsVal);
                 freeIfOwned(rhsVal);
                 callRT("perl_atomic_store", {lhsVal, result});
                 freeIfOwned(result);
                 return lhsVal;
             }
         }

        Value *result = applyOp(lhsVal, rhsVal);
        freeIfOwned(rhsVal);
        callRT("perl_assign", {lhsVal, result});
        freeIfOwned(result);
        return lhsVal;
    }

    case NK::Call: return emitCall(n);

    case NK::ScalarFunc: {
        if (n.left) {
            /* scalar @{expr} or scalar @$ref — deref then take length */
            Value *av = emitArrayPtr(*n.left);
            if (!av) {
                Value *ref = emitExpr(*n.left);
                av = callRT("perl_deref_array", {ref});
                freeIfOwned(ref);
            }
            return callRT("perl_array_len", {av});
        }
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
        Value *removed = callRT("perl_chomp", {v});
        return callRT("perl_alloc_int", {removed});
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
        if (n.name == "+") {
            Value *key = emitExpr(*n.left);
            return callRT("perl_plus_hash_get", {key});
        }
        Value *hv = lookupHash(n.name);
        if (!hv) return perlUndef();
        return emitHashGetRef(hv, *n.left);
    }

    case NK::KeysFunc: {
        if (n.name == "+") {
            Value *av = callRT("perl_plus_hash_keys", {});
            return callRT("perl_array_len", {av});
        }
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
        if (n.sval == "array") {
            Value *av = lookupArray(n.name);
            if (!av) return perlInt(0);
            Value *idx = emitIdx(*n.left);
            Value *elem = callRT("perl_array_get_ref", {av, idx});
            Value *def  = callRT("perl_defined", {elem});
            return callRT("perl_alloc_int",
                {builder_.CreateSExt(def, Type::getInt64Ty(ctx_))});
        }
        Value *hv = lookupHash(n.name);
        if (!hv) return perlInt(0);
        Value *r = emitHashExists(hv, *n.left);
        return callRT("perl_alloc_int",
            {builder_.CreateSExt(r, Type::getInt64Ty(ctx_))});
    }

    case NK::DeleteFunc: {
        if (n.sval == "array") {
            Value *av = lookupArray(n.name);
            if (!av) return perlUndef();
            Value *idx = emitIdx(*n.left);
            Value *old = callRT("perl_array_get", {av, idx});
            callRT("perl_array_set", {av, idx, perlUndef()});
            return old;
        }
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

    /* ── rand / srand / time / localtime / gmtime / sleep / alarm ───────── */
    case NK::RandFunc: {
        Value *mx = n.left ? emitExpr(*n.left) : perlUndef();
        Value *r  = callRT("perl_rand_val", {mx});
        if (n.left) freeIfOwned(mx);
        return r;
    }
    case NK::SrandFunc: {
        Value *s = n.left ? emitExpr(*n.left) : perlUndef();
        callRT("perl_srand_val", {s});
        if (n.left) freeIfOwned(s);
        return perlUndef();
    }
    case NK::TimeFunc:
        return callRT("perl_time_val", {});
    case NK::SleepFunc: {
        Value *s = n.left ? emitExpr(*n.left) : perlUndef();
        Value *r = callRT("perl_sleep_val", {s});
        if (n.left) freeIfOwned(s);
        return r;
    }
    case NK::AlarmFunc: {
        Value *s = n.left ? emitExpr(*n.left) : perlUndef();
        Value *r = callRT("perl_alarm_val", {s});
        if (n.left) freeIfOwned(s);
        return r;
    }
    /* localtime / gmtime in scalar context return a string */
    case NK::LocaltimeFunc:
    case NK::GmtimeFunc: {
        /* In scalar context we can't easily tell, so return the epoch for further use.
           Most common use is in list context via emitArrayPtr; scalar context: return time. */
        Value *t = n.left ? emitExpr(*n.left) : perlUndef();
        Value *r = callRT("perl_time_val", {});
        if (n.left) freeIfOwned(t);
        return r;
    }

    /* ── List::Util scalar results ───────────────────────────────────────── */
    case NK::SumFunc:
    case NK::MinFunc:
    case NK::MaxFunc:
    case NK::UniqFunc: {
        /* collect input list into an array then call runtime */
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
        if (n.kind == NK::SumFunc)  return callRT("perl_sum_list", {av});
        if (n.kind == NK::MinFunc)  return callRT("perl_min_list", {av});
        if (n.kind == NK::MaxFunc)  return callRT("perl_max_list", {av});
        /* UniqFunc: scalar context returns first element of unique list */
        return callRT("perl_array_get", {callRT("perl_uniq_list", {av}),
                                         ConstantInt::get(Type::getInt64Ty(ctx_), 0)});
    }

    /* ── first / any / all / none ────────────────────────────────────────── */
    case NK::FirstFunc:
    case NK::AnyFunc:
    case NK::AllFunc:
    case NK::NoneFunc: {
        auto *fn   = builder_.GetInsertBlock()->getParent();
        auto *i64  = Type::getInt64Ty(ctx_);
        auto *i32  = Type::getInt32Ty(ctx_);
        bool isFirst = (n.kind == NK::FirstFunc);
        bool isAll   = (n.kind == NK::AllFunc);
        bool isNone  = (n.kind == NK::NoneFunc);

        /* build input array */
        Value *inputArr = nullptr;
        if (n.args.size() == 1) inputArr = emitArrayPtr(*n.args[0]);
        if (!inputArr) {
            inputArr = callRT("perl_array_new", {});
            for (auto &a : n.args) {
                Value *sub = emitArrayPtr(*a);
                if (sub) callRT("perl_array_extend", {inputArr, sub});
                else     callRT("perl_array_push",   {inputArr, emitExpr(*a)});
            }
        }

        /* result slot — heap PV so perl_assign works */
        Value *resultPv = isFirst  ? perlUndef()
                        : (isAll || isNone) ? perlInt(1)
                        :                     perlInt(0); /* any starts 0 */

        Value *lenPv = callRT("perl_array_len", {inputArr});
        Value *len   = callRT("perl_to_int", {lenPv});
        auto *udAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, "$_");
        Value *udPv    = perlUndef();
        builder_.CreateStore(udPv, udAlloca);
        auto *iAlloca = builder_.CreateAlloca(i64, nullptr, "fan.i");
        builder_.CreateStore(ConstantInt::get(i64, 0), iAlloca);

        auto *condBB = BasicBlock::Create(ctx_, "fan.cond", fn);
        auto *bodyBB = BasicBlock::Create(ctx_, "fan.body", fn);
        auto *trueBB = BasicBlock::Create(ctx_, "fan.true", fn);  /* block was true */
        auto *falseBB= BasicBlock::Create(ctx_, "fan.false",fn);  /* block was false */
        auto *nextBB = BasicBlock::Create(ctx_, "fan.next", fn);  /* continue loop */
        auto *exitBB = BasicBlock::Create(ctx_, "fan.exit", fn);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(condBB);
        Value *i     = builder_.CreateLoad(i64, iAlloca);
        Value *done  = builder_.CreateICmpSGE(i, len);
        builder_.CreateCondBr(done, exitBB, bodyBB);

        builder_.SetInsertPoint(bodyBB);
        Value *elem = callRT("perl_array_get_ref", {inputArr, i});
        callRT("perl_assign", {udPv, elem});
        pushScope();
        declareVar("_", udAlloca);
        Value *blockResult = n.body ? emitBlockLast(*n.body) : perlUndef();
        popScope();
        Value *tv   = callRT("perl_is_true", {blockResult});
        Value *cond = builder_.CreateICmpNE(tv, ConstantInt::get(i32, 0));
        builder_.CreateCondBr(cond, trueBB, falseBB);

        /* true branch */
        builder_.SetInsertPoint(trueBB);
        if (isFirst) {
            callRT("perl_assign", {resultPv, elem});
            builder_.CreateBr(exitBB);
        } else if (isAll) {
            builder_.CreateBr(nextBB);             /* all: true element → continue */
        } else if (isNone) {
            callRT("perl_assign", {resultPv, perlInt(0)});
            builder_.CreateBr(exitBB);             /* none: one true → fail */
        } else {
            callRT("perl_assign", {resultPv, perlInt(1)});
            builder_.CreateBr(exitBB);             /* any: one true → succeed */
        }

        /* false branch */
        builder_.SetInsertPoint(falseBB);
        if (isAll) {
            callRT("perl_assign", {resultPv, perlInt(0)});
            builder_.CreateBr(exitBB);             /* all: one false → fail */
        } else {
            builder_.CreateBr(nextBB);             /* first/any/none: false → continue */
        }

        builder_.SetInsertPoint(nextBB);
        Value *i2 = builder_.CreateAdd(i, ConstantInt::get(i64, 1));
        builder_.CreateStore(i2, iAlloca);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(exitBB);
        return resultPv;
    }

    /* ── reduce ──────────────────────────────────────────────────────────── */
    case NK::ReduceFunc: {
        auto *fn  = builder_.GetInsertBlock()->getParent();
        auto *i64 = Type::getInt64Ty(ctx_);

        /* build input array */
        Value *inputArr = nullptr;
        if (n.args.size() == 1) inputArr = emitArrayPtr(*n.args[0]);
        if (!inputArr) {
            inputArr = callRT("perl_array_new", {});
            for (auto &a : n.args) {
                Value *sub = emitArrayPtr(*a);
                if (sub) callRT("perl_array_extend", {inputArr, sub});
                else     callRT("perl_array_push",   {inputArr, emitExpr(*a)});
            }
        }

        Value *lenPv = callRT("perl_array_len", {inputArr});
        Value *len   = callRT("perl_to_int", {lenPv});

        /* $a and $b allocas */
        auto *aAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, "$a");
        auto *bAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, "$b");
        Value *aPv    = perlUndef();
        Value *bPv    = perlUndef();
        builder_.CreateStore(aPv, aAlloca);
        builder_.CreateStore(bPv, bAlloca);

        /* accumulator starts as first element */
        auto *accAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, "red.acc");
        Value *first    = callRT("perl_array_get_ref", {inputArr, ConstantInt::get(i64, 0)});
        Value *accPv    = perlUndef();
        callRT("perl_assign", {accPv, first});
        builder_.CreateStore(accPv, accAlloca);

        auto *iAlloca = builder_.CreateAlloca(i64, nullptr, "red.i");
        builder_.CreateStore(ConstantInt::get(i64, 1), iAlloca); /* start from index 1 */

        auto *condBB = BasicBlock::Create(ctx_, "red.cond", fn);
        auto *bodyBB = BasicBlock::Create(ctx_, "red.body", fn);
        auto *exitBB = BasicBlock::Create(ctx_, "red.exit", fn);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(condBB);
        Value *i    = builder_.CreateLoad(i64, iAlloca);
        Value *done = builder_.CreateICmpSGE(i, len);
        builder_.CreateCondBr(done, exitBB, bodyBB);

        builder_.SetInsertPoint(bodyBB);
        /* $a = accumulator, $b = current element */
        callRT("perl_assign", {aPv, builder_.CreateLoad(perlPtrTy_, accAlloca)});
        Value *cur = callRT("perl_array_get_ref", {inputArr, i});
        callRT("perl_assign", {bPv, cur});

        pushScope();
        declareVar("a", aAlloca);
        declareVar("b", bAlloca);
        Value *blockResult = n.body ? emitBlockLast(*n.body) : perlUndef();
        popScope();

        /* update accumulator */
        callRT("perl_assign", {accPv, blockResult});

        Value *i2 = builder_.CreateAdd(i, ConstantInt::get(i64, 1));
        builder_.CreateStore(i2, iAlloca);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(exitBB);
        return builder_.CreateLoad(perlPtrTy_, accAlloca);
    }

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
         /* Stage 22: all-numeric elements → flat double[] stored in a PERL_FLAT_ARRAY PV.
            Eliminates per-element PV boxing and PV** indirection in hot loops.
            Reads/writes outside foreach loops fall back to perl_deref_array which
            lazy-converts FLAT_ARRAY → REF_ARRAY in-place on first such access. */
        if (!n.args.empty()) {
            /* FLOAT_PAIR: exactly 2 float elements → perl_alloc_float_pair */
            if (n.args.size() == 2) {
                bool allF64 = canEmitF64(*n.args[0]) && canEmitF64(*n.args[1]);
                if (allF64) {
                    Value *re = emitExprF64(*n.args[0]);
                    Value *im = emitExprF64(*n.args[1]);
                    return callRT("perl_alloc_float_pair", {re, im});
                }
            }
            /* FLAT_ARRAY: all F64-capable children → flat double[] with zero-init */
            bool allFloat = true;
            for (auto &e : n.args) {
                if (!canEmitF64(*e)) { allFloat = false; break; }
            }
            if (allFloat && n.args.size() >= 2) {
                auto *i8Ty  = Type::getInt8Ty(ctx_);
                auto *i64Ty = Type::getInt64Ty(ctx_);
                auto *f64Ty = Type::getDoubleTy(ctx_);
                Value *nElems  = ConstantInt::get(i64Ty, (long long)n.args.size(), true);
                Value *flatPV  = callRT("perl_alloc_float_array", {nElems});
                Value *pvalPtr = builder_.CreateConstInBoundsGEP1_64(
                    i8Ty, flatPV, 8, "flat.pval.ptr");
                Value *dblPtr  = builder_.CreateLoad(perlPtrTy_, pvalPtr, "flat.dbl");
                for (int i = 0; i < (int)n.args.size(); i++) {
                    Value *fv = emitExprF64(*n.args[i]);
                    Value *ep = builder_.CreateConstInBoundsGEP1_64(f64Ty, dblPtr, i, "flat.ep");
                    builder_.CreateStore(fv, ep);
                }
                return flatPV;
            }
        }
        Value *av = callRT("perl_anon_array_new", {});
        for (auto &elem : n.args) {
            /* List-producing nodes (qw(), reverse, range, etc.) must be
               extended into av, not pushed as a single mistyped element. */
            if (Value *sub = emitArrayPtr(*elem)) {
                callRT("perl_array_extend", {av, sub});
            } else {
                Value *pv = emitExpr(*elem);
                callRT("perl_array_push", {av, pv});
                freeIfOwned(pv);
            }
        }
        return callRT("perl_ref_array", {av});
    }

    case NK::AnonHash: {
        Value *hv = callRT("perl_anon_hash_new", {});
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

    case NK::PostfixDeref: {
        /* $r->@* / $r->%* / $r->$* — explicit postfix dereference.
           sval is "all_array" / "all_hash" / "scalar".  In scalar/boolean
           context (e.g. `( $x->@* ) ? ... : ...`) we return the array/hash
           size; callers that want the raw array go through emitArrayPtr. */
        Value *ref = emitExpr(*n.left);
        if (n.sval == "all_array") {
            Value *av = callRT("perl_deref_array", {ref});
            freeIfOwned(ref);
            /* perl_array_len returns a PerlValue* (PV), which is the correct
               scalar-context answer (true iff non-empty) and is what
               `emitBlockLast` consumers expect. */
            return callRT("perl_array_len", {av});
        } else if (n.sval == "all_hash") {
            Value *hv = callRT("perl_deref_hash", {ref});
            freeIfOwned(ref);
            return callRT("perl_hash_size", {hv});
        } else { /* "scalar" */
            Value *pv = callRT("perl_deref_scalar", {ref});
            freeIfOwned(ref);
            return pv;
        }
    }

    case NK::ArrowDeref: {
        if (n.sval == "array" && n.left && n.left->kind == NK::ScalarVar) {
            std::string nm = n.left->name;
            if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
            if (Value *pa = lookupDerefAV(nm)) {
                Value *av = builder_.CreateLoad(arrayPtrTy_, pa, nm + ".av");
                return callRT("perl_array_get_ref", {av, emitIdx(*n.right)});
            }
        }
        /* (LIST)[i] — list-producing expression used directly as array source */
        if (n.sval == "array" && n.left) {
            static auto isListNode = [](NK k) {
                return k == NK::SortFunc || k == NK::MapFunc || k == NK::GrepFunc ||
                       k == NK::ReverseFunc || k == NK::ArrayLit || k == NK::CallerFunc ||
                       k == NK::Call || k == NK::PostfixDeref;
            };
            if (isListNode(n.left->kind)) {
                Value *av = emitArrayPtr(*n.left);
                if (av) return callRT("perl_array_get_ref", {av, emitIdx(*n.right)});
            }
        }
        Value *base = emitExpr(*n.left);
        if (n.sval == "array") {
            /* Stage 32: check loop-invariant deref cache */
            Value *av;
            if (n.left->kind == NK::ScalarVar) {
                std::string nm = n.left->name;
                if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
                if (Value *cacheAlloca = lookupLoopDerefCache(nm)) {
                    av = builder_.CreateLoad(perlPtrTy_, cacheAlloca, nm + ".deref.load");
                } else {
                    av = callRT("perl_deref_array", {base});
                }
            } else {
                av = callRT("perl_deref_array", {base});
            }
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
        if (flags.find('e') != std::string::npos) hasStringEval_ = true;
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
        /* Two forms:
           (a) @arr[i,j,...]  — n.name non-empty, n.left null
           (b) $r->@[i,j,...] — n.name empty, n.left is a ref expr
           Resolve the source array accordingly. */
        Value *av  = nullptr;
        if (!n.name.empty()) {
            av = lookupArray(n.name);
        } else if (n.left) {
            Value *ref = emitExpr(*n.left);
            av = callRT("perl_deref_array", {ref});
            freeIfOwned(ref);
        }
        Value *res = callRT("perl_array_new", {});
        for (auto &idxNode : n.args) {
            Value *elem = av ? callRT("perl_array_get_ref", {av, emitIdx(*idxNode)}) : perlUndef();
            callRT("perl_array_push", {res, elem});
        }
        return callRT("perl_ref_array", {res});
    }

    case NK::HashSlice: {
        /* Two forms:
           (a) %h{k1,k2,...}  — n.name non-empty, n.left null
           (b) $r->%{k1,k2,...} — n.name empty, n.left is a ref expr */
        Value *hv  = nullptr;
        if (!n.name.empty()) {
            hv = lookupHash(n.name);
        } else if (n.left) {
            Value *ref = emitExpr(*n.left);
            hv = callRT("perl_deref_hash", {ref});
            freeIfOwned(ref);
        }
        Value *res = callRT("perl_array_new", {});
        auto pushHashKey2 = [&](const Node &keyNode) {
            if (keyNode.kind == NK::ArrayLit) {
                for (auto &k : keyNode.args) {
                    Value *elem = hv ? emitHashGetRef(hv, *k) : perlUndef();
                    callRT("perl_array_push", {res, elem});
                }
            } else if (Value *kav = emitArrayPtr(keyNode)) {
                Value *slice = hv ? callRT("perl_hash_slice", {hv, kav})
                                  : callRT("perl_array_new", {});
                callRT("perl_array_extend", {res, slice});
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
        auto *i32Ty = Type::getInt32Ty(ctx_);
        Value *level;
        if (n.left) {
            Value *lv64 = callRT("perl_to_int", {emitExpr(*n.left)});
            level = builder_.CreateTrunc(lv64, i32Ty);
        } else {
            level = ConstantInt::get(i32Ty, 0);
        }
        Value *av = callRT("perl_caller", {level});
        if (callCtx_ == 1) return av;   /* list context: return full array */
        /* scalar context: return package name (first element) */
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

        /* Save the caller's insert block — we may need to redirect to a
           continuation block when emitBlockLast's body emission leaves
           the insert point in a sub-context (e.g. a nested eval's endBB
           or a sub's exit block). */
        BasicBlock *callerBB = builder_.GetInsertBlock();

        /* alloca to hold the body's last-expression value (PerlValue*).
           Pre-allocated at the eval site so the body and endBB can both
           reference it.  Initialized to undef so a die/longjmp path that
           skips the body still yields a defined PerlValue* to clone. */
        auto *i8pTy  = PointerType::getUnqual(ctx_);
        auto *bodyRes = builder_.CreateAlloca(perlPtrTy_, nullptr, "eval.bodyres");
        builder_.CreateStore(perlUndef(), bodyRes);

        /* allocate jmp_buf on stack (256 bytes, enough for any platform) */
        auto *i8Arr  = ArrayType::get(Type::getInt8Ty(ctx_), 256);
        auto *jbAlloca = builder_.CreateAlloca(i8Arr, nullptr, "jmp_buf");
        /* cast to ptr for setjmp/perl_eval_push */
        Value *jbPtr = builder_.CreateBitCast(jbAlloca, i8pTy);
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
        /* Stash the bodyRes alloca and endBB so a `return` inside the
           eval body stores its value and jumps to endBB instead of die. */
        int  savedInEval = inEval_;
        Value *savedBodyRes = evalBodyRes_;
        BasicBlock *savedEndBB = evalEndBB_;
        inEval_ = savedInEval + 1;
        evalBodyRes_ = bodyRes;
        evalEndBB_   = endBB;
        if (n.body) {
            /* emit body and capture last expression's value.  emitBlockLast
               already clones before popScope() frees captured variables and
               returns null when the block has no expression value (e.g.
               last stmt is a declaration). */
            Value *lastVal = emitBlockLast(*n.body);
            if (lastVal) {
                builder_.CreateStore(lastVal, bodyRes);
            }
        }
        inEval_ = savedInEval;
        evalBodyRes_ = savedBodyRes;
        evalEndBB_   = savedEndBB;
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        callRT("perl_eval_pop", {});
        /* Load the body's last-expression value (or undef if die/longjmp
           skipped the body, or the block was non-expression). */
        Value *result = builder_.CreateLoad(perlPtrTy_, bodyRes);

        /* If the caller's saved block isn't endBB (meaning emitBlockLast or
           some inner statement moved us to a sub-context — typically a
           nested eval's endBB), branch endBB to a fresh continuation
           block and resume there so subsequent statements in the outer
           eval's body are emitted in the continuation, not the inner
           eval's endBB.  Without this, code following the inner eval is
           inserted into the inner eval's endBB, producing the
           "Terminator found in the middle of a basic block!" verify error. */
        if (callerBB != endBB) {
            auto *contBB = BasicBlock::Create(ctx_, "eval.cont", fn);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
        }
        return result;
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
            } else {
                /* Check int/float unboxed scopes — unboxed vars are stored
                   in intScopes_/floatScopes_, not in scopes_.  We need to
                   box them into a PerlValue* so the closure can capture them.
                   Create a stable alloca to hold the boxed value, then load
                   the PerlValue* from it (matching the pattern for regular captures). */
                if (Value *ia = lookupIntVar(nm)) {
                    Value *ival = builder_.CreateLoad(Type::getInt64Ty(ctx_), ia);
                    Value *boxed = boxI64(ival);
                    auto *pvAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, nm + ".boxed");
                    builder_.CreateStore(boxed, pvAlloca);
                    captureNames.push_back(nm);
                    captureVals.push_back(builder_.CreateLoad(perlPtrTy_, pvAlloca));
                } else if (Value *fa = lookupFloatVar(nm)) {
                    Value *fval = builder_.CreateLoad(Type::getDoubleTy(ctx_), fa);
                    Value *boxed = boxF64(fval);
                    auto *pvAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, nm + ".boxed");
                    builder_.CreateStore(boxed, pvAlloca);
                    captureNames.push_back(nm);
                    captureVals.push_back(builder_.CreateLoad(perlPtrTy_, pvAlloca));
                }
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
        Value *lastVal = emitBlockLast(*n.body);   /* capture last expr for implicit return */
        currentSubBody_ = savedSubBody;
        if (!builder_.GetInsertBlock()->getTerminator()) {
            auto *i32Ty = Type::getInt32Ty(ctx_);
            callRT("perl_pop_wantarray", {});
            Value *depth = builder_.CreateLoad(i32Ty, localDepthAlloca_);
            callRT("perl_local_restore_to", {depth});
            builder_.CreateRet(lastVal ? lastVal : perlUndef());
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
            callRT("perl_array_push_capture", {capsAv, pv});
        return callRT("perl_make_closure", {fnPtr, capsAv});
    }

    case NK::RefSub: {
        auto *subFn = mod_->getFunction(subLLVMName(n.name));
        if (!subFn) return perlUndef();
        Value *fnPtr = ConstantExpr::getPointerCast(subFn, PointerType::getUnqual(ctx_));

        /* Sub-task 2: scan the sub body for shared scalars in scope.
           If any are referenced, build a closure (with captures) so
           that when the sub is later called via threads->create (or
           any other indirection that goes through
           clone_code_ref_for_thread), the captures survive and the
           shared cells are passed by original pointer rather than
           deep-copied.  We remember the capture list in subCaptures_
           so the matching sub body emission in emitSub() can install
           the corresponding `perl_get_capture(i)` initialisers. */
        std::vector<std::string> captureNames;
        std::vector<llvm::Value*> captureVals;
        /* Look up the sub's AST.  We stored a pointer to it in subs_
           in compile() (and the eval-string JIT).  If we can't find
           the AST (e.g. the sub is forward-declared but not yet
           seen), fall back to a plain code ref. */
        const Node *subAst = nullptr;
        for (auto *s : subs_) {
            if (s->name == n.name) { subAst = s; break; }
        }
        if (subAst && subAst->body) {
            std::set<std::string> usedNames;
            collectAllScalarNames(*subAst->body, usedNames);
            for (auto &nm : usedNames) {
                if (nm == "_") continue;
                /* Only capture names that are *shared scalars in
                   scope at the RefSub site*.  Unshared captures
                   don't need closure support — the runtime
                   already deep-copies them through the existing
                   code-ref path.  We restrict to shared scalars
                   to keep the closure small and to match the
                   contract that closures only carry state the
                   sub actually needs to mutate. */
                if (!sharedScalarNames_.count(nm)) continue;
                /* Resolve the current scope's alloca for this name.
                   We need the *cell* (PerlValue*) loaded from the
                   alloca, which is exactly what the closure carries.
                   `lookupVar` returns the alloca for the name in the
                   innermost scope where it was declared. */
                llvm::Value *slot = lookupVar(nm);
                if (!slot) continue;
                /* If the name is a file-scope global (top-level my
                   that lives in a global slot), lookupVar returns
                   the GlobalVariable directly.  We need to skip
                   those for now because the closure capture
                   machinery expects a stack-allocated PerlValue*;
                   file-scope shared scalars use a different
                   codegen path (see fileScalarGlobals_) and don't
                   need closure capture to be visible to threads
                   (they live in a stable global cell). */
                if (isa<llvm::GlobalVariable>(slot)) continue;
                captureNames.push_back(nm);
                captureVals.push_back(builder_.CreateLoad(perlPtrTy_, slot));
            }
        }

        if (captureNames.empty()) {
            return callRT("perl_make_code_ref", {fnPtr});
        }

        /* Record the capture list for the sub body emission.  This
           is keyed by sub name; if the same sub is referenced from
           multiple call sites we want a single canonical capture
           list (the order must match what the sub body expects
           via perl_get_capture). */
        if (!subCaptures_.count(n.name)) {
            subCaptures_[n.name] = captureNames;
        }

        /* Build the captures array — a PerlArray* holding the cell
           pointers of the captured shared scalars.  Pushing the cell
           pointers in is enough; the runtime side of the closure
           (perl_call_code_ref) wires s_current_captures to the array
           so the sub body can fetch them with perl_get_capture(i). */
        Value *capsAv = callRT("perl_array_new", {});
        for (auto *pv : captureVals)
            callRT("perl_array_push_capture", {capsAv, pv});
        return callRT("perl_make_closure", {fnPtr, capsAv});
    }

    case NK::CallCodeRef: {
        Value *ref = emitExpr(*n.left);
        Value *av  = callRT("perl_array_new", {});
        for (auto &arg : n.args) {
            Value *src = emitArrayPtr(*arg);
            if (src) callRT("perl_array_extend", {av, src});
            else     callRT("perl_array_push",   {av, emitExpr(*arg)});
        }
        /* Set wantarray context for the called sub */
        auto *i32Ty = Type::getInt32Ty(ctx_);
        Value *ctxVal;
        if (callCtx_ == 1) {
            ctxVal = ConstantInt::get(i32Ty, 1);
        } else if (currentSubNeedsWantarray_) {
            ctxVal = callRT("perl_current_wantarray_ctx", {});
        } else {
            ctxVal = ConstantInt::get(i32Ty, 0);
        }
        callRT("perl_push_wantarray", {ctxVal});
        Value *result = callRT("perl_call_code_ref", {ref, av});
        callRT("perl_pop_wantarray", {});
        return result;
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
        /* isa / can — UNIVERSAL methods */
        if (n.sval == "isa") {
            Value *cls = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
            return callRT("perl_isa_check", {obj, cls});
        }
        if (n.sval == "can") {
            Value *meth = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
            return callRT("perl_can_check", {obj, meth});
        }
        /* SUPER::method — dispatch starting from parent of caller package */
        if (n.sval.size() > 7 && n.sval.substr(0, 7) == "SUPER::") {
            std::string realMethod = n.sval.substr(7);
            Value *callerPkg  = builder_.CreateGlobalStringPtr(n.name);
            Value *methodStr  = builder_.CreateGlobalStringPtr(realMethod);
            return callRT("perl_dispatch_method_super", {obj, callerPkg, methodStr, argsArr});
        }
        /* threads class methods — intercept before generic dispatch */
        if (n.left && n.left->kind == NK::StringLit && n.left->sval == "threads") {
            if (n.sval == "create") {
                Value *code = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
                Value *thArgs = callRT("perl_array_new", {});
                for (size_t i = 1; i < n.args.size(); i++)
                    callRT("perl_array_push", {thArgs, emitExpr(*n.args[i])});
                return callRT("perl_threads_create", {code, thArgs});
            }
            if (n.sval == "self")  return callRT("perl_threads_self",  {});
            if (n.sval == "list")  return callRT("perl_threads_list",  {});
            if (n.sval == "yield") { callRT("perl_threads_yield", {}); return perlUndef(); }
        }
        /* thread instance methods — dispatch handles PERL_THREAD objects */
        Value *methodStr = builder_.CreateGlobalStringPtr(n.sval);
        {
            auto *i32Ty = Type::getInt32Ty(ctx_);
            Value *pkgStr  = builder_.CreateGlobalStringPtr(currentPackage_);
            Value *fileStr = builder_.CreateGlobalStringPtr(sourceFile_);
            Value *lineVal = ConstantInt::get(i32Ty, n.line);
            callRT("perl_push_call_frame", {pkgStr, fileStr, lineVal});
            Value *r = callRT("perl_dispatch_method", {obj, methodStr, argsArr});
            callRT("perl_pop_call_frame", {});
            return r;
        }
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
        bool rhsTerminated = rBB->getTerminator() != nullptr;
        if (!rhsTerminated) builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        auto *phi = builder_.CreatePHI(perlPtrTy_, 2, "and.result");
        phi->addIncoming(lv, lBB);
        if (!rhsTerminated) phi->addIncoming(rv, rBB);
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
        bool rhsTerminated = rBB->getTerminator() != nullptr;
        if (!rhsTerminated) builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        auto *phi = builder_.CreatePHI(perlPtrTy_, 2, "or.result");
        phi->addIncoming(lv, lBB);
        if (!rhsTerminated) phi->addIncoming(rv, rBB);
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
        bool rhsTerminated = rBB->getTerminator() != nullptr;
        if (!rhsTerminated) builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        auto *phi = builder_.CreatePHI(perlPtrTy_, 2, "defor.result");
        phi->addIncoming(lv, lBB);
        if (!rhsTerminated) phi->addIncoming(rv, rBB);
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
        bool thenTerminated = tb->getTerminator() != nullptr;
        if (!thenTerminated) builder_.CreateBr(endBB);

        builder_.SetInsertPoint(elseBB);
        Value *ev = emitExpr(*n.right);
        auto *eb  = builder_.GetInsertBlock();
        bool elseTerminated = eb->getTerminator() != nullptr;
        if (!elseTerminated) builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        auto *phi = builder_.CreatePHI(perlPtrTy_, 2, "tern.result");
        phi->addIncoming(tv, tb);
        if (!elseTerminated) phi->addIncoming(ev, eb);
        return phi;
    }

    Value *lv = emitExpr(*n.left);
    Value *rv = emitExpr(*n.right);

    static const struct { const char *op; const char *rt; } OPS[] = {
        {"+",  "perl_add"   }, {"-",  "perl_sub"   }, {"*",  "perl_mul"   },
        {"/",  "perl_div"   }, {"%",  "perl_mod"   }, {"**", "perl_pow"   },
        {".",  "perl_concat"},
        {"&",  "perl_bitand"}, {"|",  "perl_bitor" }, {"^",  "perl_bitxor"},
        {"<<", "perl_lshift"}, {">>", "perl_rshift"},
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

/* Try to expand a user-defined sub call inline, bypassing @_ construction.
   Inlineable subs have the form: my ($p1,..) = @_; return expr.
   Returns nullptr if not inlineable; otherwise returns the expanded result.
   Safety: args are stored directly (no clone). FLOAT_PAIR fast path avoids
   deref_array mutation; _ro norm path is pure and LLVM-hoistable. */
Value *CodeGen::tryEmitInline(const Node &n) {
    auto it = inlineSubs_.find(n.name);
    if (it == inlineSubs_.end()) return nullptr;
    const auto &is = it->second;
    if (n.args.size() != is.params.size()) return nullptr;

    /* If the body is F64-capable, emit it directly in F64 context.
       This eliminates perl_clone + boxing for inlineable subs with float bodies.
       The returned Value* is a bare double (not a PV*) — callers must handle both cases. */
    if (canEmitF64(*is.bodyExpr)) {
        pushScope();
        std::vector<Value *> ownedArgs;
        auto *f64Ty = Type::getDoubleTy(ctx_);
        for (size_t i = 0; i < is.params.size(); i++) {
            Value *argVal = nullptr;
            if (n.args[i]->kind == NK::Call) argVal = tryEmitInline(*n.args[i]);
            if (!argVal) argVal = emitExpr(*n.args[i]);
            auto *slot = builder_.CreateAlloca(perlPtrTy_, nullptr, "$" + is.params[i]);
            builder_.CreateStore(argVal, slot);
            declareVar(is.params[i], slot);
            if (isOwnedTemp(argVal)) ownedArgs.push_back(argVal);
            /* Also expose F64 arg as float var for body */
            if (canEmitF64(*n.args[i])) {
                if (Value *fv = emitExprF64(*n.args[i])) {
                    auto *fslot = builder_.CreateAlloca(f64Ty, nullptr, "f$" + is.params[i]);
                    builder_.CreateStore(fv, fslot);
                    if (!floatScopes_.empty()) floatScopes_.back()[is.params[i]] = fslot;
                }
            }
        }
        Value *f64val = emitExprF64(*is.bodyExpr);
        popScope();
        for (Value *v : ownedArgs) callRT("perl_free", {v});
        return f64val;
    }

    /* Evaluate each argument, trying recursive inline for nested sub calls.
       Bail out if any arg is a list-producing expression (can't bind directly). */
    std::vector<Value *> argVals;
    std::vector<Value *> ownedArgs;
    for (size_t i = 0; i < is.params.size(); i++) {
        Value *v = nullptr;
        /* Recursively inline nested sub calls */
        if (n.args[i]->kind == NK::Call)
            v = tryEmitInline(*n.args[i]);
        /* Bail if arg would need list-expansion */
        if (!v && emitArrayPtr(*n.args[i]) != nullptr) {
            /* emitArrayPtr emitted IR — clean up by emitting a fresh expr instead.
               Actually emitArrayPtr has side effects; if it returned a PerlArray*
               we can't easily undo it, so just fall back to normal call. */
            return nullptr;
        }
        if (!v) v = emitExpr(*n.args[i]);
        argVals.push_back(v);
        if (isOwnedTemp(v)) ownedArgs.push_back(v);
    }

    /* Bind params to args via temporary allocas (no clone — arg PV* shared).
       Also add float allocas when the arg is canEmitF64 so the body can use
       the F64 path for params (enables FLOAT_PAIR for cplx(float, float)). */
    pushScope();
    auto *f64Ty = Type::getDoubleTy(ctx_);
    for (size_t i = 0; i < is.params.size(); i++) {
        auto *slot = builder_.CreateAlloca(perlPtrTy_, nullptr, "$" + is.params[i]);
        builder_.CreateStore(argVals[i], slot);
        declareVar(is.params[i], slot);
        /* If the original arg node is F64-capable, expose it as a float var too. */
        if (canEmitF64(*n.args[i])) {
            if (Value *fv = emitExprF64(*n.args[i])) {
                auto *fslot = builder_.CreateAlloca(f64Ty, nullptr, "f$" + is.params[i]);
                builder_.CreateStore(fv, fslot);
                if (!floatScopes_.empty()) floatScopes_.back()[is.params[i]] = fslot;
            }
        }
    }

    /* Emit the body expression */
    Value *result = emitExpr(*is.bodyExpr);

    /* Clean up param scope (don't free param slot PVs — we don't own them) */
    popScope();

    /* Free owned arg temps (e.g. cadd result passed to outer cadd) */
    for (Value *v : ownedArgs) callRT("perl_free", {v});
    return result;
}

Value *CodeGen::emitCall(const Node &n) {
    /* Try AST-level inline first: eliminates @_ construction for simple subs.
       If the inlined sub returns F64, box it into a PV* before returning. */
    if (Value *v = tryEmitInline(n)) {
        /* Check if this is an F64 value (not a PV*). We detect this by checking
           if the value is a ConstantFP, a PHI node of f64 type, or a binary
           operation on f64 types. If so, box it. */
        auto *ty = v->getType();
        if (ty->isFloatingPointTy() && ty->getPrimitiveSizeInBits() == 64) {
            /* Box F64 result into a PV* using perl_alloc_float */
            Value *pv = callRT("perl_alloc_float", {v});
            return pv;
        }
        return v;
    }

    /* eval EXPR — JIT-based string eval */
    if (n.name == "eval") {
        hasStringEval_ = true;
        Value *strVal = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_eval_string", {strVal});
    }
    /* ── Qualified / imported function interceptions ──────────────────────── */
    /* POSIX */
    auto buildArgArray = [&]() -> Value * {
        Value *av = callRT("perl_array_new", {});
        for (auto &arg : n.args) callRT("perl_array_push", {av, emitExpr(*arg)});
        return av;
    };
    if (n.name == "POSIX::floor" || n.name == "floor") {
        Value *v = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_posix_floor", {v});
    }
    if (n.name == "POSIX::ceil" || n.name == "ceil") {
        Value *v = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_posix_ceil", {v});
    }
    if (n.name == "POSIX::fmod") {
        Value *a = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *b = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        return callRT("perl_posix_fmod", {a, b});
    }
    if (n.name == "POSIX::strftime" || n.name == "strftime") {
        return callRT("perl_posix_strftime", {buildArgArray()});
    }
    /* Scalar::Util */
    if (n.name == "Scalar::Util::blessed" || n.name == "blessed") {
        Value *v = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_su_blessed", {v});
    }
    if (n.name == "Scalar::Util::reftype" || n.name == "reftype") {
        Value *v = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_su_reftype", {v});
    }
    if (n.name == "Scalar::Util::looks_like_number" || n.name == "looks_like_number") {
        Value *v = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_su_looks_like_number", {v});
    }
    /* Carp */
    if (n.name == "Carp::croak" || n.name == "croak" ||
        n.name == "Carp::confess" || n.name == "confess") {
        callRT("perl_carp_croak", {buildArgArray()});
        builder_.CreateUnreachable();
        auto *dead = BasicBlock::Create(ctx_, "croak.dead", builder_.GetInsertBlock()->getParent());
        builder_.SetInsertPoint(dead);
        return perlUndef();
    }
    if (n.name == "Carp::carp" || n.name == "carp" ||
        n.name == "Carp::cluck" || n.name == "cluck") {
        callRT("perl_carp_carp", {buildArgArray()});
        return perlUndef();
    }
    /* UNIVERSAL */
    if (n.name == "UNIVERSAL::isa") {
        Value *a = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *b = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        return callRT("perl_isa_check", {a, b});
    }
    if (n.name == "DBI::connect") {
        Value *dsn  = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *user = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        Value *pass = n.args.size() > 2 ? emitExpr(*n.args[2]) : perlUndef();
        Value *ret = callRT("perl_dbi_connect", {dsn, user, pass});
        freeIfOwned(dsn);
        freeIfOwned(user);
        freeIfOwned(pass);
        return ret;
    }
    if (n.name == "XS::load_library") {
        Value *lib = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        Value *ret = callRT("perl_xs_load_library", {lib});
        freeIfOwned(lib);
        return ret;
    }
    if (n.name == "XS::call") {
        Value *lib = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *func = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        Value *sig = n.args.size() > 2 ? emitExpr(*n.args[2]) : perlUndef();
        Value *av = callRT("perl_array_new", {});
        for (size_t i = 3; i < n.args.size(); i++) {
            Value *arg = emitExpr(*n.args[i]);
            callRT("perl_array_push", {av, arg});
            freeIfOwned(arg);
        }
        Value *ret = callRT("perl_xs_call_dynamic", {lib, func, sig, av});
        callRT("perl_array_free", {av});
        freeIfOwned(lib);
        freeIfOwned(func);
        freeIfOwned(sig);
        return ret;
    }
    if (auto *fn = mod_->getFunction(subLLVMName(n.name))) {
        /* Use push_nc (no-clone) for scalar args so we skip 63M clone/free per
           mbs.pl run. Owned temps (e.g. cadd result) are collected and freed
           after the call; non-owned (ScalarVar, borrow) need no action. */
        Value *argsArr = callRT("perl_array_new", {});
        for (auto &arg : n.args) {
            if (arg->kind == NK::ArrayVar) {
                Value *av = lookupArray(arg->name);
                if (av) { callRT("perl_array_extend", {argsArr, av}); continue; }
            }
            if (arg->kind == NK::HashVar) {
                Value *hv = lookupHash(arg->name);
                if (hv) { callRT("perl_array_extend_hash", {argsArr, hv}); continue; }
            }
            if (Value *av = emitArrayPtr(*arg)) {
                callRT("perl_array_extend", {argsArr, av});
                continue;
            }
            Value *v = emitExpr(*arg);
            callRT("perl_array_push", {argsArr, v});
            freeIfOwned(v);
        }
        auto *i32Ty = Type::getInt32Ty(ctx_);
        Value *pkgStr  = builder_.CreateGlobalStringPtr(currentPackage_, "caller.pkg");
        Value *fileStr = builder_.CreateGlobalStringPtr(sourceFile_,     "caller.file");
        Value *lineVal = ConstantInt::get(i32Ty, n.line);
        callRT("perl_push_call_frame", {pkgStr, fileStr, lineVal});
        Value *ctxVal;
        if (callCtx_ == 1) {
            ctxVal = ConstantInt::get(i32Ty, 1);
        } else if (currentSubNeedsWantarray_) {
            ctxVal = callRT("perl_current_wantarray_ctx", {});
        } else {
            ctxVal = ConstantInt::get(i32Ty, 0);
        }
        callCtx_ = 0;
        Value *retVal = builder_.CreateCall(fn, {argsArr, ctxVal});
        callRT("perl_pop_call_frame", {});
        callRT("perl_array_free", {argsArr});
        return retVal;
    }
    if (n.name.find("::") != std::string::npos) {
        Value *argsArr = callRT("perl_array_new", {});
        for (auto &arg : n.args) {
            if (arg->kind == NK::ArrayVar) {
                Value *av = lookupArray(arg->name);
                if (av) { callRT("perl_array_extend", {argsArr, av}); continue; }
            }
            if (arg->kind == NK::HashVar) {
                Value *hv = lookupHash(arg->name);
                if (hv) { callRT("perl_array_extend_hash", {argsArr, hv}); continue; }
            }
            if (Value *av = emitArrayPtr(*arg)) {
                callRT("perl_array_extend", {argsArr, av});
                continue;
            }
            Value *v = emitExpr(*arg);
            callRT("perl_array_push", {argsArr, v});
            freeIfOwned(v);
        }
        auto *i32Ty = Type::getInt32Ty(ctx_);
        Value *ctxVal;
        if (callCtx_ == 1) {
            ctxVal = ConstantInt::get(i32Ty, 1);
        } else if (currentSubNeedsWantarray_) {
            ctxVal = callRT("perl_current_wantarray_ctx", {});
        } else {
            ctxVal = ConstantInt::get(i32Ty, 0);
        }
        callCtx_ = 0;
        Value *nameStr = builder_.CreateGlobalStringPtr(n.name);
        Value *retVal = callRT("perl_call_named_sub", {nameStr, argsArr, ctxVal});
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

/* ── string eval compilation ─────────────────────────────────────────────── */

void CodeGen::compileForEval(const Node &program, const std::string &funcName) {
    auto *pv    = perlPtrTy_;
    auto *i32Ty = Type::getInt32Ty(ctx_);

    /* Pre-declare any subs so forward calls work (mirrors compile()) */
    subs_.clear();
    subCaptures_.clear();
    for (auto &stmt : program.args)
        if (stmt->kind == NK::SubDef) subs_.push_back(stmt.get());
    for (auto *s : subs_) {
        auto *ft = FunctionType::get(pv, {arrayPtrTy_, Type::getInt32Ty(ctx_)}, false);
        auto *fn = Function::Create(ft, Function::ExternalLinkage, subLLVMName(s->name), mod_.get());
        fn->addFnAttr(Attribute::AlwaysInline);
    }

    /* emit: PerlValue *funcName() */
    auto *evalFT = FunctionType::get(pv, {}, false);
    auto *evalFn = Function::Create(evalFT, Function::ExternalLinkage,
                                    funcName, mod_.get());
    auto *entry = BasicBlock::Create(ctx_, "entry", evalFn);
    builder_.SetInsertPoint(entry);

    currentFn_ = evalFn;
    pushScope();
    fileScopeDepth_ = (int)scopes_.size() + 1;
    inMainBody_ = true;

    /* register package-qualified subs before eval code runs */
    for (auto *s : subs_) {
        if (s->name.find("::") != std::string::npos) {
            Value *keyStr = builder_.CreateGlobalStringPtr(s->name);
            auto *fn = mod_->getFunction(subLLVMName(s->name));
            callRT("perl_register_method", {keyStr, fn});
        }
    }

    /* $_ available inside eval */
    Value *underscoreVal = callRT("perl_alloc_undef", {});
    auto *slotUs = builder_.CreateAlloca(pv, nullptr, "$_");
    builder_.CreateStore(underscoreVal, slotUs);
    declareVar("_", slotUs);

    /* local() depth tracking */
    localDepthAlloca_ = builder_.CreateAlloca(i32Ty, nullptr, "local.depth");
    builder_.CreateStore(callRT("perl_local_save_depth", {}), localDepthAlloca_);

    /* emit body, capture last value */
    Value *lastVal = emitBlockLast(program);
    if (!lastVal) lastVal = perlUndef();

    popScope();

    /* restore any local()s */
    Value *depth = builder_.CreateLoad(i32Ty, localDepthAlloca_);
    callRT("perl_local_restore_to", {depth});

    inMainBody_ = false;
    localDepthAlloca_ = nullptr;

    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateRet(lastVal);

    /* emit sub bodies (mirrors compile()) */
    for (auto *s : subs_) emitSub(*s);
}

/* ── output ──────────────────────────────────────────────────────────────── */

std::unique_ptr<Module> CodeGen::releaseModule() {
    return std::move(mod_);
}

std::unique_ptr<LLVMContext> CodeGen::releaseContext() {
    return std::move(ctx_owned_);
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
