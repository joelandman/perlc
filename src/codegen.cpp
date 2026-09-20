#include "codegen.h"
#include "runtime.h"
#include "lexer.h"
#include "parser.h"
#include <llvm/IR/Verifier.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
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

/* collect all ScalarVar names referenced in a node (skip nested SubDef —
   named subs are compiled as standalone top-level functions, unrelated to
   the enclosing lexical scope).  AnonSub IS recursed into: a nested closure
   may reference an outer variable that this closure only sees transitively
   (e.g. `sub { sub { ...$x... } }`), so it must also be captured here in
   order to relay it to the inner closure. */
static void collectAllScalarNames(const Node &n, std::set<std::string> &names) {
    switch (n.kind) {
    case NK::ScalarVar: {
        std::string nm = n.name;
        if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
        names.insert(nm);
        return;
    }
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

/* D109: s/PATTERN/REPLACEMENT/'s REPLACEMENT text is captured 100% raw by
   the lexer (readSubst/readSection do no escape processing at all, unlike
   readString for ordinary "..." literals) — so before it can be handed to
   Parser::parseInterpString (the same variable-interpolation scanner
   "..." literals use), it needs the same backslash-escape pass readString
   already applies for interpolating strings. Mirrors readString's switch
   in src/lexer.cpp exactly, including \x02-marking \$ and \@ so the
   interpolation scanner can tell an escaped-literal $/@ apart from a real
   trigger. Also covers \f \a \e \b (D108's gap in readString itself is
   NOT fixed by this — this is a separate, local copy of the table used
   only for s///'s replacement text). */
static std::string preprocessReplEscapes(const std::string &raw) {
    std::string out;
    out.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        char c = raw[i];
        if (c == '\\' && i + 1 < raw.size()) {
            char esc = raw[i + 1];
            i += 2;
            switch (esc) {
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                case 'f':  out += '\f'; break;
                case 'b':  out += '\b'; break;
                case 'a':  out += '\a'; break;
                case 'e':  out += '\x1b'; break;
                case '0':  out += '\0'; break;
                case '\\': out += '\\'; break;
                case '$':  out += '\x02'; out += '$'; break;
                case '@':  out += '\x02'; out += '@'; break;
                default:   out += esc; break; /* unknown escape: drop backslash, keep char (matches D107) */
            }
            continue;
        }
        out += c;
        i++;
    }
    return out;
}

/* Does `s` (already escape-preprocessed by preprocessReplEscapes) contain
   a real *named-variable* interpolation trigger? Deliberately excludes
   $0-$9 and $&amp; (capture refs) — those are already handled correctly
   by the existing fast raw-string path (perl_regex_subst, D107), and the
   overwhelming majority of real replacement text is plain text or
   capture-ref-only, so routing only genuinely new forms ($name, @arr,
   ${...}, $$ref, $@) through the new closure machinery keeps every
   already-passing capture-ref test on its proven path untouched. */
static bool hasInterpTrigger(const std::string &s) {
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\x02') { i++; continue; }
        if (s[i] == '$' && i + 1 < s.size()) {
            char nc = s[i + 1];
            if (nc >= '0' && nc <= '9') continue; /* $0-$9: old path */
            if (nc == '&') continue;              /* $&: old path */
            return true;
        }
        if (s[i] == '@' && i + 1 < s.size()) {
            char nc = s[i + 1];
            if (isalpha((unsigned char)nc) || nc == '_' || nc == '{' || nc == '$')
                return true;
        }
    }
    return false;
}

/* D64/D53: collect every scalar name referenced anywhere inside ANY
   closure (AnonSub, or sort{}'s custom comparator) nested within `n`, OR
   that ever has \$name (RefScalar) taken anywhere in `n` — used to decide
   whether a `my $var = <literal>` declaration is safe to place on the
   unboxed int/float fast path (intScopes_/floatScopes_), which has no
   real, addressable PerlValue* for a closure to later capture/share, or
   for \$var to point at. Boxing a snapshot on demand instead (the
   existing lookupIntVar/boxI64 fallback used both at closure-capture time
   and by plain `\$var` reference-taking) silently disconnects the
   closure's/reference's view from the real variable: D64 found this for
   closures (`sub { ...$x... }` capturing a stale copy); D53 found the
   identical underlying issue for `\$x` (`\$x` boxes a one-off snapshot of
   $x's current unboxed value, so `$$r = 99` mutates that disposable
   snapshot while $x's real unboxed storage — and therefore every
   subsequent plain read of $x — is completely unaffected; confirmed via
   --emit-ir that a file-scope $x, which never uses this fast path at all,
   is unaffected, isolating the bug to exactly this fast-path's blind
   spot). Deliberately over-approximates — a name used only in some
   unrelated, never-actually-captured/referenced context elsewhere in the
   same function still loses the fast path — rather than risk
   misclassifying a genuinely captured/referenced variable as safe (a
   correctness bug, not just a missed optimization). Computed once per
   function (named sub, AnonSub, or the top-level program body) at
   compilation entry; see D64/D53 in TESTS.md and capturedNamesInCurrentFn_
   in codegen.h. */
static void collectClosureCapturedNames(const Node &n, std::unordered_set<std::string> &names) {
    if (n.kind == NK::AnonSub || (n.kind == NK::SortFunc && n.sval == "custom")) {
        if (n.body) {
            std::set<std::string> inner;
            collectAllScalarNames(*n.body, inner);
            names.insert(inner.begin(), inner.end());
        }
    }
    if (n.kind == NK::RefScalar && n.left && n.left->kind == NK::ScalarVar) {
        names.insert(n.left->name);
    }
    if (n.left)  collectClosureCapturedNames(*n.left,  names);
    if (n.right) collectClosureCapturedNames(*n.right, names);
    if (n.cond)  collectClosureCapturedNames(*n.cond,  names);
    if (n.body)  collectClosureCapturedNames(*n.body,  names);
    if (n.init)  collectClosureCapturedNames(*n.init,  names);
    if (n.step)  collectClosureCapturedNames(*n.step,  names);
    for (auto &b : n.branches) {
        if (b.cond) collectClosureCapturedNames(*b.cond, names);
        if (b.body) collectClosureCapturedNames(*b.body, names);
    }
    for (auto &a : n.args) collectClosureCapturedNames(*a, names);
}

/* True if this subtree contains `eval EXPR` (NK::Call named eval). Named
   SubDef bodies are skipped — they compile as their own functions. AnonSub
   is recursed into so an outer `my` that a nested closure eval's is boxed. */
static bool hasEvalCall(const Node &n) {
    if (n.kind == NK::SubDef) return false;
    if (n.kind == NK::Call && n.name == "eval") return true;
    if (n.left && hasEvalCall(*n.left)) return true;
    if (n.right && hasEvalCall(*n.right)) return true;
    if (n.cond && hasEvalCall(*n.cond)) return true;
    if (n.body && hasEvalCall(*n.body)) return true;
    if (n.init && hasEvalCall(*n.init)) return true;
    if (n.step && hasEvalCall(*n.step)) return true;
    for (auto &b : n.branches) {
        if (b.cond && hasEvalCall(*b.cond)) return true;
        if (b.body && hasEvalCall(*b.body)) return true;
    }
    for (auto &a : n.args) if (hasEvalCall(*a)) return true;
    return false;
}

static bool evalPadSkipName(const std::string &nm) {
    if (nm.empty() || nm == "_" || nm == "AUTOLOAD") return true;
    bool allDigit = true;
    for (char c : nm) {
        if (c < '0' || c > '9') { allDigit = false; break; }
    }
    if (allDigit) return true; /* $0, $1..$n */
    if (nm.size() == 1 && !((nm[0] >= 'A' && nm[0] <= 'Z') ||
                            (nm[0] >= 'a' && nm[0] <= 'z')))
        return true; /* $! $/ $. $, etc. */
    return false;
}

static void collectEvalPadNames(const Node &n,
                                std::set<std::string> &scalars,
                                std::set<std::string> &arrays,
                                std::set<std::string> &hashes) {
    auto addS = [&](std::string nm) {
        if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
        if (!evalPadSkipName(nm)) scalars.insert(nm);
    };
    auto addA = [&](std::string nm) {
        if (!nm.empty() && nm[0] == '@') nm = nm.substr(1);
        if (nm.empty() || nm == "_") return;
        arrays.insert(nm);
    };
    auto addH = [&](std::string nm) {
        if (!nm.empty() && nm[0] == '%') nm = nm.substr(1);
        if (nm.empty()) return;
        hashes.insert(nm);
    };
    switch (n.kind) {
    case NK::ScalarVar: addS(n.name); break;
    case NK::ArrayVar: case NK::ArrayElem: addA(n.name); break;
    case NK::HashVar: case NK::HashElem: addH(n.name); break;
    default: break;
    }
    if (n.left)  collectEvalPadNames(*n.left,  scalars, arrays, hashes);
    if (n.right) collectEvalPadNames(*n.right, scalars, arrays, hashes);
    if (n.cond)  collectEvalPadNames(*n.cond,  scalars, arrays, hashes);
    if (n.body)  collectEvalPadNames(*n.body,  scalars, arrays, hashes);
    if (n.init)  collectEvalPadNames(*n.init,  scalars, arrays, hashes);
    if (n.step)  collectEvalPadNames(*n.step,  scalars, arrays, hashes);
    for (auto &b : n.branches) {
        if (b.cond) collectEvalPadNames(*b.cond, scalars, arrays, hashes);
        if (b.body) collectEvalPadNames(*b.body, scalars, arrays, hashes);
    }
    for (auto &a : n.args) collectEvalPadNames(*a, scalars, arrays, hashes);
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

    /* Step 3: parse PERLC_OPT_DISABLE to selectively disable optimization
       stages for diagnosis (e.g. "flatdouble,allflat,stage31,stage32,stage33").
       This implements the systematic disable/re-enable plan from REARCHITECTURE.md. */
    if (const char *env = getenv("PERLC_OPT_DISABLE")) {
        std::string s(env);
        std::string cur;
        for (char c : s) {
            if (c == ',' || c == ' ' || c == ';') {
                if (!cur.empty()) { disabledStages_.insert(cur); cur.clear(); }
            } else {
                cur.push_back((char)tolower(c));
            }
        }
        if (!cur.empty()) disabledStages_.insert(cur);
    }

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
    RT("perl_alloc_bool",    pv,  i64);
    RT("perl_alloc_float",   pv,  Type::getDoubleTy(ctx_));
    RT("perl_alloc_string",  pv,  i8p);
    RT("perl_alloc_string_len", pv, i8p, i64);
    RT("perl_clone",         pv,  pv);
    RT("perl_free",          voidTy, pv);
    RT("perl_assign",        voidTy, pv, pv);
    RT("perl_to_int",        i64, pv);
    RT("perl_to_float",      Type::getDoubleTy(ctx_), pv);
    RT("perl_is_true",       Type::getInt32Ty(ctx_), pv);
    RT("perl_is_bigint_pv",  Type::getInt32Ty(ctx_), pv);   /* D132 */
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
    RT("perl_mod_i64",       i64, i64, i64);
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
    RT("perl_flat_row_op_assign", pv,     pv, i64, pv, Type::getInt32Ty(ctx_));
    RT("perl_array_set_row",      pv,     pv, i64, pv);
    RT("perl_array_ensure_slot",  voidTy, av, i64);
    /* D97: Math::BigInt built-in methods */
    RT("perl_bigint_new",         pv,     pv);
    RT("perl_bigint_bmul",        pv,     pv, pv);
    RT("perl_bigint_badd",        pv,     pv, pv);
    RT("perl_bigint_bsub",        pv,     pv, pv);
    RT("perl_bigint_bcmp",        pv,     pv, pv);
    RT("perl_bigint_numify",      pv,     pv);
    /* D103: unblessed auto-BigInt from a decimal-literal string */
    RT("perl_bigint_from_decstr_unblessed", pv, i8p);
    RT("perl_bigint_ovl_add",     pv,     pv, pv);
    RT("perl_bigint_ovl_sub",     pv,     pv, pv);
    RT("perl_bigint_ovl_mul",     pv,     pv, pv);
    RT("perl_bigint_ovl_div",     pv,     pv, pv);
    RT("perl_bigint_ovl_cmp",     pv,     pv, pv);
    RT("perl_bigint_ovl_str",     pv,     pv);
    RT("perl_bigint_ovl_neg",     pv,     pv);
    RT("perl_array_delete",       pv,     av, i64);
    RT("perl_array_lvalue",       pv,     av, i64);
    RT("perl_array_update_float", voidTy, av, i64, Type::getDoubleTy(ctx_));
    RT("perl_array_is_all_flat", i64, av);  /* used by Stage 23 all-flat pre-check */
     RT("perl_array_len",     pv,  av);
     RT("perl_array_last",    pv,  av);
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
    RT("perl_array_slice",   av,  av, av);
    RT("perl_hash_values",   av,  av);
    RT("perl_hash_size",     pv,  av);
    RT("perl_hash_from_list",voidTy, av, av);
    RT("perl_hash_autoviv_hash",    av, av, strPtrTy);
    RT("perl_hash_autoviv_hash_sv", av, av, pv);
    RT("perl_hash_autoviv_array",   av, av, strPtrTy);
    RT("perl_hash_autoviv_array_sv",av, av, pv);
    RT("perl_array_autoviv_hash",   av, av, i64);
    RT("perl_array_autoviv_array",  av, av, i64);
    /* D50: scalar-ref-rooted autoviv (handles FLAT_ARRAY/FLOAT_PAIR safely) */
    RT("perl_array_autoviv_array_from_scalar", av, perlPtrTy_, i64);
    RT("perl_array_autoviv_hash_from_scalar",  av, perlPtrTy_, i64);
    RT("perl_array_autoviv_array_idx",         av, av, i64);
    RT("perl_hash_autoviv_hash_idx",           av, av, pv);
    RT("perl_array_autoviv_hash_idx",          av, av, i64);
    RT("perl_hash_autoviv_array_idx",          av, pv, av);
    RT("perl_hash_autoviv_array_idx_sv",       av, av, pv);
    RT("perl_deref_array_auto",                pv, pv, i64);
    RT("perl_hash_assign_slice",    voidTy, av, av, av);
    RT("perl_array_assign_slice",   voidTy, av, av, av);
    RT("perl_array_sort_str",   voidTy, av);
    RT("perl_array_extend",     voidTy, av, av);
    RT("perl_array_extend_from",voidTy, av, av, i64);
    RT("perl_array_extend_hash",voidTy, av, av);
    RT("perl_array_push_list_or_scalar", voidTy, av, pv);
    RT("perl_array_shift",      pv,  av);
    RT("perl_array_unshift",    voidTy, av, pv);
    /* string builtins */
    RT("perl_chomp",       i64,  pv);
    RT("perl_chomp_array", i64,  av);
    RT("perl_chop_array",  pv,   av);
    RT("perl_length",   pv,   pv);
    RT("perl_substr2",  pv,   pv, pv);
    RT("perl_substr3",  pv,   pv, pv, pv);
    RT("perl_join",     pv,   pv, av);
    RT("perl_split",    av,   pv, pv, i64);
    RT("perl_pack",           pv, pv, av);
    RT("perl_unpack",         pv, pv, pv);
    RT("perl_unpack_to_array", av, pv, pv);
    /* references */
    RT("perl_alloc_flat_array", pv, i64);  /* FLAT_ARRAY (Stage 22/23 path) */
    RT("perl_alloc_float_pair", pv, Type::getDoubleTy(ctx_), Type::getDoubleTy(ctx_));
    RT("perl_alloc_float_array", pv, i64);  /* FLAT_ARRAY n-element (used by FLAT_ARRAY literal path) */
    RT("perl_ref_scalar",   pv, pv);
    RT("perl_ref_array",    pv, av);
    RT("perl_ref_hash",     pv, av);  /* PerlHash* treated as opaque av */
    RT("perl_deref_scalar", pv, pv);
    RT("perl_deref_array",    av, pv);
    RT("perl_deref_array_ro", av, pv);
    RT("perl_deref_hash",   av, pv);  /* returns PerlHash* as opaque av */
    RT("perl_ref_type",     pv, pv);
    RT("perl_promote_ref_array",  voidTy, pv);  /* D105 */
    RT("perl_array_promote_refs", voidTy, av);  /* D105 */
    /* file I/O */
    RT("perl_open_fh",          pv,     pv, pv, pv);
    RT("perl_open2_fh",         pv,     pv, pv);
    RT("perl_close_fh",         voidTy, pv);
    RT("perl_readline",         pv,     pv);
    RT("perl_readline_all",     av,     pv);
    RT("perl_readline_stdin",   pv);
    RT("perl_readline_all_stdin", av);
    RT("perl_readline_argv",    pv);
    RT("perl_readline_all_argv", av);
    RT("perl_get_dollar_argv",  pv);
    RT("perl_set_data_section", voidTy, i8p, i64);
    RT("perl_get_data_fh",      pv);
    RT("perl_pv_flag_utf8",     voidTy, pv);
    RT("perl_print_fh",         voidTy, pv, pv);
    RT("perl_say_fh",           voidTy, pv, pv);
    RT("perl_printf_fh",        voidTy, pv, pv, av);
    RT("perl_eof_fh",           pv,     pv);
     RT("perl_die",              voidTy, pv, i8p, i32);
    /* perl_die never returns to its caller — it either longjmp's to an eval
       catch point (different basic block) or calls exit(1). */
    rtFuncs_["perl_die"]->addFnAttr(Attribute::NoReturn);
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
    RT("perl_warn",       voidTy, pv, i8p, i32);
    RT("perl_get_sig_hash", av);
    RT("perl_splice",     av, av, pv, pv, av);
    RT("perl_filetest",   pv, Type::getInt32Ty(ctx_), pv);
    RT("perl_env_get",    pv, pv);
    RT("perl_env_set",    voidTy, pv, pv);
    RT("perl_system",     pv, pv);
    RT("perl_syscall",    pv, pv);
    RT("perl_fork",              pv);
    RT("perl_wait_pid",          pv);
    RT("perl_waitpid",           pv, pv, pv);
    RT("perl_kill",              pv, av);
    RT("perl_exec",              pv, av);
    RT("perl_exit_n",            voidTy, pv);
    rtFuncs_["perl_exit_n"]->addFnAttr(Attribute::NoReturn);
    RT("perl_getppid_val",       pv);
    RT("perl_getuid_val",        pv);
    RT("perl_getgid_val",        pv);
    RT("perl_geteuid_val",       pv);
    RT("perl_getegid_val",       pv);
    RT("perl_setsid_val",        pv);
    RT("perl_getpgrp_val",       pv, pv);
    RT("perl_setpgrp_val",       pv, pv, pv);
    RT("perl_umask_val",         pv, pv);
    RT("perl_pipe_fh",           pv, pv, pv);
    RT("perl_socket_fh",         pv, pv, pv, pv, pv);
    RT("perl_bind_fh",           pv, pv, pv);
    RT("perl_listen_fh",         pv, pv, pv);
    RT("perl_connect_fh",        pv, pv, pv);
    RT("perl_accept_fh",         pv, pv, pv);
    RT("perl_send_fh",           pv, pv, pv, pv);
    RT("perl_recv_fh",           pv, pv, pv, pv, pv);
    RT("perl_shutdown_fh",       pv, pv, pv);
    RT("perl_getsockname_fh",    pv, pv);
    RT("perl_getpeername_fh",    pv, pv);
    RT("perl_sysopen_fh",        pv, pv, pv, pv, pv);
    RT("perl_sysread_fh",        pv, pv, pv, pv, pv);
    RT("perl_syswrite_fh",       pv, pv, pv, pv, pv);
    RT("perl_flock_fh",          pv, pv, pv);
    RT("perl_vec_get",           pv, pv, pv, pv);
    RT("perl_vec_set",           pv, pv, pv, pv, pv);
    RT("perl_select4",           pv, pv, pv, pv, pv);
    RT("perl_select_fh",         pv, pv);
    RT("perl_fcntl_fh",          pv, pv, pv, pv);
    RT("perl_ioctl_fh",          pv, pv, pv, pv);
    RT("perl_dup_fd",            pv, pv);
    RT("perl_dup2_fd",           pv, pv, pv);
    RT("perl_backtick",   pv, pv);
    RT("perl_init_argv",   av, Type::getInt32Ty(ctx_), i8p);
    RT("perl_get_dollar0", pv);
    RT("perl_make_code_ref",  pv, i8p);
    RT("perl_call_code_ref",  pv, pv, av);
    RT("perl_get_current_code_ref", pv);
    RT("perl_eval_pop",        voidTy);
    RT("perl_get_dollar_at",   pv);
    RT("perl_eval_push",       voidTy, i8p);
    RT("perl_eval_string",     pv, pv);
    RT("perl_eval_pad_begin",              voidTy);
    RT("perl_eval_pad_add_scalar",         voidTy, i8p, pv);
    RT("perl_eval_pad_add_array",          voidTy, i8p, av);
    RT("perl_eval_pad_add_hash",           voidTy, i8p, av);
    RT("perl_eval_pad_clear",              voidTy);
    RT("perl_eval_pad_pin",                voidTy);
    RT("perl_eval_pad_get_scalar",         pv, i8p);
    RT("perl_eval_pad_get_array",          av, i8p);
    RT("perl_eval_pad_get_hash",           av, i8p);
    RT("perl_eval_pad_get_scalar_or_undef", pv, i8p);
    RT("perl_eval_pad_get_array_or_new",   av, i8p);
    RT("perl_eval_pad_get_hash_or_new",    av, i8p);
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
    RT("perl_regex_subst_e",   i64, pv, i8p, i8p, i8p, av);
    RT("perl_capture",         pv,  i64);
    RT("perl_split_regex",     av,  i8p, i8p, pv, i64);
    /* OOP */
    RT("perl_bless",                   pv,     pv, pv);
    RT("perl_register_method",         voidTy, i8p, i8p);
    RT("perl_get_or_create_global_scalar", pv, i8p);
    RT("perl_glob_assign",     voidTy, i8p, pv);
    RT("perl_glob_set_scalar", voidTy, i8p, pv);
    RT("perl_glob_set_array",  voidTy, i8p, av);
    RT("perl_glob_set_hash",   voidTy, i8p, av); /* PerlHash* is also a ptr */
    RT("perl_glob_get_scalar", pv, i8p);
    RT("perl_glob_get_array",  av, i8p);
    RT("perl_glob_get_hash",   av, i8p); /* returns PerlHash* as ptr */
    RT("perl_glob_copy",       voidTy, i8p, i8p);
    RT("perl_dispatch_method",         pv,     pv, i8p, av);
    RT("perl_dispatch_method_sv",      pv,     pv, pv,  av);
    RT("perl_hash_pairs_array",        av,     pv);
    RT("perl_dispatch_method_super",   pv,     pv, i8p, i8p, av);
    RT("perl_set_isa",                 voidTy, i8p, i8p);
    RT("perl_register_overload",       voidTy, i8p, i8p, i8p);
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
    RT("perl_call_named_sub_checked", pv, i8p, av, Type::getInt32Ty(ctx_), i8p, i8p, Type::getInt32Ty(ctx_));
    RT("perl_xs_load_library",  pv, pv);
    RT("perl_xs_call_dynamic",  pv, pv, pv, pv, av);
    RT("perl_dl_load_file",     pv, pv, pv);
    RT("perl_dl_find_symbol",   pv, pv, pv);
    RT("perl_dl_install_xsub",  pv, pv, pv);
    RT("perl_dl_error",         pv);
    RT("perl_dl_bootstrap",     pv, pv, pv);
    RT("perl_dbi_connect",      pv, pv, pv, pv);
    RT("perl_local_restore_to", voidTy, Type::getInt32Ty(ctx_));
    /* special globals */
    RT("perl_get_input_sep",    pv);
    RT("perl_get_dollar_bang",  pv);
    RT("perl_push_wantarray", i32, i32);
RT("perl_pop_wantarray",  i32);
RT("perl_wantarray",             pv);
RT("perl_array_to_list_return",  pv, av);
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
    RT("perl_scalar_gmtime",  pv,   pv);
    RT("perl_scalar_localtime",pv,  pv);
    RT("perl_sleep_val",    pv,     pv);
    RT("perl_alarm_val",    pv,     pv);
    /* Time::HiRes (D30, built-in) */
    RT("perl_hires_time",               pv);
    RT("perl_hires_gettimeofday_list",  av);
    RT("perl_hires_gettimeofday_scalar",pv);
    RT("perl_hires_sleep",              pv, pv);
    RT("perl_hires_usleep",             pv, pv);
    RT("perl_hires_tv_interval",        pv, pv, pv);
    /* List::Util */
    RT("perl_sum_list",     pv,  av);
    RT("perl_min_list",     pv,  av);
    RT("perl_max_list",     pv,  av);
    RT("perl_uniq_list",    av,  av);
    /* sort with custom comparator — fn ptr passed as i8p (opaque pointer);
       third arg is the comparator's closure captures array (D61), may be
       an empty PerlArray*. */
    RT("perl_sort_custom",  av,  av, i8p, av);
    /* special globals (Tier 2) */
    RT("perl_get_dollar_dot",   pv);
    RT("perl_get_dollar_comma", pv);
    RT("perl_get_dollar_bsl",   pv);
    RT("perl_get_dollar_amp",   pv);
    RT("perl_get_dollar_question", pv);
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
    /* Getopt::Long */
    RT("perl_getopt_long",      pv, av, av);
    /* Data::Dumper */
    RT("perl_dumper",           pv, av, pv);
    /* File::Basename */
    RT("perl_basename",         pv, pv, av);
    RT("perl_dirname",          pv, pv);
    RT("perl_fileparse",        av, pv, av);
    /* Cwd / Sys::Hostname / File::Spec / Time::Local (Tier 1, native) */
    RT("perl_getcwd",           pv);
    RT("perl_abs_path",         pv, pv);
    RT("perl_realpath",         pv, pv);
    RT("perl_hostname",         pv);
    RT("perl_fpath_collect",    voidTy, av, pv);
    RT("perl_file_find",        pv, pv, av, i32);
    RT("perl_file_temp_template", pv, av, i32, i32);
    RT("perl_file_temp",        av, av, i32, i32);
    RT("perl_file_temp_tmpnam", pv);
    RT("perl_storable_dclone",  pv, pv);
    RT("perl_json_encode",      pv, pv, i64, i64);
    RT("perl_json_decode",      pv, pv);
    RT("perl_json_true",        pv);
    RT("perl_json_false",       pv);
    RT("perl_text_wrap",        pv, pv, pv, av, pv, pv, pv, pv, pv);
    RT("perl_text_fill",        pv, pv, pv, av, pv, pv, pv, pv, pv);
    RT("perl_make_path",        av, av, pv, i32);
    RT("perl_remove_tree",      av, av, pv, i32);
    RT("perl_fcopy",            pv, pv, pv, pv, pv, pv);
    RT("perl_fsyscopy",         pv, pv, pv, pv, pv, pv);
    RT("perl_fmove",            pv, pv, pv, pv, pv);
    RT("perl_fspec_canonpath",  pv, pv);
    RT("perl_fspec_catdir",     pv, av);
    RT("perl_fspec_catfile",    pv, av);
    RT("perl_fspec_splitpath",  av, pv, pv);
    RT("perl_fspec_splitdir",   av, pv);
    RT("perl_fspec_catpath",    pv, av);
    RT("perl_fspec_rel2abs",    pv, pv, pv);
    RT("perl_fspec_abs2rel",    pv, pv, pv);
    RT("perl_fspec_curdir",     pv);
    RT("perl_fspec_updir",      pv);
    RT("perl_fspec_rootdir",    pv);
    RT("perl_fspec_devnull",    pv);
    RT("perl_fspec_tmpdir",     pv);
    RT("perl_fspec_file_name_is_absolute", pv, pv);
    RT("perl_fspec_no_upwards", av, av);
    RT("perl_fspec_join",       pv, av);
    RT("perl_fspec_case_tolerant", pv);
    RT("perl_fspec_path",       av);
    RT("perl_timegm",           pv, av);
    RT("perl_timelocal",        pv, av);
    RT("perl_timegm_nocheck",   pv, av);
    RT("perl_timelocal_nocheck",pv, av);
    RT("perl_timegm_modern",    pv, av);
    RT("perl_timelocal_modern", pv, av);
    RT("perl_timegm_posix",     pv, av);
    RT("perl_timelocal_posix",  pv, av);
    RT("perl_native_constant",  pv, i8p);
    RT("perl_config_get",       pv, pv);
    RT("perl_config_exists",    Type::getInt32Ty(ctx_), pv);
    RT("perl_config_keys",      av);
    RT("perl_config_myconfig",  pv);
    RT("perl_config_configsh",  pv);
    RT("perl_config_config_vars", pv, av);
    RT("perl_config_config_re", pv, pv);
    RT("perl_env_exists",       Type::getInt32Ty(ctx_), pv);
    RT("perl_sysseek_fh",       pv, pv, pv, pv);
    RT("perl_make_qr",          pv, i8p, i8p);
    RT("perl_regex_match_sv",   pv, pv, pv, Type::getInt32Ty(ctx_));
    RT("perl_value_is_qr",      Type::getInt32Ty(ctx_), pv);
    RT("perl_regex_match_captures_list", av, pv, i8p, i8p);
    RT("perl_get_dollar_under", pv);
    RT("perl_deref_if_ref",     pv, pv);
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
                            "perl_array_len", "perl_deref_array_ro",
                            "perl_is_bigint_pv"}) {
        auto *F = getRTFunc(nm);
        F->setMemoryEffects(MemoryEffects::readOnly());
        F->addFnAttr(Attribute::NoUnwind);
        F->addFnAttr(Attribute::WillReturn);
    }
}

Function *CodeGen::getRTFunc(const std::string &nm) {
    auto it = rtFuncs_.find(nm);
    if (it != rtFuncs_.end()) return it->second;
    /* W22/W28: perl_to_string_dup (runtime.c:1772) and libc free are not
       in the RT() table (deliberately untouched for this fix), so they
       are declared on demand here — the central lookup is the one place
       every call site goes through, so a lazy declaration here covers
       all of them. */
    if (nm == "perl_to_string_dup") {
        auto *fn = Function::Create(
            makeRT(ctx_, PointerType::getUnqual(ctx_), {perlPtrTy_}),
            Function::ExternalLinkage, nm, mod_.get());
        rtFuncs_[nm] = fn;
        return fn;
    }
    if (nm == "free") {
        auto *fn = Function::Create(
            makeRT(ctx_, Type::getVoidTy(ctx_),
                          {PointerType::getUnqual(ctx_)}),
            Function::ExternalLinkage, nm, mod_.get());
        rtFuncs_[nm] = fn;
        return fn;
    }
    throw std::runtime_error("Unknown runtime function: " + nm);
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
    /* D112: a free variable reference inside a sub (no local `my` of its
       own) first tries ITS OWN package's qualified global before the bare
       name. Two different packages' same-named file-scope `my` (or `our`)
       variables are stored under distinct qualified keys (see case
       NK::My below) — without this, a sub in package Foo referencing an
       unqualified `$x` could resolve to package Bar's same-named
       file-scope variable purely because Bar's declaration happened to
       run first and claim the shared bare-name fallback slot. */
    if (!currentPackage_.empty()) {
        auto qit = fileScalarGlobals_.find(currentPackage_ + "::" + nm);
        if (qit != fileScalarGlobals_.end()) return qit->second;
    }
    auto git = fileScalarGlobals_.find(nm);
    if (git != fileScalarGlobals_.end()) return git->second;
    return nullptr;
}

static std::string globBareName(const std::string &nm) {
    std::string n = nm;
    if (!n.empty() && (n[0] == '$' || n[0] == '@' || n[0] == '%')) n = n.substr(1);
    return n;
}

/* D110: a fully-qualified variable name ("Other::h", "$Data::Dumper::x")
   is a true package global — the runtime's process-wide typeglob registry
   (perl_glob_get_*) is its single source of truth. Only qualified names
   route there; bare names keep the historical per-scope auto-vivify
   behavior unchanged. */
static bool isQualifiedName(const std::string &nm) {
    return nm.find("::") != std::string::npos;
}

bool CodeGen::isGlobName(const std::string &nm) const {
    std::string n = globBareName(nm);
    if (globNames_.count(n) || globNames_.count("main::" + n)) return true;
    auto pos = n.rfind("::");
    if (pos != std::string::npos) {
        std::string bare = n.substr(pos + 2);
        if (globNames_.count(bare) || globNames_.count("main::" + bare)) return true;
    }
    return false;
}

void CodeGen::declareVar(const std::string &nm, Value *a) {
    scopes_.back()[nm] = a;
}

void CodeGen::emitEvalPadFill() {
    callRT("perl_eval_pad_begin", {});
    std::unordered_set<std::string> addedS, addedA, addedH;
    auto addScalar = [&](const std::string &nm, Value *slot) {
        if (!slot || evalPadSkipName(nm) || addedS.count(nm)) return;
        Value *cell = builder_.CreateLoad(perlPtrTy_, slot, nm + ".epad");
        Value *key = builder_.CreateGlobalStringPtr(nm);
        callRT("perl_eval_pad_add_scalar", {key, cell});
        addedS.insert(nm);
    };
    auto addArray = [&](const std::string &nm, Value *av) {
        if (!av || nm.empty() || nm == "_" || addedA.count(nm)) return;
        Value *key = builder_.CreateGlobalStringPtr(nm);
        callRT("perl_eval_pad_add_array", {key, av});
        addedA.insert(nm);
    };
    auto addHash = [&](const std::string &nm, Value *hv) {
        if (!hv || nm.empty() || addedH.count(nm)) return;
        Value *key = builder_.CreateGlobalStringPtr(nm);
        callRT("perl_eval_pad_add_hash", {key, hv});
        addedH.insert(nm);
    };
    /* Inner scopes overwrite outer (addedS blocks a second add; walk inner first). */
    for (int i = (int)scopes_.size() - 1; i >= 0; i--)
        for (auto &kv : scopes_[i]) addScalar(kv.first, kv.second);
    for (auto &kv : fileScalarGlobals_) addScalar(kv.first, kv.second);
    for (int i = (int)arrayScopes_.size() - 1; i >= 0; i--)
        for (auto &kv : arrayScopes_[i]) addArray(kv.first, kv.second);
    for (auto &kv : fileArrayGlobals_) {
        Value *av = builder_.CreateLoad(perlPtrTy_, kv.second, kv.first + ".epad.av");
        addArray(kv.first, av);
    }
    for (int i = (int)hashScopes_.size() - 1; i >= 0; i--)
        for (auto &kv : hashScopes_[i]) addHash(kv.first, kv.second);
    for (auto &kv : fileHashGlobals_) {
        Value *hv = builder_.CreateLoad(perlPtrTy_, kv.second, kv.first + ".epad.hv");
        addHash(kv.first, hv);
    }
    /* Unboxed int/float: box a cell so the eval can read (and, with
       allNamesCaptured_, these scopes should be empty at eval sites). */
    for (int i = (int)intScopes_.size() - 1; i >= 0; i--) {
        for (auto &kv : intScopes_[i]) {
            if (evalPadSkipName(kv.first) || addedS.count(kv.first)) continue;
            Value *iv = builder_.CreateLoad(Type::getInt64Ty(ctx_), kv.second);
            Value *boxed = boxI64(iv);
            Value *key = builder_.CreateGlobalStringPtr(kv.first);
            callRT("perl_eval_pad_add_scalar", {key, boxed});
            addedS.insert(kv.first);
        }
    }
    for (int i = (int)floatScopes_.size() - 1; i >= 0; i--) {
        for (auto &kv : floatScopes_[i]) {
            if (evalPadSkipName(kv.first) || addedS.count(kv.first)) continue;
            Value *fv = builder_.CreateLoad(Type::getDoubleTy(ctx_), kv.second);
            Value *boxed = boxF64(fv);
            Value *key = builder_.CreateGlobalStringPtr(kv.first);
            callRT("perl_eval_pad_add_scalar", {key, boxed});
            addedS.insert(kv.first);
        }
    }
}

void CodeGen::emitEvalPadBind() {
    for (auto &nm : evalPadScalars_) {
        auto git = fileScalarGlobals_.find(nm);
        if (git == fileScalarGlobals_.end()) continue;
        Value *key = builder_.CreateGlobalStringPtr(nm);
        Value *cell = callRT("perl_eval_pad_get_scalar_or_undef", {key});
        builder_.CreateStore(cell, git->second);
        declareVar(nm, git->second);
    }
    for (auto &nm : evalPadArrays_) {
        auto git = fileArrayGlobals_.find(nm);
        if (git == fileArrayGlobals_.end()) continue;
        Value *key = builder_.CreateGlobalStringPtr(nm);
        Value *av = callRT("perl_eval_pad_get_array_or_new", {key});
        builder_.CreateStore(av, git->second);
    }
    for (auto &nm : evalPadHashes_) {
        auto git = fileHashGlobals_.find(nm);
        if (git == fileHashGlobals_.end()) continue;
        Value *key = builder_.CreateGlobalStringPtr(nm);
        Value *hv = callRT("perl_eval_pad_get_hash_or_new", {key});
        builder_.CreateStore(hv, git->second);
    }
    if (!subs_.empty())
        callRT("perl_eval_pad_pin", {});
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
    /* D112: see the identical package-qualified-first lookup in lookupVar. */
    if (!currentPackage_.empty()) {
        auto qit = fileArrayGlobals_.find(currentPackage_ + "::" + nm);
        if (qit != fileArrayGlobals_.end())
            return builder_.CreateLoad(perlPtrTy_, qit->second, nm);
    }
    auto git = fileArrayGlobals_.find(nm);
    if (git != fileArrayGlobals_.end())
        return builder_.CreateLoad(perlPtrTy_, git->second, nm);
    /* D110: an already-qualified name (@Other::arr) is a true global —
       exact-qualified storage first, then the runtime glob registry.
       Must precede isGlobName: glob_get's bare-name fallback would
       otherwise attach @Other::arr to whatever entry the bare "arr"
       name first created. */
    if (isQualifiedName(nm)) {
        auto qgit = fileArrayGlobals_.find(nm);
        if (qgit != fileArrayGlobals_.end())
            return builder_.CreateLoad(perlPtrTy_, qgit->second, nm);
        Value *qkey = builder_.CreateGlobalStringPtr(nm);
        return callRT("perl_glob_get_array", {qkey});
    }
    if (isGlobName(nm)) {
        Value *key = builder_.CreateGlobalStringPtr(globBareName(nm));
        return callRT("perl_glob_get_array", {key});
    }
    return nullptr;
}

void CodeGen::declareArray(const std::string &nm, Value *ptr) {
    arrayScopes_.back()[nm] = ptr;
}

Value *CodeGen::lookupHash(const std::string &nm) {
    /* D49: bare %SIG is the process-wide special hash (unless shadowed by my %SIG) */
    for (int i = (int)hashScopes_.size() - 1; i >= 0; i--) {
        auto it = hashScopes_[i].find(nm);
        if (it != hashScopes_[i].end()) return it->second;
    }
    /* D112: see the identical package-qualified-first lookup in lookupVar. */
    if (!currentPackage_.empty()) {
        auto qit = fileHashGlobals_.find(currentPackage_ + "::" + nm);
        if (qit != fileHashGlobals_.end())
            return builder_.CreateLoad(perlPtrTy_, qit->second, nm);
    }
    auto git = fileHashGlobals_.find(nm);
    if (git != fileHashGlobals_.end())
        return builder_.CreateLoad(perlPtrTy_, git->second, nm);
    if (nm == "SIG")
        return callRT("perl_get_sig_hash", {});
    /* D110: an already-qualified name (%Other::h) is a true global —
       identical shape to lookupArray's qualified branch above. */
    if (isQualifiedName(nm)) {
        auto qgit = fileHashGlobals_.find(nm);
        if (qgit != fileHashGlobals_.end())
            return builder_.CreateLoad(perlPtrTy_, qgit->second, nm);
        Value *qkey = builder_.CreateGlobalStringPtr(nm);
        return callRT("perl_glob_get_hash", {qkey});
    }
    if (isGlobName(nm)) {
        Value *key = builder_.CreateGlobalStringPtr(globBareName(nm));
        return callRT("perl_glob_get_hash", {key});
    }
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
    /* x in array/list context (D95):
       Real Perl only does *list* repetition when the left operand is
       syntactically a parenthesized list or qw// (both parse as ArrayLit).
       A bare `f() x 2` / `"ab" x 2` is always *string* repetition, even
       when the whole expression is the RHS of an array assignment — the
       result is a single-element list holding the repeated string.
       Previously every `x` here was treated as list repetition, giving
       `@a = (f() x 2)` → `("x","x")` instead of real Perl's `("xx")`. */
    if (n.kind == NK::BinOp && n.sval == "x" && n.left) {
        if (n.left->kind == NK::ArrayLit) {
            Value *srcArr = emitArrayPtr(*n.left);
            if (!srcArr) srcArr = callRT("perl_array_new", {});
            Value *nv = emitExpr(*n.right);
            return callRT("perl_repeat_list", {srcArr, nv});
        }
        /* String repetition → one-element array of the repeated string.
           Left operand of string-x is always scalar context. */
        int savedCallCtx = callCtx_;
        callCtx_ = 0;
        Value *lv = emitExpr(*n.left);
        callCtx_ = savedCallCtx;
        Value *nv = emitExpr(*n.right);
        Value *rep = callRT("perl_repeat_str", {lv, nv});
        freeIfOwned(lv);
        freeIfOwned(nv);
        Value *av = callRT("perl_array_new", {});
        callRT("perl_array_push", {av, rep});
        freeIfOwned(rep);
        return av;
    }
    if (n.kind == NK::ArrayVar) {
        return lookupArray(n.name);
    }
    /* D111: a bare %hash in list context flattens to a (key, value, ...)
       list — e.g. `my %c = %h;`, `my @flat = %h;`, `(%defaults,
       %overrides)`. Previously unhandled here, so every one of those
       fell through to this function returning null, which callers then
       treated as "not list-shaped" and re-evaluated %h in *scalar*
       context (its key count) as a single list element instead — wrong
       contents, not just a missed optimization. Mirrors the already-
       correct `perl_array_extend_hash` flattening `flattenArgInto` uses
       for `foo(%h)` sub-call arguments. */
    if (n.kind == NK::HashVar) {
        Value *hv = lookupHash(n.name);
        Value *av = callRT("perl_array_new", {});
        if (hv) callRT("perl_array_extend_hash", {av, hv});
        return av;
    }
    if (n.kind == NK::DerefHash) {
        Value *ref = emitExpr(*n.left);
        Value *hv  = callRT("perl_deref_hash", {ref});
        freeIfOwned(ref);
        Value *av = callRT("perl_array_new", {});
        callRT("perl_array_extend_hash", {av, hv});
        return av;
    }
    if (n.kind == NK::KeysFunc) {
        Value *av;
        /* keys %Config (list context) — native special hash */
        if (n.name == "Config" || n.name == "Config::Config")
            return callRT("perl_config_keys", {});
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
        /* D118: optional 3rd LIMIT argument, stored in n.args[0] if given. */
        Value *limit = n.args.empty()
            ? ConstantInt::get(Type::getInt64Ty(ctx_), 0, true)
            : callRT("perl_to_int", {emitExpr(*n.args[0])});
        if (n.ival) {
            Value *pat = builder_.CreateGlobalStringPtr(n.sval, "sp_pat");
            Value *flg = builder_.CreateGlobalStringPtr(n.name, "sp_flg");
            return callRT("perl_split_regex", {pat, flg, str, limit});
        }
        Value *sep = n.left  ? emitExpr(*n.left)  : perlStr(" ");
        return callRT("perl_split", {sep, str, limit});
    }
    /* unpack(FORMAT, EXPR) in list context — D67 */
    if (n.kind == NK::UnpackFunc) {
        Value *str = emitExpr(*n.left);
        Value *fmt = emitExpr(*n.args[0]);
        return callRT("perl_unpack_to_array", {fmt, str});
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
    /* localtime / gmtime in list context → 9-element array.
       No-arg localtime()/gmtime() means "now" — pass undef so the runtime
       uses time(NULL); a plain perlUndef() constant evaluated at compile
       time as the argument would freeze this call's epoch. */
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
            std::string cmpName = "__sort_cmp_" + std::to_string(sortCmpCounter_++);

            /* D61: closure-capture support for the comparator, mirroring
               AnonSub's Phase 1 (collectAllScalarNames + "capture every
               visible array/hash") — done here, in the *caller's* scope,
               before cmpFn's own scope reset below wipes it out. Without
               this, the comparator (compiled as a genuinely separate LLVM
               function so it can be passed as a real C function pointer to
               qsort()) previously had no way to see or modify ANY outer
               block-scoped variable at all (only a true file-scope one,
               via fileScalarGlobals_) — e.g. `push @observed, $a` inside
               the comparator silently no-opped when @observed was merely
               block-scoped. $a/$b themselves are excluded here since
               they're bound separately below (as the function's own
               parameters, or via the D28 file-scope-shadow check). */
            std::set<std::string> sortUsedNames;
            collectAllScalarNames(*n.body, sortUsedNames);
            std::vector<std::string> sortCaptureNames;
            std::vector<Value*>      sortCaptureVals;
            std::vector<char>        sortCaptureSigils;
            for (auto &nm : sortUsedNames) {
                if (nm == "_" || nm == "a" || nm == "b") continue;
                if (auto *slot = lookupVar(nm)) {
                    sortCaptureNames.push_back(nm);
                    sortCaptureVals.push_back(builder_.CreateLoad(perlPtrTy_, slot));
                    sortCaptureSigils.push_back('$');
                } else if (Value *ia = lookupIntVar(nm)) {
                    Value *ival = builder_.CreateLoad(Type::getInt64Ty(ctx_), ia);
                    Value *boxed = boxI64(ival);
                    auto *pvAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, nm + ".boxed");
                    builder_.CreateStore(boxed, pvAlloca);
                    sortCaptureNames.push_back(nm);
                    sortCaptureVals.push_back(builder_.CreateLoad(perlPtrTy_, pvAlloca));
                    sortCaptureSigils.push_back('$');
                } else if (Value *fa = lookupFloatVar(nm)) {
                    Value *fval = builder_.CreateLoad(Type::getDoubleTy(ctx_), fa);
                    Value *boxed = boxF64(fval);
                    auto *pvAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, nm + ".boxed");
                    builder_.CreateStore(boxed, pvAlloca);
                    sortCaptureNames.push_back(nm);
                    sortCaptureVals.push_back(builder_.CreateLoad(perlPtrTy_, pvAlloca));
                    sortCaptureSigils.push_back('$');
                }
            }
            {
                std::unordered_map<std::string, Value*> visibleArrays;
                for (auto &scope : arrayScopes_)
                    for (auto &kv : scope) visibleArrays[kv.first] = kv.second;
                for (auto &kv : visibleArrays) {
                    if (kv.first == "_") continue;
                    sortCaptureNames.push_back(kv.first);
                    sortCaptureVals.push_back(callRT("perl_ref_array", {kv.second}));
                    sortCaptureSigils.push_back('@');
                }
                std::unordered_map<std::string, Value*> visibleHashes;
                for (auto &scope : hashScopes_)
                    for (auto &kv : scope) visibleHashes[kv.first] = kv.second;
                for (auto &kv : visibleHashes) {
                    sortCaptureNames.push_back(kv.first);
                    sortCaptureVals.push_back(callRT("perl_ref_hash", {kv.second}));
                    sortCaptureSigils.push_back('%');
                }
            }

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

            /* D61: re-materialize the captures collected above via
               perl_get_capture(idx) — same pattern AnonSub bodies use. */
            for (size_t ci = 0; ci < sortCaptureNames.size(); ci++) {
                Value *pv = callRT("perl_get_capture",
                                   {ConstantInt::get(i64Ty, (long long)ci)});
                if (sortCaptureSigils[ci] == '@') {
                    declareArray(sortCaptureNames[ci], callRT("perl_deref_array_ro", {pv}));
                } else if (sortCaptureSigils[ci] == '%') {
                    declareHash(sortCaptureNames[ci], callRT("perl_deref_hash", {pv}));
                } else {
                    auto *capAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, sortCaptureNames[ci]);
                    builder_.CreateStore(pv, capAlloca);
                    declareVar(sortCaptureNames[ci], capAlloca);
                }
            }

            /* bind $a and $b to the function parameters — UNLESS an
               outer, file-scope `my $a`/`my $b` already exists (D28:
               matches real Perl's well-known "my $a used in sort
               comparison" footgun). sort's/reduce's $a/$b are
               dynamically-aliased *package* variables in real Perl; an
               earlier lexical `my $a` in an enclosing scope permanently
               shadows the package variable's name for all later code in
               that scope, including a comparator block compiled after
               it — the block ends up reading whatever the outer lexical
               holds (unchanging across comparisons) instead of the pair
               actually being compared, producing a broken/non-monotonic
               sort. This comparator body is compiled as its own,
               separate LLVM function with a full scope reset just above,
               so this check can only reach as far as a *file-scope*
               shadow (via fileScalarGlobals_, untouched by that reset,
               checked here before it would otherwise be re-declared) —
               an outer *sub-scoped* my $a would need closure-capture
               machinery this comparator doesn't have, and remains a
               narrower, separate, open gap (TESTS.md's D28 entry). The
               function still receives its two arguments unconditionally
               either way — the sort algorithm needs them regardless of
               whether the block's own code ends up referencing them. */
            bool aShadowed = fileScalarGlobals_.count("a") != 0;
            bool bShadowed = fileScalarGlobals_.count("b") != 0;
            Value *argA = cmpFn->getArg(0); argA->setName("a");
            Value *argB = cmpFn->getArg(1); argB->setName("b");
            auto *aAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, "a");
            auto *bAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, "b");
            builder_.CreateStore(argA, aAlloca);
            builder_.CreateStore(argB, bAlloca);
            if (!aShadowed) declareVar("a", aAlloca);
            if (!bShadowed) declareVar("b", bAlloca);

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

            /* call perl_sort_custom(av, cmpFn, capsAv) — capsAv built from
               the sortCaptureVals collected in the caller's own scope
               above (D61), now that we're back in that scope. */
            Value *fnPtr = builder_.CreateBitCast(cmpFn,
                              PointerType::getUnqual(ctx_));
            Value *capsAv = callRT("perl_array_new", {});
            for (auto *cv : sortCaptureVals)
                callRT("perl_array_push_capture", {capsAv, cv});
            return callRT("perl_sort_custom", {av, fnPtr, capsAv});
        }
        else if (mode == "subname" && !n.name.empty()) {
            /* sort SUBNAME LIST — named comparator sub, no braces. Unlike
               the `custom` block above (whose body is inlined directly
               into the wrapper comparator function, so $a/$b can just be
               local allocas in that same function), SUBNAME is a
               separately-compiled top-level function with its own fresh
               scope — it can only see $a/$b if they're file-scope globals,
               matching Perl's actual semantics ($a/$b are package globals
               for the duration of a sort). */
            auto *fn = mod_->getFunction(subLLVMName(n.name));
            if (!fn) { /* sub not found (e.g. forward reference) — fall back */
                Value *copy = callRT("perl_sort_str_asc", {av}); return copy;
            }
            auto ensureGlobalScalar = [&](const std::string &nm) -> Value * {
                auto it = fileScalarGlobals_.find(nm);
                if (it != fileScalarGlobals_.end()) return it->second;
                auto *gv = new GlobalVariable(*mod_, perlPtrTy_, false,
                    GlobalValue::InternalLinkage, Constant::getNullValue(perlPtrTy_), "g." + nm);
                builder_.CreateStore(perlUndef(), gv);
                fileScalarGlobals_[nm] = gv;
                return gv;
            };
            Value *gA = ensureGlobalScalar("a");
            Value *gB = ensureGlobalScalar("b");

            static int sortCmpSubCounter = 0;
            std::string cmpName = "__sort_cmp_sub_" + std::to_string(sortCmpSubCounter++);
            auto *i64TySn = Type::getInt64Ty(ctx_);
            auto *cmpFTsn = FunctionType::get(i64TySn, {perlPtrTy_, perlPtrTy_}, false);
            auto *cmpFnSn = Function::Create(cmpFTsn, Function::InternalLinkage,
                                             cmpName, mod_.get());

            auto *savedFnSn = currentFn_;
            auto *savedBBsn = builder_.GetInsertBlock();

            auto *cmpEntrySn = BasicBlock::Create(ctx_, "entry", cmpFnSn);
            builder_.SetInsertPoint(cmpEntrySn);
            currentFn_ = cmpFnSn;

            Value *argASn = cmpFnSn->getArg(0);
            Value *argBSn = cmpFnSn->getArg(1);
            Value *aCell = builder_.CreateLoad(perlPtrTy_, gA, "a.cell");
            Value *bCell = builder_.CreateLoad(perlPtrTy_, gB, "b.cell");
            callRT("perl_assign", {aCell, argASn});
            callRT("perl_assign", {bCell, argBSn});

            /* SUBNAME() is called with no args (real Perl: $a/$b are read
               as package globals inside the sub, not passed via @_), in
               scalar context (a comparator returns one number). */
            Value *emptyArgsSn = callRT("perl_array_new", {});
            auto *i32TySn = Type::getInt32Ty(ctx_);
            Value *scalarCtxSn = ConstantInt::get(i32TySn, 0);
            Value *cmpCallResult = builder_.CreateCall(fn, {emptyArgsSn, scalarCtxSn});
            callRT("perl_array_free", {emptyArgsSn});
            Value *rvSn = callRT("perl_to_int", {cmpCallResult});
            freeIfOwned(cmpCallResult);
            builder_.CreateRet(rvSn);

            currentFn_ = savedFnSn;
            builder_.SetInsertPoint(savedBBsn);

            Value *fnPtrSn = builder_.CreateBitCast(cmpFnSn, PointerType::getUnqual(ctx_));
            Value *noCapsSn = callRT("perl_array_new", {});
            return callRT("perl_sort_custom", {av, fnPtrSn, noCapsSn});
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
        if (n.sval.empty() || n.sval == "ARGV")
            return callRT("perl_readline_all_argv", {});
        if (n.sval == "STDIN")
            return callRT("perl_readline_all_stdin", {});
        if (n.sval == "DATA") {
            Value *fh = callRT("perl_get_data_fh", {});
            return callRT("perl_readline_all", {fh});
        }
        if (auto *slot = lookupVar(n.sval)) {
            Value *fh = builder_.CreateLoad(perlPtrTy_, slot);
            return callRT("perl_readline_all", {fh});
        }
        if (isGlobName(n.sval) || !n.sval.empty()) {
            globNames_.insert(n.sval);
            Value *key = builder_.CreateGlobalStringPtr(n.sval);
            Value *fh = callRT("perl_glob_get_scalar", {key});
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
            bool mapPairsDone = false;
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
                if (n.left->kind == NK::AnonHash) {
                    /* `map { $_ => 1 } @list` — the braces are the hash
                       constructor itself (parser put it in n.left): real
                       Perl yields the key/value PAIRS in list context. */
                    Value *hvPv = emitExpr(*n.left);
                    Value *pairs = callRT("perl_hash_pairs_array", {hvPv});
                    callRT("perl_array_extend", {resultArr, pairs});
                    mapPairsDone = true;
                } else {
                    mapAv = emitArrayPtr(*n.left);
                    if (!mapAv) mapPv = emitExpr(*n.left);
                }
            }
            /* Clone scalar result before popScope() frees scope variables it may
               reference (e.g. last expr is a ScalarVar whose alloca is being freed). */
            if (mapPv && !llvm::isa<llvm::ConstantPointerNull>(mapPv)) {
                Value *orig = mapPv;
                mapPv = callRT("perl_clone", {mapPv});
                freeIfOwned(orig);
            }
            popScope();
            if (mapPairsDone)  /* pairs already extended */
                ;
            else if (mapAv)      callRT("perl_array_extend", {resultArr, mapAv});
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
    /* List-context non-/g match: the CAPTURE LIST (empty = no match),
       exactly like real perl's `my @m = ($str =~ /pat/);`. RegexMatchExpr
       with a QR operand also lands here (`my @m = ($str =~ $re);`).
       NOTE: !~ is excluded — real perl's !~ returns a plain boolean even
       in list context (never a capture list). */
    if (((n.kind == NK::RegexMatch && n.name.find('g') == std::string::npos) ||
         n.kind == NK::RegexMatchExpr) && !n.ival) {
        Value *str = emitExpr(*n.left);
        if (n.kind == NK::RegexMatch) {
            Value *pat = builder_.CreateGlobalStringPtr(n.sval, "rmc_pat");
            Value *flg = builder_.CreateGlobalStringPtr(n.name, "rmc_flg");
            return callRT("perl_regex_match_captures_list", {str, pat, flg});
        }
        int savedCtxRmc = callCtx_;
        callCtx_ = -1;
        Value *patPv = emitExpr(*n.right);
        callCtx_ = savedCtxRmc;
        if (!rtFuncs_.count("perl_to_string_dup"))
            rtFuncs_["perl_to_string_dup"] = Function::Create(
                makeRT(ctx_, PointerType::getUnqual(ctx_), {perlPtrTy_}),
                Function::ExternalLinkage, "perl_to_string_dup", mod_.get());
        Function *freeFnRmc = rtFuncs_.count("free")
            ? rtFuncs_["free"]
            : (rtFuncs_["free"] = Function::Create(
                   makeRT(ctx_, Type::getVoidTy(ctx_),
                          {PointerType::getUnqual(ctx_)}),
                   Function::ExternalLinkage, "free", mod_.get()));
        /* QR operand: use its pattern/flags directly via accessors. */
        Value *isQr = callRT("perl_value_is_qr", {patPv});
        auto *curFnRmc = builder_.GetInsertBlock()->getParent();
        auto *qrBB = BasicBlock::Create(ctx_, "rmc.qr", curFnRmc);
        auto *strBB = BasicBlock::Create(ctx_, "rmc.str", curFnRmc);
        auto *phiBB = BasicBlock::Create(ctx_, "rmc.phi", curFnRmc);
        builder_.CreateCondBr(
            builder_.CreateICmpNE(
                isQr, ConstantInt::get(Type::getInt32Ty(ctx_), 0), "rmc.qrchk"),
            qrBB, strBB);
        builder_.SetInsertPoint(qrBB);
        if (!rtFuncs_.count("perl_qr_pattern")) {
            rtFuncs_["perl_qr_pattern"] = Function::Create(
                makeRT(ctx_, PointerType::getUnqual(ctx_), {perlPtrTy_}),
                Function::ExternalLinkage, "perl_qr_pattern", mod_.get());
            rtFuncs_["perl_qr_flags"] = Function::Create(
                makeRT(ctx_, PointerType::getUnqual(ctx_), {perlPtrTy_}),
                Function::ExternalLinkage, "perl_qr_flags", mod_.get());
        }
        Value *qpat = builder_.CreateCall(rtFuncs_["perl_qr_pattern"], {patPv});
        Value *qflg = builder_.CreateCall(rtFuncs_["perl_qr_flags"], {patPv});
        Value *qrList = callRT("perl_regex_match_captures_list", {str, qpat, qflg});
        builder_.CreateBr(phiBB);
        builder_.SetInsertPoint(strBB);
        Value *patC = builder_.CreateCall(getRTFunc("perl_to_string_dup"), {patPv});
        Value *flgE = builder_.CreateGlobalStringPtr("", "rmc_flg");
        Value *strList = callRT("perl_regex_match_captures_list", {str, patC, flgE});
        builder_.CreateCall(freeFnRmc, {patC});
        builder_.CreateBr(phiBB);
        builder_.SetInsertPoint(phiBB);
        auto *phi = builder_.CreatePHI(perlPtrTy_, 2, "rmc.av");
        phi->addIncoming(qrList, qrBB);
        phi->addIncoming(strList, strBB);
        freeIfOwned(str);
        freeIfOwned(patPv);
        return phi;
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
        /* D114: an index subscript entry can itself be list-shaped (a
           Range like `1..2`, an array variable like `@i`) rather than a
           single scalar index — mirrors HashSlice's pushHashKey dispatch
           just below, which already handles the equivalent `@h{@k}` case
           correctly. */
        auto pushArraySliceIdx = [&](const Node &idxNode) {
            if (Value *idxAv = emitArrayPtr(idxNode)) {
                Value *slice = av ? callRT("perl_array_slice", {av, idxAv})
                                   : callRT("perl_array_new", {});
                callRT("perl_array_extend", {res, slice});
            } else {
                Value *elem = av ? callRT("perl_array_get_ref", {av, emitIdx(idxNode)}) : perlUndef();
                callRT("perl_array_push", {res, elem});
            }
        };
        for (auto &idxNode : n.args) pushArraySliceIdx(*idxNode);
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
    /* D81: delete @hash{...} / delete @arr[...] in list context */
    if (n.kind == NK::DeleteFunc &&
        (n.sval == "hash_slice" || n.sval == "array_slice")) {
        Value *res = callRT("perl_array_new", {});
        if (n.sval == "hash_slice") {
            Value *hv = lookupHash(n.name);
            auto delKey = [&](const Node &keyNode) {
                if (!hv) {
                    callRT("perl_array_push", {res, perlUndef()});
                    return;
                }
                if (keyNode.kind == NK::ArrayLit) {
                    for (auto &k : keyNode.args) {
                        Value *old = emitHashDelete(hv, *k);
                        callRT("perl_array_push", {res, old});
                        freeIfOwned(old);
                    }
                } else if (keyNode.kind == NK::StringLit) {
                    /* D137 companion: qw(...) key specs now spread into
                       individual string-key args at parse time (parser.cpp
                       HashSlice), so delete @h{qw(a b)} reaches here as
                       one Str arg per key — each deletes its own entry.
                       (Before D137 the whole qw was ONE parse element that
                       this branch saw as a non-list, non-string node and
                       silently deleted nothing.) */
                    Value *old = emitHashDelete(hv, keyNode);
                    callRT("perl_array_push", {res, old});
                    freeIfOwned(old);
                } else if (Value *kav = emitArrayPtr(keyNode)) {
                    Value *lenV = callRT("perl_array_len", {kav});
                    Value *len = callRT("perl_to_int", {lenV});
                    freeIfOwned(lenV);
                    /* loop over keys at runtime would need a block; expand
                       common ArrayLit/qw path above — dynamic @keys falls
                       through one-by-one via a small unrolled helper: */
                    auto *i64Ty = Type::getInt64Ty(ctx_);
                    auto *fn = builder_.GetInsertBlock()->getParent();
                    auto *idxA = builder_.CreateAlloca(i64Ty, nullptr, "del.i");
                    builder_.CreateStore(ConstantInt::get(i64Ty, 0), idxA);
                    auto *condBB = BasicBlock::Create(ctx_, "del.cond", fn);
                    auto *bodyBB = BasicBlock::Create(ctx_, "del.body", fn);
                    auto *doneBB = BasicBlock::Create(ctx_, "del.done", fn);
                    builder_.CreateBr(condBB);
                    builder_.SetInsertPoint(condBB);
                    Value *i = builder_.CreateLoad(i64Ty, idxA);
                    builder_.CreateCondBr(builder_.CreateICmpSLT(i, len), bodyBB, doneBB);
                    builder_.SetInsertPoint(bodyBB);
                    Value *k = callRT("perl_array_get", {kav, i});
                    Value *old = callRT("perl_hash_delete_sv", {hv, k});
                    callRT("perl_array_push", {res, old});
                    freeIfOwned(k); freeIfOwned(old);
                    builder_.CreateStore(builder_.CreateAdd(i, ConstantInt::get(i64Ty, 1)), idxA);
                    builder_.CreateBr(condBB);
                    builder_.SetInsertPoint(doneBB);
                } else {
                    Value *old = emitHashDelete(hv, keyNode);
                    callRT("perl_array_push", {res, old});
                    freeIfOwned(old);
                }
            };
            for (auto &k : n.args) delKey(*k);
        } else {
            Value *av = lookupArray(n.name);
            auto delIdx = [&](const Node &idxNode) {
                if (!av) {
                    callRT("perl_array_push", {res, perlUndef()});
                    return;
                }
                if (idxNode.kind == NK::ArrayLit) {
                    for (auto &ix : idxNode.args) {
                        Value *idx = emitIdx(*ix);
                        Value *old = callRT("perl_array_delete", {av, idx});
                        callRT("perl_array_push", {res, old});
                        freeIfOwned(old);
                    }
                } else if (Value *iav = emitArrayPtr(idxNode)) {
                    auto *i64Ty = Type::getInt64Ty(ctx_);
                    auto *fn = builder_.GetInsertBlock()->getParent();
                    Value *lenV = callRT("perl_array_len", {iav});
                    Value *len = callRT("perl_to_int", {lenV});
                    freeIfOwned(lenV);
                    auto *idxA = builder_.CreateAlloca(i64Ty, nullptr, "dela.i");
                    builder_.CreateStore(ConstantInt::get(i64Ty, 0), idxA);
                    auto *condBB = BasicBlock::Create(ctx_, "dela.cond", fn);
                    auto *bodyBB = BasicBlock::Create(ctx_, "dela.body", fn);
                    auto *doneBB = BasicBlock::Create(ctx_, "dela.done", fn);
                    builder_.CreateBr(condBB);
                    builder_.SetInsertPoint(condBB);
                    Value *i = builder_.CreateLoad(i64Ty, idxA);
                    builder_.CreateCondBr(builder_.CreateICmpSLT(i, len), bodyBB, doneBB);
                    builder_.SetInsertPoint(bodyBB);
                    Value *idxPv = callRT("perl_array_get", {iav, i});
                    Value *idx = callRT("perl_to_int", {idxPv});
                    freeIfOwned(idxPv);
                    Value *old = callRT("perl_array_delete", {av, idx});
                    callRT("perl_array_push", {res, old});
                    freeIfOwned(old);
                    builder_.CreateStore(builder_.CreateAdd(i, ConstantInt::get(i64Ty, 1)), idxA);
                    builder_.CreateBr(condBB);
                    builder_.SetInsertPoint(doneBB);
                } else {
                    Value *idx = emitIdx(idxNode);
                    Value *old = callRT("perl_array_delete", {av, idx});
                    callRT("perl_array_push", {res, old});
                    freeIfOwned(old);
                }
            };
            for (auto &ix : n.args) delIdx(*ix);
        }
        return res;
    }
    if (n.kind == NK::ArrayLit) {
        Value *res = callRT("perl_array_new", {});
        for (auto &elem : n.args) {
            Value *sub = emitArrayPtr(*elem);
            if (sub) {
                callRT("perl_array_extend", {res, sub});
            } else {
                /* Flatten LIST_RESULT (e.g. `(0 || listret())` inside parens —
                   D94 + D95: single-element parens are ArrayLit, not unwrapped).
                   D105: no promotion needed here — a ScalarVar `elem` already
                   comes back promoted from emitExpr(ScalarVar) itself. */
                Value *v = emitExpr(*elem);
                callRT("perl_array_push_list_or_scalar", {res, v});
            }
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
    /* Time::HiRes::gettimeofday in list context: (seconds, microseconds).
       Must be checked before the generic user-sub Call handling below,
       since it's a builtin name-dispatch (see emitExpr), not a real
       LLVM-declared sub — mod_->getFunction() would never find it. */
    if (n.kind == NK::Call &&
        (n.name == "Time::HiRes::gettimeofday" || n.name == "gettimeofday")) {
        return callRT("perl_hires_gettimeofday_list", {});
    }
    /* File::Basename::fileparse in list context: (name, path, suffix).
       Same reasoning as gettimeofday above — must be intercepted before
       the generic user-sub Call handling below. */
    if (n.kind == NK::Call &&
        (n.name == "File::Basename::fileparse" || n.name == "fileparse")) {
        Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        Value *suf = callRT("perl_array_new", {});
        for (size_t k = 1; k < n.args.size(); k++)
            callRT("perl_array_push", {suf, emitExpr(*n.args[k])});
        return callRT("perl_fileparse", {path, suf});
    }
    /* File::Spec::splitpath in list context: (volume, directories, file).
       Same builtin-name-dispatch reasoning — intercepted before the
       generic user-sub Call handling below. */
    if (n.kind == NK::Call &&
        (n.name == "File::Spec::splitpath" || n.name == "File::Spec::Unix::splitpath" ||
         n.name == "splitpath")) {
        Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        Value *nof  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        return callRT("perl_fspec_splitpath", {path, nof});
    }
    if (n.kind == NK::Call &&
        (n.name == "File::Spec::splitdir" || n.name == "File::Spec::Unix::splitdir" ||
         n.name == "splitdir")) {
        Value *dir = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_fspec_splitdir", {dir});
    }
    if (n.kind == NK::Call &&
        (n.name == "File::Spec::no_upwards" || n.name == "File::Spec::Unix::no_upwards" ||
         n.name == "no_upwards")) {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            Value *sub = emitArrayPtr(*a);
            if (sub) callRT("perl_array_extend", {av, sub});
            else     callRT("perl_array_push",   {av, emitExpr(*a)});
        }
        return callRT("perl_fspec_no_upwards", {av});
    }
    if (n.kind == NK::Call &&
        (n.name == "File::Spec::path" || n.name == "File::Spec::Unix::path" ||
         n.name == "path")) {
        return callRT("perl_fspec_path", {});
    }
    /* File::Path in list context (@dirs = make_path(...)) — the created
       paths / per-dir removal counts reach emitArrayPtr, which must
       return the PerlArray itself (same reasoning as uniq below). */
    if (n.kind == NK::Call &&
        (n.name == "File::Path::make_path" || n.name == "make_path" ||
         n.name == "File::Path::mkpath"    || n.name == "mkpath" ||
         n.name == "File::Path::remove_tree" || n.name == "remove_tree" ||
         n.name == "File::Path::rmtree"    || n.name == "rmtree")) {
        std::vector<Value*> raw;
        for (auto &a : n.args) raw.push_back(emitExpr(*a));
        Value *opts = perlUndef();
        auto *i32TyP = Type::getInt32Ty(ctx_);
        callRT("perl_push_call_frame",
            {builder_.CreateGlobalStringPtr(currentPackage_),
             builder_.CreateGlobalStringPtr(sourceFile_),
             ConstantInt::get(i32TyP, n.line)});
        Value *dirs = callRT("perl_array_new", {});
        for (size_t k = 0; k < raw.size(); k++)
            callRT("perl_fpath_collect", {dirs, raw[k]});
        Value *r;
        if (n.name == "File::Path::make_path" || n.name == "make_path" ||
            n.name == "File::Path::mkpath"    || n.name == "mkpath")
            r = callRT("perl_make_path",
                       {dirs, opts, ConstantInt::get(i32TyP, 1)});
        else
            r = callRT("perl_remove_tree",
                       {dirs, opts, ConstantInt::get(i32TyP, 1)});
        callRT("perl_pop_call_frame", {});
        return r;
    }
    /* File::Temp list context: my ($fh, $name) = tempfile(...) — the
       (fh, name) pair reaches emitArrayPtr, which must return the raw
       PerlArray (same convention as make_path/uniq). mkstemp likewise
       returns (fh, name) in list context. */
    if (n.kind == NK::Call &&
        (n.name == "File::Temp::tempfile" || n.name == "tempfile")) {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) callRT("perl_array_push", {av, emitExpr(*a)});
        return callRT("perl_file_temp",
                      {av, ConstantInt::get(Type::getInt32Ty(ctx_), 1),
                       ConstantInt::get(Type::getInt32Ty(ctx_), 1)});
    }
    if (n.kind == NK::Call &&
        (n.name == "File::Temp::mkstemp" || n.name == "mkstemp")) {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) callRT("perl_array_push", {av, emitExpr(*a)});
        return callRT("perl_file_temp_template",
                      {av, ConstantInt::get(Type::getInt32Ty(ctx_), 0),
                       ConstantInt::get(Type::getInt32Ty(ctx_), 1)});
    }
    /* D69: List::Util::uniq in list context. Same reasoning as
       Time::HiRes::gettimeofday just above — "List::Util::uniq" only
       reaches here as a qualified NK::Call (the bare "uniq" keyword form
       is its own dedicated NK::UniqFunc node, handled elsewhere in this
       function), and it's a builtin name-dispatch, not a real
       LLVM-declared sub, so it must be intercepted before the generic
       user-sub Call handling below (which previously let it fall through
       to mod_->getFunction() finding nothing and silently returning an
       empty list). sum/min/max don't need a list-context case here — they
       always return a single scalar, even when called in list context. */
    if (n.kind == NK::Call && n.name == "List::Util::uniq") {
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
    /* eval EXPR / eval { BLOCK } in list context: force wantarray, unwrap. */
    if (n.kind == NK::EvalBlock || (n.kind == NK::Call && n.name == "eval")) {
        int saved = callCtx_;
        callCtx_ = 1;
        Value *pv = emitExpr(n);
        callCtx_ = saved;
        return callRT("perl_unwrap_list_return", {pv});
    }
    /* user-defined sub call in list context: call with ctx=1, unwrap result */
    if (n.kind == NK::Call) {
        if (auto *fn = mod_->getFunction(subLLVMName(n.name))) {
            Value *argsArr = callRT("perl_array_new", {});
            fillCallArgs(argsArr, n);
            auto *i32Ty = Type::getInt32Ty(ctx_);
            Value *one = ConstantInt::get(i32Ty, 1);
            Value *pv = builder_.CreateCall(fn, {argsArr, one});
            callRT("perl_array_free", {argsArr});
            return callRT("perl_unwrap_list_return", {pv});
        }
    }
    /* code ref call in list context: unwrap LIST_RESULT */
    if (n.kind == NK::CallCodeRef) {
        Value *ref = emitExpr(*n.left);
        Value *av  = callRT("perl_array_new", {});
        for (auto &arg : n.args) {
            Value *src = emitArrayPtr(*arg);
            if (src) callRT("perl_array_extend", {av, src});
            else     callRT("perl_array_push",   {av, emitExpr(*arg)});
        }
        auto *i32Ty = Type::getInt32Ty(ctx_);
        Value *one = ConstantInt::get(i32Ty, 1);
        callRT("perl_push_wantarray", {one});
        Value *result = callRT("perl_call_code_ref", {ref, av});
        callRT("perl_pop_wantarray", {});
        callRT("perl_array_free", {av});
        return callRT("perl_unwrap_list_return", {result});
    }
    return nullptr;
}

Value *CodeGen::perlStr(const std::string &s) {
    auto *gv = builder_.CreateGlobalString(s, ".str");
    /* D85: interior NULs (pack, "\0", "\x00") must keep their byte length. */
    return callRT("perl_alloc_string_len",
                  {gv, ConstantInt::get(Type::getInt64Ty(ctx_), (long long)s.size())});
}

/* Returns an i8* global string constant if n is a compile-time string literal,
 * otherwise nullptr. Used to bypass PerlValue key creation for hash ops. */
static llvm::Value *constKeyPtr(const Node &n, llvm::IRBuilder<> &builder) {
    if (n.kind == NK::StringLit || n.kind == NK::IntLit)
        return builder.CreateGlobalStringPtr(
            n.kind == NK::IntLit ? std::to_string(n.ival) : n.sval, ".hk");
    return nullptr;
}

/* True iff an ArrowDeref chain is rooted in a %hash/@array element
 * (e.g. $h{a}{b}{c}, $a[0]{x}[1]) rather than a bare scalar/ref expression
 * (e.g. $ref->[0][1]). Distinguishes "needs recursive autoviv through
 * perl_(hash|array)_autoviv_*" (the only path that knows how to create a
 * missing intermediate level) from "chain already bottoms out at a real
 * ref/FLAT_ARRAY value that just needs a normal, non-autovivifying deref"
 * (which must keep using the FLAT_ARRAY-aware fallback — the autoviv_*
 * runtime helpers only recognize the PERL_REF_ARRAY/PERL_REF_HASH tags and
 * would silently blow away an existing FLAT_ARRAY-tagged inner array). */
static bool isElemRootedChain(const Node &n) {
    if (n.kind == NK::HashElem || n.kind == NK::ArrayElem) return true;
    if (n.kind == NK::ArrowDeref) return isElemRootedChain(*n.left);
    return false;
}

/* D50: is `n` a chain rooted in a bare scalar variable holding a ref
 * (e.g. `$ref->{a}{b}`), where *every* level is a hash-key ArrowDeref?
 * Safe to autovivify all the way down via emitAutovivContainer() iff so
 * — an array-index level anywhere in the chain is deliberately excluded
 * (conservatively kept on the older, non-autovivifying fallback path),
 * since perl_array_autoviv_array()/perl_array_autoviv_hash() only
 * recognize the PERL_REF_ARRAY/PERL_REF_HASH tags and would silently
 * destroy an existing FLAT_ARRAY-tagged element if one were present at
 * that array level — exactly the regression already identified and
 * avoided for the element-rooted case (isElemRootedChain, D40). A hash
 * has no FLAT_ARRAY-equivalent optimization, so an all-hash-key chain
 * carries none of that risk regardless of how deep it goes. */
static bool isScalarRootedAllHashChain(const Node &n) {
    if (n.kind == NK::ScalarVar) return true;
    if (n.kind == NK::ArrowDeref) return n.sval == "hash" && isScalarRootedAllHashChain(*n.left);
    return false;
}

/* D50: is `n` a chain rooted in a bare scalar variable that contains
 * at least one array-index ArrowDeref level?  Unlike
 * isScalarRootedAllHashChain (which requires *all* levels to be
 * hash-key), this accepts chains like `$ref->[0][1]`, `$ref->[0]{k}`,
 * `$ref->{a}[0]`, etc.  The caller must use the new _from_scalar
 * runtime helpers (which handle FLAT_ARRAY safely) instead of the
 * plain perl_array_autoviv_* functions. */
static bool isScalarRootedChainWithArrayIndex(const Node &n) {
    if (n.kind == NK::ScalarVar) return false;
    if (n.kind == NK::ArrowDeref) {
        bool hasArray = (n.sval == "array");
        return hasArray || isScalarRootedChainWithArrayIndex(*n.left);
    }
    return false;
}

/* D50: is `n` a chain that contains at least one array-index ArrowDeref
 * level and is NOT purely element-rooted (HashElem/ArrayElem base)?
 * This catches chains like `$ref->{a}[0]`, `$ref->[0][1]`, etc.
 * that need the _from_scalar autoviv helpers. */
static bool needsScalarRootedAutoviv(const Node &n) {
    /* Check if chain has any array-index level */
    bool hasArrayIndex = false;
    const Node *cur = &n;
    while (cur->kind == NK::ArrowDeref) {
        if (cur->sval == "array") hasArrayIndex = true;
        cur = cur->left.get();
    }
    if (!hasArrayIndex) return false;
    /* Check if chain is NOT purely element-rooted (HashElem/ArrayElem base) */
    if (cur->kind == NK::HashElem || cur->kind == NK::ArrayElem) {
        /* Element-rooted chains are handled by existing isElemRootedChain */
        return false;
    }
    /* Scalar-var rooted or other: needs our special handling */
    return true;
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
        "perl_alloc_bool",
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
        /* method/sub dispatch always returns a freshly cloned PerlValue*.
           D97: in-place mutator methods (bmul/badd/bsub on Math::BigInt)
           return the same PV as the receiver — the codegen frees it, but
           perl_assign has already cloned it into the variable, so the
           variable's clone is unaffected.  The chaining $z1->bmul(...)->badd(...)
           works because bmul returns $z1 (self), and badd is called on that
           same PV before freeIfOwned runs (the expression tree is evaluated
           left-to-right, and freeIfOwned only runs after the full expression). */
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

Value *CodeGen::emitFlooredMod(Value *lv, Value *rv) {
    /* Perl's %, unlike C's, uses floored-division semantics: the result
       always has the same sign as the right operand (or is zero) — e.g.
       -7 % 3 == 2, 7 % -3 == -2. LLVM's SRem (like C's %) truncates toward
       zero instead, so a nonzero result with a sign mismatch against rv
       needs rv added back to floor it. Branchless via select, mirroring
       perl_mod's boxed-value equivalent in runtime.c.

       D84: Zero-divisor check is handled by a runtime helper that calls
       perl_die (eval-catchable) instead of exit(1). */
    return callRT("perl_mod_i64", {lv, rv});
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
        /* W1: bitwise ops. Real Perl prints any result with bit 63 set as an
           UNSIGNED value (e.g. ~12 → 18446744073709551603); the runtime has
           no UV storage, so variable-operand bitwise ops stay on the boxed
           path (pre-existing gap). Constant operands are exact-foldable:
           emitExprI64 checks the result and returns nullptr when bit 63 is
           set. */
        if ((n.sval == "&" || n.sval == "|" || n.sval == "^") &&
            n.left && n.left->kind == NK::IntLit &&
            n.right && n.right->kind == NK::IntLit)
            return true;
        /* W1: `>>` with a constant count in [1,63] is a logical shift of the
           bit pattern; bit 63 is always cleared, so the i64 result is
           always non-negative and matches Perl's UV semantics exactly.
           Count 0 keeps bit 63 (UV territory); variable/negative counts can
           set bit 63 (via Perl's `>> -n == << n` rule) — boxed path. */
        if (n.sval == ">>" &&
            n.right && n.right->kind == NK::IntLit &&
            n.right->ival >= 1 && n.right->ival <= 63 &&
            n.left && canEmitI64(*n.left))
            return true;
        /* W1: `<<` is constant-foldable only. Variable bases can
           produce UV results (bit 63 set: `1 << 63` prints 9223372036854775808,
           not -9223372036854775808) — emitExprI64
           returns nullptr in those cases and callers fall back. */
        if (n.sval == "<<" &&
            n.left && n.left->kind == NK::IntLit &&
            n.right && n.right->kind == NK::IntLit)
            return true;
        /* W1: `**` constant folding — must mirror real Perl's pp_pow storage
           class (perl 5.44 pp.c): the result prints as an integer only when
           Perl stores it as IV/UV:
             - |a| a power of 2 (incl. 0, 1): Perl computes an exact NV; it
               prints as integer only while |a^b| < 1e15 (15 sig digits) —
               for powers of 2 that is 2^(m*b) with m*b <= 49.
             - else: exact UV math only when b*bitlen(|a|) <= 64; printing as
               i64 additionally needs result < 2^63 (no UV storage in perlc).
           All other cases are NV (pow()) — must NOT fold to i64. */
        if (n.sval == "**" &&
            n.left && n.left->kind == NK::IntLit &&
            n.right && n.right->kind == NK::IntLit) {
            long long av = n.left->ival, bv = n.right->ival;
            if (bv < 0) return false; /* NV: 1/a^b */
            unsigned long long ab = av < 0 ? (unsigned long long)(-av)
                                           : (unsigned long long)av;
            if (ab == 0 || ab == 1) return true; /* 0/1 -> 0 or 1 */
            int m = 0; { unsigned long long t = ab; while (t >>= 1) m++; }
            if ((ab & (ab - 1)) == 0)
                return (long long)m * bv <= 49; /* pow-of-2: 2^(m*b) < 2^50 */
            /* non-pow-of-2: Perl keeps IV/UV only while e*bitlen(|a|) <= 64
               (bitlen = m+1); beyond that it is an NV pow(). */
            if ((long long)bv * (long long)(m + 1) > 64) return false; /* NV pow */
            return true; /* emit checks result < 2^63 */
        }
        return false;
    }
    case NK::UnaryOp:
        /* W1: `~` constant-fold only (variable result can set bit 63 → UV). */
        return (n.sval == "~" && n.left && n.left->kind == NK::IntLit) ||
               (n.sval == "-" && n.left && canEmitI64(*n.left));
    case NK::AbsFunc:
        /* W1: constant-fold only — abs(INT64_MIN) is UV 2^63 in real Perl,
           and the branchless negate would wrap. */
        return n.left && n.left->kind == NK::IntLit;
    case NK::IntFunc:
        /* W1: int() on an integer is the identity — no representation change,
           safe with variable operands. */
        return n.left && canEmitI64(*n.left);
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
        /* W1: `& | ^` constant folding — exact in i64; results with bit 63
           set are UV in real Perl (no UV storage in the runtime) → nullptr. */
        if ((n.sval == "&" || n.sval == "|" || n.sval == "^") &&
            n.left && n.left->kind == NK::IntLit &&
            n.right && n.right->kind == NK::IntLit) {
            unsigned long long lb = (unsigned long long)n.left->ival;
            unsigned long long rb = (unsigned long long)n.right->ival;
            unsigned long long r = (n.sval == "&") ? (lb & rb)
                                : (n.sval == "|") ? (lb | rb) : (lb ^ rb);
            if (r >> 63) return nullptr; /* UV result */
            return ConstantInt::get(i64, (long long)r);
        }
        /* W1: `>>` with constant count [1,63] — logical shift of the bit
           pattern; bit 63 is cleared, result always matches Perl's UV form. */
        if (n.sval == ">>" &&
            n.right && n.right->kind == NK::IntLit &&
            n.right->ival >= 1 && n.right->ival <= 63 &&
            canEmitI64(*n.left)) {
            Value *lv = emitExprI64(*n.left);
            if (!lv) return nullptr;
            return builder_.CreateLShr(lv, ConstantInt::get(i64, n.right->ival), "bshr");
        }
        /* W1: `<<` / `**` constant folding with real Perl's exact semantics:
           << : count clamped to [0,63] (else 0); result with bit 63 set is a
               UV — not representable as i64, return nullptr → boxed fallback.
           ** : mirror pp_pow's storage class (see canEmitI64) — fold to i64
               only where real Perl prints an integer; exact UV result via
               128-bit squaring, nullptr when NV (pow) or UV >= 2^63. */
        if ((n.sval == "<<" || n.sval == "**") &&
            n.left && n.left->kind == NK::IntLit &&
            n.right && n.right->kind == NK::IntLit) {
            long long b = n.left->ival, e = n.right->ival;
            if (n.sval == "<<") {
                if (e < 0 || e > 63) return ConstantInt::get(i64, 0);
                __uint128_t r = (__uint128_t)(unsigned long long)b << e;
                if (r >= ((__uint128_t)1) << 63) return nullptr; /* UV result */
                return ConstantInt::get(i64, (long long)r);
            }
            if (e < 0) return nullptr; /* NV: 1/b^e */
            unsigned long long ab = b < 0 ? (unsigned long long)(-b)
                                          : (unsigned long long)b;
            if (ab == 0) return ConstantInt::get(i64, e == 0 ? 1 : 0);
            int m = 0; { unsigned long long t = ab; while (t >>= 1) m++; }
            if (ab == 1)
                return ConstantInt::get(i64, (b < 0 && (e & 1)) ? -1 : 1);
            if ((ab & (ab - 1)) == 0) {
                /* power-of-2 base: Perl stores NV; integer print only while
                   2^(m*e) < 1e15, i.e. m*e <= 49. */
                if ((long long)m * e > 49) return nullptr;
                unsigned long long r = 1ULL << (m * e);
                return ConstantInt::get(i64,
                    (b < 0 && (e & 1)) ? -(long long)r : (long long)r);
            }
            /* non-pow-of-2: Perl keeps IV/UV only while e*bitlen(|b|) <= 64
               (bitlen = m+1); beyond that it is an NV pow(). */
            if ((long long)e * (long long)(m + 1) > 64) return nullptr; /* NV pow */
            unsigned __int128 res = 1, base = ab;
            for (unsigned long long ee = e; ee; ee >>= 1) {
                if (ee & 1) res *= base;
                if (ee > 1) base *= base;
            }
            if (res >= ((unsigned __int128)1) << 63) return nullptr; /* UV */
            unsigned long long rv = (unsigned long long)res;
            return ConstantInt::get(i64,
                (b < 0 && (e & 1)) ? -(long long)rv : (long long)rv);
        }
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
        return emitFlooredMod(lv, rv);
    }
    case NK::UnaryOp:
        if (n.sval == "-" && n.left && canEmitI64(*n.left)) {
            Value *v = emitExprI64(*n.left);
            return v ? builder_.CreateNeg(v, "ineg") : nullptr;
        }
        /* W1: `~` constant folding (variable result can set bit 63 → UV). */
        if (n.sval == "~" && n.left && n.left->kind == NK::IntLit) {
            long long r = ~(long long)n.left->ival;
            if (r < 0) return nullptr; /* UV result */
            return ConstantInt::get(i64, r);
        }
        return nullptr;
    case NK::AbsFunc: {
        /* W1: constant folding (abs(INT64_MIN) is UV 2^63 in real Perl). */
        if (!n.left || n.left->kind != NK::IntLit) return nullptr;
        long long v = n.left->ival;
        if (v == INT64_MIN) return nullptr;
        return ConstantInt::get(i64, v < 0 ? -v : v);
    }
    case NK::IntFunc: {
        /* W1: int() on an integer is the identity. */
        if (!n.left || !canEmitI64(*n.left)) return nullptr;
        return emitExprI64(*n.left);
    }
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

/* D135: true iff 'nm' is ever used in a position where holding a plain
   double (instead of a real PerlValue*) is observably different — string
   interpolation/concat, ref-taking (\$x), as a call argument, key/index,
   blessed-object positions, etc. Structurally identical to floatSafe but
   its bare ScalarVar case returns TRUE (a plain read is fine for a float
   var: emitExpr(ScalarVar) boxes on demand) instead of floatSafe's
   inNum-gated false. Used by `case NK::My`'s unbox branch to decide
   whether the float-alloca path is safe when the body leaves the integer
   domain: if the var is never used in a non-numeric position, the float
   alloca is observably equivalent. Conservative on every unrecognized
   node kind (returns false → boxed PV path). */
static bool floatVarUseSafe(const Node &n, const std::string &nm) {
    if (n.kind == NK::ScalarVar && n.name == nm)
        return true; /* plain read — boxing on demand is always correct */

    switch (n.kind) {
    case NK::BinOp: {
        /* string ops and non-numeric ops with nm anywhere → unsafe */
        bool isNum = (n.sval=="+"||n.sval=="-"||n.sval=="*"||n.sval=="/"||
                      n.sval=="**"||n.sval=="%"||n.sval=="<"||n.sval=="<="
                      ||n.sval==">"||n.sval==">="||n.sval=="=="||n.sval=="!=");
        if (!isNum && (hasVar(n, nm))) return false;
        bool ok = true;
        if (n.left)  ok = ok && floatVarUseSafe(*n.left,  nm);
        if (n.right) ok = ok && floatVarUseSafe(*n.right, nm);
        return ok;
    }
    case NK::CompoundAssign: {
        /* CompoundAssign nodes store the BARE operator (sval "/" for /=
           etc. — see parseCompoundAssign). += -= *= /= **= %= are numeric;
           anything else (. |= &= etc.) on nm needs the real PV. */
        bool isNum = (n.sval=="+"||n.sval=="-"||n.sval=="*"||n.sval=="/"||
                      n.sval=="**"||n.sval=="%");
        if (!isNum && n.left && n.left->kind == NK::ScalarVar &&
            n.left->name == nm)
            return false; /* .=, |=, etc. on nm need the real PV */
        bool ok = true;
        if (n.left)  ok = ok && floatVarUseSafe(*n.left,  nm);
        if (n.right) ok = ok && floatVarUseSafe(*n.right, nm);
        return ok;
    }
    default: {
        /* Assign and every other node kind: a bare assignment target or
           plain read is fine; anything the default walk reaches is
           checked positionally. */
        bool ok = true;
        if (n.left)  ok = ok && floatVarUseSafe(*n.left,  nm);
        if (n.right) ok = ok && floatVarUseSafe(*n.right, nm);
        for (auto &a : n.args) ok = ok && floatVarUseSafe(*a, nm);
        if (n.body)  ok = ok && floatVarUseSafe(*n.body,  nm);
        if (n.init)  ok = ok && floatVarUseSafe(*n.init,  nm);
        if (n.cond)  ok = ok && floatVarUseSafe(*n.cond,  nm);
        if (n.step)  ok = ok && floatVarUseSafe(*n.step,  nm);
        for (auto &b : n.branches) {
            if (b.cond) ok = ok && floatVarUseSafe(*b.cond, nm);
            if (b.body) ok = ok && floatVarUseSafe(*b.body, nm);
        }
        return ok;
    }
    }
}

/* D135: static "is this RHS guaranteed to leave the integer domain
   unchanged" test — conservative in the SAFE direction for int-promotion
   (returns true = possibly-fractional unless the expression is provably
   int-only). An IntLit is int-only; so is a BinOp over `+ - * %` whose
   operands are both int-shaped (covers `$x = 2*3+1`-style literal
   arithmetic). StringLits, FloatLits, Call/MethodCall results,
    ScalarVar reads of other variables (we can't prove their type here),
    coercions (`"7.25"+0` — a StringLit operand), and anything else are
    treated as possibly-fractional: real Perl would store whatever the
    value is, so an i64 alloca is only safe when the RHS is statically
    int-only. Everything else → treat as float-shaped (skip int-promotion;
    the boxed PV or float path stores the true value). */

/* D135: the set of scan-root variables that are provably int-only —
   every write (Assign/CompoundAssign direct target) has an int-shaped
   RHS, and any initializer is int-shaped. Computed to a fixpoint:
   start with vars whose writes' RHS are int-only WITHOUT variable
   operands (IntLits, literal arithmetic), then repeatedly admit vars
   whose RHS operands are all IntLits or vars already in the set.
   ScalarVar operands count as int-shaped iff the name is in the set
   (or the name is the variable being defined — a cycle through itself,
   e.g. `$i += $i`, stays int if all its other writes are). The classic
   hot idiom `my $s = 0; for (...) { $s += $i; }` keeps its i64 alloca
   because $i (a foreach counter, or itself an int-only var) is in the
   set. */
struct D135IntSet {
    std::set<std::string> names;

    bool count(const std::string &nm) const { return names.count(nm) != 0; }
};

/* collect every ScalarVar name appearing anywhere in the tree (walks the
   same child fields as the other D135 scanners) */
static void d135CollectVarNames(const Node &n, std::set<std::string> &out) {
    if (n.kind == NK::ScalarVar && !n.name.empty()) {
        std::string nm = n.name;
        if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
        out.insert(nm);
    }
    if (n.left)  d135CollectVarNames(*n.left,  out);
    if (n.right) d135CollectVarNames(*n.right, out);
    for (auto &a : n.args) d135CollectVarNames(*a, out);
    if (n.body)  d135CollectVarNames(*n.body,  out);
    if (n.init)  d135CollectVarNames(*n.init,  out);
    if (n.cond)  d135CollectVarNames(*n.cond,  out);
    if (n.step)  d135CollectVarNames(*n.step,  out);
    for (auto &b : n.branches) {
        if (b.cond) d135CollectVarNames(*b.cond, out);
        if (b.body) d135CollectVarNames(*b.body, out);
    }
}

/* one fixpoint iteration: would 'nm' be int-only under the given set? */

static bool rhsIsIntShapedCtx(const Node &n, const D135IntSet &cur,
                              const std::set<std::string> &selfOk) {
    switch (n.kind) {
    case NK::IntLit:
        return true;
    case NK::ScalarVar: {
        if (n.name.empty()) return false;
        std::string nm = n.name;
        if (nm[0] == '$') nm = nm.substr(1);
        return cur.count(nm) || selfOk.count(nm);
    }
    case NK::BinOp: {
        if (!(n.sval == "+" || n.sval == "-" || n.sval == "*" || n.sval == "%"))
            return false;
        return n.left && n.right && rhsIsIntShapedCtx(*n.left, cur, selfOk) &&
               rhsIsIntShapedCtx(*n.right, cur, selfOk);
    }
    case NK::UnaryOp:
        return n.sval == "-" && n.left && rhsIsIntShapedCtx(*n.left, cur, selfOk);
    case NK::CompoundAssign: {
        /* bare op stored ("+" for +=): + - * % int-preserve only when the
           RHS is int-shaped too; / and ** always leave the int domain */
        if (n.sval == "+" || n.sval == "-" || n.sval == "*" || n.sval == "%")
            return !n.right || rhsIsIntShapedCtx(*n.right, cur, selfOk);
        return false;
    }
    default:
        return false;
    }
}

/* does a write to 'nm' with this RHS keep the var int-only under 'cur'? */
static bool d135WriteKeepsInt(const Node &rhs, const D135IntSet &cur,
                              const std::string &nm,
                              const std::set<std::string> &selfOk) {
    std::set<std::string> self = selfOk;
    self.insert(nm); /* self-reference stays int as long as other writes agree */
    return rhsIsIntShapedCtx(rhs, cur, self);
}

/* walks the root finding every direct write to 'nm'; returns false the
   moment any write's RHS would leave the integer domain under 'cur' */
static bool d135CheckVarWrites(const Node &n, const std::string &nm,
                               const D135IntSet &cur) {
    switch (n.kind) {
    case NK::Assign: {
        if (n.left && n.left->kind == NK::ScalarVar && n.left->name == nm)
            return n.right && rhsIsIntShapedCtx(*n.right, cur, {nm});
        break;
    }
    case NK::CompoundAssign: {
        if (n.left && n.left->kind == NK::ScalarVar && n.left->name == nm) {
            /* bare op stored: + - * % keep int iff RHS int-shaped; / ** never */
            if (n.sval == "+" || n.sval == "-" || n.sval == "*" || n.sval == "%")
                return n.right && rhsIsIntShapedCtx(*n.right, cur, {nm});
            if (n.sval == "/" || n.sval == "**") return false;
            /* .= and other non-numeric compounds need the real PV */
            return false;
        }
        break;
    }
    default:
        break;
    }
    bool ok = true;
    if (n.left)  ok = ok && d135CheckVarWrites(*n.left,  nm, cur);
    if (n.right) ok = ok && d135CheckVarWrites(*n.right, nm, cur);
    for (auto &a : n.args) ok = ok && d135CheckVarWrites(*a, nm, cur);
    if (n.body)  ok = ok && d135CheckVarWrites(*n.body,  nm, cur);
    if (n.init)  ok = ok && d135CheckVarWrites(*n.init,  nm, cur);
    if (n.cond)  ok = ok && d135CheckVarWrites(*n.cond,  nm, cur);
    if (n.step)  ok = ok && d135CheckVarWrites(*n.step,  nm, cur);
    for (auto &b : n.branches) {
        if (b.cond) ok = ok && d135CheckVarWrites(*b.cond, nm, cur);
        if (b.body) ok = ok && d135CheckVarWrites(*b.body, nm, cur);
    }
    return ok;
}

/* build the int-only set for a scan root by fixpoint iteration */
static D135IntSet d135ComputeIntSet(const Node &root) {
    std::set<std::string> all;
    d135CollectVarNames(root, all);
    D135IntSet cur;
    for (int round = 0; round < 32; round++) {
        D135IntSet next = cur;
        bool changed = false;
        for (const auto &nm : all) {
            if (cur.count(nm)) continue;
            if (d135CheckVarWrites(root, nm, cur)) {
                next.names.insert(nm);
                changed = true;
            }
        }
        cur = next;
        if (!changed) break;
    }
    return cur;
}

/* D135: true iff 'nm' is ever the LHS/element target of an assignment or
   CompoundAssign whose RHS is *float-shaped* — i.e. NOT statically
   int-only (see rhsIsIntShapedCtx above: FloatLit/StringLit/call results/
   `"7.25"+0`-style coercions all count; IntLits and int-literal
   arithmetic stay int-shaped, and ScalarVar operands count iff the name
   is in the scan root's provably-int-only fixpoint set, so pure-int
   bodies keep the i64 fast path) — or is incremented/decremented (++, --, +=, -=, ++/--)
   somewhere in the body
   (real Perl's numeric ++/-- treat an NV holding variable exactly and can
   carry a fractional part through, so an int-promoted alloca is not
   observably equivalent for those). Used by `case NK::My`'s unbox branch
   to refuse int-promoting a sub-scoped `my $x = <int literal>` whose name
   later leaves the integer domain — the int-var Assign fallback
   truncates such an RHS via perl_to_int, silently holding 5 after
   `$x = 5.5` (D135). Scans exactly the child fields hasVar/
   needsFloatPrec walk (left/right/args/body/init/cond/step/branches); a
   RHS that merely *reads* 'nm' does not count — only writes to it.
   A variable RHS ($x = $y) counts as int-shaped iff $y is itself in the
   scan root's provably-int-only set (D135's fixpoint analysis) — so the
   classic `my $s = 0; for (...) { $s += $i; }` counter keeps its i64
   alloca, while `$x = $maybeFloat` correctly forces the non-int path. */
static bool assignsFloatLikeRhs(CodeGen &cg, const Node &n, const std::string &nm,
                                const D135IntSet *intSet);

static bool assignsFloatLikeRhs(CodeGen &cg, const Node &n, const std::string &nm,
                                const D135IntSet *intSet) {
    switch (n.kind) {
    case NK::Assign: {
        /* nm as the direct assignment target */
        if (n.left && n.left->kind == NK::ScalarVar && n.left->name == nm)
            return n.right && !rhsIsIntShapedCtx(*n.right, *intSet, {});
        /* nm inside a list-assignment LHS: its RHS pairing partner is
           positional (same index), so a same-name RHS element would
           self-pair (safe — no truncation); anything else paired with it
           is conservatively treated as float-shaped (it is a write into
           nm whose RHS shape is not statically known here). */
        if (n.left && n.left->kind == NK::ArrayLit) {
            if (n.right && n.right->kind == NK::ArrayLit) {
                for (size_t i = 0; i < n.left->args.size() && i < n.right->args.size(); i++) {
                    const Node &le = *n.left->args[i];
                    if (le.kind == NK::ScalarVar && le.name == nm) {
                        const Node &re = *n.right->args[i];
                        if (re.kind != NK::ScalarVar || re.name != nm)
                            return true;
                    }
                }
                /* fall through to the generic walk below for the rest */
            }
        }
        break;
    }
    case NK::CompoundAssign: {
        if (n.left && n.left->kind == NK::ScalarVar && n.left->name == nm) {
            /* NOTE: CompoundAssign nodes store the BARE operator in sval
               ("/" for /=, "+" for +=, "**" for **= — see parser.cpp's
               parseCompoundAssign). `/` and `**` always produce NV-shaped
               results (1/2 == 0.5, 2**-1) — real Perl stores the fraction,
               so an int alloca would truncate it; + / - carry a fraction
               when the RHS is float-shaped (a float literal, or an
               expression that isn't statically int-only); ++/-- on a
               fractional value must stay fractional (handled by NK::UnaryOp
               below — bare ++/-- has no RHS here). Int-shaped + / - of a
               counter are int-preserving in real Perl, so they alone do
               NOT force float (an all-int counter must keep its i64
               alloca); when the var separately leaves the integer domain
               the float alloca handles it via the numeric path. */
            if (n.sval == "/" || n.sval == "**") return true;
            if (n.sval == "+" || n.sval == "-")
                return n.right && !rhsIsIntShapedCtx(*n.right, *intSet, {});
        }
        break;
    }
    default:
        break;
    }
    bool r = false;
    if (n.left)  r = r || assignsFloatLikeRhs(cg, *n.left,  nm, intSet);
    if (n.right) r = r || assignsFloatLikeRhs(cg, *n.right, nm, intSet);
    for (auto &a : n.args) r = r || assignsFloatLikeRhs(cg, *a, nm, intSet);
    if (n.body)  r = r || assignsFloatLikeRhs(cg, *n.body,  nm, intSet);
    if (n.init)  r = r || assignsFloatLikeRhs(cg, *n.init,  nm, intSet);
    if (n.cond)  r = r || assignsFloatLikeRhs(cg, *n.cond,  nm, intSet);
    if (n.step)  r = r || assignsFloatLikeRhs(cg, *n.step,  nm, intSet);
    for (auto &b : n.branches) {
        if (b.cond) r = r || assignsFloatLikeRhs(cg, *b.cond, nm, intSet);
        if (b.body) r = r || assignsFloatLikeRhs(cg, *b.body, nm, intSet);
    }
    return r;
}

/* D135: true iff 'nm' is *read* in a context where a fractional value
   must survive (float arithmetic, or any non-integer context
   floatSafe's conservative walk would misclassify a genuinely-fractional
   value in) somewhere in the sub body. Mirrors needsFloatPrec's
   structure, extended to float-shaped reads needsFloatPrec itself
   doesn't recognize: the float-var fast-path Assign/CompoundAssign
   branches, which perl_to_int the value. The `my`-declaration fast path
   uses this (together with assignsFloatLikeRhs) to refuse int-promotion
   of a sub-scoped `my $x = <int literal>` whose body uses it in a way an
   unboxed i64 cannot represent. Deliberately conservative: any uncertain
   shape keeps the variable on the (always-correct) boxed PV path. */
static bool readsFloatSensitive(const Node &n, const std::string &nm) {
    bool r = false;
    switch (n.kind) {
    case NK::BinOp: {
        bool isFloatOp = (n.sval == "/" || n.sval == "**");
        if (n.left && n.right) {
            bool lHas = n.left->kind == NK::ScalarVar && n.left->name == nm;
            bool rHas = n.right->kind == NK::ScalarVar && n.right->name == nm;
            if ((lHas || rHas) && isFloatOp) return true;
        }
        break;
    }
    case NK::SqrtFunc:
        if (n.left && n.left->kind == NK::ScalarVar && n.left->name == nm)
            return true;
        break;
    case NK::UnaryOp:
        if (n.sval == "-" && n.left && n.left->kind == NK::ScalarVar &&
            n.left->name == nm)
            return true;
        break;
    default:
        break;
    }
    if (n.left)  r = r || readsFloatSensitive(*n.left,  nm);
    if (n.right) r = r || readsFloatSensitive(*n.right, nm);
    for (auto &a : n.args) r = r || readsFloatSensitive(*a, nm);
    if (n.body)  r = r || readsFloatSensitive(*n.body,  nm);
    if (n.init)  r = r || readsFloatSensitive(*n.init,  nm);
    if (n.cond)  r = r || readsFloatSensitive(*n.cond,  nm);
    if (n.step)  r = r || readsFloatSensitive(*n.step,  nm);
    for (auto &b : n.branches) {
        if (b.cond) r = r || readsFloatSensitive(*b.cond, nm);
        if (b.body) r = r || readsFloatSensitive(*b.body, nm);
    }
    return r;
}

/* Pure predicate — can this expression be computed as a bare LLVM double?
   Never emits any IR. Returns true iff emitExprF64 will succeed. */
bool CodeGen::canEmitF64(const Node &n) {
     switch (n.kind) {
     case NK::FloatLit:
         return true;
     case NK::IntLit:
         /* D78: Integers larger than 2^53 lose precision when converted to
            double. Don't use the F64 fast path for such values — fall back
            to the runtime's integer arithmetic which preserves exact values. */
         return n.ival >= -9007199254740992LL && n.ival <= 9007199254740992LL;
    case NK::ScalarVar: {
        std::string nm = n.name;
        if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
        if (lookupFloatVar(nm)) return true;
        /* D78: Don't use F64 fast path for file-scope vars initialized with
           large integer literals (> 2^53) — converting to double loses precision. */
        if (fileScalarLargeInt_.count(nm)) return false;
        /* D97: file-scope vars that may hold blessed objects can't use F64 */
        if (fileScalarBlessed_.count(nm)) return false;
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
        /* Hash values can be any type (string, ref, etc.) — unlike an array,
           there's no per-hash type tracking (D66: this used to return true
           whenever the hash was merely in scope, silently coercing string
           values to 0.0 via perl_to_float). Not safely float-promotable. */
        return false;
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
        /* Stage 31: return cached f64 if this exact element was already loaded (gated). */
        if (isOptStageEnabled("stage31") && isOptStageEnabled("flatdouble") && n.right->kind == NK::IntLit) {
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
                        /* Stage 31: cache the loaded value for repeated reads of same element (gated). */
                        if (isOptStageEnabled("stage31") && isOptStageEnabled("flatdouble") && n.right->kind == NK::IntLit) {
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
               Emits runtime tag checks; branches are perfectly predicted once type is fixed. */
                  if (n.right && lookupVar(nm)) {
                      if (Value *slot = lookupVar(nm)) {
                          auto *i32Ty = Type::getInt32Ty(ctx_);
                          auto *i64Ty = Type::getInt64Ty(ctx_);
                          auto *f64Ty = Type::getDoubleTy(ctx_);
                          auto *i8Ty  = Type::getInt8Ty(ctx_);
                          Value *pv   = builder_.CreateLoad(perlPtrTy_, slot, nm + ".pv");
                          Value *tag  = builder_.CreateLoad(i32Ty, pv, nm + ".tag");
                          Value *isPair = builder_.CreateICmpEQ(tag,
                              ConstantInt::get(i32Ty, 13), "ispair");
                          Value *isFlat = builder_.CreateICmpEQ(tag,
                              ConstantInt::get(i32Ty, 10), "isflat");
                          auto *curFn = builder_.GetInsertBlock()->getParent();
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
            /* Hash ArrowDeref ($ref->{key}) — values can be any type (string, ref, etc.).
               Cannot safely emit as F64. Fall through to regular emitExpr path. */
            return nullptr;
        }
    }
    case NK::HashElem:
        /* D66: hash values can be any type (string, ref, etc.), and unlike
           ArrayElem there's no per-hash element-type tracking to prove this
           read is safe. Bail out to the general (correct, tag-checking) path
           instead of blindly calling perl_to_float on a possibly-non-numeric
           value. Mirrors the hash-ArrowDeref case just above. */
        return nullptr;
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

/* D45: named subs are always compile-time-hoisted to package scope in real
   Perl, no matter how deeply nested in bare `{ }` blocks, if/while/for
   bodies, or eval blocks — `{ sub f {...} }` defines a perfectly normal,
   globally-callable `f`, not something scoped to the block. Recurse
   through every child field a Node can hold (mirroring
   hasWantarrayOrUserCall's traversal) so subs_ ends up with every
   NK::SubDef in the program, regardless of nesting depth. Deliberately
   still recurses into a found SubDef's own body (a named sub nested
   inside another named sub is unusual but also hoisted in real Perl) and
   into NK::AnonSub bodies (a named sub could be declared inside a
   closure) — both fall out naturally from the generic walk with no
   special-casing needed. */
static void collectGlobNames(const Node &n, std::unordered_set<std::string> &out) {
    if (n.kind == NK::Assign && n.left && n.left->kind == NK::Typeglob &&
        !n.left->name.empty())
        out.insert(n.left->name);
    if (n.kind == NK::OpenFunc && n.sval == "bare" && !n.name.empty())
        out.insert(n.name);
    if (n.left)  collectGlobNames(*n.left,  out);
    if (n.right) collectGlobNames(*n.right, out);
    if (n.cond)  collectGlobNames(*n.cond,  out);
    if (n.body)  collectGlobNames(*n.body,  out);
    if (n.init)  collectGlobNames(*n.init,  out);
    if (n.step)  collectGlobNames(*n.step,  out);
    for (auto &a : n.args) collectGlobNames(*a, out);
    for (auto &b : n.branches) {
        if (b.cond) collectGlobNames(*b.cond, out);
        if (b.body) collectGlobNames(*b.body, out);
    }
}

static void collectSubDefs(const Node &n, std::vector<const Node*> &out) {
    if (n.kind == NK::SubDef) out.push_back(&n);
    if (n.left)  collectSubDefs(*n.left,  out);
    if (n.right) collectSubDefs(*n.right, out);
    if (n.cond)  collectSubDefs(*n.cond,  out);
    if (n.body)  collectSubDefs(*n.body,  out);
    if (n.init)  collectSubDefs(*n.init,  out);
    if (n.step)  collectSubDefs(*n.step,  out);
    for (auto &a : n.args) collectSubDefs(*a, out);
    for (auto &b : n.branches) {
        if (b.cond) collectSubDefs(*b.cond, out);
        if (b.body) collectSubDefs(*b.body, out);
    }
}

/* D57: collect the names of every NK::Call found anywhere in an
   expression subtree — used to detect recursion cycles among candidate
   AST-inline subs (see below). */
static void collectCalls(const Node &n, std::vector<std::string> &out) {
    if (n.kind == NK::Call) out.push_back(n.name);
    if (n.left)  collectCalls(*n.left,  out);
    if (n.right) collectCalls(*n.right, out);
    if (n.cond)  collectCalls(*n.cond,  out);
    if (n.body)  collectCalls(*n.body,  out);
    if (n.init)  collectCalls(*n.init,  out);
    if (n.step)  collectCalls(*n.step,  out);
    for (auto &a : n.args) collectCalls(*a, out);
    for (auto &b : n.branches) {
        if (b.cond) collectCalls(*b.cond, out);
        if (b.body) collectCalls(*b.body, out);
    }
}

/* D57: tryEmitInline() has no recursion guard — inlining a call whose
   body (transitively, through other inlined subs) calls back to itself
   would recurse the *compiler* forever on the same, unchanging AST node
   (a self-recursive sub matching the inlinable shape crashed/hung the
   compiler at compile time, confirmed even at plain file scope with no
   block nesting involved). Standard 3-color DFS cycle detection over the
   call graph restricted to candidate-inlineable subs (a call to a
   non-candidate sub just becomes an ordinary, bounded function call and
   can't participate in a cycle): 0=unvisited, 1=on the current DFS
   stack, 2=fully explored. Finding an edge back to a node still marked
   "on stack" means everything from that node to the top of the stack
   forms a cycle (this also covers the direct self-recursion case, where
   the back-edge points at the node currently being visited itself). */
static void dfsFindCycles(const std::string &node,
                           const std::unordered_map<std::string, std::vector<std::string>> &callGraph,
                           std::unordered_map<std::string, int> &state,
                           std::vector<std::string> &stack,
                           std::set<std::string> &inCycle) {
    state[node] = 1;
    stack.push_back(node);
    auto it = callGraph.find(node);
    if (it != callGraph.end()) {
        for (const auto &callee : it->second) {
            int calleeState = state.count(callee) ? state[callee] : 0;
            if (calleeState == 1) {
                for (auto rit = stack.rbegin(); rit != stack.rend(); ++rit) {
                    inCycle.insert(*rit);
                    if (*rit == callee) break;
                }
            } else if (calleeState == 0) {
                dfsFindCycles(callee, callGraph, state, stack, inCycle);
            }
        }
    }
    stack.pop_back();
    state[node] = 2;
}

void CodeGen::compile(const Node &program, const std::string &modName,
                      bool asDoLib, bool asEvalPad) {
    mod_->setModuleIdentifier(modName);
    sourceFile_ = modName;
    mainBody_ = &program; /* D135: scan root for bare-block-at-file-scope declarations */
    asDoLib_ = asDoLib || asEvalPad;
    asEvalPad_ = asEvalPad;
    if (debug_) initializeDebugInfo(modName);

    /* D10: reset per-compilation counters so multi-compile is deterministic */
    sortCmpCounter_ = 0;
    substEvalCounter_ = 0;
    stateSeq_ = 0;
    endSeq_ = 0;
    lastSqrtInput_ = nullptr;
    floatSqrtOf_.clear();

    /* collect sub definitions first so forward calls work — recurse into
       every block/statement, not just program's direct top-level
       children (D45: a sub nested in a bare block is still global). */
    subs_.clear();
    subCaptures_.clear();
    globNames_.clear();
    collectSubDefs(program, subs_);
    collectGlobNames(program, globNames_);

    evalPadScalars_.clear();
    evalPadArrays_.clear();
    evalPadHashes_.clear();
    if (asEvalPad_) {
        std::set<std::string> sc, ar, hs;
        collectEvalPadNames(program, sc, ar, hs);
        evalPadScalars_.assign(sc.begin(), sc.end());
        evalPadArrays_.assign(ar.begin(), ar.end());
        evalPadHashes_.assign(hs.begin(), hs.end());
        for (auto &nm : evalPadScalars_) {
            if (fileScalarGlobals_.count(nm)) continue;
            auto *gv = new GlobalVariable(*mod_, perlPtrTy_, false,
                GlobalValue::InternalLinkage,
                Constant::getNullValue(perlPtrTy_), "epad.s." + nm);
            fileScalarGlobals_[nm] = gv;
        }
        for (auto &nm : evalPadArrays_) {
            if (fileArrayGlobals_.count(nm)) continue;
            auto *gv = new GlobalVariable(*mod_, perlPtrTy_, false,
                GlobalValue::InternalLinkage,
                Constant::getNullValue(perlPtrTy_), "epad.a." + nm);
            fileArrayGlobals_[nm] = gv;
        }
        for (auto &nm : evalPadHashes_) {
            if (fileHashGlobals_.count(nm)) continue;
            auto *gv = new GlobalVariable(*mod_, perlPtrTy_, false,
                GlobalValue::InternalLinkage,
                Constant::getNullValue(perlPtrTy_), "epad.h." + nm);
            fileHashGlobals_[nm] = gv;
        }
    }

    /* Detect inlineable subs: body = "my ($p1,..) = @_; return expr".
       These are expanded at call sites without @_ construction. Collected
       into `candidates` first (not inlineSubs_ directly) so the D57
       cycle check below can see the whole candidate set before any of
       them become "live" for tryEmitInline(). */
    std::unordered_map<std::string, InlineSub> candidates;
    for (auto *s : subs_) {
        if (!s->body) continue;
        if (!s->sval.empty()) continue; /* prototype: args are not a plain flatten */
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
        candidates[s->name] = {params, ret.left.get()};
    }

    /* D57: exclude any candidate that's part of a recursion cycle (direct
       self-recursion, or mutual recursion through other candidates) —
       see dfsFindCycles()'s comment for why inlining one would hang the
       compiler. Everything else becomes "live" in inlineSubs_. */
    {
        std::unordered_map<std::string, std::vector<std::string>> callGraph;
        for (auto &kv : candidates) {
            std::vector<std::string> calls;
            collectCalls(*kv.second.bodyExpr, calls);
            for (auto &callee : calls)
                if (candidates.count(callee)) callGraph[kv.first].push_back(callee);
        }
        std::set<std::string> inCycle;
        std::unordered_map<std::string, int> dfsState;
        for (auto &kv : candidates) {
            if (dfsState.count(kv.first)) continue;
            std::vector<std::string> stack;
            dfsFindCycles(kv.first, callGraph, dfsState, stack, inCycle);
        }
        for (auto &kv : candidates)
            if (!inCycle.count(kv.first)) inlineSubs_[kv.first] = kv.second;
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

    auto *i32Ty = Type::getInt32Ty(ctx_);
    if (!asDoLib) {
        /* emit main(int argc, char **argv) */
        auto *i8p  = PointerType::getUnqual(ctx_);
        auto *mainFT = FunctionType::get(i32Ty, {i32Ty, i8p}, false);
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
        stmtLabels_.clear();
        collectGotoLabels(program);
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

        /* D97: Register built-in Math::BigInt operator overloads.
           These map the arithmetic ops to runtime helpers using mini-gmp.
           Always registered — Math::BigInt is a built-in, not a user module. */
        {
            auto *mbStr = builder_.CreateGlobalStringPtr("Math::BigInt");
            struct OvlEntry { const char *op; const char *method; };
            static const OvlEntry bigintOverloads[] = {
                {"+",   "perl_bigint_ovl_add"},
                {"-",   "perl_bigint_ovl_sub"},
                {"*",   "perl_bigint_ovl_mul"},
                {"/",   "perl_bigint_ovl_div"},
                {"<=>", "perl_bigint_ovl_cmp"},
                {"\"\"", "perl_bigint_ovl_str"},
                {"neg", "perl_bigint_ovl_neg"},
            };
            for (auto &e : bigintOverloads) {
                Value *opStr = builder_.CreateGlobalStringPtr(e.op);
                Value *methStr = builder_.CreateGlobalStringPtr(e.method);
                callRT("perl_register_overload", {mbStr, opStr, methStr});
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

            if (hasDataSection_) {
                Value *ds = builder_.CreateGlobalStringPtr(dataSection_, ".data");
                Value *ln = ConstantInt::get(Type::getInt64Ty(ctx_),
                                             (long long)dataSection_.size());
                callRT("perl_set_data_section", {ds, ln});
            }

            Value *underscoreVal = callRT("perl_alloc_undef", {});
            auto *slotUs = builder_.CreateAlloca(perlPtrTy_, nullptr, "$_");
            builder_.CreateStore(underscoreVal, slotUs);
            declareVar("_", slotUs);
        }

        /* capture local() save depth at function entry */
        localDepthAlloca_ = builder_.CreateAlloca(i32Ty, nullptr, "local.depth");
        builder_.CreateStore(callRT("perl_local_save_depth", {}), localDepthAlloca_);

        /* D64: same pre-scan as emitSub/AnonSub, for the top-level program body */
        collectClosureCapturedNames(program, capturedNamesInCurrentFn_);
        allNamesCaptured_ = hasEvalCall(program);

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

        builder_.CreateRet(ConstantInt::get(i32Ty, 0));
    } else {
        /* D24: emit `PerlValue *__perlc_do_run(PerlArray *args, int ctx)` —
           a `do FILE`-loadable entry point instead of `main`. No @ARGV/$0
           setup (a do'd file doesn't get its own process argv) and no
           atexit(perl_cleanup) registration (only the single top-level
           program that eventually dlopen()s this .so should register
           process-exit cleanup — this library's own runtime.c symbols
           are never even linked in; see main.cpp's --do-lib link step).
           The file's top-level statements are compiled exactly like a
           sub body via emitBlockLast(), so the entry point returns the
           value of the last statement evaluated — matching real Perl's
           documented `do FILE` return-value contract (and the common
           `1;` idiom at end-of-file, matching require's convention). */
        auto *entryFT = FunctionType::get(perlPtrTy_, {arrayPtrTy_, i32Ty}, false);
        auto *entryFn = Function::Create(entryFT, Function::ExternalLinkage,
                                         "__perlc_do_run", mod_.get());
        entryFn->getArg(0)->setName("args");
        entryFn->getArg(1)->setName("ctx");
        auto *entry = BasicBlock::Create(ctx_, "entry", entryFn);
        builder_.SetInsertPoint(entry);

        if (debug_) {
            entryFn->setSubprogram(currentSP_);
            builder_.SetCurrentDebugLocation(getDebugLoc(1, currentSP_));
        }

        currentFn_ = entryFn;
        stmtLabels_.clear();
        collectGotoLabels(program);
        pushScope();
        fileScopeDepth_ = (int)scopes_.size() + 1;
        inMainBody_ = true;

        /* Register every sub — including unqualified names — so a string
           eval / do FILE that defines `sub foo {}` is callable afterwards
           via perl_call_named_sub("foo") from the loading process. Package-
           qualified names stay registered as before; bare names also get a
           main:: alias matching Perl's default package. */
        for (auto *s : subs_) {
            auto *fn = mod_->getFunction(subLLVMName(s->name));
            if (!fn) continue;
            Value *keyStr = builder_.CreateGlobalStringPtr(s->name);
            callRT("perl_register_method", {keyStr, fn});
            if (s->name.find("::") == std::string::npos) {
                Value *mainKey = builder_.CreateGlobalStringPtr("main::" + s->name);
                callRT("perl_register_method", {mainKey, fn});
            }
        }

        localDepthAlloca_ = builder_.CreateAlloca(i32Ty, nullptr, "local.depth");
        builder_.CreateStore(callRT("perl_local_save_depth", {}), localDepthAlloca_);

        /* D64: same pre-scan as the normal-main path above */
        collectClosureCapturedNames(program, capturedNamesInCurrentFn_);
        allNamesCaptured_ = hasEvalCall(program);

        if (hasDataSection_) {
            Value *ds = builder_.CreateGlobalStringPtr(dataSection_, ".data");
            Value *ln = ConstantInt::get(Type::getInt64Ty(ctx_),
                                         (long long)dataSection_.size());
            callRT("perl_set_data_section", {ds, ln});
        }

        if (asEvalPad_) emitEvalPadBind();

        /* do FILE / string eval inherit the caller's wantarray (ctx arg).
           Needed so a last-expr ArrayLit like `(1,2,3)` becomes LIST_RESULT
           in list context rather than the scalar last element. */
        bool savedDoLibWA = currentSubNeedsWantarray_;
        currentSubNeedsWantarray_ = true;
        callRT("perl_push_wantarray", {entryFn->getArg(1)});

        Value *lastVal = emitBlockLast(program);
        if (!builder_.GetInsertBlock()->getTerminator()) {
            callRT("perl_pop_wantarray", {});
            Value *depth = builder_.CreateLoad(i32Ty, localDepthAlloca_);
            callRT("perl_local_restore_to", {depth});
            popScope();
            builder_.CreateRet(lastVal ? lastVal : perlUndef());
        } else {
            popScope();  /* explicit return: already terminated this block */
        }
        currentSubNeedsWantarray_ = savedDoLibWA;
    }

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

/* ── goto LABEL support ─────────────────────────────────────────────────── */

void CodeGen::collectGotoLabels(const Node &n) {
    /* Nested subs have their own functions — don't plant their labels here. */
    if (n.kind == NK::SubDef || n.kind == NK::AnonSub) return;
    if (n.kind == NK::LabelStmt && !n.name.empty())
        (void)labelBB(n.name);
    if (!n.sval.empty() &&
        (n.kind == NK::While || n.kind == NK::For ||
         n.kind == NK::Foreach || n.kind == NK::DoWhile))
        (void)labelBB(n.sval);
    if (n.left)  collectGotoLabels(*n.left);
    if (n.right) collectGotoLabels(*n.right);
    if (n.cond)  collectGotoLabels(*n.cond);
    if (n.body)  collectGotoLabels(*n.body);
    if (n.init)  collectGotoLabels(*n.init);
    if (n.step)  collectGotoLabels(*n.step);
    for (auto &a : n.args) collectGotoLabels(*a);
    for (auto &b : n.branches) {
        if (b.cond) collectGotoLabels(*b.cond);
        if (b.body) collectGotoLabels(*b.body);
    }
}

llvm::BasicBlock *CodeGen::labelBB(const std::string &name) {
    auto it = stmtLabels_.find(name);
    if (it != stmtLabels_.end()) return it->second;
    auto *fn = currentFn_;
    auto *bb = BasicBlock::Create(ctx_, "lbl." + name, fn);
    stmtLabels_[name] = bb;
    return bb;
}

void CodeGen::flattenArgInto(Value *argsArr, const Node &arg) {
    if (arg.kind == NK::ArrayVar) {
        Value *av = lookupArray(arg.name);
        if (av) { callRT("perl_array_extend", {argsArr, av}); return; }
    }
    if (arg.kind == NK::HashVar) {
        Value *hv = lookupHash(arg.name);
        if (hv) { callRT("perl_array_extend_hash", {argsArr, hv}); return; }
    }
    if (Value *av = emitArrayPtr(arg)) {
        callRT("perl_array_extend", {argsArr, av});
        return;
    }
    Value *v = emitExpr(arg);
    callRT("perl_array_push", {argsArr, v});
    freeIfOwned(v);
}

void CodeGen::fillCallArgs(Value *argsArr, const Node &n) {
    /* ival==2 is &name() — bypass prototype. Empty sval = no prototype. */
    if (n.ival == 2 || n.sval.empty()) {
        for (auto &arg : n.args) flattenArgInto(argsArr, *arg);
        return;
    }
    const std::string &proto = n.sval;
    size_t ai = 0;
    bool slurp = false;
    for (char c : proto) {
        if (c == '\\' || c == '[' || c == ']') continue;
        if (c == ';') continue;
        if (c == '@' || c == '%') { slurp = true; break; }
        int saved = callCtx_;
        callCtx_ = 0;
        if (ai >= n.args.size()) {
            if (c == '_') {
                if (auto *slot = lookupVar("_")) {
                    Value *v = builder_.CreateLoad(perlPtrTy_, slot);
                    callRT("perl_array_push", {argsArr, v});
                } else {
                    callRT("perl_array_push", {argsArr, perlUndef()});
                }
            }
            callCtx_ = saved;
            continue;
        }
        /* $ * + & _ : one argument, scalar context (no list flatten) */
        Value *v = emitExpr(*n.args[ai++]);
        callRT("perl_array_push", {argsArr, v});
        freeIfOwned(v);
        callCtx_ = saved;
    }
    if (slurp) {
        int saved = callCtx_;
        callCtx_ = 1;
        while (ai < n.args.size()) flattenArgInto(argsArr, *n.args[ai++]);
        callCtx_ = saved;
    }
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

    /* derive package from sub name for caller() tracking */
    std::string savedPackage = currentPackage_;
    {
        auto sc = n.name.rfind("::");
        currentPackage_ = (sc != std::string::npos) ? n.name.substr(0, sc) : "main";
    }

    /* D64: pre-scan this sub's body once for names captured by any
       closure nested within it, so `case NK::My:`'s fast-path check can
       skip the unboxed int/float optimization for those specific names. */
    auto savedCapturedNames = capturedNamesInCurrentFn_;
    capturedNamesInCurrentFn_.clear();
    if (n.body) collectClosureCapturedNames(*n.body, capturedNamesInCurrentFn_);
    bool savedAllCap = allNamesCaptured_;
    allNamesCaptured_ = n.body && hasEvalCall(*n.body);

    auto *savedFn = currentFn_;
    currentFn_ = fn;
    /* D124: __SUB__ inside a named sub resolves to that sub's own code ref. */
    std::string savedSubNm124 = currentSubName_;
    bool savedAnonEmit124 = inAnonSubEmit_;
    currentSubName_ = n.name;
    inAnonSubEmit_ = false;
    auto savedLabels = std::move(stmtLabels_);
    stmtLabels_.clear();
    if (n.body) collectGotoLabels(*n.body);
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
        /* W29: the sub's $_ starts as the GLOBAL $_ cell's current value
           (real perl: bare $_ inside a sub IS the global unless
           localized — `local $_` in a caller is visible here). The
           alloca still shadows for assignment within the sub; the global
           cell is only the initial value. */
        Value *cell = callRT("perl_get_dollar_under", {});
        /* s_dollar_under IS the PerlValue (an s_dollar_at-style stable
           cell, not a pointer-slot) — clone it for the sub's local
           $_ storage; the clone is freed on scope exit while the cell
           keeps its own contents. */
        Value *udv  = callRT("perl_clone", {cell});
        auto *slotUs = builder_.CreateAlloca(perlPtrTy_, nullptr, "$_");
        builder_.CreateStore(udv, slotUs);
        declareVar("_", slotUs);
        trackPv(udv);
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
        stmtLabels_ = std::move(savedLabels);
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
    currentSubName_ = savedSubNm124;
    inAnonSubEmit_ = savedAnonEmit124;
    capturedNamesInCurrentFn_ = std::move(savedCapturedNames);
    allNamesCaptured_ = savedAllCap;
    stmtLabels_ = std::move(savedLabels);

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
    /* D115: bare `return;` (no expression) also reads the wantarray
       stack now, to decide between an empty list and undef — without
       this, currentSubNeedsWantarray_ stays false for a sub whose only
       return is bare, so the caller never pushes a real context and
       perl_current_wantarray_ctx() reads stale/wrong state inside it. */
    if (n.kind == NK::Return && !n.left) return true;
    /* implicit list return from grep/map/sort also reads wantarray stack */
    if (n.kind == NK::MapFunc || n.kind == NK::GrepFunc ||
        n.kind == NK::SortFunc) return true;
    /* implicit last-expr `(1,2,3)` / `@arr` — string eval and `sub f{(1,2)}` */
    if (n.kind == NK::ArrayLit || n.kind == NK::ArrayVar) return true;
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
    if (n.kind == NK::LocalStmt || n.kind == NK::LocalArray || n.kind == NK::LocalHash ||
        n.kind == NK::LocalGlob) return true;
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
        /* Don't stop on a terminator: goto leaves a dead BB, but a later
           LabelStmt still needs to be emitted into its own block. */
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
        const Node *work = &stmt;
        if (isLast) {
            while (work->kind == NK::LabelStmt && work->body) {
                auto *bb = labelBB(work->name);
                if (!builder_.GetInsertBlock()->getTerminator())
                    builder_.CreateBr(bb);
                builder_.SetInsertPoint(bb);
                work = work->body.get();
            }
        }
        if (isLast && work->kind == NK::ExprStmt && work->left) {
            /* If the last expr produces a list (grep/map/sort/etc.) and we're in
               a wantarray-aware sub, wrap it for list/scalar context propagation. */
            const Node &le = *work->left;
            NK lk = le.kind;
            bool isListProducer = (lk == NK::MapFunc || lk == NK::GrepFunc ||
                                   lk == NK::SortFunc || lk == NK::DerefArray ||
                                   lk == NK::ReverseFunc || lk == NK::ArrayLit ||
                                   lk == NK::ArrayVar);
            if (isListProducer && currentSubNeedsWantarray_) {
                Value *av = emitArrayPtr(le);
                if (!av) av = callRT("perl_array_new", {});
                /* grep/map return COUNT in scalar context (not last element);
                   sort returns undef in scalar context (D29). */
                if (lk == NK::GrepFunc || lk == NK::MapFunc || lk == NK::SortFunc) {
                    auto *i32Ty = Type::getInt32Ty(ctx_);
                    Value *ctx = callRT("perl_current_wantarray_ctx", {});
                    /* D87: only ctx==1 is list; void(2) and scalar(0) are not */
                    Value *isList = builder_.CreateICmpEQ(ctx, ConstantInt::get(i32Ty, 1));
                    Value *listResult = callRT("perl_array_to_list_return", {av});
                    Value *scalarResult = (lk == NK::SortFunc) ? perlUndef()
                                         : callRT("perl_array_len", {av});
                    result = builder_.CreateSelect(isList, listResult, scalarResult);
                } else {
                    result = callRT("perl_array_to_list_return", {av});
                }
            } else {
                /* D87: last expr inherits caller's wantarray (return f()) */
                int savedCtx = callCtx_;
                callCtx_ = -1;
                result = emitExpr(le);
                callCtx_ = savedCtx;
            }
        } else if (isLast && work->kind == NK::Return) {
            /* Capture the return value without emitting a ret instruction.
                The caller will handle the actual return. */
            if (work->left && (work->left->kind == NK::ArrayLit || work->left->kind == NK::ArrayVar ||
                                work->left->kind == NK::MapFunc  || work->left->kind == NK::GrepFunc ||
                                work->left->kind == NK::SortFunc || work->left->kind == NK::DerefArray ||
                                work->left->kind == NK::ReverseFunc)) {
                Value *av = emitArrayPtr(*work->left);
                if (!av) av = callRT("perl_array_new", {});
                /* grep/map return COUNT in scalar context (not last element);
                   sort returns undef in scalar context (D29). */
                if (work->left->kind == NK::GrepFunc || work->left->kind == NK::MapFunc ||
                    work->left->kind == NK::SortFunc) {
                    auto *i32Ty = Type::getInt32Ty(ctx_);
                    Value *ctx = callRT("perl_current_wantarray_ctx", {});
                    Value *isList = builder_.CreateICmpEQ(ctx, ConstantInt::get(i32Ty, 1));
                    Value *listResult = callRT("perl_array_to_list_return", {av});
                    Value *scalarResult = (work->left->kind == NK::SortFunc) ? perlUndef()
                                         : callRT("perl_array_len", {av});
                    result = builder_.CreateSelect(isList, listResult, scalarResult);
                } else {
                    result = callRT("perl_array_to_list_return", {av});
                }
            } else if (!work->left) {
                /* D115: bare `return;` as a sub's last (only) statement
                   goes through this emitBlockLast-specific duplicate of
                   case NK::Return's logic, not emitStmt's — must get the
                   identical empty-list-in-list-context fix, or a sub
                   whose sole statement is `return;` keeps the old
                   1-element-undef-list bug regardless of the emitStmt
                   fix above. */
                Value *emptyAv = callRT("perl_array_new", {});
                Value *listResult = callRT("perl_array_to_list_return", {emptyAv});
                auto *i32Ty = Type::getInt32Ty(ctx_);
                Value *ctx = callRT("perl_current_wantarray_ctx", {});
                Value *isList = builder_.CreateICmpEQ(ctx, ConstantInt::get(i32Ty, 1));
                result = builder_.CreateSelect(isList, listResult, perlUndef());
            } else {
                int savedCtx = callCtx_;
                callCtx_ = -1; /* D87: return EXPR inherits caller context */
                result = emitExpr(*work->left);
                callCtx_ = savedCtx;
            }
        } else {
            emitStmt(*work);
        }
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
    bool labelish = (n.kind == NK::LabelStmt) ||
        (!n.sval.empty() && (n.kind == NK::While || n.kind == NK::For ||
                             n.kind == NK::Foreach || n.kind == NK::DoWhile));
    if (builder_.GetInsertBlock()->getTerminator() && !labelish) return;
    /* D9: sqrt input tracking is per-statement; don't leak across stmts */
    lastSqrtInput_ = nullptr;
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

    case NK::FlatBlock: {
        /* emit contents in the current scope, no new scope push */
        for (auto &stmt : n.args) {
            emitStmt(*stmt);
        }
        break;
    }

    case NK::ExprStmt: {
        /* D87: bare statement is void context for any top-level call */
        int savedCtx = callCtx_;
        if (n.left && isCallLikeForContext(*n.left)) callCtx_ = 2;
        freeIfOwned(emitExpr(*n.left));
        callCtx_ = savedCtx;
        break;
    }

    case NK::My: {
        if (n.name.empty()) break;
        bool isArr  = n.name[0] == '@';
        bool isHash = n.name[0] == '%';
        bool atFileScope = inMainBody_ && (int)scopes_.size() == fileScopeDepth_;
        bool isOur = (n.ival & 2) != 0; /* package global even inside a sub */
        bool asGlobal = atFileScope || isOur;

        if (isHash) {
            std::string nm = n.name.substr(1);
            Value *hv = callRT("perl_hash_new", {});
            if (asGlobal) {
                /* D112: keyed primarily by package-qualified name so two
                   different packages' same-named file-scope %hash (`my`
                   or `our`) don't collide on one shared global — see the
                   identical fix/rationale on the scalar branch below. */
                std::string qualKey = currentPackage_ + "::" + nm;
                GlobalVariable *gv = nullptr;
                auto git = fileHashGlobals_.find(qualKey);
                bool reused = git != fileHashGlobals_.end();
                if (reused)
                    gv = git->second;
                else {
                    gv = new GlobalVariable(*mod_, perlPtrTy_, false,
                        GlobalValue::InternalLinkage,
                        Constant::getNullValue(perlPtrTy_), "g.hash." + nm);
                    fileHashGlobals_[qualKey] = gv;
                    if (!fileHashGlobals_.count(nm))
                        fileHashGlobals_[nm] = gv;
                }
                /* D131 (part 2): a repeated `our %hash;` (no initializer)
                   for the same qualKey used to unconditionally overwrite
                   the global with a brand-new empty hash here, wiping out
                   whatever the first declaration's code had already
                   populated — matching the identical bug/fix on the
                   scalar branch below. When reusing, keep the existing
                   hash object alive and populate through it instead of a
                   disconnected fresh one; only a fresh global gets the
                   fresh hash. */
                if (reused && !n.right)
                    hv = builder_.CreateLoad(perlPtrTy_, gv);
                else
                    builder_.CreateStore(hv, gv);
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
            if (n.right) {
                callCtx_ = 1;
                Value *rhsArr = emitArrayPtr(*n.right);
                callCtx_ = 0;
                if (rhsArr) {
                    /* D99: emitArrayPtr can return a *borrowed* pointer that
                       aliases existing storage (a plain @var, or @$ref /
                       ->@* deref — see the identical ownsTmpArr check in
                       Foreach above) rather than a freshly built array.
                       Declaring that pointer directly as this `my` array's
                       backing store would make the new variable and the
                       source share one PerlArray — mutating either would
                       corrupt the other ("my @b = @a; $b[0] = 1" also
                       changing @a). Always materialize a fresh array and
                       copy elements in, matching the sibling `@dst = @src`
                       codegen path (case NK::Assign, ArrayVar LHS) below,
                       which already does this correctly via
                       perl_array_replace. */
                    NK rk = n.right->kind;
                    bool borrowed = (rk == NK::ArrayVar || rk == NK::DerefArray ||
                                      (rk == NK::PostfixDeref && n.right->sval == "all_array"));
                    if (borrowed) {
                        av = callRT("perl_array_new", {});
                        /* D105: rhsArr's own elements may themselves be
                           FLAT_ARRAY/FLOAT_PAIR-tagged anon-array-ref
                           values that are independently aliased elsewhere
                           (e.g. `my $inner=[1,2]; my @a=($inner,...); my
                           @b=@a;` — $inner must still observe writes made
                           through $b[0]). Promote them to real REF_ARRAYs
                           in place before the extend clones them, so the
                           clone and every other alias end up sharing one
                           PerlArray instead of silently forking. */
                        callRT("perl_array_promote_refs", {rhsArr});
                        callRT("perl_array_extend", {av, rhsArr});
                    } else {
                        av = rhsArr;
                    }
                }
            }
           if (!av) {
                  av = callRT("perl_array_new", {});
                  /* scalar RHS (e.g. my @arr = $ref  or  my @arr = [1,2,3]) —
                     push the value as a single element */
                  if (n.right) {
                      callCtx_ = 1;
                      Value *rhsVal = emitExpr(*n.right);
                      callRT("perl_array_push_list_or_scalar", {av, rhsVal});
                      callCtx_ = 0;
                  }
              }
            if (asGlobal) {
                /* D112: see the identical package-qualified-key fix on the
                   hash/scalar branches. */
                std::string qualKey = currentPackage_ + "::" + nm;
                auto git = fileArrayGlobals_.find(qualKey);
                bool reused = git != fileArrayGlobals_.end();
                GlobalVariable *gv;
                if (reused) {
                    gv = git->second;
                } else {
                    gv = new GlobalVariable(*mod_, perlPtrTy_, false,
                        GlobalValue::InternalLinkage,
                        Constant::getNullValue(perlPtrTy_), "g.arr." + nm);
                    fileArrayGlobals_[qualKey] = gv;
                    if (!fileArrayGlobals_.count(nm))
                        fileArrayGlobals_[nm] = gv;
                }
                /* D131 (part 2): a repeated `our @arr;` (no initializer)
                   for the same qualKey used to unconditionally overwrite
                   the global with a brand-new empty array here, wiping
                   out whatever the first declaration's code had already
                   populated — identical bug/fix to the hash branch above.
                   Only overwrite when reused AND an initializer is
                   actually given; otherwise keep the existing array
                   alive and reload it. */
                if (reused && !n.right) {
                    av = builder_.CreateLoad(perlPtrTy_, gv);
                } else {
                    builder_.CreateStore(av, gv);
                }
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
                /* D131: was `atFileScope` alone, ignoring `isOur` — an
                   `our $x :shared` inside a nested block (not file
                   scope) fell to the local-alloca branch below instead
                   of getting real global storage, the same class of bug
                   fixed for the plain (non-shared) scalar branch below. */
                if (asGlobal) {
                    /* D131 (part 2): reuse an existing global for a
                       repeated `our $x :shared;` occurrence instead of
                       minting a fresh disconnected one each time — see
                       the identical fix/rationale on the plain scalar
                       branch below. */
                    std::string qualKey = currentPackage_ + "::" + nm;
                    Value *gv = nullptr;
                    auto sgit = fileScalarGlobals_.find(qualKey);
                    if (sgit != fileScalarGlobals_.end()) {
                        gv = sgit->second;
                        pv = builder_.CreateLoad(perlPtrTy_, gv);
                    } else {
                        auto *newGv = new GlobalVariable(*mod_, perlPtrTy_, false,
                            GlobalValue::InternalLinkage,
                            Constant::getNullValue(perlPtrTy_), "g." + nm);
                        pv = callRT("perl_make_shared_scalar", {});
                        builder_.CreateStore(pv, newGv);
                        gv = newGv;
                    }
                    fileScalarGlobals_[nm] = gv;
                    fileScalarGlobals_[qualKey] = gv;
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
            /* D131: was `atFileScope && asDoLib_` — an `our $x;` inside
               a nested block (not file scope) has isOur=true/asGlobal=
               true but atFileScope=false, so it fell all the way to the
               unbox-fast-path `else` branch below and got a plain
               local-scope alloca instead of real global storage; a sub
               referencing that same `our`-declared name from outside
               the block then saw a completely disconnected variable
               (via the "auto-vivify global-ish variable" fallback,
               which finds nothing and silently starts fresh). */
            if (asGlobal && asDoLib_) {
                /* D58: in a --do-lib build, route file-scope scalars through
                   the process-wide global-scalar registry instead of an
                   ordinary per-compilation-unit GlobalVariable — each
                   separate `do` call compiles and dlopen()s an independent
                   shared library, so a plain GlobalVariable would give
                   every call its own disconnected storage instead of the
                   single persistent package-variable slot real Perl's
                   `our`/package-scalars imply. The registry lookup runs
                   every time this declaration executes (matching a normal
                   GlobalVariable's "already initialized, just referenced
                   again" behavior on repeat execution within one process);
                   an explicit initializer (`our $x = 5`) still re-assigns
                   every time this statement runs, exactly like the
                   GlobalVariable path below and matching real Perl (an
                   initializer is a normal assignment, not run-once magic —
                   only a bare `our $x;` leaves an existing value alone). */
                std::string qualKey = (currentPackage_.empty() || currentPackage_ == "main")
                    ? ("main::" + nm) : (currentPackage_ + "::" + nm);
                Value *keyStr = builder_.CreateGlobalStringPtr(qualKey);
                Value *pv = callRT("perl_get_or_create_global_scalar", {keyStr});
                auto *slot = builder_.CreateAlloca(perlPtrTy_, nullptr, "g." + nm);
                builder_.CreateStore(pv, slot);
                 if (n.right) {
                     Value *init = emitExpr(*n.right);
                     callRT("perl_assign", {pv, init});
                     freeIfOwned(init);
                 }
                 /* D78: track file-scope vars initialized with large int literals */
                 if (n.right && n.right->kind == NK::IntLit &&
                     (n.right->ival > 9007199254740992LL || n.right->ival < -9007199254740992LL))
                     fileScalarLargeInt_.insert(nm);
                 /* D97: track file-scope vars that may hold blessed objects */
                 if (n.right &&
                     (n.right->kind == NK::MethodCall ||
                      n.right->kind == NK::BlessFunc))
                     fileScalarBlessed_.insert(nm);
                 /* D112: always register the package-qualified key (not
                    just when non-main) so lookupVar's qualified-first
                    check (see above) can tell apart two different
                    packages' same-named file-scope scalars — previously
                    only non-main packages got a qualified entry, so a
                    main-package var and a same-named module var could
                    still collide via the shared bare fallback slot. */
                 fileScalarGlobals_[nm] = slot;
                declareVar(nm, slot);
                fileScalarGlobals_[currentPackage_ + "::" + nm] = slot;
            } else if (asGlobal) {
                /* D131: was `atFileScope` alone — see the identical fix
                   note on the asDoLib_ branch just above. */
                /* D131 (part 2): a second/later textual `our $x;` (or
                   `our $x = ...;`) for the same package-qualified name —
                   e.g. one occurrence at file scope and another inside a
                   sub or a different nested block, the exact shape real
                   Perl code uses to "bring an existing our-var into
                   scope" — used to unconditionally mint a brand-new
                   GlobalVariable here every time, unlike the sibling
                   array/hash branches above which already look up
                   fileArrayGlobals_/fileHashGlobals_ by qualified name
                   and reuse the existing global. Every repeat occurrence
                   therefore created its own disconnected LLVM global
                   (LLVM auto-renames the symbol to avoid a name clash),
                   so code in one occurrence's scope never saw writes
                   made through another's. Fixed by reusing an existing
                   global for this qualKey when present, and — since a
                   bare `our $x;` with no initializer must leave the
                   existing value untouched, matching real Perl — only
                   resetting the storage to undef when the global is
                   newly created. */
                std::string qualKey = currentPackage_ + "::" + nm;
                Value *gv = nullptr;
                auto sgit = fileScalarGlobals_.find(qualKey);
                if (sgit != fileScalarGlobals_.end()) {
                    gv = sgit->second;
                } else {
                    auto *newGv = new GlobalVariable(*mod_, perlPtrTy_, false,
                        GlobalValue::InternalLinkage,
                        Constant::getNullValue(perlPtrTy_), "g." + nm);
                    /* W22: ONE shared undef PerlValue backs both storages —
                       the LLVM global (compile-time-known reads/writes of
                       the named variable) and the runtime glob registry
                       cell (symbolic dereferences by computed name,
                       ${ EXPR }, which real Perl resolves through the
                       symbol table at runtime when the name isn't known
                       at compile time). Both alias the same PerlValue, so
                       perl_assign's in-place mutation is visible through
                       either path. The bare-name fallback registration
                       matches glob_get_or_create's own bare-vs-qualified
                       matching (its ::-suffix strip), so a main-package
                       ${"name"} finds it either way. */
                    Value *shared = perlUndef();
                    builder_.CreateStore(shared, newGv);
                    if (currentPackage_.empty() || currentPackage_ == "main") {
                        Value *bareKey = builder_.CreateGlobalStringPtr(
                            std::string("main::") + nm);
                        callRT("perl_glob_set_scalar", {bareKey, shared});
                    } else {
                        Value *pkgKey = builder_.CreateGlobalStringPtr(qualKey);
                        callRT("perl_glob_set_scalar", {pkgKey, shared});
                    }
                    gv = newGv;
                }
                 if (n.right) {
                     Value *pv = builder_.CreateLoad(perlPtrTy_, gv);
                     Value *init = emitExpr(*n.right);
                     callRT("perl_assign", {pv, init});
                     freeIfOwned(init);
                 }
                 /* D78: track file-scope vars initialized with large int literals */
                 if (n.right && n.right->kind == NK::IntLit &&
                     (n.right->ival > 9007199254740992LL || n.right->ival < -9007199254740992LL))
                     fileScalarLargeInt_.insert(nm);
                 /* D97: track file-scope vars that may hold blessed objects
                    (assigned from MethodCall or BlessFunc) so canEmitF64
                    excludes them and arithmetic goes through the overload
                    dispatch path in perl_add/perl_mul/etc. */
                 if (n.right &&
                     (n.right->kind == NK::MethodCall ||
                      n.right->kind == NK::BlessFunc))
                     fileScalarBlessed_.insert(nm);
                 /* D112: register both the bare name (last-declared-wins
                    fallback slot, unchanged prior behavior for lookups
                    that don't know which package to prefer) and the
                    package-qualified name unconditionally (not just for
                    non-main) — this is what makes lookupVar's qualified-
                    first check able to distinguish two different
                    packages' same-named file-scope scalars instead of
                    silently sharing whichever one happened to declare
                    last. */
                 fileScalarGlobals_[nm] = gv;
                declareVar(nm, gv);
                fileScalarGlobals_[currentPackage_ + "::" + nm] = gv;
            } else {
                /* Unbox numeric scalars: skip PerlValue* alloca entirely.
                   Guard: 1D ArrowDeref may return an array/hash ref, not a scalar.
                   D64: also skip this fast path when `nm` is captured by some
                   closure/sort-comparator anywhere in the current function —
                   the fast path has no real PerlValue* for a closure to later
                   share, so the existing capture-collection fallback
                   (lookupIntVar/boxI64) would box a disconnected snapshot
                   instead, silently breaking mutation visibility in either
                   direction across the closure boundary. */
                bool rhsMayBeRef = n.right &&
                    n.right->kind == NK::ArrowDeref && n.right->sval == "array" &&
                    n.right->left && n.right->left->kind != NK::ArrowDeref;
                /* D97: MethodCall/BlessFunc return blessed objects that can't
                   be unboxed to int/float — arithmetic on them needs the
                   overload dispatch path in perl_add/perl_mul/etc. */
                bool rhsMayBeBlessed = n.right &&
                    (n.right->kind == NK::MethodCall ||
                     n.right->kind == NK::BlessFunc ||
                     n.right->kind == NK::Call);
                bool mayBeCaptured = allNamesCaptured_ ||
                    capturedNamesInCurrentFn_.count(nm) != 0 ||
                    capturedNamesInCurrentFn_.count("$" + nm) != 0;
                if (n.right && !atFileScope && !rhsMayBeRef && !rhsMayBeBlessed && !mayBeCaptured) {
                     /* D135: inside a sub, int-promotion must not pin the
                        variable to i64 storage for its whole lifetime when
                        the body later leaves the integer domain — every
                        subsequent Assign routes through the int-var branch
                        and its boxed fallback perl_to_ints the value,
                        silently truncating `$x = 5.5` to 5 (real Perl has
                        no sticky per-variable type).
                        Decide with the existing body-scan machinery: skip
                        the i64 promotion when the body (a) ever assigns
                        this name a float-shaped RHS (`$x = 5.5`, `+= 0.5`,
                        `/=`, ++/--), or (b) reads it in a float-sensitive
                        context (`/`, `**`, sqrt, unary minus) — an unboxed
                        i64 cannot represent either shape. When the
                        initializer is float-representable AND the body
                        never uses nm in a position where a plain double is
                        observably different from a real PerlValue*
                        (floatVarUseSafe), take the float path below; any
                        other float-shaped case falls through to the plain
                        boxed PV path — never int-promote.
                        Pure-int bodies (loop counters etc.) pass both
                        scans and keep the identical i64 fast path. The
                        scan root is the enclosing sub body, or (bare
                        block at file scope, D135) the program body; a
                        true file-scope global declaration never reaches
                        this branch (atFileScope takes the global path). */
                     bool skipIntPromo = false;
                     bool tryFloatPath = false;
                     /* D135: the scan root is the scope that can still see
                        this variable for the rest of its lifetime — the
                        enclosing sub's body, or (for a bare block at file
                        scope) the whole program body. A true file-scope
                        declaration has neither (atFileScope true takes the
                        global path long before this point). Over-scanning
                        the program root for a block-local variable is
                        conservative-safe: it may refuse promotion where
                        the var is actually block-local, which only costs
                        the boxed path (always correct), never correctness. */
                     const Node *scanRoot =
                         currentSubBody_ ? currentSubBody_
                       : (!atFileScope && mainBody_) ? mainBody_
                       : nullptr;
                       if (scanRoot) {
                           D135IntSet intSet = d135ComputeIntSet(*scanRoot);
                           bool assignsFloat = assignsFloatLikeRhs(*this, *scanRoot, nm, &intSet);
                           bool readsFloat   = readsFloatSensitive(*scanRoot, nm);
                           if (assignsFloat || readsFloat) {
                              skipIntPromo = true;
                             /* Float-shaped RHS in the body: staying in the
                                unboxed domain is only safe on the float
                                alloca, and only when the body never uses nm
                                in a position where a plain double would be
                                observably different from a real PerlValue*
                                (string ops, ref-taking, call args, keys,
                                blessed positions...). floatVarUseSafe
                                answers exactly that — a plain read is fine
                                (boxF64 on demand), while floatSafe's
                                bare-read case is deliberately false (it
                                gates *int* promotion). When in doubt, the
                                boxed PV path is always correct. */
                             tryFloatPath = floatVarUseSafe(*scanRoot, nm);
                         }
                     }
                    if (tryFloatPath) {
                        if (Value *fval = emitExprF64(*n.right)) {
                            auto *falloca = builder_.CreateAlloca(Type::getDoubleTy(ctx_), nullptr, n.name + ".f");
                            builder_.CreateStore(fval, falloca);
                            declareFloatVar(nm, falloca);
                            /* Stage 30: if this var was assigned sqrt(x), remember x.
                               Enables v*v → x and v*v*v → x*v (shorter sqrt critical path).
                               D9: always clear any stale association for this name
                               first — a new `my $var = ...` declaration is a brand-new
                               binding with no relation to a previous sqrt-tracked value
                               of the same name, whether that previous one came from an
                               earlier declaration in this same function or (worse) a
                               completely unrelated sub that happened to reuse the same
                               variable name. Leaving the old entry in place made
                               `$a*$a` silently return the *other* sub's sqrt input
                               instead of this variable's actual squared value. */
                            floatSqrtOf_.erase(nm);
                            if (lastSqrtInput_) { floatSqrtOf_[nm] = lastSqrtInput_; lastSqrtInput_ = nullptr; }
                            break;
                        }
                        /* initializer not float-representable → boxed PV path */
                    } else if (!skipIntPromo) {
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
                           Enables v*v → x and v*v*v → x*v (shorter sqrt critical path).
                           D9: always clear any stale association for this name
                           first — a new `my $var = ...` declaration is a brand-new
                           binding with no relation to a previous sqrt-tracked value
                           of the same name, whether that previous one came from an
                           earlier declaration in this same function or (worse) a
                           completely unrelated sub that happened to reuse the same
                           variable name. Leaving the old entry in place made
                           `$a*$a` silently return the *other* sub's sqrt input
                           instead of this variable's actual squared value. */
                        floatSqrtOf_.erase(nm);
                        if (lastSqrtInput_) { floatSqrtOf_[nm] = lastSqrtInput_; lastSqrtInput_ = nullptr; }
                        break;
                    }
                    } /* !skipIntPromo */
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
        /* n.left: brace-block filehandle expression (print {$fh} LIST) */
        Value *fh = nullptr;
        if (n.name == "STDERR")       fh = callRT("perl_get_stderr", {});
        else if (n.name == "STDOUT")  fh = callRT("perl_get_stdout", {});
        else if (n.name == "STDIN")   fh = callRT("perl_get_stdin", {});
        else if (!n.name.empty()) {
            if (auto *slot = lookupVar(n.name))
                fh = builder_.CreateLoad(perlPtrTy_, slot);
            else if (isGlobName(n.name)) {
                Value *key = builder_.CreateGlobalStringPtr(globBareName(n.name));
                fh = callRT("perl_glob_get_scalar", {key});
            }
        }
        /* D86: brace-block filehandle form — evaluate n.left as the fh expression */
        if (!fh && n.left) {
            fh = emitExpr(*n.left);
        }
        if (fh) {
            /* print/say to filehandle */
            if (n.args.empty()) {
                if (auto *slot = lookupVar("_")) {
                    Value *v = builder_.CreateLoad(perlPtrTy_, slot);
                    callRT(isSay ? "perl_say_fh" : "perl_print_fh", {fh, v});
                }
            } else {
                /* D12: print's arguments are always evaluated in list
                   context in real Perl, regardless of print's own
                   (always-scalar/void) context — a sub called *directly*
                   as a print argument must see wantarray()==true. callCtx_
                   is a one-shot flag consumed (reset to 0) by the first
                   Call node's own codegen, so it must be set fresh before
                   each argument, not once before the whole loop — and only
                   when the argument is itself call-like (see
                   isCallLikeForContext), or it would leak list context
                   through an enclosing operator like `eq`/`==` into a call
                   nested inside it, which must see scalar context instead. */
                for (size_t i = 0; i < n.args.size(); i++) {
                    if (i > 0) callRT("perl_print_sep_fh", {fh});
                    bool lastArg = (i + 1 == n.args.size());
                    if (isCallLikeForContext(*n.args[i])) callCtx_ = 1;
                    Value *v = emitExpr(*n.args[i]);
                    callCtx_ = 0;
                    callRT(isSay && lastArg ? "perl_say_fh" : "perl_print_fh", {fh, v});
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
                /* D12: list context for the (possibly sole) print argument
                   — only when it's itself call-like, see
                   isCallLikeForContext. */
                if (isCallLikeForContext(*n.args[0])) callCtx_ = 1;
                Value *av = isExplicitArray ? emitArrayPtr(*n.args[0]) : nullptr;
                if (av) {
                    callCtx_ = 0;
                    callRT("perl_print_array", {av});
                    if (isSay) callRT("perl_print_string",
                                     {builder_.CreateGlobalString("\n", ".nl")});
                } else {
                    Value *v = emitExpr(*n.args[0]);
                    callCtx_ = 0;
                    callRT(isSay ? "perl_say" : "perl_print", {v});
                }
            } else {
                /* D12: see the filehandle-print loop above — callCtx_ must
                   be set fresh per argument, not once before the loop, and
                   only when the argument is itself call-like. */
                for (size_t i = 0; i < n.args.size(); i++) {
                    if (i > 0) callRT("perl_print_sep", {});
                    NK ak = n.args[i]->kind;
                    bool isExplicitArray = (ak == NK::ArrayVar || ak == NK::DerefArray ||
                                            ak == NK::ArraySlice || ak == NK::HashSlice ||
                                            (ak == NK::PostfixDeref && n.args[i]->sval == "all_array"));
                    if (isCallLikeForContext(*n.args[i])) callCtx_ = 1;
                    Value *av = isExplicitArray ? emitArrayPtr(*n.args[i]) : nullptr;
                    if (av) {
                        callCtx_ = 0;
                        callRT("perl_print_array", {av});
                    } else {
                        Value *v = emitExpr(*n.args[i]);
                        callCtx_ = 0;
                        callRT("perl_print", {v});
                    }
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
        /* D12: printf's format and args are all evaluated in list context
           in real Perl. callCtx_ is a one-shot flag consumed by the first
           Call node's own codegen, so it must be set fresh before each
           sub-expression, not once before the whole sequence — and only
           when that sub-expression is itself call-like, or list context
           would leak through an enclosing scalar-forcing operator (like
           `eq`/`==`) into a call nested inside it. */
        if (isCallLikeForContext(*n.left)) callCtx_ = 1;
        Value *fmt = emitExpr(*n.left);
        callCtx_ = 0;
        Value *av  = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            if (isCallLikeForContext(*a)) callCtx_ = 1;
            Value *v = emitExpr(*a);
            callCtx_ = 0;
            callRT("perl_array_push", {av, v});
        }
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
        if (!n.sval.empty()) {
            auto *lbb = labelBB(n.sval);
            if (!builder_.GetInsertBlock()->getTerminator())
                builder_.CreateBr(lbb);
            builder_.SetInsertPoint(lbb);
        }
        auto *fn    = builder_.GetInsertBlock()->getParent();
        auto *cond  = BasicBlock::Create(ctx_, "while.cond", fn);
        auto *body  = BasicBlock::Create(ctx_, "while.body", fn);
        auto *exit  = BasicBlock::Create(ctx_, "while.end",  fn);

        /* `while (...) BLOCK continue BLOCK`: `next` runs the continue
           block, then re-checks the condition. Without a continue block,
           `next` jumps straight to the condition. */
        BasicBlock *contTarget = cond;
        BasicBlock *contBB = nullptr;
        if (n.contBlock) {
            contBB = BasicBlock::Create(ctx_, "while.cont", fn);
            contTarget = contBB;
        }
        loopExits_.push_back(exit);
        loopContinues_.push_back(contTarget);
        loopRedos_.push_back(body);
        if (!n.sval.empty()) loopLabels_.push_back({n.sval, exit, contTarget, body});

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

        /* D100: same hoisting requirement as myCondPv above, but for the
           multi-variable form `while (my ($a, $b, ...) = EXPR)`. The
           parser wraps each variable as a bare NK::My node inside an
           ArrayLit on the LHS of an Assign (see parser.cpp's
           "'my ($a, $b, ...) = expr' in expression context"). Declare
           each one exactly once, here, in the loop preheader — case
           NK::Assign's ArrayLit-LHS codegen (which runs every iteration,
           inside "while.cond") looks these up via lookupVar() and reuses
           the slot instead of allocating fresh each time it runs. */
        if (n.cond && n.cond->kind == NK::Assign && n.cond->left &&
            n.cond->left->kind == NK::ArrayLit) {
            for (auto &elem : n.cond->left->args) {
                if (elem->kind != NK::My) continue;
                std::string nm = elem->name;
                if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
                auto *alloca = builder_.CreateAlloca(perlPtrTy_, nullptr, "$" + nm);
                builder_.CreateStore(perlUndef(), alloca);
                declareVar(nm, alloca);
            }
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
            builder_.CreateBr(contTarget);
        if (contBB) {
            builder_.SetInsertPoint(contBB);
            emitBlock(*n.contBlock);
            if (!builder_.GetInsertBlock()->getTerminator())
                builder_.CreateBr(cond);
        }

        loopExits_.pop_back();
        loopContinues_.pop_back();
        loopRedos_.pop_back();
        if (!n.sval.empty()) loopLabels_.pop_back();
        builder_.SetInsertPoint(exit);
        break;
    }

    case NK::DoWhile: {
        if (!n.sval.empty()) {
            auto *lbb = labelBB(n.sval);
            if (!builder_.GetInsertBlock()->getTerminator())
                builder_.CreateBr(lbb);
            builder_.SetInsertPoint(lbb);
        }
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
        if (!n.sval.empty()) {
            auto *lbb = labelBB(n.sval);
            if (!builder_.GetInsertBlock()->getTerminator())
                builder_.CreateBr(lbb);
            builder_.SetInsertPoint(lbb);
        }
        auto *fn   = builder_.GetInsertBlock()->getParent();
        auto *condBB = BasicBlock::Create(ctx_, "for.cond", fn);
        auto *bodyBB = BasicBlock::Create(ctx_, "for.body", fn);
        auto *stepBB = BasicBlock::Create(ctx_, "for.step", fn);
        auto *exit   = BasicBlock::Create(ctx_, "for.end",  fn);

        /* `for (...) BLOCK continue BLOCK`: `next` runs the continue
           block, then the step expression, then re-checks. */
        BasicBlock *contTargetF = stepBB;
        BasicBlock *contBBF = nullptr;
        if (n.contBlock) {
            contBBF = BasicBlock::Create(ctx_, "for.cont", fn);
            contTargetF = contBBF;
        }
        loopExits_.push_back(exit);
        loopContinues_.push_back(contTargetF);
        loopRedos_.push_back(bodyBB);
        if (!n.sval.empty()) loopLabels_.push_back({n.sval, exit, contTargetF, bodyBB});

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
            emitBlock(*n.body);
        }
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateBr(contTargetF);
        if (contBBF) {
            builder_.SetInsertPoint(contBBF);
            emitBlock(*n.contBlock);
            if (!builder_.GetInsertBlock()->getTerminator())
                builder_.CreateBr(stepBB);
        }

        builder_.SetInsertPoint(stepBB);
        if (n.step && n.step->kind == NK::FlatBlock) {
            /* comma-separated step (e.g. $i++, $j--) — run each item for its
               side effect only, in the loop's own scope. */
            for (auto &item : n.step->args) emitStmt(*item);
        } else if (n.step) {
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
        if (!n.sval.empty()) loopLabels_.pop_back();
        builder_.SetInsertPoint(exit);
        if (hoistedArgs) callRT("perl_array_free", {hoistedArgs});
        popScope();  /* free for-init pvs (e.g. my $i) at loop exit */
        break;
    }

    case NK::Foreach: {
        if (!n.sval.empty()) {
            auto *lbb = labelBB(n.sval);
            if (!builder_.GetInsertBlock()->getTerminator())
                builder_.CreateBr(lbb);
            builder_.SetInsertPoint(lbb);
        }
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
            /* `foreach (...) BLOCK continue BLOCK` */
            BasicBlock *contTargetA = stepBB;
            BasicBlock *contBBA = nullptr;
            if (n.contBlock) {
                contBBA = BasicBlock::Create(ctx_, "foreach.cont", fn);
                contTargetA = contBBA;
            }
            loopExits_.push_back(exit);
            loopContinues_.push_back(contTargetA);
            loopRedos_.push_back(bodyBB);
            if (!n.sval.empty()) loopLabels_.push_back({n.sval, exit, contTargetA, bodyBB});

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
                        /* Stage 23 (allflat pre-check) gated. */
                        if (!isOptStageEnabled("stage23") || !isOptStageEnabled("allflat")) continue;
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
            if (!builder_.GetInsertBlock()->getTerminator())
                builder_.CreateBr(contTargetA);
            if (contBBA) {
                builder_.SetInsertPoint(contBBA);
                emitBlock(*n.contBlock);
                if (!builder_.GetInsertBlock()->getTerminator())
                    builder_.CreateBr(stepBB);
            }
            popScope();
            /* Clean up allflat slots added by this loop level. */
            for (auto &nm : newAllflatNames) avAllflatSlots_.erase(nm);

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

        /* loop variable — Perl foreach aliases $_/the loop var to each element
           in turn, so mutating it (or $_) writes back to the source array.
           The alloca's *contents* (which PerlValue* it points at) change every
           iteration to the array's own element cell — never a private copy. */
        auto *loopVar = builder_.CreateAlloca(perlPtrTy_, nullptr, n.name);

        /* index counter */
        auto *idxAlloca = builder_.CreateAlloca(i64, nullptr, "foreach.idx");
        builder_.CreateStore(ConstantInt::get(i64, 0), idxAlloca);

        auto *condBB = BasicBlock::Create(ctx_, "foreach.cond", fn);

        /* `foreach (...) BLOCK continue BLOCK` */
        BasicBlock *contTargetB = stepBB;
        BasicBlock *contBBB = nullptr;
        if (n.contBlock) {
            contBBB = BasicBlock::Create(ctx_, "foreach.cont", fn);
            contTargetB = contBBB;
        }
        loopExits_.push_back(exit);
        loopContinues_.push_back(contTargetB);
        loopRedos_.push_back(bodyBB);
        if (!n.sval.empty()) loopLabels_.push_back({n.sval, exit, contTargetB, bodyBB});

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
        /* Borrow (no clone) the array's own element cell — this IS the
           aliasing: any perl_assign / ++ / etc. on the loop var mutates
           the array in place. Never free the result (borrow contract). */
        Value *elemRef = callRT("perl_array_get_ref", {tmpArr, idx});
        builder_.CreateStore(elemRef, loopVar);

        emitBlock(*n.body);
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateBr(contTargetB);
        if (contBBB) {
            builder_.SetInsertPoint(contBBB);
            emitBlock(*n.contBlock);
            if (!builder_.GetInsertBlock()->getTerminator())
                builder_.CreateBr(stepBB);
        }
        popScope();

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
        if (ownsTmpArr) callRT("perl_array_free", {tmpArr});
        break;
    }

    case NK::LabelStmt: {
        auto *bb = labelBB(n.name);
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateBr(bb);
        builder_.SetInsertPoint(bb);
        if (n.body) emitStmt(*n.body);
        break;
    }

    case NK::Goto: {
        if (n.sval == "label" || (n.sval == "expr" && n.left && n.left->kind == NK::StringLit)) {
            std::string lab = (n.sval == "label") ? n.name : n.left->sval;
            auto *bb = labelBB(lab);
            builder_.CreateBr(bb);
            auto *dead = BasicBlock::Create(ctx_, "goto.dead",
                                            builder_.GetInsertBlock()->getParent());
            builder_.SetInsertPoint(dead);
            break;
        }
        if (n.sval == "sub") {
            /* goto &NAME — replace current frame: call NAME with current @_ */
            Value *argsArr = lookupArray("_");
            if (!argsArr) argsArr = callRT("perl_array_new", {});
            auto *i32Ty = Type::getInt32Ty(ctx_);
            Value *ctxVal = currentSubNeedsWantarray_
                ? callRT("perl_current_wantarray_ctx", {})
                : ConstantInt::get(i32Ty, 0);
            Value *retVal;
            if (auto *fn = mod_->getFunction(subLLVMName(n.name))) {
                retVal = builder_.CreateCall(fn, {argsArr, ctxVal});
            } else {
                Value *nameStr = builder_.CreateGlobalStringPtr(n.name);
                retVal = callRT("perl_call_named_sub", {nameStr, argsArr, ctxVal});
            }
            if (currentSubNeedsWantarray_) callRT("perl_pop_wantarray", {});
            if (localDepthAlloca_) {
                Value *depth = builder_.CreateLoad(i32Ty, localDepthAlloca_);
                callRT("perl_local_restore_to", {depth});
            }
            builder_.CreateRet(retVal);
            auto *dead = BasicBlock::Create(ctx_, "goto.sub.dead",
                                            builder_.GetInsertBlock()->getParent());
            builder_.SetInsertPoint(dead);
            break;
        }
        if (n.sval == "expr" && n.left) {
            /* computed label: only string literals handled above; runtime
               string → look up is not wired; jump to undef path. */
            Value *lv = emitExpr(*n.left);
            freeIfOwned(lv);
            builder_.CreateRet(perlUndef());
            break;
        }
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
            /* return list-producing expr — wrap for list/scalar context at runtime */
            Value *av = emitArrayPtr(*n.left);
            if (!av) av = callRT("perl_array_new", {});
            /* grep/map return COUNT in scalar context (not last element);
               sort returns undef in scalar context (D29). */
            if (n.left->kind == NK::GrepFunc || n.left->kind == NK::MapFunc ||
                n.left->kind == NK::SortFunc) {
                auto *i32Ty = Type::getInt32Ty(ctx_);
                Value *ctx = callRT("perl_current_wantarray_ctx", {});
                Value *isList = builder_.CreateICmpEQ(ctx, ConstantInt::get(i32Ty, 1));
                Value *listResult = callRT("perl_array_to_list_return", {av});
                Value *scalarResult = (n.left->kind == NK::SortFunc) ? perlUndef()
                                     : callRT("perl_array_len", {av});
                v = builder_.CreateSelect(isList, listResult, scalarResult);
            } else {
                v = callRT("perl_array_to_list_return", {av});
            }
        } else if (!n.left) {
            /* D115: bare `return;` must yield an empty LIST in list
               context (real Perl: `my @r = f();` with `sub f { return;
               }` gives `scalar(@r) == 0`), not a 1-element (undef) list
               — scalar/void context still gets plain undef. Checked at
               runtime the same way the grep/map/sort list-producing
               branch above does, since the caller's context isn't known
               at compile time here. */
            Value *emptyAv = callRT("perl_array_new", {});
            Value *listResult = callRT("perl_array_to_list_return", {emptyAv});
            auto *i32Ty = Type::getInt32Ty(ctx_);
            Value *ctx = callRT("perl_current_wantarray_ctx", {});
            Value *isList = builder_.CreateICmpEQ(ctx, ConstantInt::get(i32Ty, 1));
            v = builder_.CreateSelect(isList, listResult, perlUndef());
            } else {
                int savedCtx = callCtx_;
                callCtx_ = -1; /* D87: return EXPR inherits caller context */
                v = emitExpr(*n.left);
                callCtx_ = savedCtx;
            }
        /* `return` inside eval{} exits just that eval block with this value
           (real Perl semantics — NOT die, and NOT a return from any
           enclosing sub even if one exists; execution resumes after the
           eval). Check this BEFORE the sub/main-level return handling
           below, which is for when there's no enclosing eval at all. No
           local()-restore-to-sub-depth or scope cleanup here: those are
           calibrated for exiting the whole function, which this doesn't do
           — matching the existing last/next precedent of no cleanup at the
           branch site for an early exit that stays within the same
           function. */
        if (!evalReturnTargets_.empty()) {
            auto &target = evalReturnTargets_.back();
            builder_.CreateStore(v, target.resultAlloca);
            builder_.CreateBr(target.endBB);
            break;
        }
        /* restore any local()s before returning; clone retval first so
           restore doesn't clobber the in-place PerlValue we're returning */
        if (localDepthAlloca_) {
            auto *i32Ty = Type::getInt32Ty(ctx_);
            Value *depth = builder_.CreateLoad(i32Ty, localDepthAlloca_);
            Value *cloned = callRT("perl_clone", {v});
            freeIfOwned(v);
            if (currentSubNeedsWantarray_) callRT("perl_pop_wantarray", {});
            callRT("perl_local_restore_to", {depth});
            v = cloned;
        }
        emitScopeCleanup();  /* free tracked my-var pvs in all active scopes */
        /* In the main function (which returns i32), we can't emit `ret ptr`.
           With no enclosing eval (checked above) and no enclosing sub,
           `return` at the top level is equivalent to `die` in Perl
           semantics. Call perl_die to longjmp back to the nearest eval's
           catch point (or terminate the process if there is none). */
         if (currentFn_ && currentFn_->getReturnType()->isIntegerTy() &&
             currentFn_->getName() == "main") {
             auto *i32Ty = Type::getInt32Ty(ctx_);
             Value *fileStr = builder_.CreateGlobalStringPtr(sourceFile_, "die.file");
             Value *lineVal = ConstantInt::get(i32Ty, n.line);
             callRT("perl_die", {v, fileStr, lineVal});
        } else {
            builder_.CreateRet(v);
        }
        break;
    }

    case NK::LocalGlob: {
        /* W29: `local *_ = ...` / `local $_ = ...` — localize the GLOBAL
           $_ cell. The sub's lexical $_ shadow (if present) is saved and
           assigned too: reads inside this sub see the localized value,
           and the depth-based restore puts both the shadow and the cell
           back at scope exit (matching real perl's restore-after-local). */
        Value *shadow = lookupVar("_");
        if (shadow) {
            /* The sub's lexical $_ shadow is localized too: both storages
               are saved (depth-restore puts both back) and assigned. */
            Value *shadowPv = builder_.CreateLoad(perlPtrTy_, shadow, "lg.shadow");
            callRT("perl_local_save", {shadowPv});
        }
        Value *cell = callRT("perl_get_dollar_under", {});
        callRT("perl_local_save", {cell});
        if (n.left) {
            Value *rhs = emitExpr(*n.left);
            /* `local *_ = \join(...)`: the RHS is a REFERENCE — the glob's
               scalar slot becomes an ALIAS of the referent, so deref
               before assigning. Non-ref RHS (plain `local $_ = v`)
               assigns directly. */
            Value *val = callRT("perl_deref_if_ref", {rhs});
            if (shadow)
                callRT("perl_assign",
                       {builder_.CreateLoad(perlPtrTy_, shadow, "lg.shadow2"), val});
            callRT("perl_assign", {cell, val});
            callRT("perl_free", {val});
            freeIfOwned(rhs);
        }
        return;
    }

    case NK::LocalStmt: {
        /* save current value, optionally assign new one.
           D41: also local $h{key} / local $arr[idx] via element lvalues. */
        Value *pv = nullptr;
        if (n.sval == "hash_elem" && n.right) {
            Value *hv = lookupHash(n.name);
            if (!hv) break;
            if (Value *kp = constKeyPtr(*n.right, builder_))
                pv = callRT("perl_hash_lvalue_str", {hv, kp});
            else {
                Value *key = emitExpr(*n.right);
                pv = callRT("perl_hash_lvalue_sv", {hv, key});
                freeIfOwned(key);
            }
        } else if (n.sval == "array_elem" && n.right) {
            Value *av = lookupArray(n.name);
            if (!av) break;
            Value *idx = emitIdx(*n.right);
            pv = callRT("perl_array_lvalue", {av, idx});
        } else if (n.name == "/")   pv = callRT("perl_get_input_sep",    {});
        else if (n.name == "!") pv = callRT("perl_get_dollar_bang",{});
        else if (n.name == ".") pv = callRT("perl_get_dollar_dot",  {});
        else if (n.name == ",") pv = callRT("perl_get_dollar_comma",{});
        else if (n.name == "\\") pv = callRT("perl_get_dollar_bsl", {});
        else if (n.name == "&") pv = callRT("perl_get_dollar_amp",  {});
        else if (n.name == "?") pv = callRT("perl_get_dollar_question", {});
        else {
            Value *slot = lookupVar(n.name);
            if (!slot) {
                /* D110: `local $Other::x = ...` — same storage-selection
                   order as emitLValue's qualified branch: fileScalarGlobals_
                   slot first, else the glob registry cell in a temp alloca.
                   perl_local_save snapshots the cell's *contents*, so the
                   restore writes back through the registered cell — the
                   pre-sub value is visible again after the sub returns. */
                if (isQualifiedName(n.name)) {
                    auto qgit = fileScalarGlobals_.find(n.name);
                    if (qgit != fileScalarGlobals_.end()) {
                        slot = qgit->second;
                    } else {
                        Value *qkey = builder_.CreateGlobalStringPtr(n.name);
                        Value *cell = callRT("perl_glob_get_scalar", {qkey});
                        slot = builder_.CreateAlloca(perlPtrTy_, nullptr,
                                                     "gq." + n.name);
                        builder_.CreateStore(cell, slot);
                    }
                } else {
                    Value *uv = callRT("perl_alloc_undef", {});
                    slot = builder_.CreateAlloca(perlPtrTy_, nullptr, ("$" + n.name).c_str());
                    builder_.CreateStore(uv, slot);
                    declareVar(n.name, slot);
                }
            }
            pv = builder_.CreateLoad(perlPtrTy_, slot);
        }
        if (!pv) break;
        callRT("perl_local_save", {pv});
        if (n.left) {
            Value *rhs = emitExpr(*n.left);
            callRT("perl_assign", {pv, rhs});
        } else {
            /* bare `local $x;` / `local $h{k}` / `local $/;` — real Perl
               assigns UNDEF (that's the whole point of `local $/;` for
               slurp mode). */
            callRT("perl_assign", {pv, perlUndef()});
        }
        break;
    }

    case NK::StateDecl: {
        auto *fn  = builder_.GetInsertBlock()->getParent();
        auto *ptrTy = perlPtrTy_;
        auto *i8Ty  = Type::getInt8Ty(ctx_);
        /* module-level globals: the PerlValue* and an init flag */
        std::string gname = "state.ptr." + std::to_string(stateSeq_);
        std::string gflag = "state.init." + std::to_string(stateSeq_++);
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
        std::string endName = "perl_end_" + std::to_string(endSeq_++);
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

/* W22: the symbolic-reference half of ${ EXPR } (read form). EXPR's
   string value names a global variable, resolved through the process
   glob registry (D110's machinery) with the current package as the
   default qualifier — real Perl resolves a symbolic ref without an
   explicit package in the current package. main:: names use the bare
   key directly (the registry keys bare names under main:: for D110).
   Returns the registry CELL (the stable PerlValue* the global aliases),
   so a read sees in-place writes made through the LLVM global and the
   lvalue path can hand the same cell to the generic assign machinery. */
Value *CodeGen::emitSymbolicDeref(const Node &n) {
    int savedCtx = callCtx_;
    callCtx_ = -1;
    Value *namePv = emitExpr(*n.left);
    callCtx_ = savedCtx;
    Value *nm = callRT("perl_to_string_dup", {namePv});
    freeIfOwned(namePv);
    Value *key;
    if (!currentPackage_.empty() && currentPackage_ != "main") {
        Value *dot = builder_.CreateGlobalStringPtr("::", "sd.dot");
        Value *pkg = builder_.CreateGlobalStringPtr(currentPackage_, "sd.pkg");
        /* key = pkg . "::" . nm — via perl_concat */
        Value *p1 = callRT("perl_concat", {pkg, dot});
        key = callRT("perl_concat", {p1, nm});
        freeIfOwned(p1);
    } else {
        /* main:: — the glob registry already keys bare names under
           main:: for D110; use the bare name directly */
        key = nm;
    }
    Value *cell = callRT("perl_glob_get_scalar", {key});
    builder_.CreateCall(getRTFunc("free"), {nm});
    return cell;
}

Value *CodeGen::emitExpr(const Node &n) {
    if (debug_ && n.line > 0) {
        builder_.SetCurrentDebugLocation(getDebugLoc(n.line, currentSP_));
    }
    switch (n.kind) {
    case NK::Block:     return emitBlockLast(n);   /* do { BLOCK } in expr ctx */
    case NK::UndefLit:  return perlUndef();
    case NK::IntLit:    return perlInt(n.ival);
    case NK::FloatLit:  return perlFloat(n.fval);
    case NK::StringLit: {
        Value *s = perlStr(n.sval);
        if (n.ival) callRT("perl_pv_flag_utf8", {s});
        return s;
    }

    case NK::ScalarVar: {
        if (n.name == "!")  return callRT("perl_get_dollar_bang",  {});
        if (n.name == "/")  return callRT("perl_get_input_sep",    {});
        if (n.name == ".")  return callRT("perl_get_dollar_dot",   {});
        if (n.name == ",")  return callRT("perl_get_dollar_comma", {});
        if (n.name == "\\") return callRT("perl_get_dollar_bsl",   {});
        if (n.name == "&")  return callRT("perl_get_dollar_amp",   {});
        if (n.name == "?")  return callRT("perl_get_dollar_question", {});
        /* $AUTOLOAD — set by dispatch when AUTOLOAD is called */
        if (n.name == "AUTOLOAD") return callRT("perl_get_autoload_name", {});
        if (n.name == "ARGV") return callRT("perl_get_dollar_argv", {});
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
        if (!slot && n.name == "_") {
            /* W29: bare $_ with no lexical in scope reads the GLOBAL $_
               cell (real perl semantics: local $_ / local *_ in a caller
               is visible to called subs). The cell IS the PerlValue
               (s_dollar_at-style stable struct) — wrap it in a slot the
               generic load path can deref. */
            Value *cell = callRT("perl_get_dollar_under", {});
            auto *hold = builder_.CreateAlloca(perlPtrTy_, nullptr, "global.under");
            builder_.CreateStore(cell, hold);
            slot = hold;
        }
        if (!slot) {
            /* D110: a package-qualified name is a true global. Prefer
               D112's fileScalarGlobals_ entry (module-`our` unification),
               then the runtime glob registry. Must come BEFORE isGlobName:
               a qualified non-filehandle name ($Other::x) must not be
               swallowed by the typeglob branch's bare-name matching, and
               glob_get's bare/main:: fallback could cross-wire two
               same-bare-named packages. */
            if (isQualifiedName(n.name)) {
                auto git = fileScalarGlobals_.find(n.name);
                if (git != fileScalarGlobals_.end())
                    return builder_.CreateLoad(perlPtrTy_, git->second, n.name);
                Value *key = builder_.CreateGlobalStringPtr(n.name);
                return callRT("perl_glob_get_scalar", {key});
            }
            if (isGlobName(n.name)) {
                Value *key = builder_.CreateGlobalStringPtr(globBareName(n.name));
                return callRT("perl_glob_get_scalar", {key});
            }
            return perlUndef();
        }
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
                Value *loaded = callRT("perl_atomic_load", {pv});
                callRT("perl_promote_ref_array", {loaded});
                return loaded;
            }
        }
        {
            /* D105: reading a bare scalar variable is the single choke
               point every "hand this existing value to something that
               will clone/store it elsewhere" idiom goes through — push(),
               sub-call args, hash/array-element assignment, return, list
               literals, etc. If the variable currently holds a
               FLAT_ARRAY/FLOAT_PAIR (Stage 22/23 compact anon-array-ref),
               promote it to a real REF_ARRAY in place *before* handing it
               out, so every alias/clone made from this read shares one
               PerlArray instead of silently forking (D105). No-op for
               every other tag (int/float/string/REF_ARRAY/undef/...) — one
               cheap tag check. Values that stay unboxed the whole time
               (lookupIntVar/lookupFloatVar above) never reach here, and a
               fresh literal (AnonArray et al.) is never read back through
               a ScalarVar before its first real use, so the Stage 22/23
               fast path for freshly-constructed numeric rows/matrices
               (e.g. tests/d96_flat_row_op_assign.pl, tests/d98_flat_row_2d.pl)
               is unaffected — see perl_promote_ref_array. */
            Value *pv = builder_.CreateLoad(perlPtrTy_, slot, n.name);
            callRT("perl_promote_ref_array", {pv});
            return pv;
        }
    }

    case NK::ArrayElem: {
        Value *av = lookupArray(n.name);
        if (!av) return perlUndef();
        Value *elem = callRT("perl_array_get_ref", {av, emitIdx(*n.left)});
        /* D106: same bug/fix class as D105's ScalarVar read (see the
           detailed comment there) — reading an array element that holds
           a FLAT_ARRAY/FLOAT_PAIR anon-array-ref (Stage 22/23's compact
           literal storage) must promote it to a real REF_ARRAY *before*
           handing it out, so a second alias made from this read (`my $y
           = $arr[0]; $y->[0] = 99;`) shares the same PerlArray instead of
           silently forking. Confirmed NOT to touch the `$arr[$i] op=
           rhs` compound-assign fast path or any 2D ArrowDeref-chain fast
           path — both are separate `case`/dispatch branches that call
           `perl_array_get_ref` directly themselves rather than routing
           through this one, so this promotion (a no-op for every other
           tag, exactly like D105's) cannot reach those FLAT_ARRAY-
           sensitive branches. */
        callRT("perl_promote_ref_array", {elem});
        return elem;
    }

    case NK::ArrayVar: {
        /* @arr in scalar context — return the element count as PerlValue* */
        Value *av = lookupArray(n.name);
        return av ? callRT("perl_array_len", {av}) : perlInt(0);
    }

    case NK::ArrayLit: {
        /* Scalar/comma context of a parenthesized list: last element
           (D95 companion — single-element `(expr)` is now ArrayLit, not
           unwrapped). List context uses emitArrayPtr, not emitExpr. */
        if (n.args.empty()) return perlUndef();
        Value *last = nullptr;
        for (auto &elem : n.args) {
            if (last) freeIfOwned(last);
            last = emitExpr(*elem);
        }
        return last ? last : perlUndef();
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
        if (n.sval.empty() || n.sval == "ARGV")
            return callRT("perl_readline_argv", {});
        if (n.sval == "STDIN")
            return callRT("perl_readline_stdin", {});
        if (n.sval == "DATA") {
            Value *fh = callRT("perl_get_data_fh", {});
            return callRT("perl_readline", {fh});
        }
        if (auto *slot = lookupVar(n.sval)) {
            Value *fh = builder_.CreateLoad(perlPtrTy_, slot);
            return callRT("perl_readline", {fh});
        }
        if (isGlobName(n.sval) || !n.sval.empty()) {
            globNames_.insert(n.sval);
            Value *key = builder_.CreateGlobalStringPtr(n.sval);
            Value *fh = callRT("perl_glob_get_scalar", {key});
            return callRT("perl_readline", {fh});
        }
        return perlUndef();
    }

    case NK::OpenFunc: {
        Value *fh_pv = nullptr;
        if (n.sval == "bare") {
            globNames_.insert(n.name);
            Value *key = builder_.CreateGlobalStringPtr(n.name);
            fh_pv = callRT("perl_glob_get_scalar", {key});
        } else {
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
            fh_pv = builder_.CreateLoad(perlPtrTy_, slot);
        }
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
        /* D101: scalar-context `each %hash` must return the KEY (or
           undef once exhausted) — this used to return
           perl_array_len(av), the *count* of the [key,val] pair array
           (0, 1, or 2), never the key itself. Masked in casual testing
           because a truthy 2 happens to make `while (each ...)` iterate
           the right *number* of times even though every scalar `$k` was
           wrong. perl_array_get already returns undef for an
           out-of-range index, matching the post-exhaustion case (an
           empty pair array). */
        Value *hv = lookupHash(n.name);
        if (!hv) return perlUndef();
        Value *av = callRT("perl_each_hash", {hv});
        Value *idx0 = ConstantInt::get(Type::getInt64Ty(ctx_), 0);
        Value *key = callRT("perl_array_get", {av, idx0});
        callRT("perl_array_free", {av});
        return key;
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
        Value *modStr = builder_.CreateGlobalStringPtr(n.sval);
        return callRT("perl_runtime_require", {modStr});
    }

    case NK::DoFile:
        return callRT("perl_do_file", {emitExpr(*n.left)});

    case NK::Redo: {
        if (!loopRedos_.empty()) {
            builder_.CreateBr(loopRedos_.back());
        }
        /* Block is already terminated by the branch — return a null constant
           instead of calling perl_alloc_undef() to avoid emitting code after
           the terminator. */
        return ConstantPointerNull::get(perlPtrTy_);
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
         auto *i32Ty = Type::getInt32Ty(ctx_);
         Value *msg = n.left ? emitExpr(*n.left) : perlStr("Died");
         Value *fileStr = builder_.CreateGlobalStringPtr(sourceFile_, "die.file");
         Value *lineVal = ConstantInt::get(i32Ty, n.line);
         callRT("perl_die", {msg, fileStr, lineVal});
         /* perl_die is NoReturn — no code reachable after it */
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
        /* D130: 'my @arr = expr' / 'my %hash = expr' in expression
           context (e.g. if (my @rows = fetch())) — boolean/scalar-
           context value is the element count, matching how a bare
           @arr/%hash already behaves in boolean context elsewhere. */
        if (!n.name.empty() && n.name[0] == '@') {
            std::string nm = n.name.substr(1);
            if (Value *av = lookupArray(nm)) return callRT("perl_array_len", {av});
        }
        if (!n.name.empty() && n.name[0] == '%') {
            std::string nm = n.name.substr(1);
            if (Value *hv = lookupHash(nm)) return callRT("perl_hash_size", {hv});
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
                    Value *next = builder_.CreateAdd(cur, delta, isInc ? "preinc" : "predec");
                    builder_.CreateStore(next, ia);
                    bool isPre = (n.sval == "pre++" || n.sval == "pre--");
                    return boxI64(isPre ? next : cur);
                }
                /* D135: unboxed float variable — same shape as the int twin
                   above. A float-promoted variable passed here would
                   otherwise fall to emitIncTarget → emitExpr(ScalarVar) →
                   boxF64(load), mutate the disposable boxed temp with
                   perl_inc, and leave the alloca (and therefore every
                   later read of the variable) untouched — an infinite loop
                   in `while ($i < N) { ...; $i++ }` with a float-promoted
                   counter. Numeric ++/-- on a double is exact, matching
                   Perl (an NV counter holds 1.0 after ++). */
                if (Value *fa = lookupFloatVar(nm)) {
                    auto *f64 = Type::getDoubleTy(ctx_);
                    Value *cur = builder_.CreateLoad(f64, fa);
                    bool isInc = (n.sval == "pre++" || n.sval == "post++");
                    Value *next = builder_.CreateFAdd(cur,
                        ConstantFP::get(f64, isInc ? 1.0 : -1.0),
                        isInc ? "fpreinc" : "fpredec");
                    builder_.CreateStore(next, fa);
                    bool isPre = (n.sval == "pre++" || n.sval == "pre--");
                    return boxF64(isPre ? next : cur);
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
        /* *NAME = ... — glob slot assignment */
        if (n.left && n.left->kind == NK::Typeglob) {
            Value *key = builder_.CreateGlobalStringPtr(n.left->name);
            if (n.right && n.right->kind == NK::RefSub) {
                auto *fn = mod_->getFunction(subLLVMName(n.right->name));
                if (fn) {
                    callRT("perl_register_method", {key, fn});
                    Value *mainKey = builder_.CreateGlobalStringPtr("main::" + n.left->name);
                    callRT("perl_register_method", {mainKey, fn});
                }
            } else if (n.right && n.right->kind == NK::RefScalar && n.right->left) {
                Value *slot = emitLValue(*n.right->left);
                Value *cell = slot ? builder_.CreateLoad(perlPtrTy_, slot)
                                   : emitExpr(*n.right->left);
                callRT("perl_glob_set_scalar", {key, cell});
            } else if (n.right && n.right->kind == NK::RefArray) {
                Value *av = lookupArray(n.right->name);
                if (!av) av = callRT("perl_array_new", {});
                callRT("perl_glob_set_array", {key, av});
            } else if (n.right && n.right->kind == NK::RefHash) {
                Value *hv = lookupHash(n.right->name);
                if (!hv) hv = callRT("perl_hash_new", {});
                callRT("perl_glob_set_hash", {key, hv});
            } else if (n.right && n.right->kind == NK::Typeglob) {
                Value *src = builder_.CreateGlobalStringPtr(n.right->name);
                callRT("perl_glob_copy", {key, src});
            } else if (n.right) {
                Value *rhs = emitExpr(*n.right);
                callRT("perl_glob_assign", {key, rhs});
                freeIfOwned(rhs);
            }
            return perlStr("*" + currentPackage_ + "::" + n.left->name);
        }
        /* vec($str, off, bits) = val — lvalue bit-vector store */
        if (n.left->kind == NK::Call && n.left->name == "vec" &&
            n.left->args.size() >= 3) {
            Value *str = emitExpr(*n.left->args[0]);
            Value *off = emitExpr(*n.left->args[1]);
            Value *bits = emitExpr(*n.left->args[2]);
            Value *val = emitExpr(*n.right);
            Value *r = callRT("perl_vec_set", {str, off, bits, val});
            freeIfOwned(val);
            return r;
        }
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
            callCtx_ = 1;
            Value *rhsArr = emitArrayPtr(*n.right);
            if (!rhsArr) {
                /* RHS collapsed to a bare scalar expression — e.g. `(10)` with
                   no comma parses as just the inner IntLit, not an ArrayLit —
                   but list-assignment context still treats it as a one-element
                   list. Wrap it in a real PerlArray instead of passing a
                   PerlValue* where perl_array_get_ref expects a PerlArray*
                   (silent type confusion / OOB read otherwise). Matches the
                   pattern already used for @arr=RHS and lvalue slices below. */
                rhsArr = callRT("perl_array_new", {});
                Value *v = emitExpr(*n.right);
                callRT("perl_array_push", {rhsArr, v});
                freeIfOwned(v);
            }
            callCtx_ = 0;
            bool fromUnderbar = (n.right->kind == NK::ArrayVar && n.right->name == "_");
            for (size_t i = 0; i < n.left->args.size(); i++) {
                /* Trailing @rest / %rest — slurps every remaining RHS element
                   (Perl list-assignment semantics: an array/hash in the LHS
                   list consumes the rest of the list, not just one item). */
                if (n.left->args[i]->kind == NK::ArrayVar) {
                    if (Value *targetAv = lookupArray(n.left->args[i]->name)) {
                        callRT("perl_array_clear", {targetAv});
                        Value *startIdx = ConstantInt::get(Type::getInt64Ty(ctx_), (long long)i);
                        callRT("perl_array_extend_from", {targetAv, rhsArr, startIdx});
                    }
                    continue;
                }
                if (n.left->args[i]->kind == NK::HashVar) {
                    if (Value *targetHv = lookupHash(n.left->args[i]->name)) {
                        Value *tmpArr = callRT("perl_array_new", {});
                        Value *startIdx = ConstantInt::get(Type::getInt64Ty(ctx_), (long long)i);
                        callRT("perl_array_extend_from", {tmpArr, rhsArr, startIdx});
                        callRT("perl_hash_from_list", {targetHv, tmpArr});
                        callRT("perl_array_free", {tmpArr});
                    }
                    continue;
                }
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
                /* D100: the expression-context parse of `my ($a, $b) = EXPR`
                   (used inside a while/if condition — see parser.cpp's
                   "'my ($a, $b, ...) = expr' in expression context") wraps
                   each variable as a bare NK::My node with no separate
                   declaration statement, unlike the statement-level
                   `my ($a, $b) = EXPR;` form (which desugars into a
                   FlatBlock of individual My-decl statements followed by
                   an Assign whose LHS is already-declared ScalarVars).
                   emitLValue() only understands ScalarVar/DollarAt, so an
                   NK::My element here fell through its `default: return
                   nullptr`, silently skipping both the declaration AND
                   the assignment — `while (my ($k,$v) = each %h)` used to
                   loop the correct number of times (once the D100 fix
                   above made the loop condition's truth value correct)
                   but $k/$v stayed undef for the whole loop.

                   lookupVar() first: when this condition is a `while`
                   loop, `While`'s own codegen (below) hoists one alloca
                   per variable *before* the loop starts and pre-declares
                   it — exactly like the existing single-variable
                   `myCondPv` hoist — because this whole per-element loop
                   lives inside the "while.cond" basic block, which is a
                   loop back-edge target: an `alloca` placed directly in
                   it would re-execute (and leak stack) on every
                   iteration, not just once. Reusing an already-hoisted
                   slot here keeps this code correct for both `while`
                   (hoisted) and a one-shot `if (my ($k,$v) = ...)`
                   (never hoisted, so falls to the fresh-alloca branch —
                   safe since it only runs once either way). */
                Value *slot;
                if (n.left->args[i]->kind == NK::My) {
                    std::string nm = n.left->args[i]->name;
                    if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
                    slot = lookupVar(nm);
                    if (!slot) {
                        slot = builder_.CreateAlloca(perlPtrTy_, nullptr, "$" + nm);
                        builder_.CreateStore(perlUndef(), slot);
                        declareVar(nm, slot);
                    }
                } else {
                    slot = emitLValue(*n.left->args[i]);
                }
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
            /* D100 (was Stage 27b's "list assignment is void, return null"):
               real Perl list assignment evaluates, in scalar/boolean
               context, to the COUNT of elements on the RHS — e.g.
               `while (my ($k,$v) = each %h)` keeps looping exactly as
               long as each() returns a non-empty pair, and stops when it
               returns count 0. Returning a null/void PerlValue* here made
               `perl_is_true(null)` always false, so that idiom (and any
               `if`/`while`/`until` guarded by a list assignment) never
               ran its body at all. perl_array_len is already a registered
               "owned temp" (see isOwnedTemp), so the existing generic
               ExprStmt/emitBlockLast/If/While handling for an owned
               result already frees/clones it correctly — no other call
               site needs to change. A bare statement like
               `my ($a,$b) = @list;` just gets one extra harmless
               alloc+free of the count via ExprStmt's freeIfOwned. */
            return callRT("perl_array_len", {rhsArr});
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
                callCtx_ = 1;
                Value *av_rhs = emitArrayPtr(*n.right);
                callCtx_ = 0;
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
                          callCtx_ = 1;
                          Value *rhsVal = emitExpr(*n.right);
                          callRT("perl_array_push_list_or_scalar", {tmp, rhsVal});
                          callCtx_ = 0;
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
        /* W22: ${$ref} = val — block-form spelling of `$$ref = val`.
           The parser routes any `${ ... }` to SymbolicDeref, so a REF
           block-form lvalue lands here; deref the referent cell and
           perl_assign through it (same semantics as the DerefScalar
           branch immediately above). A string-valued EXPR instead goes
           through emitLValue's SymbolicDeref lvalue case (glob registry).
           NOTE: the ref form is detected at runtime (the tag check inside
           emitLValue), so this static branch only catches the common
           compile-time-obvious `${$ref}` spelling where the inner node
           is a ScalarVar; general expressions fall to emitLValue. */
        if (n.left->kind == NK::SymbolicDeref && n.left->left &&
            n.left->left->kind == NK::ScalarVar) {
            Value *ref = emitExpr(*n.left->left);
            Value *isRef = callRT("perl_su_reftype", {ref});
            Value *hasRef = builder_.CreateICmpNE(
                builder_.CreateCall(getRTFunc("perl_is_true"), {isRef}),
                ConstantInt::get(Type::getInt32Ty(ctx_), 0), "sd.as.isref");
            auto *curFn = builder_.GetInsertBlock()->getParent();
            auto *refBB = BasicBlock::Create(ctx_, "sd.as.ref", curFn);
            auto *genBB = BasicBlock::Create(ctx_, "sd.as.gen", curFn);
            auto *joinBB = BasicBlock::Create(ctx_, "sd.as.join", curFn);
            builder_.CreateCondBr(hasRef, refBB, genBB);
            builder_.SetInsertPoint(refBB);
            Value *rhsR = emitExpr(*n.right);
            Value *targetR = callRT("perl_deref_scalar", {ref});
            callRT("perl_assign", {targetR, rhsR});
            builder_.CreateBr(joinBB);
            auto *refBBp = builder_.GetInsertBlock();
            builder_.SetInsertPoint(genBB);
            Value *rhsG = emitExpr(*n.right);
            Value *lhsG = emitLValue(*n.left);
            if (lhsG) {
                Value *lhsValG = builder_.CreateLoad(perlPtrTy_, lhsG);
                callRT("perl_assign", {lhsValG, rhsG});
            }
            builder_.CreateBr(joinBB);
            auto *genBBp = builder_.GetInsertBlock();
            builder_.SetInsertPoint(joinBB);
            auto *phi = builder_.CreatePHI(perlPtrTy_, 2, "sd.as.res");
            phi->addIncoming(rhsR, refBBp);
            phi->addIncoming(rhsG, genBBp);
            return phi;
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
                /* autovivify $h{a}{b}[i] = val or deeper chains — base is
                    itself another ArrowDeref rooted in a hash/array element
                    (2+ levels before this one), OR (D50) a chain rooted in
                    a bare scalar variable where every level up to here is
                    a hash-key access, e.g. $ref->{a}[i] — safe, since that
                    only ever calls perl_hash_autoviv_array (hash-key
                    based), never perl_array_autoviv_array (the one with
                    the FLAT_ARRAY-destruction risk, D40). A scalar-ref-
                    rooted chain with an array-index level anywhere, like
                    $ref->[0][1], is NOT routed here — handled below. */
                if (n.left->left->kind == NK::ArrowDeref &&
                    (isElemRootedChain(*n.left->left) ||
                     isScalarRootedAllHashChain(*n.left->left))) {
                    Value *innerAv = emitAutovivContainer(*n.left->left, false);
                    if (!innerAv) return perlUndef();
                    callRT("perl_array_set", {innerAv, idx, rhs});
                    return rhs;
                }
                /* D50: chains with array-index levels that need special handling.
                    This includes scalar-ref-rooted chains like $ref->[0][1],
                    and mixed chains like $ref->{a}[0][1]. */
                if (needsScalarRootedAutoviv(*n.left)) {
                    return emitScalarRootedAutovivAssign(*n.left, false, rhs);
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
                } else if (n.left->left->kind == NK::ArrowDeref &&
                            (isElemRootedChain(*n.left->left) ||
                             isScalarRootedAllHashChain(*n.left->left))) {
                    /* $h{a}{b}{c} = val or deeper chains — base is itself
                        another ArrowDeref rooted in a hash/array element
                        (2+ levels before this one), OR (D50) a chain
                        rooted in a bare scalar variable holding a ref
                        where every level is a hash-key access, e.g.
                        $ref->{a}{b} — safe to autoviv all the way down
                        since a hash has no FLAT_ARRAY-equivalent
                        optimization to accidentally destroy (see
                        isScalarRootedAllHashChain's comment). A chain
                        with an array-index level anywhere still falls to
                        the plain-deref branch below, unchanged — that
                        case keeps the D40-established FLAT_ARRAY safety
                        margin (TESTS.md's D50 entry). */
                    hv = emitAutovivContainer(*n.left->left, true);
                    if (!hv) return perlUndef();
                } else if (needsScalarRootedAutoviv(*n.left)) {
                    /* D50: chain with array-index level, hash-key target:
                        e.g. $ref->[0]{k} = val or $ref->{a}[0]{k} = val. */
                    return emitScalarRootedAutovivAssign(*n.left, true, rhs);
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
            callCtx_ = 1;
            Value *rhsArr = emitArrayPtr(*n.right);
            callCtx_ = 0;
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
            callCtx_ = 1;
            Value *rhsArr = emitArrayPtr(*n.right);
            callCtx_ = 0;
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
        /* D52: $@ = "..." — $@ parses to its own NK::DollarAt node (not
           NK::ScalarVar with name "@" like $/ and $! above), so it fell
           through emitLValue's default case (returns nullptr for anything
           it doesn't recognize) and the assignment silently did nothing. */
        if (n.left->kind == NK::DollarAt) {
            Value *rhs = emitExpr(*n.right);
            callRT("perl_assign", {callRT("perl_get_dollar_at", {}), rhs});
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
                /* D133: this fallback previously freeIfOwned(rv)'d the temp
                   and still returned it — the statement context frees the
                   return value again (ExprStmt/emitBlockLast both treat an
                   owned-temp return as theirs to free), double-freeing
                   $acc = "x" + 0 whenever the RHS was an owned boxed temp
                   (segfault, found while verifying D125's deep test: the
                   pre-D125 snapshot binary reproduces it identically).
                   Clone before freeing so the caller gets its own value. */
                Value *ret = callRT("perl_clone", {rv});
                freeIfOwned(rv);
                return ret;
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
                /* D133: same double-free shape as the int twin above —
                   clone for the caller before freeing the temp. */
                Value *ret = callRT("perl_clone", {rv});
                freeIfOwned(rv);
                return ret;
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
            /* W29: assignments to $_ keep the GLOBAL $_ cell in sync (the
               sub/file-scope $_ shadow and the cell must agree, or a
               later `local $_`'s save snapshot misses the value). */
            if (n.left->kind == NK::ScalarVar && n.left->name == "_" &&
                !sharedScalarNames_.count("_")) {
                Value *gcell = callRT("perl_get_dollar_under", {});
                callRT("perl_assign", {gcell, rhs});
            }
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
                    if (n.sval == "%") return emitFlooredMod(lv, rv);
                    /* W1 note: `&=`/`|=`/`^=` stay boxed — a bit-63 result is
                       UV in real Perl and the runtime has no UV storage. */
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

                        /* Stage 16: normal PV* row cache.
                           D96: the row AV cache (ra) is only populated for
                           non-flat (REF_ARRAY) rows; for FLAT_ARRAY rows the
                           flat-row cache (fra) is set instead and ra stays null.
                           A compound-assign write through a null ra crashes
                           (perl_array_get_ref(null,...)).  When ra is null
                           (flat row), lazy-converting via perl_deref_array
                           would disconnect the write from the flat double[]
                           storage the read path still uses, silently corrupting
                           data.  Route through a runtime helper that re-reads
                           the row PV's tag/pval to avoid both the null crash
                           and stale-cache corruption (the row may have been
                           lazy-converted by an earlier read in the same loop
                           body, freeing the original double[]). */
                        if (Value *ra = lookupRowAV(outerNm18, idxNm)) {
                            /* Re-fetch the row PV from the outer array — don't
                               use the cached fra/ra, which may be stale after
                               earlier reads in this loop body lazy-converted
                               the row. */
                            Value *rowRef = callRT("perl_array_get_ref",
                                {outerArr, emitIdx(*n.left->left->right)});
                            Value *idxOp = emitIdx(*n.left->right);
                            Value *rhsValOp = emitExpr(*n.right);
                            int opCode = (n.sval == "+") ? 0 : (n.sval == "-") ? 1 :
                                         (n.sval == "*") ? 2 : (n.sval == "/") ? 3 : 0;
                            Value *opV = ConstantInt::get(Type::getInt32Ty(ctx_), opCode);
                            Value *res = callRT("perl_flat_row_op_assign",
                                {rowRef, idxOp, rhsValOp, opV});
                            /* rowRef is a borrowed reference from perl_array_get_ref
                               (never free it — see runtime.h comment).  rhsValOp
                               is owned and must be freed. */
                            freeIfOwned(rhsValOp);
                            return res;
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
                    /* D98: base is a row/ref that may be a FLAT_ARRAY (e.g. the
                       adjacent form $P[$i][k] op=, whose inner $P[$i] parses to an
                       ArrayElem, so the 2D fast-path above doesn't apply).
                       emitExpr returns a BORROWED row ref here, so the safe
                       perl_deref_array (which lazy-converts FLAT_ARRAY in place)
                       is correct; the old perl_deref_array_ro returned pval
                       (a double*) as a PerlArray* and the inline GEP then
                       dereferenced a double as a pointer → segfault. */
                    Value *base = emitExpr(*n.left->left);
                    av = callRT("perl_deref_array", {base});
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

    case NK::Typeglob: {
        /* If this name is a glob/FH, return the scalar/IO cell so
           print {LOG} / close LOG work. Otherwise stringify *pkg::name. */
        if (isGlobName(n.name) || n.name == "STDOUT" || n.name == "STDERR" ||
            n.name == "STDIN") {
            if (n.name == "STDOUT") return callRT("perl_get_stdout", {});
            if (n.name == "STDERR") return callRT("perl_get_stderr", {});
            if (n.name == "STDIN")  return callRT("perl_get_stdin", {});
            Value *key = builder_.CreateGlobalStringPtr(n.name);
            return callRT("perl_glob_get_scalar", {key});
        }
        return perlStr("*" + currentPackage_ + "::" + n.name);
    }

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
        /* real perl's defined() result is a boolean: "1" or "" (false
           stringifies as nothing, not IV 0) — W1's perl_alloc_bool. */
        return callRT("perl_alloc_bool", {i64});
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
                    return callRT("perl_chop_array", {av});
                }
                return callRT("perl_alloc_int", {callRT("perl_chomp_array", {av})});
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
        if (n.args.size() >= 4) {
            /* D74: 4-arg substr($str,$off,$len,$repl) — real Perl replaces
               the substring in $str in place AND returns the OLD (pre-
               replacement) substring value. Previously this 4th arg was
               parsed but silently ignored — codegen had no case for
               args.size()>=4 at all, so the call behaved exactly like the
               3-arg read-only form. `str` here is the stable PerlValue*
               the original scalar variable's slot holds (emitExpr on a
               plain ScalarVar returns that pointer itself, not a clone),
               so perl_substr_replace's in-place mutation of it correctly
               reaches back into the variable — the same stable-pointer
               reliance the existing 3-arg lvalue-assignment form
               (`substr(...) = val`, case NK::Assign above) already uses.
               Neither perl_substr3 nor perl_substr_replace frees/mutates
               their off/len args, so reusing the same off/len Values for
               both calls below is safe. */
            Value *len = emitExpr(*n.args[2]);
            Value *oldVal = callRT("perl_substr3", {str, off, len});
            Value *repl = emitExpr(*n.args[3]);
            callRT("perl_substr_replace", {str, off, len, repl});
            freeIfOwned(repl);
            return oldVal;
        }
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

    case NK::PackFunc: {
        /* D67: parser already built this node and runtime.c already
           implements perl_pack, but codegen had no case at all — pack()
           silently compiled to nothing. Args must be flattened like
           JoinFunc's list-building does (pack("C4", @arr) needs @arr's
           4 elements individually, not scalar(@arr) — an array argument
           passed through plain emitExpr() would give just the count). */
        Value *fmt = emitExpr(*n.left);
        Value *av  = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            Value *src = emitArrayPtr(*a);
            if (src) callRT("perl_array_extend", {av, src});
            else     callRT("perl_array_push",   {av, emitExpr(*a)});
        }
        return callRT("perl_pack", {fmt, av});
    }

    case NK::UnpackFunc: {
        /* D67: same gap as PackFunc. Scalar context returns just the first
           unpacked value; list context is handled separately in
           emitArrayPtr via perl_unpack_to_array. Node layout (parser.cpp):
           n.left = the string being unpacked, n.args[0] = the format. */
        Value *str = emitExpr(*n.left);
        Value *fmt = emitExpr(*n.args[0]);
        return callRT("perl_unpack", {fmt, str});
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
        /* D118: optional 3rd LIMIT argument, stored in n.args[0] if given. */
        Value *limit = n.args.empty()
            ? ConstantInt::get(Type::getInt64Ty(ctx_), 0, true)
            : callRT("perl_to_int", {emitExpr(*n.args[0])});
        if (n.ival) {  /* regex split */
            Value *pat = builder_.CreateGlobalStringPtr(n.sval, "sp_pat");
            Value *flg = builder_.CreateGlobalStringPtr(n.name, "sp_flg");
            return callRT("perl_split_regex", {pat, flg, str, limit});
        }
        Value *sep = n.left  ? emitExpr(*n.left)  : perlStr(" ");
        return callRT("perl_split", {sep, str, limit});
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
        if (n.name == "Config" || n.name == "Config::Config") {
            Value *key = emitExpr(*n.left);
            return callRT("perl_config_get", {key});
        }
        if (n.name == "+") {
            Value *key = emitExpr(*n.left);
            return callRT("perl_plus_hash_get", {key});
        }
        Value *hv = lookupHash(n.name);
        if (!hv) return perlUndef();
        /* D106: same bug/fix class as ArrayElem above and D105's
           ScalarVar read — promote a FLAT_ARRAY/FLOAT_PAIR-tagged
           anon-array-ref value read out of a hash element before it can
           be aliased a second time (`my $y = $h{k}; $y->[0] = 99;`).
           Scoped to this one plain-read case only — `emitHashGetRef` is
           a shared helper used by several other call sites (hash-slice
           literals, delete/exists LHS refs, etc.); wrapping just this
           return, rather than promoting inside the helper itself, keeps
           the change from reaching any of those. */
        Value *elem = emitHashGetRef(hv, *n.left);
        callRT("perl_promote_ref_array", {elem});
        return elem;
    }

    case NK::KeysFunc: {
        if (n.name == "Config" || n.name == "Config::Config")
            return callRT("perl_config_keys", {});
        /* D119: keys %$href / keys %{$href} in scalar context — n.name is
           empty for the deref form, so lookupHash(n.name) always missed
           and this fell straight to perlInt(0). Mirrors emitArrayPtr's
           identical n.left handling for keys in list context, which
           already worked correctly. */
        if (n.left) {
            Value *ref = emitExpr(*n.left);
            Value *h   = callRT("perl_deref_hash", {ref});
            freeIfOwned(ref);
            return callRT("perl_hash_size", {h});
        }
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
        /* D119: values %$href / values %{$href} in scalar context — same
           gap and fix as KeysFunc above. */
        if (n.left) {
            Value *ref = emitExpr(*n.left);
            Value *h   = callRT("perl_deref_hash", {ref});
            freeIfOwned(ref);
            return callRT("perl_hash_size", {h});
        }
        Value *hv = lookupHash(n.name);
        return hv ? callRT("perl_hash_size", {hv}) : perlInt(0);
    }

    case NK::ExistsFunc: {
        auto existsI32 = [&](Value *i32v) -> Value * {
            return callRT("perl_alloc_int",
                {builder_.CreateSExt(i32v, Type::getInt64Ty(ctx_))});
        };
        /* exists $Config{...} / exists $ENV{...} — native special hashes */
        if (n.left &&
            (n.name == "Config" || n.name == "Config::Config" ||
             n.name == "ENV" || n.name == "::ENV"))
            return existsI32(callRT(
                n.name == "ENV" ? "perl_env_exists" : "perl_config_exists",
                {emitExpr(*n.left)}));
        /* Chained exists $h{a}{b}: autoviv intermediates, exists on last. */
        if (!n.args.empty() && n.left) {
            struct ExLev { bool isArr; const Node *key; };
            std::vector<ExLev> chain;
            chain.push_back({n.sval == "array", n.left.get()});
            for (auto &a : n.args)
                chain.push_back({a->sval == "array", a->left.get()});
            bool isArr = chain[0].isArr;
            Value *hv = isArr ? nullptr : lookupHash(n.name);
            Value *av = isArr ? lookupArray(n.name) : nullptr;
            if (!hv && !av) return perlInt(0);
            for (size_t i = 0; i + 1 < chain.size(); i++) {
                bool nextArr = chain[i + 1].isArr;
                const Node *keyN = chain[i].key;
                if (isArr) {
                    Value *idx = emitIdx(*keyN);
                    if (nextArr)
                        av = callRT("perl_array_autoviv_array", {av, idx});
                    else {
                        hv = callRT("perl_array_autoviv_hash", {av, idx});
                        av = nullptr;
                    }
                } else {
                    if (nextArr) {
                        if (Value *kp = constKeyPtr(*keyN, builder_))
                            av = callRT("perl_hash_autoviv_array", {hv, kp});
                        else {
                            Value *key = emitExpr(*keyN);
                            av = callRT("perl_hash_autoviv_array_sv", {hv, key});
                            freeIfOwned(key);
                        }
                        hv = nullptr;
                    } else {
                        if (Value *kp = constKeyPtr(*keyN, builder_))
                            hv = callRT("perl_hash_autoviv_hash", {hv, kp});
                        else {
                            Value *key = emitExpr(*keyN);
                            hv = callRT("perl_hash_autoviv_hash_sv", {hv, key});
                            freeIfOwned(key);
                        }
                    }
                }
                isArr = nextArr;
            }
            const Node *lastKey = chain.back().key;
            if (isArr) {
                if (!av) return perlInt(0);
                Value *idx = emitIdx(*lastKey);
                Value *elem = callRT("perl_array_get_ref", {av, idx});
                Value *def  = callRT("perl_defined", {elem});
                return existsI32(def);
            }
            if (!hv) return perlInt(0);
            return existsI32(emitHashExists(hv, *lastKey));
        }
        if (n.sval == "array") {
            Value *av = lookupArray(n.name);
            if (!av) return perlInt(0);
            Value *idx = emitIdx(*n.left);
            Value *elem = callRT("perl_array_get_ref", {av, idx});
            Value *def  = callRT("perl_defined", {elem});
            return existsI32(def);
        }
        Value *hv = lookupHash(n.name);
        if (!hv) return perlInt(0);
        return existsI32(emitHashExists(hv, *n.left));
    }

    case NK::DeleteFunc: {
        /* D81: slice forms return the last deleted value in scalar context
           (list form goes through emitArrayPtr). */
        if (n.sval == "array_slice" || n.sval == "hash_slice") {
            Value *av = emitArrayPtr(n);
            if (!av) return perlUndef();
            return callRT("perl_array_last", {av});
        }
        if (n.sval == "array") {
            Value *av = lookupArray(n.name);
            if (!av) return perlUndef();
            Value *idx = emitIdx(*n.left);
            return callRT("perl_array_delete", {av, idx});
        }
        Value *hv = lookupHash(n.name);
        if (!hv) return perlUndef();
        return emitHashDelete(hv, *n.left);
    }

    case NK::MapFunc:
    case NK::GrepFunc:
        /* array-producing: scalar context returns element count */
        {
            Value *av = emitArrayPtr(n);
            if (!av) return perlUndef();
            return callRT("perl_array_len", {av});
        }
    case NK::SortFunc:
        /* D29: real Perl's sort() in scalar context returns undef (not the
           element count like grep/map) — and doesn't even evaluate its
           list argument or comparator block. emitExpr's contract here IS
           scalar context (unconditionally, no runtime wantarray check
           needed), so just return undef without touching the list. */
        return perlUndef();
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
    case NK::AbsFunc:  { if (Value *iv = emitExprI64(n)) return boxI64(iv); Value *a=emitExpr(*n.left); Value *r=callRT("perl_abs_val",  {a}); freeIfOwned(a); return r; }
    case NK::IntFunc:  { if (Value *iv = emitExprI64(n)) return boxI64(iv); Value *a=emitExpr(*n.left); Value *r=callRT("perl_int_trunc",{a}); freeIfOwned(a); return r; }
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
    /* localtime / gmtime in scalar context return a ctime()-style string
       ("Thu Jan  1 00:00:00 1970"); list context is intercepted in
       emitArrayPtr and returns the 9-element list. */
    case NK::LocaltimeFunc:
    case NK::GmtimeFunc: {
        Value *t = n.left ? emitExpr(*n.left) : perlUndef();
        Value *r = n.kind == NK::GmtimeFunc
            ? callRT("perl_scalar_gmtime", {t})
            : callRT("perl_scalar_localtime", {t});
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
        /* D69: uniq in scalar context returns the COUNT of unique
           elements (real List::Util's documented behavior) — this
           previously returned the *first* unique element's value instead,
           a second, separate bug from the consecutive-only dedup fixed in
           perl_uniq_list() itself (runtime.c). */
        return callRT("perl_array_len", {callRT("perl_uniq_list", {av})});
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
        Value *accCell  = callRT("perl_alloc_undef", {});
        callRT("perl_assign", {accCell, first});
        builder_.CreateStore(accCell, accAlloca);

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
        /* D28: don't shadow $a/$b if an outer `my $a`/`my $b` is already
           visible — matches real Perl's "my $a used in sort comparison"
           footgun (see the identical check for sort's comparator, a few
           hundred lines up, for the full explanation). reduce's block is
           compiled inline in the same function/scope stack (no separate
           LLVM function the way sort's comparator needs), so lookupVar()
           here correctly sees both file-scope *and* enclosing-sub-scope
           shadows — a strictly more general check than sort's, which can
           only reach a file-scope shadow. */
        if (!lookupVar("a")) declareVar("a", aAlloca);
        if (!lookupVar("b")) declareVar("b", bAlloca);
        Value *blockResult = n.body ? emitBlockLast(*n.body) : perlUndef();
        popScope();

        /* update accumulator */
        callRT("perl_assign", {builder_.CreateLoad(perlPtrTy_, accAlloca), blockResult});

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
                /* D105: no promotion needed here — a ScalarVar `elem` (e.g.
                   `[$inner, [3,4]]`) already comes back promoted from
                   emitExpr(ScalarVar) itself. */
                Value *pv = emitExpr(*elem);
                callRT("perl_array_push", {av, pv});
                freeIfOwned(pv);
            }
        }
        return callRT("perl_ref_array", {av});
    }

    case NK::AnonHash: {
        /* D76: must freeIfOwned each pushed element (array_push clones) and
           free listArr after hash_from_list (which clones again into the
           hash). Leaking the temps left nested blessed objects at
           refcount>=2 forever, so their DESTROY never ran when the outer
           object was freed. AnonArray already freeIfOwned's; mirror that. */
        Value *hv = callRT("perl_anon_hash_new", {});
        Value *listArr = callRT("perl_array_new", {});
        for (auto &elem : n.args) {
            /* D111 (found via the same bug class in `{ %args, k=>v }`,
               the `bless {%args}, $class` idiom): a list-producing
               element — most commonly a spread %hash or @array — must be
               extended into listArr, not pushed as one mistyped scalar
               (a bare %hash element would otherwise be evaluated in
               scalar context, i.e. its key count, as a single element).
               Mirrors NK::AnonArray's identical dispatch just above. */
            if (Value *sub = emitArrayPtr(*elem)) {
                callRT("perl_array_extend", {listArr, sub});
            } else {
                Value *pv = emitExpr(*elem);
                callRT("perl_array_push", {listArr, pv});
                freeIfOwned(pv);
            }
        }
        callRT("perl_hash_from_list", {hv, listArr});
        callRT("perl_array_free", {listArr});
        return callRT("perl_ref_hash", {hv});
    }

    case NK::DerefScalar: {
        Value *ref = emitExpr(*n.left);
        return callRT("perl_deref_scalar", {ref});
    }

    /* W22: ${ EXPR } — symbolic scalar deref by computed name (real Perl:
       a BLOCK after `$` is a symbolic reference whose string value names
       the variable to access — Getopt/Std.pm's `${"opt_$first"} = 1;`).
       When EXPR is a runtime string, the named (global) variable is
       resolved through the process glob registry (D110's machinery,
       perl_glob_get_scalar) with the current package as the default
       qualifier, exactly like the isQualifiedName/ isGlobName read paths
       above. When EXPR is a plain scalar variable holding a REF, this
       case is not used — the existing $$ref path (NK::DerefScalar from
       the `$$` parse branch) handles that; but a ${$ref} spelling also
       lands here, so detect that shape in the parser instead and keep
       this case name-only (matching real Perl: ${$r} where $r holds a
        non-string ref derefs the ref). */
    case NK::SymbolicDeref: {
        /* W22 (part 2): ${$ref} — block-form deref of a REF-typed value
           is the ordinary scalar deref (same node as `$$ref`), NOT a
           symbolic-name lookup. The symbolic path's glob-registry read
           of a REF PerlValue would have returned the ref itself, and
           the runtime's glob lookup for the "SCALAR(0x...)" stringified
           key would silently create a fresh empty global. Real Perl:
           ${$r} and $$r are the same operator — a block after `$` derefs
           whatever the block evaluates to (a REF value derefs the
           referent; a string is a symbolic reference to the named
           global, resolved in the current package). The runtime tag
           check is perl_su_reftype (returns undef when the value is not
           a reference). */
        if (n.left) {
            int savedCtx0 = callCtx_;
            callCtx_ = -1;
            Value *refPv = emitExpr(*n.left);
            callCtx_ = savedCtx0;
            Value *isRef = callRT("perl_su_reftype", {refPv});
            Value *hasRef = builder_.CreateICmpNE(
                builder_.CreateCall(getRTFunc("perl_is_true"), {isRef}),
                ConstantInt::get(Type::getInt32Ty(ctx_), 0), "sd.isref");
            auto *curFn = builder_.GetInsertBlock()->getParent();
            auto *refBB = BasicBlock::Create(ctx_, "sd.ref", curFn);
            auto *symBB = BasicBlock::Create(ctx_, "sd.sym", curFn);
            auto *joinBB = BasicBlock::Create(ctx_, "sd.join", curFn);
            builder_.CreateCondBr(hasRef, refBB, symBB);
            builder_.SetInsertPoint(refBB);
            Value *refRes = callRT("perl_deref_scalar", {refPv});
            builder_.CreateBr(joinBB);
            auto *refBBp = builder_.GetInsertBlock();
            builder_.SetInsertPoint(symBB);
            Value *symRes = emitSymbolicDeref(n);
            auto *symBBp = builder_.GetInsertBlock();
            builder_.CreateBr(joinBB);
            builder_.SetInsertPoint(joinBB);
            auto *phi = builder_.CreatePHI(perlPtrTy_, 2, "sd.res");
            phi->addIncoming(refRes, refBBp);
            phi->addIncoming(symRes, symBBp);
            return phi;
        }
        return emitSymbolicDeref(n);
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

    /* W28: $s =~ EXPR — the pattern comes from a runtime value (real Perl
       compiles the RHS's string value as a regex at runtime, m/$var/).
       left = target string, right = pattern expression, ival = !~ flag.
       No flags exist in this form (real Perl m/$var/ carries none), so
       the runtime gets the empty flags string, which perl_regex_match
       already handles (its regex cache keys on (pattern, flags)).
       The pattern must reach perl_regex_match as a `const char*` (its
       declared ABI, fixed in the RT() table which is deliberately not
       touched for this fix), so the pattern PerlValue* is converted
       with the runtime's existing perl_to_string_dup (declared here
       inline the same way declareRuntime's setjmp special case declares
       its non-table function — the C symbol exists in runtime.c:1772;
       its returned buffer is freed right after the call). */
    case NK::QrRegex: {
        /* qr/PAT/FLAGS — a compiled-pattern VALUE (real perl's qr//).
           Built once at this statement's execution and refcounted; the
           PV flows through subs/arrays/hashes like any scalar. When the
           pattern text has interpolation triggers (n.right set), it is
           built at runtime through the same interp machinery "..." uses. */
        Value *pat;
        if (n.right) {
            Value *pv = emitExpr(*n.right);
            if (!rtFuncs_.count("perl_to_string_dup"))
                rtFuncs_["perl_to_string_dup"] = Function::Create(
                    makeRT(ctx_, PointerType::getUnqual(ctx_), {perlPtrTy_}),
                    Function::ExternalLinkage, "perl_to_string_dup", mod_.get());
            pat = builder_.CreateCall(getRTFunc("perl_to_string_dup"), {pv});
            freeIfOwned(pv);
        } else {
            pat = builder_.CreateGlobalStringPtr(n.sval, "qr_pat");
        }
        Value *flg = builder_.CreateGlobalStringPtr(n.name, "qr_flg");
        Value *r = callRT("perl_make_qr", {pat, flg});
        return r;
    }

    case NK::RegexMatchInterp: {
        /* $s =~ /pat-with-$vars/ — the pattern is an InterpolatedStr AST
           (parseStringInterp output): evaluate it to its runtime string
           and match with the node's flags. !~ via ival. */
        Value *str = emitExpr(*n.left);
        Value *patPv = emitExpr(*n.right);
        if (!rtFuncs_.count("perl_to_string_dup"))
            rtFuncs_["perl_to_string_dup"] = Function::Create(
                makeRT(ctx_, PointerType::getUnqual(ctx_), {perlPtrTy_}),
                Function::ExternalLinkage, "perl_to_string_dup", mod_.get());
        Value *patC = builder_.CreateCall(getRTFunc("perl_to_string_dup"), {patPv});
        Value *flg = builder_.CreateGlobalStringPtr(n.name, "rmi_flg");
        Value *res = callRT("perl_regex_match", {str, patC, flg});
        freeIfOwned(patPv);
        freeIfOwned(str);
        return n.ival ? callRT("perl_not", {res}) : res;
    }

    case NK::RegexMatchExpr: {
        /* one-time inline declaration of `char *perl_to_string_dup(const PerlValue*)`
           (the C symbol lives in runtime.c:1772, outside the RT() table's
           namespace — declared here the same way declareRuntime's setjmp
           special case declares its non-table function) */
        if (!rtFuncs_.count("perl_to_string_dup"))
            rtFuncs_["perl_to_string_dup"] = Function::Create(
                makeRT(ctx_, PointerType::getUnqual(ctx_), {perlPtrTy_}),
                Function::ExternalLinkage, "perl_to_string_dup", mod_.get());
        /* same for `void free(void*)` (libc) — the dup'd buffer's owner */
        Function *freeFn = rtFuncs_.count("free")
            ? rtFuncs_["free"]
            : (rtFuncs_["free"] = Function::Create(
                   makeRT(ctx_, Type::getVoidTy(ctx_),
                          {PointerType::getUnqual(ctx_)}),
                   Function::ExternalLinkage, "free", mod_.get()));

        Value *str = emitExpr(*n.left);
        int savedCtx = callCtx_;
        callCtx_ = -1;              /* pattern expr inherits caller context */
        Value *patPv = emitExpr(*n.right);
        callCtx_ = savedCtx;
        /* QR-aware dispatch: a qr// operand uses its own pattern+flags;
           anything else is stringified and matched with empty flags
           (W28's original string-pattern behavior). */
        Value *res = callRT("perl_regex_match_sv",
            {str, patPv, ConstantInt::get(Type::getInt32Ty(ctx_), n.ival ? 1 : 0)});
        freeIfOwned(str);
        freeIfOwned(patPv);
        return res;
    }

    case NK::RegexSubst: {
        Value *str = emitExpr(*n.left);
        size_t sep = n.name.find('\x01');
        std::string repl  = n.name.substr(0, sep);
        std::string flags = n.name.substr(sep + 1);
        /* s/$var.../ pattern interpolation: when n.right is set (the
           pattern was parsed as an InterpolatedStr AST), build the pattern
           at runtime; otherwise it's the compile-time literal. */
        Value *pat;
        if (n.right) {
            Value *pv = emitExpr(*n.right);
            if (!rtFuncs_.count("perl_to_string_dup"))
                rtFuncs_["perl_to_string_dup"] = Function::Create(
                    makeRT(ctx_, PointerType::getUnqual(ctx_), {perlPtrTy_}),
                    Function::ExternalLinkage, "perl_to_string_dup", mod_.get());
            pat = builder_.CreateCall(getRTFunc("perl_to_string_dup"), {pv});
            freeIfOwned(pv);
        } else {
            pat = builder_.CreateGlobalStringPtr(n.sval, "rs_pat");
        }
        Value *flg  = builder_.CreateGlobalStringPtr(flags,  "rs_flg");

        /* D38c/D109: evaluate `replExpr` once per match, with $1/$& already
           installed by the runtime, via a standalone closure function that
           captures whatever outer lexicals it references — originally
           built only for /e (replExpr = the replacement text parsed AS
           CODE), and reused unchanged for D109 (replExpr = the
           replacement text's *interpolation* AST — a concat chain of
           literal-string and variable-reference parts, from
           Parser::parseInterpString — same shape as parseExprFromTokens'
           output as far as this machinery cares). */
        auto emitSubstWithReplExpr = [&](NodePtr replExpr) -> Value* {
            /* Capture outer lexicals referenced by the replacement (like sort{}) */
            std::set<std::string> usedNames;
            collectAllScalarNames(*replExpr, usedNames);
            std::vector<std::string> capNames;
            std::vector<Value*>      capVals;
            std::vector<char>        capSigils;
            for (auto &nm : usedNames) {
                if (nm == "_") continue;
                if (auto *slot = lookupVar(nm)) {
                    capNames.push_back(nm);
                    capVals.push_back(builder_.CreateLoad(perlPtrTy_, slot));
                    capSigils.push_back('$');
                } else if (Value *ia = lookupIntVar(nm)) {
                    Value *boxed = boxI64(builder_.CreateLoad(Type::getInt64Ty(ctx_), ia));
                    auto *pvA = builder_.CreateAlloca(perlPtrTy_, nullptr, nm + ".boxed");
                    builder_.CreateStore(boxed, pvA);
                    capNames.push_back(nm);
                    capVals.push_back(builder_.CreateLoad(perlPtrTy_, pvA));
                    capSigils.push_back('$');
                } else if (Value *fa = lookupFloatVar(nm)) {
                    Value *boxed = boxF64(builder_.CreateLoad(Type::getDoubleTy(ctx_), fa));
                    auto *pvA = builder_.CreateAlloca(perlPtrTy_, nullptr, nm + ".boxed");
                    builder_.CreateStore(boxed, pvA);
                    capNames.push_back(nm);
                    capVals.push_back(builder_.CreateLoad(perlPtrTy_, pvA));
                    capSigils.push_back('$');
                }
            }
            {
                std::unordered_map<std::string, Value*> visA, visH;
                for (auto &sc : arrayScopes_) for (auto &kv : sc) visA[kv.first] = kv.second;
                for (auto &sc : hashScopes_)  for (auto &kv : sc) visH[kv.first] = kv.second;
                for (auto &kv : visA) {
                    if (kv.first == "_") continue;
                    capNames.push_back(kv.first);
                    capVals.push_back(callRT("perl_ref_array", {kv.second}));
                    capSigils.push_back('@');
                }
                for (auto &kv : visH) {
                    capNames.push_back(kv.first);
                    capVals.push_back(callRT("perl_ref_hash", {kv.second}));
                    capSigils.push_back('%');
                }
            }

            std::string fnName = "__subst_e_" + std::to_string(substEvalCounter_++);
            auto *evalFT = FunctionType::get(perlPtrTy_, {}, false);
            auto *evalFn = Function::Create(evalFT, Function::InternalLinkage, fnName, mod_.get());

            auto *savedFn   = currentFn_;
            auto *savedBB   = builder_.GetInsertBlock();
            auto  savedSc   = scopes_;
            auto  savedArr  = arrayScopes_;
            auto  savedHash = hashScopes_;
            auto  savedPv   = pvScopes_;
            auto  savedF    = floatScopes_;
            auto  savedI    = intScopes_;
            auto *savedLDep = localDepthAlloca_;
            auto *savedBody = currentSubBody_;
            bool  savedNeeds = currentSubNeedsWantarray_;

            auto *entry = BasicBlock::Create(ctx_, "entry", evalFn);
            builder_.SetInsertPoint(entry);
            currentFn_ = evalFn;
            scopes_ = {}; arrayScopes_ = {}; hashScopes_ = {}; pvScopes_ = {};
            floatScopes_ = {}; intScopes_ = {};
            pushScope();
            currentSubNeedsWantarray_ = false;

            auto *i32Ty = Type::getInt32Ty(ctx_);
            auto *i64Ty = Type::getInt64Ty(ctx_);
            localDepthAlloca_ = builder_.CreateAlloca(i32Ty, nullptr, "local.depth");
            builder_.CreateStore(callRT("perl_local_save_depth", {}), localDepthAlloca_);

            for (size_t ci = 0; ci < capNames.size(); ci++) {
                Value *pv = callRT("perl_get_capture", {ConstantInt::get(i64Ty, (long long)ci)});
                if (capSigils[ci] == '@') {
                    declareArray(capNames[ci], callRT("perl_deref_array_ro", {pv}));
                } else if (capSigils[ci] == '%') {
                    declareHash(capNames[ci], callRT("perl_deref_hash", {pv}));
                } else {
                    auto *a = builder_.CreateAlloca(perlPtrTy_, nullptr, capNames[ci]);
                    builder_.CreateStore(pv, a);
                    declareVar(capNames[ci], a);
                }
            }

            Value *result = emitExpr(*replExpr);
            if (!builder_.GetInsertBlock()->getTerminator()) {
                Value *depth = builder_.CreateLoad(i32Ty, localDepthAlloca_);
                callRT("perl_local_restore_to", {depth});
                /* Clone so the returned PV outlives this frame's temps */
                Value *cloned = callRT("perl_clone", {result});
                freeIfOwned(result);
                builder_.CreateRet(cloned);
            }
            popScope();

            currentFn_ = savedFn;
            builder_.SetInsertPoint(savedBB);
            scopes_ = std::move(savedSc);
            arrayScopes_ = std::move(savedArr);
            hashScopes_ = std::move(savedHash);
            pvScopes_ = std::move(savedPv);
            floatScopes_ = std::move(savedF);
            intScopes_ = std::move(savedI);
            localDepthAlloca_ = savedLDep;
            currentSubBody_ = savedBody;
            currentSubNeedsWantarray_ = savedNeeds;

            Value *fnPtr = builder_.CreateBitCast(evalFn, PointerType::getUnqual(ctx_));
            Value *capsAv = callRT("perl_array_new", {});
            for (auto *cv : capVals)
                callRT("perl_array_push_capture", {capsAv, cv});
            Value *cnt = callRT("perl_regex_subst_e", {str, pat, flg, fnPtr, capsAv});
            return callRT("perl_alloc_int", {cnt});
        };

        bool hasE = flags.find('e') != std::string::npos;
        if (hasE) {
            NodePtr replExpr;
            try {
                Lexer lex(repl);
                auto toks = lex.tokenize();
                replExpr = Parser::parseExprFromTokens(std::move(toks));
            } catch (const std::exception &ex) {
                throw std::runtime_error(std::string("s///e: bad replacement expression: ") + ex.what());
            }
            if (!replExpr)
                throw std::runtime_error("s///e: empty replacement expression");
            return emitSubstWithReplExpr(std::move(replExpr));
        }

        /* D109: replacement text is parsed like a double-quoted string in
           real Perl — arbitrary $name/@arr interpolation, not just
           $0-$9/$&amp;. Only take this (more expensive) path when the text
           actually contains a trigger beyond what the existing fast raw-
           string path (perl_regex_subst, D107) already handles correctly,
           so plain-text and capture-ref-only replacements are completely
           unaffected. */
        std::string processedRepl = preprocessReplEscapes(repl);
        if (hasInterpTrigger(processedRepl)) {
            NodePtr replExpr = Parser::parseInterpString(processedRepl, n.line, currentPackage_);
            return emitSubstWithReplExpr(std::move(replExpr));
        }

        Value *rep  = builder_.CreateGlobalStringPtr(repl,   "rs_rep");
        Value *cnt  = callRT("perl_regex_subst", {str, pat, rep, flg});
        /* Real Perl's s/// returns "" (not 0) when nothing matched — the
           zero case stringifies empty. Branch on the count. */
        auto *fn2   = builder_.GetInsertBlock()->getParent();
        auto *zeroBB = BasicBlock::Create(ctx_, "subst.zero", fn2);
        auto *someBB = BasicBlock::Create(ctx_, "subst.some", fn2);
        auto *joinBB = BasicBlock::Create(ctx_, "subst.join", fn2);
        auto *i64Ty2 = Type::getInt64Ty(ctx_);
        Value *isZero = builder_.CreateICmpEQ(cnt, ConstantInt::get(i64Ty2, 0));
        builder_.CreateCondBr(isZero, zeroBB, someBB);
        builder_.SetInsertPoint(zeroBB);
        Value *emptyPv = callRT("perl_alloc_string_len",
                                {builder_.CreateGlobalStringPtr(""),
                                 ConstantInt::get(i64Ty2, 0)});
        builder_.CreateBr(joinBB);
        builder_.SetInsertPoint(someBB);
        Value *cntPv = callRT("perl_alloc_int", {cnt});
        builder_.CreateBr(joinBB);
        builder_.SetInsertPoint(joinBB);
        auto *phi = builder_.CreatePHI(perlPtrTy_, 2, "subst.res");
        phi->addIncoming(emptyPv, zeroBB);
        phi->addIncoming(cntPv, someBB);
        return phi;
    }

    case NK::CaptureVar: {
        return callRT("perl_capture", {builder_.getInt64(n.ival)});
    }

    case NK::WarnStmt: {
        /* D49: pass file/line for location suffix + $SIG{__WARN__} */
        auto *i32Ty = Type::getInt32Ty(ctx_);
        Value *msg = n.left ? emitExpr(*n.left) : perlStr("Warning: something's wrong");
        Value *fileStr = builder_.CreateGlobalStringPtr(sourceFile_, "warn.file");
        Value *lineVal = ConstantInt::get(i32Ty, n.line);
        callRT("perl_warn", {msg, fileStr, lineVal});
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
        Value *path;
        if (n.left) {
            path = emitExpr(*n.left);
        } else {
            /* bare `-d` etc. tests $_ (real Perl implicit-topic rule).
               Read the in-scope $_ slot if the sub has one (W29 shadow),
               else the global $_ cell. */
            Value *slot = lookupVar("_");
            path = slot ? builder_.CreateLoad(perlPtrTy_, slot)
                        : callRT("perl_get_dollar_under", {});
        }
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
        /* D114: see the identical dispatch in emitArrayPtr's ArraySlice
           handling above — a subscript entry can be list-shaped. */
        auto pushArraySliceIdx = [&](const Node &idxNode) {
            if (Value *idxAv = emitArrayPtr(idxNode)) {
                Value *slice = av ? callRT("perl_array_slice", {av, idxAv})
                                   : callRT("perl_array_new", {});
                callRT("perl_array_extend", {res, slice});
            } else {
                Value *elem = av ? callRT("perl_array_get_ref", {av, emitIdx(idxNode)}) : perlUndef();
                callRT("perl_array_push", {res, elem});
            }
        };
        for (auto &idxNode : n.args) pushArraySliceIdx(*idxNode);
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
        /* eval STRING/BLOCK sees the caller's wantarray. Push a frame unless
           callCtx_==-1 (inherit — last expr of a wantarray-aware sub). */
        bool savedNeedWA = currentSubNeedsWantarray_;
        currentSubNeedsWantarray_ = true;
        int evalCtxPush = -1;
        if (callCtx_ == 1)      evalCtxPush = 1;
        else if (callCtx_ == 2) evalCtxPush = 2;
        else if (callCtx_ != -1) evalCtxPush = 0;
        if (evalCtxPush >= 0)
            callRT("perl_push_wantarray",
                   {ConstantInt::get(Type::getInt32Ty(ctx_), evalCtxPush)});

        /* allocate jmp_buf on stack (256 bytes, enough for any platform) */
        auto *i8Arr  = ArrayType::get(Type::getInt8Ty(ctx_), 256);
        auto *jbAlloca = builder_.CreateAlloca(i8Arr, nullptr, "jmp_buf");
        /* cast to ptr for setjmp/perl_eval_push */
        Value *jbPtr = builder_.CreateBitCast(jbAlloca, PointerType::getUnqual(ctx_));
        /* perl_eval_push(jbPtr) — register this jmp_buf */
        callRT("perl_eval_push", {jbPtr});

        /* result alloca — must be BEFORE setjmp so it survives longjmp
           (longjmp restores stack pointer to setjmp's frame; alloca after
           setjmp would be outside the saved frame). */
        auto *resultAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, "eval.result");
        /* Initialize with null so that if the body terminates early (die),
           the load returns a known value (not LLVM undef), allowing the
           longjmpMissed check to work correctly. */
        builder_.CreateStore(ConstantPointerNull::get(perlPtrTy_), resultAlloca);

        /* int caught = setjmp(jbPtr) */
        auto *i32     = Type::getInt32Ty(ctx_);
        Value *caught = callRT("setjmp", {jbPtr});
        auto *fn      = builder_.GetInsertBlock()->getParent();
        Value *isCaught = builder_.CreateICmpNE(caught, ConstantInt::get(i32, 0));
        auto *bodyBB  = BasicBlock::Create(ctx_, "eval.body", fn);
        auto *endBB   = BasicBlock::Create(ctx_, "eval.end",  fn);

        /* setjmp==0 → normal entry → bodyBB; setjmp!=0 → longjmp → endBB */
        builder_.CreateCondBr(isCaught, endBB, bodyBB);

        /* ── body: execute eval block, capture return value ── */
        builder_.SetInsertPoint(bodyBB);
        auto savedIP = builder_.saveIP();
        evalReturnTargets_.push_back({resultAlloca, endBB});
        Value *bodyResult = perlUndef();
        if (n.body) {
            bodyResult = emitBlockLast(*n.body);
        }
        evalReturnTargets_.pop_back();
        /* save insert point after emitBlockLast (may be on nested eval's block) */
        auto *afterBodyBB = builder_.GetInsertBlock();
        /* restore insert point — emitBlockLast may have moved it */
        builder_.restoreIP(savedIP);
        /* store result so endBB can load it (survives longjmp via saved frame) */
        /* only emit if bodyBB doesn't already have a terminator (e.g., from die/return) */
        if (!builder_.GetInsertBlock()->getTerminator()) {
            /* D52: real Perl always resets $@="" when eval completes
               *successfully* (reaching here, as opposed to via the
               longjmp/die path straight to endBB below) — even
               overriding an explicit `$@ = ...` made inside the block
               itself. Previously this reset ran unconditionally
               *before* the body executed, which (now that $@ is a real
               assignment target, see D52's other fix) let an in-block
               assignment wrongly "stick" instead of being clobbered by
               eval's own success-clear, diverging from real Perl. */
            callRT("perl_assign", {callRT("perl_get_dollar_at", {}), perlStr("")});
            builder_.CreateStore(bodyResult, resultAlloca);
            builder_.CreateBr(endBB);
        }

        /* If emitBlockLast left us on a different block (e.g., a nested
           eval's own endBB, reached when that nested eval was a non-last
           statement in this eval's body — the trailing statements after it
           get emitted into that block), terminate that block by storing
           the actual computed body result — NOT just branching with
           nothing stored, which silently lost bodyResult and made endBB's
           load always see the initial null (routed through the
           longjmpMissed check into undef, discarding the real value). */
        if (afterBodyBB != endBB && !afterBodyBB->getTerminator()) {
            builder_.SetInsertPoint(afterBodyBB);
            callRT("perl_assign", {callRT("perl_get_dollar_at", {}), perlStr("")});
            builder_.CreateStore(bodyResult, resultAlloca);
            builder_.CreateBr(endBB);
        }
        builder_.SetInsertPoint(endBB);

        /* ── end: load result (or undef if longjmp-ed before store) ── */
        builder_.SetInsertPoint(endBB);
        Value *finalResult = builder_.CreateLoad(perlPtrTy_, resultAlloca);

        /* ensure we return a valid pointer (longjmp may have skipped the store) */
        auto *longjmpMissed = builder_.CreateICmpEQ(
            builder_.CreateLoad(perlPtrTy_, resultAlloca),
            ConstantPointerNull::get(perlPtrTy_));
        finalResult = builder_.CreateSelect(longjmpMissed, perlUndef(), finalResult);
        /* Clone into the caller scope — the eval body's temps were already
           freed, and LLVM may not keep a post-setjmp value live across
           the comparison/ternary that uses this result. */
        finalResult = callRT("perl_clone", {finalResult});

        /* pop eval stack — must happen in endBB for both normal and longjmp paths */
        callRT("perl_eval_pop", {});

        if (evalCtxPush >= 0)
            callRT("perl_pop_wantarray", {});
        currentSubNeedsWantarray_ = savedNeedWA;

        return finalResult;
    }

    case NK::AnonSub: {
        /* Phase 1: collect captures from outer scopes */
        std::set<std::string> usedNames;
        collectAllScalarNames(*n.body, usedNames);
        /* eval EXPR inside the closure can name outer lexicals that don't
           appear as ScalarVar nodes in this AST (they're inside the string).
           Capture every visible scalar so the eval pad can find them. */
        if (n.body && hasEvalCall(*n.body)) {
            for (auto &scope : scopes_)
                for (auto &kv : scope)
                    if (!evalPadSkipName(kv.first)) usedNames.insert(kv.first);
            for (auto &kv : fileScalarGlobals_)
                if (!evalPadSkipName(kv.first)) usedNames.insert(kv.first);
        }
        std::vector<std::string> captureNames;
        std::vector<Value*>      captureVals;   /* PerlValue* loaded from outer alloca */
        std::vector<char>        captureSigils; /* '$'/'@'/'%' — how to re-declare on the other side */
        for (auto &nm : usedNames) {
            if (nm == "_") continue;
            if (auto *slot = lookupVar(nm)) {
                captureNames.push_back(nm);
                captureVals.push_back(builder_.CreateLoad(perlPtrTy_, slot));
                captureSigils.push_back('$');
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
                    captureSigils.push_back('$');
                } else if (Value *fa = lookupFloatVar(nm)) {
                    Value *fval = builder_.CreateLoad(Type::getDoubleTy(ctx_), fa);
                    Value *boxed = boxF64(fval);
                    auto *pvAlloca = builder_.CreateAlloca(perlPtrTy_, nullptr, nm + ".boxed");
                    builder_.CreateStore(boxed, pvAlloca);
                    captureNames.push_back(nm);
                    captureVals.push_back(builder_.CreateLoad(perlPtrTy_, pvAlloca));
                    captureSigils.push_back('$');
                }
            }
        }
        /* Arrays/hashes referenced inside a closure aren't reachable through
           collectAllScalarNames (they're @/%  sigil, and many builtins like
           push/keys/splice reference the array/hash purely by n.name rather
           than a nested ArrayVar/HashVar child, so a used-name AST walk would
           be an incomplete allowlist and risk silently dropping a capture).
           Instead, capture every block-scoped array/hash currently visible —
           file-scope @arr/%h don't need capturing (already reachable via
           fileArrayGlobals_/fileHashGlobals_ globals). Over-capturing here is
           just a few extra cheap pointer boxes; under-capturing silently
           detaches the closure's view of the array from the caller's. */
        {
            std::unordered_map<std::string, Value*> visibleArrays;
            for (auto &scope : arrayScopes_)
                for (auto &kv : scope) visibleArrays[kv.first] = kv.second;
            for (auto &kv : visibleArrays) {
                if (kv.first == "_") continue;
                captureNames.push_back(kv.first);
                captureVals.push_back(callRT("perl_ref_array", {kv.second}));
                captureSigils.push_back('@');
            }
            std::unordered_map<std::string, Value*> visibleHashes;
            for (auto &scope : hashScopes_)
                for (auto &kv : scope) visibleHashes[kv.first] = kv.second;
            for (auto &kv : visibleHashes) {
                captureNames.push_back(kv.first);
                captureVals.push_back(callRT("perl_ref_hash", {kv.second}));
                captureSigils.push_back('%');
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
        /* D124: __SUB__ inside this closure body must resolve to the
           running closure's code-ref object (runtime call), not the
           compile-time named-sub constant path. */
        bool  savedAnonEmit    = inAnonSubEmit_;
        std::string savedSubNm = currentSubName_;
        inAnonSubEmit_ = true;
        currentSubName_.clear();
        /* A `return` inside this anon sub's body must target the anon sub
           itself, never an enclosing eval{} — clear (and restore after)
           so the new-sub body starts with no active eval-return target,
           the same isolation already given to scopes_/arrayScopes_/etc. */
        auto  savedEvalReturnTargets = evalReturnTargets_;
        evalReturnTargets_.clear();
        /* D64: pre-scan this closure's own body for names captured by any
           FURTHER-nested closure, so its own `my $var = <literal>`
           declarations get the same fast-path guard. */
        auto savedCapturedNames = capturedNamesInCurrentFn_;
        capturedNamesInCurrentFn_.clear();
        if (n.body) collectClosureCapturedNames(*n.body, capturedNamesInCurrentFn_);
        bool savedAllCap = allNamesCaptured_;
        allNamesCaptured_ = n.body && hasEvalCall(*n.body);
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
    currentSubNeedsWantarray_ = true;
    declareArray("_", argsArr);
        /* fresh local() depth for this closure */
        {
            auto *i32Ty = Type::getInt32Ty(ctx_);
            localDepthAlloca_ = builder_.CreateAlloca(i32Ty, nullptr, "local.depth");
            builder_.CreateStore(callRT("perl_local_save_depth", {}), localDepthAlloca_);
        }
        /* Phase 3: initialise captured variables in the closure's own scope */
        auto i64Ty = Type::getInt64Ty(ctx_);
        for (size_t i = 0; i < captureNames.size(); i++) {
            Value *pv = callRT("perl_get_capture",
                               {ConstantInt::get(i64Ty, (long long)i)});
            if (captureSigils[i] == '@') {
                declareArray(captureNames[i], callRT("perl_deref_array_ro", {pv}));
            } else if (captureSigils[i] == '%') {
                declareHash(captureNames[i], callRT("perl_deref_hash", {pv}));
            } else {
                auto *alloca = builder_.CreateAlloca(perlPtrTy_, nullptr, captureNames[i]);
                builder_.CreateStore(pv, alloca);
                declareVar(captureNames[i], alloca);
            }
        }
        currentSubBody_ = n.body.get();
        /* Always emit cleanup + return so that perl_local_restore_to fires on
           ALL exit paths.  This fixes the bug where the restore was skipped
           when the body ended with a loop (which has a terminator).
           Strategy: create a cleanup block, then ensure every exit path from
           the body reaches it.  We do NOT patch loop back-edges — only
           terminators that exit the sub (i.e. don't branch to an earlier
           block in the same function). */
        {
            auto *i32Ty = Type::getInt32Ty(ctx_);
            auto *cleanupBB = BasicBlock::Create(ctx_, "anon_sub_cleanup", subFn);
            BasicBlock *savedInsertBB = builder_.GetInsertBlock();
            builder_.SetInsertPoint(cleanupBB);
            callRT("perl_pop_wantarray", {});
            Value *depth = builder_.CreateLoad(i32Ty, localDepthAlloca_);
            callRT("perl_local_restore_to", {depth});
            auto *placeholderRet = builder_.CreateRet(perlUndef());
            currentSubBody_ = savedSubBody;

            builder_.SetInsertPoint(savedInsertBB);
            pushScope();
            Value *lastVal = emitBlockLast(*n.body);
            popScope();

            /* Connect body exit to cleanupBB. */
            BasicBlock *bodyEndBB = builder_.GetInsertBlock();
            Instruction *termInst = bodyEndBB->getTerminator();

            if (!termInst) {
                /* Body fell through — branch to cleanup */
                builder_.SetInsertPoint(bodyEndBB);
                builder_.CreateBr(cleanupBB);
            } else if (termInst->getNumSuccessors() == 1) {
                /* Unconditional branch — patch to target cleanup */
                cast<BranchInst>(termInst)->setSuccessor(0, cleanupBB);
            } else {
                /* Multiple successors (e.g. loop exit): only redirect successors
                   that are NOT loop back-edges (i.e. not an earlier block in
                   the function).  Back-edges must stay as-is to preserve the
                   loop.  Non-back-edge successors are redirected to a landing
                   pad that then goes to cleanup. */
                auto *landingBB = BasicBlock::Create(ctx_, "anon_sub_landing", subFn);
                builder_.SetInsertPoint(landingBB);
                builder_.CreateBr(cleanupBB);

                /* For every block in the sub, check its terminator's successors.
                   If a successor is NOT an earlier block (i.e. it's an exit),
                   redirect it to the landing pad. */
                unsigned idx = 0;
                for (auto &BB : *subFn) {
                    if (&BB == cleanupBB || &BB == landingBB) { idx++; continue; }
                    Instruction *t = BB.getTerminator();
                    if (!t) { idx++; continue; }
                    for (unsigned i = 0; i < t->getNumSuccessors(); i++) {
                        BasicBlock *succ = t->getSuccessor(i);
                        if (succ == cleanupBB || succ == landingBB) continue;
                        /* Check if succ is an earlier block (loop back-edge).
                           A back-edge goes from a later block to an earlier one. */
                        unsigned succIdx = 0;
                        for (auto &otherBB : *subFn) {
                            if (&otherBB == succ) break;
                            succIdx++;
                        }
                        if (succIdx < idx) {
                            /* succ is before BB — this is a back-edge, leave it */
                        } else {
                            /* succ is after BB or equal — redirect to landing */
                            cast<BranchInst>(t)->setSuccessor(i, landingBB);
                        }
                    }
                    idx++;
                }
            }

            placeholderRet->setOperand(0, lastVal ? lastVal : perlUndef());
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
        inAnonSubEmit_    = savedAnonEmit;
        currentSubName_   = savedSubNm;
        evalReturnTargets_ = std::move(savedEvalReturnTargets);
        capturedNamesInCurrentFn_ = std::move(savedCapturedNames);
        allNamesCaptured_ = savedAllCap;

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
        /* Set wantarray context (D87: -1 inherit, 0 scalar, 1 list, 2 void) */
        auto *i32Ty = Type::getInt32Ty(ctx_);
        Value *ctxVal;
        if (callCtx_ == 1)
            ctxVal = ConstantInt::get(i32Ty, 1);
        else if (callCtx_ == 2)
            ctxVal = ConstantInt::get(i32Ty, 2);
        else if (callCtx_ == 0)
            ctxVal = ConstantInt::get(i32Ty, 0);
        else if (currentSubNeedsWantarray_)
            ctxVal = callRT("perl_current_wantarray_ctx", {});
        else
            ctxVal = ConstantInt::get(i32Ty, 0);
        callCtx_ = 0;
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

    case NK::OverloadStmt: {
        /* D97: register each (op, method) pair for the current package */
        std::string pkg = n.name.empty() ? "main" : n.name;
        Value *pkgStr = builder_.CreateGlobalStringPtr(pkg);
        for (size_t i = 0; i + 1 < n.args.size(); i += 2) {
            std::string op = n.args[i]->sval;
            std::string method = n.args[i+1]->sval;
            /* Qualify the method name if not already qualified */
            std::string qualMethod;
            if (method.find("::") != std::string::npos)
                qualMethod = method;
            else
                qualMethod = pkg + "::" + method;
            Value *opStr = builder_.CreateGlobalStringPtr(op);
            Value *methStr = builder_.CreateGlobalStringPtr(qualMethod);
            callRT("perl_register_overload", {pkgStr, opStr, methStr});
        }
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
        /* File::Spec class methods — File::Spec->catfile(...) etc. The
           invocant is the string "File::Spec" (or "File::Spec::Unix", the
           actual implementation package real File::Spec @ISA-delegates to;
           both dispatch identically since perlc implements the Unix
           functionality natively). List-returning methods (splitpath/
           splitdir/no_upwards/path) must adapt to context: raw PerlArray*
           results are wrapped as PERL_LIST_RESULT in list context (so
           perl_array_push_list_or_scalar spreads them) or reduced to the
           first element in scalar context (real Perl's splitpath in scalar
           context returns just the file part — actually real Perl returns
           the LAST element in scalar context; hmm, list-context results
           assigned to a scalar return the LAST value in Perl, but splitpath
           docs say it "should" be used in list context; real behavior:
           scalar(splitdir) is the element count via @-assignment, but
           a bare method call in scalar context returns the last element.
           Match the last-element rule). */
        if (n.left && n.left->kind == NK::StringLit &&
            (n.left->sval == "File::Spec" || n.left->sval == "File::Spec::Unix")) {
            const std::string &m = n.sval;
            auto flattenArgs = [&]() -> Value * {
                Value *av = callRT("perl_array_new", {});
                for (auto &arg : n.args) {
                    Value *sub = emitArrayPtr(*arg);
                    if (sub) { callRT("perl_array_extend", {av, sub}); continue; }
                    callRT("perl_array_push", {av, emitExpr(*arg)});
                }
                return av;
            };
            /* adapt a PerlArray* result to the enclosing context.
               LIST_RESULT only spreads when the wantarray stack says list
               (perl_array_to_list_return reads the runtime stack, not
               callCtx_), so push/pop the caller's context around it. */
            auto adaptList = [&](Value *av) -> Value * {
                auto *i32Ty = Type::getInt32Ty(ctx_);
                int ctxInt = (callCtx_ == 1 || callCtx_ == 2) ? 1 : 0;
                callRT("perl_push_wantarray", {ConstantInt::get(i32Ty, ctxInt)});
                Value *wrapped = callRT("perl_array_to_list_return", {av});
                callRT("perl_pop_wantarray", {});
                return wrapped;
            };
            if (m == "catdir")  return callRT("perl_fspec_catdir",  {flattenArgs()});
            if (m == "catfile") return callRT("perl_fspec_catfile", {flattenArgs()});
            if (m == "catpath") return callRT("perl_fspec_catpath", {flattenArgs()});
            if (m == "join")    return callRT("perl_fspec_catfile", {flattenArgs()});
            if (m == "canonpath") {
                Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
                return callRT("perl_fspec_canonpath", {path});
            }
            if (m == "curdir")  return callRT("perl_fspec_curdir", {});
            if (m == "updir")   return callRT("perl_fspec_updir", {});
            if (m == "rootdir") return callRT("perl_fspec_rootdir", {});
            if (m == "devnull") return callRT("perl_fspec_devnull", {});
            if (m == "tmpdir")  return callRT("perl_fspec_tmpdir", {});
            if (m == "file_name_is_absolute") {
                Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
                return callRT("perl_fspec_file_name_is_absolute", {path});
            }
            if (m == "rel2abs") {
                Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
                Value *base = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
                return callRT("perl_fspec_rel2abs", {path, base});
            }
            if (m == "abs2rel") {
                Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
                Value *base = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
                return callRT("perl_fspec_abs2rel", {path, base});
            }
            if (m == "splitpath") {
                Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
                Value *nof  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
                return adaptList(callRT("perl_fspec_splitpath", {path, nof}));
            }
            if (m == "splitdir") {
                Value *dir = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
                return adaptList(callRT("perl_fspec_splitdir", {dir}));
            }
            if (m == "no_upwards") return adaptList(callRT("perl_fspec_no_upwards", {flattenArgs()}));
            if (m == "path")       return adaptList(callRT("perl_fspec_path", {}));
            if (m == "case_tolerant") return callRT("perl_fspec_case_tolerant", {});
        }
        /* SUPER::method — dispatch starting from parent of caller package */
        /* Dynamic method name: $obj->$meth(...) — the name comes from an
           expression (parser leaves sval empty and puts it in n.right). */
        if (n.sval.empty() && n.right) {
            Value *methodStr = emitExpr(*n.right);
            callRT("perl_push_call_frame",
                {builder_.CreateGlobalStringPtr(currentPackage_),
                 builder_.CreateGlobalStringPtr(sourceFile_),
                 ConstantInt::get(Type::getInt32Ty(ctx_), n.line)});
            Value *r = callRT("perl_dispatch_method_sv", {obj, methodStr, argsArr});
            callRT("perl_pop_call_frame", {});
            freeIfOwned(methodStr);
            return r;
        }
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
        /* D97: Math::BigInt method intercepts.  In-place mutators (bmul/badd/
           bsub) return self — call them directly (NOT through
           perl_dispatch_method) so freeIfOwned doesn't destroy the receiver.
           Other methods (new/config/bcmp/numify/bstr/copy/bneg) go through
           perl_dispatch_method normally (their results are fresh PVs that
           the codegen can safely free). */
        if (n.left && n.left->kind == NK::StringLit && n.left->sval == "Math::BigInt") {
            if (n.sval == "new") {
                Value *arg = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
                Value *r = callRT("perl_bigint_new", {arg});
                freeIfOwned(arg);
                return r;
            }
            if (n.sval == "config") {
                /* config is handled by perl_dispatch_method — but intercept
                   here to avoid the generic path. Return a hash ref with
                   lib=>"Math::BigInt::GMP". */
                Value *methodStr2 = builder_.CreateGlobalStringPtr(n.sval);
                callRT("perl_push_call_frame",
                    {builder_.CreateGlobalStringPtr(currentPackage_),
                     builder_.CreateGlobalStringPtr(sourceFile_),
                     ConstantInt::get(Type::getInt32Ty(ctx_), n.line)});
                Value *r = callRT("perl_dispatch_method", {obj, methodStr2, argsArr});
                callRT("perl_pop_call_frame", {});
                return r;
            }
        }
        if (n.sval == "bmul" || n.sval == "badd" || n.sval == "bsub") {
            /* These are in-place mutators that return self — call directly
               so the result is NOT freed (it's the receiver's PV). */
            const char *fn = n.sval == "bmul" ? "perl_bigint_bmul"
                          : n.sval == "badd" ? "perl_bigint_badd"
                          : "perl_bigint_bsub";
            Value *arg0 = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
            Value *r = callRT(fn, {obj, arg0});
            freeIfOwned(arg0);
            /* The result (self) is NOT owned — mark it as a LoadInst-like
               value so freeIfOwned skips it.  We can't easily do that, so
               we return it and let the caller handle it.  For chained calls
               (bmul->badd), the intermediate result is used as the receiver
               of the next call, and the final freeIfOwned will free it.
               But self is the variable's PV — freeing it would corrupt the
               variable.  So we DON'T free it (return it as-is).
               The trick: we return obj (the loaded PV), which freeIfOwned
               sees as a non-CallInst (it's a LoadInst) and skips.  But
               callRT returns a CallInst... So we need a different approach. */
            return r;
        }
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

Value *CodeGen::emitAutovivContainer(const Node &node, bool wantHash) {
    if (node.kind == NK::HashElem) {
        Value *outerHv = lookupHash(node.name);
        if (!outerHv) return nullptr;
        const char *hashFn  = wantHash ? "perl_hash_autoviv_hash"    : "perl_hash_autoviv_array";
        const char *hashFnSv= wantHash ? "perl_hash_autoviv_hash_sv" : "perl_hash_autoviv_array_sv";
        if (Value *kp = constKeyPtr(*node.left, builder_))
            return callRT(hashFn, {outerHv, kp});
        Value *key = emitExpr(*node.left);
        Value *result = callRT(hashFnSv, {outerHv, key});
        freeIfOwned(key);
        return result;
    }
    if (node.kind == NK::ArrayElem) {
        Value *outerAv = lookupArray(node.name);
        if (!outerAv) return nullptr;
        Value *idx = emitIdx(*node.left);
        return callRT(wantHash ? "perl_array_autoviv_hash" : "perl_array_autoviv_array",
                       {outerAv, idx});
    }
    if (node.kind == NK::ArrowDeref) {
        bool innerWantHash = (node.sval == "hash");
        Value *container = emitAutovivContainer(*node.left, innerWantHash);
        if (!container) return nullptr;
        if (innerWantHash) {
            const char *hashFn   = wantHash ? "perl_hash_autoviv_hash"    : "perl_hash_autoviv_array";
            const char *hashFnSv = wantHash ? "perl_hash_autoviv_hash_sv" : "perl_hash_autoviv_array_sv";
            if (Value *kp = constKeyPtr(*node.right, builder_))
                return callRT(hashFn, {container, kp});
            Value *key = emitExpr(*node.right);
            Value *result = callRT(hashFnSv, {container, key});
            freeIfOwned(key);
            return result;
        }
        Value *idx = emitIdx(*node.right);
        return callRT(wantHash ? "perl_array_autoviv_hash" : "perl_array_autoviv_array",
                       {container, idx});
    }
     /* Base case: a plain ref-producing expression (e.g. a $scalar variable
        holding a ref already). Matches the pre-existing fallback behavior
        used elsewhere for `$ref->{k} = val` / `$ref->[i] = val` on a bare
        scalar base — NOT full autoviv-from-undef-scalar (a separate, deeper
        gap: TESTS.md D50), just a normal deref of whatever's there. */
    Value *base = emitExpr(node);
    Value *result = callRT(wantHash ? "perl_deref_hash" : "perl_deref_array", {base});
    freeIfOwned(base);
    return result;
}

/* D50: emit code for `$ref->[i] = val` or `$ref->{k} = val` where the
 * chain contains at least one array-index level and is not purely
 * element-rooted with all-hash-key access.  Walks the chain from the
 * base, using _from_scalar autoviv helpers that safely handle
 * FLAT_ARRAY/FLOAT_PAIR.  `targetIsHash` indicates whether the final
 * ArrowDeref is a hash-key. */
Value *CodeGen::emitScalarRootedAutovivAssign(const Node &chain, bool targetIsHash, Value *rhs) {
    /* Collect steps from base outward.  Each step is either a hash-key
     * or an array-index.  The last step is the target. */
    struct Step {
        bool isHash;
        const Node *key;
        Step(bool h, const Node *k) : isHash(h), key(k) {}
    };
    std::vector<Step> steps;
    const Node *cur = &chain;
    while (cur->kind == NK::ArrowDeref) {
        steps.push_back(Step((cur->sval == "hash"), cur->right.get()));
        cur = cur->left.get();
    }
    std::reverse(steps.begin(), steps.end());
    /* cur now points to the base (ScalarVar, HashElem, or ArrayElem). */

    /* Evaluate the base and get the initial container PV*. */
    Value *base;
    if (cur->kind == NK::ScalarVar) {
        base = emitExpr(*cur);
        freeIfOwned(base);
    } else if (cur->kind == NK::HashElem) {
        /* HashElem base: get hash from named variable, then get slot. */
        Value *hv = lookupHash(cur->name);
        if (!hv) return perlUndef();
        Value *key = emitExpr(*cur->left);
        base = callRT("perl_hash_get_sv", {hv, key});
        freeIfOwned(key);
    } else if (cur->kind == NK::ArrayElem) {
        /* ArrayElem base: get array from named variable, then get element. */
        Value *av = lookupArray(cur->name);
        if (!av) return perlUndef();
        Value *idx = emitIdx(*cur->left);
        base = callRT("perl_array_get", {av, idx});
    } else {
        return perlUndef();
    }

    /* Walk intermediate steps (all but the last). */
    for (size_t i = 0; i + 1 < steps.size(); i++) {
        auto &step = steps[i];
        /* Check what the NEXT step needs: if next is array-index, we need
         * an array ref; if next is hash-key, we need a hash ref. */
        bool nextIsHash = steps[i + 1].isHash;
        if (step.isHash) {
            /* Hash-key level: deref current PV* as hash, autoviv appropriate
             * container type based on what the next step needs. */
            Value *key = emitExpr(*step.key);
            Value *hv = callRT("perl_deref_hash", {base});
            if (nextIsHash) {
                /* Next step needs hash: autoviv hash */
                callRT("perl_hash_autoviv_hash_idx", {hv, key});
            } else {
                /* Next step needs array: autoviv array */
                callRT("perl_hash_autoviv_array_idx_sv", {hv, key});
            }
            /* Read back the PV* at that slot for the next iteration. */
            base = callRT("perl_hash_get_sv", {hv, key});
            freeIfOwned(key);
        } else {
            /* Array-index level: deref current PV* as array, autoviv element.
             * The element type is determined by the next step. */
            Value *av = callRT("perl_deref_array", {base});
            Value *idx = emitIdx(*step.key);
            if (nextIsHash) {
                /* Next step needs hash: autoviv hash at this array index */
                callRT("perl_array_autoviv_hash_idx", {av, idx});
            } else {
                /* D98: ensure the row exists WITHOUT converting an existing
                   FLAT_ARRAY.  The old perl_array_autoviv_array_idx flipped a
                   flat row to REF_ARRAY (perl_array_autoviv_array_from_scalar),
                   breaking the "all rows flat" invariant the 2D read fast-path
                   bakes in as llvm.assume(tag==FLAT) → a later read loaded pval
                   (now a PerlArray*) as a double* and segfaulted (nb.pl/nbody.pl). */
                callRT("perl_array_ensure_slot", {av, idx});
            }
            /* Read back the PV* for the next iteration.
               D98: for the LAST array intermediate (the row the final
               array-target writes into) and the target is an array index, borrow
               the actual row (perl_array_get_ref, no clone) so the final
               perl_array_set_row writes into the real slot — persists AND keeps
               it FLAT_ARRAY.  A non-last intermediate's row is dereffed again by
               perl_deref_array next iteration, so clone it there: the deref then
               converts a throwaway copy, never the real slot. */
            if (!nextIsHash && i + 2 == steps.size())
                base = callRT("perl_array_get_ref", {av, idx});
            else
                base = callRT("perl_array_get", {av, idx});
        }
    }

    /* Final step: the target ArrowDeref. */
    auto &target = steps.back();
    if (target.isHash) {
        /* Hash-key target: $ref->...->{key} = val */
        Value *key = emitExpr(*target.key);
        Value *hv = callRT("perl_deref_hash", {base});
        callRT("perl_hash_set_sv", {hv, key, rhs});
        freeIfOwned(key);
    } else {
        /* Array-index target: $ref->...->[idx] = val
         * D98: `base` here is a row that may be a FLAT_ARRAY.  Use
         * perl_array_set_row so a flat row is written in place (into its
         * backing double[]) and STAYS FLAT_ARRAY.  The old perl_deref_array
         * here lazy-converted the row — and because base is a shallow clone
         * sharing the row's double[], that conversion free()'d the shared
         * double[], leaving the original row with a dangling pval that a later
         * flat read fast-path (baked with llvm.assume(tag==FLAT)) would read. */
        Value *idx = emitIdx(*target.key);
        callRT("perl_array_set_row", {base, idx, rhs});
    }
    return rhs;
}

Value *CodeGen::emitLValue(const Node &n) {
    switch (n.kind) {
    case NK::DollarAt: {
        /* D52: $@'s underlying storage (s_dollar_at) is a stable
           thread-local PerlValue, not a PerlValue* alloca slot like
           ordinary scalars — wrap its address in a slot so compound
           assignment forms ($@ .= "x", $@ ||= "y") work via the same
           generic path as everything else. Plain `$@ = ...` is handled
           earlier in NK::Assign directly (this covers the rest). */
        Value *dollarAt = callRT("perl_get_dollar_at", {});
        auto *slot = builder_.CreateAlloca(perlPtrTy_, nullptr, "dollarat.slot");
        builder_.CreateStore(dollarAt, slot);
        return slot;
    }
    case NK::ScalarVar: {
        /* D97: special global variables ($,, $\, $!, $/, $., $&, $0,
           $AUTOLOAD) have stable thread-local storage in the runtime,
           not alloca slots.  Wrap their stable pointer in a temporary
           alloca so the generic assign/compound-assign path writes to
           the real global, not a disconnected fresh alloca. */
        static const std::unordered_map<std::string, const char*> specialGlobals = {
            {",",   "perl_get_dollar_comma"},
            {"\\",  "perl_get_dollar_bsl"},
            {"!",   "perl_get_dollar_bang"},
            {"/",   "perl_get_input_sep"},
            {".",   "perl_get_dollar_dot"},
            {"&",   "perl_get_dollar_amp"},
            {"?",   "perl_get_dollar_question"},
            {"AUTOLOAD", "perl_get_autoload_name"},
            {"0",   "perl_get_dollar0"},
        };
        auto it = specialGlobals.find(n.name);
        if (it != specialGlobals.end()) {
            Value *gv = callRT(it->second, {});
            auto *slot = builder_.CreateAlloca(perlPtrTy_, nullptr,
                                                std::string("spec.") + n.name);
            builder_.CreateStore(gv, slot);
            return slot;
        }
        auto *slot = lookupVar(n.name);
        if (!slot) {
            /* D110: qualified-name write path — same ordering as the read
               path above (fileScalarGlobals_ slot first, then the glob
               registry cell held in a temp alloca). NO declareVar: this
               is not a lexical, and registering it in the current scope
               would shadow a later bare-name lookup, exactly as the
               isGlobName branch below already behaves. */
            if (isQualifiedName(n.name)) {
                auto git = fileScalarGlobals_.find(n.name);
                if (git != fileScalarGlobals_.end()) return git->second;
                Value *key = builder_.CreateGlobalStringPtr(n.name);
                Value *cell = callRT("perl_glob_get_scalar", {key});
                auto *hold = builder_.CreateAlloca(perlPtrTy_, nullptr, "gq." + n.name);
                builder_.CreateStore(cell, hold);
                return hold;
            }
            if (isGlobName(n.name)) {
                Value *key = builder_.CreateGlobalStringPtr(globBareName(n.name));
                Value *cell = callRT("perl_glob_get_scalar", {key});
                auto *hold = builder_.CreateAlloca(perlPtrTy_, nullptr, "glob." + globBareName(n.name));
                builder_.CreateStore(cell, hold);
                return hold;
            }
            /* auto-vivify global-ish variable in current scope */
            auto *alloca = builder_.CreateAlloca(perlPtrTy_, nullptr, n.name);
            builder_.CreateStore(perlUndef(), alloca);
            declareVar(n.name, alloca);
            return alloca;
        }
        return slot;
    }
    /* W22: ${ EXPR } = ... — lvalue form of the symbolic scalar deref.
       Same glob-registry resolution as the read case in emitExpr, but
       returns the registry CELL itself (a PerlValue** held in a temp
       alloca, matching the isQualifiedName/isGlobName write paths just
       above) so the generic assign path writes through it to the real
       global. */
    case NK::SymbolicDeref: {
        /* W22 lvalue: ${ EXPR } = ... — for a REF-typed EXPR this is the
           ordinary scalar deref (same node as `$$ref`), whose lvalue is
           handled by the existing NK::DerefScalar lvalue case above; for
           a string-valued EXPR the glob registry resolves the named
           global. The read path's phi-split can't be reused here (the
           two arms return different shapes), so the tag check simply
           selects which of the two lvalue forms to return. */
        int savedCtx = callCtx_;
        callCtx_ = -1;
        Value *namePv = emitExpr(*n.left);
        callCtx_ = savedCtx;
        Value *isRef = callRT("perl_su_reftype", {namePv});
        Value *hasRef = builder_.CreateICmpNE(
            builder_.CreateCall(getRTFunc("perl_is_true"), {isRef}),
            ConstantInt::get(Type::getInt32Ty(ctx_), 0), "sd.lv.isref");
        auto *curFn = builder_.GetInsertBlock()->getParent();
        auto *refBB = BasicBlock::Create(ctx_, "sd.lv.ref", curFn);
        auto *symBB = BasicBlock::Create(ctx_, "sd.lv.sym", curFn);
        auto *joinBB = BasicBlock::Create(ctx_, "sd.lv.join", curFn);
        builder_.CreateCondBr(hasRef, refBB, symBB);
        builder_.SetInsertPoint(refBB);
        /* ref arm: deref the ref — the referent cell itself is the lvalue
           storage (a live PerlValue* in a temp alloca, same shape the
           generic assign path expects). */
        Value *refCell = callRT("perl_deref_scalar", {namePv});
        auto *holdRef = builder_.CreateAlloca(perlPtrTy_, nullptr,
                                              "symderef.lval");
        builder_.CreateStore(refCell, holdRef);
        builder_.CreateBr(joinBB);
        auto *refBBp = builder_.GetInsertBlock();
        builder_.SetInsertPoint(symBB);
        /* name arm: the glob registry's cell IS the stable PerlValue* the
           generic assign path mutates in place via perl_assign — hold it
           in a temp alloca, exactly like the isQualifiedName/isGlobName
           write paths just above. */
        Value *cell = emitSymbolicDeref(n);
        auto *holdSym = builder_.CreateAlloca(perlPtrTy_, nullptr,
                                              "symderef.lval");
        builder_.CreateStore(cell, holdSym);
        builder_.CreateBr(joinBB);
        auto *symBBp = builder_.GetInsertBlock();
        builder_.SetInsertPoint(joinBB);
        auto *phi = builder_.CreatePHI(perlPtrTy_, 2, "sd.lv.res");
        phi->addIncoming(holdRef, refBBp);
        phi->addIncoming(holdSym, symBBp);
        return phi;
    }
    default: return nullptr;
    }
}

bool CodeGen::isCallLikeForContext(const Node &n) {
    /* D12: print/printf's arguments are evaluated in list context in real
       Perl, so a sub called *directly* as an argument must see
       wantarray()==true. But callCtx_ is a blunt, unscoped one-shot flag
       (set here, consumed by the next Call/MethodCall/CallCodeRef node's
       own codegen) with no awareness of intervening operators — if the
       argument is a compound expression like `ctx() eq "x"`, blindly
       setting callCtx_=1 before evaluating the whole BinOp leaks list
       context through "eq" into ctx(), even though real Perl's comparison
       operators always force scalar context on their own operands
       regardless of the outer context their *result* is used in. This
       exact same leak already existed for `my @a = (ctx() eq "x")` before
       this fix (a separate, pre-existing gap in callCtx_'s design, not
       something introduced here) — confirmed directly, not fixed as part
       of this narrower change. Restricting list-context propagation to
       only the case where the argument's own top-level node is itself a
       call avoids extending that leak into print/printf without needing
       to fix callCtx_'s general design. */
    return n.kind == NK::Call || n.kind == NK::MethodCall || n.kind == NK::CallCodeRef;
}

Value *CodeGen::emitShortCircuitRhs(const Node &rhsNode) {
    /* D8a: `EXPR or return VALUE` / `EXPR and return VALUE` parse into a
       BinOp("||"/"&&") whose RHS is a Block wrapping a single Return
       statement (Parser::parseOrRhs). Routing that through the ordinary
       emitExpr()->emitBlockLast() path is wrong here: emitBlockLast treats
       a trailing Return specially by only *capturing its value* without
       emitting a real return, on the assumption that some outer caller (a
       sub body's own emission code, e.g. the code right after
       emitBlockLast(program) for a sub/do-file body) will perform the
       actual return afterward — true for that use case, but there is no
       such outer caller here. The captured value just silently flowed into
       the "or"/"and" result and execution fell through to the next
       statement — confirmed exactly matching the reported bug: `f() or
       return "X";` compiled but never actually returned "X". Detecting
       this exact shape and dispatching straight to emitStmt() (the real
       Return handling — eval-target check, local-restore, scope cleanup,
       and the actual ret/branch) fixes it without touching emitBlockLast's
       shared logic, which many other callers (sub bodies, do-file bodies,
       ternary/if branches) correctly rely on behaving the way it already
       does. */
    if (rhsNode.kind == NK::Block && rhsNode.args.size() == 1 &&
        rhsNode.args[0]->kind == NK::Return) {
        emitStmt(*rhsNode.args[0]);
        if (builder_.GetInsertBlock()->getTerminator()) return nullptr;
        /* Extremely rare edge case: `return` with no enclosing sub or eval
           ("or return X" at true top level) compiles to a bare perl_die()
           call, which is NoReturn but not itself an LLVM terminator
           instruction — execution never truly continues past it at
           runtime, but the caller's PHI node still needs a well-typed
           value here to keep the IR valid. */
        return perlUndef();
    }
    return emitExpr(rhsNode);
}

/* D103: the raw i64 fast path below (emitExprI64) does native wrapping
   arithmetic with no overflow check at all — `my $big = 9223372036854775807;
   $big + 1` silently wraps to a negative number instead of promoting like
   real Perl. This only intercepts the OUTERMOST operator of a top-level
   `+`/`-`/`*` BinOp: it uses LLVM's overflow-checked intrinsics on the two
   operands (each still obtained via the ordinary emitExprI64, so a nested
   arithmetic sub-expression's own overflow, if any, is unaffected — a
   scoped, deliberately narrower fix than redesigning the fast path's
   recursive descent itself, which stays untouched) and falls back to the
   boxed perl_add/perl_sub/perl_mul (now overflow-aware — see runtime.c)
   only on the rare overflow branch; the non-overflowing common case keeps
   the exact same instructions as before. Returns nullptr (falls through to
   the unchanged raw-i64 path just below) when the operands aren't directly
   i64-representable — mirrors emitExprI64's own canEmitI64 guard exactly,
   so neither operand is evaluated more than once. */
Value *CodeGen::emitI64OverflowCheckedBinOp(const Node &n) {
    if (n.sval != "+" && n.sval != "-" && n.sval != "*") return nullptr;
    if (!n.left || !n.right || !canEmitI64(*n.left) || !canEmitI64(*n.right)) return nullptr;
    Value *lv = emitExprI64(*n.left);
    Value *rv = emitExprI64(*n.right);
    if (!lv || !rv) return nullptr;

    Intrinsic::ID id = n.sval == "+" ? Intrinsic::sadd_with_overflow
                      : n.sval == "-" ? Intrinsic::ssub_with_overflow
                                      : Intrinsic::smul_with_overflow;
    auto *i64 = Type::getInt64Ty(ctx_);
    Function *intr = Intrinsic::getDeclaration(mod_.get(), id, {i64});
    Value *pair = builder_.CreateCall(intr, {lv, rv});
    Value *rawResult = builder_.CreateExtractValue(pair, 0);
    Value *overflowed = builder_.CreateExtractValue(pair, 1);

    auto *fn    = builder_.GetInsertBlock()->getParent();
    auto *okBB  = BasicBlock::Create(ctx_, "i64ovf.ok",   fn);
    auto *slowBB = BasicBlock::Create(ctx_, "i64ovf.slow", fn);
    auto *endBB = BasicBlock::Create(ctx_, "i64ovf.end",  fn);
    builder_.CreateCondBr(overflowed, slowBB, okBB);

    builder_.SetInsertPoint(okBB);
    Value *fastResult = boxI64(rawResult);
    builder_.CreateBr(endBB);
    okBB = builder_.GetInsertBlock();

    builder_.SetInsertPoint(slowBB);
    Value *boxedL = boxI64(lv);
    Value *boxedR = boxI64(rv);
    const char *rt = n.sval == "+" ? "perl_add" : n.sval == "-" ? "perl_sub" : "perl_mul";
    Value *slowResult = callRT(rt, {boxedL, boxedR});
    callRT("perl_free", {boxedL});
    callRT("perl_free", {boxedR});
    builder_.CreateBr(endBB);
    slowBB = builder_.GetInsertBlock();

    builder_.SetInsertPoint(endBB);
    PHINode *phi = builder_.CreatePHI(perlPtrTy_, 2);
    phi->addIncoming(fastResult, okBB);
    phi->addIncoming(slowResult, slowBB);
    return phi;
}

/* D132: emitBinOp's F64 "stay unboxed" fast path (emitExprF64) converts
   operands straight to double via perl_to_float and emits native
   fadd/fsub/fmul — bypassing perl_add/perl_sub/perl_mul's D103 BigInt-aware
   logic entirely. When a scalar-variable operand is BigInt-tagged at RUNTIME
   (the tag isn't statically known: D103 auto-promotion can put an exact
   UINT64_MAX-boundary value into an ordinary variable at any time), the
   native double path truncates the value and diverges by 1 ULP after
   stringification vs real Perl. This wrapper emits the same F64 fast-path
   expression behind a runtime tag-check branch on each ScalarVar operand
   (via the cheap, pure perl_is_bigint_pv predicate): if EITHER operand is
   BigInt-tagged, fall back to the boxed runtime op (D103-aware); otherwise
   emit the exact same native F64 instructions as before and box the double
   result, so both branches yield a PerlValue* and this composes with
   emitBinOp's normal (boxed-value) contract. Follows the branch-and-PHI
   pattern emitI64OverflowCheckedBinOp (D103) already establishes.
   Deliberately narrow: only the + - * operators with at least one ScalarVar
   operand reach this; literals are never BigInt-tagged at runtime (a D103
   huge literal is itself an emitCall node producing a boxed PerlValue*, and
   emitExprF64 rejects it anyway), so no check is emitted for them and
   non-variable code keeps identical instructions. Returns nullptr to fall
   through to the unguarded paths when neither operand is a ScalarVar. */
Value *CodeGen::emitF64BinOpWithBigIntGuard(const Node &n) {
    if (n.sval != "+" && n.sval != "-" && n.sval != "*") return nullptr;
    if (!n.left || !n.right) return nullptr;
    bool lVar = n.left->kind == NK::ScalarVar;
    bool rVar = n.right->kind == NK::ScalarVar;
    if (!lVar && !rVar) return nullptr;
    /* Both operands must be F64-representable for the fast path to be in
       play at all — mirror emitExprF64's own gate so the guarded path is
       only ever emitted where the unguarded one would have been. */
    if (!canEmitF64(n)) return nullptr;

    /* PerlValue* of each variable operand, loaded BEFORE the branch so it
       dominates both arms. */
    auto pvOf = [&](const Node &v) -> Value * {
        std::string nm = v.name;
        if (!nm.empty() && nm[0] == '$') nm = nm.substr(1);
        Value *slot = lookupVar(nm);
        if (!slot) return nullptr;
        return builder_.CreateLoad(perlPtrTy_, slot, nm + ".gpv");
    };
    Value *lPv = lVar ? pvOf(*n.left) : nullptr;
    Value *rPv = rVar ? pvOf(*n.right) : nullptr;
    if ((lVar && !lPv) || (rVar && !rPv)) return nullptr;

    Value *lBig = lPv ? callRT("perl_is_bigint_pv", {lPv}) : nullptr;
    Value *rBig = rPv ? callRT("perl_is_bigint_pv", {rPv}) : nullptr;
    Value *anyBig;
    auto *i32Ty = Type::getInt32Ty(ctx_);
    if (lBig && rBig)
        anyBig = builder_.CreateOr(lBig, rBig, "big.any");
    else if (lBig)
        anyBig = lBig;
    else
        anyBig = rBig;
    Value *isBig = builder_.CreateICmpNE(anyBig, ConstantInt::get(i32Ty, 0),
                                         "big.cond");

    auto *fn      = builder_.GetInsertBlock()->getParent();
    auto *fastBB  = BasicBlock::Create(ctx_, "big.fast", fn);
    auto *slowBB  = BasicBlock::Create(ctx_, "big.slow", fn);
    auto *endBB   = BasicBlock::Create(ctx_, "big.end",  fn);
    builder_.CreateCondBr(isBig, slowBB, fastBB);

    builder_.SetInsertPoint(fastBB);
    Value *fastResult = emitExprF64(n);
    if (!fastResult) return nullptr;   /* caller falls back; block is dead */
    Value *fastBoxed = boxF64(fastResult);
    builder_.CreateBr(endBB);
    fastBB = builder_.GetInsertBlock();

    builder_.SetInsertPoint(slowBB);
    const char *rt = n.sval == "+" ? "perl_add" : n.sval == "-" ? "perl_sub"
                                                                : "perl_mul";
    Value *lv = emitExpr(*n.left);
    Value *rv = emitExpr(*n.right);
    Value *slowResult = callRT(rt, {lv, rv});
    freeIfOwned(lv);
    freeIfOwned(rv);
    builder_.CreateBr(endBB);
    slowBB = builder_.GetInsertBlock();

    builder_.SetInsertPoint(endBB);
    PHINode *phi = builder_.CreatePHI(perlPtrTy_, 2, "big.res");
    phi->addIncoming(fastBoxed, fastBB);
    phi->addIncoming(slowResult, slowBB);
    return phi;
}

Value *CodeGen::emitBinOp(const Node &n) {
    /* Fast path: stay unboxed for integer arithmetic.
       W1: extended to constant-foldable `& | ^ <<` (bit-63/UV results return
       nullptr from emitExprI64), `>>` with constant count [1,63], and
       constant `**` (integer-power range exp 0..30, i64 fit). */
    if (n.sval == "+" || n.sval == "-" || n.sval == "*") {
        if (Value *checked = emitI64OverflowCheckedBinOp(n))
            return checked;
    }
    if (n.sval == "+" || n.sval == "-" || n.sval == "*" || n.sval == "%" ||
        n.sval == "&" || n.sval == "|" || n.sval == "^" ||
        n.sval == "<<" || n.sval == ">>" || n.sval == "**") {
        if (Value *iv = emitExprI64(n))
            return boxI64(iv);
    }
    /* W1: comparisons and spaceship as VALUES on exact i64 operands.
       - The false value of a Perl comparison is the empty string, not IV 0
         (perl_alloc_bool) — this also fixes the latent box-path bug where
         `print ($a < $b)` printed "0" on false.
       - i64 compare is exact for magnitudes > 2^53, where the boxed
         perl_to_float comparison loses precision (latent bug #2 fixed).
       - `<=>` yields IV -1/0/1 (boxI64). */
    if (n.sval == "<" || n.sval == "<=" || n.sval == ">" || n.sval == ">=" ||
        n.sval == "==" || n.sval == "!=" || n.sval == "<=>") {
        if (n.left && n.right && canEmitI64(*n.left) && canEmitI64(*n.right)) {
            Value *lv = emitExprI64(*n.left), *rv = emitExprI64(*n.right);
            if (lv && rv) {
                auto *i64 = Type::getInt64Ty(ctx_);
                if (n.sval == "<=>") {
                    Value *lt = builder_.CreateICmpSLT(lv, rv, "ss.lt");
                    Value *gt = builder_.CreateICmpSGT(lv, rv, "ss.gt");
                    Value *r = builder_.CreateSelect(
                        lt, ConstantInt::get(i64, -1),
                        builder_.CreateSelect(gt, ConstantInt::get(i64, 1),
                                               ConstantInt::get(i64, 0), "ss.mid"),
                        "spaceship");
                    return boxI64(r);
                }
                llvm::CmpInst::Predicate pred;
                if      (n.sval == "<")  pred = llvm::CmpInst::Predicate::ICMP_SLT;
                else if (n.sval == "<=") pred = llvm::CmpInst::Predicate::ICMP_SLE;
                else if (n.sval == ">")  pred = llvm::CmpInst::Predicate::ICMP_SGT;
                else if (n.sval == ">=") pred = llvm::CmpInst::Predicate::ICMP_SGE;
                else if (n.sval == "==") pred = llvm::CmpInst::Predicate::ICMP_EQ;
                else                    pred = llvm::CmpInst::Predicate::ICMP_NE;
                Value *c = builder_.CreateICmp(pred, lv, rv, "icmpv");
                return callRT("perl_alloc_bool", {builder_.CreateZExt(c, i64)});
            }
        }
    }
    /* Fast path: if both operands can be expressed as doubles, stay unboxed.
       D132: when either operand is a scalar variable, route through the
       BigInt-guard wrapper instead of committing unconditionally to the
       native-double path — a variable can hold a BigInt-tagged value at
       runtime (D103 auto-promotion), which the native path truncates. The
       wrapper emits the identical F64 instructions on the non-BigInt
       branch, so non-BigInt code is unchanged; variable-free operands
       skip the wrapper entirely (literals are never runtime-BigInt). */
    if (n.sval == "+" || n.sval == "-" || n.sval == "*" || n.sval == "/") {
        bool hasVarOperand = (n.left && n.left->kind == NK::ScalarVar) ||
                             (n.right && n.right->kind == NK::ScalarVar);
        if (hasVarOperand && n.sval != "/") {
            if (Value *gv = emitF64BinOpWithBigIntGuard(n))
                return gv;
        }
        if (Value *fv = emitExprF64(n))
            return boxF64(fv);
    }
    /* short-circuit ops.
       D94: Perl evaluates the LEFT operand of ||/&&/or/and/ // in scalar
       context always (it's only boolean-tested / defined-tested). The RIGHT
       operand, when reached, inherits the surrounding expression's own
       context — so `my @a = (0 || listret())` must give listret() list
       context. callCtx_ is a one-shot flag consumed by the first Call it
       reaches; without saving/restoring here, a Call on the left would
       both wrongly see list context AND starve the right of it. */
    if (n.sval == "&&") {
        auto *fn   = builder_.GetInsertBlock()->getParent();
        auto *rhsBB  = BasicBlock::Create(ctx_, "and.rhs",  fn);
        auto *endBB  = BasicBlock::Create(ctx_, "and.end",  fn);
        int savedCallCtx = callCtx_;
        callCtx_ = 0;   /* left always scalar */
        Value *lv  = emitExpr(*n.left);
        callCtx_ = savedCallCtx;
        Value *lb  = callRT("perl_is_true", {lv});
        Value *ltrue = builder_.CreateICmpNE(lb, ConstantInt::get(Type::getInt32Ty(ctx_), 0));
        auto *lBB  = builder_.GetInsertBlock();
        builder_.CreateCondBr(ltrue, rhsBB, endBB);

        builder_.SetInsertPoint(rhsBB);
        callCtx_ = savedCallCtx;  /* right inherits outer context */
        Value *rv = emitShortCircuitRhs(*n.right);
        callCtx_ = savedCallCtx;
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
        int savedCallCtx = callCtx_;
        callCtx_ = 0;   /* left always scalar */
        Value *lv  = emitExpr(*n.left);
        callCtx_ = savedCallCtx;
        Value *lb  = callRT("perl_is_true", {lv});
        Value *ltrue = builder_.CreateICmpNE(lb, ConstantInt::get(Type::getInt32Ty(ctx_), 0));
        auto *lBB  = builder_.GetInsertBlock();
        builder_.CreateCondBr(ltrue, endBB, rhsBB);

        builder_.SetInsertPoint(rhsBB);
        callCtx_ = savedCallCtx;  /* right inherits outer context */
        Value *rv = emitShortCircuitRhs(*n.right);
        callCtx_ = savedCallCtx;
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
        int savedCallCtx = callCtx_;
        callCtx_ = 0;   /* left always scalar (defined-test) */
        Value *lv   = emitExpr(*n.left);
        callCtx_ = savedCallCtx;
        Value *lb   = callRT("perl_defined", {lv});
        Value *ldef = builder_.CreateICmpNE(lb, ConstantInt::get(Type::getInt32Ty(ctx_), 0));
        auto *lBB   = builder_.GetInsertBlock();
        builder_.CreateCondBr(ldef, endBB, rhsBB);

        builder_.SetInsertPoint(rhsBB);
        callCtx_ = savedCallCtx;  /* right inherits outer context */
        Value *rv = emitExpr(*n.right);
        callCtx_ = savedCallCtx;
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

        /* Helper: emit a ternary branch. ArrayLit/list-producers need
           wantarray-aware wrapping. For ArrayLit/ArrayVar/DerefArray,
           perl_array_to_list_return already picks list-wrap vs last-elem
           from the wantarray stack (same as `return (1,2)`). Map/grep/
           sort need the count/undef scalar special cases. */
        auto emitBranch = [&](const Node &branchNode) -> Value * {
            if (branchNode.kind == NK::ArrayLit || branchNode.kind == NK::ArrayVar ||
                branchNode.kind == NK::DerefArray) {
                Value *av = emitArrayPtr(branchNode);
                if (!av) av = callRT("perl_array_new", {});
                return callRT("perl_array_to_list_return", {av});
            }
            if (branchNode.kind == NK::MapFunc || branchNode.kind == NK::GrepFunc ||
                branchNode.kind == NK::SortFunc || branchNode.kind == NK::ReverseFunc) {
                auto *i32Ty = Type::getInt32Ty(ctx_);
                Value *ctx = callRT("perl_current_wantarray_ctx", {});
                Value *isList = builder_.CreateICmpEQ(ctx, ConstantInt::get(i32Ty, 1));
                Value *av = emitArrayPtr(branchNode);
                if (!av) av = callRT("perl_array_new", {});
                Value *listResult = callRT("perl_array_to_list_return", {av});
                Value *scalarResult = perlUndef();
                if (branchNode.kind == NK::MapFunc || branchNode.kind == NK::GrepFunc)
                    scalarResult = callRT("perl_array_len", {av});
                else if (branchNode.kind == NK::SortFunc)
                    scalarResult = perlUndef();
                else if (branchNode.kind == NK::ReverseFunc)
                    scalarResult = callRT("perl_array_len", {av});
                return builder_.CreateSelect(isList, listResult, scalarResult);
            }
            return emitExpr(branchNode);
        };

        builder_.SetInsertPoint(thenBB);
        Value *tv = emitBranch(*n.left);
         auto *tb  = builder_.GetInsertBlock();
         bool thenTerminated = tb->getTerminator() != nullptr;
         if (!thenTerminated) builder_.CreateBr(endBB);

         builder_.SetInsertPoint(elseBB);
         Value *ev = emitBranch(*n.right);
         auto *eb  = builder_.GetInsertBlock();
         bool elseTerminated = eb->getTerminator() != nullptr;
         if (!elseTerminated) builder_.CreateBr(endBB);

         builder_.SetInsertPoint(endBB);
         auto *phi = builder_.CreatePHI(perlPtrTy_, 2, "tern.result");
         phi->addIncoming(tv, tb);
         if (!elseTerminated) phi->addIncoming(ev, eb);
         return phi;
     }

    /* D88: every operator reaching this point (arithmetic, string,
       comparison, bitwise, `x`, `<=>`/`cmp`) always evaluates BOTH
       operands in scalar context in real Perl, regardless of the
       surrounding expression's own context — but callCtx_ is a blunt,
       unscoped one-shot flag consumed by whichever Call/MethodCall/
       CallCodeRef node happens to be evaluated next. Several call sites
       elsewhere in this file set callCtx_=1 before evaluating an entire
       RHS/argument expression tree without knowing (or caring) whether
       that tree's root is itself a call or, as here, a BinOp with a call
       buried in one of its operands — e.g. `my @a = (ctx() eq "x")`
       previously leaked the list-context RHS's callCtx_=1 straight
       through `eq` into `ctx()`, reporting list context even though `eq`
       always forces scalar context on its own operands. Since every
       non-short-circuit binary operator funnels through this single
       spot, clearing callCtx_ here (and restoring it after) fixes the
       leak for this whole operator class in one place, without needing
       to audit or special-case every individual call site that sets
       callCtx_ elsewhere in the file (D12's print/printf-specific
       isCallLikeForContext guard remains the right, narrower fix for
       print/printf's own list-context *propagation* into a directly-
       nested call argument — this is the complementary fix for what
       happens once evaluation descends into a scalar-forcing operator). */
    int savedCallCtx = callCtx_;
    callCtx_ = 0; /* D88: force scalar on both operands */
    Value *lv = emitExpr(*n.left);
    callCtx_ = 0;
    Value *rv = emitExpr(*n.right);
    callCtx_ = savedCallCtx;

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

     /* Don't inline subs that use wantarray() or call other subs — inlining
        would break wantarray context propagation (the inlined body would see
        the caller's wantarray context instead of the sub's own). */
     if (hasWantarrayOrUserCall(*is.bodyExpr)) return nullptr;

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
    /* D116: __FILE__ — the parser can't resolve this itself (only
       codegen's sourceFile_ tracks the compiling filename), so it's
       represented as a plain Call and intercepted here, first, so it
       never falls through to the generic "undefined sub" die (D113). */
    if (n.name == "__FILE__") return perlStr(sourceFile_);

    /* D124: __SUB__ — reference to the currently-executing sub.
       - Inside an emitted closure body (AnonSub / sort comparator), the
         correct answer is the RUNNING closure's code-ref object — only
         the runtime knows it (it carries the fn pointer AND the capture
         set; a compile-time fresh perl_make_code_ref(currentFn_) would
         silently drop the closure's own captures). The runtime call
         returns undef when no closure is executing, which is also the
         right answer for __SUB__ at file scope (matches real perl).
       - Inside a named sub's body, it's that named sub's own code ref —
         same node shape NK::RefSub already produces (named subs here
         resolve free variables by name, not captures, so a capture-less
         code ref matches the existing model). */
    if (n.name == "__SUB__") {
        if (inAnonSubEmit_)
            return callRT("perl_get_current_code_ref", {});
        if (!currentSubName_.empty()) {
            auto *subFn = mod_->getFunction(subLLVMName(currentSubName_));
            if (subFn) {
                Value *fnPtr = ConstantExpr::getPointerCast(
                    subFn, PointerType::getUnqual(ctx_));
                return callRT("perl_make_code_ref", {fnPtr});
            }
            return perlUndef();
        }
        return callRT("perl_get_current_code_ref", {});
    }

    /* D103: an integer literal beyond INT64_MAX but within Perl's UV range
       (0..UINT64_MAX) — see parser.cpp's identical interception pattern
       and comment for __FILE__ above, and the D103 comment block above
       perl_add in runtime.c for the full auto-BigInt rationale. */
    if (n.name == "__auto_bigint_lit")
        return callRT("perl_bigint_from_decstr_unblessed",
                       {builder_.CreateGlobalStringPtr(n.sval)});

    /* Try AST-level inline first: eliminates @_ construction for simple subs. */
    if (Value *v = tryEmitInline(n)) return v;

    /* eval EXPR — constant strings without new subs compile as EvalBlock
       (outer lexicals visible). Dynamic strings and strings that define
       subs go through perl_eval_string (--do-lib). */
    if (n.name == "eval") {
        int savedCallCtx = callCtx_;
        callCtx_ = 0;
        auto nodeHasSub = [&](auto &&self, const Node *nd) -> bool {
            if (!nd) return false;
            if (nd->kind == NK::SubDef || nd->kind == NK::AnonSub ||
                nd->kind == NK::PackageStmt || nd->kind == NK::BeginBlock ||
                nd->kind == NK::EndBlock)
                return true;
            if (self(self, nd->left.get()) || self(self, nd->right.get()) ||
                self(self, nd->cond.get()) || self(self, nd->body.get()) ||
                self(self, nd->init.get()) || self(self, nd->step.get()))
                return true;
            for (auto &a : nd->args) if (self(self, a.get())) return true;
            for (auto &br : nd->branches)
                if (self(self, br.cond.get()) || self(self, br.body.get())) return true;
            return false;
        };
        const Node *src = n.args.empty() ? nullptr : n.args[0].get();
        /* eval("str") parses as a 1-element ArrayLit because (LIST) is a list. */
        if (src && src->kind == NK::ArrayLit && src->args.size() == 1)
            src = src->args[0].get();
        if (src && src->kind == NK::StringLit) {
            try {
                Lexer lex(src->sval);
                auto toks = lex.tokenize();
                Parser p(std::move(toks));
                NodePtr body = p.parseProgram();
                if (body && !nodeHasSub(nodeHasSub, body.get())) {
                    Node ev;
                    ev.kind = NK::EvalBlock;
                    ev.line = n.line;
                    ev.body = std::move(body);
                    callCtx_ = savedCallCtx;
                    Value *r = emitExpr(ev);
                    callCtx_ = 0;
                    /* Materialize through an alloca so the result is a
                       stable load after setjmp (needed when eval STRING
                       is a Call used as a comparison operand). */
                    auto *hold = builder_.CreateAlloca(perlPtrTy_, nullptr, "eval.hold");
                    builder_.CreateStore(r, hold);
                    return builder_.CreateLoad(perlPtrTy_, hold, "eval.val");
                }
            } catch (const std::exception &ex) {
                std::string msg = std::string(ex.what()) + " at (eval 1) line 1.";
                callRT("perl_assign", {callRT("perl_get_dollar_at", {}), perlStr(msg)});
                return perlUndef();
            }
            /* SubDef in the string: fall through to runtime compile so the
               sub is actually emitted and registered. */
        }
        Value *code = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        auto *i32Ty = Type::getInt32Ty(ctx_);
        bool pushedWA = false;
        if (savedCallCtx != -1) {
            int ctxInt = (savedCallCtx == 1) ? 1 : (savedCallCtx == 2) ? 2 : 0;
            callRT("perl_push_wantarray", {ConstantInt::get(i32Ty, ctxInt)});
            pushedWA = true;
        }
        emitEvalPadFill();
        Value *r = callRT("perl_eval_string", {code});
        callRT("perl_eval_pad_clear", {});
        if (pushedWA) callRT("perl_pop_wantarray", {});
        freeIfOwned(code);
        return r;
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
    if (n.name == "POSIX::fmod" || n.name == "fmod") {
        Value *a = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *b = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        return callRT("perl_posix_fmod", {a, b});
    }
    if (n.name == "POSIX::strftime" || n.name == "strftime") {
        return callRT("perl_posix_strftime", {buildArgArray()});
    }
    /* D69: List::Util::sum/min/max/uniq, scalar context. These only reach
       emitCall as a qualified NK::Call — the bare "sum"/"min"/"max"/"uniq"
       keyword forms are their own dedicated node kinds (NK::SumFunc etc.,
       handled in emitExpr's own switch) and never reach here at all.
       Previously the qualified names matched none of these checks and
       fell through to the generic "::"-qualified-call fallback further
       below, which only knows how to call a real, separately-compiled
       LLVM sub — since none of these are that, it silently produced an
       empty/undef result. buildArgArray() (defined above) doesn't flatten
       an array-variable argument's elements, so it can't be reused here;
       this mirrors the correct flattening NK::SumFunc/MinFunc/MaxFunc/
       UniqFunc's own emitExpr case already does. */
    if (n.name == "List::Util::sum" || n.name == "List::Util::min" ||
        n.name == "List::Util::max" || n.name == "List::Util::uniq") {
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
        if (n.name == "List::Util::sum") return callRT("perl_sum_list", {av});
        if (n.name == "List::Util::min") return callRT("perl_min_list", {av});
        if (n.name == "List::Util::max") return callRT("perl_max_list", {av});
        /* uniq: scalar context returns the COUNT of unique elements
           (matching the identical fix in the bare-keyword NK::UniqFunc
           case above). */
        return callRT("perl_array_len", {callRT("perl_uniq_list", {av})});
    }
    /* Time::HiRes (D30, built-in) — scalar-context path. "Time::HiRes::time"
       and "Time::HiRes::sleep" only reach here when explicitly imported
       (parser.cpp gates them); gettimeofday/usleep/tv_interval have no
       pre-existing keyword meaning so are always available unqualified,
       matching the POSIX::floor precedent above. gettimeofday's list-
       context form is handled separately in emitArrayPtr. */
    if (n.name == "Time::HiRes::time") {
        return callRT("perl_hires_time", {});
    }
    if (n.name == "Time::HiRes::sleep") {
        Value *v = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_hires_sleep", {v});
    }
    if (n.name == "Time::HiRes::usleep" || n.name == "usleep") {
        Value *v = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_hires_usleep", {v});
    }
    if (n.name == "Time::HiRes::gettimeofday" || n.name == "gettimeofday") {
        return callRT("perl_hires_gettimeofday_scalar", {});
    }
    if (n.name == "Time::HiRes::tv_interval" || n.name == "tv_interval") {
        Value *a = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *b = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        return callRT("perl_hires_tv_interval", {a, b});
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
        /* perl_carp_croak never returns — it calls die/exit */
        return perlUndef();
    }
    if (n.name == "Carp::carp" || n.name == "carp" ||
        n.name == "Carp::cluck" || n.name == "cluck") {
        callRT("perl_carp_carp", {buildArgArray()});
        return perlUndef();
    }
    /* Getopt::Long: GetOptions("foo=s" => \$foo, ...) or
       GetOptions(\%opt, "foo=s", ...) — build a plain array of the raw
       evaluated call args (spec strings and ref targets, in order; refs
       evaluate to real REF_SCALAR/REF_ARRAY/REF_HASH PerlValue*s via the
       ordinary \$x/\@x/\%x codegen, same as any other call argument) and
       hand it to perl_getopt_long along with the live @ARGV array, which
       it mutates in place to remove recognized options. */
    /* Getopt::Long::Configure("no_ignore_case", ...) — real Getopt::Long
       mutates its parser config; perlc's perl_getopt_long implements the
       fixed default configuration (no pass_through/gnu_getopt), so a
       Configure() call is accepted and ignored. Without this, any real
       script calling Getopt::Long::Configure (xsubpp, dirsplit's
       qw(:config ...) import path, ...) died "Undefined subroutine
       &Getopt::Long::Configure". Returns 1 like real Configure. */
    if (n.name == "Configure" || n.name == "Getopt::Long::Configure") {
        return perlInt(1);
    }
    if (n.name == "GetOptions" || n.name == "Getopt::Long::GetOptions") {
        Value *argsArr = buildArgArray();
        Value *argv = lookupArray("ARGV");
        if (!argv) argv = callRT("perl_array_new", {});
        return callRT("perl_getopt_long", {argsArr, argv});
    }
    /* Data::Dumper: flatten any array/list args (Dumper(@list)) the same
       way List::Util::sum/min/max do above, rather than buildArgArray()'s
       plain per-expression push, so `Dumper(@list)` dumps one $VARn per
       element instead of one $VAR1 holding a LIST_RESULT. */
    if (n.name == "Dumper" || n.name == "Data::Dumper::Dumper") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            Value *sub = emitArrayPtr(*a);
            if (sub) callRT("perl_array_extend", {av, sub});
            else     callRT("perl_array_push",   {av, emitExpr(*a)});
        }
        /* $Data::Dumper::Sortkeys: a plain in-scope variable lookup, not
           a true cross-package global read (see perl_dumper's comment) —
           works when set earlier in the same scope, which is by far the
           most common real-world usage (set once near the top of a
           script or sub). */
        Value *skVar = lookupVar("Data::Dumper::Sortkeys");
        Value *sk = skVar ? builder_.CreateLoad(perlPtrTy_, skVar) : perlUndef();
        return callRT("perl_dumper", {av, sk});
    }
    /* File::Basename */
    if (n.name == "File::Basename::basename" || n.name == "basename") {
        Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        Value *suf = callRT("perl_array_new", {});
        for (size_t k = 1; k < n.args.size(); k++)
            callRT("perl_array_push", {suf, emitExpr(*n.args[k])});
        return callRT("perl_basename", {path, suf});
    }
    if (n.name == "File::Basename::dirname" || n.name == "dirname") {
        Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_dirname", {path});
    }
    if (n.name == "File::Basename::fileparse" || n.name == "fileparse") {
        /* Scalar context: just the name (first element) — matches real
           File::Basename. List context is intercepted in emitArrayPtr
           above and never reaches here. */
        Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        Value *suf = callRT("perl_array_new", {});
        for (size_t k = 1; k < n.args.size(); k++)
            callRT("perl_array_push", {suf, emitExpr(*n.args[k])});
        Value *arr = callRT("perl_fileparse", {path, suf});
        Value *elem = callRT("perl_array_get_ref", {arr, ConstantInt::get(Type::getInt64Ty(ctx_), 0)});
        Value *cloned = callRT("perl_clone", {elem});
        callRT("perl_array_free", {arr});
        return cloned;
    }
    /* ── Storable::dclone (Tier 2, native) ──
       bare `dclone` (the codebase's looseness for @EXPORT_OK names) is
       kept; real Storable has no `copy` function (dclone IS the copy
       entry point), and bare `copy` must stay File::Copy's. */
    if (n.name == "Storable::dclone" || n.name == "dclone") {
        return callRT("perl_storable_dclone",
                      {n.args.size() >= 1 ? emitExpr(*n.args[0]) : perlUndef()});
    }
    /* ── JSON::PP (Tier 2, native) ──
       Functional encode_json/decode_json (real @EXPORT, so the bare
       unqualified name always works — same looseness Storable::dclone's
       comment above documents). Not canonical/pretty by default, matching
       real encode_json's signature; the ->canonical/->pretty chain lives
       in perl_dispatch_method (src/runtime.c) since it's genuinely
       stateful OO, not a pure function. JSON::PP::true/false are the
       bare constant forms (`use JSON::PP qw(true false)`). */
    if (n.name == "JSON::PP::encode_json" || n.name == "JSON::encode_json" ||
        n.name == "encode_json") {
        auto *i64Ty = Type::getInt64Ty(ctx_);
        return callRT("perl_json_encode",
                      {n.args.size() >= 1 ? emitExpr(*n.args[0]) : perlUndef(),
                       ConstantInt::get(i64Ty, 0), ConstantInt::get(i64Ty, 0)});
    }
    if (n.name == "JSON::PP::decode_json" || n.name == "JSON::decode_json" ||
        n.name == "decode_json") {
        return callRT("perl_json_decode",
                      {n.args.size() >= 1 ? emitExpr(*n.args[0]) : perlUndef()});
    }
    if (n.name == "JSON::PP::true" || n.name == "JSON::true") return callRT("perl_json_true", {});
    if (n.name == "JSON::PP::false" || n.name == "JSON::false") return callRT("perl_json_false", {});
    /* ── Text::Wrap (Tier 1, native) ──
       wrap(IP, XP, @texts) / fill(IP, XP, @lines) with the live package
       vars $Text::Wrap::columns / $separator / $separator2 / $huge read
       at call time through the glob registry. */
    {
        auto isWrapCall = [&](const char *bare, const char *qual) {
            return n.name == qual || n.name == bare;
        };
        if (isWrapCall("wrap", "Text::Wrap::wrap") ||
            isWrapCall("fill", "Text::Wrap::fill")) {
            bool isFill = isWrapCall("fill", "Text::Wrap::fill");
            Value *ip = n.args.size() >= 1 ? emitExpr(*n.args[0]) : perlStr("");
            Value *xp = n.args.size() >= 2 ? emitExpr(*n.args[1]) : perlStr("");
            Value *av = callRT("perl_array_new", {});
            for (size_t k = 2; k < n.args.size(); k++) {
                Value *sub = emitArrayPtr(*n.args[k]);
                if (sub) callRT("perl_array_extend", {av, sub});
                else     callRT("perl_array_push",   {av, emitExpr(*n.args[k])});
            }
            auto pkgVar = [&](const char *bare, const char *dflt) -> Value* {
                std::string qn = std::string("Text::Wrap::") + bare;
                Value *key = builder_.CreateGlobalStringPtr(qn);
                Value *pv = callRT("perl_glob_get_scalar", {key});
                /* if unset, the PV is undef — runtime falls back to the default */
                (void)dflt;
                return pv;
            };
            Value *cols = pkgVar("columns", "76");
            Value *sep  = pkgVar("separator", "");
            Value *sep2 = pkgVar("separator2", "");
            Value *huge = pkgVar("huge", "");
            Value *unx  = pkgVar("unexpand", "");
            if (isFill)
                return callRT("perl_text_fill",
                              {ip, xp, av, cols, sep, sep2, huge, unx});
            return callRT("perl_text_wrap",
                          {ip, xp, av, cols, sep, sep2, huge, unx});
        }
    }
    /* ── File::Temp (Tier 1, native) ──
       tempdir(TEMPLATE | DIR=>.. | TEMPLATE=>.. | no args) → new 0700
       dir; tempfile(DIR=>.., SUFFIX=>..) → (fh, name) in list ctx, fh in
       scalar ctx; mkstemp/mkdtemp/mktemp on an XXXXXX template;
       tmpnam(). Args go to the runtime flat so DIR=>/SUFFIX=>/TEMPLATE=>
       key/value pairs are paired there (D136 auto-quote makes the keys
       strings). */
    if (n.name == "File::Temp::tempdir" || n.name == "tempdir") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) callRT("perl_array_push", {av, emitExpr(*a)});
        return callRT("perl_file_temp",
                      {av, ConstantInt::get(Type::getInt32Ty(ctx_), 0),
                       ConstantInt::get(Type::getInt32Ty(ctx_), 0)});
    }
    if (n.name == "File::Temp::tempfile" || n.name == "tempfile") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) callRT("perl_array_push", {av, emitExpr(*a)});
        return callRT("perl_file_temp",
                      {av, ConstantInt::get(Type::getInt32Ty(ctx_), 1),
                       ConstantInt::get(Type::getInt32Ty(ctx_), 1)});
    }
    if (n.name == "File::Temp::mkstemp" || n.name == "mkstemp") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) callRT("perl_array_push", {av, emitExpr(*a)});
        return callRT("perl_file_temp_template",
                      {av, ConstantInt::get(Type::getInt32Ty(ctx_), 0),
                       ConstantInt::get(Type::getInt32Ty(ctx_), 1)});
    }
    if (n.name == "File::Temp::mkdtemp" || n.name == "mkdtemp") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) callRT("perl_array_push", {av, emitExpr(*a)});
        return callRT("perl_file_temp_template",
                      {av, ConstantInt::get(Type::getInt32Ty(ctx_), 1),
                       ConstantInt::get(Type::getInt32Ty(ctx_), 0)});
    }
    if (n.name == "File::Temp::mktemp" || n.name == "mktemp") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) callRT("perl_array_push", {av, emitExpr(*a)});
        return callRT("perl_file_temp_template",
                      {av, ConstantInt::get(Type::getInt32Ty(ctx_), 2),
                       ConstantInt::get(Type::getInt32Ty(ctx_), 0)});
    }
    if (n.name == "File::Temp::tmpnam" || n.name == "tmpnam")
        return callRT("perl_file_temp_tmpnam", {});
    /* ── File::Find (Tier 1, native) ──
       find(\&wanted, @dirs) / finddepth(\&wanted, @dirs) /
       find({wanted=>..., no_chdir=>..., bydepth=>...}, @dirs) — the
       wanted coderef + dirs go to the runtime, which sets $_ and the
       $File::Find::* globals per entry. Returns undef like real find. */
    if (n.name == "File::Find::find" || n.name == "find" ||
        n.name == "File::Find::finddepth" || n.name == "finddepth") {
        if (n.args.empty()) return perlUndef();
        int dfs = (n.name == "finddepth" ||
                   n.name == "File::Find::finddepth") ? 1 : 0;
        Value *w = emitExpr(*n.args[0]);
        Value *dirs = callRT("perl_array_new", {});
        for (size_t k = 1; k < n.args.size(); k++) {
            Value *sub = emitArrayPtr(*n.args[k]);
            if (sub) callRT("perl_array_extend", {dirs, sub});
            else     callRT("perl_array_push",   {dirs, emitExpr(*n.args[k])});
        }
        return callRT("perl_file_find",
                      {w, dirs, ConstantInt::get(Type::getInt32Ty(ctx_), dfs)});
    }
    /* ── File::Path (Tier 1, native) ──
       make_path(dirs..., {opts}) / mkpath(dirs... | [$dirs], $verbose,
       $mode) — creates each missing component; list ctx returns the
       created component paths, scalar ctx the count. remove_tree/rmtree
       return per-dir removal counts (list) or the total (scalar). */
    if (n.name == "File::Path::make_path" || n.name == "make_path" ||
        n.name == "File::Path::mkpath"   || n.name == "mkpath") {
        std::vector<Value*> raw;
        for (auto &a : n.args) raw.push_back(emitExpr(*a));
        Value *opts = perlUndef();
        auto *i32TyP = Type::getInt32Ty(ctx_);
        callRT("perl_push_call_frame",
            {builder_.CreateGlobalStringPtr(currentPackage_),
             builder_.CreateGlobalStringPtr(sourceFile_),
             ConstantInt::get(i32TyP, n.line)});
        Value *dirs = callRT("perl_array_new", {});
        for (size_t k = 0; k < raw.size(); k++)
            callRT("perl_fpath_collect", {dirs, raw[k]});
        Value *r = callRT("perl_make_path",
                          {dirs, opts, ConstantInt::get(Type::getInt32Ty(ctx_), 0)});
        callRT("perl_pop_call_frame", {});
        return r;
    }
    if (n.name == "File::Path::remove_tree" || n.name == "remove_tree" ||
        n.name == "File::Path::rmtree"   || n.name == "rmtree") {
        std::vector<Value*> raw;
        for (auto &a : n.args) raw.push_back(emitExpr(*a));
        Value *opts = perlUndef();
        Value *dirs = callRT("perl_array_new", {});
        for (size_t k = 0; k < raw.size(); k++)
            callRT("perl_fpath_collect", {dirs, raw[k]});
        return callRT("perl_remove_tree",
                      {dirs, opts, ConstantInt::get(Type::getInt32Ty(ctx_), 0)});
    }
    /* ── File::Copy (Tier 1, native) ──
       copy(FROM,TO[,BUF])/syscopy/cp and move/mv — FROM/TO may be path
       strings or filehandles; returns 1/0 with $! set on failure. Bare
       copy/move are @EXPORT (work after plain `use File::Copy;`); cp/mv
       are @EXPORT_OK (importMap routes them to the qualified names).
       A `*GLOB` argument additionally passes its display name so the
       runtime's identical-file warning prints '*main::A' like real
       File::Copy (whose stat() sees through glob handles). */
    auto emitFhArg = [&](const Node &a, Value **pv, Value **np) {
        *pv = emitExpr(a);
        if (a.kind == NK::Typeglob)
            *np = perlStr("*" + currentPackage_ + "::" + a.name);
        else if (a.kind == NK::StringLit)
            *np = perlStr(a.sval);
        else
            *np = perlUndef();
    };
    if (n.name == "File::Copy::copy" || n.name == "copy" ||
        n.name == "File::Copy::syscopy" || n.name == "syscopy" ||
        n.name == "File::Copy::cp" || n.name == "cp") {
        Value *a, *an, *b, *bn;
        emitFhArg(*n.args[0], &a, &an);
        emitFhArg(*n.args[1], &b, &bn);
        Value *c = n.args.size() > 2 ? emitExpr(*n.args[2]) : perlUndef();
        auto *i32TyF = Type::getInt32Ty(ctx_);
        callRT("perl_push_call_frame",
            {builder_.CreateGlobalStringPtr(currentPackage_),
             builder_.CreateGlobalStringPtr(sourceFile_),
             ConstantInt::get(i32TyF, n.line)});
        Value *r = callRT("perl_fcopy", {a, b, c, an, bn});
        callRT("perl_pop_call_frame", {});
        return r;
    }
    if (n.name == "File::Copy::move" || n.name == "move" ||
        n.name == "File::Copy::mv" || n.name == "mv") {
        Value *a, *an, *b, *bn;
        emitFhArg(*n.args[0], &a, &an);
        emitFhArg(*n.args[1], &b, &bn);
        auto *i32TyM = Type::getInt32Ty(ctx_);
        callRT("perl_push_call_frame",
            {builder_.CreateGlobalStringPtr(currentPackage_),
             builder_.CreateGlobalStringPtr(sourceFile_),
             ConstantInt::get(i32TyM, n.line)});
        Value *r = callRT("perl_fmove", {a, b, an, bn});
        callRT("perl_pop_call_frame", {});
        return r;
    }
    /* ── Cwd (Tier 1, native) ──
       getcwd()/cwd()/fastcwd()/fastgetcwd() — all the same getcwd(3) call
       in real Cwd. abs_path/fast_abs_path/realpath/fast_realpath — all the
       same realpath(3)-equivalent (dies ENOENT, undef other failures).
       Both bare (explicitly imported or the export-defaulted names) and
       fully-qualified forms dispatch here. */
    if (n.name == "Cwd::getcwd" || n.name == "getcwd" ||
        n.name == "Cwd::cwd" || n.name == "cwd" ||
        n.name == "Cwd::fastcwd" || n.name == "fastcwd" ||
        n.name == "Cwd::fastgetcwd" || n.name == "fastgetcwd") {
        return callRT("perl_getcwd", {});
    }
    if (n.name == "Cwd::abs_path" || n.name == "abs_path" ||
        n.name == "Cwd::fast_abs_path" || n.name == "fast_abs_path" ||
        n.name == "Cwd::realpath" || n.name == "realpath" ||
        n.name == "Cwd::fast_realpath" || n.name == "fast_realpath") {
        Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_abs_path", {path});
    }
    /* ── Sys::Hostname (Tier 1, native) ── */
    if (n.name == "Sys::Hostname::hostname" || n.name == "hostname") {
        return callRT("perl_hostname", {});
    }
    /* ── File::Spec / File::Spec::Functions / File::Spec::Unix ──
       Method calls (File::Spec->catfile) are intercepted in the MethodCall
       case; these bare/qualified Call forms cover File::Spec::Unix::catdir
       and explicitly-imported File::Spec::Functions names (importMap maps
       them to File::Spec::Functions::<name>). */
    if (n.name == "File::Spec::catdir" || n.name == "File::Spec::Unix::catdir" ||
        n.name == "File::Spec::Functions::catdir" || n.name == "catdir") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            Value *sub = emitArrayPtr(*a);
            if (sub) callRT("perl_array_extend", {av, sub});
            else     callRT("perl_array_push",   {av, emitExpr(*a)});
        }
        return callRT("perl_fspec_catdir", {av});
    }
    if (n.name == "File::Spec::catfile" || n.name == "File::Spec::Unix::catfile" ||
        n.name == "File::Spec::Functions::catfile" || n.name == "catfile" ||
        n.name == "File::Spec::join" || n.name == "File::Spec::Unix::join" ||
        n.name == "File::Spec::Functions::join" || n.name == "join") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            Value *sub = emitArrayPtr(*a);
            if (sub) callRT("perl_array_extend", {av, sub});
            else     callRT("perl_array_push",   {av, emitExpr(*a)});
        }
        return callRT("perl_fspec_catfile", {av});
    }
    if (n.name == "File::Spec::catpath" || n.name == "File::Spec::Unix::catpath" ||
        n.name == "File::Spec::Functions::catpath" || n.name == "catpath") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            Value *sub = emitArrayPtr(*a);
            if (sub) callRT("perl_array_extend", {av, sub});
            else     callRT("perl_array_push",   {av, emitExpr(*a)});
        }
        return callRT("perl_fspec_catpath", {av});
    }
    if (n.name == "File::Spec::canonpath" || n.name == "File::Spec::Unix::canonpath" ||
        n.name == "File::Spec::Functions::canonpath" || n.name == "canonpath") {
        Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_fspec_canonpath", {path});
    }
    if (n.name == "File::Spec::curdir" || n.name == "File::Spec::Unix::curdir" ||
        n.name == "File::Spec::Functions::curdir" || n.name == "curdir") {
        return callRT("perl_fspec_curdir", {});
    }
    if (n.name == "File::Spec::updir" || n.name == "File::Spec::Unix::updir" ||
        n.name == "File::Spec::Functions::updir" || n.name == "updir") {
        return callRT("perl_fspec_updir", {});
    }
    if (n.name == "File::Spec::rootdir" || n.name == "File::Spec::Unix::rootdir" ||
        n.name == "File::Spec::Functions::rootdir" || n.name == "rootdir") {
        return callRT("perl_fspec_rootdir", {});
    }
    if (n.name == "File::Spec::devnull" || n.name == "File::Spec::Unix::devnull" ||
        n.name == "File::Spec::Functions::devnull" || n.name == "devnull") {
        return callRT("perl_fspec_devnull", {});
    }
    if (n.name == "File::Spec::tmpdir" || n.name == "File::Spec::Unix::tmpdir" ||
        n.name == "File::Spec::Functions::tmpdir" || n.name == "tmpdir") {
        return callRT("perl_fspec_tmpdir", {});
    }
    if (n.name == "File::Spec::file_name_is_absolute" ||
        n.name == "File::Spec::Unix::file_name_is_absolute" ||
        n.name == "File::Spec::Functions::file_name_is_absolute" ||
        n.name == "file_name_is_absolute") {
        Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_fspec_file_name_is_absolute", {path});
    }
    if (n.name == "File::Spec::rel2abs" || n.name == "File::Spec::Unix::rel2abs" ||
        n.name == "File::Spec::Functions::rel2abs" || n.name == "rel2abs") {
        Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        Value *base = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        return callRT("perl_fspec_rel2abs", {path, base});
    }
    if (n.name == "File::Spec::abs2rel" || n.name == "File::Spec::Unix::abs2rel" ||
        n.name == "File::Spec::Functions::abs2rel" || n.name == "abs2rel") {
        Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        Value *base = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        return callRT("perl_fspec_abs2rel", {path, base});
    }
    if (n.name == "File::Spec::Functions::splitpath" || n.name == "splitpath") {
        Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        Value *nof  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        /* same context-adaptation as the MethodCall splitpath case */
        auto *i32Ty = Type::getInt32Ty(ctx_);
        int ctxInt = (callCtx_ == 1 || callCtx_ == 2) ? 1 : 0;
        callRT("perl_push_wantarray", {ConstantInt::get(i32Ty, ctxInt)});
        Value *r = callRT("perl_array_to_list_return",
                          {callRT("perl_fspec_splitpath", {path, nof})});
        callRT("perl_pop_wantarray", {});
        return r;
    }
    if (n.name == "File::Spec::Functions::splitdir" || n.name == "splitdir") {
        Value *dir = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        auto *i32Ty = Type::getInt32Ty(ctx_);
        int ctxInt = (callCtx_ == 1 || callCtx_ == 2) ? 1 : 0;
        callRT("perl_push_wantarray", {ConstantInt::get(i32Ty, ctxInt)});
        Value *r = callRT("perl_array_to_list_return",
                          {callRT("perl_fspec_splitdir", {dir})});
        callRT("perl_pop_wantarray", {});
        return r;
    }
    if (n.name == "File::Spec::Functions::no_upwards" || n.name == "no_upwards") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            Value *sub = emitArrayPtr(*a);
            if (sub) callRT("perl_array_extend", {av, sub});
            else     callRT("perl_array_push",   {av, emitExpr(*a)});
        }
        /* same context-adaptation as the MethodCall splitpath/splitdir cases */
        auto *i32Ty = Type::getInt32Ty(ctx_);
        int ctxInt = (callCtx_ == 1 || callCtx_ == 2) ? 1 : 0;
        callRT("perl_push_wantarray", {ConstantInt::get(i32Ty, ctxInt)});
        Value *r = callRT("perl_array_to_list_return",
                          {callRT("perl_fspec_no_upwards", {av})});
        callRT("perl_pop_wantarray", {});
        return r;
    }
    if (n.name == "File::Spec::Functions::path") {
        auto *i32Ty = Type::getInt32Ty(ctx_);
        int ctxInt = (callCtx_ == 1 || callCtx_ == 2) ? 1 : 0;
        callRT("perl_push_wantarray", {ConstantInt::get(i32Ty, ctxInt)});
        Value *r = callRT("perl_array_to_list_return", {callRT("perl_fspec_path", {})});
        callRT("perl_pop_wantarray", {});
        return r;
    }
    if (n.name == "File::Spec::Functions::case_tolerant" || n.name == "case_tolerant") {
        return callRT("perl_fspec_case_tolerant", {});
    }
    /* ── Time::Local (Tier 1, native) ── */
    if (n.name == "Time::Local::timegm" || n.name == "timegm") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            Value *sub = emitArrayPtr(*a);
            if (sub) callRT("perl_array_extend", {av, sub});
            else     callRT("perl_array_push",   {av, emitExpr(*a)});
        }
        /* croak-location support: push a caller frame so the runtime's
           range-check die reports this source line (real Time::Local's
           Carp::croak does exactly that). */
        auto *i32Ty = Type::getInt32Ty(ctx_);
        callRT("perl_push_call_frame",
            {builder_.CreateGlobalStringPtr(currentPackage_),
             builder_.CreateGlobalStringPtr(sourceFile_),
             ConstantInt::get(i32Ty, n.line)});
        Value *r = callRT("perl_timegm", {av});
        callRT("perl_pop_call_frame", {});
        return r;
    }
    if (n.name == "Time::Local::timelocal" || n.name == "timelocal") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            Value *sub = emitArrayPtr(*a);
            if (sub) callRT("perl_array_extend", {av, sub});
            else     callRT("perl_array_push",   {av, emitExpr(*a)});
        }
        /* croak-location support: push a caller frame so the runtime's
           range-check die reports this source line (real Time::Local's
           Carp::croak does exactly that). */
        auto *i32Ty = Type::getInt32Ty(ctx_);
        callRT("perl_push_call_frame",
            {builder_.CreateGlobalStringPtr(currentPackage_),
             builder_.CreateGlobalStringPtr(sourceFile_),
             ConstantInt::get(i32Ty, n.line)});
        Value *r = callRT("perl_timelocal", {av});
        callRT("perl_pop_call_frame", {});
        return r;
    }
    if (n.name == "Time::Local::timegm_nocheck" || n.name == "timegm_nocheck") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            Value *sub = emitArrayPtr(*a);
            if (sub) callRT("perl_array_extend", {av, sub});
            else     callRT("perl_array_push",   {av, emitExpr(*a)});
        }
        /* croak-location support: push a caller frame so the runtime's
           range-check die reports this source line (real Time::Local's
           Carp::croak does exactly that). */
        auto *i32Ty = Type::getInt32Ty(ctx_);
        callRT("perl_push_call_frame",
            {builder_.CreateGlobalStringPtr(currentPackage_),
             builder_.CreateGlobalStringPtr(sourceFile_),
             ConstantInt::get(i32Ty, n.line)});
        Value *r = callRT("perl_timegm_nocheck", {av});
        callRT("perl_pop_call_frame", {});
        return r;
    }
    if (n.name == "Time::Local::timelocal_nocheck" || n.name == "timelocal_nocheck") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            Value *sub = emitArrayPtr(*a);
            if (sub) callRT("perl_array_extend", {av, sub});
            else     callRT("perl_array_push",   {av, emitExpr(*a)});
        }
        /* croak-location support: push a caller frame so the runtime's
           range-check die reports this source line (real Time::Local's
           Carp::croak does exactly that). */
        auto *i32Ty = Type::getInt32Ty(ctx_);
        callRT("perl_push_call_frame",
            {builder_.CreateGlobalStringPtr(currentPackage_),
             builder_.CreateGlobalStringPtr(sourceFile_),
             ConstantInt::get(i32Ty, n.line)});
        Value *r = callRT("perl_timelocal_nocheck", {av});
        callRT("perl_pop_call_frame", {});
        return r;
    }
    if (n.name == "Time::Local::timegm_modern" || n.name == "timegm_modern") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            Value *sub = emitArrayPtr(*a);
            if (sub) callRT("perl_array_extend", {av, sub});
            else     callRT("perl_array_push",   {av, emitExpr(*a)});
        }
        /* croak-location support: push a caller frame so the runtime's
           range-check die reports this source line (real Time::Local's
           Carp::croak does exactly that). */
        auto *i32Ty = Type::getInt32Ty(ctx_);
        callRT("perl_push_call_frame",
            {builder_.CreateGlobalStringPtr(currentPackage_),
             builder_.CreateGlobalStringPtr(sourceFile_),
             ConstantInt::get(i32Ty, n.line)});
        Value *r = callRT("perl_timegm_modern", {av});
        callRT("perl_pop_call_frame", {});
        return r;
    }
    if (n.name == "Time::Local::timelocal_modern" || n.name == "timelocal_modern") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            Value *sub = emitArrayPtr(*a);
            if (sub) callRT("perl_array_extend", {av, sub});
            else     callRT("perl_array_push",   {av, emitExpr(*a)});
        }
        /* croak-location support: push a caller frame so the runtime's
           range-check die reports this source line (real Time::Local's
           Carp::croak does exactly that). */
        auto *i32Ty = Type::getInt32Ty(ctx_);
        callRT("perl_push_call_frame",
            {builder_.CreateGlobalStringPtr(currentPackage_),
             builder_.CreateGlobalStringPtr(sourceFile_),
             ConstantInt::get(i32Ty, n.line)});
        Value *r = callRT("perl_timelocal_modern", {av});
        callRT("perl_pop_call_frame", {});
        return r;
    }
    if (n.name == "Time::Local::timegm_posix" || n.name == "timegm_posix") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            Value *sub = emitArrayPtr(*a);
            if (sub) callRT("perl_array_extend", {av, sub});
            else     callRT("perl_array_push",   {av, emitExpr(*a)});
        }
        /* croak-location support: push a caller frame so the runtime's
           range-check die reports this source line (real Time::Local's
           Carp::croak does exactly that). */
        auto *i32Ty = Type::getInt32Ty(ctx_);
        callRT("perl_push_call_frame",
            {builder_.CreateGlobalStringPtr(currentPackage_),
             builder_.CreateGlobalStringPtr(sourceFile_),
             ConstantInt::get(i32Ty, n.line)});
        Value *r = callRT("perl_timegm_posix", {av});
        callRT("perl_pop_call_frame", {});
        return r;
    }
    if (n.name == "Time::Local::timelocal_posix" || n.name == "timelocal_posix") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            Value *sub = emitArrayPtr(*a);
            if (sub) callRT("perl_array_extend", {av, sub});
            else     callRT("perl_array_push",   {av, emitExpr(*a)});
        }
        /* croak-location support: push a caller frame so the runtime's
           range-check die reports this source line (real Time::Local's
           Carp::croak does exactly that). */
        auto *i32Ty = Type::getInt32Ty(ctx_);
        callRT("perl_push_call_frame",
            {builder_.CreateGlobalStringPtr(currentPackage_),
             builder_.CreateGlobalStringPtr(sourceFile_),
             ConstantInt::get(i32Ty, n.line)});
        Value *r = callRT("perl_timelocal_posix", {av});
        callRT("perl_pop_call_frame", {});
        return r;
    }
    /* Native constants (Fcntl/POSIX/Errno): a zero-arg call whose name is
       an all-caps identifier in one of those packages resolves through the
       generated value table (src/native_constants.h). Unknown names die
       like the real XS AUTOLOAD's invalid-macro die (message inside the
       runtime helper). Bare all-caps names reach here only via importMap
       ("<mod>::<name>" entries) after `use Fcntl qw(...)` etc. */
    {
        std::string bare = n.name;
        auto sc = bare.rfind("::");
        if (sc != std::string::npos) bare = bare.substr(sc + 2);
        bool allCaps = !bare.empty();
        for (char c : bare)
            if (!isupper((unsigned char)c) && c != '_' && !isdigit((unsigned char)c))
                { allCaps = false; break; }
        if (allCaps && n.args.empty() &&
            (n.name.rfind("Fcntl::", 0) == 0 ||
             n.name.rfind("POSIX::", 0) == 0 ||
             n.name.rfind("Errno::", 0) == 0)) {
            Value *r = callRT("perl_native_constant",
                {builder_.CreateGlobalStringPtr(n.name)});
            return r;
        }
    }
    /* sysseek(FH, offset, whence) — real lseek(2); SEEK_* constants are
       plain values by the time they arrive. */
    if (n.name == "sysseek" || n.name == "Fcntl::sysseek") {
        Value *fh = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        Value *off = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        Value *wh = n.args.size() > 2 ? emitExpr(*n.args[2]) : perlUndef();
        Value *r = callRT("perl_sysseek_fh", {fh, off, wh});
        freeIfOwned(fh); freeIfOwned(off); freeIfOwned(wh);
        return r;
    }
    /* Config (native): the 4 real functions. %Config access goes through
       the HashElem/ExistsFunc/KeysFunc special cases, not here. */
    if (n.name == "Config::myconfig" || n.name == "myconfig") {
        return callRT("perl_config_myconfig", {});
    }
    if (n.name == "Config::config_sh" || n.name == "config_sh") {
        return callRT("perl_config_configsh", {});
    }
    if (n.name == "Config::config_vars" || n.name == "config_vars") {
        Value *av = callRT("perl_array_new", {});
        for (auto &a : n.args) {
            Value *sub = emitArrayPtr(*a);
            if (sub) callRT("perl_array_extend", {av, sub});
            else     callRT("perl_array_push",   {av, emitExpr(*a)});
        }
        return callRT("perl_config_config_vars", {av});
    }
    if (n.name == "Config::config_re" || n.name == "config_re") {
        Value *pat = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        Value *r = callRT("perl_config_config_re", {pat});
        freeIfOwned(pat);
        return r;
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
    /* syscall() — takes syscall number as first arg, optional additional args */
    if (n.name == "syscall") {
        /* D134: push each argument by reference (perl_array_push_nc, no
           clone) so a syscall that writes through a pointer argument —
           SYS_clock_gettime's struct timespec buffer, read()'s buffer,
           etc. — writes into the caller's own string buffer (a stable
           variable cell's sval) instead of a pushed clone the caller can
           never see. This is the same in-place-mutation mechanism
           vec($str,off,bits)=val already relies on: emitExpr on a scalar
           variable returns the stable PerlValue* cell. Temporaries (e.g.
           `syscall(228, 4, "x" x 16)`) behave like real Perl's: the
           write lands in the temp and is discarded with it. The array
           shell is torn down with perl_array_free_nc (elements borrowed,
           never freed by the array); owned temp elements are freed
           explicitly after the call. */
        Value *av = callRT("perl_array_new", {});
        SmallVector<Value *, 8> owned;
        for (auto &arg : n.args) {
            Value *v = emitExpr(*arg);
            callRT("perl_array_push_nc", {av, v});
            if (isOwnedTemp(v)) owned.push_back(v);
        }
        Value *r = callRT("perl_syscall", {av});
        callRT("perl_array_free_nc", {av});
        for (Value *v : owned) callRT("perl_free", {v});
        return r;
    }
    /* W2: process / IPC / sockets — generic Call → perl_* helpers, no new AST */
    if (n.name == "fork")     return callRT("perl_fork", {});
    if (n.name == "wait")     return callRT("perl_wait_pid", {});
    if (n.name == "waitpid") {
        Value *pid = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *fl  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlInt(0);
        return callRT("perl_waitpid", {pid, fl});
    }
    if (n.name == "kill")     return callRT("perl_kill", {buildArgArray()});
    if (n.name == "exec")     return callRT("perl_exec", {buildArgArray()});
    if (n.name == "exit") {
        Value *c = n.args.empty() ? perlInt(0) : emitExpr(*n.args[0]);
        callRT("perl_exit_n", {c});
        return perlUndef();
    }
    if (n.name == "getppid")  return callRT("perl_getppid_val", {});
    if (n.name == "getuid")   return callRT("perl_getuid_val", {});
    if (n.name == "getgid")   return callRT("perl_getgid_val", {});
    if (n.name == "geteuid")  return callRT("perl_geteuid_val", {});
    if (n.name == "getegid")  return callRT("perl_getegid_val", {});
    if (n.name == "setsid")   return callRT("perl_setsid_val", {});
    if (n.name == "getpgrp") {
        Value *p = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_getpgrp_val", {p});
    }
    if (n.name == "setpgrp") {
        Value *a = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlInt(0);
        Value *b = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlInt(0);
        return callRT("perl_setpgrp_val", {a, b});
    }
    if (n.name == "umask") {
        Value *m = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_umask_val", {m});
    }
    if (n.name == "pipe") {
        Value *r = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *w = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        return callRT("perl_pipe_fh", {r, w});
    }
    if (n.name == "socket") {
        Value *fh = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *d  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlInt(0);
        Value *t  = n.args.size() > 2 ? emitExpr(*n.args[2]) : perlInt(0);
        Value *p  = n.args.size() > 3 ? emitExpr(*n.args[3]) : perlInt(0);
        return callRT("perl_socket_fh", {fh, d, t, p});
    }
    if (n.name == "bind") {
        Value *fh = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *a  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        return callRT("perl_bind_fh", {fh, a});
    }
    if (n.name == "listen") {
        Value *fh = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *b  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlInt(5);
        return callRT("perl_listen_fh", {fh, b});
    }
    if (n.name == "connect") {
        Value *fh = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *a  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        return callRT("perl_connect_fh", {fh, a});
    }
    if (n.name == "accept") {
        Value *nw = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *ls = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        return callRT("perl_accept_fh", {nw, ls});
    }
    if (n.name == "send") {
        Value *fh = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *m  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        Value *f  = n.args.size() > 2 ? emitExpr(*n.args[2]) : perlInt(0);
        return callRT("perl_send_fh", {fh, m, f});
    }
    if (n.name == "recv") {
        Value *fh = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *b  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        Value *l  = n.args.size() > 2 ? emitExpr(*n.args[2]) : perlInt(0);
        Value *f  = n.args.size() > 3 ? emitExpr(*n.args[3]) : perlInt(0);
        return callRT("perl_recv_fh", {fh, b, l, f});
    }
    if (n.name == "shutdown") {
        Value *fh = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *h  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlInt(2);
        return callRT("perl_shutdown_fh", {fh, h});
    }
    if (n.name == "getsockname") {
        Value *fh = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_getsockname_fh", {fh});
    }
    if (n.name == "getpeername") {
        Value *fh = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_getpeername_fh", {fh});
    }
    if (n.name == "sysopen") {
        Value *fh = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *p  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        Value *m  = n.args.size() > 2 ? emitExpr(*n.args[2]) : perlInt(0);
        Value *pe = n.args.size() > 3 ? emitExpr(*n.args[3]) : perlInt(0666);
        return callRT("perl_sysopen_fh", {fh, p, m, pe});
    }
    if (n.name == "sysread") {
        Value *fh = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *b  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        Value *l  = n.args.size() > 2 ? emitExpr(*n.args[2]) : perlInt(0);
        Value *o  = n.args.size() > 3 ? emitExpr(*n.args[3]) : perlInt(0);
        return callRT("perl_sysread_fh", {fh, b, l, o});
    }
    if (n.name == "syswrite") {
        Value *fh = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *b  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        Value *l  = n.args.size() > 2 ? emitExpr(*n.args[2]) : perlUndef();
        Value *o  = n.args.size() > 3 ? emitExpr(*n.args[3]) : perlInt(0);
        return callRT("perl_syswrite_fh", {fh, b, l, o});
    }
    if (n.name == "flock") {
        Value *fh = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *op = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlInt(2); /* LOCK_EX */
        return callRT("perl_flock_fh", {fh, op});
    }
    if (n.name == "vec") {
        Value *str = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *off = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlInt(0);
        Value *bits = n.args.size() > 2 ? emitExpr(*n.args[2]) : perlInt(1);
        return callRT("perl_vec_get", {str, off, bits});
    }
    if (n.name == "select") {
        if (n.args.size() >= 4) {
            Value *r = emitExpr(*n.args[0]);
            Value *w = emitExpr(*n.args[1]);
            Value *e = emitExpr(*n.args[2]);
            Value *t = emitExpr(*n.args[3]);
            return callRT("perl_select4", {r, w, e, t});
        }
        Value *fh = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_select_fh", {fh});
    }
    if (n.name == "fcntl") {
        Value *fh = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *c  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlInt(0);
        Value *a  = n.args.size() > 2 ? emitExpr(*n.args[2]) : perlInt(0);
        return callRT("perl_fcntl_fh", {fh, c, a});
    }
    if (n.name == "ioctl") {
        Value *fh = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *c  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlInt(0);
        Value *a  = n.args.size() > 2 ? emitExpr(*n.args[2]) : perlInt(0);
        return callRT("perl_ioctl_fh", {fh, c, a});
    }
    if (n.name == "dup" || n.name == "POSIX::dup") {
        Value *fd = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        return callRT("perl_dup_fd", {fd});
    }
    if (n.name == "dup2" || n.name == "POSIX::dup2") {
        Value *a = n.args.size() > 0 ? emitExpr(*n.args[0]) : perlUndef();
        Value *b = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        return callRT("perl_dup2_fd", {a, b});
    }
    if (n.name == "XS::load_library") {
        Value *lib = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        Value *ret = callRT("perl_xs_load_library", {lib});
        freeIfOwned(lib);
        return ret;
    }
    /* DynaLoader-compatible FFI (phase 2): the native surface real
       DynaLoader scripts and XSLoader use. All return through perl_*
       runtime functions; see the runtime.c section comment for the
       perlc calling/boot conventions. */
    if (n.name == "DynaLoader::dl_load_file" || n.name == "dl_load_file") {
        Value *path = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        Value *flg  = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        Value *ret = callRT("perl_dl_load_file", {path, flg});
        freeIfOwned(path);
        freeIfOwned(flg);
        return ret;
    }
    if (n.name == "DynaLoader::dl_find_symbol" || n.name == "dl_find_symbol") {
        Value *libref = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        Value *sym    = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        Value *ret = callRT("perl_dl_find_symbol", {libref, sym});
        freeIfOwned(libref);
        freeIfOwned(sym);
        return ret;
    }
    if (n.name == "DynaLoader::dl_install_xsub" || n.name == "dl_install_xsub") {
        Value *nm  = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        Value *ref = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        Value *ret = callRT("perl_dl_install_xsub", {nm, ref});
        freeIfOwned(nm);
        freeIfOwned(ref);
        return ret;
    }
    if (n.name == "DynaLoader::dl_error" || n.name == "dl_error") {
        return callRT("perl_dl_error", {});
    }
    if (n.name == "DynaLoader::bootstrap" || n.name == "bootstrap" ||
        n.name == "XSLoader::load") {
        Value *mod = n.args.empty() ? perlUndef() : emitExpr(*n.args[0]);
        Value *ver = n.args.size() > 1 ? emitExpr(*n.args[1]) : perlUndef();
        Value *ret = callRT("perl_dl_bootstrap", {mod, ver});
        freeIfOwned(mod);
        freeIfOwned(ver);
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
        fillCallArgs(argsArr, n);
        auto *i32Ty = Type::getInt32Ty(ctx_);
        Value *pkgStr  = builder_.CreateGlobalStringPtr(currentPackage_, "caller.pkg");
        Value *fileStr = builder_.CreateGlobalStringPtr(sourceFile_,     "caller.file");
        Value *lineVal = ConstantInt::get(i32Ty, n.line);
        callRT("perl_push_call_frame", {pkgStr, fileStr, lineVal});
        Value *ctxVal;
        if (callCtx_ == 1)
            ctxVal = ConstantInt::get(i32Ty, 1);
        else if (callCtx_ == 2)
            ctxVal = ConstantInt::get(i32Ty, 2);
        else if (callCtx_ == 0)
            ctxVal = ConstantInt::get(i32Ty, 0);
        else if (currentSubNeedsWantarray_)
            ctxVal = callRT("perl_current_wantarray_ctx", {});
        else
            ctxVal = ConstantInt::get(i32Ty, 0);
        callCtx_ = 0;
        Value *retVal = builder_.CreateCall(fn, {argsArr, ctxVal});
        callRT("perl_pop_call_frame", {});
        callRT("perl_array_free", {argsArr});
        return retVal;
    }
    if (n.name.find("::") != std::string::npos) {
        Value *argsArr = callRT("perl_array_new", {});
        fillCallArgs(argsArr, n);
        auto *i32Ty = Type::getInt32Ty(ctx_);
        Value *ctxVal;
        if (callCtx_ == 1)
            ctxVal = ConstantInt::get(i32Ty, 1);
        else if (callCtx_ == 2)
            ctxVal = ConstantInt::get(i32Ty, 2);
        else if (callCtx_ == 0)
            ctxVal = ConstantInt::get(i32Ty, 0);
        else if (currentSubNeedsWantarray_)
            ctxVal = callRT("perl_current_wantarray_ctx", {});
        else
            ctxVal = ConstantInt::get(i32Ty, 0);
        callCtx_ = 0;
        Value *nameStr = builder_.CreateGlobalStringPtr(n.name);
        Value *qualStr = builder_.CreateGlobalStringPtr(n.name);
        Value *fileStr = builder_.CreateGlobalStringPtr(sourceFile_);
        Value *lineVal = ConstantInt::get(i32Ty, n.line > 0 ? n.line : 1);
        Value *retVal = callRT("perl_call_named_sub_checked",
                                {nameStr, argsArr, ctxVal, qualStr, fileStr, lineVal});
        callRT("perl_array_free", {argsArr});
        return retVal;
    }
    /* D36 residual: bareword call `foo "arg"` to an unknown name is a
       compile error in real Perl ("String found where operator expected
       / Do you need to predeclare"). Parenthesized `foo()` is deferred to
       runtime, matching real Perl — see D113 below. */
    if (n.ival == 1) {
        throw std::runtime_error(
            "String found where operator expected (Do you need to predeclare \"" +
            n.name + "\"?) at " + sourceFile_ + " line " +
            std::to_string(n.line > 0 ? n.line : 1));
    }
    /* Parenthesized call to a name not known at compile time — e.g. a sub
       defined later via eval STRING, or a genuinely undefined sub (real
       Perl defers this check to runtime and dies there — D113: this used
       to silently return undef instead). Dispatch through the runtime
       table, which now dies with "Undefined subroutine ... called" if
       still unresolved by the time this call actually executes. */
    {
        Value *argsArr = callRT("perl_array_new", {});
        fillCallArgs(argsArr, n);
        auto *i32Ty = Type::getInt32Ty(ctx_);
        Value *ctxVal;
        if (callCtx_ == 1)
            ctxVal = ConstantInt::get(i32Ty, 1);
        else if (callCtx_ == 2)
            ctxVal = ConstantInt::get(i32Ty, 2);
        else if (callCtx_ == 0)
            ctxVal = ConstantInt::get(i32Ty, 0);
        else if (currentSubNeedsWantarray_)
            ctxVal = callRT("perl_current_wantarray_ctx", {});
        else
            ctxVal = ConstantInt::get(i32Ty, 0);
        callCtx_ = 0;
        Value *nameStr = builder_.CreateGlobalStringPtr(n.name);
        std::string qualName = (n.name.find("::") != std::string::npos)
                                    ? n.name
                                    : (currentPackage_ + "::" + n.name);
        Value *qualStr = builder_.CreateGlobalStringPtr(qualName);
        Value *fileStr = builder_.CreateGlobalStringPtr(sourceFile_);
        Value *lineVal = ConstantInt::get(i32Ty, n.line > 0 ? n.line : 1);
        Value *retVal = callRT("perl_call_named_sub_checked",
                                {nameStr, argsArr, ctxVal, qualStr, fileStr, lineVal});
        callRT("perl_array_free", {argsArr});
        return retVal;
    }
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
