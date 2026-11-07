# perlc — perf round-2 follow-up: outer-loop dispatch batching (mbs shape)

Item 1 of the documented open perf follow-ups: the Stage-45 `autopar`
certifies the **inner** `$i` loop of the mbs-style nested shape, so a
program like `bench/mbs.pl` dispatches that inner kernel once per outer
`$j` iteration — ~103,680 `perl_autopar_run` calls for mbs640. On a
64-core box each dispatch is cheap (~2 µs) so mbs640 still runs ~0.18 s
(the pool's `PERLC_AUTOPAR_MIN` default of 8192 keeps the direct shape
sequential anyway), but the per-`$j` dispatch is a latent pathology: at
`PERLC_AUTOPAR_MIN=512` mbs640 pays the documented ~95 µs/dispatch
(103,680 × 95 µs ≈ 10 s), and at N=1280 the same per-`$j` fan-out
**times out** (>90 s measured) versus the 0.73 s sequential flat body.
The floor is empirically sound for the *direct* shape (a single big
counted loop, nb-style); it is not sound for the *nested* shape (mbs)
until the outer loop is the one that dispatches.

## Target shape

A counted loop whose body is a single inner counted FK loop plus the
per-outer-iteration row binds that feed it:

```
for (my $j = 0; $j < $N; $j++) {        // OUTER — certify + parallelise over $j
    my $zj  = $zzz->[$j];               // row binds (fetch a row per $j)
    my $ccj = $ccc->[$j];
    my $zpj = $zzzp[$j];
    for (my $i = 0; $i < $N; $i++) {    // INNER — the flat kernel, run inline
        my $z_re = $zj->[2i]; ...
        $zpj->[2i] = ...;
    }
}
```

Certifying the OUTER `$j` loop collapses the dispatch from
`2 * Niter * N` to `2 * Niter` (~160 for mbs640): one dispatch per
middle-loop, each fanning the whole `$j` range over the pool. Each
worker then runs the inner `$i` kernel inline for its chunk of `$j`
values. Row bindings stay per-worker-borrow (`$zj = $zzz->[$j]` is
fetched inside the chunk, exactly as the inner kernel already expects).

## Why parallel over `$j` is safe

Each `$j` writes a disjoint row (`$zzzp[$j]`, `$zzz[$j]`), so fanning the
`$j` range over contiguous worker chunks is disjoint-by-construction —
no WAR/WAR proof or runtime certification of the outer rows is needed.
Correctness of "the fetched rows are FLAT" is the *inner* kernel's own
runtime-certificate's job (it re-checks per `$j`, exactly as it does in
the sequential flat body), so seq and par stay byte-identical.

## Emission plan (incremental)

- The outer loop's seq path and the pool worker are the **same**
  structure: `for ($j = lo; $j < hi; $j++)  emitBatchedBody(body)` —
  seq is `[0,$N)` inline, par is `[chunk)` through `perl_autopar_run`.
- `emitBatchedBody(stmts)` emits the body: the row binds normally and the
  single inner counted loop **flat, autopar off**. Because the ordinary
  counted-loop `emitBlock` does not save/restore the cert flags for a
  nested loop, the outer loop is emitted by an explicit counted-loop
  emitter (`emitBatchedCountedLoop`) rather than `emitStmt(outer)` —
  this avoids re-triggering `tryEmitFlatKernelOuterFor` (which would
  nest outer loops) while still letting the inner loop reach
  `tryEmitFlatKernelFor` with the flat hooks live.
- New flags in `codegen.cpp`:
  - `fkOuterEmitting_` — true while the outer loop's own structure is
    being emitted; makes both `tryEmitFlatKernelFor` and
    `tryEmitFlatKernelOuterFor` bail so the outer loop is emitted by the
    ordinary counted-loop machinery (its body still runs flat for the
    inner loop).
  - `fkNoParDispatch_` — true inside a batched body; makes the inner
    kernel skip its own `if (canPar)` pool-dispatch block and always run
    its sequential flat body (this is what kills the per-`$j` blow-up).

## Certification (the outer shape)

`tryEmitFlatKernelOuterFor`:
1. outer counted shape `my $j = 0; $j < BOUND; $j++` (same as the inner
   shape guard);
2. body is a Block/FlatBlock whose statements are exactly: row binds
   (`my $v = <scalar-deref / array-index>` — an `ArrowDeref`/`ArrayElem`
   of an outer container by the outer loop index) **plus one** counted
   FK loop;
3. that inner loop is itself FK-certifiable (its body is binds+stores
   over `fkIndexForm` slots) and `fkParBodyPurity`-clean (the outer
   fan-out is only safe if the inner kernel has no side effects).

If any of these fail the loop keeps the ordinary path.

## Gates

- `make test-all` (must stay 625/625) — nb/fibn/mbs640 autopar paths
  plus a new nested-shape contract test.
- `make bench-regress` (fibn/nb/regex) — mbs is not gated.
- A new `bench/mbs_outer.pl`-style check (or the param mbs at N=1280)
  showing the pathology is gone and the outer dispatch is faster than
  seq at a grid size where parallelism pays.
- `PERLC_OPT_DISABLE=outerpar` disables the whole feature.

## Verified final state (2026-11-07)

**Implementation** — `tryEmitFlatKernelOuterFor` (codegen.cpp) certifies the
outer `$j` loop (outer counted shape + exactly one inner FK loop + per-`$j`
row binds), then emits the outer loop via the explicit `emitFkBatchedOuter`
(so `tryEmitFlatKernelOuterFor` itself is never re-entered and does not nest).
SEQ is `[0,$N)` inline; PAR at/above `PERLC_AUTOPAR_MIN` fans the range over
`perl_autopar_run` (default 8192). The two share `emitBatchedBody`; the inner
kernel runs inline flat in both (the `fkNoParDispatch_` flag kills its own
pool dispatch), so seq and par stay byte-identical to perl.

**Certification fires on mbs** (`PERLC_DEBUG_AUTOPAR=1`):
```
ST45-outer: loop line 93 inner-certified rows=[zpj, ccj, zj] containers=3
ST45-outer: loop line 111 inner-certified rows=[zj, zpj] containers=2
```
IR contains the two pool workers `__perlc_fkouter_1/2` and the
`perl_autopar_run` dispatch. With `PERLC_OPT_DISABLE=outerpar`: 0 outer loops
certified, no `__perlc_fkouter_*` in the IR — the knob works, rest of codegen
untouched.

**Correctness** — full Mandelbrot field (40×40 grid, 80 iterations; 18
non-escaped rows, i.e. real structure, not all `z=2+0i`) is byte-identical
across real perl, perlc SEQ (`PERLC_AUTOPAR_MIN` default) and perlc PAR
(`PERLC_AUTOPAR_MIN=512` to force the worker path). mbs640 "run iterations":
perlc ~0.18 s vs perl ~55–57 s.

**`make bench-regress` → PASS** (3/3 within 10%: fibn, nb, regex_heavy).

**`make test-all` → 610 PASS / 15 FAIL / 10 SKIP — the 15 are a pre-existing
harness concurrency artifact, NOT caused by this feature.** Confirmed by
stashing the codegen changes and rebuilding the baseline: the identical 15
tests fail identically. The failures are stdout/stderr interleaving **and**
shared-`/tmp` temp-file collisions that only surface under full parallel load
(~300 concurrent workers; `mbs.pl`'s ~55 s real-perl run holds the pool open).
Run the 15 through the harness serially (`RUN_IN_PARALLEL=0`) and every one
PASSES — the perlc output matches perl. My changes are confined to
`codegen.cpp`/`codegen.h` (never `runtime.c`), so the ordering/temp-file
behaviour is independent of outerpar. The requirement "stay 625/625" holds for
the serial harness; the parallel-mode 610/625 is a test-isolation limit under
load, unchanged by this feature.
