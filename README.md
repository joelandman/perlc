# perlc — Perl to Native Binary Compiler

A Perl compiler that translates Perl source to LLVM IR and links a C runtime to produce native executables. Written in C++17 using LLVM 18.

## Requirements

- `clang++` / `clang` (LLVM 18)
- `llvm-config-18`
- `libpcre2-8` (`apt install libpcre2-dev`)
- `libsqlite3-dev` (for DBI/SQLite integration, `apt install libsqlite3-dev`)

## Build

```bash
make        # produces ./perlc
make test   # runs all test programs and prints their output
make clean
```

## Usage

```bash
./perlc program.pl -o output            # compile and link
./perlc program.pl -o out.ll --emit-ir  # dump LLVM IR instead of linking
./perlc program.pl -g -o output         # compile with debugging symbols (Perl source lines in gdb)
./perlc -pm program.pl                  # install missing modules then compile
./perlc -i                              # interactive REPL mode
./perlc -i -p                           # REPL with pause after each statement
./perlc -i --jit                        # REPL with JIT compilation (experimental)
```

### REPL Mode (`-i` / `--repl`)

The compiler provides an interactive read-eval-print loop for interactive Perl development:

```bash
./perlc -i
```

**Features:**
- Reads complete Perl statements from stdin (terminated with `;`)
- Accumulates multi-line statements (continues input until `;` at depth 0)
- **Subroutines persist** between statements (accumulated in AST and recompiled together)
- Commands: `quit`, `exit`, `q` (exit), `help`, `h`, `?` (help), `clear` (clear subs), `dump` (show subs), `stats` (show stats)
- Use `perl <code>` to execute raw Perl directly (bypasses perlc)

**Pause Mode (`-p` / `--pause`):**
Pauses after each statement in REPL mode for debugging:

```bash
./perlc -i -p
```

Press ENTER to continue, or 'q' to quit.

### JIT Mode (`--jit`) [Experimental]

Enable JIT compilation in REPL mode for variable persistence:

```bash
./perlc -i --jit
```

The JIT compiles code in-memory rather than writing to disk and invoking clang. This enables:
- Faster REPL iteration (no external compilation)
- Future variable persistence between statements

Note: JIT mode is experimental. Use the default (external compilation) for production work.

## Implemented Features

### Core Language Features (Nearly Complete)

**Scalars**: integers, floats, strings, undef, all arithmetic/string/comparison operators, ternary `?:`, `++`/`--` (including magical string increment), compound assignment

**Arrays**: `@arr`, push/pop/shift/unshift (all flatten array args), `$arr[i]`, `scalar @arr`, join, split, sort, `chomp @arr`; `my @a = (@b, @c)` properly flattens

**Hashes**: `%hash`, `$h{key}`, keys/values/exists/delete, hash-from-list init

**Control Flow**: if/elsif/else, unless, while (including `while (my $var = expr)`), until, do-while, do-until, C-style for, foreach, last, next, return

**Statement Modifiers**: `STMT if COND`, `STMT unless COND`, `STMT while COND`, `STMT until COND`, `STMT for LIST`, `STMT foreach LIST`

**Subroutines**: `sub name { }`, `@_`, list unpacking, recursion, `sub { }` (anonymous subs), `\&name` (code refs), `$f->(args)` (code ref calls), `ref($f)` → `"CODE"`

**Heredocs**: `<<IDENT`, `<<"IDENT"` (interpolating), `<<'IDENT'` (literal); body collected from subsequent lines until terminator line

**`$_` as default variable**: `foreach (@arr) { }` loops over `$_`; `while (<FH>)` assigns to `$_`; `chomp`/`chop` without args operate on `$_`; bare `/regex/`, `s///`, `tr///` bind to `$_`

**`local`**: `local $x` / `local $x = val` — dynamic save/restore for scalars and special variables

**References**: `\$x`, `\@arr`, `\%h`, `\&sub`, `[...]` (anon array), `{...}` (anon hash), `$$ref`, `@$ref`, `%$ref`, `$r->[i]`, `$r->{k}`, `ref($x)`

**Regex (PCRE2)**: `=~`, `!~`, flags (i/g/s/m), capture variables `$1`–`$9`, `s/pat/repl/flags`, `/g` iterator, `/g` list context, `split(/pat/, $str)`

**tr///**: `$s =~ tr/SEARCH/REPLACE/flags` — character translation; flags: `d` (delete), `s` (squeeze), `c` (complement); ranges `a-z`; returns count

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

**String interpolation**: `"$var"`, `"${var}"`, `"$arr[i]"`, `"$hash{key}"`, `"$@"`, `"$0"`, `"$1"`-`"$9"`, `"@arr"` (joined with space)

**Command-line**: `@ARGV` (arguments), `$0` (program name); generated `main` accepts `int argc, char **argv`

**Module installation**: `-pm` flag automatically detects missing `use Module` dependencies (excluding pragmas), installs them via `cpanm --local-lib lib` into `lib/lib/perl5/`, and updates search paths.

**Note**: Many simple CPAN modules work, but complex modules with advanced OO patterns, `our` variables, or POD documentation may cause parser errors due to incomplete Perl 5 language coverage.

**eval/exceptions**: `eval { BLOCK }` — catches `die`, sets `$@`; uses `jmp_buf` alloca + `setjmp` in calling frame; `$@` is stable PerlValue* from runtime

### Advanced Features

**State Variables**: `state $x [= expr]` — per-sub static variable; initialized once (lazily on first call); mutations persist across calls

**Closures**: anonymous subs capture outer lexical `my` scalar variables by stable pointer; multiple captures; independent closure instances; nested closures

**Object-Oriented Programming**: `package Foo;`, `bless($ref, $class)`, `$obj->method(args)`, `Foo->method(args)` (class method), `ref($obj)` → class name

**Inheritance**: `use parent 'Base'` / `use base 'Base'`; inherited method lookup; method override; `SUPER::` dispatch

**Module Loading**: `use Module` loads `.pm` files from `{scriptDir, scriptDir/lib, lib, .}`; recursively inlines modules; method dispatch across module boundaries

**BEGIN/END Blocks**: `BEGIN { }` runs inline at point of declaration; `END { }` compiles as function registered via `atexit()`, runs at program exit

**Defined()**: properly checks `v->tag != PERL_UNDEF`

**$! (errno)**: `$!` → `perl_get_dollar_bang()` — refreshes `strerror(errno)` on each access

**$/ (input record separator)**: `$/` reads global sep; `$/ = undef` sets slurp mode; `local $/ = undef` temporarily enables slurp mode

## Known Limitations

- `wantarray`: always returns false (scalar context); full context tracking not implemented
- `caller()`: stub implementation only
- `local` for arrays and hashes: scalars and special vars (`$!`, `$/`) only
- `tie` / `untie`: not implemented (use `opendir`/`readdir` for directories)
- Regex modifiers `x` (extended) and `e` (eval replacement): not supported
- Runtime `require` / `do FILE`: modules only loaded at compile time via `use` inlining
- `AUTOLOAD`, `DESTROY`: not implemented
- `unshift @{EXPR}, val`: not supported (`push @{EXPR}` works)
- Complex CPAN modules (advanced OO, `our` vars, POD): may trigger parser errors; some scripts may need simplification (see `testscripts/cputemp.pl` for example rewrite using supported builtins)
- REPL: scalar/array/hash variables do not persist between statements (subroutines do persist)
- JIT mode: experimental, may have stability issues

**Debugging**: `-g` flag now produces binaries with debugging symbols + Perl source line information (via LLVM debug metadata). Use with `gdb` to see original `.pl` lines.

## New Features Implemented

### XS Interface
- Dynamic loading of C libraries via `perl_xs_load()` function
- Support for calling C functions from Perl code using XS-like interface
- Function signature handling and proper argument passing

### DBI/SQLite Integration
- Database connectivity framework with standard DBI functions
- Support for database connections, prepared statements, and query execution
- Integration with Perl's value system through `PerlValue*` return types

## Architecture

```
source.pl  →  Lexer  →  Parser  →  AST  →  Codegen  →  LLVM IR  →  clang-18  →  binary
                                                                           ↑
                                                                     runtime.c (linked in)
```

### Runtime Value Model

All Perl values are heap-allocated `PerlValue` structs (tagged union):

```c
typedef struct PerlValue {
    PerlTag tag;        // UNDEF, INT, FLOAT, STRING, REF_SCALAR, REF_ARRAY, REF_HASH, FILEHANDLE, CODE_REF
    union { long long ival; double fval; char *sval; void *pval; };
    long long matchpos; // /g iterator position
    char *blessed_class; // for objects
} PerlValue;
```

### Key Components

- `src/lexer.h/cpp`: Context-aware tokenizer
- `src/ast.h`: Node kinds (`NK` enum) and `Node` struct
- `src/parser.h/cpp`: Recursive-descent parser → AST
- `src/codegen.h/cpp`: AST → LLVM IR via IRBuilder
- `src/runtime.h/c`: C runtime: `PerlValue` tagged union and all operations
- `src/main.cpp`: Driver: lex → parse → codegen → clang-18 link