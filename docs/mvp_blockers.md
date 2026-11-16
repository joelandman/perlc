# MVP Blockers

Authoritative list of the remaining MVP blockers, as tracked by the user.
This is the working list to complete before perlc can claim MVP. Kept here
(only here) so it survives a session reset — do not also duplicate it
elsewhere; cross-reference this file.

_Source: captured 2026-11-15. Re-verified against the live tree at the
bottom of this file._

| # | Blocker | Category | Impact | Severity | Evidence | Recommended |
|---|---------|----------|--------|----------|----------|-------------|
| 1 | Generated binary is not self-contained | Portability | A perlc binary re-invokes the compiler at runtime for `do FILE`, string `eval EXPR`, and dynamic XS: it bakes `PERLC_SELF_PATH` into the linked binary (`main.cpp:1769`) and calls `system("$PERLC --do-lib …")` then `dlopen`s (`runtime.c:909`). Requires perlc + clang-18 + `/bin/sh` + writable `/tmp` on the target. | Critical | Central to "stand alone MVP" — the output is not a standalone native binary. | Decide scope: (a) restrict runtime do/eval to compile-time-only, (b) bundle a static eval path, or (c) document "needs toolchain at runtime." Until decided, any do/eval script is not deployable. |
| 2 | 4 active correctness-gate failures | Defect | `test-all` is the mandatory pre-commit gate and must read 0 FAIL for MVP. Currently 605/609. | High | See table above. `any` and `can_ssl` are genuine divergences; Term::ReadLine is Stub-vs-host. | Fix `any` and `can_ssl` (real bugs); align `term_readline` test with the documented Stub behavior (or implement Gnu) so the gate is green. |
| 3 | Module availability for offline/standalone use | Ecosystem | 10/30 real scripts fail only on missing modules (Pod::Text/Man/Html, Archive::Tar, IO::Compress::Zip, ExtUtils::Packlist, App::Cpan, App::Prove, Encode::Alias). Not compiler defects, but a standalone MVP needs them present. | High | `tests/realworld/baseline.txt` / live `make test-realworld`. | Two paths: native-implement the high-freq core modules (Pod::*, Archive::Tar, IO::Compress), or make `-pm`/`cpanm` reliably offline-capable (`cpanm` is absent from this host). |
| 4 | Exporter / `import()` full semantics | Missing feature | `%EXPORT_TAGS` / `:tag` / `:all` / `:OK` are implemented, but full `import()` lifecycle, default `@EXPORT`, and conditional imports are only partially there — the documented ceiling on "arbitrary pure-Perl modules just work." | High | README/CLAUDE.md "Known Limitations"; `MVP_ROADMAP.md` historically flagged as the real ceiling. | Audit `import()` in `main.cpp`; cover `@EXPORT` + caller-namespace injection end-to-end before claiming general-CPAN support. |
| 5 | `format`/`write` runtime fidelity | Unverified | Now compiles (was an explicit non-goal), but the corpus is compile-only, so runtime output is unverified. | Med | `docs/format_write_design.md`; `perlbug`/`ucfq` now compile. | Run byte-for-byte vs perl before marking resolved; confirm it isn't a compile-only win with empty runtime output (the prior root cause was passing a raw LLVM string ptr to `perl_sprintf`). |
| 6 | Runtime do/eval compiler-spawn cost | Perf/Portability | Even once #1 is scoped, every runtime `do`/`eval` currently launches a full perlc+clang compile (seconds each) into `/tmp`. | Med | `runtime.c` `perl_do_file`/`perl_eval_string` use `system()`. | Prefer compile-time resolution / static lib where possible; cache built `.sos`; document the cold-start cost. |
| 7 | Full XS/XSUB (SV\* ABI) loading | Non-goal/limitation | DynaLoader-compatible FFI works for perlc's own `.so` and hand-built C libs; genuine perlguts XSUB `.so`s cannot load. Explicit non-goal. | Low | `docs/xs_ffi_design.md`. | Leave as documented non-goal for MVP (sysadmin/CLI scope). |
| 8 | Auto-parallel `map`/`foreach` | Deferred (perf) | No dependence analyzer; only pure-`$_` `map` is safely parallelizable. | Low (perf) | CLAUDE.md "Known remaining gaps." | Defer; not a correctness MVP gate. |
| 9 | DBI beyond SQLite | Scope | Only the SQLite subset is implemented; non-SQLite drivers out of scope. | Low | README. | Keep SQLite-only for MVP. |
| 10 | valgrind SIGILL on perlc binaries | Env issue | `make test-valgrind` can't run on this host (SIGILL, environmental, noted as D151). | Non-blocker | README/TESTS.md notes. | Track as host-specific, not a code defect. |

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
- **#3** — `tests/realworld/baseline.txt` / `make test-realworld` remain the
  source of truth for missing-module coverage.
- **#4** — `%EXPORT_TAGS` / `:tag` / `:all` / `:OK` exist; full `import()`
  lifecycle is partial. See `MVP_ROADMAP.md`.
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
