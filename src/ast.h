#pragma once
#include <memory>
#include <string>
#include <vector>

/* ── node kinds ─────────────────────────────────────────────────────────── */
enum class NK {
    /* literals */
    IntLit, FloatLit, StringLit, UndefLit,
    /* variables */
    ScalarVar, ArrayVar, HashVar,
    /* array/hash element access */
    ArrayElem,   /* $arr[idx] */
    HashElem,    /* $hash{key} */
    /* expressions */
    BinOp, UnaryOp, Assign, CompoundAssign,
    Call,           /* func(args) */
    InterpolatedStr,/* "hello $name" */
    ArrayLit,       /* (1,2,3) in list context */
    /* statements */
    ExprStmt, Block, FlatBlock,  /* FlatBlock emits without pushing a scope */
    My, Our,
    If,             /* if/elsif/else */
    While,
    DoWhile,        /* do { body } while/until (cond) – body runs first */
    For,            /* C-style for */
    Foreach,
    Last, Next, Return,
    PrintStmt, SayStmt, PrintfStmt, SprintfFunc,
    /* file I/O */
    Readline,   /* <$fh>  – sval = var name ("" or "STDIN"/"STDERR") */
    OpenFunc,   /* open   – name=fhvar, sval="my"/"", args=[mode,file] or [modeFile] */
    CloseFunc,  /* close  – left = fh expr */
    EofFunc,    /* eof    – left = fh expr */
    DieStmt,    /* die    – left = msg expr */
    UnlinkFunc, /* unlink – args = filenames */
    SubDef,
    UseStmt,
    /* builtins that look like functions */
    PushStmt, PopExpr, ShiftExpr, UnshiftStmt,
    ScalarFunc, DefinedFunc,
    /* hash builtins */
    KeysFunc, ValuesFunc, ExistsFunc, DeleteFunc,
    SortFunc,
    /* string/array builtins */
    ChompFunc, LengthFunc, SubstrFunc, JoinFunc, SplitFunc,
    UnshiftStmt2,  /* unshift @arr, val,... */
    /* math builtins */
    AbsFunc, IntFunc, SqrtFunc,
    /* string case / search / conversion builtins */
    UcFunc, LcFunc, UcfirstFunc, LcfirstFunc,
    IndexFunc, RindexFunc,
    ChrFunc, OrdFunc, HexFunc, OctFunc,
    /* list builtins */
    ReverseFunc, MapFunc, GrepFunc,
    SpliceFunc,   /* splice(@arr, off[, len[, repl...]]) – name=arr, args=[off,len,repl...] */
    /* slices */
    ArraySlice,   /* @arr[0,1,2]    – name=arr,  args=indices */
    HashSlice,    /* @hash{'a','b'} – name=hash, args=keys */
    /* I/O / system */
    WarnStmt,     /* warn EXPR      – left=msg */
    SystemFunc,   /* system(cmd)    – left=cmd */
    BacktickExpr, /* `cmd`          – left=cmd-expr */
    FileTestOp,   /* -e/-f/etc.     – sval=flag-char, left=path */
    /* references */
    RefScalar,    /* \$x        – left = ScalarVar          */
    RefArray,     /* \@arr      – name = array name         */
    RefHash,      /* \%h        – name = hash name          */
    AnonArray,    /* [list]     – args = elements           */
    AnonHash,     /* {k=>v,...} – args = flat k,v list      */
    DerefScalar,  /* $$ref      – left = ref expr           */
    DerefArray,   /* @$ref      – left = ref expr           */
    DerefHash,    /* %$ref      – left = ref expr           */
    ArrowDeref,   /* $r->[i] or $r->{k} – left=base, right=subscript, sval="array"/"hash" */
    RefFunc,      /* ref($x)    – left = expr               */
    /* regex */
    Range,        /* lo..hi             – left=lo, right=hi                           */
    RegexMatch,   /* $s =~ /pat/flags  – left=str, sval=pat, name=flags, ival=1 if !~ */
    RegexSubst,   /* $s =~ s/p/r/flags – left=lval, sval=pat, name=repl\x01flags      */
    CaptureVar,   /* $1..$9            – ival=n                                        */
    /* code references */
    AnonSub,      /* sub { BLOCK }     – body=block, name=generated-fn-name           */
    CallCodeRef,  /* $f->(args)        – left=code-ref expr, args=arg list            */
    RefSub,       /* \&name or &name   – name=subname                                 */
    /* eval */
    EvalBlock,    /* eval { BLOCK }    – body=block                                   */
    DollarAt,     /* $@                – the eval error variable                      */
    /* tr/y */
    TrOp,         /* $s =~ tr/a/b/flags – left=str, sval=search\x01replace\x01flags  */
    /* OOP */
    PackageStmt,  /* package Foo;      – sval=package_name (parser sets currentPackage_) */
    BlessFunc,    /* bless $ref, $cls  – left=ref, right=class_expr                   */
    MethodCall,   /* $obj->method(args)– left=obj, sval=method, args=extra_args;
                     name=callerPkg when sval starts with "SUPER::"                  */
    SetIsa,       /* use parent 'Base' – name=child_pkg, sval=parent_pkg              */
    LocalStmt,    /* local $x [= expr] – name=var_name, left=init(opt)               */
    StateDecl,    /* state $x [= expr] – name=var_name, left=init(opt)               */
    WantarrayFunc,/* wantarray()       – no children                                  */
    CallerFunc,   /* caller()          – no children                                  */
    BeginBlock,   /* BEGIN { BLOCK }   – body=block                                   */
    EndBlock,     /* END   { BLOCK }   – body=block                                   */
};

struct Node;
using NodePtr  = std::unique_ptr<Node>;
using NodeList = std::vector<NodePtr>;

struct IfBranch {
    NodePtr cond;   /* nullptr = else branch */
    NodePtr body;
};

struct Node {
    NK   kind;
    int  line = 0;

    /* literal values */
    long long   ival = 0;
    double      fval = 0.0;
    std::string sval;

    /* variable / function name */
    std::string name;

    /* children */
    NodePtr            left, right, cond, body, init, step;
    NodeList           args;      /* call args, print args, array elements */
    std::vector<IfBranch> branches; /* if/elsif/else */

    /* sub definition */
    std::vector<std::string> params;
};

/* ── helpers ────────────────────────────────────────────────────────────── */
inline NodePtr makeInt(long long v, int line = 0) {
    auto n = std::make_unique<Node>(); n->kind = NK::IntLit; n->ival = v; n->line = line; return n;
}
inline NodePtr makeFloat(double v, int line = 0) {
    auto n = std::make_unique<Node>(); n->kind = NK::FloatLit; n->fval = v; n->line = line; return n;
}
inline NodePtr makeStr(std::string s, int line = 0) {
    auto n = std::make_unique<Node>(); n->kind = NK::StringLit; n->sval = std::move(s); n->line = line; return n;
}
inline NodePtr makeBin(std::string op, NodePtr l, NodePtr r, int line = 0) {
    auto n = std::make_unique<Node>(); n->kind = NK::BinOp; n->sval = std::move(op);
    n->left = std::move(l); n->right = std::move(r); n->line = line; return n;
}
inline NodePtr makeUnary(std::string op, NodePtr operand, int line = 0) {
    auto n = std::make_unique<Node>(); n->kind = NK::UnaryOp; n->sval = std::move(op);
    n->left = std::move(operand); n->line = line; return n;
}
inline NodePtr makeScalar(std::string name, int line = 0) {
    auto n = std::make_unique<Node>(); n->kind = NK::ScalarVar; n->name = std::move(name); n->line = line; return n;
}
inline NodePtr makeBlock(NodeList stmts, int line = 0) {
    auto n = std::make_unique<Node>(); n->kind = NK::Block; n->args = std::move(stmts); n->line = line; return n;
}
