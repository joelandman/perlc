# mbs Benchmark Memory Analysis

## Problem

The mbs benchmark (N=4096) hangs after ~56 iterations. `strace` shows thousands of
`brk()` calls, indicating the PV allocator is exhausted and stuck in a heap-growth loop.

## Root Cause Analysis

### What mbs does

```perl
my $N = 4096;
# fill_z creates a 4096×4096 grid of complex numbers
sub fill_z {
    for ($j = 0; $j < $N; $j++) {
        $z->[$j] = [];                    # Row array
        for ($i = 0; $i < $N; $i++) {
            $z->[$j][$i] = cplx(re, im);  # FLOAT_PAIR
        }
    }
}
# mbi creates another 4096 row arrays × 4096 elements each
my @zzzp;
for ($j = 0; $j < $N; $j++) { $zzzp[$j] = []; }
for ($iter = 0; $iter < $Niter; $iter++) {
    # Each iteration creates 2 × N×N new FLOAT_PAIRs
    for ($j = 0; $j < $N; $j++) {
        for ($i = 0; $i < $N; $i++) {
            $zpj->[$i] = cadd(cmul($z, $z), $ccj->[$i]);  # New FLOAT_PAIR
        }
    }
}
```

### Memory layout per element

| Type | Size | perlc representation |
|------|------|---------------------|
| FLOAT_PAIR (complex) | 48 bytes (PerlValue) | Compact: fval + matchpos |
| REF_ARRAY row | 48 bytes (PerlValue) + 4096×8 bytes (ptr array) | 33KB per row |
| PerlArray (row data) | ~33KB | elems[4096] pointers |

### Total memory for N=4096, 80 iterations

| Component | Count | Size each | Total |
|-----------|-------|-----------|-------|
| FLOAT_PAIR complex numbers (fill) | 16M | 48B | 768MB |
| FLOAT_PAIR complex numbers (80 iters) | 2.56B | 48B | ~120GB (cumulative) |
| REF_ARRAY row objects | 16K (4 rows × 4096) | 48B | 768KB |
| PerlArray elems arrays | 16K | 33KB | ~512MB |
| **Total live at any time** | | | **~1.3GB** |
| **Total allocated over lifetime** | | | **~120GB** |

### Why it hangs

1. **PV slab exhaustion**: The PV slab allocator has `PV_SLAB_CAP=16384` slabs,
   each with `PV_SLAB=512` PerlValues. Total: 16384×512 = 8,388,608 slots.
   With 2.56B allocations over 80 iterations, the freelist is exhausted after
   the first ~8M elements, and subsequent allocations require new slabs.

2. **Heap growth loop**: When slabs are exhausted, `pv_alloc()` calls
   `calloc(PV_SLAB, sizeof(PerlValue))` to allocate a new slab. With 2.56B
   allocations needed, this triggers millions of `brk()` syscalls.

3. **No memory reclamation**: The mbs benchmark creates new PerlValues
   every iteration but doesn't free the old ones (the `@zzzp` grid is
   rebuilt each iteration). This causes memory to grow unbounded.

4. **Optimization disabled**: O0 means no constant folding or dead code elimination.

### Current status

- **PV_SLAB_CAP**: Increased from 256→16384 (64×)
- **PV_SLAB**: Increased from 128→512 (4×)
- **N=256**: Works (~7.5s for iterations)
- **N=512**: Works (~30s for iterations)
- **N=1024**: Works (~119s for iterations)
- **N=2048**: Works (~478s for iterations)
- **N=4096**: Hangs after ~56 iterations (PV allocator exhaustion)

## Remediation

### Option 1: Increase PV slab capacity (done)

```c
#define PV_SLAB_CAP 16384
#define PV_SLAB 512
```

**Result:** Extended N=4096 from hanging immediately to hanging after ~56 iterations.
Not sufficient for full benchmark completion.

### Option 2: FLAT_ARRAY for row arrays (recommended)

The row arrays should use FLAT_ARRAY instead of REF_ARRAY:

```perl
# Instead of:
$z->[$j] = [];
for ($i = 0; $i < $N; $i++) { $z->[$j][$i] = cplx(...); }

# Use:
$z->[$j] = [0.0, 0.0] x N;  # FLAT_ARRAY (64× memory reduction)
```

This requires:
1. Codegen change to detect `for` loops that fill arrays sequentially
2. Runtime change to support FLAT_ARRAY autovivification
3. Benchmark rewrite to use the new syntax

**Memory savings:** 64× for row data (double[] instead of PerlValue pointers)

### Option 3: 2D numeric grids (best performance)

Represent the entire grid as a single FLAT_ARRAY (double[]) with
2×N×N elements (re, im interleaved):

```c
// Instead of:
PerlValue *grid[N][N];  // 2D array of PerlValues

// Use:
double *grid = malloc(2 * N * N * sizeof(double));  // Flat array
// Access: grid[2*(j*N+i)+0] = re, grid[2*(j*N+i)+1] = im
```

**Memory savings:** 64× vs current approach

## Long-term Plan

### Phase 1: Quick wins (done)
- [x] Increase PV_SLAB_CAP to 16384
- [x] Increase PV_SLAB to 512
- [x] Document memory limits in CLAUDE.md

### Phase 2: FLAT_ARRAY improvements (next)
- [ ] Support `[expr] x N` for FLAT_ARRAY literals
- [ ] Auto-convert sequential-fill REF_ARRAYs to FLAT_ARRAY
- [ ] Add `perl_array_to_flat()` runtime helper

### Phase 3: 2D numeric grids (future)
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

- PV slab allocator: `src/runtime.c` lines 72-130
- FLAT_ARRAY codegen: `src/codegen.cpp` lines 8340-8430
- FLOAT_PAIR optimization: `src/codegen.cpp` lines 8360-8380
- mbs benchmark: `bench/mbs.pl` (hardcoded N=4096)
- `perl_repeat_flat_array`: `src/runtime.c` line ~1368 (unused)
- RepeatArray AST node: `src/ast.h` (unused)
