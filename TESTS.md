# perlc — Test Defect Tracking

## Phase Tracking

| Phase | Status | Description |
|-------|--------|-------------|
| Phase 0 | **COMPLETE** | Establish correctness gates |
| Phase 1 | **IN PROGRESS** | Original D1-D16/B1 registry fully re-verified 2026-07-09 (most already fixed or stale/not-applicable — see Defect Registry). 26 new defects found in the same pass (D17-D44 numbering, some IDs retired/merged). **10 OPEN correctness defects prioritized for immediate fix** — see the top-10 list in the project conversation log / next commit's `PLANS.md` update. |

**Note**: Status is also tracked in `PLANS.md`. `REMEDIATION.md` tracks individual fixes with commit references.

---

## Test Infrastructure

### Test Runner: `run_tests.sh`

A comprehensive test runner that provides correctness validation for all test files by comparing compiled binary output against the Perl interpreter.

#### Key Features

- **Automatic correctness validation**: For every `.pl` test file, the runner:
  1. Compiles the file with `perlc`
  2. Runs the Perl interpreter on the original file
  3. Runs the compiled binary
  4. Compares outputs byte-for-byte
  5. Reports PASS/FAIL with diff details

- **Test classification**: Automatically classifies tests as:
  - **Assertion tests**: Have built-in `die()` checks (threads_atomic.pl, xs_ffi.pl, etc.)
  - **Smoke tests**: Print output but no assertions (hello.pl, arith.pl, etc.)
  - **Benchmark tests**: Performance-oriented (fibn.pl, mbs.pl, nbody.pl, fk.pl, bt.pl)
  - **Placeholder tests**: No real testing (xs_dbi_test.pl — skipped)

- **Benchmark result caching**: Long-running benchmark tests are cached per-hostname in `tests.csv`:
  - Before running a benchmark, checks if a cached result exists for the current hostname
  - If found, uses the cached result instead of re-running (saves time on CI/build servers)
  - Cache file format: `hostname,benchmark_test,time_seconds,success,output,accuracy`
  - Cache commands: `--show-cache` (view), `--clear-cache` (clear), `--force-benchmark` (re-run), `--no-benchmark-cache` (disable)
  - Default timeout for benchmarks: 300 seconds (configurable via `--benchmark-timeout`)

- **Flexible modes**:
  - `test-all`: Run all tests with perl output comparison
  - `test-smoke`: Run only smoke/benchmark tests
  - `test-assertion`: Run only assertion tests
  - `--smoke-only` / `--assertion-only`: Filter modes
  - `--skip-compile`: Only run perl interpreter (verify perl works)
  - `--skip-interp`: Only run compiled binary (verify compilation works)
  - `--sort-output`: Sort output lines before comparing (for non-deterministic tests)
  - `--ignore-whitespace`: Ignore trailing whitespace differences
  - `--ignore-exit`: Ignore exit code differences
  - `--verbose`: Show diff details for failures
  - `--dry-run`: Print what would be tested
  - `-j N`: Run N tests in parallel

- **Output status codes**:
  - `PASS`: Compiled output matches perl
  - `FAIL`: General failure
  - `SKIP`: Test skipped (placeholder, filter, etc.)
  - `COMP`: Compilation failed, perl passed
  - `RUNT`: Perl failed, compiled binary passed
  - `MISM`: Both ran but outputs differ
  - `TIME`: Timed out

- **Temporary files**: `/tmp/perlc_test_<name>_*/`
  - `perl_output.txt`, `compiled_output.txt`
  - `perl_stderr.txt`, `compiled_stderr.txt`
  - `diff.txt`, `status.txt`, `compile.log`

#### Usage

```bash
# Run all tests with perl output comparison
./run_tests.sh tests/

# Run with verbose output
./run_tests.sh -v tests/arith.pl

# Run in parallel
./run_tests.sh -j 4 tests/

# Only smoke tests
./run_tests.sh --smoke-only tests/

# Only assertion tests
./run_tests.sh --assertion-only tests/

# Dry run
./run_tests.sh -n tests/
```

#### Makefile Targets

```bash
make test        # Run assertion-based contract tests (fast)
make test-all    # Run all tests with perl output comparison
make test-smoke  # Run only smoke/benchmark tests
make test-assertion  # Run only assertion tests
```

### Test Quality Assessment (Pre-Runner)

Before the test runner was added:

| Category | Count | Description |
|----------|-------|-------------|
| Smoke tests (zero assertions) | 29 | Print output but never verify correctness |
| Light validation (conditional prints) | 9 | Use `print "ok" if $cond` but don't affect exit code |
| Proper assertions (die-on-fail) | 5 | Exit non-zero on failure |
| Placeholders (no-op) | 1 | Always passes |

**The test runner eliminates the smoke test problem by providing external correctness validation for all 29 smoke tests automatically.**

## Defect Registry

**Last full re-verification: 2026-07-09** — every entry below (D1-D16, B1) was empirically re-tested against the current source (not assumed from prior doc state); D17+ are newly discovered in that same pass, together with a 3-way parallel review (defect-registry re-verification, root-cause of the 13 `harness.sh` failures, and a broad probe for the "documented as working but silently wrong in one corner" failure pattern that produced the D1xx-series fixes committed today). Every CRITICAL item below with a repro was independently re-run and confirmed (not just trusted from the sub-agent report) before being recorded here.

Status `FIXED` entries are in `REMEDIATION.md` with commit references.
Status `OPEN` entries are unresolved; severity tiers below double as the fix-priority order.

### Build Environment

| Defect ID | Problem | Status | Notes |
|-----------|---------|--------|-------|
| B1 | LLVM 18 + GCC 15/16 incompatibility — `__normal_iterator` incomplete type errors | **NOT-APPLICABLE** | `make clean && make` builds cleanly on this system with g++ 15.2.0 + LLVM 18 (the `force_complete_std.h` workaround already handles it). Separately: `CLAUDE.md`/`README.md` claim **LLVM 21**/clang-22 while the `Makefile` hardcodes `llvm-config-18`/`clang-18` and `main.cpp` links generated programs via `clang-18` — this is a **documentation bug**, not a build defect; the toolchain is and has been LLVM 18. |

### Critical Defects (fix first — crashes and silent wrong data)

| Defect ID | Problem | Status | Notes |
|-----------|---------|--------|-------|
| D38 | `my ($x, $y) = (10)` — list assignment with fewer RHS values than LHS scalars **segfaults** | **FIXED (2026-07-09)** | Root cause: a single-element parenthesized RHS with no comma parses down to a bare scalar node, not an `ArrayLit` (parser's grouping-parens simplification). The list-assignment codegen's `emitArrayPtr(RHS)` then returned null, and the fallback path passed the resulting `PerlValue*` directly to `perl_array_get_ref()` as if it were a `PerlArray*` — raw type confusion reading garbage memory. Fixed (codegen.cpp ~4264) by wrapping the lone scalar in a real one-element `PerlArray` first, matching the pattern already used by `@arr = RHS` and lvalue-slice assignment elsewhere in the same function (those were already correct; only this call site had the bug). Tests: `tests/list_assign_arity_smoke.pl`, `tests/list_assign_arity.pl` (16 sections), verified byte-for-byte against real Perl. |
| D39 | `my ($a, $b, @rest) = LIST` — trailing array does not collect remaining values | **FIXED (2026-07-10)** | Root cause: `parseMy`'s `my (LIST) = RHS` handling correctly tracked each LHS variable's sigil for its `my` declaration, but when building the *assignment-target* list it unconditionally called `makeScalar()` on every name — stripping the sigil, so `@rest` became a bare `ScalarVar` node named "rest" that codegen treated as a same-named (never-declared) scalar slot; the real `@rest` array stayed empty. Fixed in two places: parser.cpp now preserves the sigil (emits `ArrayVar`/`HashVar` for a trailing `@rest`/`%rest`), and codegen.cpp's list-assignment loop (~4266-4351) now recognizes an `ArrayVar`/`HashVar` LHS target and slurps every remaining RHS element into it via a new `perl_array_extend_from(dst,src,start)` runtime helper (plus `perl_hash_from_list` for the hash case), instead of doing a single per-index scalar assign. Also fixed a related pre-existing bug surfaced while testing: `perl_hash_from_list` silently dropped a trailing unpaired key (odd-length list) instead of assigning it `undef` (real Perl's behavior); now fixed to match. Tests: `tests/list_assign_rest_smoke.pl`, `tests/list_assign_rest.pl` (13 harness-compared sections + the odd-trailing-key case verified manually, since real Perl's accompanying stderr warning can't be suppressed — no `no warnings 'misc'`/`%SIG` support — see the test file's comment), verified byte-for-byte against real Perl. |
| D37 | `foreach`/loop-variable aliasing is entirely absent — mutating `$_` or the loop var does not write back to the source array | **FIXED (2026-07-09)** | Root cause: the general (non-integer-range) `foreach` codegen allocated one stable `PerlValue*` cell for the loop var before the loop, then each iteration cloned the current element (`perl_array_get`, which calls `perl_clone`) and copied that clone's value into the stable cell via `perl_assign` — the loop var was always a private copy. Fixed (codegen.cpp ~3506-3538) by borrowing the array's own element pointer each iteration (`perl_array_get_ref`, no clone) and storing that pointer into the loop var's alloca every iteration instead of copying a value into a fixed cell; any mutation now writes through to the array's own cell. Existing per-iteration closure-capture isolation (`foreach my $x (@a) { push @subs, sub { $x } }`) is unaffected — it already relied on `perl_array_push_capture` cloning the value at capture time, not on loop-var cell identity, so it stays correct with either implementation. **Not fixed as part of this**, logged as a new low-priority gap: real Perl raises "Modification of a read-only value attempted" when the loop var aliases a literal-list element (`foreach my $x (1,2,3) { $x *= 2 }`); perlc now silently allows the mutation instead (discarded along with the temporary list, so not a correctness risk — just a missing diagnostic). Tests: `tests/foreach_aliasing_smoke.pl`, `tests/foreach_aliasing.pl` (12 sections: `$_`/named-var/string/nested/hashref-element aliasing, next/last interaction, empty/single-element arrays, closure isolation, post-loop growth, sequential loops), verified byte-for-byte against real Perl. Also TSan-clean (`threads.pl`/`threads_atomic.pl`/`destroy.pl` use `foreach` heavily). |
| D40 | 3+ level chained hash/array autovivification silently fails on both read and write | **FIXED (2026-07-10)** | Root cause: the `ArrowDeref`-assignment codegen (`$ref->[i]=val`/`$ref->{k}=val`, with autoviv) only special-cased a base that was exactly one `HashElem`/`ArrayElem` level (`$h{k}[i]=val`, `$a[i]{k}=val`). A base that was itself another `ArrowDeref` — exactly what a 3+ level chain produces (`$h{a}{b}{c}` parses as `ArrowDeref(ArrowDeref(HashElem(h,a),"b"),"c")`) — fell through to the generic non-autovivifying fallback (`emitExpr` + `perl_deref_hash`/`perl_deref_array`), which silently produced a fresh, disconnected, immediately-discarded container when the middle level didn't exist yet. Fixed with a new recursive `emitAutovivContainer()` helper (codegen.cpp) that walks an arbitrary-depth `HashElem`/`ArrayElem`/`ArrowDeref` chain, autovivifying every missing intermediate level via the existing `perl_(hash\|array)_autoviv_(hash\|array)[_sv]` runtime primitives. **Scoping fix mid-implementation**: the first version of this fix intercepted *any* `ArrowDeref` base, which broke `nb.pl`/`nbody.pl` (segfault) — a scalar-ref-rooted chain like `$bodies->[0][3]` was already correctly handled by a pre-existing FLAT_ARRAY-aware fallback, and the `perl_array_autoviv_array`/`perl_hash_autoviv_hash` runtime helpers only recognize the `PERL_REF_ARRAY`/`PERL_REF_HASH` tags — routing a FLAT_ARRAY-tagged inner array through them silently destroyed it. Fixed by adding an `isElemRootedChain()` check so the new recursive path only activates for chains genuinely rooted in a `%hash`/`@array` element; a scalar/ref-rooted chain keeps using the original FLAT_ARRAY-aware fallback untouched. **Separate, still-open gap found and NOT fixed here** (logged as **D50**): `$ref->{a}{b} = val` starting from an *existing* scalar ref (e.g. `my $ref = {}; $ref->{a}{b}=1;`) still silently fails — that's the plain-deref fallback branch (scalar/ref-rooted, not element-rooted), a different code path than what this fix touches. Tests: `tests/autoviv_chain_smoke.pl`, `tests/autoviv_chain.pl` (14 sections: 2-5 level chains, mixed hash/array nesting in all orderings, sibling-branch independence, missing-path reads, `ref()` checks, and an explicit `nb.pl`-style FLAT_ARRAY regression guard), verified byte-for-byte against real Perl. |
| D22 | `sort` silently returns an empty list for any argument shape beyond its few special-cased forms (`sort grep{...}@arr`, `sort map{...}@x`, `sort some_func()`) | **FIXED (2026-07-10)** | Root cause: the parser's `sort` handling (parser.cpp ~2031-2091) only recognized `sort keys/values %h`, `sort @arr`, `sort (LIST)`, `sort qw(...)` — anything else (grep{}/map{}/reverse/function-call results, nested sort chains) fell through with `elems` never populated, silently becoming `sort()`. Fixed by adding a final fallback: parse any other argument as a single general expression into `n->left` (the same slot already used for `sort keys %h`/`sort @arr`) — no codegen changes needed, since `emitArrayPtr()` already has cases for `GrepFunc`/`MapFunc`/`Call`/`ReverseFunc`/etc. that correctly turn any of those into a `PerlArray*`. **Scoping note**: `sort BAREWORD(args)` (e.g. `sort get_nums()`) is deliberately NOT covered — real Perl itself treats a bareword immediately followed by `(` in this position as the `sort SUBNAME LIST` comparator form (see D42), a separate, genuinely ambiguous grammar case, not "call it and sort the result" — confirmed by testing directly against real Perl, which gives surprising (non-call) output for that exact shape too. Tests: `tests/sort_list_expr_smoke.pl`, `tests/sort_list_expr.pl` (12 sections: grep/map/reverse arguments, custom comparator + grep, nested sort/map chains, inline in join/print, empty-result case, regressions for the pre-existing forms), verified byte-for-byte against real Perl. |
| D42 | `sort SUBNAME LIST` (named comparator sub, no braces) is broken — silently empty, or a hard parse error inside a nested call | **OPEN** | `sort by_name @words` (named comparator function, the standard alternative to a `{ }` block) returns nothing; `join(",", sort by_name @words)` is a parse error. Same family as D22 but a distinct code path. |
| D23 | Regex literals collapse `\\` (escaped backslash) into a single `\`, silently changing match semantics | **OPEN** | The lexer's `readRegex()` (lexer.cpp:158-164) rewrites `\\d` in source to `\d` before handing the pattern to PCRE2. Real Perl passes two literal characters (backslash, `d`) through; perlc turns it into the `\d` digit metaclass. Any pattern containing a literal backslash silently matches differently, with no error. |
| D25 | `return` inside a nested `eval{}` with no enclosing named sub compiles to `perl_die`, corrupting `$@` and control flow | **OPEN** | `case NK::Return` (codegen.cpp:3608-3619) assumes "in `main`, no enclosing sub" and always emits `perl_die(v)`. But `return` inside `eval{}` should exit just that eval with `v` as its value — a common early-exit-from-eval idiom. Confirmed: turns the intended return value into a die message that gets caught by the *outer* eval instead. |
| D35 | `Carp::croak`/`confess` call `exit(1)` directly, bypassing `eval` entirely — cannot be caught, kills the whole process | **OPEN** | `perl_carp_croak()` (runtime.c:5119) calls `exit(1)` instead of raising a catchable exception. The standard Carp usage pattern, `eval { croak(...) }` to convert a library error into a catchable exception, kills the entire process under perlc instead of setting `$@`. |
| D36 | Any unrecognized bareword call without parentheses (`foo "hello";`) is silently parsed as two no-op statements — zero diagnostics | **OPEN** | This is *why* `croak "msg"` / `carp "msg"` (idiomatic, no-parens Carp style) silently vanish — croak/carp aren't lexer keywords, so `croak "boom";` parses as bareword `croak` followed by a discarded string literal, with no call emitted and no error raised. Broader than Carp: any bareword-call-without-parens to an unrecognized name is swallowed silently. |
| D9 | `floatSqrtOf_` cache (Stage 30: `v*v → x` rewrite for values assigned from `sqrt()`) is keyed only by variable name and never cleared, leaking across unrelated subs | **OPEN** | Confirmed with `sub f1 { my $a = sqrt(4); return $a } sub f2 { my $a = 10.0; return $a*$a }` — `f2()` returns `4` instead of `100`; the stale sqrt-input from `f1`'s `$a` bleeds into `f2`'s unrelated `$a*$a` because the cache is never invalidated across function boundaries. Silently wrong numeric result, not a crash. |

### High Severity Defects

| Defect ID | Problem | Status | Notes |
|-----------|---------|--------|-------|
| D24 | `do FILE` / runtime `require` of a dynamic file is a **non-functional no-op stub** — never actually parses or executes the target | **OPEN** | `perl_eval_loaded_code()` (runtime.c:589-598) contains a comment admitting the file is not actually parsed/executed post-JIT-removal, and unconditionally returns fake success after only checking the file is readable. `CLAUDE.md`'s claim that `do FILE` "executes file each time" is **inaccurate** — 4 of 7 checks in `test_do_filename.pl` fail because of this. |
| D44 | `tie`/`untie`: `TIESCALAR`/`STORE`/`FETCH` interception does not happen — tied variables behave as plain scalars | **OPEN** | Verified directly with a `Doubler` tie class (`STORE` doubles the value): real Perl prints `x=10`, perlc prints `x=5` — `STORE`/`FETCH` are never actually invoked on read/write, only `TIESCALAR` runs (at `tie` time). `CLAUDE.md`'s top-level feature bullet ("tie/untie ... support") is misleading without its own buried caveat ("FETCH/STORE interception not yet implemented") being visible at the point of the claim. Makes `tie` non-functional for its core purpose. `README.md` separately (and more accurately, if bluntly) says "tie/untie: not implemented" — the two docs contradict each other; this entry is the reconciled truth. |
| D38a | `s/pat/repl/` with alternate delimiters (`s{pat}{repl}`, `s#pat#repl#`, etc.) is not lexed at all | **OPEN** | `readSubst()` (lexer.cpp) is only invoked when the token is literally `s/`; every other delimiter falls through to identifier parsing and produces "Expected /regex/ or s/// or tr/// after =~". A common Perl style (especially for patterns that contain `/`) is entirely unsupported. |
| D38b | `s/$/text/` (or other empty/anchor-only matches) **segfaults** | **FIXED (2026-07-09)** | Root cause: `perl_regex_subst`'s zero-length-match handling (runtime.c ~4441) copied `s[pos]` (the old search-start) instead of `s[mstart]` (the actual match position) — wrong for an anchor like `$` that matches ahead of `pos` (at end-of-string). This left `pos = mstart+1 = slen+1` after a match exactly at end-of-string, and the following `size_t rem = slen - pos` (both unsigned) underflowed to ~SIZE_MAX, driving a multi-exabyte `memcpy`/`realloc` that corrupted the heap — the segfault surfaced later, inside `pcre2_match_data_free()`, once the corruption reached PCRE2's own bookkeeping. This is what the original registry's vague "npos underflow" description was actually pointing at. Fixed: use `mstart` for the zero-width-match char copy, and clamp the final `rem` computation so it can't underflow. Tests: `tests/regex_subst_zero_width_smoke.pl`, `tests/regex_subst_zero_width.pl` (17 sections: end/start anchors × global/non-global, empty/single-char subjects, lookahead, empty-pattern-every-position, `/mg`, embedded newlines, return-value-is-count, array-element targets), verified byte-for-byte against real Perl. |
| D38c | `s///e` does not evaluate the replacement as Perl code — just interpolates captures literally | **OPEN** | `s/(\d)\+(\d)/$1+$2/e` gives `"2+3"` under perlc instead of real Perl's `"5"`. `CLAUDE.md` explicitly and repeatedly claims `/e` "evaluates replacement as Perl expression" — it does not; it behaves identically to `/e`-less substitution. |
| D8a | ``EXPR or return VALUE`` parses successfully but does not actually return — falls through as a dead value-producing expression | **OPEN** | Semantic bug, not a parse error: `f() or return "X";` compiles, but execution continues to the next statement instead of returning. Confirmed: real Perl returns early with `"X"`; perlc does not. |
| D8b | `EXPR or printf(...)` fails to parse | **OPEN** | `parseOrRhs`'s keyword whitelist (parser.cpp:933-953) covers `return/die/warn/last/next/redo/print/say/push/unshift` but omits `printf`, producing a hard parse error. |
| D12 | `wantarray` context is not propagated into `print`/`printf` sub-call arguments | **OPEN** | Confirmed: `print ctx(), "\n"` calls `ctx()` in scalar context under perlc regardless of `print`'s actual (list) context; real Perl correctly propagates list context. 3 of 4 constructed test lines diverge from real Perl. |
| D34 | `defined EXPR` without surrounding parens fails to parse (`defined $x`, not just `defined($x)`) | **OPEN** | One of the single most common Perl idioms; a bare `if (defined $x)` is a hard parse error under perlc. |
| D41 | `local $h{key}` / `local $arr[idx]` (element-level `local`) not supported | **OPEN** | Parse error. `local` on a whole array/hash/scalar works (D3, fixed); `local` on one element of a hash/array does not. |

### Medium Severity Defects

| Defect ID | Problem | Status | Notes |
|-----------|---------|--------|-------|
| D26 | `use constant` from an inlined module is exposed as a global sub, ignoring package scoping / `@EXPORT` | **OPEN** | `inlineModules()` (main.cpp:298-320, 466-469) textually rewrites any `use constant NAME => VAL` anywhere in the token stream — including inside inlined `.pm` files — into a global sub with no package/export filtering. Constants defined but not exported by a module silently become visible to the importer. |
| D27 | `system()` returns the plain exit code instead of Perl's shifted wait-status word | **OPEN** | `perl_system` (runtime.c:4706-4711) already unwraps with `WEXITSTATUS`; real Perl's documented idiom is `$rc >> 8` on the raw status. Code following that idiom computes wrong values under perlc. |
| D28 | `sort`/`reduce` comparator blocks always rebind `$a`/`$b` to fresh locals, ignoring an outer lexical `my $a`/`my $b` that should shadow them | **OPEN** | Narrow: only triggers on the well-known "my $a used in sort comparison" Perl footgun (real Perl warns and produces a broken/non-monotonic sort in this case; perlc instead produces the "sensible" result, which is a *behavioral* divergence even though it looks like an improvement). |
| D29 | `sort` in scalar context returns element count instead of `undef` | **OPEN** | codegen.cpp:3588 (and duplicated at :2611, :2635) treats `SortFunc` like `GrepFunc`/`MapFunc` for scalar-context list-producer handling. Real Perl's scalar-context `sort` is documented as unsupported/undefined and returns `undef`, unlike grep/map. |
| D30 | `Time::HiRes` not implemented | **OPEN** | No stub/implementation exists; `use Time::HiRes qw(time)` is silently ignored the way unrecognized pragmas are, so `time()` stays integer-second resolution. Commonly used for benchmarking/timing. |
| D14 | Duplicated `cmpOps` array in two places in parser.cpp | **OPEN** | Confirmed still duplicated verbatim at parser.cpp:241 and parser.cpp:683. |
| D45 | Named `sub` declared *inside* a bare `{ }` block that follows another bare block resolves to the wrong value at its call site | **OPEN** | Found while writing regression tests for D38 (not yet root-caused). Repro: `{ my $z=1; } { sub f { my ($x,$y)=(@_); return "$x:$y" } ... }` two-bare-blocks-deep gives a wrong/empty result for `f(...)`; declaring the sub at file scope (outside any block) — including *before* the same preceding block — works correctly. Narrow trigger (needs ≥2 preceding sibling blocks in some configurations); avoided in new tests by declaring subs at file scope, which is idiomatic anyway. |
| D46 | Ternary with no space before `:` fails to parse when the true-branch is a bare scalar var: `$x?$y:"str"` | **OPEN** | Found while writing regression tests for D38. `(defined($x) ? $x : "nope")` parses fine; `(defined($x)?$x:"nope")` (no spaces) gives "Parse error: expected : but got 'nope'" — looks like a lexer tokenization issue where `$y:` with no space is mis-scanned. Minor/easily worked around (use spaces), but worth fixing since terse ternaries without spaces are common style. |
| D48 | `no PRAGMA ...;` (e.g. `no warnings 'misc';`) is a hard parse error, not silently ignored like `use PRAGMA;` | **OPEN** | Found while writing regression tests for D39 — needed to suppress real Perl's "Odd number of elements in hash assignment" warning for a deliberately-odd test input, but perlc doesn't parse the `no` form of a pragma statement at all (`use` pragmas are silently accepted/ignored per existing docs; `no` isn't handled the same way). Low priority (moot until a warnings system exists to have anything to suppress), but worth a one-line parser fix (treat `no IDENT ...;` the same inert way as unrecognized `use IDENT ...;`). |
| D49 | `%SIG` is not implemented — `$SIG{__WARN__} = sub {...}` is a hard parse error | **OPEN** | Found alongside D48 while looking for an alternative way to suppress a warning in a test. Not in `CLAUDE.md`'s feature list at all (signals generally aren't implemented, consistent with "Not Yet Implemented: ... signals"), so this is an expected gap, not a regression — logged here mainly as a cross-reference since it was hit in the same investigation. |
| D50 | `$ref->{a}{b} = val` (chained autoviv starting from an *existing* scalar ref, not a hash/array element) silently fails | **OPEN** | Found while fixing D40. `my $ref = {}; $ref->{a}{b} = 1; print $ref->{a}{b};` prints empty under perlc (real Perl: `1`). Distinct code path from D40 (which is now fixed): the base of the outer `ArrowDeref` here is another `ArrowDeref` rooted in a bare scalar variable, not a `HashElem`/`ArrayElem`, so it takes the plain-deref fallback (`emitExpr` + `perl_deref_hash`, no autoviv) rather than the new `emitAutovivContainer()` path — deliberately, since that fallback is also what correctly handles `nb.pl`-style FLAT_ARRAY-tagged scalar-ref chains (see D40's note), and naively extending autoviv to all `ArrowDeref` bases regressed that. A correct fix needs the fallback to autoviv only when the *existing* value is genuinely missing/undef (not when it's a FLAT_ARRAY it shouldn't touch) — more invasive, deferred. |

### Low / Cosmetic Defects

| Defect ID | Problem | Status | Notes |
|-----------|---------|--------|-------|
| D31 | Float-to-string formatting uses C's default `%g` (6 significant digits) instead of Perl's `%.15g` | **OPEN** | `10/3` prints `3.33333` under perlc vs Perl's `3.33333333333333`. Affects all default float stringification (print/interpolation) — cosmetic but broad-reaching precision loss. runtime.c:975, :1028. |
| D32 | `$.` (input line number) is not reset to 0 when its filehandle is closed | **OPEN** | `perl_close_fh` (runtime.c:3199-3204) never touches `$.`. |
| D33 | `Scalar::Util::looks_like_number` returns integer `0` for false instead of Perl's empty string `""` | **OPEN** | runtime.c:5095-5113. Logically equivalent in boolean context, but string interpolation output differs. |
| D43 | `wantarray()` called at top level (outside any sub) returns `0` instead of `undef` | **OPEN** | Minor context-value mismatch. |
| — | No `use warnings` diagnostic system exists at all | **OPEN** (completeness, not correctness) | Explains 2 of the 13 `harness.sh` diffs (`advanced.pl`, `fileio.pl` — both differ from real Perl only in the absence of stderr warning lines perlc never emits). Affects debuggability of every perlc-compiled program, not just these two tests. |

### Resolved / Not Applicable

| Defect ID | Problem | Status | Notes |
|-----------|---------|--------|-------|
| D1 / D11 | Compound assignment (`-=`,`*=`,`/=`,`%=`) on shared scalars used the wrong op | **FIXED** | Re-verified directly: all four ops give correct results (90/30/5/2) on a `: shared` scalar across a thread boundary. |
| D2 | `chop @arr` behaved like `chomp @arr` | **FIXED** | Re-verified: `chop @a` removes the last char of every element, matches real Perl exactly. |
| D3 | `local @arr`/`local %hash` no-op for function-scope vars | **FIXED** | Re-verified including restore-on-`die`/`eval`-unwind. |
| D5 | Closure + range-with-captured-variable emitted `undef` bound | **FIXED** | Re-verified with 2 and 3-level nested closures. |
| D6 | `for (my $i = 0; ...)` C-style init claimed dead code | **STALE — was already fixed**, doc not updated | Re-verified 4 variants including pre-declared-var and multi-clause comma forms (`for (my $a=0, my $b=5; $a<3; $a++,$b--)`, added this session) — all correct. |
| D13 | DESTROY handling "unclear if fully functional" | **FIXED / confirmed working** | `tests/destroy.pl` output is byte-identical to real Perl. |
| D15 | `specialVars` list duplicated in codegen.cpp | **FIXED / stale** | Only one definition exists now (codegen.cpp:1617-1618). |
| D16 | `xs_dbi_test.pl` no-op placeholder | **NOT-APPLICABLE** | File no longer exists; superseded by `tests/dbi_sqlite.pl` and `tests/xs_ffi.pl`, both of which have real `check()`-based assertions. |
| D10 | Static counters don't reset between compilations | **NOT-APPLICABLE** | `main.cpp` processes exactly one input file per process invocation (no REPL/multi-file loop exists post-JIT-removal) — counters can't leak across compilations because there is never more than one per process. |
| D18-D21, D7 (original number, retired) | Renumbered/split into D38a/D38b/D38c (substitution bugs) and D8a/D8b (or-RHS bugs) above with concrete repros, replacing the original vague "npos underflow"/"small subset of keywords" descriptions. | — | — |

### This session's fixes (2026-07-08/09, already committed: `b403168`, `fa5961b`)

These were found and fixed while adding C-style `for`-loop comma-operator support (`for ($i=0,$j=10; ...; $i++,$j--)`) — all three were pre-existing and independent of that feature, just newly exposed by it:

- **Unboxed-int `pre--`/`post--` fast path double-negated the decrement** (`cur - (-1)` instead of `cur - 1`) — wrong results and even infinite loops for `$j--` outside the single-item `for`-step fast path. Fixed in `src/codegen.cpp` (~line 4162).
- **`AnonSub` closures never captured `@arrays`/`%hashes`** from an enclosing block, only scalars — `push @log,...` inside a closure over a block-scoped array silently wrote to a detached copy. Fixed by capturing every block-scoped array/hash visible at closure-creation time.
- **`next LABEL`/`last LABEL` silently no-op'd on C-style `for` loops** (only `foreach` registered into `loopLabels_`) — corrupted block control flow. Fixed by registering `for` loops into `loopLabels_` the same way `foreach` does.
- **`make test-tsan` failed to link** — `TSAN_OBJS` in the `Makefile` was missing `ast_tsan.o`/`llvm_early_init_tsan.o` (present in the regular `OBJS`). Fixed; TSan build now clean with zero race reports on `threads.pl`/`threads_atomic.pl`/`destroy.pl`.

---

## Test Coverage Gaps

Re-verified 2026-07-09. Of the original 10 "claimed but untested" items, **9 are confirmed correctly implemented** (smoke + deep tests still need writing, per the testing policy below) and **1 (Carp) is confirmed broken** — see D35/D36 above.

| Feature | Status | Test Needed |
|---------|--------|-------------|
| `@{$href}{LIST}` hash-ref slices | Confirmed working | Smoke + deep test |
| `@{$aref}[LIST]` array-ref slices | Confirmed working | Smoke + deep test |
| `scalar(@{$ref})` | Confirmed working | Smoke + deep test |
| `$h{k}++` on missing keys | Confirmed working | Smoke + deep test |
| lvalue slices `@arr[i,j] = list` / `@h{qw(a b)} = list` | Confirmed working | Smoke + deep test |
| `map { @$_ } @aoa` flattening | Confirmed working | Smoke + deep test |
| `next`/`last` with labels | Confirmed working (`for`/`foreach`/`while`, all mixed-nesting combos) | Smoke + deep test (extend `modifiers.pl`) |
| `$Pkg::arr` / `$Pkg::hash` cross-package | Confirmed working | Smoke + deep test |
| `use parent -norequire` | Confirmed working | Smoke + deep test (extend `inherit.pl`) |
| Carp module (`croak`/`carp`/`confess`/`cluck`) | **Confirmed BROKEN** — see D35, D36 | Blocked on the fix; write regression test alongside it |

### Testing policy going forward

Per project direction: every fix should ship with (1) a **smoke test** that just confirms the feature/fix exists and does the basic right thing, and (2) a **deep test** that explores edge cases and interactions as comprehensively as practical — both verified byte-for-byte against real Perl output via `tests/harness.sh`, not just self-checking assertions in isolation. See `tests/comma_operator_smoke.pl` / `tests/comma_operator.pl` (added 2026-07-08) for the established pattern.

---

## Recommended Development Workflow

**Harness-as-Gate Policy**: `make test-all` is the mandatory pre-commit gate. Run it before every commit.

```bash
# 1. Build the compiler
make clean && make

# 2. Run ALL correctness tests (harness gate — mandatory)
make test-all

# 3. If benchmarks are cached, results are reused. If not, they are recorded.
#    To force re-run of benchmarks after a significant change:
make test-all -- --force-benchmark

# 4. To view cached benchmark results:
make test-cache

# 5. To clear cache and force fresh runs:
make test-clear-cache

# 6. Safety gates before merging
make test-valgrind    # memory safety
make test-tsan        # data race detection (threads/destroy)
make test-tsan-full   # broader TSan coverage
```

### When to Use `--force-benchmark`
- After fixing a bug that could affect benchmark correctness
- After optimizing code that changes algorithmic behavior
- When adding new features that might impact performance
- When the cache is stale (e.g., after hardware changes)

### When Cache Is Sufficient
- Small fixes to unrelated code paths
- Bug fixes that don't affect numerical output
- Documentation/comment changes
- Build system changes (verify with `make test` first)
