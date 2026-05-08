# perlc — Perl→LLVM Compiler: Project State

## Overview

A Perl compiler targeting LLVM IR, written in C++17 with LLVM 18. All Perl operations lower to calls into a C runtime (`src/runtime.c`).

## Build & Test

```bash
make              # builds ./perlc
make test         # runs all 9 test programs
make clean

./perlc foo.pl -o output          # compile and link
./perlc foo.pl --emit-ir -o out.ll  # dump LLVM IR for debugging
```

## Source Files

| File | Role |
|------|------|
| `src/lexer.h/cpp` | Tokenizer; context-aware `%` (modulo vs hash sigil) and `/` (division vs regex) |
| `src/ast.h` | `NK` enum of node kinds + `Node` struct |
| `src/parser.h/cpp` | Recursive-descent parser → AST |
| `src/codegen.h/cpp` | AST → LLVM IR via IRBuilder |
| `src/runtime.h/c` | C runtime: `PerlValue` tagged union + all operations |
| `src/main.cpp` | Driver: lex→parse→codegen→clang-18 link |

## Architecture

- **PerlValue**: `{ PerlTag tag; union { long long ival; double fval; char *sval; void *pval; }; long long matchpos; }`
- **PerlTag**: `UNDEF=0, INT=1, FLOAT=2, STRING=3, REF_SCALAR=4, REF_ARRAY=5, REF_HASH=6`
- **PerlArray**: `{ PerlValue **elems; long long len, cap; }`
- **PerlHash**: 64-bucket chained hash table
- **matchpos**: per-PerlValue `/g` iterator offset; reset to 0 on `perl_clone`/`perl_assign`
- **Scope model**: three parallel scope stacks — `scopes_` (scalars), `arrayScopes_`, `hashScopes_`
- **Assignment model**: `perl_assign` — each variable's alloca holds a *stable* `PerlValue*` for its lifetime; assignment mutates in-place. Required for references to work correctly.
- **Codegen pattern**: every op is `callRT("perl_xyz", {args...})` → C runtime does the work

## Implemented Features

### All passing tests (9/9)

| Test | What it covers |
|------|----------------|
| `hello.pl` | strings, print/say, double-quoted interpolation |
| `arith.pl` | arithmetic, compound assign (`+=` etc.), string concat/repeat |
| `fib.pl` | recursive subs, `@_`, `my ($n) = @_` list assignment |
| `hash.pl` | `%hash`, `$h{k}`, keys/values/exists/delete, sort keys, hash args |
| `builtins.pl` | shift/unshift/chomp/length/substr/join/split (including regex split) |
| `refs.pl` | all reference types, anonymous array/hash, arrow subscript, `ref()` |
| `regex.pl` | match, `!~`, case-insensitive, captures `$1`–`$9`, substitution, split |
| `regex_g.pl` | `/g` iterator in `while`, captures in `/g`, list context `/g`, foreach `/g` |
| `modifiers.pl` | postfix if/unless/while/until/for/foreach, last/next with modifier |

### Language features

**Scalars**: integers, floats, strings, undef, all arithmetic/string/comparison operators, ternary `?:`, `++`/`--`, compound assignment

**Arrays**: `@arr`, push/pop/shift/unshift, `$arr[i]`, `scalar @arr`, join, split, sort

**Hashes**: `%hash`, `$h{key}`, keys/values/exists/delete, hash-from-list init

**Control flow**: if/elsif/else, unless, while, until, do-while, do-until, C-style for, foreach, last, next, return

**Statement modifiers**: `STMT if COND`, `STMT unless COND`, `STMT while COND`, `STMT until COND`, `STMT for LIST`, `STMT foreach LIST`

**Subroutines**: `sub name { }`, `@_`, list unpacking, recursion

**References**: `\$x`, `\@arr`, `\%h`, `[...]` (anon array), `{...}` (anon hash), `$$ref`, `@$ref`, `%$ref`, `$r->[i]`, `$r->{k}`, `ref($x)`

**Regex (PCRE2)**: `=~`, `!~`, flags (i/g/s/m), capture variables `$1`–`$9`, `s/pat/repl/flags`, `/g` iterator, `/g` list context, `split(/pat/, $str)`

**Builtins**: chomp, length, substr, join, split, push, pop, shift, unshift, sort, keys, values, exists, delete, scalar, defined, ref, print, say

**String interpolation**: `"$var"`, `"${var}"`, `"$arr[i]"`, `"$hash{key}"`

## Key Invariants

- `perl_assign` model: stable `PerlValue*` per alloca for the variable's lifetime. Captures via `\$x` work because the pointer never moves.
- In codegen: `pv` = opaque ptr = `PerlValue*`; `av` = opaque ptr = `PerlArray*` or `PerlHash*`. All opaque in LLVM 18.
- `emitArrayPtr` returns `PerlArray*` (av) for array-producing expressions; `emitExpr` returns `PerlValue*` (pv).
- `FatArrow =>` is treated identically to `,` everywhere.
- `foreach` loop uses a `stepBB` for the index increment so that `next` jumps through the increment before re-checking the condition (not directly to condBB, which would cause an infinite loop).
- `%` context: after INT/FLOAT/STRING/IDENT/RPAREN/RBRACKET/`++`/`--` → PERCENT (modulo); otherwise → hash sigil.
- `/` context: same heuristic → SLASH (division) vs REGEX literal.
- `parseModifier(stmt, line)`: called at end of every statement path; consumes the `;` itself. Modifier keywords (if/unless/while/until/for/foreach) are detected with `isModifier()` to prevent arg-list parsing from over-consuming.

## Passing Test Expected Outputs

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
(key iteration order may vary — tests use `sort keys`)

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

### refs.pl
```
42
99
1
3
20
10
30
40
1
3
10
20
ARRAY
HASH
SCALAR
2
42
```

### regex.pl
```
match
no xyz
icase
2024
03
15
baz bar foo
aaxxcc
[hello] [world]
one
two
three
digits only
```

### regex_g.pl
```
match
match
match
x
1
y
2
z
3
aa
bb
cc
hello
world
foo
3
one
two
three
```

### modifiers.pl
```
positive
ok
3
0
1
2
3
a
b
c
5
0
7
1
2
4
```

## Known Limitations / Not Implemented

- Range operator `..` (workaround: write out list elements explicitly)
- String `sprintf` / `printf`
- `wantarray`, `caller`, `local`
- Object-oriented features (`bless`, `->method()`)
- File I/O (`open`, `close`, `<FH>`)
- `use` statements (parsed but ignored, except `use strict`/`use warnings`)
- Regular expression modifiers `x` (extended) and `e` (eval replacement)
- Named captures `(?<name>...)`
