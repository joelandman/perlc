# perlc Review + Gap-Filling Plan — Working Notes (Steps A+B done)

## 0. User direction (2026-08-23)

- **Fill the gaps** (the §2 table from Step A) to move perlc toward "the
  majority of Perl".
- **Add an I64 fast path** alongside the existing F64 unboxing path.
- Step B (doc read) executed per plan; findings below.

## 1. FRAMING (verified, do not repeat stale assumptions)

- **This is a PERL→LLVM compiler** (`perlc`), not Python. C++17, LLVM 18,
  clang-18 link step, boxed C runtime (`src/runtime.c`).
- Real source layout: `lexer.cpp/.h` (784L), `parser.cpp/.h` (3765L),
  `ast.h` (~190 NK kinds), `codegen.cpp/.h` (8732L), `runtime.c/.h` (8513L),
  `main.cpp` (832L, incl. REPL `-i`), `mini-gmp.c/.h` (Math::BigInt).
- **No parser generators, no embedded Python, no `IMPLEMENTATION.md`/`ISSUES.md`/
  `FEATURES.md`/`FIXES.md`** — those doc names were a stale assumption from a
  corrupted prior context. Real docs: `CLAUDE.md`, `README.md` (190L),
  `TESTS.md` (345L/210KB, long lines — the defect registry), `PLANS.md` (425L,
  somewhat stale vs TESTS.md), `plans` (393L, older consolidated analysis),
  `OPTIMIZATION_PASSES.md` (249L, 2026-06-26 rearchitecture proposal),
  `REMEDIATION.md` (272L, R1–R5), `INSTRUCTIONS.md` (291L),
  `THREADS_SHARED_ATOMIC.md` (226L).

## 2. STEP A — what exists (frontend/arch/perf)

**Frontend: broad, not a toy.** ~115 keywords; full sigils/elements/slices;
refs + all deref forms incl. postfix (`$r->@* $r->@*[LIST] $f->()`); regex
(`=~ !~ s/// tr/// y/// $1..$9 qw backticks filetests`); OOP (package/bless/
method/SUPER::/use parent/base/isa/can/AUTOLOAD/DESTROY); threads (lock/cond_*/
lock-free cmpxchg16b CAS); pragmas (strict/warnings/overload); List::Util;
pack/unpack; eval{B}; BEGIN/END; state/local; do FILE; require; tie/untie;
module loading (`use Module` → loads+inlines `.pm` from scriptDir/lib paths;
`-pm` installs missing modules); XS/FFI (`XS::load_library`/`XS::call`,
constrained MVP ABI: long/double/string/ptr/void, ≤4 scalar args); DBI/SQLite.

**Real gaps vs "majority of Perl" (verified absent from src):**

| # | Gap | Notes |
|---|-----|-------|
| G1 | **Process/IPC/sockets** | `fork exec wait waitpid pipe socket bind listen accept connect send recv shutdown select kill umask flock fcntl ioctl dup dup2 sysopen sysread syswrite` — none exist; only `syscall()`. Biggest hole. Note: the project's own "top remaining missing features" list (CLAUDE.md) deliberately omits these — they were scope-excluded, not forgotten. |
| G2 | `goto` | minor |
| G3 | prototypes, typeglobs | documented limitation |
| G4 | string `eval EXPR` | stubbed (post-JIT cut); `eval {B}` works |
| G5 | signals beyond `%SIG` | documented |
| G6 | full XS | typemaps/>4 args/non-scalar; MVP ABI only |
| G7 | `sprintf` positional arg indexes `%1$s` | TESTS.md defect (niche, silent) |

**Perf model:** AOT LLVM (default **O1**, upgradable); **no JIT** (removed);
boxed `PerlValue` (tag + union + matchpos + blessed_class); F64 unboxing
fast path; FLAT_ARRAY/FLOAT_PAIR inline representations; Stages 26–33.

## 3. STEP B — goals, correctness history, process findings

### Stated goals / scope framing
- README: "Core Language Features (**Nearly Complete**)" — self-assessed as
  near-complete for the core, not "a small subset".
- CLAUDE.md: "Top remaining missing features" = string eval EXPR, regex `/x`,
  prototypes, typeglobs, full signals. (Process/IPC absent from list →
  deliberate scope cut.)
- "MVP" language recurs (MVP contract tests, MVP types) — the product is
  explicitly framed as an MVP Perl subset; `make test` = "assertion-based MVP
  contract tests".
- **Hypothesis verdict (refined):** NOT "it became a toy / intent lost". The
  frontend was always broad; the gap to "majority of Perl" is (a) a set of
  **deliberate scope cuts** (process/IPC, real-XS, prototypes/typeglobs,
  string-eval, JIT) and (b) **documented small remaining gaps** (G3–G7).
  The user's new direction (fill G1–G7) is a **scope expansion** decision,
  not a recovery of lost intent.

### Correctness history (defect registry D1–D98 + R1–R5)
- ~98 defects logged 2026-07-08 → 2026-08-23; overwhelming majority are
  **silent wrong-data** (wrong values, truncated strings, detached copies),
  a minority crashes/segfaults.
- **Root-cause theme:** the unboxing representations (FLAT_ARRAY tag=10,
  FLOAT_PAIR tag=13) and fast paths were added **for speed without
  value-printing tests** → silently wrong results (REMEDIATION R4: "no test in
  the corpus printed the resulting values"; D98: FLAT_ARRAY 2D-row write
  segfault fixed 2026-08-23).
- **Process wound:** the regression test corpus was **deleted (69 tests → 9)**
  at some point (PLANS.md item 1: "restore the deleted test corpus and make it
  the regression gate"). Later restored; current policy: every fix ships a
  **smoke test + deep test**, verified **byte-for-byte against real Perl** via
  `tests/harness.sh`. Gate: `make test-all` (harness-as-gate, mandatory
  pre-commit).
- **Currently open (as of latest docs):**
  - D54 — perlc_tsan tooling hang (TSan+fork interaction; NOT generated-code
    correctness).
  - D66 — `$h{s}` bareword hash key with `s` tokenized as regex (narrow).
  - `make test` FAILS on `tests/xs_ffi.pl`: 2 `clock_gettime_*` (unpack
    consequence of D67, fixed) + 4 `getuid/gid/pid` (need a syscall-based
    builtin — related to G1).
  - G7 — `sprintf %N$s` positional indexes (niche).

### Performance history (numbers + open perf work)
- Benchmarks (PLANS.md snapshot, partially stale): fibn n=35 → **2.2×** perl
  (perlc 4.4s vs 9.6s); nb n=5M and mbs were **broken** in that snapshot
  (FLAT_ARRAY bug / unregistered `perl_alloc_float_array`) — both fixed by
  D96/D98 (2026-08-22/23). Refresh numbers with `bench/bench.sh`.
- Open perf items (PLANS.md, still valid):
  1. mbs-style inner-loop **DerefAV cache not firing** (`loopDerefCache_`/
     `loopInvariantPVs_` not populated for the inner vars) — target 90%
     reduction in `perl_array_get_ref`.
  2. **clone-free read accessor** `perl_array_get_ref_into()` for
     FLAT_ARRAY/FLOAT_PAIR in tight loops — ~50% `perl_free` reduction.
  3. `perl_to_string` specialization for FLAT_ARRAY (single-alloc join).
  4. **RT() lookup caching** (`callRT` does an unordered_map lookup per call)
     — 5–10% on tight loops.
  5. **CI regression benchmark** (`bench/bench.sh --regress N`, fail on >10%
     slowdown; `make test-perf`).
- Rearchitecture proposal (OPTIMIZATION_PASSES.md, 2026-06-26, phases marked
  ⏳ = likely incomplete): symbol table with `SymbolDescriptor`, storage
  abstraction (`ScalarStorage/ArrayStorage/HashStorage`), **FLAT_ARRAY as a
  separate type rather than a tag** (aliasing/complexity motivation), pipeline
  reorganization. Relevant if G1's new builtins + I64 path grow codegen
  complexity.

### I64 fast path — current state (verified in codegen.cpp)
Existing machinery: `canEmitI64` / `emitExprI64` / `boxI64` /
`emitFlooredMod` (→ `perl_mod_i64` runtime helper: floored semantics +
eval-catchable div-by-zero, per D84). **Current I64 coverage:**
- `IntLit` → constant
- `ScalarVar` → only if `lookupIntVar(nm)` finds an unboxed i64 alloca
- `BinOp` → **only `+ - * %`**
- `UnaryOp` → only unary `-`
- dispatch sites: BinOp emit, for-loop bounds, assignment RHS, `++/--` on
  unboxed int var (Stage 26b)

**F64 coverage for comparison:** BinOp `+ - * /` + `**2`; unary `-`;
SqrtFunc/AbsFunc/IntFunc/LengthFunc(FLAT_ARRAY); ArrowDeref (2D, DerefAV 1D,
FLOAT_PAIR [0]/[1]); inlineable subs. So **I64 is the thinner path** — the
user's I64 requirement = expand I64 to parity where Perl semantics allow.

## 4. WORK PLAN — filling the gaps + I64

> Ordering: each item ships with smoke+deep tests byte-for-byte vs real Perl
> (policy), runs `make test-all` as gate. Start with I64 (contained, high
> value) or G1 (biggest scope win) — user's call.

### W1. I64 fast path expansion (contained, in codegen.cpp + runtime.c)
Perl-semantics-safe additions to `canEmitI64`/`emitExprI64` (extend the
`intOps` table + new cases):
1. **Bitwise** `& | ^ ~ << >>` — exact in i64; no runtime helper needed
   (LLVM `CreateAnd/CreateOr/Xor/CreateNot/CreateShl/CreateAShr` — use
   arithmetic shift for >> to match Perl's signed semantics).
2. **Comparisons** `< > <= >= == !=` and `<=>` — on int-typed operands the
   numeric comparison is exact in i64 (emit select to i64; `<=>` → -1/0/1).
   Gate: both operands `canEmitI64`.
3. **`**` with non-negative IntLit exponent** (small bound, e.g. ≤30, to avoid
   silent overflow — or route overflow via runtime check) → integer result.
4. **`abs` / `int`** on I64 — identity/branchless select (parity with F64's
   AbsFunc/IntFunc).
5. **`++/--` on arbitrary I64 expressions** (Stage 26b currently var-only).
6. **Keep `/` EXCLUDED** — Perl `$a / $b` on ints numifies to float (10/4=2.5);
   unboxing it as i64 division is semantically wrong.
7. New dispatch sites mirroring F64's (comparisons in `emitExpr`, builtins).
8. **Tests**: deep int-stress (bit patterns, negative floors, shifts,
   overflow boundaries) byte-for-byte vs real Perl; verify no regression in
   existing I64 tests (`perl_mod_i64` D84 behavior).
- Risk note: every new unboxed path is a **silent-wrong-data candidate** (the
  D98 lesson). The byte-for-byte deep test is mandatory, not optional.

### W2. G1 — process/IPC/sockets builtins (biggest scope win)
- Lexer keywords + AST nodes (or generic Call routing — prefer generic Call
  into runtime helpers to keep the AST small, matching how `syscall`/builtins
  like `system` work; verify `system`'s pattern first).
- Runtime C: `perl_fork perl_exec* perl_wait perl_waitpid perl_pipe
  perl_socket* perl_bind perl_listen perl_accept perl_connect perl_send
  perl_recv perl_shutdown perl_select4 perl_kill perl_umask perl_flock
  perl_fcntl perl_ioctl perl_dup* perl_sysopen perl_sysread perl_syswrite
  perl_getppid perl_getpgrp perl_setpgrp perl_setsid perl_getpriority
  perl_setpriority perl_sigaction perl_sigprocmask` (G5 folds in here).
- This also unblocks the 4 open `getuid/gid/pid` xs_ffi test failures
  (syscall-based builtins).
- Tests: coprocess (pipe + fork + read), TCP echo (socket/connect/accept/send/
  recv), kill/wait status words, select timeout — byte-for-byte vs real Perl.
- Note the README/CLAUDE.md docs must be updated (currently these are
  scope-excluded, not "missing").

### W3. G4 — string `eval EXPR` (re-introduce, AOT-friendly)
- Currently a stub (sets $@, returns undef). Options: (a) compile-time
  pre-compilation of statically-known eval strings (common case:
  `eval $var` where $var is a const) → generate code; (b) a small
  interpreter fallback for truly dynamic strings (the removed JIT's role).
- Decide (a) vs (b) with the user; (a) is cheaper and covers most real code.

### W4. G6 — full XS (typemaps, >4 args, non-scalar signatures)
- Extend the constrained MVP ABI: variadic arg counts, arrays/hashes across
  the FFI boundary, typemap file support.
- Lower priority than W1/W2 unless a specific use case drives it.

### W5. G3 — prototypes + typeglobs
- Prototypes: parser support for `sub f(@)` + arg-list checking (codegen:
  list-context enforcement).
- Typeglobs: `*name`, `*name{ARRAY|HASH|SCALAR|CODE|IO}`, Symbol:: semantics.
- Niche in practice; do only if driven by real code.

### W6. G2/G7 — small
- `goto` (BLOCK/label forms — runtime longjmp to label BB; `goto &sub` is
  a tail call).
- `sprintf %N$s` positional arg indexes (runtime fix).

### W7. Perf items (from PLANS.md, still open) — batch after W1
- W7.1 RT() lookup caching (5–10%).
- W7.2 `perl_array_get_ref_into` clone-free reads (~50% free reduction).
- W7.3 mbs DerefAV-cache firing fix (90% `perl_array_get_ref` reduction).
- W7.4 FLAT_ARRAY `perl_to_string` single-alloc join.
- W7.5 CI regression benchmark (`--regress N`, `make test-perf`).
- Consider raising default O1→O2 (cheap, measure first with W7.5 in place).

### W8. Docs + state
- Update README/CLAUDE.md "missing features" list as each W lands.
- If W1/W2 grow codegen complexity: revisit the OPTIMIZATION_PASSES.md
  rearchitecture (SymbolDescriptor / FLAT_ARRAY-as-type) before it bit-rots
  further.

## 5. Resumption pointers (files/functions, verified)
- I64 machinery: `codegen.cpp` ~1716–1900 (`unboxed float helpers` /
  `unboxed integer helpers` sections: `boxI64` 1790, `emitFlooredMod` 1794,
  `canEmitI64` 1807, `emitExprI64` 1827); `intOps` tables at 1816/1840;
  Stage 26b ++/-- at ~4124; dispatch sites ~5110, ~5639, ~5828; for-bounds
  ~4177.
- F64 for parity: `canEmitF64` (search `canEmitF64(const Node`),
  `emitExprF64`.
- Runtime mod helper: `perl_mod_i64` in runtime.c (D84).
- Defect registry: TESTS.md `## Defect Registry` (line 114), `### Test
  Coverage Gaps` (line 285).
- Bench: `bench/bench.sh`, results in `bench/results.csv`.
- Gate: `make test-all` (harness-as-gate policy).
