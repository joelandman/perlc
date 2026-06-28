# REARCHITECTURE.md — Optimization Pass Cleanup Plan

## Current State

The perlc compiler has accumulated multiple optimization passes over time (Stage 22, 23, 31, 32, 33):
- **Stage 22**: FLAT_ARRAY dispatch (2023)
- **Stage 23**: DerefAV cache (2023) 
- **Stage 31**: Flat double caching (2024)
- **Stage 32**: Loop-invariant PV deferral + deref hoisting (2024)
- **Stage 33**: Known tag type tracking + array elem type tracking (2024)

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

## Success Criteria

- [ ] nb.pl runs without SEGV at all optimization levels
- [ ] All 69 test programs pass at all optimization levels
- [ ] Documentation explains each optimization and how to disable it
- [ ] Debugging new bugs is straightforward (not requiring weeks of effort)
