# perlc — Generated-Code Performance Study, Round 2 (2026-11-04)

Sequel to `docs/optimization_findings_2026-11-04.md` (round 1). Round 1
found and landed perf items 1/3/4 (LTO link, `@_` unboxing, flat-row
binding). This round re-baselines all four ~60-second-perl workloads on
today's HEAD (`4c44eb6`), profiles at two levels, and — the main event —
**root-causes the remaining hot-loop de-optimizations and measures each
proposed fix as a patch experiment**. Two experiments measured 26× (nb)
and −36…−40% (mbs); both passed `make test-all` 609/609.

Patches used here (NOT committed; saved for the landing session):
`/tmp/opencode/perf/r2/experiment_full.patch` — the complete A+B+C patch
against HEAD (`git apply` from the repo root; note the two ST34F
env-gated debug-trace lines inside can be dropped when landing), plus
`/tmp/opencode/perf/r2/patches_exp_single_arraylit.py` (A: single-elem
ArrayLit see-through + 2D-store predicate widening) and
`/tmp/opencode/perf/r2/exp_rawidx.py` (C: raw-i64 index arithmetic)
if re-applying from a pristine tree piecewise. Nothing in the repo is
modified by this round other than the report itself.

## 1. Method

Same four workloads as round 1, each sized so **native `perl` runs
50–65 s** solo on this host (Perl 5.44, 64-core, idle-ish, sequential
runs, best-of-3 per binary):

| workload | binary args | perl (s, measured ×3: best) | source |
|---|---|---|---|
| fibn 37 | `37` | **49.31** (49.31/49.35/49.48) | `bench/fibn.pl` as-is |
| nb 4M | `4000000` | **63.46** | `bench/nb.pl` as-is |
| regex3m | — | **49.96** (runs 49.96/50.+ ) | scaled copy `/tmp/opencode/perf/cal/regex3m.pl` — 3M corpus × 70 passes |
| mbs 640 | — | **57.67** | `/tmp/opencode/perf/cal/mbs640p.pl` — `bench/mbs.pl` at N=640 with a pure-perl `flat_fill_pair` stub so real perl can run it (perlc routes the calls to its native `perl_flat_fill_pair`, same values) |

All compiled binaries' stdout was **byte-identical to `perl` at every
level** (fibn/nb/regex/mbs — mbs modulo its own timing print lines; the
`sample abs(z[0,0])=2.000000` line matched).

## 2. Where the -O levels live (fresh, post item-1/3/4 landings)

Still true from round 1, now re-verified: `CodeGen::runOptimization()` is
inert on LLVM 22 (the `-O` digit only sets the clang-22 level on the
link line, which compiles both the generated IR and the three runtime
sources), and **the emitted IR is byte-identical at -O0 through -O3**
(`diff nb(O0).ll nb(O2).ll` → 0 lines). Everything below O3 lives either
in the codegen IR (the always-on fast-path stages) or in how clang treats
that IR.

Fresh matrix (best-of-3 × 3, sequential; "2f"/"3f" = `-O2/-O3 -flto`, the
round-1 opt-in LTO link with the warm bitcode cache):

| workload | perl | O0 | O1 | O2 | O3 | O2f | O3f | best |
|---|---|---|---|---|---|---|---|---|
| fibn 37 | 49.31 | 31.69 | 10.39 | 10.49 | 10.54 | 7.87 | **7.56** | **6.5×** |
| nb 4M | 63.46 | 37.25 | 18.04 | 17.52 | 17.86 | 16.26 | **16.24** | 3.9× |
| regex3m | 49.96 | 41.75 | 25.74 | 25.47 | 25.80 | 21.20 | **20.93** | 2.4× |
| mbs 640 | 57.67 | 49.64 | 14.69 | 15.53 | 14.96 | 13.22 | **12.90** | **4.5×** |

Compile times per binary (warm caches): -O0 ≈ 2.5 s, -O1 ≈ 15 s,
-O2 ≈ 19 s, -O3 ≈ 20 s, `-flto` adds ≈ +6–7 s over the same level (the
runtime bitcode cache removes the recompile; the remaining cost is the
LTO link itself).

Readings:

- **fibn: the O0 cliff narrowed from 3.2× (78.5 s, round 1) to 1.86×**
  (31.69 s) — the Stage 36/37 de-boxing helps O0 too; with an -O2-compiled
  runtime it also shrinks further (§5, E3).
- **O1 ≈ O2 ≈ O3 within ±4%** on every workload (nb/fibn/regex/mbs). Past
  -O1 LLVM is doing almost nothing different on this call-dense IR —
  confirmed again (the -O2 vs -O1 of nb: 18.04 → 17.52, ~3%).
- **-flto remains the second cliff**: −20–27% everywhere relative to the
  same level without LTO (nb 17.5→16.3, regex 25.5→21.2, mbs 15.5→13.2,
  fibn 10.5→7.9). Since the codegen IR itself never changes, this is
  again purely "letting the small hot leaf runtime functions inline into
  call sites" — i.e. round 1's item 1, still opt-in.
- **mbs's O1 outlier earlier in the day (14.69) repeats the round-1
  finding that -O1 is sometimes the best plain level.** mbs O1 = 14.69 vs
  O2 = 15.53 (−5%); within-noise but consistently O1-favorable.

## 3. Residual cost structure (fresh profiles, `perf record -F 3000 -g`)

Full-run profiles of the -O2 and -O3-flto binaries (top symbols,
non-children), all sequential solo runs:

**fibn @O2** (10.5 s class):
```
18.8% perl_clone     17.6% perlsub_fib   12.8% perl_free
 9.3% perl_alloc_int 7.6% perl_to_int   6.0% perl_array_new
 5.6% perl_add       5.0% perl_array_len 4.3% perl_array_free
 3.0% perl_push_call_frame   2.9% perl_array_get_ref
```
→ ~95% bookkeeping; the recursion itself is a rounding error. At O3f the
inlined equivalents stay the same shape: `perlsub_fib` 50.9%
(attributed there once everything's inlined) + `perl_clone` 24.9% +
`perl_free` 14.8% + `perl_array_free` 9.4%.

**nb @O2** (17.5 s class):
```
15.0% main (generated loop)   13.0% perl_flat_row_op_assign
10.5% perl_mul   5.4% perl_alloc_float   5.1% perl_free   2.7% perl_array_get_ref
```
→ the 6 velocity updates per iteration still go through the runtime
helper (`perl_flat_row_op_assign`), because the store RHS isn't
f64-able (§4, E1) and because `$mag` itself is boxed.

**nb @O3f** (16.2 s class):
```
33.9% main   12.9% perl_flat_row_op_assign   7.0% perl_free
2.5% __memset (allocator slab zeroing)      ~10% kernel/misc
```

**regex3m @O2 / @O3f** (25.5 / 20.9 s class):
```
O2:   17.2% perl_promote_ref_array    16.3% libpcre2 (match)
     13.4% regex_compile_cached       8.8% __strcmp_avx2
      7.8% pcre2_match_8  6.0% pcre2_jit_match_8   3.3% perl_regex_match_bool
      1.3% perl_clone  1.1% perl_inc
O3f:  18.5% libpcre2  14.9% regex_compile_cached   11.5% __strcmp_avx2
      8.0% pcre2_match_8 + jit   8.0% main   2.4% clones/inc
```
→ identical conclusion to round 1 §5.6/§6 — pcre2 itself is only ~25%;
~40% is per-match cache/promote/clone overhead, all addressable with
three small contained changes (§5, E5).

**mbs640 @O2 / @O3f** (15.5 / 12.9 s class):
```
O2:   18.6% perl_free   16.5% perl_clone   15.0% main
      9.5% perl_alloc_int   9.3% perl_add   7.9% perl_deref_array
      4.3% perl_array_set   4.1% perl_cplx_elem   2.9% perl_array_set_row
O3f:  39.9% main   22.8% perl_free   17.8% perl_clone  10.6% perl_deref_array
```
→ the kernel's per-j row re-bind (`my $zj = $zzz->[$j]`) still deep-clones
the row through `perl_assign`(clone) and the D105 choke-point promote
turns around and re-boxes/converts; ~40–50% of cycles are
clone/free/deref churn on row bindings. Raw-index fixes (E2) cut the
rest.

### IR census (today's emitted IR, calls per unit)

- `fib` per call: **34 runtime calls** (was 45 in round 1's census): 9
  free + 7 alloc_int + 2 sub + 1 add + 2×(array_new/free/push) + 2×
  push_call_frame/pop_call_frame + wantarray push/pop + array_len +
  array_get_ref + to_int. With `-flto` everything on this list inlines
  (all are in the leaf-diet set) — that's the measured 10.5 → 7.6.
- `advance` (nb): **62 calls/function-in-IR**; hot j-loop per iteration ≈
  4 alloc_float + 6 mul + 6 flat_row_op_assign + 6 promote + ~17 free +
  s17/s18 GEP-dispatch chains.
- regex match loop per element: **3 calls** (array_get_ref,
  promote_ref_array, regex_match_bool) + 4 more on hit
  (promote+clone+inc+free for `$count++`).
- mbi (mbs): **198 calls/function-in-IR**, of which the kernel-1 i-loop is
  71: 29 free + 16 alloc_int + 4 mul + 4 add + 4 to_int (the boxed
  `$i*2+1` index pipelines) + 6 promote + 4 cplx_elem (dispatch fallback,
  not taken at runtime) + 2 array_set_row + 2 alloc_float.
  After E2 (raw-index) the whole function is 102 calls and the kernel-1
  i-loop is ~35.

**No `!llvm.loop` metadata exists in any of the four workloads' IR** even
post-round-1 — the Stage-34 hint predicate (`stmtIsUnboxedNumeric`)
rejects all of these kernels today. The reasons are §4's E1/E2 (plus
2D-store acceptance gaps). None of nb's velocity stores, mbs's pair
stores, or the `$mag` decls qualified. The first real vectorization is
what the experiments in §4 unlock.

## 4. What's actually making the hot loops slow — three measured root causes

### E1 (measured, 26×): single-element paren groups are ArrayLit, and every numeric fast path refuses them

The parser keeps `(expr)` as an `NK::ArrayLit` node with one element —
deliberately, for D95's `(f()) x N` list-repetition recognition
(`parser.cpp:5456-5473`: *"a single-element `(expr)` is kept as ArrayLit
(not unwrapped) so `(f()) x N` is recognized as list-repetition"*).

In **value** positions a one-element list without a trailing comma is
observably exactly its element (scalar context keeps the last), but
`canEmitF64` / `canEmitI64` / `emitExprF64` / `emitExprI64` / `emitIdx`
all fall through `NK::ArrayLit` to `false`. Measured consequences, all
verified in IR/ASM:

- nb `advance`: `my $mag = $dt / ( $distance * $distance * $distance )` —
  the paren group → ArrayLit → **`$mag` stays a boxed PV** (ST25 trace:
  `ST25 'mag': ... F64=0 I64=0`), so every velocity update's RHS
  (`... * $mag`) is not f64-able → the 2D compound-assigns take the
  **boxed** `perl_flat_row_op_assign` runtime path instead of the s18
  inline GEP store, and the Stage-34 vec-hint predicate rejects the whole
  j-loop. One paren pair costs nb ~26×:
  `benchmark run: 17.52 s → 0.67 s at -O2 (no LTO!), 16.26 → 0.56 at
  -O2-flto` — the hot loop becomes raw scalar math with packed 2-double
  stores (vsubpd/vmovupd), 94% of cycles in `main` (the physics itself),
  runtime bookkeeping 0.65%+0.41% (down from ~40%).
- Any other kernel declaring temporaries from paren-grouped arithmetic
  (`my $x = (a*b) + c`) is silent-invisible in the same way.

**Fix shape (the experiment patch):** a `singleArrayLitElem()` helper at
the top of `canEmitF64`/`canEmitI64`/`emitExprF64`/`emitExprI64`/`emitIdx`
(value positions only; the AST is untouched, so D95's `(f()) x N` and
list contexts keep their semantics). Gate: full `make test-all` —
**609/609 PASS** on the experiment build; bench outputs byte-identical.

**Caveat for landing (must be handled + tested):** unwrapping paren groups
into the F64 path moves `/` from `perl_div`'s die-on-zero (exit 255,
`runtime.c:2811`) to unboxed `fdiv`. Verified pre-existing divergence —
**today's compiler already prints `Inf` and exits 0** for
`my $y = ($a)/($b)` with `$b == 0` where real perl dies — but the
experiment widens the affected surface (paren-grouped divisors). The
unboxed `/` emit site should keep a zero-divisor check (a branch to a
catchable die, following `perl_mod_i64`'s pattern) or restrict the unwrap
for `/`/`**` divisors. Add `tests/div_zero_paren_{smoke,deep}.pl` when
landing.

### E2 (measured, −35…−40% on mbs): the promised raw-i64 index arithmetic isn't wired

`emitIdx`'s own comment (2026-11-04) says *"pure-i64 arithmetic index
(e.g. `$i * 2`) emits plain i64 math… an i64-wrapped index is simply an
out-of-bounds element, which for an index position is unobservable"*, and
the code says "emitExprI64's own operand checks" — but `canEmitI64`'s
arithmetic BinOps were **deliberately reverted** (D103 exactness at VALUE
positions), so `emitExprI64($i*2+1)` returns null and every odd-element
index `[$i*2+1]` in a flat/pair row pays the full boxed pipeline
(smul.with.overflow + alloc_int + alloc_int(1) + perl_add + perl_to_int +
2 frees — 8+ calls per access; visible in the kernel IR and, in the old
build, 16 alloc_int + 4 mul + 4 add + 4 toi_int + 29 free per
kernel-loop iteration).

**Fix shape (the experiment patch):** a local `raw64` walker in `emitIdx`
(IntLit/ScalarVar-int/`+ - *`/`<<`-with-small-const, recursion, gated by
`isOptStageEnabled("rawidx")` so a disable knob exists). Index positions
are OOB-safe under wrap per the function's own comment; VALUE positions
keep D103's exactness.

Measured: mbs640 **15.53 → 9.39 s at -O2 (−40%), 12.90 → 8.40 s at
-O2-flto (−35%)**; nb unchanged (its indexes are plain vars);
`make test-all` **609/609 PASS** on the A+B+C build. mbi's IR census:
198 → 102 calls (kernel i-loop 71 → 25).

### E3 (measured, ~2× on O0): mixed-level linking shrinks the O0 cliff

Verified first: the emitted IR is **byte-identical** at -O0/-O2, and the
standard single-invocation link's runtime.o is equivalent to a prebuilt
`clang-22 -O2 -c runtime.c` object (linking nb's IR at -O2 with the
prebuilt objects measures the same 17.58 vs 17.55 as the standard link).

| nb build | time |
|---|---|
| standard -O0 (IR **and** runtime compiled at O0) | 37.25 s |
| **mixed link: IR compiled -O0 + runtime.c prebuilt -O2** | **19.34 s** |
| standard -O2 (both TUs at O2) | 17.55 s |
| mixed: A+B+C IR -O0 + runtime -O2 | 2.21 s (the same de-boxed IR measures 0.68 s at -O2) |

→ Half the O0 cliff is just "runtime.c compiled unoptimized" (its PV
pool, `perl_free` teardown ladder and `pv_alloc` stay out-of-line and
uncombined). A `-O0` mode that links a **prebuilt -O2 runtime** (exactly
the round-1 `-flto` bitcode cache, linked at `-O0` from `.o` files
instead) narrows O0 to within ~10% of O2 with zero IR changes and zero
correctness risk (separate TUs, standard multi-level linking). With A+B
it goes to 2.21 s (the same binary that measures 0.68 at -O2).

### Vectorization state (what SIMD actually fires today, and after E1/E2)

- Baseline binaries: **0 `!llvm.loop` metadata anywhere**; disassembly
  shows scattered SLP-style packed stores (current nb O3f:
  15× vsubpd + 1× vaddpd + 1× vmulpd + 71× vmovupd across
  perlsub_advance; sqrt scalar vsqrtsd) — LLVM packs pairs of
  independent stores but no loop-level vectorization of the kernels.
- After E1 (+ the 2D-store vec-predicate widening), nb's j-loop carries
  `!llvm.loop` metadata and the compiled loop is raw scalar+SLP math:
  benchmark-level 26×.
- After E2, mbs's kernel remains non-vectorized (the per-j row re-bind +
  promote + if-abs2 dispatches resist LV) — mbs's SIMD story needs E4
  (below) plus LLVM-level noalias evidence, not just hints.

## 5. Landing plan for the perf queue (flipped 2026-11-05)

**Landed (2026-11-05, commit "E1+E2"):**

- ~~#1 E1~~ **LANDED**: single-elem ArrayLit see-through in the five
  numeric paths (`canEmitI64`/`emitExprI64`/`emitIdx`/`canEmitF64`/
  `emitExprF64`) + 2D-store/compound gates in `stmtIsUnboxedNumeric` (both
  Assign and CompoundAssign branches; call-shaped RHS refused). With the
  zero-divisor guard (see below) measured: nb 17.52 → **0.70 s** at -O2,
  16.26 → **0.64 s** at -O2flto (≈26×); all others byte-identical.
- ~~#2 E2~~ **LANDED**: raw-i64 index arithmetic in `emitIdx` (gated
  `rawidx`, `PERLC_OPT_DISABLE=rawidx` disables). mbs640 15.53 →
  **9.5–9.8 s** at -O2 (−37%); nb unchanged. Verified: mbs640 sample
  line matches perl exactly.
- **Zero-divisor guard on unboxed `/`** (E1's landing caveat, §4/§6):
  every unboxed division site (value position in `emitExprF64`, the
  float-var compound-assign path, and the three s18/s18-PV* `applyF64`
  lambdas) emits one ordered `fcmp oeq div, 0.0` + branch to a shared
  per-module cold helper (`__perlc_div_zero_die`, internal linkage,
  NoReturn) that calls the catchable `perl_die` with the real source
  line — byte-compat with perl including the `...at FILE line N.` text,
  `$@` catchability, and the exit-255 finale. **LTO lesson (measured
  16.2 s vs 0.64 s on nb):** building the die message per site (a
  `perl_alloc_string_len` call + per-site global string inside a
  NoReturn-cold block) catastrophically defeats clang's post-link
  inliner/vectorizer decisions; a single shared helper taking just the
  line number keeps everything sane. Guard cost on nb: −O2 0.67 →
  0.70 s, −O2flto 0.57 → 0.64 s (+7…12% for the soundness check; every
  other benchmark division-free in the kernel → unaffected). Non-zero
  constant divisors skip the check at emit time.
- **Tests**: `tests/div_zero_paren_{smoke,deep}.pl` — value position,
  float-var compound, array-ref compound, 2D literal-idx compound, int 0,
  −0.0, literal `(0)`, mid-loop die with partial-state pin, nested evals,
  chain order, exact `$@` location text, and the uncaught exit-255 finale;
  611/611 PASS; `bench/regress.sh` PASS with the refreshed baseline.

**Remaining queue, re-ordered post-landing:**

| # | item | gain (measured/est.) | effort | risk |
|---|---|---|---|---|
| 1 | **E3**: -O0 mixed-level link (runtime prebuilt at -O2 via the existing LTO bitcode cache, even without LTO) | O0 ≈ 2× faster; unblocks fast -O0 builds | small (driver only) | none observed — TUs are independent; keep opt-in for miscompile-debug workflows |
| 2 | **E5 (regex, round-1 item 2)**: (a) pointer-keyed first-check pattern cache — literal patterns are stable per-callsite globals; (b) skip the D105 promote on a match-subject read; (c) statement-context `$count++` without the clone/free pair | regex profile: ~35–40% of cycles (17% promote + 13–15% cache + ~4% clone/inc/free per match) | small-medium | pointer-cache soundness (fall back to strcmp); promote-skip needs the capture-install path audited. NOTE: a work-in-progress memo-slot + promote-inline patch was seen mid-flight in the working tree on 2026-11-05 and set aside — see `/tmp/opencode/perf/r2/wip_{runtime,codegen}_regex_memo.patch`; finish/redo on top of this landing. |
| 3 | **E4 (mbs row borrow)**: `my $zj = $zzz->[$j]` under an alias-safe borrow rule — skips per-row deep clone + subsequent promote conversion | profile: clone 17.8% + free 22.8% + deref 10.6% at O3f → est. **1.3–1.6×** on mbs-class kernels | medium (ownership analysis + borrow flag) | aliasing correctness — needs the D105/D196 alias tests + new borrow tests |
| 4 | **fib-class recursion**: borrowed-args direct calls for known-arity subs (skip array_new/push/free + frames + wantarray pair per call) | fibn O3f profile: clone 24.9% + free 14.8% + array_free 9.4% ≈ halving the remaining ~7.6 s → est. **1.4–1.8×** | medium | dynamic-dispatch/capture poisoning rules (Stage 37's pre-pass classes) |
| 5 | Default-on `-flto` at O2+ (or ship -flto=thin by default) | −20–27% everywhere vs same level without | tiny (driver default flip) | full-LTO link cost +6–7 s w/ cache; measure on the big-IR worst offenders; re-verify after the guard landing (the LTO-inliner sensitivity the div-guard hit is a probe for this whole class) |
| 6 | Keep `-march=native`; `-ffast-math` stays off (byte-exact gates) | — | — | — |

Strategic (unchanged direction from round 1, re-prioritized):

- **Provably-flat loop specialization** (round-1 §5.2) remains the
  requirement for mbs/nb-class **loop vectorization** (today only SLP
  fires); E1/E2 shrink the surrounding overhead but the kernels still
  carry row re-binds, tag dispatches and the if-abs2 branch that block
  LV. With flat specialization + `noalias` evidence, mbs's kernel is a
  textbook vectorization target; combined with E4 it approaches the
  "packed grid, zero PV boxing" end-state (round-1's Stage-36 note).
- **Auto-parallel map/counted-loops** (TESTS.md 2026-09-22 design) on top
  of specialized kernels for mbs/nb via the existing threads infra.
- D197 (`defined()` after INT promotion) and D198 (wantarray ctx) remain
  the open unboxing-safety classes gating E5/E4's promotion machinery.

## 6. Correctness notes surfaced by this round (for TESTS.md)

- **D197/D198 remain OPEN** (round-1 logged; TESTS.md rows 180/181):
  `defined($param)` after Stage-25 INT promotion lies (`U`→`D`), and
  `wantarray()` may read a stale LIST ctx in some multi-sub arg-collection
  shapes. Both are unboxing-safety classes: the fixes belong with E1/E6's
  landing (same predicate machinery), not separately.
- **Division-by-zero on unboxed `/`** — real perl dies (exit 255);
  perlc's unboxed fdiv (and today even some boxed paths through parens)
  silently yields `Inf`. Pre-existing (verified on both the current and
  the experiment binaries); **landed 2026-11-05** with E1: every unboxed
  division site now branches to a catchable `perl_die` with the real
  source line (see §5); `tests/div_zero_paren_{smoke,deep}.pl` pin it.
  The boxed paths keep their pre-existing behavior (see D199's
  scoped-out notes) — still open as a separate item.
- `bench/results.csv` is stale for all rows (it mixes the pre-D196
  degenerate-mbs workload and the old fibn sizes); `bench/baseline.csv`
  (the regress.sh gate) reflects the PRE-Stages-36/37 binary — verified
  today (`./bench/regress.sh` PASS: fibn −55% vs its recorded baseline
  (150 ms vs 333 ms @N=28), nb −4.6%, regex −2.6%): nothing regressed,
  but re-record with `./bench/regress.sh --fix` after the next landing.

## 7. Reproduction

```bash
cd /tmp/opencode/perf/r2            # everything lives here
bash summary.sh                     # consolidated tables (A: matrix, B: experiments)

# baseline matrix per benchmark (col.sh does perl x3 + 6 perlc builds x3 runs)
bash col.sh fibn bench/fibn.pl 37
bash col.sh nb bench/nb.pl 4000000
bash col.sh regex3m /tmp/opencode/perf/cal/regex3m.pl
bash col.sh mbs640 /tmp/opencode/perf/cal/mbs640p.pl

# experiment build (patches A+B+C; ST25/ST34F env-gated debug tracing inside)
python3 /tmp/opencode/perf/r2/patches_exp_single_arraylit.py  # (A+B, from repo root) — then make
python3 /tmp/opencode/perf/r2/exp_rawidx.py                   # (C, on top) — then make
./perlc /tmp/opencode/perf/st34/h.pl --emit-ir -o t.ll        # probe kernels

# per-op IR census
for f in fibn nb regex3m mbs640; do grep -oE "@perl_[a-z0-9_]+" $f.ll | wc -l; done
grep -c 'llvm.loop' *.ll            # 0 today everywhere; nb gets 2 with E1

# profiles (O2 and O3f)
perf record -F 3000 -g -o p.data -- bin_nb/O2 4000000
perf report -i p.data --stdio --no-children -g none

# E3 mixed link
clang-22 -O2 -march=native -c runtime.c -o rt.o     # + mini-gmp/native_stdlib
clang-22 -O0 prog.ll rt.o mg.o nst.o -o mixed ...    # the E3 shape

# gates
make test-all && make bench-regress
```
