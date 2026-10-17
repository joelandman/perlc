# perlc — Perl to Native Binary Compiler

A Perl compiler that translates Perl source to LLVM IR and links a C runtime to produce native executables. Written in C++17 using LLVM 18.

## Requirements

- `g++` (any recent version; verified with 15.2.0) and `clang-18` (used to link generated programs)
- `llvm-config-18`
- `libpcre2-8` (`apt install libpcre2-dev`)
- `libsqlite3-dev` (for DBI/SQLite integration, `apt install libsqlite3-dev`)

## Build and test

`make` with no goal builds the compiler. It is the same as `make all`:
it links `./perlc` from the C++ sources and `src/runtime.c`, and it does
not run any tests. `make help` prints the target list. The recipes below
rebuild `./perlc` first when sources are newer than the binary, except
`make clean` and `make help`.

### Build the compiler

```bash
make
```

Success is a `./perlc` you can run. If the binary is already current,
make prints `Nothing to be done for 'all'.` A one-file check:

```bash
./perlc -e 2>/dev/null || true
printf '%s\n' 'print "hi\n";' > /tmp/perlc-hello.pl
./perlc /tmp/perlc-hello.pl -o /tmp/perlc-hello
/tmp/perlc-hello
```

The last command prints `hi`. `./perlc` with no script prints the usage
text to stderr and exits 1. That is the driver, not the `make` default.

`make clean` deletes `./perlc`, `./perlc_tsan`, and every `src/*.o`.
The next `make` is a full rebuild.

### `make test` — four assertion programs

Runs these programs and requires each process to exit 0. It does not
diff them against `perl`.

- `tests/test_do_filename.pl` — `do FILE` return value, missing file, parse error, runtime `die`
- `tests/test_require_simple.pl` — `require` of a small module
- `tests/dbi_sqlite.pl` — DBI talking to SQLite (`libsqlite3`)
- `tests/xs_ffi.pl` — the DynaLoader-style FFI fixture under `tests/lib/auto/`

```bash
make test
```

Each program prints `name=ok` lines and a `*_done` line. The recipe
stops at the first program that exits non-zero. Compiled binaries go
to `/tmp/perlc_<name>`. The `do` test creates and then deletes
`test_do_*.pl` in the current directory.

Use this when you touched `do`, `require`, DBI, or the FFI loader and
want a short contract check. The full gate below skips the DBI, XS,
and Cpanel::JSON::XS files unless you name them.

### `make test-all` — correctness gate

Compiles every `tests/*.pl` with `./perlc`, runs that binary and `perl`
on the same script, and diffs the output. This is the gate to run
before a commit that changes the compiler. It takes several minutes.

```bash
make test-all
```

A passing run ends with:

```text
=== Summary ===
Total compared: 599
PASS: 599
FAIL: 0
All compared tests match perl output (within tolerance where applicable).
```

The compared count grows as new tests are added. Exit status is 0 only
when `FAIL` is 0. These files are listed and not compared, because the
host `perl` cannot load the module either (`Cpanel::JSON::XS`) or the
test needs a local database or a built fixture:

- `tests/dbi_sqlite.pl`
- `tests/xs_ffi.pl`
- `tests/cpanel_json_xs_smoke.pl`
- `tests/cpanel_json_xs_deep.pl`

Run one of those on purpose:

```bash
./tests/harness.sh tests/dbi_sqlite.pl
./tests/harness.sh tests/cpanel_json_xs_smoke.pl
```

One script, or a few, without the rest of the suite:

```bash
./tests/harness.sh tests/double_heredoc_smoke.pl tests/or_print_deep.pl
```

`nb.pl`, `nbody.pl`, `mbs.pl`, `fibn.pl`, and `arith.pl` allow a
relative float tolerance of `1e-9` (override with `TOLERANCE=`).
`mbs.pl` gets 300 seconds; every other test gets 60
(`TIMEOUT_SEC=`). Tests run in a pool of `nproc/2` (`CORE_DIVISOR=`,
or `RUN_IN_PARALLEL=0` for one at a time). `make test-full` is the
same recipe as `make test-all`.

### `make test-smoke` — smoke tests only

Same diff as `make test-all`, but only files named `*_smoke.pl`.
Use this after a parser or lexer change when you want the short
repro for each fix before paying for the deep tests.

```bash
make test-smoke
```

Success is the same `FAIL: 0` summary, with a smaller `Total compared`.
`cpanel_json_xs_smoke.pl` is still skipped unless you name it.
A single smoke file:

```bash
./tests/harness.sh --smoke-only tests/double_heredoc_smoke.pl
```

`--smoke-only` together with a path that is not a smoke test selects
nothing and exits 1 (`No tests selected.`).

### `make test-assertion` — the four programs, diffed

Same four programs as `make test`, compared byte-for-byte with `perl`
instead of only checking the exit code. DBI and the XS fixture are
included on purpose.

```bash
make test-assertion
```

Success is `Total compared: 4`, `PASS: 4`, `FAIL: 0`. If SQLite or
the FFI fixture is missing, this target fails; `make test-all` does
not, because it skips those two files.

### `make test-realworld` — compile system scripts, do not run them

Compiles each absolute path in `tests/realworld/manifest.txt`. It
never executes the binary. Several of those scripts are system tools.
The recipe always exits 0. A missing module (`Can't locate`) is a
report, not a compiler failure. This target is not part of
`make test-all`.

```bash
make test-realworld
```

Each path prints one class: `ok`, `missing-file`, `missing-module`,
`parse-error`, or `other-compile-error`. The summary line looks like:

```text
Summary  total=30  ok=15  missing-file=0  missing-module=10  parse-error=5  other=0
Report   tests/realworld/last-report.txt
This corpus is not a gate. Exit 0.
```

The counts are host-specific. `last-report.txt` is rewritten every
run. `tests/realworld/baseline.txt` is replaced only when it is
missing, or when you ask:

```bash
REALWORLD_UPDATE_BASELINE=1 make test-realworld
```

`REALWORLD_TIMEOUT` is seconds per compile (default 60). A timeout is
`other-compile-error`. `PERLC=/path/to/perlc` selects another binary.

### `make test-tsan` — threads under ThreadSanitizer

Builds a separate binary, `./perlc_tsan`, with `-fsanitize=thread`.
It does not replace `./perlc`. It then compiles and runs only:

- `tests/threads.pl`
- `tests/threads_atomic.pl`
- `tests/destroy.pl`

The compile step sets `TSAN_OPTIONS=die_after_fork=0` (the compiler
itself forks `clang-18`). The run step sets
`TSAN_OPTIONS=halt_on_error=1`. Output is the program's own prints.
There is no diff against `perl`.

```bash
make test-tsan
```

Success is three sections (`=== TSan: tests/threads.pl ===` and the
two siblings) and exit 0. Any ThreadSanitizer error fails the recipe.
The first compile of `perlc_tsan` is a full second build.

### `make test-tsan-full` — ThreadSanitizer on the suite

Same `./perlc_tsan` binary and the same `TSAN_OPTIONS`, over every
`tests/*.pl` except `dbi_sqlite.pl`, `xs_dbi_test.pl`, and `xs_ffi.pl`
(`xs_dbi_test.pl` is not in this tree; the name is still skipped if
it appears). This is a long run. It checks that the compiled programs
stay quiet under TSan. It does not diff against `perl`.

```bash
make test-tsan-full
```

Success is exit 0 and a `=== TSan: ... ===` header per file. A
sanitizer report aborts that program and fails the recipe.

### `make test-valgrind` — leak check

Uses the ordinary `./perlc`, then runs each compiled program under:

```bash
valgrind --tool=memcheck --leak-check=full --errors-for-leak-kinds=all --error-exitcode=1
```

Program stdout is discarded. DBI and XS tests are skipped, same names
as `make test-tsan-full`. There is no diff against `perl`.

```bash
make test-valgrind
```

Success is exit 0 and a `=== Valgrind: ... ===` header per file. On
this host, valgrind has died with SIGILL on every perlc binary
(noted with the D151 file-op work). If that still happens, the recipe
fails immediately and this target is not evidence about leaks.

### `make test-optN` and `make test-opt-matrix` — optimization levels

`make test-opt2` is the full `make test-all` diff, with `./perlc -O2`
for every script. The digit is required and must be 0, 1, 2, 3, 4,
or 5. `make test-opt` alone is an error.

```bash
make test-opt0
make test-opt2
```

`make test-opt-matrix` runs that gate three times, at `-O0`, then
`-O2`, then `-O3`, and stops at the first level that fails. Budget
about three times a `make test-all` run.

```bash
make test-opt-matrix
```

What the digit means is in the Usage section below. The compiler
binary itself is still the normal `./perlc`; only the programs it
emits are optimized at that level.

### `make bench` — timing

Builds `./perlc` if needed, then runs `bench/bench.sh`. With no
arguments that script times every `bench/*.pl` (or `tests/*.pl` if
`bench/` has none) once, against `perl`, and writes `bench/results.csv`.

```bash
make bench
make bench BENCH_ARGS='-n 3 bench/nb.pl bench/fibn.pl'
make bench BENCH_ARGS='--baseline'
make bench BENCH_ARGS='--compare'
```

`-n 3` is three runs, average reported. `--baseline` records perlc
times only. `--compare` diffs the current run against the last
baseline. Each run's wall clock is capped at 120 seconds.

## Usage

```bash
./perlc program.pl -o output            # compile and link (default opt level 2)
./perlc program.pl -o out.ll --emit-ir  # write LLVM IR and stop
./perlc program.pl -o out.bc --emit-bc  # write LLVM bitcode and stop
./perlc program.pl -g -o output         # Perl source lines in the binary for gdb
./perlc -pm program.pl                  # cpanm-install missing modules, then compile
./perlc -I/opt/lib program.pl -o output # extra module search directory
./perlc program.pl -O0 -o output        # no IR optimization
./perlc program.pl -v -o output         # print the clang link line
```

With no script, `./perlc` prints this option list and exits 1.

| Option | What it does |
|---|---|
| `-o <file>` | Output path. Default is `a.out` in the current directory. With `--emit-ir` and no `-o`, the path is the script name with `.ll`. `--emit-bc` uses `.bc` the same way. |
| `--emit-ir` | Write LLVM IR and do not link. |
| `--emit-bc` | Write LLVM bitcode and do not link. |
| `--do-lib` | Write a shared library for runtime `do FILE`. The runtime invokes this itself. The library is not linked with `runtime.c`; it uses the caller's runtime when loaded. |
| `--eval-lib` | Same as `--do-lib`, and bind outer `my` cells from the eval pad. The runtime invokes this for string `eval`. |
| `-O` / `-O0` … `-O5` | Optimization. No flag means level **2**. A bare `-O` means level **1**. `-O0` skips the LLVM IR passes. `-O1` uses the LLVM O1 pipeline. `-O2` uses O2. `-O3`, `-O4`, and `-O5` use the LLVM O3 pipeline. The same digit is passed to `clang-18 -O<digit> -march=native` when linking. `clang-18` warns that `-O4` is deprecated, and it rejects `-O5`. |
| `-g` | Debugging symbols, so a debugger can show Perl source lines. |
| `-v` | Print the `clang-18` link command. |
| `-pm` | Run `cpanm` for modules the script `use`s and that are not found, then compile. |
| `-I<dir>` | Add `<dir>` to the module search path. `-I <dir>` is the same. `PERL5LIB` (colon-separated) is prepended as well, and `use lib` inside the script is honored. |
| `--mini-gmp` | Accepted and ignored as a switch. Math::BigInt always uses the bundled mini-gmp. System GMP is not linked. It also turns on `-v`. |

Workflow: see the IR for one function, then link a binary at `-O0` when
you are chasing a miscompile.

```bash
./perlc tests/double_heredoc_smoke.pl --emit-ir -o /tmp/heredoc.ll
./perlc tests/double_heredoc_smoke.pl -O0 -g -o /tmp/heredoc
/tmp/heredoc
```

`perlc` looks for `src/runtime.c` next to the compiler binary, then in
the current directory. Run it from a checkout, or keep the `src/`
directory beside the binary you copied.

## Performance

perlc compiles to a native binary via LLVM, so `-O` level matters.
Benchmarked 2026-10-01 against the host's real `perl` (5.42) on the
same machine, using `bench/fibn.pl`, `bench/nb.pl`, `bench/mbs.pl`,
and `bench/regex_heavy.pl` (the last two scaled up from their
checked-in defaults — see footnotes — since sub-30-second native
`perl` runs are too noisy for a meaningful comparison):

| Benchmark | Opt | perlc (ms) | perl (ms) | Speedup (perl/perlc) |
|---|---|---|---|---|
| **fibn(39)** — recursive, exponential | O0 | 69,841 | 39,240 | 0.56× (perlc slower) |
| | O1 | 26,966 | 39,240 | 1.46× |
| | O2 | 25,292 | 39,240 | 1.55× |
| | O3 | 25,071 | 39,240 | **1.57×** |
| **nb(50,000,000)** — N-body, tight numeric loop | O0 | 25,955 | 250,395 | 9.65× |
| | O1 | 14,739 | 250,395 | 16.99× |
| | O2 | 13,494 | 250,395 | **18.56×** |
| | O3 | 14,790 | 250,395 | 16.93× |
| **mbs** †(Mandelbrot grid, N=1024) | O0 | 22,587 | 65,789 | 2.91× |
| | O1 | 9,076 | 65,789 | 7.25× |
| | O2 | 8,451 | 65,789 | **7.79×** |
| | O3 | 8,497 | 65,789 | 7.74× |
| **regex_heavy** ‡(6M-item corpus × 70 passes, 420M matches) | O0 | 28,807 | 30,578 | 1.06× |
| | O1 | 11,676 | 30,578 | 2.62× |
| | O2 | 11,698 | 30,578 | 2.61× |
| | O3 | 11,671 | 30,578 | **2.62×** |

† `bench/mbs.pl` ships with a hardcoded `$N=4096` grid, too expensive
to run under native `perl` in a reasonable time/memory budget for
benchmarking (12+ minutes, 6+ GB RSS). The table uses a `$N=1024`
copy of the same code instead.
‡ `bench/regex_heavy.pl` ships with a 100,000-item corpus × 50
passes (5M matches); the table uses a scaled-up 6,000,000-item
corpus × 70 passes (420M matches) to get a stable, non-noisy native
`perl` baseline.

**Takeaways:**
- Once past `-O0`, perlc beats native `perl` by **1.6×–18.6×** across
  these benchmarks — the N-body numeric loop sees the largest win.
- `-O0` → `-O1` captures nearly all the available speedup; `-O2`/`-O3`
  add only a few more percent on top of `-O1` here (and `-O3` is
  within noise of `-O2`, sometimes marginally behind it).
- `fibn` (pure recursive call/return, no loop to optimize) is the one
  case where perlc's unoptimized (`-O0`) output is slower than real
  Perl's mature interpreter loop — it needs at least `-O1`'s call
  overhead cleanup/inlining to pull ahead, and even then the margin
  is the smallest of the four benchmarks.
- `regex_heavy`'s lower ceiling (~2.6×) reflects that both perlc and
  Perl bottleneck on the same underlying PCRE2 engine for the actual
  matching; perlc's edge there is purely from avoiding Perl's
  per-iteration interpreter dispatch overhead around each match call.

Run it yourself: `make bench` (see `bench/bench.sh`), or compile a
benchmark directly with an explicit level, e.g.
`./perlc bench/nb.pl -O3 -o nb_opt3`.

## Implemented Features

### Core Language Features (Nearly Complete)

**Scalars**: integers, floats, strings, undef, all arithmetic/string/comparison operators, ternary `?:`, `++`/`--` (including magical string increment), compound assignment (`+=` `-=` `*=` `/=` `.=` `%=` `**=` `||=` `&&=` `//=` `&=` `|=` `^=` `<<=` `>>=` `x=`)

**Arrays**: `@arr`, push/pop/shift/unshift (all flatten array args), `$arr[i]`, `scalar @arr`, join, split, sort, `chomp @arr`; `my @a = (@b, @c)` properly flattens; `@arr = @other` copy; `@arr = ()` clear; `(sort { } @arr)[0]` list subscript

**Hashes**: `%hash`, `$h{key}`, keys/values/exists/delete, hash-from-list init; `%h = (list)` replaces all entries

**Control Flow**: if/elsif/else, unless, while (including `while (my $var = expr)`), until, do-while, do-until, C-style for, foreach, last, next, return

**Statement Modifiers**: `STMT if COND`, `STMT unless COND`, `STMT while COND`, `STMT until COND`, `STMT for LIST`, `STMT foreach LIST`

**Subroutines**: `sub name { }`, `@_`, list unpacking, recursion, `sub { }` (anonymous subs), `\&name` (code refs), `$f->(args)` (code ref calls), `ref($f)` → `"CODE"`

**Heredocs**: `<<IDENT`, `<<"IDENT"` (interpolating), `<<'IDENT'` (literal); body collected from subsequent lines until terminator line

**`$_` as default variable**: `foreach (@arr) { }` loops over `$_`; `while (<FH>)` assigns to `$_`; `chomp`/`chop` without args operate on `$_`; bare `/regex/`, `s///`, `tr///` bind to `$_`

**`local`**: `local $x`, `local @arr`, `local %hash` — dynamic save/restore for scalars, arrays, hashes, and special variables (`local @ARGV`, `local $/`, etc.)

**References**: `\$x`, `\@arr`, `\%h`, `\&sub`, `[...]` (anon array), `{...}` (anon hash), `$$ref`, `@$ref`, `%$ref`, `$r->[i]`, `$r->{k}`, `ref($x)`

**Regex (PCRE2)**: `=~`, `!~`, flags (i/g/s/m/e/x), capture variables `$1`–`$9`, `s/pat/repl/flags`, `/g` iterator, `/g` list context, `split(/pat/, $str)`, named captures `(?<name>...)` → `$+{name}` / `keys %+`; `/e` evaluates replacement as Perl expression; `/x` ignores unescaped whitespace and `#` comments (PCRE2_EXTENDED); `m{pat}x` / `m(pat)x` delimiters

**tr///**: `$s =~ tr/SEARCH/REPLACE/flags` — character translation; flags: `d` (delete), `s` (squeeze), `c` (complement); ranges `a-z`; returns count

**Range**: `1..N` in for/foreach, list context (`my @r = (1..10)`), join context

**sprintf/printf**: full format string — `%s %d %i %u %f %e %E %g %G %x %X %o %b %c %%`, width/precision literals and `*` from args

**File I/O**: `open(my $fh, mode, file)`, `open(my $fh, "modeFile")` (2-arg), `close($fh)`, `<$fh>` (scalar readline), `my @lines = <$fh>` (array readline), `print $fh`, `say $fh`, `printf $fh`, `eof($fh)`, `die`, `print STDERR`, `unlink`

**Operators**: `<=>` (spaceship numeric), `cmp` (spaceship string), `x` (string repetition), bitwise `&` `|` `^` `~` `<<` `>>`, low-precedence `and` `or` `not`

**List ops**: `map { BLOCK } LIST`, `grep { BLOCK } LIST`, `sort { CMP } LIST` (with `$a`/`$b` comparator patterns), `reverse @arr` (array), `scalar reverse $str` (string)

**Math builtins**: `abs`, `int` (truncate), `sqrt`

**String builtins**: `uc`, `lc`, `ucfirst`, `lcfirst`, `index($str, $sub[, $pos])`, `rindex($str, $sub[, $pos])`, `chr`, `ord`, `hex`, `oct`

**Slices**: `@arr[0,1,2]` (array slice), `@hash{'a','b'}` (hash slice); qw() and list args auto-flattened

**System/env**: `system("cmd")` (exit code), `` `cmd` `` (output capture, interpolated), `$ENV{KEY}` / `$ENV{KEY}=val`, `syscall`, `fork`/`wait`/`waitpid`/`kill`/`exec`/`exit`/`pipe`, `getppid`/`getpgrp`/`umask`, sockets (`socket`/`bind`/`listen`/`accept`/`connect`/`send`/`recv`), `sysopen`/`sysread`/`syswrite`/`flock`, `vec`, `select` (1-arg default FH and 4-arg bitmasks), `fcntl`, `ioctl`, `POSIX::dup`/`dup2`, live `%SIG`, `$?`

**File tests**: `-e` (exists), `-f` (file), `-d` (dir), `-r` (readable), `-w` (writable), `-x` (executable), `-z` (empty), `-s` (size), `-l` (symlink), `-p` (pipe)

**Builtins**: chomp, chop, length, substr, join, split, push, pop, shift, unshift, splice, sort, keys, values, exists, delete, scalar, defined, ref, warn, print, say, printf, sprintf, unlink

**String interpolation**: `"$var"`, `"${var}"`, `"$arr[i]"`, `"$arr[$i]"`, `"$hash{key}"`, `"$hash{$var}"`, `"$Pkg::var"`, `"@Pkg::arr"`, `"$@"`, `"$0"`, `"$1"`-`"$9"`, `"@arr"` (joined with space), `"@{expr}"`, `"@$ref"`, `"${\expr}"`

**Array last index**: `$#arr` (equivalent to `scalar(@arr) - 1`)

**`qq{...}`**: double-quoted string with balanced brace support — nested `{` `}` do not terminate the string

**`chomp` return value**: returns number of characters removed (1 or 0), not the modified string

**`substr` as lvalue**: `substr($str, offset, len) = $replacement` — replaces a substring in-place

**Command-line**: `@ARGV` (arguments), `$0` (program name); generated `main` accepts `int argc, char **argv`

**Module installation**: `-pm` flag automatically detects missing `use Module` dependencies (excluding pragmas), installs them via `cpanm --local-lib lib` into `lib/lib/perl5/`, and updates search paths.

**Getopt::Long** (2026-09-09): `GetOptions("foo=s" => \$foo, ...)` and `GetOptions(\%opt, "foo=s", ...)`; boolean flags, alternate names (`"foo|f"`), negation (`"foo!"`/`--no-foo`), increment (`"foo+"`), typed values (`=s`/`=i`/`=f`), array-collecting (`=s@`) and hash-collecting (`=s%`) options, long/short forms, `--` end-of-options, unknown-option failure with the real `"Unknown option: name"` message. Mutates `@ARGV` in place. Not supported: bundled short options, glued short values (real Getopt::Long rejects these too by default), `pass_through`/`gnu_getopt` config.

**Data::Dumper** (2026-09-09): `Dumper(...)` reproduces the default `Indent=2` style byte-for-byte, including blessed objects, scalar refs, and the IV-unquoted/NV-and-string-quoted distinction real Dumper makes. `$Data::Dumper::Sortkeys` is honored within the scope it's set in (see Known Limitations). Not supported: circular-reference detection, code refs/globs, other `$Data::Dumper::*` config knobs.

**File::Basename** (2026-09-09): `basename`, `dirname`, `fileparse` (both list- and scalar-context forms). Suffix arguments must be plain strings, not `qr//` (not implemented as a value type at all yet).

**Note**: Many simple CPAN modules work, but complex modules with advanced OO patterns or `our` variables may cause parser errors. POD (`=pod`/`=head1`…`=cut`) is skipped by the lexer.

**eval/exceptions**: `eval { BLOCK }` — catches `die`, sets `$@`; uses `jmp_buf` alloca + `setjmp` in calling frame; `$@` is stable PerlValue* from runtime. String `eval EXPR`: constant strings without new subs are inlined (outer `my` visible); dynamic strings and strings that define subs compile via `--eval-lib`/`dlopen` with an eval pad so the caller's `my` cells are aliased.

**`use v5.xx` / signatures**: `use v5.36` (and `use 5.036`) enables the feature bundle — signatures at ≥5.20, warnings at ≥5.36. `use feature 'signatures'` / `qw(signatures)` also works. `sub f($x, $y = 0, @rest)` desugars to `my (...) = @_` plus default-arity `if`s. Without the feature, `sub f($$)` remains a prototype.

**Prototypes**: `sub f ($$)`, `(@)`, `()`, `(&@)` (block form), `($;$)`, `(_)`. `&name()` bypasses the prototype. Arity is checked at parse time.

**goto**: `goto LABEL` (including labeled loops) and `goto &NAME` (tail-call, current `@_`).

**Typeglobs**: `*NAME` stringifies as `*main::NAME`; `*alias = \&sub` registers a callable alias; `*a = \$x` / `\@arr` / `\%h` share the pointed-to scalar/array/hash cell (`*a = *b` copies slots). Bare filehandles: `open LOG, ">", $path`, `print LOG ...`, `<IN>` store/use the glob scalar slot as a FILEHANDLE.

**UTF-8 layers**: `open`/`binmode` with `:utf8` or `:encoding(UTF-8)` mark the handle; readline sets `PV_FLAG_UTF8` on the string; writes latin-1-upgrade when the FH is UTF-8 and the string is not. `use utf8` marks source string literals as characters (`length("é") == 1`).

**Diamond / DATA**: `<>` and `<ARGV>` walk `@ARGV` (or STDIN if empty), set `$ARGV`, and skip missing files with a warning. `__DATA__` / `__END__` in the main file feed `<DATA>`.

**unshift / exists**: `unshift @$ref, LIST` and `unshift @{EXPR}, LIST`; `exists $h{a}{b}` (intermediates autovivify, last key does not).

### Advanced Features

**State Variables**: `state $x [= expr]` — per-sub static variable; initialized once (lazily on first call); mutations persist across calls

**Closures**: anonymous subs capture outer lexical `my` scalar variables by stable pointer; multiple captures; independent closure instances; nested closures.  **Named subs** (`\&worker` passed to `threads->create`) also build a `PerlClosure` and capture shared scalars in the enclosing scope — the runtime's `clone_code_ref_for_thread` preserves the cell pointer for `PV_FLAG_SHARED` cells so spawned threads see the same cell the parent sees.

**Object-Oriented Programming**: `package Foo;`, `bless($ref, $class)`, `$obj->method(args)`, `Foo->method(args)` (class method), `ref($obj)` → class name, chained method calls (`->m1->m2->m3`), `AUTOLOAD` (catches unknown methods, sets `$AUTOLOAD`), `DESTROY` (fires for hash-backed and array-backed objects), `caller()` returns `(package, file, line)`, `$Package::var` cross-package variable access

**Inheritance**: `use parent 'Base'` / `use base 'Base'` (including `-norequire`); inherited method lookup; method override; `SUPER::` dispatch

**Module Loading**: `use Module` loads `.pm` files from `{scriptDir, scriptDir/lib, lib, .}`; recursively inlines modules; method dispatch across module boundaries

**Threads** (`use threads`): `threads->create(sub{...}, @args)`, `$thr->join()`, `$thr->detach()`, `$thr->tid()`, `threads->self()`, `threads->list()`, `threads->yield()`.  Each thread gets its own thread-local freelist/eval-stack.  Closure captures are deep-copied per thread for isolation; **shared** captured scalars (declared with `: shared`) preserve the cell pointer across the clone so cross-thread writes are visible.

**threads::shared** (`use threads::shared`): `my $x : shared` (or `our $x : shared`, or `our ($a, $b) : shared = ...`) declares scalars/arrays/hashes as cross-thread shared.  Shared scalars use a tagged-cell layout (`PerlValue*` with `PV_FLAG_SHARED`, no wrapper).  Reads/writes go through `perl_atomic_load` / `perl_atomic_store`; RMW on int/float payloads (`$x++`, `$x = $x + 1`, `$x += N`) goes through a **lock-free 16-byte CAS-on-payload** (`cmpxchg16b` on x86_64 / `ldxp+stxp` on aarch64) — no syscall, no kernel scheduling.  Non-numeric payloads fall back to a lazy-installed per-cell `SharedMutex` (allocated only on the first `lock()` / `cond_wait()` call).  `lock($x)` / `cond_wait($x)` / `cond_signal($x)` / `cond_broadcast($x)` with per-thread re-entry.  See `THREADS_SHARED_ATOMIC.md` for the full memory model and cost table.

**BEGIN/END Blocks**: `BEGIN { }` runs inline at point of declaration; `END { }` compiles as function registered via `atexit()`, runs at program exit

**Defined()**: properly checks `v->tag != PERL_UNDEF`

**$! (errno)**: `$!` → `perl_get_dollar_bang()` — refreshes `strerror(errno)` on each access

**$/ (input record separator)**: `$/` reads global sep; `$/ = undef` sets slurp mode; `local $/ = undef` temporarily enables slurp mode

## Known Limitations

- **(2026-09-13, no open generated-code defects remain — see `TESTS.md`
  for the fixed-defect write-ups and `MVP_ROADMAP.md` for the current
  priority plan)**
  There is no `Exporter`/`@EXPORT` mechanism yet, so arbitrary pure-Perl
  CPAN modules can't export subs into a caller's namespace — arguably
  the remaining ceiling on "arbitrary module just works."
  (Fixed 2026-09-13: fully-qualified `$Pkg::var`/`@Pkg::arr`/`%Pkg::hash`
  are now true cross-scope globals, D110; `$$aref[0]`-style subscripted
  dereference inside plain `"..."` strings now interpolates correctly,
  D120; `__SUB__` works — anonymous recursion keeps the closure's own
  captures, D124. Fixed 2026-09-14/15: bareword `all => 1` auto-quote +
  `@h{qw(a b)}` slice keys, D136/D137; `%EXPORT_TAGS`/`:tag`/`:all`
  Exporter imports; DynaLoader-compatible FFI; Cwd, Sys::Hostname,
  File::Spec(+::Functions), Time::Local native modules; mixed-sigil
  `my (%h)`/`our (%a, $b)` lists; multi-token `use constant` values;
  `q!`/`tr|/|_` arbitrary delimiters; `1<<5` heredoc mis-lex.
  Fixed 2026-09-16: native `Config` module (`$Config{...}` + the 4
  functions, host-exact values), Fcntl/POSIX/Errno native constants
  (`use Fcntl qw(:seek :flock)`, SEEK_SET/O_CREAT/LC_ALL/ENOENT ...),
  `sysseek`, `$s =~ $var` dynamic match patterns, `${"opt_$x"}`
  symbolic deref, `sub f { return (32); }` returning the element,
  `$h{lc $k}` computed keys, `use constant B => A + 1` chains.
  Fixed 2026-09-16 (cont.): `qr//` as a compiled-pattern value
  (`ref()` → Regexp, stringification, =~/!~ dispatch), `/$var/`
  regex-pattern interpolation (also in `s///` and `qr/$var/`), and
  list-context match captures (`my @m = ($s =~ /pat/)`).
  (Fixed 2026-09-12: `use`/`no` pragmas now parse inside any nested
  scope, D125; `split` patterns with capturing groups now interleave
  the captured delimiter text, D126; a BigInt-tagged variable operand
  no longer bypasses overflow-aware arithmetic through the F64 fast
  path, D132; a double-free in the int/float-variable assignment
  fallback — `my $x = 0; { $x = "s" + 0; }` — no longer corrupts the
  allocator, D133; `syscall()` buffer arguments now receive kernel
  writes, D134. Fixed 2026-09-11: D103/D104/D106/D108. Fixed
  2026-09-09/10: `my @b = @a;` used to alias `@a`'s storage (D99); an
  aliased FLAT_ARRAY anon-arrayref silently forked into two arrays
  (D105, D106); a list assignment used as a boolean condition never ran
  its body (D100); `s/\\/\\\\/g` doubled the inserted backslash (D107);
  hash-to-hash copy produced wrong contents (D111); module file-lexical
  `my` collided with the main script's (D112); undefined-sub calls and
  unresolvable `use`s were silent no-ops (D113); non-literal array
  slices returned one element (D114); bare `return;` yielded a 1-element
  list (D115); `s///` replacement text didn't interpolate variables
  (D109); `$::name` shorthand was a parse error (D121); `use vars`
  export declarations were missed (D122); `&`-sigil export names were
  missed (D127); `local($var) = EXPR` was a parse error (D129); `die
  REF` lost the reference (D102); string→number coercion used a
  hand-rolled float parser (D117); `split` had no LIMIT (D118).)
- `while (...) { } continue { }` is not supported (parse error)
- `q[...]`/`qq[...]`/`qw[...]` don't balance nested `[`/`]` (unlike `qq{...}`, which balances nested `{`/`}`) — a `[` inside the string body causes a parse error
- `qr/PATTERN/` is not implemented as a value type at all
- `\my $var` / `\my %var` (reference to an inline lexical declaration) is not supported (parse error)
- Typeglob `{IO}`/`{FORMAT}` slots are not implemented (`*alias = \&sub`, stringify, `*a = \$x`/`\@a`/`\%h`, and bare `open LOG`/`print LOG` work)
- XS: DynaLoader-compatible FFI (`dl_load_file`/`dl_find_symbol`/
  `dl_install_xsub`/`bootstrap`/`XSLoader::load`) for perlc-compiled
  modules and hand-built C libraries (via `XS::call`); real perlguts
  XSUB `.so`s (SV* ABI) can't load
- DBI is the SQLite subset in the contract tests
- Complex CPAN (advanced OO / `our`) may fail to parse; POD is skipped
- String `eval` / `do FILE` at runtime re-invoke `perlc` + `clang-18`

**Debugging**: `-g` flag now produces binaries with debugging symbols + Perl source line information (via LLVM debug metadata). Use with `gdb` to see original `.pl` lines.

## New Features Implemented

### Runtime Loading
- Runtime `require "path.pm"` and `do "path.pl"` (`perl_do_file` re-invokes `perlc --do-lib` and `dlopen`s the result into the host runtime)
- Compile-time `use` inlines `.pm` files

### XS / FFI Interface
- `XS::load_library("libname")` loads and caches shared libraries with `dlopen`
- `XS::call($lib, "symbol", "signature", @args)` calls native functions through a constrained ABI bridge
- Supported MVP types: `long`, `double`, `string`, `ptr`, `void`; supported across scalar-only signatures up to 4 arguments as validated by the contract tests
- `ptr` values are opaque native pointers suitable for handles and buffers; null pointer returns map to `undef`
- **DynaLoader-compatible surface** (2026-09-14): `DynaLoader::dl_load_file`/`dl_find_symbol`/`dl_install_xsub`/`dl_error`, `bootstrap(Module)` and `XSLoader::load(Module)`. perlc-built modules (a `.pl` under `auto/...` compiled on demand, or a prebuilt `--do-lib` `.so`) load with their boot hook (`<Module>::boot`) called like real DynaLoader's `boot_<mangled>`; hand-built C libraries load via `dl_*` + `XS::call`. Real perlguts XSUB `.so`s (SV* ABI) are out of scope.

### DBI/SQLite Integration
- SQLite-backed DBI subset: `connect`, `prepare`, `execute`, `do`, `fetchrow_arrayref`, `fetchall_arrayref`, `rows`, `disconnect`, `errstr`
- Prepared statements and result rows are represented as native runtime handle/value types
- Errors propagate back into Perl-visible DBI error state and `$@` where applicable

### Concurrency (`threads` + `threads::shared`)
- **`use threads`**: native pthreads; `threads->create`, `->join`, `->detach`, `->tid`, `->self`, `->list`, `->yield`; thread-local freelist/eval-stack
- **`use threads::shared`**: `my $x : shared` / `our $x : shared` / `our ($a, $b) : shared`; tagged-cell layout; cross-thread reads/writes visible without `lock()`; RMW atomicity without `lock()`
- **Lock-free 16-byte CAS-on-payload** for int/float RMW: a single `cmpxchg16b` on x86_64 / `ldxp+stxp` on aarch64 per `$x++` or `$x = $x + 1`
- **Lazy-installed SharedMutex** side-table: per-cell mutex allocated on the first `lock()` / `cond_wait()` call; per-thread re-entry on `lock($x); $x = $x + 1`
- **`cond_wait` / `cond_signal` / `cond_broadcast`** with standard pthread semantics
- **Named-sub closure capture**: `\&worker` passed to `threads->create` builds a `PerlClosure` and captures shared scalars in the enclosing scope

## Architecture

```
source.pl  →  Lexer  →  Parser  →  AST  →  Codegen  →  LLVM IR  →  clang-18  →  binary
                                                                            ↑
                                                                      runtime.c (linked in)
```

### Runtime Value Model

All Perl values are heap-allocated `PerlValue` structs (tagged union, 48 bytes):

```c
typedef struct PerlValue {
    PerlTag tag;         /* UNDEF, INT, FLOAT, STRING, refs, FILEHANDLE, ... */
    unsigned int flags;  /* shared / UTF-8 / capture */
    union { long long ival; double fval; char *sval; void *pval; };
    long long matchpos;  /* /g iterator position */
    char *blessed_class;
    long long slen;      /* authoritative byte length of sval (embedded NULs) */
    long long _pad_reserved;
} PerlValue;
```

### Key Components

- `src/lexer.h/cpp`: Context-aware tokenizer
- `src/ast.h`: Node kinds (`NK` enum) and `Node` struct
- `src/parser.h/cpp`: Recursive-descent parser → AST
- `src/codegen.h/cpp`: AST → LLVM IR via IRBuilder
- `src/runtime.h/c`: C runtime: `PerlValue` tagged union and all operations
- `src/main.cpp`: Driver: lex → parse → codegen → clang-18 link
- `src/mini-gmp.c`: Math::BigInt backend
