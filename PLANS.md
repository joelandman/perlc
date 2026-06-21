# PLANS.md — Correctness, Completeness, Performance Plans

## Correctness Plans

1. **Fix `NK::EvalBlock` LLVM codegen crash.** `eval { BLOCK }` produces invalid LLVM IR because `endBB` lacks a `ret` instruction — LLVM verify error: `Function return type does not match operand type of return inst! ret ptr %17 i32`. Fix: add `builder_.CreateRet(perlUndef())` after `perl_eval_pop` in the `endBB` block. This is a compiler crash blocking any program using block eval from compiling. Test: `tests/eval_exception.pl` currently fails.

2. **Fix `require` caching bug.** `test_require_simple.pl` fails on `require_loads_named_sub` and `require_repeat_keeps_sub_available`. The runtime `require` does not properly register named subs from `.pm` files in the dispatch table. Fix: ensure `perl_runtime_require` adds subs to the code reference table so they're callable via dispatch. Test: `tests/test_require_simple.pl` currently fails.

3. **Fix compound `-=` on shared scalars.** `$shared -= 5` emits `perl_atomic_add($shared, +5)` instead of subtracting — the delta is not negated before the atomic add. Fix: negate delta via `perl_to_float` → `fneg` → `boxF64` before `perl_atomic_add`. Test: `tests/regression_bugs.pl` `regression_multi_subtract` currently fails.

4. **Add `*=` `/=` `%=` atomic RMW for shared scalars.** These compound ops fall through to non-atomic `perl_assign` instead of using atomic RMW primitives. For int/float payloads, use lock-free 16-byte CAS-on-payload (same pattern as `$x++`). For non-numeric tags, fall back to SharedMutex. Test: `tests/regression_bugs.pl` `regression_multiply` currently fails.

5. **Add valgrind/memcheck verification to test harness.** No automated memory-safety gate exists. The PV slab allocator and PerlArray freelist pool hide leaks from standard tools. Add `make test-valgrind` that runs all 36 tests under `valgrind --tool=memcheck --leak-check=full --errors-for-leak-kinds=all`. Fix every reported leak (currently 4,096 bytes "still reachable" from PV slab at exit).

## Completeness Plans

1. ~~**Implement `tie`/`untie` with minimal TIESCALAR/TIEARRAY/TIEHASH interface.**~~ — FIXED (commit TBD)
    - Added `tie`/`untie` lexer tokens, AST nodes, parser rules, and codegen
    - Runtime: `perl_tie()` calls CLASS->TIESCALAR/TIEARRAY/TIEHASH and blesses result
    - Runtime: `perl_untie()` calls UNTIE() method on blessed object
    - Note: FETCH/STORE interception for tied variables requires additional codegen changes

2. ~~**Implement `do FILE` runtime execution.**~~ — FIXED (already working, verified with tests/test_do_filename.pl)
    - `do "file.pl"` executes file each time (no caching), returns last expression value, sets `$@` on failure

3. **Add `pack`/`unpack` for binary data serialization.** These are among the most-used builtins for network protocols, file formats, and FFI. Start with the most common format codes: `C`, `c`, `S`, `s`, `L`, `l`, `N`, `V`, `F`, `D`, `A`, `a`, `H`, `h`, `B`, `b`, `x`, `X`, `@`. The runtime would interpret format strings and produce/consume byte buffers, integrating with `ptr` values in the XS interface.

4. **Add UTF-8/unicode support.** Currently strings are byte-only. Adding `use utf8`, `use Encode`, and the `unicode` pragma would require: (a) UTF-8 decoding in the lexer for source files, (b) UTF-8 aware `length`, `substr`, `index`, `rindex`, and regex operations in the runtime, (c) `chr`/`ord` for code points above 255. This is a large effort but essential for CPAN compatibility.

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

**Current state**: `isOnlyArrayRefDeref()` correctly identifies candidates, but `declareDerefAV()` is not being called in practice. Debug why:
1. Check if `fromUnderbar` is true for `my ($arr) = @_`
2. Check if `currentSubBody_` is set when the assignment is processed
3. Check if `isOnlyArrayRefDeref()` returns true for the mbs.pl subs

**Fix**: Once the root cause is found, ensure DerefAV allocas are created and used in `emitExprF64`.

**Expected impact**: Eliminates 1 `perl_deref_array_ro` call per row access in mbs.pl inner loop.

### Phase 2: Extend FLAT_ARRAY to Runtime-Constructed Arrays

**Goal**: Use FLAT_ARRAY (tag=10) for arrays constructed at runtime, not just literals.

**Current state**: FLAT_ARRAY is only created for `AnonArray` literals with ≥4 all-numeric elements. Runtime arrays (e.g., `cadd` return value) use REF_ARRAY.

**Changes needed**:
1. **`perl_alloc_flat_array(n)`** — already exists in runtime.c
2. **`perl_float_array_new()`** — new runtime function: creates a FLAT_ARRAY with `n` zero-initialized doubles
3. **`perl_float_array_push()`** — new runtime function: pushes a double into a FLAT_ARRAY
4. **codegen**: When emitting an `AnonArray` with all-F64 children, call `perl_alloc_flat_array(n)` + stores instead of `perl_anon_array_new` + `perl_array_push`
5. **`canEmitF64(AnonArray)`** already returns true (after removing `has1DArrow` guard in commit). Now `emitExpr(AnonArray)` needs to use FLAT_ARRAY when all children are F64.

**Implementation in codegen.cpp** (AnonArray case, ~line 5490):
```cpp
// After the existing FLOAT_PAIR check (size==2), add FLAT_ARRAY for size>=2:
if (allFloat && n.args.size() >= 2) {
    // Use perl_alloc_flat_array + direct stores
    Value *flatPV = callRT("perl_alloc_flat_array", {i64(n.args.size())});
    Value *dblPtr = load pval from flatPV;
    for each arg: store emitExprF64(arg) into dblPtr[i];
    return flatPV;
}
```

**Expected impact**: `cadd`/`cmul` return values become FLAT_ARRAY instead of REF_ARRAY. Eliminates inner PerlArray alloc + 2 PV allocs per call.

### Phase 3: F64 Array Element Access

**Goal**: Read array elements as bare `double` when the array is known to be FLAT_ARRAY or FLOAT_PAIR.

**Current state**: Even with DerefAV cache, `emitExprF64(ArrowDeref)` still calls `perl_array_get_ref` + `perl_to_float`.

**Changes needed**:
1. **FLAT_ARRAY row cache**: When a row is known to be FLAT_ARRAY, emit direct `double*` load:
   ```cpp
   // In emitExprF64 for ArrowDeref:
   if (Value *flatPtr = lookupFlatRow(nm, idxNm)) {
       Value *idx = emitIdx(*n.right);
       Value *ep = builder_.CreateGEP(f64Ty, flatPtr, idx);
       return builder_.CreateLoad(f64Ty, ep);
   }
   ```
2. **FLOAT_PAIR element access**: Already partially implemented (lines 1945-1993). Needs extension to variable indices.
3. **Type-stable arrays**: Track whether an array is FLAT_ARRAY at compile time. If all writes to an array use FLAT_ARRAY, the reads can skip the tag check.

**Implementation**:
1. Extend `declareFlatRow` to work for 1D arrays (not just 2D)
2. In `emitExprF64(ArrowDeref)`, check for flat row cache before calling runtime
3. Add `lookupFlatRow` for 1D arrays with known float-only elements

**Expected impact**: Eliminates `perl_array_get_ref` + `perl_to_float` for FLAT_ARRAY elements. ~4 RT calls saved per cmul/cadd iteration.

### Phase 4: Unboxed Sub Returns

**Goal**: Return bare `double` from inlineable subs with F64 bodies, eliminating `perl_clone` + boxing.

**Current state**: `tryEmitInline` returns `emitExpr(*is.bodyExpr)` which returns a PV*. Even when the body is F64, the result is boxed.

**Changes needed**:
1. **`tryEmitInline` return type**: Return `Value*` that can be either PV* or bare f64. Use a wrapper struct or convention (e.g., null PV* means f64 value in a thread-local slot).
2. **Caller handling**: When the inlined sub returns f64, use it directly. When it returns PV*, box/unbox as needed.
3. **Alternative**: Use a dedicated "f64 return" alloca per inlined sub call. The inlined body stores to this alloca, and the caller loads from it.

**Simpler approach**: Keep returning PV*, but when the body is F64, create a thread-local f64 slot and store the result there. The PV* points to a temporary PV with the f64 value. This avoids the `perl_clone` call.

**Expected impact**: Eliminates 1 `perl_clone` + 1 `perl_free` per cmul/cadd call.

### Phase 5: Inline Literal Boxing

**Goal**: Eliminate `perl_alloc_int`/`perl_alloc_float` for literals used only in arithmetic.

**Current state**: Every `IntLit` and `FloatLit` calls `perlInt()`/`perlFloat()` which allocates a PV.

**Changes needed**:
1. **`emitExprF64(IntLit/FloatLit)`**: Already returns bare `double` (lines 1657-1660). The issue is that `emitExpr` is called instead when the context doesn't demand F64.
2. **F64 context propagation**: Extend `canEmitF64`/`emitExprF64` to be used more aggressively. Currently, F64 path is only taken when the parent node is F64-capable (BinOp, Call, etc.).
3. **Literal temporaries**: When a literal is used only in arithmetic, emit it as bare f64 and skip boxing entirely.

**Implementation**: The F64 fast path already handles this for literals inside BinOp trees. The issue is when literals are stored in arrays or passed to subs. For those cases, the boxing is necessary.

**Expected impact**: Minor — literals are already F64 when used in arithmetic. The main benefit is for literals stored in FLAT_ARRAY (Phase 2).

### Phase 6: PerlArray Freelist Optimization

**Goal**: Reduce allocation cost for PerlArray structs and their element buffers.

**Current state**: `pa_alloc`/`pa_pool_push` already implement a freelist pool (PA_POOL_CAP_MAX=4096). But the inner `elems` buffer is not pooled.

**Changes needed**:
1. **Pool small elems buffers**: For arrays with ≤64 elements, reuse a small buffer pool instead of `malloc`/`free`.
2. **FLAT_ARRAY buffer pooling**: Pool the `double[]` buffers for FLAT_ARRAYs.

**Expected impact**: Reduces allocation pressure for small arrays (common in mbs.pl).

### Implementation Order

1. **Phase 1** (DerefAV debug) — quick fix, immediate impact
2. **Phase 2** (FLAT_ARRAY for runtime arrays) — medium effort, high impact
3. **Phase 3** (F64 array element access) — medium effort, high impact
4. **Phase 4** (Unboxed sub returns) — lower effort, moderate impact
5. **Phase 5** (Inline literal boxing) — already partially done
6. **Phase 6** (Freelist optimization) — low effort, incremental improvement

### Target Performance

| Benchmark | Current | Phase 1-2 | Phase 3-4 | Target |
|-----------|---------|-----------|-----------|--------|
| fibn (n=35) | 2.2x | 3x | 5x | 10x |
| mbs (1024²) | 12x | 18x | 25x | 50x |
| nb (1M) | 98x | 100x | 110x | 150x |

fibn is already fast (recursive, F64 path covers most ops). mbs is the main target — eliminating array access overhead is key. nb is already fast due to simple arithmetic loop.
