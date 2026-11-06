# perlc — perf round 2 landing (2026-11-05, worktree `perf-r2-landing`)

Lands the round-2 experiments (E1–E6) plus three new optimization stages
measured on the same four ~60-s-real-perl workloads. Every landed piece is
covered by `make test-all` (609 base tests + 2 new div-zero contract tests)
run against real perl, byte-for-byte.

## What landed in this worktree

| piece | gate | measured (plain -O2, best-of-3) |
|-------|------|--------------------------------|
| E1/E2 (experiment_full.patch) | already committed to main as `301b2d8` | — |
| Stage 38: D105 promote inline (`emitPromoteInline`) | always-on | nb4M 17.7→0.7 (this alone was the whole nb story) |
| Regex per-call-site pinned-pattern memo (E5a) | `regexMemoSeq_` slots; runtime pins + `perl_cleanup` frees | regex3m 25.4→17.0 |
| Stage 39: row-element de-boxing (E4) | `PERLC_OPT_DISABLE=cplxrow,st39` | mbs640 9.07→1.43 |
| Stage 39: float compare de-box (E5b) | `PERLC_OPT_DISABLE=st39` | (part of the above) |
| D186-class shadow rule (canEmitF64/emitExprF64) | always-on correctness fix | fixed loop_var_shadow; cost: none on benches |
| Stage 40: known-arity `_ai` direct calls (E6) | `PERLC_OPT_DISABLE=st40` | fibn37 10.0→4.41 |

## Final numbers (all outputs byte-verified vs perl 5.44)

At plain `-O2` (best of 3, sequential): `fibn.pl 37` 4.87s (perl 49.3 —
10.1×), `nb.pl 4000000` 0.72s (perl 63.5 — 88×), `regex_3m70.pl` 17.08s
(perl ~50 — 2.9×), `mbs640p.pl` 1.43s (perl 57.7 — 40×).

With -O3+flto: 3.21 / 0.54 / 17.04 / 1.40 (the lto bitcode cache made the
link ~24s/benchmark after the first cold ~50s). regex3m is pcre2-bound —
after E5d its profile is ~90% inside libpcre2, so LTO can't help it
further without a different match strategy.

## Mechanism notes (for the record)

1. **nb (17.7→0.7)**: the D105 promote call (`perl_promote_ref_array`,
   NoInline) sat on every scalar-var/element read in the 17-deep loop
   body; as an unknown-effect call it killed GVN/LICM of the flat-row
   caches. Inlining the promote's common case (2 tag
   compares + branch) restored the loop to pure arithmetic: 62
   `perl_*` calls in `advance` → 14.
2. **regex (25.4→17.0)**: per-element regex matching in a 3M×70 loop
   still paid the LRU pattern cache (key build/strcmp/LRU promote) per
   match + the D105 promote call per subject read. Literal patterns now
   get a per-call-site pinned memo slot (`re.memo.N`) — compiled once,
   straight to `pcre2_match`, freed in `perl_cleanup`; the promote is
   the Stage-38 inline.
3. **mbs (9.07→1.43)**: at runtime every row was a REF_ARRAY-backed
   borrow (332.6M no-op `perl_deref_array` calls); ALL element access
   was boxed — 200M `perl_cplx_elem` reads, 132.7M
   `perl_alloc_float`+`perl_array_set_row`+`perl_free` writes (with
   `perl_array_set` doing free(old)+clone(new) per store — 220M float
   clones!). Stage 39 adds REF-row inline read/load/store to the
   existing pair machinery, zero-box void-context stores, and a raw
   fcmp for the `abs2 < 4` test. RT churn collapsed: clone 335M→3.0M,
   free 569M→198M, setrow 132.7M→762, cplx_elem 200M→156K, deref
   332M→159K. Ablation on the landed tree
   (`PERLC_OPT_DISABLE=st39,cplxrow`): 17.4s = the pre-Stage-39 class.
4. **fib (10.06→4.87 at -O2; ablation PERLC_OPT_DISABLE=st40 → 10.06)**:
   recursive `fib($n-1)` calls are i64-expressible
   but canEmitI64 rightly refuses arithmetic (D103 overflow); Stage 40's
   call sites emit `llvm.ssub.with.overflow` and pick the `_ai` direct
   call on the no-overflow arm, re-routing the overflow arm into the
   full boxed generic call. Per call: args array_new/push/array_free +
   2 arg boxes + probing are gone; frames + wantarray pair stay exact.

## Soundness rules baked in (each with a test or a logged D-row)

- Unboxed `/` keeps perl's division-by-zero die (catchable, `$@` =
  `Illegal division by zero at FILE line N.\n`, −0.0 divisor dies, NaN
  divisor does not) — `tests/div_zero_paren_{smoke,deep}.pl`.
- REF-row in-place element stores are observability-equal to
  box-replace (element boxes are never aliased outside the row; reads
  copy values; `\$row->[k]` lvalues see the same mutation, matching
  real perl).
- The `_ai` qualification rejects captures, `\&ref`/typeglob aliasing,
  dynamic dispatch, prototypes, extra/missing-arg arities, non-contiguous
  unpacks, `local()`, implicit-@_ consumers (`shift` etc.), and any other
  @_ reference; wantarray is passed as a ctx parameter per call.
- canEmitF64/emitExprF64's file-global fallback must never beat a
  nearer lexical (D186-style); the file scope itself (same storage) is
  still allowed.

## Stage 41 vectorization evidence (2026-11-05, this session)

Built and landed the "flat-row kernel dual-emit" engine (gated by
`PERLC_OPT_DISABLE=st41`): for a qualifying counted loop (`my $i = 0; $i <
BOUND; $i++`, body = pure pair-arithmetic over row variables bound in the
enclosing iteration), the compiler emits the loop twice behind a
per-iteration prelude that AND-proves, per row variable:

- `tag(pv) == PERL_FLAT_ARRAY` (element-indexed packed doubles),
- `pv->matchpos >= 2 * bound` (every loop index `(2i | 2i+1)` is in range),
- the rows' packed data pointers are pairwise distinct (WAR versioning —
  read rows / written rows never overlap; LLVM's dependence analysis
  consumes this branch directly),

then runs a FLAT body (direct indexed GEPs, no tag load, no promote, no
bounds check, no runtime call, plus the counted-loop vectorize hint) with
the GENERAL body (today's verbatim machinery) as the fallback.

**What blocked the actual vectorization, measured on mbs640:**

1. **The rows are not packed at runtime.** Instrumented probe: every row
   value stored into `@z`/`@zzzp` is a REF_ARRAY (1,280 REF-tagged
   row-stores; the fill's `flat_fill_pair` returns `\@r` — an anon PerlArray
   wrapper — and every boxed element store inside it materializes real PV
   boxes). The prelude's tag check is false for every iteration, so the
   general body (already call-free after Stage 39) runs and FK is a
   no-op there. Checksum-verified: identical field values with and without
   st41 and identical to real perl.
2. **Even the literal-row probe (tag-10 flat rows, 16-element rows, the
   exact qualification shape) doesn't vectorize at -O3+flto**: the only
   vector ops in the binary are the zero-initialization fills (157
   `vmovupd` = memset patterns, none in the loops). No `!llvm.loop`
   metadata attached because the counted-loop vectorize hint predicate
   (`cforIsCountedNumeric`) doesn't classify this body shape either.

**What the packed-rows lever would take (the honest design ledger):**

- *Representation*: rows whose backing is a refcounted packed buffer
  (mirroring `PerlCplxRow`'s alias model: every PV alias shares the
  buffer; writes through any alias are visible to all; derefs
  materialize-on-demand). The PV-cell shape can stay `{tag, pval(double*),
  matchpos}` with the refcount in a malloc header — that keeps every
  codegen fast path unchanged and makes the D105 promote an O(1)
  header-ize instead of an unpack.
- *Fork hazard*: any representation conversion (packed → boxes or
  boxes → packed) while multiple aliases share the value forks the
  view. Today this is managed by the D105 promote being the single
  conversion point (unpack at the first handout, then everything is
  REF-aliased, which is why the kernel ends up boxed). A packed world
  needs the same discipline in reverse and a loudly-documented D-row for
  multi-holder materialization.
- *Once rows are packed*: the Stage-41 engine above becomes live for
  these loops; the remaining LV conditions are the uniform branch
  (`if (abs2 < 4.0)`, if-convertible) and pointer-distinctness (the
  prelude already proves it).

## Integration record (2026-11-05)

- Landed in this worktree from parent `4c44eb6`, then main (`301b2d8`,
  = E1/E2 + the div-zero die) was merged in; the conflict resolution
  kept main's div-zero variant (the shared `__perlc_div_zero_die`
  noreturn helper + constant-nonzero-divisor elimination) and this
  tree's stages/tests. Main fast-forwarded to the merge commit
  (`bf2ea88`) and re-passed both gates there (test-all 611/611,
  bench-regress PASS).
- `tests/lib/E/Tagged.pm` was untracked (the `.gitignore` `lib/`
  pattern); now tracked (`git add -f`, commit `48ec79d`) so fresh
  clones/worktrees pass test-all.
- bench.sh's recorded rows were being measured at the implicit -O0
  default; it now compiles at `-O2` (matching regress.sh) and carries
  ~60-s shapes (regex_heavy 3M×70 via args; mbs stubbed at N=640).
  `bench/results.csv` refreshed accordingly (commit `b73cdca`).

## Worktree / merge notes (original state before integration)

- This worktree sits at parent commit `4c44eb6` with one uncommitted
  landing patch. Main repo moved to `301b2d8` (E1/E2 + a slightly
  different div-zero guard: constant-nonzero elimination + one shared
  `__perlc_div_zero_die` noreturn helper). On merge: take main's
  div-zero variant, drop this tree's duplicate site guards
  (`emitDivZeroGuard` differs only in IR shape), keep everything else.
- Main repo's `tests/lib/E/Tagged.pm` is untracked (the `.gitignore`
  `lib/` pattern) — a fresh clone/worktree fails
  `exporter_tags_{smoke,deep}.pl` until it's committed with a
  `tests/lib/**` un-ignore or explicit `git add -f`.
- `bench/baseline.csv` and `bench/results.csv` must be re-recorded
  after this lands (`./bench/regress.sh --fix`, then `bench/bench.sh`).

## E3 mixed-level `-O0` link + default-on `-flto` (2026-11-05, this session)

Completes the last two small items of the round-2 queue (§5 items 1 and
5), both driver-only changes in `src/main.cpp`:

- **E3 — prebuilt `-O2` runtime objects at `-O0`** (the mixed-level
  link): the three runtime sources are compiled once with
  `clang-22 -O2 -march=native -c` into a small asset cache
  (`src/.perlc_lto_cache` — the LTO cache directory; `make clean`
  clears it) and linked as plain objects into every `-O0` binary. The
  generated IR still compiles in the same fast `-O0` pass; the runtime
  ships pre-optimized. `PERLC_NO_PREBUILT_RT=1` links the runtime from
  source (full `-O0` binary, for debug-into-the-runtime /
  miscompile-repro workflows). Assets are suffix-distinguished per
  flavor and debug state (`.o2.o` / `.o2.g.o`; `-g` users get
  `-g`-compiled runtime objects), keys are
  (basename, size, mtime, self-path-hash, `.v2` schema), pruning never
  crosses flavors, and concurrent perlc processes share the cache via
  atomic renames (the LTO cache's existing discipline).
- **Default-on `-flto` at `-O2+`**: every `-O2`+ build LTO-links unless
  `-noflto` / `PERLC_NO_LTO=1` / `PERLC_LTO=0|off`; explicit `-flto`
  still forces it at `-O1`; `PERLC_LTO=thin` selects the summary-based
  pipeline (measured: nb `-O2` thin compile 22.9 s vs full 25.3 s —
  cheap link, weaker inlining; full stays the default).
- **D202 (see TESTS.md) — cached LTO assets now bake `PERLC_SELF_PATH`**: a
  link-line `-D` never reaches precompiled bitcode inputs, so the old
  cached bitcode was built without the D24 define and every LTO binary
  silently carried the `"perlc"` fallback (verified: the pre-fix binary
  FAILs `test_do_filename.pl` under `PERLC_FLAGS="-O2 -flto"`, PASSes at
  the from-source level). The asset builder now passes the identical
  define at compile time, and the `.v2` key bump discards (never
  reuses) pre-fix, define-less cached bitcode.
- **Trace builds skip the cache**: `make TRACE=1` compilers
  (`-DPERLC_GAP_TRACE`) link the runtime from source at every level —
  the detector's define must reach the runtime compile.
- **Usage text fixed**: the CLI default was (and is) `-O0` — the
  emitted IR is byte-identical at every level, only the clang link
  level differs; the old usage claimed "default: 2".

### Measured (same shapes as the landing headline, warm caches)

Compile times (best-of-1):

| | before (d296301) | after |
|---|---|---|
| `-O0` compile, any program | 2.32–2.46 s | **0.2–0.3 s (~9–11×)** |
| `-O0` w/ `PERLC_NO_PREBUILT_RT=1` | 2.32 s | 2.31 s (parity) |
| `-O2` from-source (`-noflto`) | 19.3 s | 19.2 s (parity) |
| `-O2` default (now LTO) | 25.2–30.8 s as `-O2 -flto` | 25.2–30.8 s (parity) |
| `-O2` `PERLC_LTO=thin` | — | 22.9 s |

Runtimes (best-of-1, sequential):

| workload | -O0 before | -O0 after (E3) | -O2+LTO before/after |
|---|---|---|---|
| fibn 37 | 13.56 s | **5.24 s (2.6×)** | 3.30 / 3.26 s |
| nb 4M | 2.27 s | 1.95 s (1.16×) | 0.66 / 0.67 s |
| mbs 640 | 4.22 s | 3.89 s (1.08×) | 1.43 / 1.43 s |
| regex 3M×70 | 24.44 s | **16.56 s (1.48×)** | 16.55 / 16.88 s |

The old `-O0` cliff is mostly closed: the generated IR is
level-independent, and the previously per-compile `-O0` runtime build
was the bulk of the gap — `-O0` runtimes now land at (regex) or near
(fibn 1.6×) the `-O2`+LTO numbers, with `-O0` compiles ~9–11× faster.
Outputs byte-verified vs perl 5.44 at both levels (mbs's own four
`Time::HiRes` self-timing lines excluded, as at landing).

### Gates

- `tests/harness.sh --assertion-only` (incl. the do-FILE
  self-reinvocation path through both new asset flavors) PASS at the
  default level and at `OPT_LEVEL=2`; `dbi_sqlite.pl` unchanged on
  pre- and post-change binaries at both levels (pre-existing
  external-deps gap, skip-by-default in `test-all`).
- `make test-all` (611 tests, default level = the E3 path) **611/611
  PASS in 4m01s** (per-test compile ~2.3 s → ~0.25 s via the cached
  objects).
- `bench/regress.sh --fix` re-recorded with `BENCH_OPT=-O2` now
  measuring the shipped default (LTO-on), then PASS.

### Notes / follow-ups

- The remaining `-O0` vs `-O2` runtime gap is clang's per-program
  optimization of the produced IR itself (loop opts/lifetime on the
  `.ll`); E3 by design changes only how the runtime is compiled and
  linked.
- The `o2-obj` assets could also serve `-O1` (and plain `-noflto -O2`)
  to remove the ~17 s from-source runtime recompile there — a one-line
  driver flip, not done now: plain `-O2` is the LTO path by default and
  `-O1` is unused.
- Round-2 queue state after this session: items 1 (E3) and 5 (-flto
  default) done; item 6 (-march=native keeps) already standing policy.
  Remaining strategic: the packed-rows representation (Stage 41's
  lever), auto-parallel counted loops, and the D197/D198
  unboxing-safety classes.

## Stage 42 — packed rows + live flat-kernel prelude (2026-11-06, this session)

The strategic "packed-rows" item, landed. The design ledger's
representation already existed — `PERL_CPLX_ROW` (Stage 35) IS the
refcounted packed-buffer row model (clone = refcount++ share, writes
through any alias in place, promote skips it) — so the work collapsed
to three contained pieces:

1. **Producers** (`packflat`, gated `PERLC_OPT_DISABLE=packflat`): a
   sub's `return \@r;` — when a codegen scan proves @r is a sub-local
   row-fill array with no alias escapes outside return positions — packs
   the boxed interleaved numeric row into a refcounted CPLX_ROW
   (`perl_pack_flat_row`) before the hand-out. Every refusal path
   returns the plain REF hand-out byte-identical (odd length /
   non-numeric elements / thread-shared / refcount > 1 / mixed int
   halves — the last carried by D154 bits on the row PV and honored on
   re-boxing). The alias contract needs NO promote at the hand-out
   (promote already skips CPLX_ROW): clones share the packed buffer
   exactly like REFs share a PerlArray.
2. **FK prelude over packed rows**: certifies FLAT (`>= 2*bound`
   elements) AND CPLX (`>= bound` pairs) rows, refuses thread-shared
   rows, and replaces the all-pairs data-distinctness with role-aware
   WAR (writer-vs-anything distinct; reader/reader sharing is
   conflict-free — required by the mbs shapes, where the c rows alias
   the z rows). Row pair-load/store emitters resolve the packed buffer
   once (`fkKernelData_`) so FLAT and CPLX rows share one body.
3. **Pre-existing Stage-41 bug fixed**: the If arm's relational
   condition was handed to `fkNoRowTouch` (which only accepts +-*/), so
   every `if ($abs2 < 4.0)`-shaped kernel was structurally unreachable.
   The operands are checked instead; kernel-2 class certified live.

### Measured (mbs640, plain -O2 default build)

| config | mbs runtime |
|---|---|
| old tree (= 8cd0c4b) | 1.43 s |
| Stage 42, packflat on, FK off | **1.43 s** (rows packed, zero regression) |
| Stage 42, everything on (enable-only hint) | **1.44 s** |
| Stage 42, **width=4 forced hint** | 3.98 s (reverted — see below) |

Outputs byte-verified vs perl 5.44 in every configuration. New tests
`tests/packflat_{smoke,deep}.pl` byte-identical (alias-share through
packed rows, element round-trips, IV-hint re-boxing, refusal fallbacks,
kernel-1/kernel-2 agreement, push hand-out aliasing).

### The hint finding (recorded for the follow-up)

Forcing `llvm.loop.vectorize.width=4` on the certified flat-loops
REGRESSED mbs ~2.8× at -O2+LTO — the vector pipeline displaced the
SLP-packed scalar code the general path already gets (357 vs ~992
vector ops in the LTO'd binary). The landed hint is `enable`-only,
measured neutral: LLVM's LV currently declines these loops anyway
("control flow cannot be substituted for a select" / pass-ordering —
captured via `-Rpass-analysis=loop-vectorize`), so the flat body runs
and the SLP-packed general code is preserved. The actual SIMD win now
hinges on clearing THAT LV refusal (loop-versioning/if-convert ordering
over the certified data pointers) — the next LLVM-side iteration, not
a codegen-side one.

### Strategic state after Stage 42

- Packed rows: **landed, live, no regression** — producers emit packed
  rows and every consumer (Stage 35/36/39/41 machinery) handles them.
- Flat-kernel dual-emit: **live over packed rows** (both kernel
  classes), byte-exact, neutral today pending the LV work above.
- **D197 fixed** (2026-11-06, TESTS.md row): the defined()-observer
  refusal in the Stage-25 ladder + Stage-37 pre-pass (with the
  pre-existing ternary-condition walk gap found and fixed in the same
  pass — the ladder's binOp walk never reached `n->cond`). One of the
  two open unboxing-safety classes now closed.
- Auto-parallel counted loops: unchanged from the 2026-09-22 design
  note (TESTS.md §"Future: auto-parallel"); its precondition ("a packed
  FLAT/CPLX C worker over double[]") is now materially closer.
- D198 (ternary context propagation, see Stage 44 below) — the last
  open unboxing-safety row — closes same-day (2026-11-06).

## Stage 43 — FK kernel store/if-conversion: filling the LV refusal (2026-11-06, this session)

Root-caused on the round-2 isolated-module
setup (`llvm-extract --func=perlsub_mbi` + `opt -passes="lto-pre-link<O2>"
-pass-remarks-analysis=loop-vectorize`), the certified flat loops declined
for three stacked reasons, of which two were codegen-side:

1. **Kernel-1's flat body carried a per-iteration row tag-check branch and
   two boxed store-value allocations** (`perl_alloc_float` per store via
   the Stage-39 `emitRowElemStoreFast` void-store route, which intercepts
   before anything asks whether the row is a certified FK row). Postlink,
   with the runtime leaves inlined, this branch+call ladder is what LV
   actually refused; the general variant's ladder is the same shape, so
   cleaning only the flat variant is observability-free.
2. **Kernel-2's uniform `if (abs2 < 4.0)` reached LV as an if/else pair of
   stores** — the "control flow cannot be substituted for a select"
   refusal — at least in the prelink pipeline; postlink LLVM if-converts
   that much by itself once the stores are clean.
3. LV's own pointer-group memcheck handles the rest (the fk prelude's WAR
   proof is informational for LV; its own versioning is what fires).

Fixes (both inside the guarded flat re-emission only, `PERLC_OPT_DISABLE`
knobs kept):

- **`tryFkRowElemStore(n, needValue)`** — certified FK rows store directly
  into `fkKernelData_[row]` (no promote, no tag check); void-context
  statement stores skip the result PV entirely (the alloc_float call lived
  inside the loop and blocked LV by itself). Routed from
  `emitUnboxedNumericVoid` (kernel-1's void stores) and the
  chain-assign FK branch (deduped, value-context keeps the boxed hand-back).
- **`tryEmitFkIfConvert`** (`fkifconv` knob) — inside the certified body,
  an if/else whose arms are *only* pairwise-(row, index)-identical
  element stores of row-pure f64 values (no `/` anywhere — the unboxed
  zero-divisor die must stay exactly where the taken-arm evaluation puts
  it) emits as per-element `select` + one unconditional flat store; the
  condition is the same ordered fcmp the general path builds
  (`tryEmitI1Cond`), so NaN semantics are unchanged.

### Measured (mbs640, plain -O2 default build, byte-verified vs perl)

| config | mbs runtime | perlsub_mbi packed-double ops | -Wpass-failed noise |
|---|---|---|---|
| Stage 42 tree (= 84f019b) | 1.43–1.44 s | 0 (all-scalar sd math in the kernels) | 2–3 warnings per compile |
| Stage 43 | **1.42–1.43 s** | 32 (vmulpd/vaddpd/vunpck/vshuf/vpermpd in the kernels — the flat loops now vectorize) | **0** |

The isolated prelink A/B shows the refusal ladder closing:
2× "control flow cannot be substituted for a select" → 1× (after
if-conv) → 0 (after the raw store routing). At the real link both
`llvm.loop.vectorize.enable` demands are now fulfilled, which also removes
the user-visible `-Wpass-failed` stderr noise every FK-program `-O2`
compile used to print.

The mbs runtime is unchanged (memory-bound at N=640 with LTO inlining);
the win is structural — the certified kernels are now genuinely
vectorized, which the Stage-45+ work (wider grids, kernel reshaping,
auto-parallel) inherits without further codegen-side blocks.

### Stage 44 (D198) — the wantarray/ctx family item closes as ternary context propagation ("wantarray() inside a de-boxed
prologue reads a stale LIST ctx") did NOT reproduce: real perl prints
`want-list` for the row's own `print cq(1)` repro (print args are list
context — the row's perl-side expectation was wrong), and a 28-position
probe battery of wantarray-reading subs in every call position is
byte-identical perlc vs perl. Re-deriving the family empirically found
the real defect hiding under the same repro: the conditional operator's
branch context (full write-up in the TESTS.md D198 row). Real Perl
propagates the ternary's OWN immediate context to whichever branch runs;
perlc was consulting the wantarray stack (the sub's CALL context) in
both directions — wrong for list-position ternaries inside scalar-called
subs and for scalar-position ternaries inside list-called subs. Fixed
with a syntactic-context model (emitArrayPtr "?: case for list positions
+ scalar-context emitExpr branches + the D87 runtime consult kept only
for return-EXPR positions), plus print/sprintf/printf/method-arg
expansion. Tests `tests/d198_ternary_ctx_{smoke,deep}.pl` byte-identical;
`make test-all` 617/617 incl. the new pair. The one open unboxing-safety
row is now closed; the remaining strategic items are auto-parallel
counted loops and D199's boxed-path div-zero tail.

### Stage 45 + D199 tail — both remaining items to completion (2026-11-06, this session)

Both closed same-session; full forensics in TESTS.md
(D199-tail row + D204 `$0`, D205 pack-pair store corruption, D206
paren-blind FK walk, D207 the packed-PV/promote boundary, and the Stage-45
section in the auto-parallel exploration note). Summary:

- **D199 tail (boxed division/modulo dies)**: the runtime entries
  `perl_div`/`perl_mod`/`perl_mod_i64`/`perl_flat_row_op_assign` now take
  `(file, line)` from codegen (`srcFileGV()` + the statement's `n.line`),
  so every boxed-path zero divisor — array-element divisors, string-shaped
  operands, sub-call returns, `/=`, `%=`, 2D flat-row op `/=` — dies
  catchably with the exact real-perl `$@`/stderr text instead of the old
  uncatchable `fprintf+exit(255)` without location. `**` verified out of
  scope (real perl `0**-1` → `Inf`). Tests `tests/d199_boxed_div_{smoke,deep}.pl`
  byte-identical incl. rc.
- **D204 `$0`**: reads dispatched to `perl_get_dollar0` + the driver seeds
  the script path (`perl_set_dollar0`) and the storage became a module
  global (was a dangling init()-frame alloca) — `$0` byte-parity with perl.
- **Stage 45 auto-parallel counted loops** (`autopar` knob): certified FK
  loops gain a parallel variant — worker re-emission under the flat hooks
  + a persistent pthread pool (`perl_autopar_run`) + the runtime threshold
  (`PERLC_AUTOPAR_MIN` 8192) with the sequential flat body kept verbatim
  below it. Safety is structural (fkIndexForm slot-pair disjointness +
  closed bodies + no division). Measured 9.3× pool-level parallel speedup
  (20 dispatches: 12 ms wall vs 113 ms chunk compute) and byte-identical
  results at every worker count and tiny-chunk stress;
  `tests/autopar_{smoke,deep}.pl` cover it.
- **D205/D206** (correctness, exposed by the probe battery): the packed-
  pair store no longer fires on flat-scalar index forms (`data[4i]`
  corruption/overrun on mbs-shaped stores via the general path), and the
  FK purity walk now looks through E1 paren groups (paren-rich kernel
  bodies no longer drop off the flat path).
- **Stage-43 retrospective (D207)**: the cert probes proved the certified
  flat bodies never executed for promote-touching rows (D105 converts
  read rows to REF in place), and the packflat producer hook had never
  fired (three stacked dead-bugs, now fixed + gated behind
  `PERLC_PACK_RET=1` pending D203's value-model work). Stages 41–43
  remain net-positive through their general-path effects; the flat-
  eligible end-to-end story is D207/D203's.
