# perlc — Perl→LLVM Compiler

AOT compiler for a large Perl 5 subset. C++17 + LLVM 18 (`clang-18` /
`llvm-config-18`). Host Perl is 5.42. All operations lower to a C runtime
(`src/runtime.c`). **No JIT, no REPL.** String `eval EXPR` and
`eval { BLOCK }` both work (outer `my` is visible to eval STRING).

```
.pl → lexer → parser → AST (128 NK) → LLVM IR → clang-18 link → binary
                                           ↑
                                      runtime.c
```

## Current state (2026-09-10)

Core language, OOP, regex (PCRE2 including `/x`), threads::shared, overload,
Math::BigInt (mini-gmp), pack/unpack, `do FILE`, string `eval EXPR`,
`syscall()`, and Unix process/IPC/sockets are implemented. Correctness is
gated by `make test-all` (byte-for-byte vs real `perl`).

**Harness (2026-09-10, re-verified after D113/D111/D112/D114, again
after D109 — 275/275 PASS, 0 FAIL — and again pending after D121/D122):**
New this session: `d113_undefined_sub_die_{smoke,deep}.pl`,
`d111_hash_flatten_{smoke,deep}.pl`,
`d112_module_scope_{smoke,deep}.pl` (+ `tests/lib/D112Leaky.pm`),
`d114_array_slice_{smoke,deep}.pl`, `d109_subst_interp_{smoke,deep}.pl`,
`d121_bare_maincolon_{smoke,deep}.pl`,
`d122_scanexports_usevars_{smoke,deep}.pl` (+
`tests/lib/D122UseVarsExport.pm`).
Skipped by default: `dbi_sqlite.pl`, `xs_ffi.pl`, `pidigits.pl`.

**D99, D105, D100, D107, D113, D111, D112, D114, D109, D121, and D122
are now fixed (D121/D122/D113/D111/D112/D114/D109 detailed just below;
D99/D105/D100/D107 write-ups follow):**
- D121 (`src/lexer.cpp`): `$::name`/`@::arr`/`%::hash` (Perl's `main::`
  shorthand) no longer a parse error — the lexer now synthesizes the
  same token a spelled-out `main::name` would produce. Found (and
  fixed) via the 2026-09-10 CPAN-module compile survey below
  (`/usr/bin/ucfq`). Widened **D110** while verifying this: an
  undeclared qualified array/hash (not just scalar) doesn't cross
  scopes either, and more severely than scalars — whole-array/hash
  access returns nothing, not just elements.
- D122 (`src/main.cpp` `scanExports()`): now also recognizes the older
  `use vars qw(@EXPORT_OK); @EXPORT_OK = qw(...)` export-declaration
  style (no `our` prefix) that real core `File::Path.pm` itself ships
  with — previously this caused a false "not exported by the module"
  compile error for a genuinely-exported name. Found via the same
  survey, then re-verified directly against the real, unmodified
  `File::Path.pm` from a Perl install.
- D109 (`src/codegen.cpp` `case NK::RegexSubst`, new `Parser::
  parseInterpString` in `src/parser.{h,cpp}`): `s/PATTERN/REPLACEMENT/`'s
  REPLACEMENT text now supports full `$name`/`@arr` variable
  interpolation, not just `$0`-`$9`/`$&` capture refs — reuses the same
  interpolation scanner ordinary `"..."` literals already use, routed
  through the `/e` flag's existing per-match closure/capture machinery
  (refactored into a shared lambda). Only takes the new, more expensive
  path when the replacement text actually has a named-variable trigger;
  plain-text and capture-ref-only replacements (the common case) stay on
  the original fast path, untouched. Split off a narrower, harder,
  *not*-`s///`-specific remainder as **D120** (subscripted deref in any
  interpolated string, e.g. `"$$aref[0]"`) — logged, not fixed.
- D113 (`src/runtime.c` `perl_call_named_sub_checked`, `src/main.cpp`
  `inlineModules`): an undefined sub call now dies with Perl's own
  `Undefined subroutine &Pkg::name called at FILE line N.` (exit 255)
  instead of silently returning `undef`; an unresolvable `use
  Some::Module;` now errors with `Can't locate .../Module.pm in @INC`
  instead of being silently dropped; `-I`, `PERL5LIB`, and `use lib` are
  now honored in the module search path. This was the most important
  single fix in the 2026-09-10 review — it's what let three
  completely-unimplemented modules go unnoticed until a human manually
  diffed output back on 2026-09-09.
- D111 (`src/codegen.cpp` `emitArrayPtr` + `NK::AnonHash`): `my %c = %h`,
  `(%defaults, %overrides)` merges, and `{ %args, k=>v }` anon-hashref
  spreads (the `bless { %args }, $class` idiom) all silently produced
  wrong contents instead of flattening correctly — `emitArrayPtr` had no
  case for a hash-shaped node at all. Found a second, separate bug of
  the same shape while fixing this: **D119** (`scalar(keys %$href)`
  returns 0) — logged, not fixed.
- D112 (`src/codegen.cpp` `case NK::My` + `lookupVar`/`lookupArray`/
  `lookupHash`): a module's file-scope `my $counter` no longer collides
  with the main script's (or another module's) same-named variable —
  file-scope global storage is now keyed by package-qualified name
  (`Leaky::counter` vs `main::counter`) instead of a shared bare name. A
  first-attempt fix (wrapping each inlined module's tokens in a `{ ... }`
  block) was tried and rejected — see the D112 write-up in TESTS.md for
  why; named subs in this codebase resolve free variables by name, not
  via true lexical closure capture over blocks, so the wrap broke
  module-internal sub access to the module's own file-scope variables.
- D114 (`src/codegen.cpp` + new `perl_array_slice` in `src/runtime.c`):
  `@x[1..2]` / `@x[@indices]` array slices now return the full slice
  instead of one element — ported `HashSlice`'s existing dynamic-list
  dispatch to `ArraySlice` at both codegen sites.

**D99, D105, D100, and D107 are now fixed:**
- D99 (`src/codegen.cpp:4118-4149`): `my @b = @a;` no longer aliases
  `@a`'s storage.
- D105 (`src/runtime.c` `perl_promote_ref_array`, `src/codegen.cpp`
  `case NK::ScalarVar`): a FLAT_ARRAY/FLOAT_PAIR-tagged anon-array-ref
  (Stage 22/23's compact storage for `[1,2]`/`[1,2,3]`-style literals) no
  longer silently forks into two independent arrays when aliased a second
  time (via `push`, sub args, hash values, `my $y = $x`, array-to-array
  copy, etc.) — it's promoted to a real `PERL_REF_ARRAY` in place first.
  Fresh literal construction (e.g. `tests/d96_flat_row_op_assign.pl`'s
  numeric matrix rows) is untouched, so the fast path survives.
- D100 (`src/codegen.cpp` `case NK::Assign`'s `ArrayLit`-LHS branch, plus
  `While`'s condition-hoisting logic): `while (my ($k,$v) = each %h)` /
  `if ((...) = ...)` now runs its body the correct number of times *and*
  the loop variables are actually populated (two stacked bugs — see
  `TESTS.md`). Verified with a 50k-iteration stress test for the new
  per-variable alloca hoist (no stack growth from re-executing an alloca
  every loop iteration).
- D107 (`src/runtime.c` `perl_regex_subst`'s replacement-expansion loop):
  `s/PATTERN/REPLACEMENT/`'s replacement text now processes backslash
  escapes (`\\ \n \t \r \f \b \a \e \0 \$ \@`) instead of copying them
  verbatim — `s/\\/\\\\/g` used to double the inserted backslash, and
  `\t`/`\n` etc. in a replacement stayed as literal 2-char sequences
  instead of becoming the actual escape character. Found via the real
  `/usr/bin/debconf-escape` script.

**Open generated-code defects:** **D101–D104, D106, D108, D110, D115–D120**
(see `TESTS.md`). **D54** (tooling): `perlc_tsan` can hang compiling
`tests/threads.pl` (TSan+`fork` of clang); workaround
`TSAN_OPTIONS=die_after_fork=0`.

**2026-09-10 five-agent MVP review:** a code-reviewer/architect, three
engineers (runtime/codegen/parser depth), and a PM assessed real-world
readiness for "common CPAN modules work." Found and byte-for-byte-
verified **8 new defects (D111–D118)**; **D111–D114 are now fixed**, and
the pre-existing **D109 is now fixed too** (see above) — the remaining
open ones are `__PACKAGE__` etc. being unimplemented (D116), two runtime
correctness gaps in very hot paths (D117: hand-rolled float parser;
D118: `split` missing LIMIT + trailing-empty-trim), D115 (bare
`return;` in list context), **D119** (found while fixing D111 —
`scalar(keys %$href)` returns 0), and **D120** (split off D109's
widened scope — subscripted deref in an interpolated string, e.g.
`"$$aref[0]"`, still wrong; a harder, general-interpolation-engine fix,
not `s///`-specific). Also found: no `Exporter`/`@EXPORT` mechanism
exists at all — arguably the real ceiling on "arbitrary pure-Perl CPAN
module just works," bigger than any individual defect. Full assessment,
re-ranked priorities,
CPAN-module candidate list, and critical path: **`MVP_ROADMAP.md`**
(status there needs a pass to mark D113/D111/D112/D114 done — next
step).

**Real-world module survey (2026-09-09/10):** before continuing the
synthetic-probe D-number list, compiled ~36 unmodified real Debian
system Perl scripts plus common module-usage patterns to check whether
remaining items were likely to matter in practice. Found **Getopt::Long,
Data::Dumper, and File::Basename were completely unimplemented** (higher
real-world impact than anything left on the D-list) — **all three are
now fixed** (`src/runtime.c` `perl_getopt_long`/`perl_dumper`/
`perl_basename`+`perl_dirname`+`perl_fileparse`). This pass also found
and fixed **D107**, and found (not fixed) **D110** (`$Package::var` is
not a true cross-scope global — found via `$Data::Dumper::Sortkeys`),
**D108** (plain `"..."` string literals don't recognize `\f`/`\a`/`\e`/
`\b`, found while writing D107's tests), **D109** (`s///` replacement
text not supporting arbitrary variable interpolation, found while fixing
D107), and several parser gaps (`continue {}` blocks,
nested-bracket `q[...]`, `\my %var`, `qr//` not implemented at all — see
`TESTS.md`'s missing-features list). D101 and D103 were reassessed and
demoted to lower priority: real-world code almost always uses `each` in
list context (already fixed under D100), and integer overflow at the
`2**63` boundary essentially never comes up outside bigint-specific
code. See `TESTS.md` → "Real-world module survey" for full detail.
**Fix the remaining correctness items before adding more new features.**

**Known remaining gaps (not defects in implemented code):**

| Gap | Notes |
|-----|-------|
| Typeglob `{IO}`/`{FORMAT}` | `*alias = \&sub`, stringify, `*a = \$x`/`\@a`/`\%h`, and bare `open LOG` / `print LOG` work. `*FH{IO}` / FORMAT slots are not implemented. |
| Full XS | MVP FFI, ≤4 scalar args — not DynaLoader / `boot_` XSUBs |
| `pidigits.pl` vs perl | Skipped in harness: mini-gmp spigot `extract_digit` still diverges from Calc. `$,`/`$\` work. |
| Complex CPAN | Parser may fail on advanced `our`/OO. POD (`=pod`…`=cut`) is skipped. |
| eval/`do` at runtime | Needs `perlc` + `clang-18` on the target (`--eval-lib` / `--do-lib`). |

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
- **string `eval EXPR`:** constant strings without new subs are inlined.
  Dynamic / sub-defining strings compile via `--eval-lib`; the caller dumps
  in-scope `my` cells into an eval pad so the compiled string aliases them.

Source: `lexer.cpp` (785), `parser.cpp` (3.8k), `codegen.cpp` (~9k),
`runtime.c` (~8.8k), `mini-gmp.c`, `main.cpp`.

## Implemented (summary)

Scalars/arrays/hashes, slices, autoviv, refs, postfix deref; operators
including `and`/`or`/`xor`, bitwise, compounds; subs, closures, `wantarray`;
regex `i/g/s/m/e/x`, `s///`, `tr///`, named captures; `eval { BLOCK }` and
string `eval EXPR` (outer `my` via eval pad); `use v5.xx` / `use feature
'signatures'`; sub signatures (`sub f($x, $y=0, @rest)`); prototypes
(`$ @ % & _ ; ()`), `goto LABEL` / `goto &NAME`; typeglob `*name` stringify
and `*alias = \&sub`; bare filehandles (`open LOG`, `print LOG`, `<IN>`);
UTF-8 `:utf8`/`:encoding(UTF-8)` open/binmode layers and `use utf8` source
encoding; diamond `<>` / `<ARGV>` / `$ARGV`; `__DATA__`/`__END__` + `<DATA>`;
`unshift @{EXPR}`; `exists $h{a}{b}`; POD skip; OOP (`bless`, `SUPER::`,
`AUTOLOAD`, `DESTROY`, `use overload`); `local`/`state`/`our`; `tie`/`untie`
with FETCH/STORE; file I/O, file tests, `stat`/`glob`; List::Util, POSIX
floor/ceil/fmod/strftime, Scalar::Util, Carp, Time::HiRes, `pack`/`unpack`;
Getopt::Long, Data::Dumper, File::Basename (2026-09-09 — see TESTS.md's
"Real-world module survey");
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
| `MVP_ROADMAP.md` | 2026-09-10 multi-agent MVP assessment + CPAN-module plan |
| `THREADS_SHARED_ATOMIC.md` | Shared-scalar memory model |

Historical defect write-ups (D1–D98) live in git, not in-tree.

## Agent workflow

- Do not commit unless asked.
- Gate: `make test-all` before any commit that touches the compiler.
- `make clean && make` after pulling — object files have been left stale before.
- Do not grow the AST for new builtins: route `Call` → `perl_*` like `syscall`/`fork`.
