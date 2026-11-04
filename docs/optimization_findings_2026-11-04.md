# perlc — Generated-Code Performance Study (2026-11-04)

Status: benchmark + profile pass over the current `./perlc` (LLVM 22
codegen, clang-22 link). Includes one correctness fix found by the
benchmark work (REF-base element stores — D196), with `make test-all`
607/607 PASS and `make bench-regress` PASS (fibn 3% faster than the
pre-guard baseline, nb 1.4%, regex 1.6%).

## 1. Methodology

Four benchmarks scaled so **real `perl` runs ≈ 50–65 s** on this host
(Perl 5.44, 64-core box, otherwise idle; sequential timing runs):

| Benchmark | Workload | perl (s) | source of the copy |
|---|---|---|---|
| fibn | `fib(0..37)`, recursive | 49.4 | `bench/fibn.pl` as-is |
| nb | N-body, 4,000,000 iterations | 64.4 | `bench/nb.pl` as-is |
| regex3m | 3,000,000-item corpus × 70 passes ≈ 210M match attempts | 50.8 | `bench/regex_heavy.pl` scaled (6M×70 = 111 s also measured) |
| mbs | Mandelbrot, N=640 grid (FLAT_ARRAY rows) | 57.3 | `bench/mbs.pl` with N=640 + a pure-perl `flat_fill_pair` stub so real perl can run it |

Every perlc number is best-of-3 of the compiled binary.
Compile times per binary: -O0 ≈ 2.4 s, -O1 ≈ 15.3 s, -O2 ≈ 19.1 s,
-O3 ≈ 19.9 s (-O4/-O5 are accepted and both map to clang -O3).
`bench/mbs.pl` hard-codes N=4096 (`docs/mbs_memory_analysis.md`), so
mbs runs here use N=640/256 copies. Outputs were diffed vs `perl`
per level (identical for fibn/nb/regex; mbs timing lines ignored,
`sample abs(z[0,0])=2.000000` matched post-fix).

**Correctness of the benchmark set itself:** running mbs under perlc
surfaced a real generated-code bug (D196) — `bench/mbs.pl`'s `fill_z`
stores rows through a promoted `@_` ref (`my ($z,...) = @_; $z->[$j] =
flat_fill_pair(...)`) and every row store was silently **dropped**:
`@z` stayed empty and all 80×N² iterations ran on undef/0 rows.
Compiled binary printed `sample abs(z[0,0])=0.000000` where perl
prints `2.000000`. Fix (runtime, `perl_array_set_row`): REF_* bases
are dereferenced to the outer container before any element store; the
pack-convert branch no longer rewrites ref PVs in place. Second piece
(codegen, `emitPackedPairComponents`): the `ArrowDeref` RHS branch that
reinterpreted any `$x->[i]` RHS as a pair read was removed (a scalar
projection `$pair->[0]` stored a bogus pair). A side consequence:
**mbs's perlc column in `bench/results.csv` has been measuring a
degenerate all-zero workload since the FLAT_ARRAY rewrite.**

## 2. Where the -O levels actually live

Under LLVM 22, `CodeGen::runOptimization()` does nothing
(`src/codegen.cpp:15906`), so **the `-O` digit only sets the clang-22
level on the link line**, which compiles BOTH the generated IR and
`runtime.c`/`mini-gmp.c`/`native_stdlib.c` in one invocation. There is
no LTO: runtime calls stay opaque, informed only by declaration
attributes (`NoInline` on everything; `memory(read)` + `WillReturn` on
six functions).

Measured (perl baseline in parentheses; post-D196 for mbs):

| Workload | O0 | O1 | O2 | O3 | perl | best perlc/perl |
|---|---|---|---|---|---|---|
| fibn 37 | 78.5 | 25.8 | 24.6 | 24.5 | 49.4 | **2.0×** |
| nb 4M | 38.8 | 18.3 | 18.2 | 18.2 | 64.4 | **3.5×** |
| regex3m | 41.6 | 23.8 | 25.9 | 23.2 | 50.8 | **2.2×** |
| mbs 640 | 166.3 | 70.4 | 67.5 | 67.3 | 57.3 | **0.85× (slower)** |

Findings:

- **The O0 cliff is the runtime, not the IR.** The generated IR barely
  changes between levels (codegen's fast-path stages are always on);
  the 2–3× O0 penalty comes from compiling `runtime.c` unoptimized —
  HOT/always_inline is ignored at -O0, `pv_alloc`/`pv_pool_push` stay
  out-of-line, no instruction combining on the value model. regex is
  exempt (PCRE2 JIT + corpus build dominate; its cost is call-count-
  independent).
- **O1 ≈ O2 ≈ O3 within ±2–9% everywhere.** Past -O1 LLVM has nothing
  to do: hot loops are call-dense (§3), `noinline` blocks inlining +
  IPA, no unroll/vectorize hints fire on these loops (§5), and the
  flat-vs-ref-row dispatch never unswitches at O2 (verified in
  disassembly: nb's hot loop still re-checks the `allflat` flag every
  iteration).
- **fibn** is now ≈2.0× faster than perl at O1+; the 2026-10-31
  `bench/results.csv` (0.52×, "perlc slower") is stale — the LLVM-22
  switch + the `perl_free` fast path moved it. At O0 perlc is still
  1.6× slower than perl on fibn.
- **mbs** is the remaining deficit: 0.85× (18% slower than perl), and
  its Stage-35 CPLX packing is a measured **net loss** for this
  workload: `PERLC_OPT_DISABLE=cplxrow` makes mbs N=256 run 7.30 s vs
  10.79 s enabled (−32%).

## 3. What the generated code does per operation (IR census)

- `fib($n)`: **45 runtime calls** per call for ~10 Perl operations —
  two `PerlArray` alloc/free cycles for `@_`, the unpack
  get_ref/assign/array_len triple, boxed `perl_num_le` with alloc/free
  boilerplate, two `perl_sub`, `perl_add`, a `perl_clone`+free pair on
  the return path, 4× `perl_promote_ref_array`, wantarray
  push/pop and a local-depth save even though the sub has no `local`
  (Stage 24b only skips it when there is no explicit `return`).
- nb `advance` j-loop: **52 runtime calls + 35 FP ops + 88
  branch/icmp/phi** per iteration for ~26 real flops. Each
  `$bodies->[i][k]` re-loads the outer array pointer, takes a
  flat-vs-REF_ARRAY phi pair and a null check (s17/s18 blocks).
- regex loop body: 8 calls per element — `perl_array_get_ref`,
  `perl_promote_ref_array`, `perl_regex_match_bool`, plus a
  `perl_clone`+`perl_inc`+`perl_free` triple per matched element for
  file-scope `$count++` (the clone is dead when the value is unused).

So at every level ≥O1 the machine state per Perl op is "one `noinline`
call plus its prologue/epilogue". The wins are (a) not emitting those
calls, (b) letting LLVM see through them — both structural.

## 4. The biggest lever available today: selective LTO + attribute diet

Link-level experiment only (same `.ll`, link with
`clang-22 -O3 -flto`, runtime.c/mini-gmp/native_stdlib in the same
invocation; variant B strips `noinline` from the small hot leaf fns
and keeps `memory(read)` on the six readonly ones):

| Variant | nb 4M | fibn 37 | mbs 640 |
|---|---|---|---|
| non-LTO -O3 (current perlc) | 18.13 | 24.20 | 67.37 |
| LTO, attributes as-is | **16.52 (−9%)** | — | — |
| LTO + leaf fns un-noinlined | 16.74 (−8%) | **15.93 (−34%, 1.52×)** | **66.00 (−2%)** |

fibn gains 1.52× from letting LLVM inline
`perl_alloc_int/perl_free/perl_clone/perl_num_le/...` across the TU
boundary — zero codegen changes. nb gains ~9% from readonly/willreturn
IPA even with `noinline` intact. The earlier pre-fix mbs LTO
"regression" (200 s) turned out to be an artifact of the
garbage-data workload; post-fix LTO is neutral-to-slightly-positive.
Caution: blanket un-noinlining of complex dispatchers
(`perl_cplx_elem`, `perl_deref_array`) risks the bloat the original
D95 hang-fix was protecting against — un-inline the small leaves
(alloc/free/clone/to_float/to_int/num_*/is_true/array_get_ref/array_len/i64)
and keep `NoInline` on the big dispatchers.

Strategic recommendation: switch the link line to `-flto`
(thin-LTO keeps link time near today's) and apply that attribute diet.
This is confined to `declareRuntime()`'s `RT()` macro + `main.cpp`'s
link command.

## 5. Auto-vectorization / SIMD / unrolling: current state and gaps

- nb evaluates `sqrt` as `llvm.sqrt.f64` (vectorizable to `vsqrtpd`),
  and Stage 30 already fuses `$d*$d` on the sqrt critical path.
- Stage 34 attaches `llvm.loop.vectorize.width=4` hints — but only
  when `stmtIsUnboxedNumeric(body)` passes, and that predicate
  **rejects statement-context stores to array elements** (`$zpj->[$i*2]
  = $new_re` — precisely mbs's inner-loop shape) — so **there is zero
  `!llvm.loop` metadata in fibn.ll, nb.ll, or mbs.ll**. Stage 28's
  unroll (count=2 + interleave=2) likewise only fires on integer-range
  `foreach` bodies that are also unboxed-numeric.
- nb's hot loop cannot vectorize even in principle today because
  (a) rows i and j are unproven-nonaliasing (`double[7]` mallocs
  behind `PerlArray.elems`), (b) the allflat flag remains a
  per-iteration branch (unswitch never fires on the alloca-held flag
  at O2), (c) `perl_flat_row_op_assign` calls sit inside the loop
  (12.6% of nb's cycles per the profile).
- mbs's inner i-loops are already pure double arithmetic after the
  Stage-35 pair unboxing (interleaved re/im) — exactly what LV
  vectorizes — but (a) no hint (predicate above), (b) loop bounds
  (`$j < $N`) go through boxed `perl_num_lt` because sub params stay
  boxed (Stage 25's `needsFloatPrec` classifies any `+,-,*` use as
  float-needing and `floatSafe` fails on any `Call` argument — the
  same two refusals that keep fibn's `$n` boxed), (c) per-row bindings
  (`my $zj = $zzz->[$j]`) redo alloc_undef+get_ref+assign+promote+
  tag-dispatch chains each j-iteration, plus FLAT→PerlArray
  `perl_deref_array` conversions (5.9% of mbs's cycles).

Opportunistic, cheapest first:
1. **Extend `stmtIsUnboxedNumeric`** to accept flat-row element stores
   (ArrowDeref LHS, unboxed index, proven-flat RHS) and re-test the
   mbs inner loop for actual vectorization at -O3; also gate Stage 28
   unroll on the same widened predicate.
2. **Specialize whole loops on provably-flat rows**: when codegen can
   see rows come from numeric-literal arrays (`@bodies = ([...]...)` in
   nb) or `flat_fill_pair`, emit ONLY the flat path — deleting the
   s17/s18 dispatch and the allflat branch — and assert row noalias so
   LV can fire.
3. **Unbox `@_` params correctly**: relax `floatSafe`'s Call-argument
   refusal and `needsFloatPrec`'s `+,-,*` over-classification for
   provably-int variables. This turns fibn (i64 recursion) and mbs's
   `$N` comparisons into unboxed fast paths.
4. **Sub-call fast path**: direct calls with borrowed-args arrays for
   known-arity subs whose body has no `local`/`wantarray`/`caller`
   (skips array_new + array_free + push/pop_call_frame per call); skip
   the local-depth save when the sub has no `local` at all (Stage 24b
   currently also requires "no explicit return").
5. **Kill dead `perl_clone`s**: `post++` on a boxed PV clones even when
   the statement value is discarded (regex's `$count++`: measurable);
   `return fib($x)+fib($y)` clones-then-frees the temp.
6. **Regex hot path**: per the profile below,
   `perl_promote_ref_array` is 15% and `regex_compile_cached` 14% of
   regex runtime — the match loop should (a) skip/borrow instead of
   promote per element, (b) resolve constant literal patterns to a
   pre-compiled pattern at codegen time (or intern a direct pointer
   cache) instead of a strcmp-keyed cache lookup per match, (c) skip
   the `$count++` clone.

Strategic:
- LTO + attribute diet (§4) — measured today.
- A packed `double[]` 2D numeric grid ("Stage 36",
  `docs/mbs_memory_analysis.md` Phase 3) would make mbs-like kernels
  textbook SIMD targets and remove the PV boxing entirely from the
  inner loop.
- Auto-parallel `map`/counted loops (TESTS.md 2026-09-22 design) on
  top of packed kernels for mbs/nb via existing `threads`
  infrastructure.
- `-march=native` is already passed at link; `-ffast-math` stays off
  (would break byte-for-byte output gates).

## 6. Profiling summary (O2 binaries, `perf record -F 2200 -g`)

fibn (49.4 s perl / 24.5 s perlc — 2.0×):
```
16.3% perlsub_fib        (the actual recursion)
12.2% perl_free          (PV pool churn)
11.9% perl_clone
 7.6% perl_assign
 7.2% perl_num_le        (boxed `$n <= 1`)
 6.4% perl_sub
 6.4% perl_is_true
 4.5% perl_alloc_int
 4.2% perl_array_push    (@_ building)
 3.9% perl_array_len
```
→ ~84% of cycles are runtime bookkeeping, ~16% real work; exactly
what LTO inlining attacks (measured 1.52×).

nb (64.4 / 18.2 — 3.5×):
```
14.8% main               (generated loop code — the flat loads/F math)
12.6% perl_flat_row_op_assign
11.8% perl_mul
 5.7% perl_alloc_float
 5.6% perl_free
 2.1% __memset           (allocator zeroing on cold slabs)
 2.6% perl_array_get_ref + 2.5% perl_assign + 1.7% perl_div
 7.0% kernel ([unknown]) — allocator-growth/mmap churn
```

mbs (57.3 / 67.5 — 0.85×, the deficit case):
```
14.3% perl_free          10.0% perl_clone
 6.4% perl_assign         6.4% perl_alloc_int
 6.3% main               (generated code)
 5.9% perl_deref_array   (FLAT→PerlArray conversions)
 5.5% perl_add            2.9% perl_to_int
 2.0% perl_cplx_elem      1.7% perl_promote_ref_array
```
→ allocation/clone/free/deref churn; and Stage-35 CPLX_ROW packing is
a net loss here (−32% when disabled, N=256). The packed-row design
should either be reworked (skip repacking for row-bound reads, drop
per-row `perl_cplx_elem` dispatch) or default-off for this shape.

regex (50.8 / 23.2 — 2.2×):
```
18.7% libpcre2 (pcre2_match_8 + jit_match_8 + JIT code)
15.3% perl_promote_ref_array   ← hoistable
14.4% regex_compile_cached     ← strcmp-keyed cache lookup per match
 8.8% __strcmp_avx2            (cache-key comparisons)
 3.4% perl_regex_match_bool
```
→ pcre2 itself is only ~19%; ~30% is per-call lookup/promotion
overhead that a pattern-pointer cache + hoisted deref would remove.

## 7. Stage-gate ablations (O2, best-of-3)

- nb @400k: default 1.80 s vs `PERLC_OPT_DISABLE=allflat,flatdouble,stage31`
  1.81 s → **Stage 23/31 ≈ no contribution at O2**; nb's 3.5× comes
  from the always-on Stage-22 flat storage + inline GEP/TBAA loads.
- mbs @256 (post-fix): default 10.79 s vs `PERLC_OPT_DISABLE=cplxrow`
  7.30 s → **Stage 35 CPLX_ROW packing is a 48% net cost** for the
  mbs shape.

## 8. Recommendation order (gain × confidence / effort)

1. **Selective LTO + leaf-attribute diet** (§4): measured 1.52× on
   fibn-class workloads and ~9% on nb; confined to `declareRuntime()`
   + the clang link line. Keep `NoInline` on complex dispatchers; run
   `make test-all` + `make bench-regress` as gates.
2. **Regex hot path fixes** (§5.6/§6): pattern-pointer cache + skip
   per-element promote + no dead clone on `$count++`; ~30% of regex
   runtime is on the table with small, contained codegen/runtime
   changes.
3. **Loop-body flat specialization + vectorize-hint predicate widening**
   (§5.1/5.2): unlocks the SIMD story for nb/mbs kernels.
4. **`@_` unboxing + sub-call fast path** (§5.3/5.4): collapses fibn's
   45-calls-per-call; combined with (1) this is the route to >4× on
   recursion-shaped workloads.
5. **Re-evaluate Stage-35 CPLX packing** (§7): it currently costs more
   than it saves on its own motivating benchmark; either fix the
   overhead or limit it to the shapes where it demonstrably pays.
6. Keep the `mbs` rows of `bench/results.csv` suspect until re-based on
   post-D196 binaries (they measured an all-zero grid); refresh with
   a `bench.sh` run after next perf changes.

## 9. Reproduction commands

```bash
# benchmark set (all in /tmp/opencode/perf/cal — see session notes)
./perlc bench/fibn.pl -O2 -o fibn && ./fibn 37          # perl: perl bench/fibn.pl 37
./perlc bench/nb.pl   -O2 -o nb   && ./nb 4000000       # perl: perl bench/nb.pl 4000000
# regex/mbs copies: see docs note — regex3m.pl (3M×70), mbs640p.pl (N=640 + stub)

# IR census
./perlc bench/nb.pl --emit-ir -o nb.ll && grep -c "call.*@perl_" nb.ll

# LTO experiment
clang-22 -O3 -flto -march=native nb.ll src/runtime.c src/mini-gmp.c \
  src/native_stdlib.c -rdynamic -DPERLC_SELF_PATH='"perlc"' \
  -o nb_lto -lm -lpcre2-8 -lsqlite3 -latomic -ldl -lcrypto

# gates
make test-all && make bench-regress
```