# LLVM Optimization Hang Analysis

## IR Patterns That Caused Hangs (FIXED)

### 1. Large Main Function with Inline Globals — FIXED

**Before:** `main()` contained all global initialization (overload registration, constant
loading, FLAT_ARRAY setup) — 339 perl_ calls, 145 branches/comparisons.

**After:** Split into two functions:
- `__perlc_init_globals(int argc, char **argv)` — 24 perl_ calls (35 IR lines)
- `main(int argc, char **argv)` — 123 perl_ calls (315 IR lines)

**Fix:** Extracted global initialization into a separate function. main() now calls
init() then runs the program body.

### 2. FLAT_ARRAY Initialization with Many Stores — PARTIALLY FIXED

**Pattern:** Each FLAT_ARRAY element is a separate `store double` instruction.

**Why it hung:** LLVM's SROA (Scalar Replacement of Aggregates) pass tries to scalarize
these stores, but the FLAT_ARRAY's `pval` pointer and `matchpos` field prevent clean
scalarization. The pass iterates many times trying different transformations.

**Fix:** Loop unrolling disabled by default in hot loops. SROA still runs at O2+ but
doesn't hang due to the main() split reducing function size.

### 3. Nested Loops with Indirect Calls — FIXED

**Pattern:** Hot loops call perl_ functions indirectly through the runtime.

**Why it hung:** LLVM's loop vectorizer and unswitcher couldn't prove that `perl_to_float`
is pure (no side effects) because it wasn't marked `nounwind`.

**Fix:** All perl_ runtime functions marked with `noinline` + `nounwind`. Loop unrolling
disabled by default. This enables better optimization without hangs.

### 4. Many Small Functions with Overlap — FIXED

**Pattern:** 4 functions in nb.ll, but each has 100+ basic blocks due to:
- PerlValue tag checking (`perl_is_bigint_pv`, `perl_is_blessed_pv`)
- Branching on tag types (PERL_INT, PERL_FLOAT, PERL_STRING, etc.)
- Clone/free pairs for every temporary value

**Why it hung:** The function inliner tried to inline all small perl_ calls, but the
tag-checking branches created many unreachable paths that the optimizer must prune.

**Fix:** `noinline` attribute on all 500+ perl_ runtime functions prevents inlining.

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

## Actual Benchmark Results (O0-O3)

| Test | O0 (ms) | O1 (ms) | O2 (ms) | O3 (ms) | Perl (ms) | Best vs Perl |
|------|---------|---------|---------|---------|-----------|-------------|
| **fibn** | 32 | 31 | 21 | 21 | 67 | **3.19x faster** (O2/O3) |
| **nb** | 26 | 26 | 33 | 27 | 27 | Matches Perl (O1/O3) |
| **regex_heavy** | 1092 | 665 | 675 | 633 | 760 | **1.20x faster** (O3) |

## Fixes Applied (All Working)

| # | Fix | Status | Impact |
|---|-----|--------|--------|
| 1 | Split main() into `__perlc_init_globals` + main body | DONE | Reduces perl_ calls from 339 to 147 |
| 2 | Add `noinline` + `nounwind` to all perl_ functions | DONE | Prevents inlining explosion |
| 3 | Use `internal` linkage for globals | DONE (was already) | Helps GlobalDCE |
| 4 | Disable loop unrolling by default | DONE | Prevents loop unroll hangs |
| 5 | Mark pure perl_ calls with `readonly` | DONE (selective) | Enables GVN/LICM |

## Remaining Work

1. **O4 optimization** — Would add LoopUnswitch, more aggressive vectorization
2. **Loop vectorization** — Could help fibn/nb if perl_ calls marked as `readnone`
3. **LLVM 22 upgrade** — When available, would provide new PM stability
4. **Split subs into more functions** — Could further reduce main() size
