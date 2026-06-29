# REARCHITECTURE.md — Optimization Pass Cleanup Plan

## Current State (post Step 4 re-architecture)

Optimization passes are now centralized behind `PERLC_OPT_DISABLE` (Step 3 gates) and the recent layered passes (Stage 31/32/33) have been mapped and verified not to introduce correctness regressions vs real perl on the harness.

Gated (disableable) stages:
- **Stage 23 / allflat**: all-flat pre-check + loop-invariant flag for 2D FLAT_ARRAY rows (hoist `perl_array_is_all_flat`, branch on flag so LLVM can unswitch + prove !nonnull).
- **Stage 31 / flatdouble**: per-(outer,idx,lit) f64 SSA cache for repeated reads of the same flat element inside inner loops (cache invalidation on exact writes).

Stubs for removed stages (kept for docs/compatibility of PERLC_OPT_DISABLE strings; no runtime bodies):
- Stage 32 (loopderef/derefhoist), Stage 33 (knowntag) — no active code; gates return true unless explicitly disabled.

Foundational (always-on) fast paths that are not behind the stage gates:
- FLAT_ARRAY (Stage 22) 1D/2D ArrowDeref fast paths with tag dispatch + flatBB/PHI.
- DerefAV cache (Stages 15/25/27c) for @_ array-ref params and derived locals.
- FLOAT_PAIR (tag 13) inline complex numbers.
- F64 fast path (canEmitF64 / emitExprF64) + unboxed int/float vars + sub inlining.
- TBAA metadata (PerlValue vs flat double vs array elems) for alias disambiguation.
- AST-level sub inlining, PV slab, PerlArray freelist, PCRE2 LRU cache.

All verification uses `tests/harness.sh` (perl output diff, FP tolerance for numeric tests, ARGV matrix, OPT_LEVEL forwarding, PERLC_FLAGS). Key sets (nb, fibn, nbody, closures, regression_bugs, completeness) pass at default and with heavy disable across OPT_LEVEL 0/2/3; no SEGV. mbs.pl produces correct sample magnitude (2.0). Tier-1/arith diffs (sort order, FP formatting) are pre-existing and not introduced by gated stages.

These optimizations are layered and interdependent, making debugging extremely difficult:
- The nb.pl SEGV bug has resisted multiple weeks of debugging attempts
- Debug output added to codegen.cpp doesn't appear in LLVM IR
- Multiple optimization paths can be taken for the same code
- Changes in one optimization pass affect others

## Evidence of Technical Debt

1. **Debugging difficulty**: Added debug output to 2D ArrowDeref path, but LLVM IR shows neither the new path nor fallback path being used
2. **Complex conditional logic**: `codegen.cpp` lines 4654-4730 have nested conditionals for different optimization paths
3. **Multiple similar code paths**: FLAT_ARRAY fast path, 2D ArrowDeref path, fallback path all handling the same operation
4. **Hidden interactions**: Stage 32 and 33 optimizations may interact in unexpected ways

## Root Cause Hypothesis

The SEGV is likely caused by:
1. A bug in one of the optimization passes (Stage 32/33 most likely given recent additions)
2. The optimization passes are interfering with each other
3. The fallback paths are not properly handling edge cases

## Proposed Solution

### Phase 1: Reset and Diagnose (1-2 weeks)

1. **Disable all recent optimizations** (Stage 32/33):
   - Comment out known tag type tracking
   - Comment out array element type tracking  
   - Comment out loop-invariant PV deferral
   - Comment out deref hoisting

2. **Test nb.pl with optimizations disabled**:
   - If SEGV persists: bug is in Stage 22/23 (older code)
   - If SEGV fixed: bug is in Stage 32/33 (recent code)

3. **If bug in recent code**:
   - Re-enable Stage 33 only → test
   - Re-enable Stage 32 only → test
   - Identify which pass causes the SEGV

4. **If bug in older code**:
   - Re-enable Stage 23 only → test
   - Re-enable Stage 22 only → test

### Phase 2: Fix the Bug (1 week)

1. Fix the identified buggy optimization pass
2. Verify with all test cases
3. Re-enable other optimizations one at a time

### Phase 3: Simplify Optimization Infrastructure (2-3 weeks)

1. **Consolidate optimization passes**:
   - Merge related optimizations
   - Reduce nested conditionals
   - Add clear comments for each optimization's purpose

2. **Add optimization level control**:
   - `--opt-level=0`: No optimizations (for debugging)
   - `--opt-level=1`: Basic optimizations (Stage 22)
   - `--opt-level=2`: Advanced optimizations (Stage 22 + 23)
   - `--opt-level=3`: Full optimizations (all stages)

3. **Add verification**:
   - Test with different optimization levels
   - Verify LLVM IR matches expected patterns
   - Add litmus tests for each optimization

### Phase 4: Add Documentation (1 week)

1. Document each optimization pass:
   - Purpose and goals
   - Implementation approach
   - Expected performance impact
   - Known limitations

2. Add debugging guide:
   - How to disable optimizations
   - How to trace optimization paths
   - Common optimization-related bugs

## Immediate Action Items

1. ✅ Build with `-O0` to confirm codegen bug (done - SEGV persists)
2. Create a branch to disable recent optimizations
3. Systematically re-enable optimizations to identify culprit
4. Fix the bug
5. Simplify optimization infrastructure
6. Document changes

## Risk Assessment

- **High risk**: Removing optimizations may significantly impact performance
- **Mitigation**: Keep optimizations behind flags, test performance at each step
- **Rollback plan**: Keep current optimizations until new ones are verified

## Success Criteria (Step 4 complete)

- [x] nb.pl / fibn / nbody / closures / regression_bugs / completeness run without SEGV at default + heavy-disable + OPT 0/2/3 (harness-verified)
- [x] Harness (perl-equivalence) is the gate; numeric/perf core set passes at default and disabled stages; remaining diffs are pre-existing (runtime stubs, FP formatting, sort/wantarray order) and not opt-stage induced
- [x] `PERLC_OPT_DISABLE` + `isOptStageEnabled` provide uniform, named control; non-gated foundational passes (FLAT_ARRAY 22, DerefAV, FLOAT_PAIR, F64, inlining, TBAA) remain always-on
- [x] Docs updated (this file + CLAUDE.md) with current stage map and verification approach
