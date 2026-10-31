# LLVM Optimization Hang Analysis

## IR Patterns That Cause Hangs

### 1. Large Main Function with Inline Globals

**Pattern:** `main()` contains all global initialization (overload registration, constant
loading, FLAT_ARRAY setup) — 339 perl_ calls, 145 branches/comparisons.

```llvm
define i32 @main(...) {
  ; 13 overload registrations
  ; 3 global constant initializations (pi, solar_mass, days_per_year)
  ; 5 body array initializations (FLAT_ARRAY with 7 doubles each)
  ; 100+ store/load instructions for global variables
  ; Nested loops with 50M+ iterations
}
```

**Why it hangs:** LLVM's inliner tries to inline all perl_ calls into main, creating a
massive function with complex control flow. The IPO (Interprocedural Optimization) passes
then struggle with:
- Alias analysis on 10+ global variables
- Dead code elimination across 339 function calls
- Loop optimization on nested loops with indirect calls

### 2. FLAT_ARRAY Initialization with Many Stores

**Pattern:** Each FLAT_ARRAY element is a separate `store double` instruction:

```llvm
%flat.ep = getelementptr inbounds double, ptr %flat.dbl, i64 0
store double 0.000000e+00, ptr %flat.ep, align 8
%flat.ep14 = getelementptr inbounds double, ptr %flat.dbl, i64 1
store double 0.000000e+00, ptr %flat.ep14, align 8
; ... 50+ more stores
```

**Why it hangs:** LLVM's SROA (Scalar Replacement of Aggregates) pass tries to scalarize
these stores, but the FLAT_ARRAY's `pval` pointer and `matchpos` field prevent clean
scalarization. The pass iterates many times trying different transformations.

### 3. Nested Loops with Indirect Calls

**Pattern:** Hot loops call perl_ functions indirectly through the runtime:

```llvm
for (...) {
  %z_re = load ptr, ptr %zj, align 8
  %z_im = load ptr, ptr %zj, align 8
  %new_re = call double @perl_to_float(ptr %z_re)
  %new_im = call double @perl_to_float(ptr %z_im)
  store double %new_re, ptr %zpj, align 8
  ; ... 100+ iterations
}
```

**Why it hangs:** LLVM's loop vectorizer and unswitcher can't prove that `perl_to_float`
is pure (no side effects) because it's declared with `memory(read)` but not marked
`nounwind` or `readnone`. This prevents vectorization and causes the optimizer to
iterate many times trying to simplify the loop.

### 4. Many Small Functions with Overlap

**Pattern:** 4 functions in nb.ll, but each has 100+ basic blocks due to:
- PerlValue tag checking (`perl_is_bigint_pv`, `perl_is_blessed_pv`)
- Branching on tag types (PERL_INT, PERL_FLOAT, PERL_STRING, etc.)
- Clone/free pairs for every temporary value

**Why it hangs:** The function inliner tries to inline all small perl_ calls, but the
tag-checking branches create many unreachable paths that the optimizer must prune.

## Optimization Passes at Each Level

### O1 (Current — Legacy PM)
```
FunctionPassManager:
  1. InstructionCombiningPass    - Simplify instructions
  2. DeadCodeEliminationPass     - Remove dead code
  3. InstructionCombiningPass    - Second round
  4. DeadCodeEliminationPass     - Second round
```

### O2 (LLVM Default)
```
ModulePassManager:
  1. ModuleInlinerPass           - Inline small functions
  2. GlobalDCEPass               - Remove unused globals
  3. FunctionPassManager:
     a. SROA                     - Scalar replacement of aggregates
     b. InstCombine              - Instruction combining
     c. SimplifyCFG              - CFG simplification
     d. DCE                      - Dead code elimination
     e. InstCombine              - Second round
     f. LoopRotate               - Rotate loops for optimization
  4. FunctionPassManager:
     a. SROA                     - Second scalar replacement
     b. InstCombine              - Third round
     c. SimplifyCFG              - Second CFG simplification
     d. DCE                      - Second DCE
  5. GlobalOptimizerPass         - Optimize global variables
  6. FunctionPassManager:
     a. SROA                     - Third scalar replacement
     b. InstCombine              - Fourth round
     c. SimplifyCFG              - Third CFG simplification
     d. DCE                      - Third DCE
  7. PartialInliningPass         - Partial inlining
```

### O3 (LLVM Default — Aggressive)
```
ModulePassManager:
  1. ModuleInlinerPass           - Aggressive inlining
  2. GlobalDCEPass
  3. FunctionPassManager (3 rounds):
     a. SROA
     b. InstCombine
     c. SimplifyCFG
     d. DCE
     e. LoopRotate
     f. LoopUnroll               - Unroll small loops
     g.LICM                      - Loop-invariant code motion
  4. GlobalOptimizerPass
  5. FunctionPassManager (3 rounds):
     a. SROA
     b. InstCombine
     c. SimplifyCFG
     d. DCE
     e. LoopRotate
     f. LoopUnroll
     g. LICM
  6. PartialInliningPass
  7. FunctionPassManager (2 rounds):
     a. SROA
     b. InstCombine
     c. SimplifyCFG
     d. DCE
     e. LoopRotate
     f. LoopUnroll
     g. LICM
  8. LoopVectorize               - Vectorize loops
  9. SLIP                        - Scalar replacement in loops
```

### O4 (LLVM Default — Maximum)
```
ModulePassManager:
  1. ModuleInlinerPass           - Maximum inlining
  2. GlobalDCEPass
  3. FunctionPassManager (4 rounds):
     a. SROA
     b. InstCombine
     c. SimplifyCFG
     d. DCE
     e. LoopRotate
     f. LoopUnroll (full)
     g. LICM
     h. LoopUnswitch             - Switch loop-invariant conditions
  4. GlobalOptimizerPass
  5. FunctionPassManager (4 rounds):
     a. SROA
     b. InstCombine
     c. SimplifyCFG
     d. DCE
     e. LoopRotate
     f. LoopUnroll (full)
     g. LICM
     h. LoopUnswitch
  6. PartialInliningPass
  7. FunctionPassManager (3 rounds):
     a. SROA
     b. InstCombine
     c. SimplifyCFG
     d. DCE
     e. LoopRotate
     f. LoopUnroll (full)
     g. LICM
     h. LoopUnswitch
  8. LoopVectorize               - Aggressive vectorization
  9. SLIP
 10. SLIP                        - Second scalar replacement pass
```

## Key Passes That Likely Cause Hangs

| Pass | Why It Hangs | Fix |
|------|-------------|-----|
| **ModuleInlinerPass** | Tries to inline 339 perl_ calls into main | Add `noinline` to perl_ functions |
| **SROA** | Can't scalarize FLAT_ARRAY due to mixed types | Pre-convert FLAT_ARRAY to simple arrays |
| **LoopVectorize** | Can't prove perl_ calls are pure | Mark perl_ calls with `readnone` where possible |
| **LoopUnroll** | Unrolls 50M+ iteration loops | Add `#pragma clang loop unroll(disable)` |
| **LICM** | Can't hoist through perl_ calls | Mark perl_ calls as `memory(read)` only |
| **GlobalDCEPass** | Can't prove globals are unused | Use `internal` linkage for globals |

## Recommended Fixes

1. **Mark perl_ functions as `nounwind readnone`** where they don't modify memory
2. **Add `noinline` attribute** to perl_ functions to prevent inlining
3. **Use `internal` linkage** for globals instead of `global`
4. **Split main() into smaller functions** (one per Perl sub)
5. **Add `llvm.loop.unroll.disable` metadata** to hot loops
6. **Use `optnone` attribute** on functions that confuse the optimizer
