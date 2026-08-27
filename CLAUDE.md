# perlc — Perl→LLVM Compiler

AOT compiler for a large Perl 5 subset. C++17 + LLVM 18 (`clang-18` /
`llvm-config-18`). Host Perl is 5.42. All operations lower to a C runtime
(`src/runtime.c`). **No JIT, no REPL.** String `eval EXPR` and
`eval { BLOCK }` both work (see gaps table for eval STRING limits).

```
.pl → lexer → parser → AST (128 NK) → LLVM IR → clang-18 link → binary
                                           ↑
                                      runtime.c
```

## Current state (2026-08-27)

Core language, OOP, regex (PCRE2 including `/x`), threads::shared, overload,
Math::BigInt (mini-gmp), pack/unpack, `do FILE`, string `eval EXPR`,
`syscall()`, and Unix process/IPC/sockets are implemented. Correctness is
gated by `make test-all` (byte-for-byte vs real `perl`).

**Harness (2026-08-27):** **233/233 PASS**, 0 FAIL. Skipped by default:
`dbi_sqlite.pl`, `xs_ffi.pl`, `pidigits.pl`.

**Open generated-code defects:** none in the registry. **D54** (tooling):
`perlc_tsan` can hang compiling `tests/threads.pl` (TSan+`fork` of clang);
workaround `TSAN_OPTIONS=die_after_fork=0`.

**Known remaining gaps (not defects in implemented code):**

| Gap | Notes |
|-----|-------|
| String `eval EXPR` outer `my` | Constant strings without new subs are inlined (outer lexicals visible). Dynamic strings and strings that define subs compile via `--do-lib` and do **not** see the caller's `my` variables. |
| Typeglob slots | `*alias = \&sub` and `*NAME` stringify work. Scalar/array/hash slot aliasing (`*a = \$x`) is not implemented. |
| Full XS | MVP FFI, ≤4 scalar args |
| `pidigits.pl` vs perl | Skipped in harness: mini-gmp spigot `extract_digit` still diverges from Calc. `$,`/`$\` work. |
| Complex CPAN | Parser may fail on advanced `our`/POD/OO |

## Build & test

```bash
make                 # ./perlc  (g++ 15 + LLVM 18.1)
make test            # 4 assertion files (do/require/DBI/XS)
make test-all        # harness vs real perl — mandatory pre-commit gate
make test-tsan       # threads + destroy under TSan
make test-valgrind   # memcheck (skips DBI/XS)
make clean
```

`./perlc foo.pl -o out` · `--emit-ir` · `-g` · `-pm` (cpanm local-lib)

Every fix ships a **smoke + deep** test compared against real Perl.

## Architecture (short)

- **PerlValue** (48 bytes, 16-aligned): tag + flags + union + matchpos +
  blessed_class + slen (NUL-safe strings, D85) + pad. Tags include
  FLAT_ARRAY (10), FLOAT_PAIR (13), BIGINT (17).
- **Stable `PerlValue*`** identity for refs, closures, `local`, shared vars.
- **Unboxing:** i64 / f64 locals, FLAT_ARRAY, FLOAT_PAIR, AST inliner,
  DerefAV cache. Fast paths are the historical source of silent-wrong-data
  bugs — new unbox paths need a byte-for-byte deep test.
- **threads::shared:** acquire/release + lock-free 16-byte CAS on int/float
  RMW. See `THREADS_SHARED_ATOMIC.md`.
- **`do FILE`:** re-invoke `perlc --do-lib`, `dlopen` into the host runtime.

Source: `lexer.cpp` (785), `parser.cpp` (3.8k), `codegen.cpp` (~9k),
`runtime.c` (~8.8k), `mini-gmp.c`, `main.cpp`.

## Implemented (summary)

Scalars/arrays/hashes, slices, autoviv, refs, postfix deref; operators
including `and`/`or`/`xor`, bitwise, compounds; subs, closures, `wantarray`;
regex `i/g/s/m/e/x`, `s///`, `tr///`, named captures; `eval { BLOCK }` and
string `eval EXPR`; prototypes (`$ @ % & _ ; ()`), `goto LABEL` /
`goto &NAME`; typeglob `*name` stringify and `*alias = \&sub`; OOP (`bless`, `SUPER::`,
`AUTOLOAD`, `DESTROY`, `use overload`); `local`/`state`/`our`; `tie`/`untie`
with FETCH/STORE; file I/O, file tests, `stat`/`glob`; List::Util, POSIX
floor/ceil/fmod/strftime, Scalar::Util, Carp, Time::HiRes, `pack`/`unpack`;
`syscall`; **process/IPC:** `fork` `wait` `waitpid` `kill` `exec` `exit`
`pipe` `getppid` `getpgrp` `setpgrp` `setsid` `umask` `getuid` `getgid`
`geteuid` `getegid`; **sockets:** `socket` `bind` `listen` `accept` `connect`
`send` `recv` `shutdown` `getsockname` `getpeername`; `sysopen` `sysread`
`syswrite` `flock`; `vec`; 4-arg and 1-arg `select`; `fcntl`; `ioctl`;
`POSIX::dup`/`dup2`; live `%SIG` (Unix signals deferred to safe points,
plus `__WARN__`/`__DIE__`); `$?`; Math::BigInt; threads + threads::shared.

`getuid`/`getgid` are provided as bare names (Perl keeps them in POSIX.pm).

## Docs

| File | Role |
|------|------|
| `README.md` | User-facing |
| `CLAUDE.md` | This file — project state + agent workflow |
| `TESTS.md` | Test policy + open items |
| `THREADS_SHARED_ATOMIC.md` | Shared-scalar memory model |

Historical defect write-ups (D1–D98) live in git, not in-tree.

## Agent workflow

- Do not commit unless asked.
- Gate: `make test-all` before any commit that touches the compiler.
- `make clean && make` after pulling — object files have been left stale before.
- Do not grow the AST for new builtins: route `Call` → `perl_*` like `syscall`/`fork`.
