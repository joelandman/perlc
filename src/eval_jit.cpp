#include "eval_jit.h"
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "jit.h"
#include "runtime.h"

#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/TargetSelect.h>

#include <atomic>
#include <mutex>
#include <memory>
#include <string>
#include <vector>
#include <setjmp.h>

using namespace llvm;

static std::unique_ptr<PerlJIT> g_jit;
static std::mutex               g_jit_mutex;
static std::atomic<int>         g_eval_counter{0};

static std::string eval_sub_llvm_name(const std::string &name) {
    std::string result = name;
    size_t pos;
    while ((pos = result.find("::")) != std::string::npos)
        result.replace(pos, 2, "__");
    return "perlsub_" + result;
}

static PerlValue *jit_eval_impl(const char *code) {
    /* ── parse ── */
    std::vector<Token> toks;
    try {
        std::string src(code);
        Lexer lex(std::move(src));
        toks = lex.tokenize();
    } catch (const std::exception &e) {
        PerlValue *err = perl_alloc_string(e.what());
        perl_assign(perl_get_dollar_at(), err);
        perl_free(err);
        return perl_alloc_undef();
    }

    NodePtr ast;
    std::vector<std::string> namedSubs;
    try {
        Parser parser(std::move(toks));
        ast = parser.parseProgram();
        for (auto &stmt : ast->args) {
            if (stmt->kind == NK::SubDef && stmt->name.find("::") != std::string::npos)
                namedSubs.push_back(stmt->name);
        }
    } catch (const std::exception &e) {
        PerlValue *err = perl_alloc_string(e.what());
        perl_assign(perl_get_dollar_at(), err);
        perl_free(err);
        return perl_alloc_undef();
    }

    /* ── codegen ── */
    int id = ++g_eval_counter;
    std::string funcName = "perl_eval_body_" + std::to_string(id);

    std::unique_ptr<Module> mod;
    std::unique_ptr<LLVMContext> ctx;
    try {
        CodeGen cg(false, 0);
        cg.compileForEval(*ast, funcName);
        mod = cg.releaseModule();
        ctx = cg.releaseContext();  /* keep context alive with the module */
    } catch (const std::exception &e) {
        PerlValue *err = perl_alloc_string(e.what());
        perl_assign(perl_get_dollar_at(), err);
        perl_free(err);
        return perl_alloc_undef();
    }

    if (!mod) {
        PerlValue *err = perl_alloc_string("eval: codegen produced no module");
        perl_assign(perl_get_dollar_at(), err);
        perl_free(err);
        return perl_alloc_undef();
    }

    /* ── JIT compile & lookup (mutex: LLJIT is not thread-safe for addModule) ── */
    using EvalFn = PerlValue *(*)();
    EvalFn fn = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_jit_mutex);
        g_jit->addModuleWithContext(std::move(mod), std::move(ctx));
        fn = (EvalFn)g_jit->getSymbolAddress(funcName);
        for (const auto &subName : namedSubs) {
            void *subAddr = g_jit->getSymbolAddress(eval_sub_llvm_name(subName));
            if (subAddr)
                perl_register_method(subName.c_str(), (PerlSubFnCtx)subAddr);
        }
    }

    if (!fn) {
        PerlValue *err = perl_alloc_string("eval: JIT symbol lookup failed");
        perl_assign(perl_get_dollar_at(), err);
        perl_free(err);
        return perl_alloc_undef();
    }

    /* ── execute; catch die via eval stack ── */
    jmp_buf jb;
    perl_eval_push(&jb);
    if (setjmp(jb) != 0) {
        /* die was called inside the eval body — $@ already set by perl_die */
        perl_eval_pop();
        return perl_alloc_undef();
    }

    PerlValue *result = fn();
    perl_eval_pop();
    return result ? result : perl_alloc_undef();
}

extern "C" void perl_eval_init(void) {
    /* initialise LLVM targets once */
    static bool llvm_inited = false;
    if (!llvm_inited) {
        InitializeNativeTarget();
        InitializeNativeTargetAsmPrinter();
        llvm_inited = true;
    }

    std::lock_guard<std::mutex> lk(g_jit_mutex);
    if (g_jit) return;  /* already initialised (e.g. from REPL) */

    g_jit = std::make_unique<PerlJIT>();
    if (g_jit->isReady()) {
        perl_eval_string_fn = jit_eval_impl;
    } else {
        g_jit.reset();
    }
}

/* Auto-initialise when this object is linked into a binary */
__attribute__((constructor))
static void auto_eval_init(void) {
    perl_eval_init();
}
