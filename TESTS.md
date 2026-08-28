# perlc — Tests

## Gate

```bash
make              # build ./perlc
make test-all     # tests/harness.sh — compare every tests/*.pl vs real perl
make test         # 4 assertion files only (do / require / DBI / XS)
make test-tsan    # threads.pl threads_atomic.pl destroy.pl
```

`tests/harness.sh` compiles each `tests/*.pl` with `./perlc`, runs it and
`perl`, and diffs stdout. Numeric tests (`nb.pl`, `nbody.pl`, `mbs.pl`,
`fibn.pl`, `arith.pl`) allow FP tolerance. `mbs.pl` gets a 300s timeout.

**Skipped by default** (run explicitly if you have the deps):
`dbi_sqlite.pl`, `xs_dbi_test.pl`, `xs_ffi.pl`, `pidigits.pl`
(BigInt spigot still diverges from perl's Calc).

**Policy:** every compiler fix ships a smoke test (`*_smoke.pl`) and a deep
test, verified byte-for-byte against real Perl.

## Scoreboard

**2026-08-28: 251 compared, 251 PASS, 0 FAIL.**

Skipped by default: `dbi_sqlite.pl`, `xs_ffi.pl`, `pidigits.pl`.

Previously open compared failures, now closed:

| Test | Resolution |
|------|------------|
| `eval_string.pl` | Passes — `eval { BLOCK }`. String `eval EXPR` is in `eval_expr{,_smoke}.pl`. |
| `syscall_smoke.pl` / `syscall_deep.pl` | Pass — tests no longer print raw PIDs (those differ across processes). |
| `d66_hash_elem_string.pl` | Pass — `$h{s}` is no longer lexed as `s///` (closer delimiters `}` `]` `)` are not s/// openers). |
| `pidigits.pl` | Skipped — `$,`/`$\` work; mini-gmp `extract_digit` still diverges. |

New IPC tests: `ipc_process{,_smoke}.pl`, `ipc_socket{,_smoke}.pl`.

New sys tests (2026-08-27): `sys_vec_select{,_smoke}.pl`, `sys_fcntl_dup{,_smoke}.pl`,
`sys_sig{,_smoke}.pl` — `vec`, 4-arg/1-arg `select`, `fcntl`, `ioctl`,
`POSIX::dup`/`dup2`, live `%SIG{USR1,USR2,ALRM}` plus existing `$SIG{__WARN__}`.

New language tests (2026-08-27): `regex_x{,_smoke}.pl` (`/x`, `m{}`/`m()`), 
`eval_expr{,_smoke}.pl` (string `eval EXPR`: constants, `$@`, outer `my`,
subs defined in eval, list/wantarray).

New language tests (2026-08-27, later): `proto_{smoke,deep}.pl` (`$$`, `@`, `()`,
`&@` block, `_`, `;$`, `&name` bypass), `goto_{smoke,deep}.pl` (`goto LABEL`,
`goto &NAME`, labeled loops), `glob_{smoke,deep}.pl` (`*alias = \&sub`, stringify),
`glob_slot_{smoke,deep}.pl` (`*a = \$x`/`\@a`/`\%h` cell sharing, `*a = *b`).

MVP follow-ups (2026-08-27): `use_version_smoke.pl` (`use v5.36` / signatures
bundle), `signatures_deep.pl` (`sub f($x, $y=0, @rest)`), `utf8_open_smoke.pl`
(`:utf8` / `:encoding(UTF-8)`), `bare_fh_smoke.pl` (`open LOG`, `print LOG`,
`<IN>`), `pod_skip_smoke.pl` (`=pod`…`=cut`), `closure_int_capture_smoke.pl`
(unboxed-int capture shares the cell), `eval_lex_{smoke,deep}.pl` (dynamic
eval STRING and eval-defined subs see outer `my`).

## Open

| ID | Status | Notes |
|----|--------|-------|
| D54 | OPEN (tooling) | `perlc_tsan` hangs compiling `tests/threads.pl` (TSan+fork of clang-18). `TSAN_OPTIONS=die_after_fork=0` works around it. Not a generated-code bug. |

D1–D53, D55–D98 are **FIXED** (or STALE/N/A). The long-form registry with
root causes is in git history (`TESTS.md` prior to 2026-08-26).

## Remaining product gaps (not logged as D-numbers)

Full XS (FFI is not DynaLoader); complex CPAN (advanced `our`/OO — POD is
skipped). Typeglob `{IO}`/`{FORMAT}` slots are not implemented. String
`eval EXPR` sees outer `my`. Runtime `eval`/`do` still needs clang+perlc
on the target.

Diamond `<>` / `<ARGV>`, `__DATA__`/`<DATA>`, `use utf8`, `unshift @{EXPR}`,
and `exists $h{a}{b}`: `diamond_{smoke,deep}.pl`, `data_section_{smoke,deep}.pl`,
`utf8_source_{smoke,deep}.pl`, `unshift_exists_{smoke,deep}.pl`.

## Source layout

| File | Role |
|------|------|
| `src/lexer.cpp` | Tokenizer |
| `src/parser.cpp` | Recursive descent |
| `src/codegen.cpp` | AST → LLVM IR |
| `src/runtime.c` | PerlValue + builtins |
| `src/mini-gmp.c` | Math::BigInt |
| `src/main.cpp` | Driver |
