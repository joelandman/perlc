# perlc — Perl→LLVM Compiler: Project State

## Overview

A Perl compiler targeting LLVM IR, written in C++17 with LLVM 18. All Perl operations lower to calls into a C runtime (`src/runtime.c`). Currently implements a substantial subset of Perl including scalars, arrays, hashes, subroutines, control flow, string/array builtins, and (in-progress) references.

## Build & Test

```bash
make          # builds ./perlc
make test     # runs hello/arith/fib tests
# manual tests:
./perlc tests/hash.pl     -o /tmp/t && /tmp/t
./perlc tests/builtins.pl -o /tmp/t && /tmp/t
./perlc tests/refs.pl     -o /tmp/t && /tmp/t   # WIP — see bug below
./perlc foo.pl --emit-ir -o foo.ll   # dump LLVM IR for debugging
```

## Source Files

| File | Role |
|------|------|
| `src/lexer.h/cpp` | Tokenizer with context-aware `%` (modulo vs hash sigil) and `/` (slash vs regex) |
| `src/ast.h` | `NK` enum of node kinds + `Node` struct |
| `src/parser.h/cpp` | Recursive-descent parser → AST |
| `src/codegen.h/cpp` | AST → LLVM IR via IRBuilder |
| `src/runtime.h/c` | C runtime: `PerlValue` tagged union + all operations |
| `src/main.cpp` | Driver: lex→parse→codegen→clang-18 link |

## Architecture

- **PerlValue**: `{ PerlTag tag; union { long long ival; double fval; char *sval; void *pval; } }`
- **PerlTag**: `UNDEF=0, INT=1, FLOAT=2, STRING=3, REF_SCALAR=4, REF_ARRAY=5, REF_HASH=6`
- **PerlArray**: `{ PerlValue **elems; long long len, cap; }`
- **PerlHash**: 64-bucket chained hash table
- **Scope model**: three parallel scope stacks — `scopes_` (scalars), `arrayScopes_`, `hashScopes_`
- **Assignment model**: `perl_assign` — each variable's alloca holds a *stable* `PerlValue*` for its lifetime; assignment mutates in-place. This is required for references to work correctly.
- **Codegen pattern**: every op is `callRT("perl_xyz", {args...})` → C runtime does the work

## Implemented Features (all passing tests)

- `hello.pl` — strings, print/say, interpolation
- `arith.pl` — arithmetic, compound assign, string ops
- `fib.pl` — recursive subs, `@_`, `my ($n) = @_`
- `hash.pl` — `%hash`, `$h{k}`, keys/values/exists/delete, sort keys, hash args to subs
- `builtins.pl` — shift/unshift/chomp/length/substr/join/split (incl. regex literal split)

## References (`tests/refs.pl`) — Complete

All reference plumbing is implemented and passing.

### What is done

**runtime.h/c** — complete:
- `PERL_REF_SCALAR=4`, `PERL_REF_ARRAY=5`, `PERL_REF_HASH=6` added to `PerlTag`
- `void *pval` added to `PerlValue` union
- `perl_ref_scalar/array/hash`, `perl_deref_scalar/array/hash`, `perl_ref_type` implemented
- `perl_to_string` updated (prints `ARRAY(0x...)` etc.)
- `perl_is_true` updated (refs are truthy)

**lexer** — complete:
- `KW_REF` added; `"ref"` maps to it

**ast.h** — complete:
- `RefScalar, RefArray, RefHash` — `\$x`, `\@arr`, `\%h`
- `AnonArray, AnonHash` — `[list]`, `{k=>v,...}`
- `DerefScalar, DerefArray, DerefHash` — `$$ref`, `@$ref`, `%$ref`
- `ArrowDeref` — `$ref->[i]` or `$ref->{k}` (sval="array"/"hash")
- `RefFunc` — `ref($x)`

**parser.cpp** — complete except for the bug below:
- `parsePrimary`: handles `\`, `[...]` (AnonArray), `{...}` (AnonHash), `$$ref`, `@$ref`, `%$ref`, `KW_REF`
- `parseSubscript(NodePtr base, int line)`: chains `->[]` and `->{}`
- `parsePostfix`: calls `parseSubscript` when `->` is seen — **BUG HERE**
- `parsePush`/`parseUnshift`: handle `@$ref` form (n.left = ref expr when name is empty)

**codegen.cpp** — complete:
- All new NK nodes handled in `emitExpr`
- `emitArrayPtr` extended for `DerefArray`, `AnonArray`
- `NK::My` scalar path uses `perl_assign` model (stable PerlValue*)
- `NK::Assign` handles `DerefScalar`, `ArrowDeref`, `ArrayElem` lvalues
- `NK::CompoundAssign` uses `perl_assign`
- `NK::Foreach` loop variable uses `perl_assign` (stable `loopPv`)
- `NK::PushStmt`/`NK::UnshiftStmt2` handle `n.left` = ref expr

### Bug Fix Applied

**Root cause**: Use-after-move UB in `parsePostfix()` — `std::move(expr)` and `expr->line` were evaluated in the same function call with unspecified argument evaluation order.

**Fix** (`src/parser.cpp:667`):
```cpp
if (check(TK::ARROW)) {
    int ln = expr->line;
    expr = parseSubscript(std::move(expr), ln);
}
```

### Test file (`tests/refs.pl`) — expected output

```
42       # $$ref
99       # $x after $$ref = 99
1        # $aref->[0]
3        # $aref->[2]
20       # $arr[1] after $aref->[1]=20
10       # $anon->[0]
30       # $anon->[2]
40       # $anon->[3] after push @$anon,40
1        # $href->{a}
3        # $h{c} after $href->{c}=3
10       # $ah->{x}
20       # $ah->{y}
ARRAY    # ref($aref)
HASH     # ref($href)
SCALAR   # ref(\$x)
2        # $nested->{a}->[1]
42       # $nested->{b}->{c}
```

## Key Decisions & Invariants

- `perl_assign` model: every `my $x` alloca holds a single `PerlValue*` for the variable's lifetime. Assignment calls `perl_assign(load(alloca), new_val)` to mutate in-place. This is what makes `\$x` work — the captured pointer is always current.
- In codegen, `pv` = `PointerType::getUnqual(ctx_)` = opaque ptr used for `PerlValue*`; `av` = same type used for `PerlArray*`/`PerlHash*`. All are opaque in LLVM 18.
- `emitArrayPtr` returns `PerlArray*` (av) for array-producing expressions; `emitExpr` returns `PerlValue*` (pv).
- FatArrow `=>` is treated identically to `,` everywhere in list/call contexts.
- `%` context: after INT/FLOAT/STRING/IDENT/RPAREN/RBRACKET/++/-- → PERCENT; else → HASH sigil.
- `/` context: same heuristic → SLASH vs REGEX literal.

## Passing Test Expected Outputs (for regression checking)

### hello.pl
```
Hello, world!
Hello again!
```

### arith.pl
```
13
7
30
3.33333
1
6
Hello World
10
```

### fib.pl
```
0 1 1 2 3 5 8 13 21 34
```

### hash.pl
```
1
2
3
4
10
green exists
purple missing
green deleted
blue
red
yellow
3
12
```
(exact key order in hash iterating may vary for `keys`/`values`)

### builtins.pl
```
1
4
10
20
6
hello
5
6
2
cdef
bcd
ef
bc
one, two, three
a-b-c
onetwothree
4
a
d
3
hello
foo
3
local
one:two:three
foo
bar
baz
```
