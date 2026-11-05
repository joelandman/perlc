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
