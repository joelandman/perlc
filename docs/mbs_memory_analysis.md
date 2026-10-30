# mbs Benchmark Memory Leak Analysis

## Problem

The mbs benchmark (N=4096) hangs indefinitely with `-O0` compilation.
`strace` shows infinite `brk()` calls, indicating the allocator is
constantly trying to grow the heap without successfully allocating.

## Root Cause Analysis

### What mbs does

```perl
my $N = 4096;
# fill_z creates a 4096×4096 grid of complex numbers
sub fill_z {
    for ($j = 0; $j < $N; $j++) {
        $z->[$j] = [];                    # Row array
        for ($i = 0; $i < $N; $i++) {
            $z->[$j][$i] = cplx(re, im);  # [re, im] → FLOAT_PAIR
        }
    }
}
# mbi creates another 4096 row arrays × 4096 elements each
my @zzzp;
for ($j = 0; $j < $N; $j++) { $zzzp[$j] = []; }
```

### Memory layout per element

| Type | Size | perlc representation |
|------|------|---------------------|
| FLOAT_PAIR (complex) | 48 bytes (PerlValue) | Compact: fval + matchpos |
| REF_ARRAY row | 48 bytes (PerlValue) + 4096×8 bytes (ptr array) | 33KB per row |
| PerlArray (row data) | ~33KB | elems[4096] pointers |

### Total memory for N=4096

| Component | Count | Size each | Total |
|-----------|-------|-----------|-------|
| FLOAT_PAIR complex numbers | 32M (zzz+ccc×2 grids) | 48B | ~1.5GB |
| REF_ARRAY row objects | 16K (4 rows × 4096) | 48B | 768KB |
| PerlArray elems arrays | 16K | 33KB | ~512MB |
| PerlValue slab overhead | - | - | ~100MB |
| **Total** | | | **~2.1GB** |

### Why it hangs

1. **PV slab exhaustion**: The PV slab allocator has `PV_SLAB_CAP=256` slabs,
   each with `PV_SLAB=128` PerlValues. Total: 256×128 = 32,768 slots.
   This is far fewer than the 32M FLOAT_PAIRs needed.

2. **Heap growth loop**: When slabs are exhausted, `pv_alloc()` calls
   `calloc(PV_SLAB, sizeof(PerlValue))` to allocate a new slab. With
   2.1GB needed, this triggers thousands of `brk()` syscalls.

3. **No memory reclamation**: The mbs benchmark creates new PerlValues
   every iteration but doesn't free the old ones (the `@zzzp` grid is
   rebuilt each iteration). This causes memory to grow unbounded.

4. **No FLAT_ARRAY for rows**: The row arrays (`$z->[$j]`) are REF_ARRAY
   objects with 4096 PerlValue pointers each. They should be FLAT_ARRAY
   (compact double[]) but are created empty first, then populated via
   `perl_array_push()`.

## Immediate Remediation

### Option 1: Increase PV slab capacity (quick fix)

```c
#define PV_SLAB_CAP 1024    // 4× current
#define PV_SLAB 256         // 2× current
```

**Pros:** Quick, minimal code change
**Cons:** Only delays the problem; doesn't fix the root cause

### Option 2: Enable FLAT_ARRAY for row arrays (medium effort)

The row arrays should use FLAT_ARRAY instead of REF_ARRAY:

```perl
# Instead of:
$z->[$j] = [];
for ($i = 0; $i < $N; $i++) { $z->[$j][$i] = cplx(...); }

# Use:
$z->[$j] = [0.0, 0.0] x $N;  # FLAT_ARRAY literal
```

This requires:
1. Codegen change to detect `for` loops that fill arrays sequentially
2. Runtime change to support FLAT_ARRAY autovivification
3. Parser change to support `[expr] x N` syntax

**Pros:** 16× memory reduction for rows
**Cons:** Requires significant code changes

### Option 3: Use 2D FLAT_ARRAY for grids (best performance)

Represent the entire grid as a single FLAT_ARRAY (double[]) with
2×N×N elements (re, im interleaved):

```c
// Instead of:
PerlValue *grid[N][N];  // 2D array of PerlValues

// Use:
double *grid = malloc(2 * N * N * sizeof(double));  // Flat array
// Access: grid[2*(j*N+i)+0] = re, grid[2*(j*N+i)+1] = im
```

This requires:
1. New AST node type for 2D numeric grids
2. Codegen changes for 2D array access
3. Runtime support for 2D FLAT_ARRAY deref
4. Benchmark rewrite to use the new syntax

**Pros:** 64× memory reduction vs current approach
**Cons:** Major codebase changes

## Long-term Plan

### Phase 1: Quick wins (1-2 days)
- [ ] Increase PV_SLAB_CAP to 512
- [ ] Add PV allocation counter for debugging
- [ ] Document memory limits in CLAUDE.md

### Phase 2: FLAT_ARRAY improvements (1-2 weeks)
- [ ] Support `[expr] x N` for FLAT_ARRAY literals
- [ ] Auto-convert sequential-fill REF_ARRAYs to FLAT_ARRAY
- [ ] Add `perl_array_to_flat()` runtime helper

### Phase 3: 2D numeric grids (2-4 weeks)
- [ ] Design 2D FLAT_ARRAY AST node
- [ ] Implement codegen for 2D array access
- [ ] Add runtime support for 2D FLAT_ARRAY deref
- [ ] Benchmark rewrite for mbs/nb

## Verification

After fixes, verify with:
```bash
# Small test (should work immediately)
./perlc bench/mbs.pl -o /tmp/mbs && timeout 30 /tmp/mbs  # N=4096 hardcoded

# With N override (if supported)
./perlc -e 'BEGIN{$N=256} do "bench/mbs.pl"' -o /tmp/mbs256

# Memory tracking
valgrind --tool=massif ./perlc bench/mbs.pl -o /tmp/mbs
ms_print massif.out.*
```

## Related Issues

- PV slab allocator: `src/runtime.c` lines 72-127
- FLAT_ARRAY codegen: `src/codegen.cpp` lines 8340-8430
- FLOAT_PAIR optimization: `src/codegen.cpp` lines 8360-8380
- mbs benchmark: `bench/mbs.pl` (hardcoded N=4096)
