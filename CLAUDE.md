# perlc — Perl→LLVM Compiler: Project State

## Overview

A Perl compiler targeting LLVM IR, written in C++17 with LLVM 18. All Perl operations lower to calls into a C runtime (`src/runtime.c`).

**Current Status**: Core language features are ~95% implemented with 21/21 test programs passing. Significant coverage of Perl 5 semantics including OOP, closures, regex, modules, and advanced builtins.

## Build & Test

```bash
make              # builds ./perlc
make test         # runs all 21 test programs
make clean

./perlc foo.pl -o output            # compile and link
./perlc foo.pl --emit-ir -o out.ll  # dump LLVM IR for debugging
```

## Source Files

| File | Role |
|------|------|
| `src/lexer.h/cpp` | Context-aware tokenizer (`%` modulo vs hash sigil, `/` division vs regex) |
| `src/ast.h` | `NK` enum of node kinds + `Node` struct |
| `src/parser.h/cpp` | Recursive-descent parser → AST |
| `src/codegen.h/cpp` | AST → LLVM IR via IRBuilder |
| `src/runtime.h/c` | C runtime: `PerlValue` tagged union + all operations |
| `src/main.cpp` | Driver: lex→parse→codegen→clang-18 link with module inlining |

## Architecture

- **PerlValue**: `{ PerlTag tag; union { long long ival; double fval; char *sval; void *pval; }; long long matchpos; char *blessed_class; }`
- **PerlTag**: `UNDEF=0, INT=1, FLOAT=2, STRING=3, REF_SCALAR=4, REF_ARRAY=5, REF_HASH=6, FILEHANDLE=7, CODE_REF=8`
- **PerlArray**: `{ PerlValue **elems; long long len, cap; }`
- **PerlHash**: 64-bucket chained hash table
- **Assignment model**: `perl_assign` — each variable's alloca holds a *stable* `PerlValue*` for its lifetime (critical for references and closures)
- **Codegen pattern**: every operation calls into C runtime via `callRT("perl_xyz", {args...})`
- **Scope model**: three parallel scope stacks for scalars, arrays, and hashes
- **Module loading**: `use Module` recursively inlines `.pm` files at compile time via `inlineModules()`

## Major Implemented Features

### Core Language
- **Variables & Literals**: scalars, arrays, hashes, integers, floats, strings (single/double-quoted with interpolation), `undef`
- **Operators**: arithmetic, string (`.` , `x`), range (`..`), comparisons (`==`, `eq`, `<=>`, `cmp`), logical, increment/decrement (`++`/`--` including magical string increment), compound assignment, ternary
- **Control Flow**: `if`/`elsif`/`else`, `unless`, `while`/`until` (including `while (my $var = expr)`), `do-while`/`do-until`, C-style `for`, `foreach`, `last`/`next`, statement modifiers
- **Subroutines**: named and anonymous subs, recursion, `@_`, list unpacking, code references (`\&sub`, `$f->()`), `ref()` returning `"CODE"`
- **Builtins**: `print`/`say`/`printf`/`sprintf`, `chomp`/`chop`, `length`/`substr`, `join`/`split`/`sort`, `push`/`pop`/`shift`/`unshift`/`splice`, `keys`/`values`/`exists`/`delete`, `defined`, `ref`, `warn`, `die`, `abs`/`int`/`sqrt`, `uc`/`lc`/`ucfirst`/`lcfirst`, `index`/`rindex`, `chr`/`ord`/`hex`/`oct`, `reverse`, `map`/`grep`

### Advanced Features
- **References**: all types (`\$x`, `\@arr`, `\%hash`, `\&sub`), anonymous arrays/hashes, dereferencing (`$$ref`, `@$ref`, `%$ref`, `->`), `ref()`
- **Regex (PCRE2)**: `=~`/`!~`, captures (`$1`-`$9`), substitution (`s///`), `/g` iterator and list context, `split` with regex, flags `i/g/s/m`, named captures `(?<name>)` → `%+{name}`
- **String Interpolation**: `"$var"`, `"${var}"`, `"$arr[i]"`, `"$hash{key}"`, `"$@"`, `"$0"`, `"$1"`, `"@arr"` (space-joined)
- **Heredocs**: `<<END`, `<<'END'`, `<<"END"` with proper interpolation and lexer support
- **Special Variables**: `$_` (default for many builtins), `$!` (errno), `$/` (input separator with `local` support)
- **State Variables**: `state $x` — persistent per-sub variables with lazy initialization
- **File I/O**: `open` (2-arg and 3-arg forms), filehandles, readline (scalar and array context), `eof`, `unlink`, `print`/`say`/`printf` to filehandles
- **System**: `system()`, backticks (`` `cmd` ``), `$ENV{KEY}`, file tests (`-e`/`-f`/`-d`/`-r`/`-w`/`-x`/`-z`/`-s`/`-l`/`-p`)

### Object-Oriented Programming
- `package`, `bless`, `->` method calls (class and instance)
- `use parent`/`use base` with ISA chain traversal
- `SUPER::` dispatch
- Method registration and dynamic dispatch via `perl_dispatch_method`

### Advanced Perl Semantics
- **Closures**: lexical capture of `my` variables by stable pointer, nested closures, independent instances
- **Local**: dynamic scoping for scalars and special variables (`local $x`, `local $/`, block-scoped restore)
- **Exceptions**: `eval { BLOCK }` with `$@` support using `setjmp`/`longjmp`
- **BEGIN/END**: `BEGIN` runs inline, `END` registered via `atexit()`
- **Modules**: `use Module` with recursive inlining, `@EXPORT`/`@EXPORT_OK` support, constant subs via `use constant`
- **Array/Hash Slices**, `qw()`, fat comma (`=>`), list flattening in various contexts

## Passing Tests (21/21)

All tests in `tests/` pass:
- Core: `hello.pl`, `arith.pl`, `fib.pl`, `range.pl`, `modifiers.pl`
- Data structures: `hash.pl`, `refs.pl`, `builtins.pl`, `builtins2.pl`
- I/O & strings: `fileio.pl`, `sprintf.pl`
- Advanced: `regex.pl`, `regex_g.pl`, `advanced.pl`, `features.pl`
- OOP & modules: `oop.pl`, `closures.pl`, `usemod.pl`, `inherit.pl`
- Modern features: `defaults.pl`, `newfeatures.pl` (state, wantarray stub, caller stub, $!, $/, BEGIN/END, defined(), local blocks)

## Known Limitations

The following features are **not yet implemented** or only partially supported:

### Context and Call Stack
- Proper `wantarray` context tracking (currently always returns false/scalar context)
- Full `caller()` implementation (stub returns `("main", "unknown", 0)`)

### Scoping
- `local` for arrays and hashes (only scalars and special vars like `$!`/`$/` supported)

### Module System
- Runtime `require` and `do FILE` (modules only loaded at compile time via inlining)
- Pragmas that aren't backed by `.pm` files are silently ignored

### Regex
- Modifiers `x` (extended) and `e` (eval replacement)

### OOP
- `AUTOLOAD` and `DESTROY` methods

### Reference Operations
- `unshift @{EXPR}, val` (though `push @{EXPR}` works)

### Not Yet Implemented
- Threads (POSIX ithreads mentioned in architecture but no test coverage)
- XS interface
- DBI/SQLite integration
- Overload, prototypes, globs, signals, pack/unpack, unicode handling
- Many CPAN modules beyond basic `use`

## Key Implementation Details

- **Stable Pointer Model**: Variables hold stable `PerlValue*` pointers for correct reference and closure semantics
- **Runtime Heavy**: Most Perl semantics implemented in `runtime.c` (tagged union + extensive C functions)
- **Module Inlining**: `use` statements cause recursive parsing and token stream concatenation
- **Regex**: Uses PCRE2 with custom iterator state per `PerlValue` (`matchpos`)
- **Error Handling**: `die`/`eval` uses `jmp_buf` with careful stack management
- **Performance**: LLVM optimization + C runtime; no garbage collection (manual memory management via `perl_free`)

See `README.md` for user-facing documentation and individual test files for usage examples.

**Last Updated**: Current state reflects all features demonstrated in the 21 test suite.