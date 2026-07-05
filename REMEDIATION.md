# REMEDIATION.md — Critical Fixes Needed

All issues have been fixed:

1. ~~**Fix `NK::EvalBlock` LLVM codegen crash**~~ — FIXED (commit a701cd1)
   - Added check for `return` inside eval blocks in main function
   - Calls `perl_die` instead of emitting `ret ptr` (which violates LLVM's type requirements)

2. ~~**Fix `require` caching bug**~~ — FIXED (already working, verified)
   - Runtime `require` correctly registers named subs in the dispatch table

3. ~~**Fix compound `-=` on shared scalars**~~ — FIXED (commit db7ba77)
   - Added `get_or_install_mutex` call in `perl_atomic_add` to ensure mutex is installed

4. ~~**Add `*=` `/=` `%=` atomic RMW for shared scalars**~~ — FIXED (commit db7ba77)
   - Added `perl_atomic_rmw` runtime function with mutex protection
   - Codegen calls `perl_atomic_rmw` for `*=`, `/=`, `%=` on shared scalars

5. ~~**Fix closure + range with captured variable**~~ — FIXED (commit 776b963)
    - Fixed list return bug: added `perl_array_push_list_or_scalar` to unwrap `PERL_LIST_RESULT` tags
    - Fixed `perl_call_code_ref` to use caller's wantarray context

6. ~~**Stage 32: Loop-invariant PV deferral**~~ — FIXED (implemented in codegen.cpp)
    - Added `loopInvariantPVs_` stack; `trackPv()` routes `perl_alloc_undef` and `perl_deref_array` results to deferred tracking when inside a loop
    - `popScope()` skips freeing these PVs; `freeLoopInvariantPVs()` called after loop exit
    - perl_free calls reduced from 114 to 93 (18% reduction)

7. ~~**Stage 32: Deref hoisting**~~ — FIXED (implemented in codegen.cpp)
    - Added `loopDerefCache_` stack; `collectDerefTargets()` finds ScalarVars in ArrowDeref
    - `isVarModified()` checks if variable is written inside loop
    - `emitHoistedDerefs()` emits `perl_deref_array` before loop and caches result
    - `emitDerefArray()` checks cache before calling `perl_deref_array`

8. ~~**Stage 33: Known tag type tracking**~~ — FIXED (implemented in codegen.cpp)
    - Added `knownTagTypes_` stack; scalar assignments from AnonArray `[float, float]` set tag=13 (FLOAT_PAIR), `[float, ...]` set tag=10 (FLAT_ARRAY)
    - ArrowDeref RHS assignments propagate array element type to LHS
    - `emitExprF64` ArrowDeref skips tag dispatch when type is known

9. ~~**Stage 33: Array element type tracking**~~ — FIXED (implemented in codegen.cpp)
    - Added `arrayElemTypes_` stack; `$arr[i] = [float, ...]` sets element type for array
    - Type propagation through ArrowDeref assignments (`$var = $arr->[idx]`)

10. ~~**Function argument element type tracking removed**~~ — FIXED (correctness issue resolved)
    - Removed `funcArgElemTypes_` which caused incorrect code for non-array function arguments (e.g., `cabs` takes single FLOAT_PAIR, not array of FLOAT_PAIRs)
    - Type tracking now conservative — only tracks local variables and array elements, not cross-subroutine

11. ~~**Phase 4: Unboxed sub returns**~~ — FIXED (implemented in tryEmitInline)
    - `tryEmitInline` now checks if body is F64-capable via `canEmitF64(*is.bodyExpr)`
    - If so, emits body using `emitExprF64` and returns raw F64 value
    - `emitCall` detects F64 return type and boxes via `perl_alloc_float`
    - Eliminates `perl_clone` + boxing for inlineable subs with float bodies

12. ~~**Fix D2: `chop @arr` behaved identically to `chomp @arr`**~~ — FIXED
    - Added `perl_chop_array()` to runtime.c/runtime.h: chops every element in place, returns the last removed character (matching Perl's `chop LIST` semantics)
    - `codegen.cpp` array-chop branch now calls `perl_chop_array` instead of `perl_chomp_array`
    - Scalar `chop` return value now boxed via `perl_alloc_int` instead of leaking a bare int as `perlInt(0)`
    - Test coverage added to `tests/builtins.pl` (scalar and array forms); verified byte-for-byte against real perl via `tests/harness.sh`
