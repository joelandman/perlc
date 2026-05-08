# perlc — Perl→LLVM Compiler: Project State

## Overview

A Perl compiler targeting LLVM IR, written in C++17 with LLVM 18. All Perl operations lower to calls into a C runtime (`src/runtime.c`).

## Build & Test

```bash
make              # builds ./perlc
make test         # runs all 14 test programs
make clean

./perlc foo.pl -o output            # compile and link
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
- **PerlTag**: `UNDEF=0, INT=1, FLOAT=2, STRING=3, REF_SCALAR=4, REF_ARRAY=5, REF_HASH=6, FILEHANDLE=7`
- **PerlArray**: `{ PerlValue **elems; long long len, cap; }`
- **PerlHash**: 64-bucket chained hash table
- **matchpos**: per-PerlValue `/g` iterator offset; reset to 0 on `perl_clone`/`perl_assign`
- **Scope model**: three parallel scope stacks — `scopes_` (scalars), `arrayScopes_`, `hashScopes_`
- **Assignment model**: `perl_assign` — each variable's alloca holds a *stable* `PerlValue*` for its lifetime; assignment mutates in-place. Required for references to work correctly.
- **Codegen pattern**: every op is `callRT("perl_xyz", {args...})` → C runtime does the work
- **File handles**: `PERL_FILEHANDLE` tag; `FILE*` stored in `pval` field; stable PerlValue* for the fh lifetime; `perl_open_fh` closes the old FILE* before opening a new one on the same PerlValue*

## Implemented Features

### All passing tests (14/14)

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
| `range.pl` | `..` range operator in for, array assignment, join, scalar context |
| `sprintf.pl` | `sprintf`/`printf` with `%s %d %f %x %o %b %e %g %c %%`, width/precision |
| `fileio.pl` | open/close (2-arg/3-arg), readline scalar+array, print/say/printf to fh, eof, die, unlink |
| `builtins2.pl` | abs/int/sqrt, uc/lc/ucfirst/lcfirst, index/rindex, chr/ord/hex/oct, reverse, map, grep, sort comparators, <=>, cmp |
| `features.pl` | chop, warn, qw(), splice, array/hash slices, \$ENV{}, file tests (-e/-f/-d/-r/-w/-x/-z/-s/-l), system, backtick |

### Language features

**Scalars**: integers, floats, strings, undef, all arithmetic/string/comparison operators, ternary `?:`, `++`/`--`, compound assignment

**Arrays**: `@arr`, push/pop/shift/unshift, `$arr[i]`, `scalar @arr`, join, split, sort, `chomp @arr`

**Hashes**: `%hash`, `$h{key}`, keys/values/exists/delete, hash-from-list init

**Control flow**: if/elsif/else, unless, while (including `while (my $var = expr)`), until, do-while, do-until, C-style for, foreach, last, next, return

**Statement modifiers**: `STMT if COND`, `STMT unless COND`, `STMT while COND`, `STMT until COND`, `STMT for LIST`, `STMT foreach LIST`

**Subroutines**: `sub name { }`, `@_`, list unpacking, recursion

**References**: `\$x`, `\@arr`, `\%h`, `[...]` (anon array), `{...}` (anon hash), `$$ref`, `@$ref`, `%$ref`, `$r->[i]`, `$r->{k}`, `ref($x)`

**Regex (PCRE2)**: `=~`, `!~`, flags (i/g/s/m), capture variables `$1`–`$9`, `s/pat/repl/flags`, `/g` iterator, `/g` list context, `split(/pat/, $str)`

**Range**: `1..N` in for/foreach, list context (`my @r = (1..10)`), join context

**sprintf/printf**: full format string — `%s %d %i %u %f %e %E %g %G %x %X %o %b %c %%`, width/precision literals and `*` from args

**File I/O**: `open(my $fh, mode, file)`, `open(my $fh, "modeFile")` (2-arg), `close($fh)`, `<$fh>` (scalar readline), `my @lines = <$fh>` (array readline), `print $fh`, `say $fh`, `printf $fh`, `eof($fh)`, `die`, `print STDERR`, `unlink`

**Operators**: `<=>` (spaceship numeric), `cmp` (spaceship string)

**List ops**: `map { BLOCK } LIST`, `grep { BLOCK } LIST`, `sort { CMP } LIST` (with `$a`/`$b` comparator patterns), `reverse @arr` (array), `scalar reverse $str` (string)

**Math builtins**: `abs`, `int` (truncate), `sqrt`

**String builtins**: `uc`, `lc`, `ucfirst`, `lcfirst`, `index($str, $sub[, $pos])`, `rindex($str, $sub[, $pos])`, `chr`, `ord`, `hex`, `oct`

**Slices**: `@arr[0,1,2]` (array slice), `@hash{'a','b'}` (hash slice); qw() and list args auto-flattened

**System/env**: `system("cmd")` (exit code), `` `cmd` `` (output capture, interpolated), `$ENV{KEY}` / `$ENV{KEY}=val`

**File tests**: `-e` (exists), `-f` (file), `-d` (dir), `-r` (readable), `-w` (writable), `-x` (executable), `-z` (empty), `-s` (size), `-l` (symlink), `-p` (pipe)

**Builtins**: chomp, chop, length, substr, join, split, push, pop, shift, unshift, splice, sort, keys, values, exists, delete, scalar, defined, ref, warn, print, say, printf, sprintf, unlink

**String interpolation**: `"$var"`, `"${var}"`, `"$arr[i]"`, `"$hash{key}"`

## Key Invariants

- `perl_assign` model: stable `PerlValue*` per alloca for the variable's lifetime. Captures via `\$x` work because the pointer never moves.
- In codegen: `pv` = opaque ptr = `PerlValue*`; `av` = opaque ptr = `PerlArray*` or `PerlHash*`. All opaque in LLVM 18.
- `emitArrayPtr` returns `PerlArray*` (av) for array-producing expressions; `emitExpr` returns `PerlValue*` (pv).
- `FatArrow =>` is treated identically to `,` everywhere.
- `foreach` loop uses a `stepBB` for the index increment so that `next` jumps through the increment before re-checking the condition (not directly to condBB, which would cause an infinite loop).
- `while (my $var = expr)` condition: the variable alloca and initial `PerlValue*` are allocated in the pre-loop block (once); each `while.cond` iteration only calls `perl_assign` + truth test — no stack growth or leaking.
- `%` context: after INT/FLOAT/STRING/IDENT/RPAREN/RBRACKET/`++`/`--` → PERCENT (modulo); otherwise → hash sigil.
- `/` context: same heuristic → SLASH (division) vs REGEX literal.
- `parseModifier(stmt, line)`: called at end of every statement path; consumes the `;` itself. Modifier keywords (if/unless/while/until/for/foreach) are detected with `isModifier()` to prevent arg-list parsing from over-consuming.
- Filehandle detection in print/say/printf: `$var` is treated as a filehandle only when followed by an expression-start token (scalar, string, int, float, ident, `(`, `[`, `{`) — not an operator. Prevents `say $a + $b` from treating `$a` as a filehandle.
- `die` in expression context (e.g. `open(...) or die "..."`) uses the dead-block pattern: after `CreateUnreachable()`, insert point moves to a fresh dead basic block so surrounding phi nodes remain well-formed.
- `map`/`grep` blocks compile as inline LLVM loops; `$_` alloca is hoisted before the loop; `emitBlockLast` returns the last expression from the block.
- `sort { CMP }` block: parser detects 4 patterns (`$a <=> $b`, `$b <=> $a`, `$a cmp $b`, `$b cmp $a`) and stores the mode in `Node::sval`; codegen dispatches to specialized C sort functions.
- `reverse` in scalar context (`scalar reverse $str`) calls `perl_reverse_str`; in array context calls `perl_reverse_array`.
- `scalar EXPR` (beyond `@arr`/`keys`/`values`): parser now falls through to `parsePrimary()` and sets `sval = "scalar_ctx"` on the inner node.
- `index`/`rindex` pass `perlUndef()` for missing pos arg; runtime checks `pos_pv->tag != PERL_UNDEF` to distinguish "no pos given" from "pos=0".
- File test `-e $var ? x : y` parses at postfix precedence (path is parsePrimary/parsePostfix, not full parseExpr), so the ternary binds outside the file test.
- `qw(a b c)` → `ArrayLit` of `StringLit`; hash slice `@h{qw(a c)}` gets a single `ArrayLit` arg which codegen auto-flattens.
- `splice` in array context (emitArrayPtr) returns removed elements as `PerlArray*`; scalar context returns element count.
- `$ENV{key}` and `$ENV{key}=val` are special-cased in HashElem/Assign codegen (name=="ENV") to call `perl_env_get`/`perl_env_set` rather than normal hash lookup.

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

### range.pl
```
1
2
3
4
5
10
55
1-2-3-4
4
```

### sprintf.pl
```
Hello, Alice! You are 30 years old.
Pi is approximately 3.1416
1.234568e+04
0.000123
[     right]
[left      ]
[00042]
(foo, 99)
ff
FF
10
item 01: value
item 02: value
item 03: value
      42
3.142
100%
Result: 3 + 4 = 7
```

### fileio.pl
```
line one
line two
line three
3
line one
line three
4
eof
line one
answer=42
done
```

### builtins2.pl
```
5
3.7
3
-3
4.0
HELLO
world
Foo
bAR
6
-1
4
9
3
A
65
97
255
255
63
10
5,4,3,2,1
olleh
2,4,6,8,10
n1
n5
2,4
4,5
1,2,3,4,5
5,4,3,2,1
apple,banana,cherry
cherry,banana,apple
-1
0
1
-1
0
1
done
```

## Known Limitations / Not Implemented

- `wantarray`, `caller`, `local`
- Object-oriented features (`bless`, `->method()`)
- `use` statements (parsed but ignored, except `use strict`/`use warnings`)
- Regular expression modifiers `x` (extended) and `e` (eval replacement)
- Named captures `(?<name>...)`
