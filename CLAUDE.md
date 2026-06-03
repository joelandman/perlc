# perlc — Perl→LLVM Compiler: Project State

## Overview

A Perl compiler targeting LLVM IR, written in C++17 with LLVM 18. All Perl operations lower to calls into a C runtime (`src/runtime.c`).

**Current Status**: Core language features are ~99% implemented with 36/36 test programs passing. Significant coverage of Perl 5 semantics including OOP, closures, regex, modules, advanced builtins, List::Util, POSIX, Scalar::Util, Tier 2 and Tier 3 builtins, threads with threads::shared, wantarray context propagation (including through call chains), require, DESTROY (hash and array objects), XS interface, DBI/SQLite integration, `caller()`, AUTOLOAD, `local @arr`/`local %hash`, `(LIST)[i]` subscript, `/e` regex modifier, `$Package::var` cross-package access, lvalue array/hash slices, autovivification, and labeled `next`/`last`.

## Build & Test

```bash
make              # builds ./perlc
make test         # runs all 36 test programs
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
- **PerlTag**: `UNDEF=0, INT=1, FLOAT=2, STRING=3, REF_SCALAR=4, REF_ARRAY=5, REF_HASH=6, FILEHANDLE=7, CODE_REF=8, FLAT_ARRAY=10`
- **PerlArray**: `{ PerlValue **elems; long long len, cap; int refcount; }`
- **PerlHash**: 64-bucket chained hash table
- **Assignment model**: `perl_assign` — each variable's alloca holds a *stable* `PerlValue*` for its lifetime (critical for references and closures)
- **Codegen pattern**: every operation calls into C runtime via `callRT("perl_xyz", {args...})`
- **Scope model**: parallel scope stacks for scalars, arrays, hashes, float vars, int vars, and DerefAV-cached array-ref params
- **FLAT_ARRAY**: all-numeric AnonArray literals with ≥4 elements compile to `double[]` inline (tag=10, pval=double*, matchpos=count), eliminating PV boxing in hot loops; `perl_assign` deep-copies the double[] for correct ownership semantics
- **Module loading**: `use Module` recursively inlines `.pm` files at compile time via `inlineModules()`

## Major Implemented Features

### Core Language
- **Variables & Literals**: scalars, arrays, hashes, integers, floats, strings (single/double-quoted with interpolation), `undef`
- **Operators**: arithmetic, string (`.`, `x` repetition), range (`..`), comparisons (`==`, `eq`, `<=>`, `cmp`), logical (`&&`, `||`, `!`, `//`), low-precedence (`and`, `or`, `not`), bitwise (`&`, `|`, `^`, `~`, `<<`, `>>`), increment/decrement (`++`/`--` including magical string increment), compound assignment (`+=`, `-=`, `*=`, `/=`, `.=`, `%=`, `**=`, `||=`, `&&=`, `//=`, `&=`, `|=`, `^=`, `<<=`, `>>=`, `x=`), ternary
- **Control Flow**: `if`/`elsif`/`else`, `unless`, `while`/`until` (including `while (my $var = expr)`), `do-while`/`do-until`, C-style `for`, `foreach`, `last`/`next`/`redo` with optional labels (`LABEL: for ... { next LABEL }`), statement modifiers
- **Subroutines**: named and anonymous subs, recursion, `@_`, list unpacking, code references (`\&sub`, `$f->()`), `ref()` returning `"CODE"`
- **Builtins**: `print`/`say`/`printf`/`sprintf`, `chomp`/`chop`, `length`/`substr`, `join`/`split`/`sort` (including `sort { BLOCK }`), `push`/`pop`/`shift`/`unshift`/`splice`, `keys`/`values`/`exists`/`delete` (all accepting `%{$ref}` / `%$ref` deref forms; `exists`/`delete` support both `$h{k}` and `$arr[N]`), `defined`, `ref`, `warn`, `die`, `abs`/`int`/`sqrt`, `uc`/`lc`/`ucfirst`/`lcfirst`, `index`/`rindex`, `chr`/`ord`/`hex`/`oct`, `reverse`, `map`/`grep`; `print @arr` prints all elements
- **Time**: `time`, `localtime`, `gmtime` (list context → 9-element list: sec,min,hour,mday,mon,year,wday,yday,isdst)
- **Randomness**: `rand [MAX]`, `srand [SEED]`
- **Process**: `sleep SECS`, `alarm SECS`
- **List::Util** (built-in, no CPAN): `sum`, `min`, `max`, `first { BLOCK } LIST`, `any { BLOCK } LIST`, `all { BLOCK } LIST`, `none { BLOCK } LIST`, `uniq LIST`, `reduce { BLOCK } LIST`

### Extended Features
- **XS Interface**: Dynamic loading of C libraries via `perl_xs_load()` function and support for calling C functions from Perl
- **DBI/SQLite Integration**: Database connectivity framework with standard DBI functions including connection, prepared statements, and query execution

### Advanced Features
- **References**: all types (`\$x`, `\@arr`, `\%hash`, `\&sub`), anonymous arrays/hashes, dereferencing (`$$ref`, `@$ref`, `%$ref`, `->`), `ref()`
- **Regex (PCRE2)**: `=~`/`!~`, captures (`$1`-`$9`), substitution (`s///`), `/g` iterator and list context, `split` with regex, flags `i/g/s/m/e` (`/e` evaluates replacement as Perl code), named captures `(?<name>)` → `$+{name}` / `keys %+`
- **String Interpolation**: `"$var"`, `"${var}"`, `"$arr[i]"`, `"$hash{key}"`, `"$Pkg::var"`, `"@Pkg::arr"`, `"$@"`, `"$0"`, `"$1"`, `"@arr"` (space-joined), `"$hash{$var}"` (variable key), `"$arr[$i]"` (variable index)
- **Heredocs**: `<<END`, `<<'END'`, `<<"END"` with proper interpolation and lexer support
- **Special Variables**: `$_` (default for many builtins), `$!` (errno), `$/` (input separator with `local` support)
- **State Variables**: `state $x` — persistent per-sub variables with lazy initialization
- **File I/O**: `open` (2-arg and 3-arg forms), filehandles, readline (scalar and array context), `eof`, `unlink`, `print`/`say`/`printf` to filehandles; `seek`/`tell`/`binmode`
- **Filesystem**: `stat`/`lstat` (13-element list), `glob`
- **System**: `system()`, backticks (`` `cmd` ``), `$ENV{KEY}`, file tests (`-e`/`-f`/`-d`/`-r`/`-w`/`-x`/`-z`/`-s`/`-l`/`-p`)
- **Special Variables**: `$.` (line number), `$,` (output field sep), `$\` (output record sep), `$&` (last match), `$/` (input sep), `$!` (errno); all support `local`
- **POSIX module** (built-in): `floor`, `ceil`, `fmod`, `strftime`
- **Scalar::Util** (built-in): `blessed`, `reftype`, `looks_like_number`
- **Carp** (built-in): `croak`/`carp`/`confess`/`cluck`
- **UNIVERSAL**: `->isa(class)`, `->can(method)`, `our @ISA = (...)` inheritance
- **Tier 3 builtins**: `read($fh,$buf,$n)`, `fileno($fh)`, `truncate($fh,$len)`, `each %hash`, `pos($str)`, `getpid()` / `$$`, `$^O` (OS name)
- **Threads**: `use threads`; `threads->create(sub{...}, @args)`, `$thr->join()`, `$thr->detach()`, `$thr->tid()`, `threads->self()`, `threads->list()`, `threads->yield()`; thread-local freelist/eval-stack/captures via `__thread`; `PERL_THREAD` tag (11); closure capture deep-copied per thread for isolation (non-shared vars are independent copies)
- **threads::shared**: `use threads::shared`; `my $x : shared` / `my @arr : shared` / `my %hash : shared`; `lock($x)` / `lock(@arr)` / `lock(%hash)` with auto-unlock at block exit; `cond_wait($x)` / `cond_signal($x)` / `cond_broadcast($x)`; `PerlSharedVar` struct wraps PerlValue with embedded pthread_mutex+cond; shared vars bypass thread isolation (original pointer shared across threads)

### Object-Oriented Programming
- `package`, `bless`, `->` method calls (class and instance), chained `->m1->m2->m3`
- `use parent`/`use base` (including `-norequire`) with ISA chain traversal
- `SUPER::` dispatch
- `AUTOLOAD` — catches unknown method calls; `$AUTOLOAD` set to `"Package::method"`
- `DESTROY` — fires on scope exit, undef/overwrite, for both hash-backed and array-backed blessed objects
- `caller()` — returns `(package, filename, line)` at any call depth; `caller(N)` for outer frames
- `$Package::var`, `@Package::arr`, `%Package::hash` — cross-package variable access
- Method registration and dynamic dispatch via `perl_dispatch_method`

### Advanced Perl Semantics
- **Closures**: lexical capture of `my` variables by stable pointer, nested closures, independent instances
- **Local**: dynamic scoping for scalars, arrays, and hashes (`local $x`, `local @arr`, `local %hash`, `local $/`, `local @ARGV`; block-scoped restore)
- **Array/hash assignment**: `@arr = @other`, `@arr = ()` (clear), `%h = (list)` (replaces all entries), `(LIST)[i]` subscript on sort/map/grep/caller results; lvalue slices `@arr[i,j] = list` and `@h{qw(a b)} = list`; autovivification `$h{a}{b} = val`, `$a[i]{k} = val`, `push @{$h{k}}, val`
- **Exceptions**: `eval { BLOCK }` with `$@` support using `setjmp`/`longjmp`
- **BEGIN/END**: `BEGIN` runs inline, `END` registered via `atexit()`
- **Modules**: `use Module` with recursive inlining, `@EXPORT`/`@EXPORT_OK` support, constant subs via `use constant`. The new `-pm` flag automatically detects missing modules (excluding pragmas), installs them via `cpanm --local-lib lib` into `lib/lib/perl5/`, and updates search paths.
  - **Limitation**: Complex CPAN modules (with advanced OO, `our` vars, POD, etc.) may trigger parser errors. Simple modules and our custom test modules work well.
- **Array/Hash Slices**, `qw()`, fat comma (`=>`), list flattening in various contexts

## Passing Tests (36/36)

All tests in `tests/` pass:
- Core: `hello.pl`, `arith.pl`, `fib.pl`, `range.pl`, `modifiers.pl`
- Data structures: `hash.pl`, `refs.pl`, `builtins.pl`, `builtins2.pl`
- I/O & strings: `fileio.pl`, `fileops.pl`, `sprintf.pl`
- Advanced: `regex.pl`, `regex_g.pl`, `regex_named.pl`, `advanced.pl`, `features.pl`
- OOP & modules: `oop.pl`, `closures.pl`, `usemod.pl`, `inherit.pl`
- Modern features: `defaults.pl`, `newfeatures.pl`, `interp.pl` (string interpolation), `misc.pl`, `tr.pl`, `wantarray.pl`
- Performance benchmarks: `fibn.pl` (Fibonacci), `mbs.pl` (Mandelbrot set 512×512×80 iters)
- Tier 1 builtins: `tier1.pl` (rand/srand, time/localtime/gmtime, sleep/alarm, sort { BLOCK }, List::Util)
- Tier 2 builtins: `tier2.pl` ($/.$,/$\/$&, POSIX::floor/ceil/fmod/strftime, Scalar::Util::blessed/reftype/looks_like_number, seek/tell/binmode, stat/lstat, glob, isa/can, our @ISA)
- Tier 3 builtins: `tier3.pl` ($$/$^O, fileno, read, truncate, each %hash, pos, getpid)
- Threads: `threads.pl` (create/join/tid/self, closure capture, thread isolation, threads::shared scalars/arrays/hashes, lock/cond_wait/cond_signal/cond_broadcast)
- Object lifecycle: `destroy.pl` (DESTROY on scope exit, undef assignment, overwrite, loop, data access in destructor)
- String eval (JIT): `eval_string.pl`
- Completeness: `completeness.pl` (caller(), local @arr/local %hash, AUTOLOAD, pos() write, runtime require)

## Known Limitations

The following features are **not yet implemented** or only partially supported:

### Context and Call Stack
- `wantarray` context propagation: fully implemented — list vs. scalar context at call sites, `wantarray` builtin, and propagation through call chains (`sub outer { inner() }` inherits the caller's context)

### Module System
- `require Module::Name` and `require "file.pm"` are implemented (compile-time inlining, same as `use`); runtime `require` and `do FILE` are not yet supported
- Pragmas that aren't backed by `.pm` files are silently ignored
- `tie` / `untie` not implemented

### Regex
- Modifier `x` (extended/whitespace-ignoring patterns) not supported

### Command-line / Debugging
- `-g` flag supported: adds debugging symbols + **Perl source line mapping** via LLVM debug metadata (visible in gdb/lldb)

### Not Yet Implemented
- Overload, prototypes, typeglobs, signals, `pack`/`unpack`, unicode handling
- Many complex CPAN modules (parser may fail on advanced OO/`our`/POD; simplify scripts as needed)
- `exists $h{a}{b}` chained hash subscript without arrow (use `$h{a}->{b}` instead)
- `or`/`and`/`not` precedence relative to `my` declaration initializer: `my $x = 0 or 1` gives `$x=1` (not `$x=0` as in real Perl); use explicit parens
- `do FILE` runtime file execution (only compile-time `require` works)

## Key Implementation Details

- **Stable Pointer Model**: Variables hold stable `PerlValue*` pointers for correct reference and closure semantics
- **Runtime Heavy**: Most Perl semantics implemented in `runtime.c` (tagged union + extensive C functions)
- **Module Inlining**: `use` statements cause recursive parsing and token stream concatenation
- **Regex**: Uses PCRE2 with custom iterator state per `PerlValue` (`matchpos`)
- **Error Handling**: `die`/`eval` uses `jmp_buf` with careful stack management
- **Performance**: LLVM optimization (O2 + LTO) + C runtime with freelist pool allocator; no GC (manual via `perl_free`). Extensive unboxing optimizations: float scalar vars (`floatScopes_`), unboxed arithmetic (`canEmitF64`/`emitExprF64`), FLAT_ARRAY for numeric arrays, DerefAV cache for array-ref @_ params, borrow reads for array/hash elements, TBAA metadata for alias disambiguation. nb.pl n=5M runs in 0.22s vs Perl's ~33s (~150× faster).

See `README.md` for user-facing documentation and individual test files for usage examples.

**Last Updated**: Current state reflects all features demonstrated in the 36-test suite. Recent additions: lvalue slices, autovivification, labeled loops, `keys/values %{$ref}`, `push @{$h{k}}` autoviv, `print @arr`, array-in-boolean-context, wantarray chain propagation, `exists`/`delete` with parens and on array elements.