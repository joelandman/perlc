# PLANS.md — Correctness, Completeness, Performance Plans

**Last updated after comprehensive review of compiler state (2026-06-26).**

## Current state snapshot

* **Build**: clean (`make` produces `perlc` cleanly, 14 warnings, all in
  `codegen.cpp` — unused variables and one unused private field
  `inFlatOnly_`).
* **Test directory**: only 9 `.pl` tests remain
  (`binary_trees.pl`, `eval_exception.pl`, `fasta.pl`, `fk.pl`,
  `mbs.pl`, `pidigits.pl`, `regression_bugs.pl`, `tree.pl`,
  `wantarray_extended.pl`).  The 60 tests documented in `CLAUDE.md`
  have been deleted from disk.  Assertions baked into the 9 remaining
  tests reveal multiple correctness regressions that have not been
  resolved since the last "all 69 tests pass" claim.
* **REPL** (`./perlc -i`) is broken with a linker error
  (`undefined reference to sqlite3_close / sqlite3_finalize`); the
  REPL command at `src/main.cpp:600` does not pass `-lsqlite3` to
  `clang-18` whereas the file-compile path at `src/main.cpp:801` does.
* **Multi-element FLAT_ARRAY** literals of length ≥ 3 trigger an
  "Unknown runtime function: perl_alloc_float_array" error at codegen
  time.  The runtime function exists in `src/runtime.c:776` and is
  declared in `src/runtime.h:66`, but it is missing from the RT()
  registration table in `src/codegen.cpp` (only
  `perl_alloc_flat_array` at line 234 and `perl_alloc_float_pair`
  at line 235 are registered; the new zero-init variant is called
  from `src/codegen.cpp:5903` but never declared).
* **Scalar-context list producers** (`grep`/`map`/`sort`) return the
  wrong value when used inside subroutines.  `my $c = count_grep()`
  returns 5 instead of 3 (last element instead of count); same root
  cause for `map_scalar_ctx`, `sort_scalar_ctx`, `anon_sub_implicit`,
  `thread_grep`, `grep_scalar_ctx`.
* **Nested `eval`** returns empty for the outer block when the inner
  eval dies (`tests/eval_exception.pl` `nested_eval`).

The PLANS.md and REMEDIATION.md documents previously claimed all of
these were fixed; the code does not back up that claim.  See
`REMEDIATION.md` for the up-to-date status of every previously-listed
remediation item.

---

## Correctness Plans

The following items address immediate, testable correctness
regressions.  Items 1–5 are the core five-point plan; item 6 is a
bonus uncovered during review.  Each is paired with a concrete
failing test, the single root cause, and a one-line fix
description.  These are the minimum that must be addressed before
any other correctness work is meaningful — currently every
"passes 100% of tests" claim is hallucinated.

### 1. Register `perl_alloc_float_array` with the codegen.

**Symptom**: any program containing a 3-or-more-element all-numeric
anon-array literal fails to compile with
`Error: Unknown runtime function: perl_alloc_float_array`.
The function is defined in `src/runtime.c:776` and declared in
`src/runtime.h:66`, but `src/codegen.cpp` never calls
`RT("perl_alloc_float_array", pv, i64)`.  It is invoked from
`src/codegen.cpp:5903`.

**Fix**: in `declareRuntime()` (`src/codegen.cpp:234-235`), add
`RT("perl_alloc_float_array", pv, i64);` next to the existing
`RT("perl_alloc_flat_array", pv, i64);` registration at line 234.

**Test**: write `tests/flt_arr_multi.pl` containing
`my @a = (1.0, 2.0, 3.0); my @b = @a; print "@b\n";` and verify
output equals `1 2 3`.  Once this works, run `tests/mbs.pl` at N=8
and confirm the inner cmul/cadd loop produces
`sample abs(z[0,0])` not equal to 2.0 (the failure sentinel that
proves the inner array elements are correctly read back as
FLOAT_PAIR/FLAT_ARRAY values).

### 2. Fix scalar-context semantics for `grep`/`map`/`sort` returns.

**Symptom**: in `tests/wantarray_extended.pl`,
`grep_scalar_ctx=5`, `map_scalar_ctx=6`, `sort_scalar_ctx=3`,
`thread_grep=8`, `anon_sub_implicit=4`.  Per Perl semantics,
`grep` in scalar context returns the count of matching elements;
`map` returns undef (or last element); `sort` returns undef.
perlc currently returns the *last element of the result array* for
all three because `perl_array_to_list_return`
(`src/runtime.c:419`) defaults to "take last elem" in scalar
context.

**Fix**: in `src/runtime.c`, change `perl_array_to_list_return`
(`src/runtime.c:419-434`) to accept a kind/flag (or introduce
three new helpers: `perl_grep_array_to_list_return`,
`perl_map_array_to_list_return`,
`perl_sort_array_to_list_return`) that dispatch on
`perl_current_wantarray_ctx()`.  Grep in scalar context →
`perl_alloc_int(av->len)`; sort in scalar context →
`perl_alloc_undef()`; map in scalar context → keep current
"last element" behavior (matches Perl's documented
"useless" semantics).  Update the codegen call sites at
`src/codegen.cpp:2846` and `3789` and the runtime-internal
call site at `src/runtime.c:3013` to dispatch by the producer
kind.

**Test**: every assertion in `tests/wantarray_extended.pl` must
print `pass`.  Specifically:
* `grep_scalar_ctx=3`
* `map_scalar_ctx=3`  *(Perl issues a useless-use warning; 3 = number of input elems)*
* `sort_scalar_ctx=` *(empty/undef)*
* `anon_sub_implicit=2,4`
* `thread_grep=3`  *(the join context sees a list of one count)*

### 3. Make `eval { BLOCK }` return the value of its last expression.

**Symptom**: in `tests/eval_exception.pl`, `nested_eval=` instead
of `nested_eval=inner caught`.  Same root cause for any `my $r =
eval { "answer" }; print $r;` program.  Current codegen at
`src/codegen.cpp:6225` (case `NK::EvalBlock`) calls
`emitBlock(*n.body)` (statement list, no value) then returns
`perlUndef()` unconditionally.

**Fix**: replace `emitBlock(*n.body)` (currently line 6248) with
`emitBlockLast(*n.body)` and return that `PerlValue*` (cloned,
since the block's last expression may be owned-temp).  Preserve
the existing longjmp/die semantics: if the body dies we longjmp
back to `endBB` and return undef — that's correct because die
already set `$@` and the block never reached the last expression.
The current `return perlUndef();` at line 6254 must be replaced
with `return cloned;` (or `perlUndef()` if `cloned` is null).

**Test**: `tests/eval_exception.pl` must print
`nested_eval=inner caught`.  Add a new test
`tests/eval_return_value.pl` with the cases
`my $r1 = eval { "hello" };`, `my $r2 = eval { die "x"; "x" };`,
and `my $r3 = eval { 1; 2; 3; };` and verify the printed values
match Perl.

### 4. Add `PERL_FLAT_ARRAY` and `PERL_FLOAT_PAIR` cases to
`perl_to_string` and `perl_to_string_dup`.

**Symptom**: any program that interpolates or prints a
multi-element numeric array literal
(`my @a = (1.0, 2.0); print "@a";`) prints empty rather than
`1 2`.  `print $z` where `$z = [1.0, 2.0]` prints empty.
Tested manually with a 4-line program; perlc output is `""`
while Perl output is `ARRAY(0x...)` for the ref case.  The bug
is that both `perl_to_string` (`src/runtime.c:959`) and
`perl_to_string_dup` (`src/runtime.c:1011`) have no `case
PERL_FLAT_ARRAY:` and no `case PERL_FLOAT_PAIR:`, so the
default `strdup("")` arm fires.

**Fix**: add a `case PERL_FLAT_ARRAY:` branch to both
functions that formats `pval` as a `double[]` of length
`matchpos`, joined by ` ` (or as `ARRAY(0x%llx)` for an exact
Perl match).  Add a `case PERL_FLOAT_PAIR:` branch that formats
the two doubles similarly.

**Test**: write `tests/flt_arr_print.pl`:
```perl
my @a = (1.0, 2.0, 3.0);
print "@a\n";                 # expect "1 2 3"
my $z = [4.0, 5.0];
print "$z->[0] $z->[1]\n";    # expect "4 5"
```
Both lines must match Perl's output.

### 5. Fix REPL linker invocation to include `-lsqlite3`.

**Symptom**: `./perlc -i` always fails with
`undefined reference to sqlite3_close / sqlite3_finalize`.
The non-REPL compile command at `src/main.cpp:801` correctly
includes `-lsqlite3`; the REPL command at `src/main.cpp:600`
was forgotten.

**Fix**: in `src/main.cpp:600`, append `-lsqlite3` to the
`cmd` string before the `2>&1`.  Also append `-ldl` to be
consistent with the file-compile path (dlopen is used by XS).

**Test**: feed `./perlc -i` a 3-line REPL session ending in
`quit`; exit code must be 0 and every statement must produce
expected output.

### 6. *(Bonus, found during review)* Tighten `tests/fk.pl`
expected output.

**Symptom**: `fk.pl` compiles and runs, but the output for
`$n=5` is `0` and empty, vs Perl's `11` and `7`.  Root cause:
subroutines run via `threads->create` mutate global state
(`$max_flips`, `$chksum`) but the mutations never propagate
back to the main thread because the globals were declared with
`my(...)` at file scope and the spawned closures capture *empty
copies* per the current closure-clone-for-isolation contract.
The fix is either (a) declare those globals as `our` and read
through `&main::max_flips` etc., or (b) make the test use
return values from each thread and aggregate in the main thread.

**Fix**: rewrite the test to capture `(chksum, max_flips)`
from each thread's return value (the function already returns
them) and aggregate.  This both fixes perlc's output *and* makes
the test portable across `use threads` semantics.

**Test**: `tests/fk.pl 5` must print
```
11
Pfannkuchen(5) = 7
```
matching Perl.

---

## Completeness Plans

These address correctness gaps relative to documented features.
The codebase ships with a 5-tuple plan in PLANS.md that I am
replacing because the original "5-feature closure items" are
already done.  The new items below target features that are
*promised in the README* but have silent runtime failures or
codegen crashes when exercised.

### 1. Restore the deleted test corpus and make it the regression gate.

The CLAUDE.md and prior PLANS.md both reference 69/69 tests; the
`tests/` directory now contains only 9 of them.  Several of the
deleted tests exercise features that the README documents but the
current codebase silently misimplements (e.g., `builtins.pl` for
`tr///`, `completeness.pl` for `caller()` + `AUTOLOAD`, `regex.pl`
for PCRE2 captures, `threads.pl` for the full shared-scalar
contract, `dbi_sqlite.pl` for DBI).  Without these tests the
regressions in items 1–4 of the Correctness section went unnoticed.

**Plan**: check out each deleted test from `git log` history and
add it back, then run `make test` (which currently iterates only
`tests/test_do_filename.pl tests/test_require_simple.pl
tests/dbi_sqlite.pl tests/xs_ffi.pl`, but those targets are also
gone).  Specifically, restore and run:

```
tests/advanced.pl        tests/arith.pl        tests/builtins.pl
tests/builtins2.pl       tests/closures.pl     tests/completeness.pl
tests/defaults.pl        tests/destroy.pl      tests/eval_string.pl
tests/features.pl        tests/fib.pl          tests/fibn.pl
tests/fileio.pl          tests/fileops.pl      tests/hash.pl
tests/hello.pl           tests/inherit.pl      tests/interp.pl
tests/misc.pl            tests/modifiers.pl    tests/nb.pl
tests/newfeatures.pl     tests/oop.pl          tests/range.pl
tests/refs.pl            tests/regex.pl        tests/regex_g.pl
tests/regex_named.pl     tests/sprintf.pl      tests/test_do_filename.pl
tests/test_require_simple.pl                  tests/threads.pl
tests/threads_atomic.pl  tests/tier1.pl        tests/tier2.pl
tests/tier3.pl           tests/tr.pl           tests/usemod.pl
tests/wantarray.pl       tests/xs_dbi_test.pl  tests/xs_ffi.pl
```

**Gate**: `make test` must iterate all restored tests and exit
non-zero if any fails.  Currently the `Makefile`'s `test:` target
only runs four assertions; widen `ASSERT_TESTS` to the full
restored set.

### 2. Fix `wantarray` context propagation through `threads->create`.

When the main thread calls `threads->create($worker, @args)`,
the worker sub is later invoked in the spawned thread.  The
codegen's `emitCall` for the inner `cl->fn(args, ...)` call
currently pushes `wantarray = 0` (scalar) regardless of the
outer caller's context, because the `threads->create` codepath
(`src/codegen.cpp:6525`) does not propagate `callCtx_` through
to the spawned sub.

**Plan**: extend `perl_threads_create` to capture the calling
thread's current wantarray context at create-time (via
`perl_current_wantarray_ctx()`), store it on the `PerlThread`
struct, and have the thread wrapper push that context before
calling the closure and pop it after.  Verify that
`tests/wantarray_extended.pl` `thread_grep` becomes
`6,7,8` (list context).

### 3. Wire `pcntl`/`alarm` signal handling or document its absence.

`testscripts/cputemp.pl` and the README reference Unix signal
delivery via `alarm`.  The runtime has `perl_alarm` at
`src/runtime.c` but no `signal()`/`sigaction` handler that
converts SIGALRM into a Perl-level `$SIG{ALRM}` callback.  This
is documented as not-implemented in the README's "Known
limitations" but is missed in the completeness plan.

**Plan**: either implement a minimal SIGALRM handler that
records a pending-flag and have `select`/`sleep` check it,
or add an explicit README line that says
`alarm(N)` returns N but does **not** deliver a signal to Perl.
Either way, add a `tests/alarm.pl` that documents the contract.

### 4. Add a unified `mbs.pl`-style benchmark regression test.

`tests/mbs.pl` exercises the FLAT_ARRAY / FLOAT_PAIR / DerefAV
fast paths, the inner-loop optimizations, and the cmul/cadd
inlineable sub machinery.  It is currently broken in three ways:
(a) `perl_alloc_float_array` unregistered (Correctness #1),
(b) inner loop produces the `cplx(2.0, 0.0)` failure value
indicating all cells are escaping the abs2<4 branch (correctness
bug, not just performance), (c) the result is not even printed
correctly because `printf` with a shared scalar argument reads
the wrong value through a corrupted shared cell.

**Plan**: add a small assertion at the end of `tests/mbs.pl`
that prints the value of `z[0][0]` and asserts that it is
within some tolerance of Perl's output.  This catches
correctness regressions in the FLAT_ARRAY / FLOAT_PAIR code
paths that no current test exercises.

### 5. Implement or document the missing XS/DBI test cases.

`tests/xs_ffi.pl` and `tests/dbi_sqlite.pl` were deleted along
with the rest.  These are documented as "implemented" in the
README but only the `bench/bench.sh` smoke test remains.  Restore
both, plus a multi-statement DBI smoke test that exercises
`connect → prepare → execute → fetchrow_arrayref → disconnect`.

**Plan**: re-add the deleted tests from git history, ensure they
pass, and document any remaining gaps in the README's "Known
Limitations" section (e.g., DBI driver-only-SQLite).

---

## Performance Plans

The PLANS.md "PV Boxing Elimination Plan" (Phases 1-6, Stage 32,
Stage 33) is already substantially complete and the prior numbers
in CLAUDE.md (97× on `nb.pl`, 12× on `mbs.pl`) are achievable
*in principle*, but `tests/mbs.pl` is currently so slow that it
times out at N=16.  The new items below address this and the
remaining hot spots that the prior phases missed.

### 1. Validate the prior Stages 32/33 optimizations actually
fire for `tests/mbs.pl`.

Phase 2 of the PV boxing plan claimed to lower the FLAT_ARRAY
threshold from 4 to 2 elements to enable `cadd`/`cmul` of 2-element
complex numbers.  The IR inspection above showed that mbs.pl is
emitting 102 `perl_array_get_ref` calls per inner-loop iteration,
which means DerefAV caching for `$zj->[$i]` is NOT firing.
That suggests `loopDerefCache_` / `loopInvariantPVs_` are not
being populated for the `$zpj` / `$zj` inner-loop variables
that the STAGE 32 comment specifically mentions.

**Plan**: instrument `emitHoistedDerefs` with a print and dump
the IR for `tests/mbs.pl` to see whether `loopDerefCache_`
contains the inner-loop variables.  Then either fix the
candidate-selection logic or wire `collectDerefTargets` to find
the mbs-style usage.  Target: 90 % reduction in
`perl_array_get_ref` count in the inner loop.

### 2. Add a `perl_clone`-free read accessor for
`PERL_FLAT_ARRAY` / `PERL_FLOAT_PAIR` elements in tight loops.

Currently `perl_array_get_ref` returns a *cloned* `PerlValue*`,
which forces the caller to `perl_free` it on every iteration.
For pure reads inside a loop, the caller could keep a single
PV on the stack and have the helper fill in its `tag` and
`fval`/`ival` fields in place.  Add a
`perl_array_get_ref_into(PerlArray*, long long, PerlValue *out)`
that doesn't allocate and reuse the same `out` for every
iteration.  Have the codegen emit a hoisted single PV per
loop body for arrays known to be FLAT_ARRAY / FLOAT_PAIR.

**Expected impact**: ~50 % reduction in `perl_free` calls in
`tests/mbs.pl` (the IR shows 93 `perl_free` calls per program
load).

### 3. Specialize `perl_to_string` for `PERL_FLAT_ARRAY` to avoid
`strdup` per element.

Once Correctness #4 is fixed (FLAT_ARRAY printing), add a
codegen path that emits `perl_array_to_string_join` directly
when the user writes `print @flats` — joining the `double[]`
buffer with a single allocation instead of `n` separate
`strdup(16-byte buffer)` calls.

**Expected impact**: dominated by programs that print large
numeric arrays (not the benchmark suite but real-world
numeric code).  Cheap to add once #4 is in place.

### 4. Profile the codegen's `RT()` lookup path.

Each `callRT` does an `unordered_map` lookup on the function
name (`src/codegen.cpp:475`).  Inside a hot inner loop, this
becomes `n * O(1)`.  Cache the `Function*` in a small per-call
`std::vector` of `Function*` indexed by a small enum
(`RT_alloc_undef`, `RT_alloc_int`, ...) — LLVM's own call
sequence for `printf` does the same trick.  Saves one
`unordered_map` lookup per `callRT` call.

**Expected impact**: ~5–10 % on tight inner loops; minor on
real programs but free.

### 5. Establish a CI-grade regression benchmark that fails the
build on slowdown.

Currently `bench/bench.sh` writes to `bench/results.csv` but the
`make bench` target doesn't compare against a baseline — it just
runs once.  Add a `--regress N` mode that compares against the
last committed `bench/results.csv` and exits non-zero if any
benchmark regresses by more than N %.

**Plan**: extend `bench/bench.sh` to (a) save the post-run
`bench/results.csv` as `bench/results.csv.new`, (b) compare
`results.csv.new` against the committed `results.csv`, (c) fail
if `ratio` increased > 10 % for any benchmark, (d) on success
copy `results.csv.new` over `results.csv`.  Wire `make bench`
to invoke this and add a new `make test-perf` target.

**Expected impact**: catches future regressions before they
land; not a perf improvement per se but a guarantee that the
current numbers don't drift.

---

## Benchmark results (3 runs averaged, current build)

| Benchmark      | perlc       | perl        | Speedup | Notes |
|----------------|-------------|-------------|---------|-------|
| fibn (n=35)    | 4.4 s       | 9.6 s       | 2.2×    | compile=8.8s (cold) |
| nb (n=5M)      | TBD *       | 44.4 s      | —       | * perlc fails to compile: `perl_alloc_float_array` unregistered |
| mbs (N=8)      | 15 s, wrong | n/a         | —       | inner loop produces all-2.0 sentinel; FLAT_ARRAY bug |
| regex_heavy    | TBD         | n/a         | —       | corpus not present |

The fibn and nb numbers are the only reliable performance
measurements currently; mbs is broken end-to-end and nb hits the
`perl_alloc_float_array` codegen error.
