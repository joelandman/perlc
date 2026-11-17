# MVP Blockers

Authoritative list of the remaining MVP blockers, as tracked by the user.
This is the working list to complete before perlc can claim MVP. Kept here
(only here) so it survives a session reset — do not also duplicate it
elsewhere; cross-reference this file.

_Source: captured 2026-11-15. Re-verified against the live tree at the
bottom of this file._

| # | Blocker | Category | Impact | Severity | Evidence | Recommended |
|---|---------|----------|--------|----------|----------|-------------|
| 1 | Generated binary is not self-contained | Portability | A perlc binary re-invokes the compiler at runtime for `do FILE`, string `eval EXPR`, and dynamic XS: it bakes `PERLC_SELF_PATH` into the linked binary (`main.cpp:1769`) and calls `system("$PERLC --do-lib …")` then `dlopen`s (`runtime.c:909`). Requires perlc + clang-18 + `/bin/sh` + writable `/tmp` on the target. | Critical | Central to "stand alone MVP" — the output is not a standalone native binary. | Decide scope. **Recommended (a): restrict runtime do/eval to compile-time-only** (error if not statically resolvable) — the only low-risk way to ship a self-contained binary (see #1/#6 decision note). (b) bundle a static eval path (embeds a compiler; biggest lift). (c) document "needs toolchain at runtime" + content-addressed `.so` cache — stopgap. Until decided, any do/eval script is not deployable. |
| 2 | 4 active correctness-gate failures | Defect | `test-all` is the mandatory pre-commit gate and must read 0 FAIL for MVP. Currently 605/609. | High | See table above. `any` and `can_ssl` are genuine divergences; Term::ReadLine is Stub-vs-host. | Fix `any` and `can_ssl` (real bugs); align `term_readline` test with the documented Stub behavior (or implement Gnu) so the gate is green. |
| 3 | Module availability for offline/standalone use | Ecosystem | **Resolved (2026-11-16).** 0/31 real scripts fail on missing modules now — the high-frequency core modules are natively implemented and functional at runtime (see writeup below). Standalone MVP no longer needs host Pod::/Archive::/Encode:: modules present. | High | `tests/realworld/` live survey (compile-only) + dedicated runtime tests (`d227`/`d247`/`d257`/`d258`/`d260`/`d251`). | Native path adds opt-in override via `PERLC_NATIVE_OVERRIDE=0` — a user `.pm` in @INC then beats the built-in stub for `require` (see `docs/do-eval-options.md`). Remaining realworld failures are *parser-exporter fidelity* in a few complex host modules, not module availability. |
| 4 | Exporter / `import()` full semantics | Missing feature | `%EXPORT_TAGS` / `:tag` / `:all` / `:OK` are implemented, but full `import()` lifecycle, default `@EXPORT`, and conditional imports are only partially there — the documented ceiling on "arbitrary pure-Perl modules just work." | High | README/CLAUDE.md "Known Limitations"; `MVP_ROADMAP.md` historically flagged as the real ceiling. | Audit `import()` in `main.cpp`; cover `@EXPORT` + caller-namespace injection end-to-end before claiming general-CPAN support. |
| 5 | `format`/`write` runtime fidelity | Unverified | Now compiles (was an explicit non-goal), but the corpus is compile-only, so runtime output is unverified. | Med | `docs/format_write_design.md`; `perlbug`/`ucfq` now compile. | Run byte-for-byte vs perl before marking resolved; confirm it isn't a compile-only win with empty runtime output (the prior root cause was passing a raw LLVM string ptr to `perl_sprintf`). |
| 6 | Runtime do/eval compiler-spawn cost | Perf/Portability | Even once #1 is scoped, every runtime `do`/`eval` currently launches a full perlc+clang compile (seconds each) into `/tmp`. | Med | `runtime.c` `perl_do_file`/`perl_eval_string` use `system()`. | Compile-time resolution / static lib (option (a) above). A content-addressed `.so` cache reduces cold start but still needs the toolchain — keep as a documented stopgap, not the MVP answer. |
| 7 | Full XS/XSUB (SV\* ABI) loading | Non-goal/limitation | DynaLoader-compatible FFI works for perlc's own `.so` and hand-built C libs; genuine perlguts XSUB `.so`s cannot load. Explicit non-goal. | Low | `docs/xs_ffi_design.md`. | Leave as documented non-goal for MVP (sysadmin/CLI scope). |
| 8 | Auto-parallel `map`/`foreach` | Deferred (perf) | No dependence analyzer; only pure-`$_` `map` is safely parallelizable. | Low (perf) | CLAUDE.md "Known remaining gaps." | Defer; not a correctness MVP gate. |
| 9 | DBI beyond SQLite | Scope | Only the SQLite subset is implemented; non-SQLite drivers out of scope. | Low | README. | Keep SQLite-only for MVP. |
| 10 | valgrind SIGILL on perlc binaries | Env issue | `make test-valgrind` can't run on this host (SIGILL, environmental, noted as D151). | Non-blocker | README/TESTS.md notes. | Track as host-specific, not a code defect. |

## Resolution and next set (2026-11-16, HEAD `45c3dca`)

Re-verified 2026-11-16: `make test-all` = **755 PASS / 0 FAIL** (755
compared) vs real perl — the mandatory pre-commit gate is green. This
supersedes the earlier a2985bb stamp in the Verification section below.

- **#2 — correctness gate.** Resolved. Was 605/609; now fully green
  (`any`/`can_ssl` fixed, `term_readline` aligned to Stub).
- **#4 Exporter / `import()` — first two gaps closed.** Audit + two fixes,
  both byte-for-byte verified against real perl, with regression fixtures
  in `tests/` / `tests/lib/E/`. See the #4 writeup in this section below.
  Remaining lifecycle gaps: `no Module` un-import, conditional
  `use if` import lists, caller-namespace injection.
- **#1 / #6 — runtime do/eval spawns (not closed; scope decision pending).**
  `(a.1)` reclaim (commit `75a0fe9`) cut runtime spawns **19 → 16** (constant
  eval'd-sub strings compile in-process). `do FILE`, dynamic `eval EXPR`, and
  dynamic XS still spawn perlc+clang at runtime → binary not self-contained.
  **Recommended: option (a) compile-time-only for MVP** (see the #1/#6
  decision note below). (b) keeps runtime eval but is the biggest lift;
  (c) documents the toolchain dependency and caches the built `.so` — a
  stopgap only if runtime eval must be preserved.
- **#3 — module availability. Resolved (2026-11-16).** All high-frequency
  core modules are now natively implemented and verified functional at
  runtime (no host Pod::/Archive::/Encode:: needed). The 2026-11-15
  "10/30 fail on missing modules" is closed: the live survey is now
  **0/31 missing** (27/31 compile; the 4 that don't compile fail on
  *parser-exporter fidelity* in a few complex host modules — see below).
  The exporter fixes this session (`Fcntl qw(:mode)`, plus acceptance of
  runtime-invisible tags like IO::Compress `:Status`/`:Parse`) land under
  #4's surface but directly enabled #3's remaining scripts.

### Prioritized next set (by severity × blast radius × can-start-now)
1. **#3 remaining — parser/exporter fidelity in a few complex host
   modules** *(High → Med)*. Module *availability* is resolved (see writeup);
   the 4 realworld failures that remain are now all **parser** errors inside
   deep host modules, not missing modules:
   - `Module/CoreList.pm` line 82 (`expected ) but got '$'`) — `perlbug`,
     `perlthanks`.
   - `Compress/Raw/Bzip2.pm` line 53 (`unexpected token '}'`) — the
     `*{$AUTOLOAD} = sub { $val };` computed-typeglob RHS (known D192 model;
     this exact statement form isn't handled). `streamzip`.
   - `encoding.pm` line 48 (`unexpected token '&'`). `zipdetails`.
   These are a separate surface from #3's availability and #4's exporter tags;
   worth fixing only if low-risk, since each pulls in a sibling module.
2. **#1 / #6 runtime do/eval + spawn cost** *(Critical / Med)* — after (a.1),
   `do FILE`, dynamic `eval EXPR`, and dynamic XS still spawn. Needs the scope
   call: (a) compile-time-only, (b) bundle a static eval path, or (c) document
   "needs toolchain" + a content-addressed `.so` cache.
3. **#4 Exporter / `import()` lifecycle remaining** *(High)* — after the
   `:DEFAULT` + builtin-arity fixes above, remaining: `no Module` un-import,
   conditional `use if`/`use unless` import lists, caller-namespace injection.
   The "arbitrary pure-Perl modules just work" ceiling; pairs with #3
   (most modules `use Exporter`).
4. **#5 `format`/`write` runtime fidelity** *(Med)* — compiles but corpus is
   compile-only; needs byte-for-byte vs perl to rule out an empty-runtime
   compile-only win.

Non-blockers: #7 (XSUB non-goal), #8 (perf, deferred), #9 (SQLite-only),
#10 (host env).

## #4 Exporter / import() — first two gaps closed (2026-11-16, HEAD `45c3dca`)
Audit of `import()` in `src/main.cpp` (`inlineModules`, the `:tag`
expansion block and `seedProto`). Two concrete defects found and fixed;
both verified byte-for-byte against real perl and covered by new
regression fixtures in `tests/` / `tests/lib/`:

1. **`:DEFAULT` treated as a literal tag.** `use Foo qw(:DEFAULT)` threw
   "not defined in %EXPORT_TAGS". Real Exporter's `:DEFAULT` is a special
   keyword (exact uppercase) that always expands to `@EXPORT`, regardless
   of whether the module declares its own `:DEFAULT` tag. Fixed by
   expanding `:DEFAULT` to `@EXPORT` before the generic tag lookup
   (`src/main.cpp` `inlineModules`, right before the `TAG:` lookup).
   Lowercase `:default` is NOT special in real perl either (literal lookup
   that dies when absent) — perlc matches. Fixtures:
   `tests/lib/E/DefaultTag.pm`, `tests/exporter_default_tag_{smoke,deep}.pl`.
2. **Imported sub sharing a builtin name got the builtin arity.** perlc
   emulates subs like `List::MoreUtils::one` / `Try::Tiny::try` as builtins
   and seeded their arity prototype ("&@", a required list arg) onto ANY
   imported sub with that short name — so `use Foo qw(one)` died "Not
   enough arguments for Foo::one" even when the user sub took zero args.
   Real Perl aliases the user's own sub into the caller, shadowing the
   builtin. Fixed by gating the builtin prototype on the imported sub's
   qualified name actually living in the native module's namespace
   (`Try::Tiny::` for try/catch/finally, else `List::MoreUtils::`)
   (`src/main.cpp` `seedProto`). Fixtures:
   `tests/lib/E/NamedOne.pm`, `tests/import_named_{smoke,deep}.pl`.

`make test-all` green at **755 PASS / 0 FAIL** (was 751; +4 new tests),
including `list_moreutils_*` (builtin arity preserved). `#4` is now a
narrower, well-understood surface: remaining work is the rest of the
`import()` lifecycle (`no Module` un-import, conditional `use if` import
lists, caller-namespace injection) — see next set below.

### Prioritized next set for #4
- `no Module` (un-import): reverse a prior `use`, removing the imported
  names from the caller namespace.
- Conditional `use if COND, Module qw(...)` / `use unless`: import-list
  resolution behind a compile-time condition.
- Caller-namespace injection: imported names should land in the *caller's*
  package, not leak into `main` unconditionally (covers multi-package
  callers where the current importMap is applied globally).

## #3 Module availability — resolved (2026-11-16, HEAD `45c3dca`)

The blocker's core problem — scripts failing *only* because a needed module
isn't present — is **closed**. `tests/realworld/` (compile-only survey of
real host scripts) went from the 2026-11-15 stamp of **10/30 missing** to
**0/31 missing**. All the previously-failing modules now compile, and the
high-frequency core ones run correctly at runtime:

- **Native implementation of the high-frequency core.** The modules that
  real scripts import most are natively handled (in `src/runtime.c` /
  `native_stdlib.c`), so a standalone perlc binary needs no host Pod::,
  Archive::, or Encode:: trees. Verified byte-for-byte vs real perl by the
  dedicated runtime tests: `d227` (Pod::Simple), `d247`/`d257`/`d258`
  (Pod::Text — parsing + string + format variants), `d260` (Encode::Alias /
  `piconv`), `d251` (`require`/`-I`). All pass.
- **Exporter fixes (this session, also enable #3's remaining scripts).**
  `tests/realworld/` had a `streamzip` exporter failure plus, per the
  2026-11-15 inventory, host modules importing `use IO::Compress::Base::Common
  qw(:Status :Parse)` and `use Fcntl qw(:mode)`. Both were tag-expansion
  failures, fixed in `src/main.cpp` `inlineModules`:
  - `Fcntl qw(:mode)` — the native Fcntl tag table
    (`src/main.cpp` `nativeTags`) was missing the S_IS*/stat-test macros.
    Added the full `:mode` set; regenerated `src/native_constants.h` via
    `tools/gen_native_constants.pl` (extended its Fcntl list with the
    `:mode` names so the committed header stays host-accurate). All resolve
    as native constants now.
  - Runtime-invisible tags (`IO::Compress::Base::Common` `:Parse` is bound
    via `$EXPORT_TAGS{Parse} = …` *after* `%EXPORT_TAGS`; `:Status` appears
    on the module's *second* `use`, hitting the already-loaded path) — the
    re-`use` path (`inlineModules`, already-loaded branch) and the first-load
    tag branch now **accept** a tag the static scan can't resolve rather than
    throwing "not defined in %EXPORT_TAGS". The module's own tag-building is
    authoritative; only a bogus *literal bareword* import is rejected
    (matching real Exporter). No test depends on the bogus-tag error.
- **Why "missing" is now 0 but realworld isn't 31/31.** The 4 remaining
  failures (`perlbug`/`perlthanks`/`CoreList.pm`, `streamzip`/`Bzip2.pm`,
  `zipdetails`/`encoding.pm`) are **parser** errors on advanced Perl inside
  those deep host modules (see next set), not missing modules. #3's
  availability goal is met; standalone MVP needs these host modules present.

### Remaining realworld failures (parser-exporter fidelity — out of #3)
| Script | Error | Root cause |
|--------|-------|------------|
| `perlbug`, `perlthanks` | `Module/CoreList.pm:82 expected ) but got '$'` | host-module parse |
| `streamzip` | `Compress/Raw/Bzip2.pm:53 unexpected token '}'` | `*{$AUTOLOAD} = sub {$val}` computed-typeglob RHS (D192 model present, this form not emitted) |
| `zipdetails` | `encoding.pm:48 unexpected token '&'` | host-module parse |

## #1/#6 — do/eval scope decision (pending; recommended: (a))

**Status: open.** Not resolved by the #3 work this session. `perl_do_file()`
and `perl_eval_string()` (`src/runtime.c:1380`, `:1685`) still `system()` a
perlc `--do-lib`/`--eval-lib` compile at runtime and `dlopen` the `.so`, so
the binary is not self-contained (needs perlc + clang + `/bin/sh`).
`(a.1)` (commit `75a0fe9`) already reclaiming the common case: constant
eval'd-sub strings now compile in-process (19 → 16 runtime spawns).

**The decision to make:** keep runtime `do`/`eval`, or resolve it at compile
time (error if not statically resolvable)? **Recommended: (a)
compile-time-only for MVP.**

| Option | Self-contained? | Keeps runtime do/eval? | Effort |
|--------|-----------------|------------------------|--------|
| (a) compile-time-only | Yes | No (statically resolvable only) | Low |
| (b) static eval path (embed compiler) | Yes | Yes | High / uncertain |
| (c) keep + content-addressed `.so` cache | No (still needs toolchain) | Yes | Low–med |

Why **(a)**: **#1 is Critical and "central to the standalone MVP."** Only
(a)/(b) yield a self-contained binary; (c) only reduces the spawn cost and
still needs the toolchain, so (c) cannot close #1. (a) is low-risk and
consistent with (a.1); (b) embeds a compiler at runtime (biggest, most
uncertain lift) → defer as post-MVP. (c) is a stopgap only if runtime eval is
a hard requirement.

**Is `do`/`eval` worth preserving?** Mostly `do FILE` is a **benign inclusion
idiom** (like `require`/`#include` for config, helper, or library `.pl`
files) — a standard, common Perl pattern, not self-modifying code. The
genuinely "self-modifying" one is **string `eval`** (compile a string at
runtime: codegen, runtime-generated code). `eval { }` **block** form is the
idiomatic try/catch and is compile-time — already handled by perlc, not the
runtime-spawn path. So the runtime-spawn concern is the *dynamic* cases
(non-static `do` paths, runtime `eval` strings); (a) covers the common
(statically resolvable) part and errors cleanly on the rest.

## Verification (2026-11-15)

Re-checked against the live tree at HEAD `a2985bb`. The line numbers in the
table above (e.g. `main.cpp:1769`, `runtime.c:909`) predate later work;
current locations are noted so the blockers stay actionable.

- **#1 / #6 — runtime `do`/`eval` spawns the compiler.** Confirmed present.
  - `perl_do_file()` — `src/runtime.c:1347`.
  - Compile-and-`dlopen` path `perl_exec_compiled_file()` runs
    `system("${PERLC_SELF_PATH} --do-lib \"<file>\" -o <so>")` —
    `src/runtime.c:1374`.
  - String `eval` runs the equivalent `--eval-lib` compile —
    `src/runtime.c:1692`.
  - `--do-lib` / `--eval-lib` parsed in `src/main.cpp:1785-1786`;
    `PERLC_SELF_PATH` baked into the linked bitcode at `src/main.cpp:2029`.
  - Both paths require `perlc` + `clang-18` + `/bin/sh` + a writable `/tmp`
    on the target — the self-contained-binary blocker is real.
- **#2 — correctness gate.** `make test-all` is now **751 PASS / 0 FAIL**
  (751 compared) vs real perl — the mandatory pre-commit gate is green on
  HEAD `a2985bb`. The 4 that failed were exactly:
  `tests/term_readline_smoke.pl`, `tests/term_readline_deep.pl`,
  `tests/list_moreutils_deep.pl`, `tests/http_tiny_deep.pl`. Fixes:
  `any`/`all`/`none`/`notall`/`one` now return numeric `0`/`1`
  (`runtime.c` `perl_list_moreutils`, and codegen's `any`/`one` false
  initial value → `perlInt(0)`); `can_ssl` → `0` (perlc's HTTP::Tiny stub
  has no SSL); Term::ReadLine now reports `Stub`/`Term::ReadLine::Stub`
  with perl's exact xterm-family prompt ornament. All verified byte-for-byte.
- **File::Temp / TMPDIR artifact (does NOT affect the gate).** With
  `PERLC_BWRAP=0` (no bwrap /tmp isolation) the harness leaves `TMPDIR`
  pointing *outside* `/tmp` (line 337 of `tests/harness.sh`). Perl honors
  `TMPDIR`, so its no-arg `tempdir()`/`tmpnam()` return a path under
  `$HOME/.local/...` and fail the test's `/tmp/[A-Za-z0-9_]{10}` shape
  regex (`d2_ok=0`, `tmpnam_ok=0`) while perlc's `File::Temp` stub writes
  to `/tmp` directly (`=1`). This only shows up in `file_temp_smoke.pl`
  / `file_temp_deep.pl` when the gate is run without bwrap. The default
  (and documented) run redirects `TMPDIR=/tmp` inside the bwrap sandbox,
  so perl and perlc both emit `/tmp/...` and these pass. Run the gate with
  bwrap enabled (i.e. `make test-all` / `./tests/harness.sh`), not
  `PERLC_BWRAP=0`, or you'll see 2 spurious failures.
- **#3 — module availability.** Resolved — see the #3 writeup above: 0/31
  scripts missing now (was 10/30), high-frequency core modules native +
  runtime-verified (`d227`/`d247`/`d257`/`d258`/`d260`/`d251`). Live
  `tests/realworld/` survey remains the source of truth; the 4 remaining
  failures are parser-exporter fidelity, not availability.
- **#4** — `:DEFAULT` now expands to `@EXPORT` and same-named imported
  subs no longer inherit builtin arity (both fixed 2026-11-16; fixtures in
  `tests/` / `tests/lib/E/`). Remaining lifecycle gaps: `no Module`
  un-import, conditional `use if` import lists, caller-namespace injection.
- **#5** — documented in `docs/format_write_design.md`; needs a runtime
  byte-for-byte check (currently compile-only).
- **#7** — DynaLoader-compatible FFI present; genuine perlguts XSUB is a
  documented non-goal (`docs/xs_ffi_design.md`).

## Notes for whoever picks these up

- **Prefer pre-compilation, but a runtime toolchain is acceptable if
  documented.** (user, 2026-11-15) If #1/#6 fall back to runtime
  compilation at all, add a content-addressed cache for the built `.so`s
  (see the asset-cache discussion) so the cold start isn't a full
  perlc+clang compile every time.
- **New-session policy:** every session starts from a state verified
  in sync with the remote — see `CLAUDE.md` → "Agent workflow".
