# perlc — Perl→LLVM Compiler: Project State

## Overview

A Perl compiler targeting LLVM IR, written in C++17 with LLVM 18 (`clang-18`/`llvm-config-18` — the `Makefile` and `main.cpp`'s link step are the source of truth; prior versions of this doc and `README.md` incorrectly claimed LLVM 21/clang-22). All Perl operations lower to calls into a C runtime (`src/runtime.c`).

**⚠ Correctness state (updated 2026-07-13)**: A full re-verification pass starting 2026-07-09 (empirical re-test of every existing defect, root-cause of all `tests/harness.sh` failures, and a broad probe for silently-wrong-in-one-corner features) found 10 CRITICAL defects, all fixed by 2026-07-11: `my ($x,$y)=(10)` segfault, `s/$/.../ ` segfault, `foreach` not aliasing its source array, `my ($a,$b,@rest)=LIST` dropping the trailing array, 3+-level chained autovivification, `sort` returning an empty list for most argument shapes, `sort SUBNAME LIST`, regex `\\` mangling, `Carp::croak` bypassing `eval` via `exit(1)`, and `return` inside nested `eval{}` compiling to `die`. Work then continued through the Medium/Low-severity tiers of `TESTS.md`'s Defect Registry, fixing (2026-07-11 through 07-13): `use constant` from an inlined module leaking as a global sub ignoring package scoping (D26, plus its Exporter-validation follow-up), `system()`'s exit-code convention (D27), `sort` in scalar context returning a count instead of `undef` (D29), a named `sub` declared inside any bare block/if/while/for/eval body being silently uncallable (D45), a self-recursive sub matching the AST inliner's shape crashing the compiler at compile time (D57), five small runtime.c defects (`$.` not resetting on close, `looks_like_number`'s false-value type, top-level `wantarray()`, default float stringification precision — D32/D33/D43/D31), a parser code-quality duplication (D14), `Time::HiRes` implemented from scratch (D30), `do FILE` given real runtime semantics via a `--do-lib` compiler mode + `dlopen()` instead of the removed JIT (D24), `our $pkgvar` not persisting across repeated `do` calls on the same file (D58, scalar case), a compiler-side optimization cache leaking a `sqrt()`-assigned value across unrelated subs sharing a variable name (D9), chained autoviv from an existing scalar ref for all-hash-key chains (D50, e.g. `$ref->{a}{b} = val`), `\$`/`\@` not producing a literal `$`/`@` inside a double-quoted string plus two related lexer bugs found alongside it — single-quoted strings over-resolving escapes, and `qq(...)`-with-a-non-brace-delimiter losing its content entirely (D51), `sort`/`reduce` comparator blocks ignoring an outer lexical `my $a`/`my $b` shadow (D28), `sort { BLOCK }`'s comparator having zero closure-capture support for any outer block-scoped variable (D61, given real closure-capture support mirroring `AnonSub`'s), `$$` (PID) never interpolating correctly inside a double-quoted string (D59), and `$$ref[idx]`/`$$ref{key}` dereference-then-subscript producing the wrong result via an incorrect double dereference (D63). See `TESTS.md`'s Defect Registry and `REMEDIATION.md` for full root causes/fixes/test coverage. The feature-completeness prose below should still be read with the general caveat that **`TESTS.md`'s Defect Registry is the current source of truth**, not this summary — remaining open defects (mostly narrow/cosmetic now, e.g. no `use warnings` diagnostic system, sub redefinition on repeated `do`, bare `q(...)` with a non-brace delimiter not recognized (D60), a scalar-ref-rooted autoviv chain with an array-index level (D50's remainder), a subscripted dereference inside string interpolation like `"$$ref[0]"` (a narrower remainder of D63), a closure's captured scalar not seeing a reassignment/increment made to it propagate back to the caller (D62), a couple of block-scoped self-assignment edge cases) are tracked there.

**⚠ Correctness state (updated 2026-07-14)**: A second comprehensive re-review (4-way parallel: re-test every prior OPEN defect against HEAD `4ce6e88`; a fresh docs-vs-source audit; a broad new probe of string/numeric/list/regex/OOP corners; a full `tests/harness.sh` rerun) found the harness itself has **zero regressions** (same 7 pre-existing failures as 2026-07-13), but surfaced **16 new defects (D67-D82)** not previously tracked, several of them CRITICAL/HIGH silent-wrong-data bugs in extremely common code paths — most notably **`pack`/`unpack` are completely non-functional** (D67: codegen never emits calls to the runtime functions that already exist, despite being documented above as a shipped feature), **`%` (modulo) is wrong for any negative operand** (D71), and **`"$ref->{key}"`/`"$ref->[i]"` arrow-dereference does not interpolate in strings** (D72, one of the most common OOP idioms in real Perl code). Also found: `make test` currently **fails** (not "passes" as previously claimed — see D67's note in TESTS.md), `List::Util::uniq` is broken on its two most common call shapes (D69), `substr` is not actually UTF-8 aware despite the claim two paragraphs below (D68, only `length` is), and `POSIX::fmod` silently returns undef when imported unqualified (D70). Two pieces of good news: **D44 (tie/untie STORE/FETCH interception) is now fixed** (verified working, no dedicated test yet — add one before trusting it stays fixed), and D49 (`%SIG`) no longer hard-parse-errors (though it's still functionally inert). Full details, repro scripts, and severity notes for all 16 new defects are in `TESTS.md`'s "Newly Discovered Defects (2026-07-14 comprehensive re-review)" section. **Top-10 prioritized correctness fixes and top-10 missing features are listed immediately below**, in `## Priority Lists (as of 2026-07-14)`. **Fix-work update (same day)**: D66 and D71 are now both fixed (see `REMEDIATION.md` items 49-50); each uncovered one further, separate, still-open defect while its regression test was being written (D83, D84 respectively).

## Priority Lists (as of 2026-07-14)

Ordered by blast radius × silence (a silent-wrong-data bug on a common idiom outranks a loud parse error, which outranks a rare/niche gap). Full repro details for every ID are in `TESTS.md`'s Defect Registry.

### Top 10 correctness fixes (do these first)

Fixed so far: ~~**D66**~~ (2026-07-14, block-scoped `my $x=$hash{key}` coerced string values to `0`; `REMEDIATION.md` item 49), ~~**D71**~~ (2026-07-14, `%` used C truncating semantics instead of Perl's floored-division convention; item 50), ~~**D72**~~ (2026-07-14, `"$ref->{key}"`/`"$ref->[i]"` didn't interpolate at all; item 51), ~~**D67**~~ (2026-07-14, `pack`/`unpack` completely non-functional plus 7 further latent runtime bugs found while fixing it; item 52), ~~**D73**~~ (2026-07-14, array/hash slice interpolation in strings didn't work at all; item 53), ~~**D34**~~ (2026-07-14, `defined EXPR` without parens was a hard parse error; item 54), ~~**D8a**~~ (2026-07-14, `EXPR or return VALUE` parsed but never actually returned; item 55), ~~**D12**~~ (2026-07-14, `wantarray` context wasn't propagated into `print`/`printf` arguments; item 56), and ~~**D74**~~ (2026-07-14, `substr` 4-arg in-place replacement was a silent no-op; item 57). Four of the nine uncovered a separate, unrelated, still-open defect while writing their regression test — D83 (`ref()` wrong for FLAT_ARRAY-tagged scalars), D84 (`%`/`%=` by zero isn't eval-catchable), D85 (pack/unpack silently truncates at an embedded NUL byte — a deep, architectural string-representation limitation, not a targeted bug), and D12 itself uncovered three more (D86: `print {$fh} LIST` brace-form misparsed; D87: `wantarray()` has no void state at all; D88: `callCtx_` leaks list context through scalar-forcing operators at every call site *other than* print/printf, which D12's own fix specifically had to guard against). D74's own test-writing reconfirmed the already-logged D77 (negative-length `substr`) as unrelated/pre-existing rather than something it introduced. All folded into the list below.

1. **D69** — `List::Util::uniq` is broken on its two most common call shapes (array-variable argument, and fully-qualified `List::Util::uniq(...)`).
2. **D75** — multiple-inheritance method resolution picks the wrong parent (doesn't follow Perl's depth-first left-to-right `@ISA` order).
3. **D53** — a self-assigned reference (`my $r = \$x; $r = $r;`) loses aliasing to the original variable when done inside a nested block, silently going stale on subsequent writes through it.
4. **D52** — `$@ = "..."` is a silent no-op; `$@` isn't wired up as an assignment target at all, so clearing a caught exception doesn't work.
5. **D85** — `pack`/`unpack`'d binary data containing an embedded NUL byte is silently truncated (e.g. `pack("N", 1234567)`, a hugely common 4-byte network-order pack, has a leading zero byte). Architectural — `PerlValue`'s string representation has no explicit length field. Found while fixing D67.
6. **D77** — `substr` mishandles negative length (`substr($s,$off,-$n)`) and far-out-of-range negative start; reproduces identically in the plain read-only form, reconfirmed while fixing D74.
7. **D88** — `callCtx_` (list-context propagation) leaks through scalar-forcing operators like `eq`/`==` into a nested call, outside of print/printf (e.g. `my @a = (ctx() eq "x")` wrongly gives `ctx()` list context). Found while fixing D12; D12's own fix only guards against this at print/printf's call sites.
8. **D84** — `%`/`%=` by zero isn't a catchable Perl exception, and the unboxed-int fast path skips the zero-divisor check entirely. Found while fixing D71.
9. **D83** — `ref()` returns empty string instead of `"ARRAY"` for a scalar holding a FLAT_ARRAY-tagged value (e.g. `my $y=[4,5,6]; ref($y)`). Found while fixing D66.
10. **D78** — signed integer overflow wraps around instead of auto-promoting to float, silently producing a negative number instead of the correct large value.

Next tier worth fixing soon after (not in the top 10 only for space): D68 (`substr` not UTF-8 aware despite the claim below), D70 (`fmod` broken when imported unqualified), D76 (nested `DESTROY` doesn't cascade), D87 (`wantarray()` has no void state — always reports scalar/list, never `undef`, for a bare-statement void-context call), D86 (`print {$fh} LIST` brace-block filehandle form misparsed as a hash-literal argument).

### Top 10 missing features (completeness, after correctness)

1. **No `use warnings` diagnostic system** — no runtime "uninitialized value"/deprecation-style warnings exist at all (D56 and others); affects debuggability of every perlc-compiled program.
2. **`use overload`** — operator overloading is entirely unimplemented; blocks idiomatic numeric/string-like OOP classes.
3. **Alternate-delimiter substitution** (D38a) — `s{pat}{repl}`, `s#pat#repl#`, etc. aren't lexed at all; only `s/pat/repl/` works.
4. **`s///e` doesn't evaluate the replacement as code** (D38c) — silently behaves like `/e`-less substitution instead.
5. **`local` on individual hash/array elements** (D41) — `local $h{key}` / `local $arr[idx]` is a hard parse error; whole-variable `local` works.
6. **`%SIG` / signal & warn-handler wiring** (D49) — parses now but `$SIG{__WARN__}` is never actually invoked by `warn()`.
7. **`delete @hash{...}` / `delete @arr[...]`** (D81) — slice-form `delete` is a hard parse error; single-key `delete` works.
8. **`sprintf`/`printf` positional args** (D82) — `%N$s`-style explicit argument indices aren't supported.
9. **String `eval EXPR`** — deliberately cut when the JIT was removed; only `eval { BLOCK }` works. A real, intentional scope gap, not a regression.
10. **Regex `/x` (extended/whitespace-ignoring) modifier** — documented as unsupported; still a real, commonly-wanted gap for readable patterns.

Also notable but outside the top 10: `syscall()` builtin (found missing while investigating the `make test` failure — not previously documented anywhere), prototypes, typeglobs, signals generally.

**Current Status**: Core language features are broadly implemented, with substantial correctness coverage but real, common-path gaps (see Priority Lists above). `make test` (4 assertion-based files) currently **still fails**, but for a narrower reason than originally found: D67's fix resolved `tests/xs_ffi.pl`'s `clock_gettime_*` failures (they exercise `unpack`, which is now correct), leaving only 4 failures (`getuid_defined`/`getgid_defined`/`getpid_defined`/`getpid_positive`) caused by a missing `syscall()` builtin — not previously documented anywhere, found while investigating the original `make test` failure. This is a change from the previously-claimed fully-passing state, not a new regression (the underlying gap predates this doc update, it just wasn't previously exercised/noticed). `tests/harness.sh` (128 files on disk as of 2026-07-14's fix-work session — 114 plus 14 new regression tests added alongside the 7 fixes below — 126 compared after the 2 external/DBI-dependent skips) currently shows 119/126 passing, re-confirmed after each of the 7 fixes below with zero regressions at every step. Of the 7 remaining failures, only 1 represents a genuine functional gap (string `eval EXPR` — a deliberate scope cut after JIT removal, `eval { BLOCK }` works fully); the rest are either cosmetic (perlc doesn't emit `use warnings`-style runtime diagnostics — the underlying data/values already match real Perl exactly), a harness-timeout artifact on one heavy benchmark (real Perl exceeds the harness's 60s timeout; perlc finishes in ~6s), or a pre-existing qsort-vs-real-Perl's-stable-mergesort divergence on a comparator that becomes non-transitive once `$a`/`$b` is intentionally shadowed (`tier1.pl`, a narrower remainder after D28's fix). Significant coverage of Perl 5 semantics including OOP, closures, regex, modules, advanced builtins, List::Util, POSIX, Scalar::Util, Tier 2 and Tier 3 builtins, threads with threads::shared (atomic memory model: visibility-without-lock, RMW atomicity without `lock()` for single-scalar RMW, **lock-free 16-byte CAS-on-payload** for int/float RMWs, lazy-installed SharedMutex side-table, per-thread re-entry), wantarray context propagation (including through call chains and implicit returns of grep/map/sort), require, **`do FILE` runtime execution**, **`tie`/`untie` with TIESCALAR/TIEARRAY/TIEHASH support (STORE/FETCH interception confirmed working as of 2026-07-14, D44)**, **`pack`/`unpack` for binary data** (C, S, L, s, l, n, N, v, V, f, d, a, A format codes fixed and verified 2026-07-14, D67 — `h`/`H`/`b`/`B` still just copy raw bytes rather than real nibble/bit encoding, a narrower remaining gap; **any packed data containing an embedded NUL byte is silently truncated, see D85** — a deep, separate, architectural string-representation limitation, not a pack/unpack-specific bug), **UTF-8/Unicode support** (chr/ord for code points > 127, UTF-8 aware length; **substr is NOT UTF-8 aware despite earlier claims, see D68**), DESTROY (hash and array objects; does not cascade to nested blessed objects, see D76), XS interface, DBI/SQLite integration, `caller()`, AUTOLOAD, `local @arr`/`local %hash`, `(LIST)[i]` subscript, `/e` regex modifier, `$Package::var` cross-package access, lvalue array/hash slices, autovivification, labeled `next`/`last`, `map { @$_ }` flattening, hash-ref slices `@{$href}{LIST}`, `scalar(@{$ref})`, range expansion in function call args, anonymous sub implicit return, `$h{k}++` on missing keys, `sort { } qw(...)` lists, `split //` into characters, correct map body scoping, `PERL_FLOAT_PAIR` inline complex numbers, `PERL_LIST_RESULT` correct list-return tag, AST-level sub inlining, **named-sub closure capture of shared scalars** (`\&worker` passed to `threads->create`), **`our $x : shared`** parser+codegen support, **closure capture of unboxed int/float vars**, **compound `-=` on shared scalars**, **correct `or`/`and`/`xor` precedence** below `my`/`local`/`state` declaration initializers (Perl statement separators), **FLAT_ARRAY 1D ArrowDeref fast path with variable index support**, **DerefAV cache for local variables assigned from array derefs**, **`sort { BLOCK }` comparator closure-capture of outer block-scoped arrays/hashes**, and **correct `$$name[idx]`/`$$name{key}` single-dereference-then-subscript semantics**. List::Util's `uniq` is listed below as implemented but is broken on its two most common call shapes — see D69.

## Build & Test

```bash
make              # builds ./perlc
make test         # runs all 69 test programs
make test-tsan    # runs threads.pl, threads_atomic.pl, destroy.pl with -fsanitize=thread
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
| `src/main.cpp` | Driver: lex→parse→codegen→clang-21 link with module inlining |

## Architecture

- **PerlValue**: `{ PerlTag tag; union { long long ival; double fval; char *sval; void *pval; }; long long matchpos; char *blessed_class; }`
- **PerlTag**: `UNDEF=0, INT=1, FLOAT=2, STRING=3, REF_SCALAR=4, REF_ARRAY=5, REF_HASH=6, FILEHANDLE=7, CODE_REF=8, FLAT_ARRAY=10, THREAD=11, LIST_RESULT=12, FLOAT_PAIR=13`
- **PerlArray**: `{ PerlValue **elems; long long len, cap; int refcount; pthread_mutex_t *mu; }`
- **PerlHash**: 64-bucket chained hash table
- **Assignment model**: `perl_assign` — each variable's alloca holds a *stable* `PerlValue*` for its lifetime (critical for references and closures)
- **Codegen pattern**: every operation calls into C runtime via `callRT("perl_xyz", {args...})`
- **Scope model**: parallel scope stacks for scalars, arrays, hashes, float vars, int vars, and DerefAV-cached array-ref params
- **FLAT_ARRAY** (tag=10): all-numeric AnonArray literals with ≥2 elements compile to `double[]` inline (pval=double*, matchpos=count), eliminating PV boxing in hot loops; 1D ArrowDeref fast path supports both fixed and variable indices via runtime tag checks
- **LIST_RESULT** (tag=12): wraps a PerlArray* returned from a sub in list context; `perl_unwrap_list_return` spreads only this tag, not plain REF_ARRAY, so scalar refs like `[$re,$im]` are never incorrectly flattened into argument lists
- **FLOAT_PAIR** (tag=13): 2-element all-float AnonArray stored inline in one PerlValue (fval=elem[0], matchpos bits=elem[1]); eliminates inner PerlArray + 2 float PV allocations per complex number; `$z->[0]`/`$z->[1]` become direct field loads via runtime tag-check branch (perfectly predicted); variable index support via runtime PHI; `perl_assign` preserves matchpos for FLOAT_PAIR
- **AST-level sub inliner** (`tryEmitInline`): detects subs with body `my (@params)=@_; return expr`; at call sites evaluates args and binds to temp allocas without @_ construction or cloning; recursive for nested calls; `canEmitF64(NK::Call)` + `emitExprF64(NK::Call)` extend the F64 fast path through inlineable float-body subs (e.g. `cabs2($z) < 4.0` emits as pure double comparison)
- **DerefAV cache for local variables**: when `$local = $cached->[idx]` where `$cached` is a DerefAV-cached @_ param, the PerlArray* is cached for `$local` so inner-loop `$local->[i]` skips repeated `perl_deref_array_ro` calls
- **PV slab allocator**: `pv_alloc()` cold miss allocates 128 PVs contiguously (calloc), linking via pval; keeps pool entries cache-hot for tight loops with many short-lived PVs. With `-DPERL_ALLOC_DEBUG`, tracks every allocation with a sentinel for leak detection at exit.
- **PerlArray freelist pool** (`pa_alloc`/`pa_pool_push`): reuses struct + elems buffer across alloc/free cycles; PA_POOL_CAP_MAX=4096 preserves large row elems buffers
- **Module loading**: `use Module` recursively inlines `.pm` files at compile time via `inlineModules()`
- **Closure capture of unboxed vars**: closure Phase 1 capture now checks `intScopes_` and `floatScopes_` in addition to `scopes_`, boxing unboxed int/float values into `PerlValue*` for correct capture semantics

## Major Implemented Features

### Core Language
- **Variables & Literals**: scalars, arrays, hashes, integers, floats, strings (single/double-quoted with interpolation), `undef`
- **Operators**: arithmetic, string (`.`, `x` repetition), range (`..`), comparisons (`==`, `eq`, `<=>`, `cmp`), logical (`&&`, `||`, `!`, `//`), low-precedence (`and`, `or`, `not`), bitwise (`&`, `|`, `^`, `~`, `<<`, `>>`), increment/decrement (`++`/`--` including magical string increment), compound assignment (`+=`, `-=`, `*=`, `/=`, `.=`, `%=`, `**=`, `||=`, `&&=`, `//=`, `&=`, `|=`, `^=`, `<<=`, `>>=`, `x=`), ternary
- **Control Flow**: `if`/`elsif`/`else`, `unless`, `while`/`until` (including `while (my $var = expr)`), `do-while`/`do-until`, C-style `for`, `foreach`, `last`/`next`/`redo` with optional labels (`LABEL: for ... { next LABEL }`), statement modifiers
- **Subroutines**: named and anonymous subs, recursion, `@_`, list unpacking, code references (`\&sub`, `$f->()`), `ref()` returning `"CODE"`
- **Builtins**: `print`/`say`/`printf`/`sprintf`, `chomp`/`chop`, `length`/`substr`, `join`/`split`/`sort` (including `sort { BLOCK }`, `sort { BLOCK } qw(...)`, `sort { BLOCK } @arr`), `push`/`pop`/`shift`/`unshift`/`splice`, `keys`/`values`/`exists`/`delete` (all accepting `%{$ref}` / `%$ref` deref forms; `exists`/`delete` support both `$h{k}` and `$arr[N]`), `defined`, `ref`, `warn`, `die`, `abs`/`int`/`sqrt`, `uc`/`lc`/`ucfirst`/`lcfirst`, `index`/`rindex`, `chr`/`ord`/`hex`/`oct`, `reverse`, `map`/`grep`; `print @arr` prints all elements
- **Time**: `time`, `localtime`, `gmtime` (list context → 9-element list: sec,min,hour,mday,mon,year,wday,yday,isdst)
- **Randomness**: `rand [MAX]`, `srand [SEED]`
- **Process**: `sleep SECS`, `alarm SECS`
- **List::Util** (built-in, no CPAN): `sum`, `min`, `max`, `first { BLOCK } LIST`, `any { BLOCK } LIST`, `all { BLOCK } LIST`, `none { BLOCK } LIST`, `uniq LIST` (**broken on its two most common call shapes — array-variable argument and fully-qualified `List::Util::uniq(...)` — see D69**; only a bare-name call with a literal list works), `reduce { BLOCK } LIST`

### Extended Features
- **XS Interface**: Dynamic loading of C libraries via `perl_xs_load()` function and support for calling C functions from Perl
- **DBI/SQLite Integration**: Database connectivity framework with standard DBI functions including connection, prepared statements, and query execution

### Advanced Features
- **References**: all types (`\$x`, `\@arr`, `\%hash`, `\&sub`), anonymous arrays/hashes, dereferencing (`$$ref`, `@$ref`, `%$ref`, `->`), `ref()`; hash-ref slices `@{$href}{LIST}` / `@$href{LIST}`; array-ref slices `@{$aref}[LIST]`; `scalar(@{$ref})`
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
- **Threads**: `use threads`; `threads->create(sub{...}, @args)`, `$thr->join()`, `$thr->detach()`, `$thr->tid()`, `threads->self()`, `threads->list()`, `threads->yield()`; thread-local freelist/eval-stack/captures via `__thread`; `PERL_THREAD` tag (11); closure capture deep-copied per thread for isolation (non-shared captured vars are independent copies); shared captured scalars use the tagged-cell layout so both threads see the same cell (reads inside the sub go through `perl_atomic_load` automatically); **named-sub dispatch** (`\&worker` passed to `threads->create`) also captures shared scalars via a runtime `PerlClosure` with the captured cell pointers
- **threads::shared**: `use threads::shared`; `my $x : shared` / `my @arr : shared` / `my %hash : shared` (and `our $x : shared` / `our ($a, $b) : shared`); tagged-cell layout (a shared scalar is a `PerlValue*` with `PV_FLAG_SHARED`, no wrapper); `SharedMutex` is lazy-installed on first `lock()` / `cond_wait()` call and kept in a process-wide side-table; reads/writes go through `perl_atomic_load` / `perl_atomic_store`; RMW on int/float payloads (`$x++`, `$x = $x + 1`, `$x += N`) goes through `perl_atomic_inc` / `perl_atomic_add` which try a **lock-free 16-byte CAS-on-payload** (`cmpxchg16b` on x86_64, `ldxp`+`stxp` on aarch64) and fall back to the SharedMutex for non-numeric tags; `lock($x)` / `lock(@arr)` / `lock(%hash)` with auto-unlock at block exit; per-thread re-entry on the SharedMutex makes `lock($x); $x = $x + 1` safe; `cond_wait($x)` / `cond_signal($x)` / `cond_broadcast($x)`; see `THREADS_SHARED_ATOMIC.md` for the full memory model and cost table

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
- **Subs returning lists**: `sub f { grep { ... } @_ }` / `sub f { map { ... } @_ }` / `sub f { sort @_ }` correctly return lists in list context and count in scalar context; `return grep/map/sort` also propagates list context; anonymous subs (`sub { expr }->()`) correctly return the last expression's value
- **Local**: dynamic scoping for scalars, arrays, and hashes (`local $x`, `local @arr`, `local %hash`, `local $/`, `local @ARGV`; block-scoped restore)
- **Array/hash assignment**: `@arr = @other`, `@arr = ()` (clear), `%h = (list)` (replaces all entries), `(LIST)[i]` subscript on sort/map/grep/caller results; lvalue slices `@arr[i,j] = list` and `@h{qw(a b)} = list`; autovivification `$h{a}{b} = val`, `$a[i]{k} = val`, `push @{$h{k}}, val`; `map { @$_ } @aoa` flattening; range `1..N` expands in function call args; `map { expr_using_$_ }` correctly scopes results
- **Exceptions**: `eval { BLOCK }` with `$@` support using `setjmp`/`longjmp`
- **BEGIN/END**: `BEGIN` runs inline, `END` registered via `atexit()`
- **Modules**: `use Module` with recursive inlining, `@EXPORT`/`@EXPORT_OK` support, constant subs via `use constant`. The new `-pm` flag automatically detects missing modules (excluding pragmas), installs them via `cpanm --local-lib lib` into `lib/lib/perl5/`, and updates search paths.
  - **Limitation**: Complex CPAN modules (with advanced OO, `our` vars, POD, etc.) may trigger parser errors. Simple modules and our custom test modules work well.
- **Array/Hash Slices**, `qw()`, fat comma (`=>`), list flattening in various contexts

## Test Suite (128 files on disk; 119/126 compared by `tests/harness.sh` pass as of 2026-07-14)

**The "69/69 passing" claim below is stale and refers to `make test`, which only runs 4 assertion-based files — it does NOT mean all listed tests match real Perl output, and as of 2026-07-14 `make test` itself currently FAILS (`tests/xs_ffi.pl`; D67's `pack`/`unpack` fix resolved that file's `clock_gettime_*` failures, leaving only a missing `syscall()` builtin as the remaining cause — see TESTS.md).** `tests/harness.sh` (compiles + runs each test under both `perlc` and real `perl`, diffs the output) is the actual correctness gate; the test suite has grown substantially since 2026-07-09 (49 → 128 files, 14 of them added in the same 2026-07-14 session that fixed D66/D71/D72/D67/D73/D34/D8a) as each defect fix in `TESTS.md`'s Defect Registry added its own dedicated regression tests (2 files — `dbi_sqlite.pl`, `xs_ffi.pl` — are skipped by default as external/DBI/threads-dependent, leaving 126 compared). As of the 2026-07-14 full rerun after all 7 fixes (re-confirmed, zero regressions at every step), only 7 files diverge from real Perl (`advanced.pl`, `eval_string.pl`, `fileio.pl`, `mbs.pl`, `regex_named.pl`, `tier1.pl`, `wantarray_extended.pl`) — see `TESTS.md`'s Defect Registry for root causes: most of these are now cosmetic (perlc doesn't emit `use warnings`-style runtime diagnostics, D56) or a harness-timeout artifact (`mbs.pl`: real Perl exceeds the harness's 60s timeout on this heavy benchmark; perlc finishes in ~6s); `tier1.pl`'s remaining divergence is the pre-existing qsort-vs-mergesort algorithm-choice difference on a comparator made intentionally non-transitive (D28's fix corrected the shadowing itself, not this separate, inherent characteristic); only `eval_string.pl` reflects a genuine, deliberate scope cut (string `eval EXPR` was removed along with the JIT). **Note**: these 7 pre-existing harness failures are a different, narrower list than the newly-discovered defects (D67-D85) — the new defects were found by targeted probing outside the existing test suite's coverage, not by the harness itself; 7 of them (D66/D71/D72/D67/D73/D34/D8a) are now fixed with dedicated regression tests, and the remaining open ones would grow the harness's own failure count once tests are added for them too. Run `./tests/harness.sh` yourself before trusting either number.

Historical list of tests in `tests/` (originally documented as all-passing under `make test`'s narrower check):
- Core: `hello.pl`, `arith.pl`, `fib.pl`, `range.pl`, `modifiers.pl`
- Data structures: `hash.pl`, `refs.pl`, `builtins.pl`, `builtins2.pl`
- I/O & strings: `fileio.pl`, `fileops.pl`, `sprintf.pl`
- Advanced: `regex.pl`, `regex_g.pl`, `regex_named.pl`, `advanced.pl`, `features.pl`
- OOP & modules: `oop.pl`, `closures.pl`, `usemod.pl`, `inherit.pl`
- Modern features: `defaults.pl`, `newfeatures.pl`, `interp.pl` (string interpolation), `misc.pl`, `tr.pl`, `wantarray.pl`
- Performance benchmarks: `fibn.pl` (Fibonacci), `mbs.pl` (Mandelbrot set 1024×1024×80 iters — `tests/mbs.pl` sets `my $N = 1024;`; this line previously and incorrectly said 512×512, contradicting the Benchmark Results table further down in this same file)
- Tier 1 builtins: `tier1.pl` (rand/srand, time/localtime/gmtime, sleep/alarm, sort { BLOCK }, List::Util)
- Tier 2 builtins: `tier2.pl` ($/.$,/$\/$&, POSIX::floor/ceil/fmod/strftime, Scalar::Util::blessed/reftype/looks_like_number, seek/tell/binmode, stat/lstat, glob, isa/can, our @ISA)
- Tier 3 builtins: `tier3.pl` ($$/$^O, fileno, read, truncate, each %hash, pos, getpid)
- Threads: `threads.pl` (create/join/tid/self, closure capture, thread isolation, threads::shared scalars/arrays/hashes, lock/cond_wait/cond_signal/cond_broadcast, **named-sub closure capture of shared scalars**)
- Thread atomicity: `threads_atomic.pl` (Phase-1 contract for the new model — visibility-without-lock, RMW atomicity, cond_wait/signal, cond_broadcast, lock auto-release, plain-var isolation, **`our $x : shared` scalar/list/cross-package forms**; see `THREADS_SHARED_ATOMIC.md`)
- Object lifecycle: `destroy.pl` (DESTROY on scope exit, undef assignment, overwrite, loop, data access in destructor)
- String eval (EXPR): stubbed (sets $@, returns undef); `eval_string.pl` updated to test the stub; `eval { BLOCK }` still works via eval_exception.pl
- Completeness: `completeness.pl` (caller(), local @arr/local %hash, AUTOLOAD, pos() write, runtime require)

## Known Limitations

The following features are **not yet implemented** or only partially supported:

### Context and Call Stack
- `wantarray` context propagation: fully implemented — list vs. scalar context at call sites, `wantarray` builtin, and propagation through call chains (`sub outer { inner() }` inherits the caller's context)

### Module System
- `require Module::Name` and `require "file.pm"` are implemented (compile-time inlining, same as `use`); runtime `require` is also supported
- **`do FILE`** — **fixed (D24, 2026-07-12)**: real runtime execution, implemented without a JIT by having `perl_do_file()` re-invoke the `perlc` compiler itself as a subprocess (`--do-lib` mode) to compile the target into a shared library, then `dlopen()` it. The library doesn't link its own `runtime.c` — its `perl_*` symbols resolve at `dlopen()` time against the *loading* process's own runtime (via `-rdynamic` on the main executable), so a do'd file's subs, `$@`, and all other state are genuinely shared, not an isolated copy. Handles return value, calling a loaded file's subs afterward, missing files, syntax errors, and runtime `die()` (via the same `setjmp`/eval-stack mechanism `eval{}` uses) — all 8/8 checks in `tests/test_do_filename.pl` pass, byte-for-byte matching real Perl. **D58 (2026-07-12, scalar case fixed)**: `our $pkgvar` now persists/accumulates correctly across *repeated* `do` calls on the same file — file-scope scalars in `--do-lib` builds route through a new process-wide registry (`perl_get_or_create_global_scalar()`) instead of a per-compilation-unit `GlobalVariable`. **Still open**: sub redefinition on a repeated `do` of a modified file doesn't take effect (the method table keeps the first-registered version); the same persistence fix hasn't been extended to file-scope arrays/hashes; and a separately-compiled *loading* program still can't access a package variable that only a dynamically-`do`'d file ever declares (a deeper, compile-time-resolution limitation, not a regression).
- **`tie`/`untie`** — structurally present (`TIESCALAR`/`TIEARRAY`/`TIEHASH` run at `tie` time and the object is blessed) but **FETCH/STORE interception does not happen** (D44): reads/writes on a tied variable behave as a plain scalar, never calling `FETCH`/`STORE`. Verified directly with a class whose `STORE` doubles the value — perlc silently skips the doubling. This makes `tie` non-functional for its actual purpose.
- Pragmas that aren't backed by `.pm` files are silently ignored

### Regex
- Modifier `x` (extended/whitespace-ignoring patterns) not supported

### Command-line / Debugging
- `-g` flag supported: adds debugging symbols + **Perl source line mapping** via LLVM debug metadata (visible in gdb/lldb)

### Not Yet Implemented
- Overload, prototypes, typeglobs, signals, unicode handling (basic UTF-8 support added for chr/ord/length/substr)
- Many complex CPAN modules (parser may fail on advanced OO/`our`/POD; simplify scripts as needed)
 - `exists $h{a}{b}` chained hash subscript without arrow (use `$h{a}->{b}` instead)
 - `unshift @{EXPR}, val`: not supported (`push @{EXPR}` works)

## Key Implementation Details

- **Stable Pointer Model**: Variables hold stable `PerlValue*` pointers for correct reference and closure semantics
- **Runtime Heavy**: Most Perl semantics implemented in `runtime.c` (tagged union + extensive C functions)
- **Module Inlining**: `use` statements cause recursive parsing and token stream concatenation
- **Regex**: Uses PCRE2 with custom iterator state per `PerlValue` (`matchpos`)
- **Error Handling**: `die`/`eval` uses `jmp_buf` with careful stack management
- **Performance**: LLVM optimization (O2 + LTO) + C runtime with freelist pool allocator; no GC (manual via `perl_free`). Extensive unboxing optimizations: float scalar vars (`floatScopes_`), unboxed arithmetic (`canEmitF64`/`emitExprF64`), FLAT_ARRAY for numeric arrays (≥2 elements), FLOAT_PAIR for 2-element float arrays, AST-level sub inlining (eliminates @_ construction), DerefAV cache for array-ref @_ params and local variables assigned from array derefs, borrow reads for array/hash elements, TBAA metadata for alias disambiguation, PV slab allocator, PerlArray freelist pool. nb.pl n=5M runs in 0.34s vs Perl's ~33s (~97× faster); mbs.pl (1024×1024 Mandelbrot) runs in 6.0s vs Perl's ~72s (~12× faster).

See `README.md` for user-facing documentation and individual test files for usage examples.

**Last Updated**: Current state reflects all features demonstrated in the 69-test suite. Recent additions (this commit): three queued follow-ups to the threads::shared atomic rewrite —
1. **Lock-free 16-byte CAS-on-payload** for the RMW path. The first 16 bytes of `PerlValue` (`{tag, flags, ival/fval/sval/pval}`) are exposed as `PerlValueAtomic16`; `perl_atomic_inc/dec/add` try `__atomic_compare_exchange` (a single `cmpxchg16b` on x86_64 / `ldxp`+`stxp` on aarch64) on the int/float payload, falling back to the lazy-installed SharedMutex for non-numeric tags or after a non-locking failure. `perl_atomic_swap` still uses the mutex (it replaces the full 32-byte cell). Per-thread re-entry tracking extracted into `atomic_mutex_acquire/release` helper. Build flags updated: `-mcx16` for the codegen, `-latomic` to link the libatomic shim for 16-byte `__atomic_*` builtins, `-Wno-atomic-alignment` (the slab allocator guarantees 16-byte alignment; clang's conservative warning is a portability warning).
2. **Named-sub closure capture of shared scalars**. Promoted the AST-level `subs` list to a `CodeGen` member (`subs_`) and added `subCaptures_` (capture list per sub). `case NK::RefSub` now scans the sub body for shared scalars in scope, builds a `PerlClosure` with their cell pointers (via `perl_make_closure` + `perl_array_push_capture`), and `emitSub` installs the captures via `perl_get_capture(i)` at sub entry. The runtime's `clone_code_ref_for_thread` already special-cases shared cells (preserves original pointer), so the spawned thread sees the same cell the parent sees. Test in `tests/threads.pl` exercises 5 threads incrementing via `\&worker_named` (named sub) → 500 race-free.
3. **`our $x : shared` parser+codegen support**. Two changes: the parser's `(LIST)` form (`our ($a, $b) : shared = ...`) now accepts the `: shared` attribute; the codegen's file-scope shared-scalar path now also registers the cell in `fileScalarGlobals_` (under both bare and `Package::name` keys) so cross-package access (`$Foo::counter` from main, `\&Foo::worker` from main) resolves to the same cell. Tests in `tests/threads_atomic.pl` cover 3 forms: bare `our $x : shared`, `our (LIST) : shared`, and cross-package `our $x : shared` in `package Foo` + `\&Foo::bump` dispatch from main.
4. **FLAT_ARRAY 1D ArrowDeref fast path with variable index support**. Fixed LLVM verify error by adding `flatBB` block for FLAT_ARRAY fast path and third PHI incoming. FLAT_ARRAY threshold lowered from 4 to 2 elements (enables cadd/cmul with 2-element arrays). `perl_alloc_float_array(n)` added to runtime.c for zero-initialized FLAT_ARRAY creation. FLOAT_PAIR path now handles variable indices via runtime PHI (selects between re at offset 8 and im at offset 16). FLAT_ARRAY path uses `emitIdx(*n.right)` for variable index GEP.
5. **DerefAV cache for local variables**. When `$local = $cached->[idx]` where `$cached` is a DerefAV-cached @_ param, the PerlArray* is cached for `$local` so inner-loop `$local->[i]` skips repeated `perl_deref_array_ro` calls. Handles both FLAT_ARRAY (direct double* load) and REF_ARRAY (perl_deref_array_ro) paths via PHI.
6. **Stage 32: Loop-invariant PV deferral**. Added `loopInvariantPVs_` stack; `trackPv()` routes `perl_alloc_undef` and `perl_deref_array` results to deferred tracking when inside a loop. `popScope()` skips freeing these PVs; `freeLoopInvariantPVs()` called after loop exit. perl_free calls reduced from 114 to 93 (18% reduction).
7. **Stage 32: Deref hoisting**. Added `loopDerefCache_` stack; `collectDerefTargets()` finds ScalarVars in ArrowDeref; `isVarModified()` checks if variable is written inside loop; `emitHoistedDerefs()` emits `perl_deref_array` before loop and caches result; `emitDerefArray()` checks cache before calling `perl_deref_array`.
8. **Stage 33: Known tag type tracking**. Added `knownTagTypes_` stack; scalar assignments from AnonArray `[float, float]` set tag=13 (FLOAT_PAIR), `[float, ...]` set tag=10 (FLAT_ARRAY); ArrowDeref RHS assignments propagate array element type to LHS; `emitExprF64` ArrowDeref skips tag dispatch when type is known.
9. **Stage 33: Array element type tracking**. Added `arrayElemTypes_` stack; `$arr[i] = [float, ...]` sets element type for array; type propagation through ArrowDeref assignments (`$var = $arr->[idx]`).

TSan verification: `tests/threads_atomic.pl`, `tests/threads.pl`, `tests/destroy.pl` all clean (zero race reports) under `-fsanitize=thread`. (Threading correctness is solid; general correctness is not — see the callout near the top of this file and `TESTS.md`.)

## Memory Safety

- **`perl_cleanup()`**: registered via `atexit()` in `main.cpp`; frees shared-mutex side-table entries (all mutexes/condvars), `perl_plus_hash` (named captures), and XS module list. Valgrind reports zero leaks from runtime internal state.
- **`perl_to_string()`**: refactored to return stable pointers for `PERL_STRING`/`PERL_UNDEF` (no malloc/free). Added `perl_to_string_dup()` for callers needing heap-allocated strings. All ~100 callers use `perl_to_string_dup()`, eliminating leaks from forgotten frees on error paths.
- **`PERL_ALLOC_DEBUG`**: compile-time leak checker for the PV slab allocator. Tracks every `pv_alloc`/`pv_pool_push` with a sentinel value; at exit reports PVs still marked as allocated. Compile with `-DPERL_ALLOC_DEBUG` to enable.

## Known Limitations

- Regex modifier `x` (extended/whitespace-ignoring): not supported
- `exists $h{a}{b}` chained hash subscript without arrow (use `$h{a}->{b}` instead)
- Complex CPAN modules (advanced OO, `our` vars, POD): may trigger parser errors; some scripts may need simplification
- string `eval` (EXPR form): not available (removed with JIT); `eval { BLOCK }` still works for exceptions; REPL removed entirely
- XS is an MVP FFI-style interface, not full Perl XS bootstrap/module compatibility
- DBI support is currently the SQLite subset exercised by the contract tests

## Performance Optimizations

### Benchmarking Framework
- `bench/bench.sh` — standardized benchmarks with CSV logging (`bench/results.csv`)
- Tests: fibn.pl, mbs.pl, nb.pl, regex_heavy.pl
- Supports `-n N` averaging, `--baseline`/`--compare` tracking

### PCRE2 Pattern Cache
- Per-thread LRU cache (max 256 entries) in `runtime.c`
- Keyed by (pattern ‖ flags), uses `__thread` storage (no locking)
- All 5 regex functions check cache before compiling
- Cache entries freed in `perl_cleanup()`

### F64 Fast Path Extensions
- `abs(x)` → `llvm::Intrinsic::fabs` (no PV boxing)
- `int(x)` → floor/ceil select + SIToFP for truncation toward zero
- `length(@arr)` → `perl_array_len_f64()` for DerefAV-cached arrays
- Existing: arithmetic (`+`, `-`, `*`, `/`, `**2`), `sqrt`, comparisons, inlineable subs, 2D ArrowDeref, FLOAT_PAIR, FLAT_ARRAY

### Stage 32: Loop-invariant PV deferral
- Added `loopInvariantPVs_` stack; `trackPv()` routes `perl_alloc_undef` and `perl_deref_array` results to deferred tracking when inside a loop
- `popScope()` skips freeing these PVs; `freeLoopInvariantPVs()` called after loop exit
- perl_free calls reduced from 114 to 93 (18% reduction)

### Stage 32: Deref hoisting
- Added `loopDerefCache_` stack; `collectDerefTargets()` finds ScalarVars in ArrowDeref
- `isVarModified()` checks if variable is written inside loop
- `emitHoistedDerefs()` emits `perl_deref_array` before loop and caches result
- `emitDerefArray()` checks cache before calling `perl_deref_array`

### Stage 33: Known tag type tracking
- Added `knownTagTypes_` stack; scalar assignments from AnonArray `[float, float]` set tag=13 (FLOAT_PAIR), `[float, ...]` set tag=10 (FLAT_ARRAY)
- ArrowDeref RHS assignments propagate array element type to LHS
- `emitExprF64` ArrowDeref skips tag dispatch when type is known

### Stage 33: Array element type tracking
- Added `arrayElemTypes_` stack; `$arr[i] = [float, ...]` sets element type for array
- Type propagation through ArrowDeref assignments (`$var = $arr->[idx]`)

### Benchmark Results (3 runs averaged)
| Benchmark | perlc | Perl | Speedup |
|-----------|-------|------|---------|
| fibn (n=30) | 266ms | 613ms | 2.30x |
| mbs (1024×1024, 80 iters) | 7000ms | 72000ms | 10.29x |
| nb (n=1M) | 60ms | 5813ms | 96.88x |
| regex_heavy (100K items × 50) | 826ms | 240ms | 0.29x |

Note: regex_heavy shows perlc slower than Perl because Perl's regex engine is highly optimized; the PCRE2 cache eliminates redundant `pcre2_compile` calls but the perlc overhead (function calls, PV boxing) still outweighs the benefit for regex-heavy workloads.