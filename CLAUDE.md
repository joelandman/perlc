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

**Harness (2026-09-10, re-verified after each fix this session — most
recently 279/279 PASS, 0 FAIL after D121/D122; D116 re-verification
pending):** New this session: `d113_undefined_sub_die_{smoke,deep}.pl`,
`d111_hash_flatten_{smoke,deep}.pl`,
`d112_module_scope_{smoke,deep}.pl` (+ `tests/lib/D112Leaky.pm`),
`d114_array_slice_{smoke,deep}.pl`, `d109_subst_interp_{smoke,deep}.pl`,
`d121_bare_maincolon_{smoke,deep}.pl`,
`d122_scanexports_usevars_{smoke,deep}.pl` (+
`tests/lib/D122UseVarsExport.pm`), `d116_dunder_consts_{smoke,deep}.pl`.
Skipped by default: `dbi_sqlite.pl`, `xs_ffi.pl`, `pidigits.pl`.

**D99, D105, D100, D107, D113, D111, D112, D114, D109, D121, D122,
D116, D117, D118, D119, D127, D129, D102, and D115 are now fixed
(D115/D102/D129/D127/D119/D118/D117/D116/D121/D122/D113/D111/D112/
D114/D109 detailed just below; D99/D105/D100/D107 write-ups follow):**
- D115 (`src/codegen.cpp` `case NK::Return` **and** its duplicate in
  `emitBlockLast`): bare `return;` in list context now yields a
  genuinely empty list instead of a 1-element `(undef)` list — needed
  fixing in *two* separate places, since `emitBlockLast` has its own
  copy of this logic for when `return` is a sub's last statement (the
  common case for a trivial `sub f { return; }`), and also needed
  `hasWantarrayOrUserCall()` extended to recognize a bare `return;` as
  something that now reads the wantarray stack (previously only
  `return LIST_EXPR` and explicit `wantarray()` triggered that). Found
  **D130** (`if (my @arr = EXPR)`, single array var with no parens, is
  a parse error) while testing — logged, not fixed.
- D102 (`src/runtime.c` `perl_die`): `die REF` / `die $blessed_obj` no
  longer loses the reference into `$@` — `perl_die` now detects a
  reference argument and assigns it directly via `perl_assign` instead
  of unconditionally stringifying, and no longer wrongly appends the
  `" at FILE line N."` location suffix to a reference (confirmed real
  Perl never does, even at top level). Verified for hashref, arrayref,
  scalarref, and blessed objects; plain-string dies are unaffected.
- D129 (`src/parser.cpp`): `local(VAR, VAR, ...) = EXPR` — parenthesized
  list-form `local`, including the single-variable case found verbatim
  in real `Pod::Usage.pm` (`local($_) = shift;`) — no longer a parse
  error. Desugars exactly like `my (...)`'s identical list form (a
  `FlatBlock` of per-variable local-decls + one list-assignment), reusing
  the existing, already-tested codegen path entirely — no codegen
  changes needed. Found **D131** (`our $var;` inside a nested block
  doesn't work correctly) while testing — logged, not fixed.
- D127 (`src/main.cpp` `extractQw()`): a `qw()`-listed export/import
  name with a leading `&`/`*` sigil (real `Pod::Usage.pm`'s `our
  @EXPORT = qw(&pod2usage);`) is now stripped before being stored or
  compared, so both default-`@EXPORT` bare-`use` and explicit
  `@EXPORT_OK` imports resolve correctly instead of falsely rejecting a
  genuinely-exported name or leaving it unreachable unqualified.
  Verified against a local fixture and the real, unmodified
  `Pod::Usage.pm`; re-verifying against the latter surfaced a new,
  separate bug once past the export issue — **D129** (`local($var) =
  EXPR;`, parenthesized single-variable `local`, is a parse error) —
  logged, not fixed.
- D119 (`src/codegen.cpp` `case NK::KeysFunc`/`ValuesFunc` in
  `emitExpr`): `scalar(keys %$href)` / `scalar(values %$href)` now
  return the correct count instead of `0` — the scalar-context cases
  only ever resolved a *named* hash variable, missing the `n.left`
  deref-hash handling `emitArrayPtr`'s identical list-context cases
  already had. List-context `keys %$href` and named-hash `keys %h` (in
  either context) were already correct and stay unaffected.
- D118 (`src/parser.cpp` split-call parsing, `src/runtime.c`
  `perl_split`/`perl_split_regex`): `split(/:/, $line, 2)`'s 3rd LIMIT
  argument was a hard parse error (only 2 args were ever consumed) —
  now bounds the field count correctly, the last field absorbing the
  remainder unsplit. Both split implementations now also trim trailing
  empty fields when LIMIT is omitted/zero (real Perl's default), via a
  shared `perl_split_trim_trailing_empty()` helper — negative LIMIT
  stays unbounded with no trimming, matching real Perl. Found a
  second, separate, pre-existing bug while testing this: **D126**
  (`split(/(,)/, ...)` — a capturing-group pattern — doesn't include
  the captured delimiter text in the result) — logged, not fixed.
- D117 (`src/runtime.c` `perl_atof_decimal`): implicit string→number
  coercion now scans the same decimal-only prefix as before (still no
  hex/auto-`0x`, matching real Perl) but hands the matched substring to
  `strtod` instead of a hand-rolled digit accumulator with a repeated-
  multiply exponent loop — confirmed diverging from real Perl on
  extreme exponents. Also now recognizes `Inf`/`Infinity`/`NaN` string
  coercion, found to be completely unhandled while designing the fix.
  Split off **D125** (found while testing this — `use`/`no` pragmas
  only parse at file top-level, not inside a nested block/sub).
- D116 (`src/parser.cpp` `parsePrimary`, `src/codegen.cpp` `emitCall`):
  `__PACKAGE__`/`__FILE__`/`__LINE__` now resolve correctly instead of
  a hard parse error — `bless {...}, __PACKAGE__` and
  `__PACKAGE__->method(...)`-style OO constructor idioms (very common
  in CPAN modules) now work. Two auto-quote contexts (`__PACKAGE__ =>
  1` and `$h{__PACKAGE__}`) needed explicit handling to keep matching
  real Perl's literal-bareword behavior there. Split off `__SUB__`
  (current-sub reference — needs real closure-capture support, not
  just a constant substitution) as **D124** — logged, not fixed.
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

**Open generated-code defects:** **D101, D103, D104, D106, D108, D110,
D120, D124, D125, D126, D128, D130, D131** (see `TESTS.md`). **D54**
(tooling): `perlc_tsan` can hang compiling `tests/threads.pl`
(TSan+`fork` of clang); workaround `TSAN_OPTIONS=die_after_fork=0`.

**2026-09-10 real-module survey #2:** 11 more real system scripts,
targeting `Pod::Usage`, `Encode`, `File::Copy`, `Storable`, and others
not covered by survey #1. Found and **fixed D127** (`Pod::Usage`'s
`&pod2usage`-sigil export broke `scanExports()`'s match — likely
blocked more real CLI scripts than anything else found so far, since
`pod2usage()` for `--help`/`--man` is nearly universal). Re-verifying
D127 against the real, unmodified `Pod::Usage.pm` got past the export
issue and hit a new, separate bug — **D129** (`local($var) = EXPR;`,
parenthesized single-variable `local`, is a parse error) — logged, not
fixed. Also found **D128** (a parse error inside an inlined module
reports a misleading line number/no filename — diagnostics issue, not
correctness). Confirmed `File::Copy` (missing entirely, 10 real scripts
hit it) should join the Tier 1 CPAN candidate list in
`MVP_ROADMAP.md`, and reconfirmed `Storable::dclone` and the
`%EXPORT_TAGS`/`:tag`-import gap are real (both already tracked). Full
table: `TESTS.md` → "CPAN-module compile survey #2".

**2026-09-10 five-agent MVP review:** a code-reviewer/architect, three
engineers (runtime/codegen/parser depth), and a PM assessed real-world
readiness for "common CPAN modules work." Found and byte-for-byte-
verified **8 new defects (D111–D118)**; **D111–D114 are now fixed**, and
the pre-existing **D109 is now fixed too** (see above) — as is **D116**
(`__PACKAGE__`/`__FILE__`/`__LINE__`, split from `__SUB__` which is now
**D124**, harder, still open) and **D117** (`perl_atof_decimal` hand-
rolled float parser → `strtod`, plus now-recognized `Inf`/`Infinity`/
`NaN` string coercion — split off **D125**, found while testing D117:
`use`/`no` pragmas only parse at file top-level, not inside a nested
block/sub). The remaining open ones are D118 (`split` missing LIMIT +
trailing-empty-trim), D115 (bare
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
