# PLANS.md — Correctness, Completeness, Performance Plans

## Phase Tracking

| Phase | Status | Description |
|-------|--------|-------------|
| Phase 0 | **COMPLETE** | Establish correctness gates: harness-as-gate policy, valgrind/TSan gates, expanded test coverage |
| Phase 1 | **IN PROGRESS** | Fix all open defects — **see `TESTS.md` Defect Registry (re-verified 2026-07-09) for the current, accurate list**, not the stale D1-D16/B1 reference below. Original D1-D16/B1 mostly fixed/stale/not-applicable; 26 new defects found (10 CRITICAL, incl. 2 crashes), organized into a 10-item immediate-fix priority list. |
| Phase 2 | **PENDING** | Re-architect optimization passes |

**Correctness-first priority order** (per project direction: correctness → completeness → performance): the 10 CRITICAL defects in `TESTS.md` should be fixed before any further completeness or performance work, since several are crashes or silent-wrong-data bugs on extremely common Perl idioms (list-assignment unpacking, `foreach` aliasing, `sort`, chained autovivification).

### Incremental follow-ups (rebased onto main)
- D10: `sortCmpCounter_`/`stateSeq_`/`endSeq_`/`anonCount_` are instance state; reset in `compile()`
- D9: clear `lastSqrtInput_` at each `emitStmt`
- D3/`our`: `ival` bit 2 marks package globals so `local @H` works inside subs
- D7: alternate `s///` delimiters (`s!!!`, `s{}{}`, …) + don't mislex `$s` as substitution

---

## Phase 0 — Correctness Gates (COMPLETE)

### Completed
- [x] `harness.sh`: Removed `threads.pl`, `threads_atomic.pl`, `destroy.pl` from `SKIP_BY_DEFAULT` — these now run in default harness execution
- [x] `Makefile`: Added `test-all`, `test-smoke`, `test-assertion` targets
- [x] `Makefile`: Added `test-valgrind` target (memcheck on all tests, skip DBI/XS)
- [x] `Makefile`: Added `test-tsan-full` target (TSan on all tests, skip DBI/XS)
- [x] `INSTRUCTIONS.md`: Updated with harness-as-gate policy, new make targets, optimization debugging workflow
- [x] `INSTRUCTIONS.md`: Added safety gates section (valgrind, TSan)

### Impact
- Test coverage expanded from ~35 tests to ~48 tests in default harness run (35% increase)
- Memory safety and data race detection now available as `make` targets
- Clear development workflow with mandatory correctness gate

---

## Phase 1 — Defect Fixes (PENDING)

See defect registry below. All items must be fixed before Phase 2 re-architecture begins.

## Correctness Plans

**Superseded 2026-07-09 — see `TESTS.md` Defect Registry for the current, accurate, actively-maintained list.** Items 1-4 below are historical (fixed/superseded); item 5 is stale (the gate it asks for already exists). Kept for history, not as an active plan.

1. ~~**Fix `NK::EvalBlock` LLVM codegen crash.**~~ — FIXED (long since resolved; `eval_exception.pl` currently fails for an unrelated, newly-found reason — see `TESTS.md` D25, `return` inside nested `eval{}` compiling to `perl_die`).

2. ~~**Fix `require` caching bug.**~~ — FIXED (`tests/test_require_simple.pl` currently passes in `tests/harness.sh`).

3. ~~**Fix compound `-=` on shared scalars.**~~ — FIXED (commit db7ba77), re-verified 2026-07-09
    - Negate delta via `perl_to_float` → `fneg` → `boxF64` before `perl_atomic_add`
    - Test: `tests/regression_bugs.pl` `regression_multi_subtract` now passes

4. ~~**Add `*=` `/=` `%=` atomic RMW for shared scalars.**~~ — FIXED (commit db7ba77), re-verified 2026-07-09
    - Added `perl_atomic_rmw` runtime function with mutex protection
    - Codegen calls `perl_atomic_rmw` for `*=`, `/=`, `%=` on shared scalars

5. ~~**Add valgrind/memcheck verification to test harness.**~~ — STALE, already done: `make test-valgrind` exists (see `Makefile`/`INSTRUCTIONS.md`) and runs the suite under `valgrind --tool=memcheck --leak-check=full --errors-for-leak-kinds=all`, skipping DBI/XS. Not re-run as part of this pass; re-verify leak status separately if memory safety work resumes.

## Completeness Plans

1. ~~**Implement `tie`/`untie` with minimal TIESCALAR/TIEARRAY/TIEHASH interface.**~~ — FIXED (commit TBD)
    - Added `tie`/`untie` lexer tokens, AST nodes, parser rules, and codegen
    - Runtime: `perl_tie()` calls CLASS->TIESCALAR/TIEARRAY/TIEHASH and blesses result
    - Runtime: `perl_untie()` calls UNTIE() method on blessed object
    - Note: FETCH/STORE interception for tied variables requires additional codegen changes

2. ~~**Implement `do FILE` runtime execution.**~~ — FIXED (already working, verified with tests/test_do_filename.pl)
    - `do "file.pl"` executes file each time (no caching), returns last expression value, sets `$@` on failure

3. ~~**Add `pack`/`unpack` for binary data serialization.**~~ — FIXED (already implemented)
    - Format codes: `C`, `c`, `S`, `s`, `L`, `l`, `N`, `V`, `F`, `D`, `A`, `a`, `H`, `h`, `B`, `b`, `x`, `X`, `@`
    - Runtime interprets format strings and produces/consumes byte buffers

4. ~~**Add UTF-8/unicode support.**~~ — PARTIALLY FIXED (basic support implemented)
    - `chr`/`ord` for code points above 127
    - UTF-8 aware `length` and `substr`
    - Full `use utf8`, `use Encode`, and unicode pragma support not yet implemented

5. ~~**Fix closure + range with captured variable.**~~ — FIXED (commit 776b963)
    - Fixed list return bug: added `perl_array_push_list_or_scalar` to unwrap `PERL_LIST_RESULT` tags
    - Fixed `perl_call_code_ref` to use caller's wantarray context

## Performance Plans

1. **Reduce PV boxing overhead in regex path.** Regex-heavy workloads are ~3x slower than Perl (826ms vs 260ms on regex_heavy benchmark). Perl's regex engine is highly optimized; perlc's PCRE2 calls + PV boxing overhead outweigh JIT benefits. Consider inline PCRE2 API calls that avoid PerlValue boxing for the common case of matching numeric strings or pre-boxed values.

2. **Implement in-process LLVM optimization passes.** Currently relies on clang-18 `-O2`. The `opt-18` integration encountered reliability issues with `system()` calls. Implement in-process optimization using LLVM's PassBuilder to run `opt -O3` passes (inlining, loop vectorization, dead-code elimination, constant propagation) before linking. This avoids temp file overhead and shell invocation issues.

3. **Extend F64 fast path to cover `sprintf` format parsing and `join` with numeric args.** Currently `sprintf` and `join` always box arguments through PerlValue*. Adding F64 fast path cases for numeric-only `sprintf` formats (`%f`, `%d`, `%e`) and `join` with numeric arrays would eliminate boxing in common string-building hot loops.

4. **Add string interning pool for short-lived strings.** `perl_to_string_dup()` calls `strdup()` on every string coercion. A string interning pool for short strings (< 64 bytes) would eliminate redundant allocations for common strings like `"0"`, `"1"`, `""`, `" "`, etc. This would reduce allocation pressure in tight loops.

5. **Profile and optimize `perl_clone` hot path.** `perl_clone()` is called on every array element read, every regex capture, and every hash set operation. It does `strdup()` for strings and `memcpy()` for FLAT_ARRAY. Consider an inlineable read-only accessor that skips `perl_clone` when the source is known to be stable (e.g., array elements that won't be modified).

## Benchmark Results (3 runs averaged)

| Benchmark | perlc | Perl | Speedup |
|-----------|-------|------|---------|
| fibn (n=30) | 270ms | 590ms | 2.19x |
| mbs (512×512, 80 iters) | 1,380ms | 18,990ms | 13.76x |
| nb (n=1M) | 60ms | 5,850ms | 97.50x |
| regex_heavy (100K items × 50) | 810ms | 260ms | 0.32x |

Note: regex_heavy shows perlc slower than Perl because Perl's regex engine is highly optimized; the PCRE2 cache eliminates redundant `pcre2_compile` calls but the perlc overhead (function calls, PV boxing) still outweighs the benefit for regex-heavy workloads.

## PV Boxing Elimination Plan

### Problem Statement

Every numeric value in perlc is boxed into a `PerlValue*` (PV), even when the value is used only in arithmetic. This causes:
- 1 `pv_alloc()` per literal/binop result (freelist hit, but still cache-miss-prone)
- 1 `pv_free()` per temporary (freelist round-trip)
- Indirection through PV* chains for array elements
- Runtime calls for type coercion (`perl_to_float`, `perl_to_int`)

**Current mbs.pl inner loop**: ~20 runtime calls per `cmul`/`cadd` iteration, of which ~13 are PV alloc/free. Target: eliminate >80% of these.

### Phase 1: Fix DerefAV Cache for @_ Params

**Goal**: Cache `PerlArray*` for @_ params that are only used for array deref, eliminating repeated `perl_deref_array_ro` calls.

**Current state**: ~~FIXED~~ — DerefAV cache now populated for @_ params and local variables assigned from ArrowDeref. `declareDerefAV()` is called correctly, `isOnlyArrayRefDeref()` identifies candidates, and `emitExprF64` uses cached PerlArray*.

**Expected impact**: Eliminates 1 `perl_deref_array_ro` call per row access in mbs.pl inner loop.

### Phase 2: Extend FLAT_ARRAY to Runtime-Constructed Arrays

**Goal**: Use FLAT_ARRAY (tag=10) for arrays constructed at runtime, not just literals.

**Current state**: ~~FIXED~~ — FLAT_ARRAY threshold lowered from 4 to 2 elements. `perl_alloc_float_array(n)` added to runtime.c. All-F64 AnonArray literals compile to FLAT_ARRAY. 1D and 2D ArrowDeref fast paths handle FLAT_ARRAY.

**Expected impact**: `cadd`/`cmul` return values become FLAT_ARRAY instead of REF_ARRAY. Eliminates inner PerlArray alloc + 2 PV allocs per call.

### Phase 3: F64 Array Element Access

**Goal**: Read array elements as bare `double` when the array is known to be FLAT_ARRAY or FLOAT_PAIR.

**Current state**: ~~FIXED~~ — FLAT_ARRAY and FLOAT_PAIR 1D ArrowDeref fast path in `emitExprF64` skips tag dispatch when type is known. Variable index support via runtime PHI. DerefAV cache for local variables.

**Expected impact**: Eliminates `perl_array_get_ref` + `perl_to_float` for FLAT_ARRAY/FLOAT_PAIR elements. ~4 RT calls saved per cmul/cadd iteration.

### Phase 4: Unboxed Sub Returns

**Goal**: Return bare `double` from inlineable subs with F64 bodies, eliminating `perl_clone` + boxing.

**Current state**: ~~FIXED~~ — `tryEmitInline` now checks if body is F64-capable via `canEmitF64(*is.bodyExpr)`. If so, emits body using `emitExprF64` and returns raw F64 value. `emitCall` detects F64 return type and boxes via `perl_alloc_float`. Eliminates `perl_clone` + boxing for inlineable subs with float bodies (e.g., `cabs2($z) < 4.0` emits as pure double comparison).

**Expected impact**: Eliminates 1 `perl_clone` + 1 `perl_free` per cmul/cadd call for subs returning bare F64.

### Phase 5: Inline Literal Boxing

**Goal**: Eliminate `perl_alloc_int`/`perl_alloc_float` for literals used only in arithmetic.

**Current state**: ~~PARTIALLY FIXED~~ — F64 fast path already handles literals in BinOp trees. Stage 33 known tag type tracking propagates FLOAT_PAIR/FLAT_ARRAY types through assignments.

**Expected impact**: Minor — literals are already F64 when used in arithmetic. Main benefit for literals stored in FLAT_ARRAY (Phase 2).

### Phase 6: PerlArray Freelist Optimization

**Goal**: Reduce allocation cost for PerlArray structs and their element buffers.

**Current state**: ~~PARTIALLY FIXED~~ — `pa_alloc`/`pa_pool_push` already implement freelist pool (PA_POOL_CAP_MAX=4096). Small elems buffer pooling not yet implemented.

**Changes needed**:
1. **Pool small elems buffers**: For arrays with ≤64 elements, reuse a small buffer pool.
2. **FLAT_ARRAY buffer pooling**: Pool the `double[]` buffers for FLAT_ARRAYs.

**Expected impact**: Reduces allocation pressure for small arrays (common in mbs.pl).

### Implementation Order

1. ~~**Phase 1** (DerefAV debug)~~ — ~~DONE~~ — FIXED
2. ~~**Phase 2** (FLAT_ARRAY for runtime arrays)~~ — ~~DONE~~ — FIXED
3. ~~**Phase 3** (F64 array element access)~~ — ~~DONE~~ — FIXED
4. ~~**Phase 4** (Unboxed sub returns)~~ — ~~DONE~~ — FIXED
5. ~~**Phase 5** (Inline literal boxing)~~ — ~~DONE~~ — PARTIALLY FIXED
6. **Phase 6** (Freelist optimization) — Small elems buffer pooling for PerlArray

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

### Target Performance

| Benchmark | Current | Phase 1-2 | Phase 3-4 | Target |
|-----------|---------|-----------|-----------|--------|
| fibn (n=35) | 2.2x | 3x | 5x | 10x |
| mbs (1024²) | 12x | 18x | 25x | 50x |
| nb (1M) | 98x | 100x | 110x | 150x |

fibn is already fast (recursive, F64 path covers most ops). mbs is the main target — eliminating array access overhead is key. nb is already fast due to simple arithmetic loop.
