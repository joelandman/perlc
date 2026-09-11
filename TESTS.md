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

**2026-09-10: 263 compared (pre-D107), 263 PASS, 0 FAIL** — re-verified
after the D107 fix with no regression, plus 2 new D107 tests verified
separately via the harness (265 total once counted together).

Skipped by default: `dbi_sqlite.pl`, `xs_ffi.pl`, `pidigits.pl`.

New (2026-09-09/10): `Getopt::Long`, `Data::Dumper`, `File::Basename` —
see "Real-world module survey" below. Plus the D99/D100/D105/D107 fix
tests.

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
| D101 | **FIXED 2026-09-11** | `each %hash` in scalar context returned the pair length (0/1/2), not the key. See below. |
| D102 | **FIXED** (2026-09-10) | `die REF` / `die $blessed_obj` lost the reference — `$@` became a stringified `TYPE(0xaddr)` plus a wrongly-appended `" at FILE line N."`. Broke OO exception handling. See below. |
| D103 | OPEN (correctness, low freq.) | Integer overflow uses wrapping signed 64-bit arithmetic instead of Perl's IV→UV→NV promotion; values at/beyond the `2**63` boundary silently go wrong or print in scientific notation instead of exact digits. See below. |
| D104 | OPEN (missing syntax) | Indented heredoc `<<~IDENT` (Perl 5.26+) is not recognized by the lexer at all — hard parse error, not silent-wrong-data. See below. |
| D106 | OPEN (correctness, narrow, found while fixing D105) | Same bug class as D105 but for a FLAT_ARRAY/FLOAT_PAIR ref read back out of an array/hash element (`$arr[0]`, `$h{k}`) rather than a plain scalar variable — a second alias made from that read doesn't see further writes. Deliberately not fixed alongside D105: the fix location (`case NK::ArrayElem`/`HashElem` in `emitExpr`) sits right next to the exact fast-path code that caused a segfault regression while fixing D105 (2D compound-assign, `llvm.assume(tag==FLAT_ARRAY)`). See below. |
| D108 | OPEN (correctness, narrow, found while fixing D107) | Plain double-quoted string literals (`"..."`, unrelated to `s///`) don't recognize `\f`/`\a`/`\e`/`\b` — they pass through as literal backslash+letter. `src/lexer.cpp`'s double-quoted-string escape switch only has cases for `n t r 0 x \ ' " $ @`. Narrow, low real-world frequency. See below. |
| D109 | **FIXED** (2026-09-10) | `s///` replacement text didn't support arbitrary variable interpolation (`$name`, `@arr`) — only `$0`-`$9`/`$&` (capture refs) worked, even though real Perl parses the replacement like a double-quoted string. See below. |
| D120 | OPEN (correctness, split off D109's widened scope, found 2026-09-10) | The *general* string-interpolation engine used by plain `"..."` literals (not just `s///`, which D109 now separately fixes) is wrong for `$$aref[0]` (prints `[0]`) and `@{$r}[0,1]` (prints `1 2 3[0,1]`) — subscripted dereference inside a double-quoted string. `src/parser.cpp`'s `parseStringInterp` explicitly documents this as a known, unfixed gap in its own `$$` handling comment. See below. |
| D121 | **FIXED** (2026-09-10) | `$::name` / `@::arr` / `%::hash` (Perl's shorthand for `$main::name` — a bare `::` package prefix meaning "main") was a hard parse error ("unexpected token ':'"). Common in older/sysadmin-style Perl (found via the real `/usr/bin/ucfq` script). See below. |
| D122 | **FIXED** (2026-09-10) | `inlineModules`'s `scanExports()` (`src/main.cpp`) only recognized `our @EXPORT[_OK] = qw(...)` — it missed the (still common, used by core `File::Path`) older `use vars qw(@EXPORT_OK); @EXPORT_OK = qw(...)` style, where the assignment isn't prefixed with `our`. Caused a false "X is not exported by the Y module" compile error for a name that real Perl does export. See below. |
| D111 | **FIXED** (2026-09-10) | `my %c = %h;` (hash-to-hash copy) silently produced wrong contents — not a fresh copy. Corrected a wrong claim in D99's write-up below (`did NOT have this bug`). See below. |
| D112 | **FIXED** (2026-09-10) | `inlineModules()` (`src/main.cpp`) splices a `.pm`'s tokens directly into the main token stream with no lexical scope boundary — a module's file-scope `my $x` collided with the main script's `my $x` of the same name. See below. |
| D113 | **FIXED** (2026-09-10) | No "unimplemented" signal: calling an undefined sub silently returned `undef` (real Perl: fatal `Undefined subroutine ... called`, exit 255) and an unresolvable `use Some::Module;` was silently dropped instead of erroring; `use lib`/`-I`/`PERL5LIB` weren't honored. See below. |
| D114 | **FIXED** (2026-09-10) | Array slices `@x[LIST]` returned only one element when the subscript list was non-literal — a range (`@x[1..2]`) or an array variable (`@x[@i]`). Hash slices `@h{...}` already dispatched correctly for the equivalent cases; array slices weren't ported to the same dispatch. See below. |
| D115 | **FIXED** (2026-09-10) | Bare `return;` in list context yielded a 1-element list instead of Perl's empty list — broke `my %h = (k => f())`-style "return nothing on failure" patterns. See below. |
| D130 | **FIXED 2026-09-11** | `if (my @arr = EXPR)` — a single ARRAY/HASH variable declared inline as an `if`/`while` condition — was a hard parse error ("unexpected token 'my'"). See below. |
| D131 | **FIXED 2026-09-11** | `our $var;`/`our @arr;`/`our %hash;` declared inside a nested bare `{ }` block, or repeated as a bare redeclaration anywhere, didn't work correctly. See below. |
| D116 | **FIXED** (2026-09-10, `__PACKAGE__`/`__FILE__`/`__LINE__` only) | `__PACKAGE__` / `__FILE__` / `__LINE__` were not implemented at all (hard parse error) despite `bless {...}, __PACKAGE__` being one of the most common OO-Perl idioms in CPAN modules. `__SUB__` (reference to the currently-executing sub) is intentionally not covered — harder, split off as **D124**. See below. |
| D124 | OPEN (missing syntax, split off D116's `__SUB__` case, found 2026-09-10) | `__SUB__` (a reference to the currently-executing sub, needed for anonymous recursion — `use feature 'current_sub'`) is still a hard parse error. Needs codegen support for a reference to the current closure's own captures, not just a compile-time constant substitution like `__PACKAGE__`/`__LINE__`/`__FILE__`. See below. |
| D117 | **FIXED** (2026-09-10) | `perl_atof_decimal` (`src/runtime.c`) was a hand-rolled decimal-string→float parser (manual digit accumulation plus a repeated-multiply exponent loop) instead of `strtod`, accumulating rounding error on ordinary decimal strings — every implicit string→number coercion goes through it. See below. |
| D125 | OPEN (missing syntax, found 2026-09-10 while testing D117) | `use`/`no` pragma statements (`use strict;`, `no warnings 'numeric';`, etc.) are only recognized at the very top level of a file — nested inside a `sub {}` or a bare `{ }` block, they're a hard parse error ("unexpected token 'warnings'"/"'use'"). Root cause: the `use`/`no` handling (`src/parser.cpp:73`) lives in `parseProgram()`, not in the general `parseStmt()` every nested block/sub actually uses. See below. |
| D118 | **FIXED** (2026-09-10) | `split` had no 3rd LIMIT argument at all (hard parse error, not just silently ignored) and didn't trim trailing empty fields from the result, unlike real Perl's default `split` behavior. See below. |
| D126 | OPEN (correctness, found 2026-09-10 while testing D118) | `split(/(,)/, $str)` — a split pattern with a capturing group — doesn't include the captured delimiter text in the result the way real Perl does (`split(/(,)/, "a,b,c")` should give `("a", ",", "b", ",", "c")`, 5 elements; perlc gives `("a","b","c")`, 3). Pre-existing, confirmed unrelated to the D118 fix (reproduced on the pre-D118/D117 binary too). See below. |
| D127 | **FIXED** (2026-09-10) | `scanExports()` stored an export name with a leading `&`/`*` sigil verbatim (e.g. real `Pod::Usage.pm`'s `our @EXPORT = qw(&pod2usage);`) instead of stripping it, so it never string-matched a plain `pod2usage` explicit import *or* an unqualified `pod2usage()` call after a bare `use Pod::Usage;`. High real-world impact — `pod2usage()` for `--help`/`--man` handling is one of the most common patterns in documented Perl CLI tools. See below. |
| D129 | **FIXED** (2026-09-10) | `local($var) = EXPR;` — a parenthesized, single-variable list-form `local` — was a hard parse error ("expected $ but got '('"). Found in real, unmodified `Pod::Usage.pm` (`local($_) = shift;`). See below. |
| D128 | OPEN (tooling/diagnostics, found 2026-09-10 real-module survey #2) | A parse error occurring *inside* an inlined module (`use Some::Module;`) is reported with a line number belonging to the wrong file (the main script's own line count at the point of inlining, not the module's internal line count) and no indication of which file the error is actually in — makes a real bug inside a `use`d module very hard to diagnose. Found via `corelist`/`podchecker` (both real system scripts) reporting implausibly early line numbers. See below. |
| D119 | **FIXED** (2026-09-10) | `scalar(keys %$href)` (keys on a deref'd hashref, in scalar context) returned `0` instead of the key count — `scalar(keys %h)` on a plain named hash and list-context `keys %$href` were both correct, so this was specific to the scalar-context + deref-hash combination. See below. |
| D110 | OPEN (correctness, found while implementing Data::Dumper; scope widened 2026-09-10) | `$Package::var` (an arbitrary fully-qualified global not declared via `our`) is not a true cross-scope global — it auto-vivifies as a plain variable in whatever scope first references it, so setting it at file scope is invisible from inside an unrelated `sub`. General bug, not module-specific; found via `$Data::Dumper::Sortkeys`. Widened 2026-09-10 while verifying D121: the same gap applies to `@Package::arr`/`%Package::hash` too, and more severely — an undeclared qualified array/hash doesn't just fail to cross scopes, whole-array/hash access (`my @c = @main::arr`, not just elements) returns nothing at all, vs. an `our`-declared array/hash (which works correctly, cross-package, today). See below. |
| D99 | **FIXED** (2026-09-09) | `my @b = @a;` aliased storage — mutating `@b` mutated `@a`. Fixed in `src/codegen.cpp:4118-4149` (`case NK::My`, `isArr` branch): a borrowed pointer from `emitArrayPtr` (plain `@var`, `@$ref`, `->@*`) is now always copied into a fresh array via `perl_array_new`+`perl_array_extend`, instead of being declared directly as the new variable's backing store. Tests: `tests/d99_array_copy_smoke.pl`, `tests/d99_array_copy_deep.pl`. |
| D100 | **FIXED** (2026-09-09) | List-assignment used as a boolean condition (`while (my ($k,$v)=...)`, `if ((...)=...)`) always evaluated false — loop/branch body never ran. Two stacked bugs, both fixed: (1) `case NK::Assign` (ArrayLit LHS) always returned a void/null PerlValue*; now returns `perl_array_len(rhsArr)` (real Perl's list-assignment-in-scalar-context semantics), which every existing consumer already handles correctly since `perl_array_len` was already a registered "owned temp". (2) The expression-context `my ($a,$b) = EXPR` parse wraps each variable as a bare `NK::My` node that `emitLValue()` didn't understand, so `$k`/`$v` stayed undef even once the loop iterated correctly; fixed by declaring `NK::My` LHS elements directly in the assignment loop, with `While` hoisting the one-time alloca before the loop (mirroring the existing single-variable `myCondPv` hoist) so a long-running loop doesn't re-execute an alloca (and leak stack) every iteration — verified with a 50k-iteration stress test. Tests: `tests/d100_list_assign_cond_smoke.pl`, `tests/d100_list_assign_cond_deep.pl`. |
| D105 | **FIXED** (2026-09-09) | Reads through one alias to a shared array-ref (`$m[0][0]` after `my @m2=@m; $m2[0][0]=99`, or a bare `$inner->[0]=55` then reading `$m[0][0]` where `$m[0]==$inner`) didn't see writes made through another alias. Root cause: FLAT_ARRAY/FLOAT_PAIR (Stage 22/23's compact storage for numeric anon-array-ref literals) were deep-copied on clone instead of preserving reference identity. Fixed via a new `perl_promote_ref_array()` in `src/runtime.c` that lazily promotes FLAT_ARRAY/FLOAT_PAIR to a real `PERL_REF_ARRAY` in place (reusing `perl_deref_array`'s existing lazy-conversion) at the exact points a second alias can be created — `case NK::ScalarVar` reads in `src/codegen.cpp` (covers push/sub-args/hash-values/return/etc. generically) plus D99's array-to-array copy path — while leaving fresh literal construction (`@bodies = ([1,2,3], ...)`) untouched, so Stage 22/23 keeps its fast path. See below. |
| D107 | **FIXED** (2026-09-10) | `s/\\/\\\\/g; s/\n/\\n/g;` on a string with no literal backslashes doubled the inserted backslash. Root cause: `perl_regex_subst`'s replacement-expansion loop (`src/runtime.c`) only handled `$0`-`$9`/`$&` and copied every other character — including backslash escapes — verbatim; the lexer hands it the replacement text 100% raw with zero escape processing of its own. Fixed by adding `\\ \n \t \r \f \b \a \e \0 \$ \@` recognition to that loop, with an unrecognized `\X` falling back to bare `X` (matching Perl). Found via the real `/usr/bin/debconf-escape` script. Tests: `tests/d107_subst_replace_escapes_{smoke,deep}.pl`. See below. |

D1–D53, D55–D98 are **FIXED** (or STALE/N/A). The long-form registry with
root causes is in git history (`TESTS.md` prior to 2026-08-26).

## 2026-09-09 correctness audit

`make test-all` reproduced cleanly at **251/251 PASS, 0 FAIL** — the
existing suite's claims are accurate for what it covers. To check
correctness *beyond* that suite (per project priority: correctness first),
~25 small hand-written probes were compiled with `./perlc` and diffed
against real `perl` output, covering idioms the 254 `tests/*.pl` files
don't happen to exercise. 6 defects found, all reproducible, none crash
(they silently produce wrong output or hard-fail to parse). None of these
are logged as passing/failing in the harness because no test file exercises
them — that gap in coverage is itself a finding (see the completeness list
below: "smoke+deep tests for D99–D104").

### D99 — `my @new = @existing;` aliased instead of copying — **FIXED 2026-09-09**

```perl
my @a = (1,2,3);
my @b = @a;
$b[0] = 99;
print "a=@a b=@b\n";   # perl: a=1 2 3 b=99 2 3   |  was: a=99 2 3 b=99 2 3 (pre-fix)
```

Root cause was `src/codegen.cpp:4118-4132` (`case NK::My`, `isArr` branch).
When the initializer was itself array-shaped (`emitArrayPtr(*n.right)`
succeeded — true for a plain `@a`, `@$ref`, etc.), the returned
`PerlArray*` was declared directly as the new variable's backing store
with no copy:
```cpp
if (n.right) { callCtx_ = 1; av = emitArrayPtr(*n.right); callCtx_ = 0; }
...
declareArray(nm, av);   // av IS the RHS array, not a clone
```
Compare the **correct** sibling path for plain (non-`my`) assignment,
`@dd = @c;`, at `src/codegen.cpp:6125-6132`, which allocates/keeps a
distinct `PerlArray*` for the LHS and calls `perl_array_replace(av_lhs,
av_rhs)` to copy contents in. Hash declarations (`my %h2 = %h1`) did NOT
have this bug — they always allocate fresh via `perl_hash_new` +
`perl_hash_from_list` (verified by probe); this was array-declaration
specific.

**Fix** (`src/codegen.cpp:4118-4149`): classify the RHS the same way
`Foreach`'s existing `ownsTmpArr` check already does — `NK::ArrayVar`,
`NK::DerefArray`, and `NK::PostfixDeref` (`->@*`) return a *borrowed*
pointer from `emitArrayPtr`; everything else (`Range`, `sort`, `map`,
sub-call results, etc.) returns an already-fresh one. For the borrowed
case, allocate a new `PerlArray` and `perl_array_extend` the borrowed
array's elements into it (each element is cloned via `perl_clone`, which
already does the correct thing for refs — bump the refcount and share the
pointee, not a deep copy — so `my @b = @a` where `@a` holds arrayrefs
still shares the *inner* arrays, matching real Perl's shallow-copy
semantics). For the fresh case, keep aliasing directly (no wasted copy).

**Correction (2026-09-10):** the claim above that "hash declarations
(`my %h2 = %h1`) did NOT have this bug ... verified by probe" was
**wrong** — it was verified against the *aliasing* symptom only, not
against correct contents. `my %c = %h;` has a *different* (and worse)
bug: it doesn't alias, but it also doesn't copy correctly. See **D111**
below.

Verified against real Perl for: plain array copy, copy-after-push, `@$ref`
deref copy, `->@*` postfix deref copy, independent sub-return arrays,
`our` (file-scope) array copy, range-array copy, the scalar-RHS fallback
(`my @arr = $ref`), and shallow-copy-of-refs preservation. `make test-all`
re-run clean at 251/251 after the fix. Tests: `d99_array_copy_smoke.pl`,
`d99_array_copy_deep.pl`.

Previously contradicted `README.md`'s documented behavior ("`@arr =
@other` copy").

### D105 — aliased reads through a shared array-ref don't see writes — **FIXED 2026-09-09**

```perl
my $inner = [1, 2];
my @m  = ($inner, [3, 4]);
my @m2 = @m;                 # $m[0] and $m2[0] are confirmed == (same ref)
$m2[0][0] = 99;
print "$m[0][0]\n";          # perl: 99   |   was: 1 (stale, pre-fix)

# reproduced with NO copy involved at all:
$inner->[0] = 55;
print "$m[0][0]\n";          # perl: 55   |   was: still stale
```

**Root cause** (not a "DerefAV cache" bug as first suspected): `[1, 2]` and
`[1, 2, 3]` — an anonymous array-ref literal with 2+ all-numeric elements —
don't build a real `PERL_REF_ARRAY`. Codegen's "Stage 22/23" optimization
(`case NK::AnonArray`, `src/codegen.cpp` ~7717 today) represents them
compactly instead: exactly-2-numeric as `PERL_FLOAT_PAIR` (the two doubles
inlined into the `PerlValue` struct itself), 3+-all-numeric as
`PERL_FLAT_ARRAY` (a raw `double[]` in `pval`). Both are still genuine
Perl references (`ref()` returns `"ARRAY"`, `$m[0] == $inner` compares
true) — but `perl_clone()` (`src/runtime.c`) treated them as plain values
and **deep-copied** them on every clone (a fresh `double[]`/pair for
FLAT_ARRAY, or a flat struct copy for FLOAT_PAIR) instead of preserving
reference identity the way it already does for `PERL_REF_ARRAY` (bump a
refcount, share the pointee). Every `perl_array_push` (called by list/array
construction, `push`, sub-arg binding, etc.) clones its argument, so the
moment a FLAT_ARRAY/FLOAT_PAIR-tagged ref got pushed a second place — into
another array, a hash value, `@_`, a closure capture, anywhere — the two
copies silently forked into two independent arrays.

**Why this wasn't simply "always promote FLAT_ARRAY/FLOAT_PAIR to
REF_ARRAY on every clone":** that was tried first and immediately
segfaulted `tests/d96_flat_row_op_assign.pl`. That test's `@bodies =
([...7 numeric elements...], [...], ...)` builds every row as a fresh
FLAT_ARRAY literal, and each row gets pushed into `@bodies` (cloned) once,
immediately, as part of ordinary array-literal construction — with no
other alias anywhere. Blanket promotion-on-clone converted every row to
REF_ARRAY right there at construction time, before some other codegen
path's `llvm.assume(tag==FLAT_ARRAY)` (baked in for exactly this "known
all-numeric literal row" case) ran — violating that assumption is
undefined behavior, hence the segfault. This also would have defeated the
whole point of Stage 22/23 (fast unboxed numeric row storage) for the
overwhelmingly common case (fresh literal rows, never separately aliased)
in exchange for fixing a correctness bug that only matters when a second
alias actually exists.

**Fix:** added `perl_promote_ref_array(PerlValue*)` (`src/runtime.c`) —
a no-op for every tag except FLAT_ARRAY/FLOAT_PAIR, for which it reuses
`perl_deref_array`'s existing lazy in-place promotion (mutate `tag` to
`PERL_REF_ARRAY`, materialize a real backing `PerlArray*`) — and calls it
only at the specific points where a *second* alias of an existing value
can be created, never at fresh-literal-construction time:
- `case NK::ScalarVar` in `emitExpr` (`src/codegen.cpp`) — the single
  choke point every "hand this variable's current value to something
  that will clone it" idiom reads through (`push`, sub-call args,
  hash/array-element assignment, `return`, list literals, closures, …).
  A fresh `AnonArray`/`AnonHash` literal is never read back through a
  `ScalarVar` before its first real use, so this never fires for
  freshly-constructed rows — only for a value that was bound to a name
  and is now being read again.
- D99's array-to-array copy path (`perl_array_promote_refs(PerlArray*)`,
  promotes every element of the source array) — needed because that copy
  operates on a runtime `PerlArray*`, not AST nodes, so there's no
  "was this a ScalarVar" distinction available; promoting unconditionally
  there is both correct and cheap (whole-array copies are not a hot-loop
  operation the way scalar reads are).

Verified against real Perl for: the original two repros, `my $y =
$inner;` (scalar-to-scalar copy), `push @arr, $r;`, passing as a sub
argument, storing as a hash value, `my @b = @a` where `@a`'s elements are
FLAT_ARRAY/FLOAT_PAIR refs (D99×D105 interaction), return-value aliasing,
ternary-result aliasing, closures capturing a FLAT_ARRAY-holding variable,
and a nested array-of-array-of-refs copy chain — all match real Perl.
`tests/d83_ref_flat_array{,_smoke}.pl`, `tests/d96_flat_row_op_assign{,_smoke}.pl`,
and `tests/d98_flat_row_2d{,_smoke}.pl` (the existing Stage 22/23
correctness tests) still pass unchanged, confirming the fast path for
fresh literal construction is intact. `bench/nb.pl` and `bench/mbs.pl`
(numeric-heavy benchmarks exercising this exact optimization) still
produce byte-for-byte correct output. `make test-all` re-run clean at
253/253 (251 + the 2 D99 tests) after the fix. Tests:
`tests/d105_flat_ref_alias_smoke.pl`, `tests/d105_flat_ref_alias_deep.pl`.

**Known remaining narrower gap — logged as D106, not fixed:** promotion is
scoped to `ScalarVar` reads and the D99 array-copy path. Confirmed still
broken (tested, not speculative): a FLAT_ARRAY/FLOAT_PAIR ref read back out
of a **hash or array element** (as opposed to a plain scalar variable) and
aliased a second time does not see further writes:
```perl
my @arr = ([1,2,3]);
my $y = $arr[0];
$y->[0] = 99;
print "$arr[0][0]\n";   # perl: 99   |   perlc: 1 (stale)
```
(identical failure for a hash element, `$h{k}` in place of `$arr[0]`).
`case NK::ArrayElem` / `case NK::HashElem` in `emitExpr` (`src/codegen.cpp`
~5589, ~7254) are the analogous choke points to the `ScalarVar` fix above,
but this exact neighborhood is where the D96 segfault regression came
from during this fix (compound-assign on `$P[$i][$k]` and other 2D
fast-path reads have their own `ArrayElem`-vs-`ArrowDeref` and
`llvm.assume(tag==FLAT_ARRAY)`-sensitive branches living right next to the
plain scalar-read code) — extending promotion there risks a repeat of that
crash without a much more careful read of every fast path sharing this
code. Deliberately left open rather than risking a rushed, under-verified
change in a proven-fragile area; the `ScalarVar` fix already covers the
common cases (bare `my $x = ...`, `push`, sub args, `return`, list
literals) and this narrower one only bites when a FLAT_ARRAY/FLOAT_PAIR
ref is round-tripped through an array/hash element specifically.

### D100 — list assignment as a boolean condition never runs — **FIXED 2026-09-09**

```perl
my %h = (a=>1,b=>2,c=>3);
my $n = 0;
while (my ($k,$v) = each %h) { $n++; }
print "n=$n\n";   # perl: n=3   |   was: n=0
```
Also failed for `if ((...) = ...)`, and for any function on the RHS, not
just `each` — also confirmed with `while (my ($x,$y) = splice(@list,0,2))`.

**This turned out to be two stacked bugs**, both needed for the idiom to
actually work (fixing only the first got the loop to iterate the right
number of times, but with `$k`/`$v` staying `undef` the whole time):

**Bug 1 — truth value.** `src/codegen.cpp` (`case NK::Assign`, the
`n.left->kind == NK::ArrayLit` branch) performed the list assignment and
then `return llvm::ConstantPointerNull::get(perlPtrTy_);` unconditionally
— by design for the common *void*-context case (`my ($a,$b) = @list;` as
a bare statement). `While`/`If` condition codegen has a special-cased
fast path only for `n.cond->kind == NK::My` with a **scalar** name
(`while (my $line = <$fh>)`); anything else — including this
`Assign`-of-`ArrayLit` node — falls through to the generic
`cv = emitExpr(*n.cond); callRT("perl_is_true", {cv})`, and `perl_is_true`
on that null pointer harmlessly (not a crash) reports false, so the
loop/branch body never ran. **Fix:** return `perl_array_len(rhsArr)`
instead — real Perl's list-assignment-in-scalar-context semantics (the
count of RHS elements) — which conveniently was already a registered
"owned temp" (see `isOwnedTemp`), so `ExprStmt`, `emitBlockLast`, `If`,
and `While` all handle it correctly via their existing generic logic;
no other call site needed to change.

**Bug 2 — loop variables stayed undef.** The parser's expression-context
form of `my ($a, $b, ...) = EXPR` (used inside a `while`/`if` condition —
distinct from the statement-level `my ($a,$b) = EXPR;` form, which
desugars into separate declaration statements followed by an `Assign`
whose LHS is already-declared `ScalarVar`s) wraps each variable as a bare
`NK::My` node directly inside the `ArrayLit`. `emitLValue()` only
understands `ScalarVar`/`DollarAt`, so an `NK::My` element fell through
to `default: return nullptr`, silently skipping the assignment for that
variable entirely. **Fix:** handle `NK::My` directly in the per-element
assignment loop — look it up first (`lookupVar`) and only allocate if not
already declared. That "look up first" matters because `While` now
hoists one alloca per variable *before* the loop starts (mirroring the
existing single-variable `myCondPv` hoist) — the per-element assignment
loop lives inside the `while.cond` basic block, which is a loop
back-edge target, so an un-hoisted `alloca` placed there would
re-execute (and leak stack) on every iteration instead of running once.
Verified this doesn't regress with a 50,000-iteration `while (each ...)`
stress test (no crash, exact count). `If` needs no such hoist — its
branches aren't loop-carried, so a fresh per-evaluation alloca there is
fine and already how the pre-existing single-variable `if (my $x = ...)`
form has always worked.

This fixed the standard Perl "drain an iterator" idiom, which comes up
constantly for `each`, `splice`-based chunking, and multi-value generator
subs — the single highest-impact correctness bug found in the audit.
Tests: `tests/d100_list_assign_cond_smoke.pl`,
`tests/d100_list_assign_cond_deep.pl` (covers `while`/`if`/`until`/
`unless`, bare list-assignment without `my`, `splice`-based draining, an
if-inside-while nesting, the statement-level form staying unaffected, and
the 50k-iteration stress test).

### D101 — `each %hash` in scalar context returns the wrong value — **FIXED 2026-09-11**

```perl
my %h = (a=>1,b=>2,c=>3);
while (my $k = each %h) { print "$k\n"; }
# perl:  c / a / b  (actual keys, order unspecified)
# was:   2 / 2 / 2   (always the pair-array length, never a key)
```
Root cause: `src/codegen.cpp`'s scalar-context `case NK::EachFunc`
called `perl_each_hash` to get the `[key,val]` pair array, then
returned `perl_array_len(av)` — the *count* of the pair (0, 1, or 2) —
not element 0 (the key). Masked in casual testing because a truthy 2
happens to make simple `while (each ...)` loops iterate the right
*number* of times even though every `$k` is wrong.

**Fix**: return `perl_array_get(av, 0)` instead — the existing
runtime accessor already returns `undef` for an out-of-range index,
which is exactly the post-exhaustion case (an empty pair array), so no
new runtime code was needed. List-context `each %hash` (`my ($k,$v) =
each %h`) was already correct and untouched — only the scalar-context
case had the bug.

Verified against real Perl for: a multi-key hash iterated to
exhaustion (keys collected and sorted, since real iteration order is
unspecified), a single-key hash (first call returns the key, second
returns `undef`), an empty hash (immediately `undef`), and
re-iterating a hash after it auto-resets past exhaustion. Tests:
`tests/d101_each_scalar_{smoke,deep}.pl`.

### D102 — `die REF` loses the reference (breaks OO exceptions) — **FIXED 2026-09-10**

```perl
eval { die { code => 42 }; };
print ref($@), "\n";      # perl: HASH   |   was: (empty) (perlc, pre-fix)
print "$@\n";              # perl: HASH(0x...)   |   was: HASH(0x...) at FILE line N. (pre-fix)
```

Root cause (`src/runtime.c` `perl_die`, pre-fix): unconditionally called
`perl_to_string_dup(msg)` at the very start regardless of `msg`'s tag,
immediately losing reference identity for `die REF`/`die
$blessed_obj` — the standard Perl idiom for typed/OO exceptions
(hand-rolled or `Exception::Class`-style). Also wrongly appended the
`" at FILE line N."` location suffix even to a reference — confirmed
directly against real Perl that this suffix is *never* appended to a
reference, at top level or inside `eval` alike (only a plain string
message gets it).

**Fix**: `perl_die` now checks whether `msg` is a reference (any of
`PERL_REF_ARRAY`/`PERL_REF_HASH`/`PERL_REF_SCALAR`/`PERL_CODE_REF` —
covers plain and blessed refs alike, since blessing only sets
`blessed_class` alongside one of these tags, not a separate tag) before
deciding how to handle it: a reference skips the location-suffix logic
entirely and is assigned into `$@` via `perl_assign(&s_dollar_at, msg)`
— the same primitive already used elsewhere for scalar globals, which
already handles reference refcounting and `blessed_class` propagation
correctly — instead of being stringified into a throwaway string first.
The `$SIG{__DIE__}` handler now also receives the original reference
(not a freshly-stringified copy) when one was thrown, matching real
Perl's handler semantics. A plain string message (or no message at
all) is completely unaffected — same stringify + location-suffix path
as before. Verified against real Perl for: hashref, arrayref,
scalarref, and a blessed object, plus regression checks that plain
string dies (with and without a trailing newline) and a bare `die;`
(defaults to `"Died"`) are unchanged. Tests:
`tests/d102_die_ref_{smoke,deep}.pl`.

### D103 — integer overflow wraps instead of promoting (low frequency, but silent)

```perl
my $big = 9223372036854775807;  # IV_MAX
print $big + 1, "\n";            # perl: 9223372036854775808  |  perlc: -9223372036854775808 (wrapped)
print -9223372036854775808, "\n"; # perl: -9223372036854775808 (exact) | perlc: -9.22337203685478e+18
```
Real Perl's numeric model auto-promotes IV arithmetic that overflows
64-bit signed range into UV (if positive and within 64-bit unsigned range)
or NV (double), and its stringifier prints whole-valued NVs at this
magnitude using exact digits, not `%g`-style scientific notation. perlc's
"W1: I64 fast path" (see git log) does native wrapping `i64` arithmetic
with no overflow check, and literals beyond `INT64_MAX` fall back to a
plain `double` with no special-case exact-integer formatting on print.
Affects only values within ~`LLONG_MAX` of the 64-bit boundary — rare in
ordinary scripts, but silent (wrong answer, no warning) rather than a
crash, which is why it's still worth fixing ahead of new features.

### D104 — indented heredoc `<<~IDENT` unsupported (parse error, not silent)

```perl
sub f {
    my $x = <<~END;
        hello
        END
    return $x;
}
```
`./perlc` reports `Parse error line N: unexpected token '<<'`. `grep -n
"<<~" src/lexer.cpp` finds nothing — the `<<~` indented-heredoc form
(Perl 5.26+, strips the terminator's leading whitespace from every body
line) was never added; only `<<IDENT`, `<<"IDENT"`, `<<'IDENT'` are
recognized. This one fails loudly (compile error) rather than silently,
so it's lower risk than D99–D103, but it's a syntax form common enough in
modern Perl (used heavily for readable multi-line strings inside indented
code) that it belongs on the missing-features list too.

## Remaining product gaps (not logged as D-numbers)

Full XS (FFI is not DynaLoader); complex CPAN (advanced `our`/OO — POD is
skipped). Typeglob `{IO}`/`{FORMAT}` slots are not implemented. String
`eval EXPR` sees outer `my`. Runtime `eval`/`do` still needs clang+perlc
on the target.

**2026-09-09 note:** `src/runtime.c` (commit `706b478`, "updates") added
`perl_glob_set_io`/`perl_glob_get_io`/`perl_glob_slot` — runtime support
for `*FH{IO}` / `*FH{SCALAR}` / `*FH{FORMAT}` etc. — but nothing in
`src/parser.cpp` or `src/codegen.cpp` calls these (`grep -rn
"perl_glob_slot\|perl_glob_get_io" src/codegen.cpp src/parser.cpp` is
empty). This is dead code: the runtime half of `*FH{IO}` exists but the
syntax `*FH{IO}` still isn't parsed, so the feature is not actually usable.
Finishing the parser/codegen wiring is now most of the way there — see
the missing-features priority list below.

**2026-09-10 note:** there is no `Exporter`/`@EXPORT`/`@EXPORT_OK`/
`import` mechanism at all (grepped `src/codegen.cpp`/`src/parser.cpp`
for `Exporter`/`import`: zero hits). `use Foo;` only does something
meaningful for the hardcoded set of builtin modules perlc special-cases
(List::Util, Scalar::Util, POSIX, Getopt::Long, Data::Dumper,
File::Basename, Carp, etc.). An ordinary pure-Perl CPAN module that
defines subs and relies on `Exporter` to import them unqualified into
the caller's namespace has no support path today — this sits outside
the current whole-program single-token-stream model (see D112) and is
architecturally the single biggest blocker to "arbitrary pure-Perl CPAN
module just works," bigger than any individual D-numbered item. See
`MVP_ROADMAP.md`.

See "PRIORITY LISTS" below for the ranked top-10 correctness fixes and
top-10 missing features that came out of the 2026-09-09 audit, and
`MVP_ROADMAP.md` for the 2026-09-10 multi-agent review's re-ranking
against the "common CPAN modules work" goal specifically.

## PRIORITY LISTS (2026-09-09 audit; superseded in priority order by MVP_ROADMAP.md's 2026-09-10 Tier 0/1/2 — kept here for history)

### Top 10 correctness fixes (ranked; do these before new features)

1. ~~**D99** — `my @b = @a;` aliases array storage instead of copying.~~ **FIXED 2026-09-09**, see above.
2. ~~**D105** — aliased reads through a shared array-ref don't see writes made through another alias.~~ **FIXED 2026-09-09**, see above.
3. ~~**D100** — list assignment as a boolean condition always evaluates false.~~ **FIXED 2026-09-09**, see above.
4. ~~**D107** — `s///` replacement text doubles backslash escapes (and never processed `\t`/`\n`/etc. either).~~ **FIXED 2026-09-10**, see above.
5. **D109** — arbitrary variable interpolation (`$name`, `@arr`) in `s///` replacement text doesn't work at all — only `$0`-`$9`/`$&` (capture refs) are recognized. Found while fixing D107; needs codegen-level string-interpolation support for the replacement text (the same machinery regular `"..."` interpolation already has), materially bigger than D107 itself. Now the top open correctness item — real-world-sourced.
6. **D110** — `$Package::var` isn't a true cross-scope global — an arbitrary fully-qualified variable (not declared via `our`) auto-vivifies as a plain variable in whichever scope first references it, so setting it at file scope and reading it inside an unrelated `sub` sees nothing. Found while implementing Data::Dumper's `$Data::Dumper::Sortkeys` support. General bug, not module-specific.
7. **D106** — same bug as D105, narrowed to reading a FLAT_ARRAY/FLOAT_PAIR ref back out of an array/hash element (`$arr[0]`, `$h{k}`) rather than a scalar variable. Deliberately left open — the fix site sits next to the exact fast-path code that caused a segfault regression while fixing D105.
8. **D102** — `die REF` / `die $blessed_obj` loses the reference; `$@` is always stringified and gets a wrongly-appended `" at FILE line N."` (`src/runtime.c` die/eval-catch path). Breaks all OO/typed exception handling — real-world impact, not a corner case.
9. **D108** — plain double-quoted string literals don't recognize `\f`/`\a`/`\e`/`\b`. Found while fixing D107 (a confound in a test's own comparison string). Narrow, low real-world frequency (these four escapes are rare outside terminal-control code) but a genuine, separate bug in `src/lexer.cpp`'s string-literal escape switch.
10. **D101** (corner case, kept for completeness) and **D103** (corner case, kept for completeness) — see the 2026-09-09 reassessment above; `pidigits.pl`'s mini-gmp `extract_digit` divergence (pre-existing, documented, skipped in the harness).

The 2026-09-09/10 real-world module survey (see above) is a standing
reminder to re-derive this list from actual code rather than only mining
variations of already-known bugs — that's how D107, D109, and D110 were
found, and how D101/D103 got correctly demoted. Re-run `make test-tsan` /
`make test-valgrind` after each further correctness fix — `make
test-all` re-passed clean (263/263) after the D99/D105/D100/D107 fixes
and the three new builtins, but TSan/valgrind haven't been re-run since,
and D106 in particular (aliased pointers, tag promotion) is exactly the
shape of bug those tools are
positioned to catch collateral damage from once it's fixed.

### Top 10 missing features (ranked by likely impact)

1. **`while (...) { } continue { }` block** — hard parse error, core language feature independent of any module. Found via the 2026-09-09 real-world survey (`/usr/bin/dbilogstrip`).
2. **`q[...]`/`qq[...]`/`qw[...]` don't balance nested `[`/`]`** the way `qq{...}` already balances nested `{`/`}` — hard parse error. Found via the real-world survey (`/usr/bin/ptardiff`'s multi-line `q[...]`-delimited usage text).
3. **`qr/PATTERN/` is not implemented as a value type at all** — blocks storing/passing a compiled regex (e.g. `fileparse($path, qr/\.[^.]*/)`, found in `/usr/bin/ptardiff`). A prerequisite for full regex-as-value idioms generally, not just File::Basename's suffix argument.
4. **D104** — indented heredoc `<<~IDENT` (Perl 5.26+) — hard parse error, common modern-Perl idiom, lexer has zero support today.
5. **`\my $var` / `\my %var`** (reference to an inline lexical declaration, e.g. `GetOptions(\my %opt, ...)`) — parse error. Found in `/usr/bin/lwp-dump`, a real (if newer-style) Getopt::Long idiom.
6. **Typeglob `{IO}}`/`{FORMAT}` slots** — parser/codegen wiring only; the runtime half (`perl_glob_slot` etc.) already landed unused (see note above) — this is now a small, well-scoped piece of work, not a new subsystem.
7. **Full XS / DynaLoader** — current XS is an MVP FFI capped at ≤4 scalar args; real CPAN `.so`/`.xs` modules (most non-pure-Perl CPAN) don't load.
8. **Complex CPAN OO / advanced `our`** — parser still fails on some advanced module patterns; blocks `-pm`-installed dependencies with nontrivial internals. Confirmed again via the real-world survey (`Debian::Debhelper::Dh_Lib.pm` — a real, if Debian-specific, module — still fails to parse once its own missing-module error is worked around).
9. **DBI beyond the SQLite subset** — no MySQL/Postgres drivers, no full DBI method surface.
10. **`given`/`when` / smart-match `~~`**, `format`/`write`, runtime `eval`/`do FILE` needing `clang-18`+`perlc` on the target, `sprintf`/`printf`'s `%vd` vector flag, and POD being skipped/not introspectable — all previously-logged lower-impact items, unchanged by this pass.

Diamond `<>` / `<ARGV>`, `__DATA__`/`<DATA>`, `use utf8`, `unshift @{EXPR}`,
and `exists $h{a}{b}`: `diamond_{smoke,deep}.pl`, `data_section_{smoke,deep}.pl`,
`utf8_source_{smoke,deep}.pl`, `unshift_exists_{smoke,deep}.pl`.

## Real-world module survey (2026-09-09)

Before continuing down the synthetic-probe D-number list, compiled ~36
unmodified real Debian system Perl scripts (`dpkg-*`, `dh_*`, `lwp-*`)
plus common real-world module usage patterns, to check whether the
remaining open items (D101/D103 especially) were actually likely to
matter in real code, or whether something bigger was being missed.
Only 6 of 36 scripts even compiled — most failures were missing
Debian-internal modules (untestable here, not a perlc bug) or complex OO
in an inlined dependency (an already-documented gap) — but digging into
the failures surfaced genuinely high-impact, previously-unknown gaps:

- **`Getopt::Long` was completely unimplemented** — `GetOptions(...)`
  silently resolved to nothing and returned false, so every script using
  it (probably *the* most common way real Perl CLI tools parse `@ARGV`;
  found broken in `/usr/bin/debconf-escape` on the test system) fell
  straight into its own error-handling path no matter what flags were
  passed. **FIXED** — see below.
- **`Data::Dumper` was completely unimplemented** — `Dumper(...)`
  silently produced no output; one of the most common debugging tools in
  real Perl code. **FIXED** — see below.
- **`File::Basename` was completely unimplemented** — `basename()`/
  `dirname()` silently returned empty strings instead of erroring;
  common path-manipulation utility. **FIXED** — see below.
- **`while (...) { } continue { }` is a hard parse error.** Core
  language feature, unrelated to any module. Not fixed — logged as a
  missing-feature item (see priority list below).
- **`q[...]` (and presumably `qq[...]`/`qw[...]`) don't balance nested
  `[`/`]`** the way `qq{...}` already balances nested `{`/`}`. Hard
  parse error; bit a real script's multi-line `q[...]`-delimited usage
  text. Not fixed — logged as a missing-feature item.
- **D107 — FIXED 2026-09-10.** `s/\\/\\\\/g; s/\n/\\n/g;` on a string
  with no literal backslashes doubled the inserted backslash (`perl`:
  `hello world\n`, `perlc`: `hello world\\n`). Root cause: the
  replacement text of `s/PATTERN/REPLACEMENT/` is parsed like a
  double-quoted string in real Perl (backslash escapes processed, plus
  `$1`../`$&` capture interpolation) — but `src/runtime.c`
  `perl_regex_subst`'s replacement-expansion loop only ever handled
  `$0`-`$9`/`$&` and copied every other character, *including backslash
  sequences*, straight through verbatim (confirmed via the lexer: `s///`
  captures pattern and replacement 100% raw, with zero escape
  processing — `readSubst` in `src/lexer.cpp` explicitly preserves
  `\X` pairs unexamined, so nothing upstream does this work either).
  Not just `\\` — `\t`/`\n`/etc. inside a replacement also stayed as
  literal 2-char backslash+letter instead of becoming the actual
  escape character (`s/X/\t/` produced 2 literal chars, not a tab).
  **Fix:** the expansion loop now recognizes `\\ \n \t \r \f \b \a \e
  \0 \$ \@` before falling back to "unknown escape: drop the backslash,
  keep the character" for anything else, matching Perl's own fallback
  for an unrecognized double-quote escape. `/e` replacements
  (`perl_regex_subst_e`, a separate function that evals the replacement
  as code) are unaffected. Verified against the real
  `/usr/bin/debconf-escape` script (both `-e` and `-u` modes) and a
  battery of escape/capture/global-flag combinations. `make test-all`
  re-passed clean (263/263, no regression) plus the 2 new tests.
  Tests: `tests/d107_subst_replace_escapes_{smoke,deep}.pl`.

  **Found via this work, not fixed — now logged as D109 and D108:**
  - **D109** (new): arbitrary variable interpolation (`$name`, `@arr`)
    in `s///` replacement text doesn't work at all — only `$0`-`$9`/`$&`
    (capture refs) are recognized. Fixing this needs codegen-level
    string-interpolation support for the replacement text (the same
    machinery regular `"..."` string interpolation already has), not a
    runtime escape-table fix — a materially bigger change than D107
    itself.
  - **D108** (new): plain double-quoted string literals (ordinary
    `"..."`, unrelated to `s///`) don't recognize `\f`/`\a`/`\e`/`\b` —
    confirmed via `perl -e 'print length("\r\f\a\e")'` → `4` vs perlc's
    `7` (the unrecognized escapes pass through as literal
    backslash+letter pairs). `src/lexer.cpp`'s double-quoted-string
    escape `switch` (~line 134) only has cases for `n t r 0 x \ ' " $
    @`. A different code path than D107 (lexer string-literal parsing,
    not runtime `s///` replacement expansion) — narrow, low real-world
    frequency (these four escapes are rare outside terminal-control
    code), but a genuine, separate bug.
- `\my %opt` (reference to an inline lexical declaration, e.g.
  `GetOptions(\my %opt, ...)`, found in `/usr/bin/lwp-dump`) is a parse
  error. Not fixed — a parser gap independent of Getopt::Long itself.
- `qr/PATTERN/` is not implemented as a value type at all (confirmed via
  grep — no lexer/parser/codegen/runtime support). This blocks the
  `fileparse($path, qr/\.[^.]*/)` idiom (found in `/usr/bin/ptardiff`)
  and anything else that stores/passes a compiled regex as a value. Not
  fixed — a separate, larger missing feature; `File::Basename`'s
  `basename`/`fileparse` suffix arguments only support plain strings as
  a result (documented in `perl_basename`'s comment).

### Getopt::Long — FIXED 2026-09-09

`src/runtime.c` `perl_getopt_long()` (dispatched from `src/codegen.cpp`'s
`"GetOptions"`/`"Getopt::Long::GetOptions"` case, which builds a plain
array of the raw call arguments — spec strings and ref targets, evaluated
normally so `\$x`/`\@x`/`\%x` already produce real REF_SCALAR/REF_ARRAY/
REF_HASH values — plus the live `@ARGV` array). Supports both calling
conventions (`GetOptions(spec=>ref,...)` and `GetOptions(\%opt,
spec,...)`), boolean flags, alternate names (`"foo|f"`), negation
(`"foo!"`/`--no-foo`), increment (`"foo+"`), typed values (`=s`/`=i`/
`=f`), array-collecting (`=s@`) and hash-collecting (`=s%`) options, long
(`--foo`, `--foo=val`, `--foo val`) and short (`-f`, `-f val`) forms,
`--` end-of-options, and unknown-option failure with the real
`"Unknown option: name"` stderr message (lowercased, matching real
Getopt::Long's own — slightly surprising — behavior). Mutates `@ARGV` in
place to remove recognized options, matching real Getopt::Long's
contract.

Deliberately NOT supported (documented simplifications, none seen in the
real-world scripts this was built against): bundled short options
(`-abc`), glued short values (`-fVALUE` — verified real default-config
Getopt::Long rejects this too, as "Unknown option: fvalue"), optional-
value (`:s`, treated as required `=s`), `pass_through`/`gnu_getopt`
config. Tests: `tests/getopt_long_{smoke,deep}.pl`.

### Data::Dumper — FIXED 2026-09-09

`src/runtime.c` `perl_dumper()`. Reproduces the default `Indent=2` style
byte-for-byte: nested structures are indented to (column of their
opening bracket) + 2, and the closing bracket goes back at that same
column — which depends on the actual rendered width of everything
before it on the line (key length, `"bless( "` prefix, etc.), not a
fixed per-depth indent. Supports hash/array refs (arbitrarily nested),
scalar refs, blessed objects (`bless( {...}, 'Class' )`), `undef`, and
the IV-unquoted / NV-and-string-quoted distinction real Dumper makes
(`Dumper(42)` → `42`, `Dumper(3.5)` → `'3.5'`).
`$Data::Dumper::Sortkeys` is honored — but only within the same lexical
scope it was set in, because codegen resolves it via a plain in-scope
variable lookup rather than a true cross-package global read (see next
paragraph). Tests: `tests/data_dumper_{smoke,deep}.pl`.

**Found via this work, not fixed — now logged as D110:** `$Package::var`
(an arbitrary fully-qualified global not declared via `our`) is not a
true cross-scope global in perlc — it auto-vivifies as a plain variable
in whatever scope first references it, so `$Data::Dumper::Sortkeys = 1;`
at file scope is invisible from inside an unrelated `sub`. This is a
real, separate, pre-existing correctness gap in general `$Pkg::var`
handling (not specific to Dumper) — logged as a missing/incorrect-feature
item on the priority list below rather than fixed here, since properly
fixing it means giving arbitrary qualified names real global storage,
which is a larger change than "implement Dumper."

Also (not a Dumper bug): an all-numeric array/hash-value literal (e.g.
`[1,2,3]`) that takes the Stage 22/23 FLAT_ARRAY/FLOAT_PAIR fast path
(see D105/D106 above) loses the IV/NV distinction in that optimization's
raw `double[]` storage, so Dumper prints its elements quoted
(`'1','2','3'`) instead of bare (`1,2,3`) — a pre-existing
representational limitation of that fast path, confirmed independent of
the Dumper implementation itself (mixed-type arrays, which don't take
the fast path, dump correctly).

### File::Basename — FIXED 2026-09-09

`src/runtime.c` `perl_basename`/`perl_dirname`/`perl_fileparse`. Matches
real File::Basename's behavior: trailing slashes stripped before
splitting, `dirname("/")` is `"/"`, `dirname` of a slash-less path is
`"."`, `fileparse`'s dir part always ends in `"/"` (or is `"./"` when the
path has no directory component). `fileparse` has both a list-context
case (`emitArrayPtr`, returns the full `(name, path, suffix)` triple) and
a scalar-context case (`emitExpr`, returns just the name) — mirroring how
`localtime`/`stat` already handle the two contexts differently. Suffix
arguments to `basename`/`fileparse` only support plain strings, not a
`qr//` regex (see the `qr//` gap above) — a non-string suffix argument is
silently skipped rather than crashing. Tests:
`tests/file_basename_{smoke,deep}.pl`.

## 2026-09-10 multi-agent MVP review

Five-agent review (architect/code-reviewer, three engineers on
runtime/codegen/parser depth, one PM on docs) tasked with assessing
real MVP readiness for "common real-world Perl scripts and commonly-used
CPAN modules compile and run correctly." The architect agent
deliberately wrote fresh probe scripts (not derived from TESTS.md or
the existing test corpus) and found **seven previously-unlogged
defects**, all independently re-verified byte-for-byte against real
Perl 5.42 in this session before being logged here. Full assessment and
forward plan: see `MVP_ROADMAP.md`.

### D111 — `my %c = %h;` (hash-to-hash copy) produces wrong contents — **FIXED 2026-09-10**

```perl
my %h = (a=>1, b=>2);
my %c = %h;
# perl:  a=1 b=2 count=2
# perlc: 2=   count=1
```

Root cause (`src/codegen.cpp:4133-4139`, `case NK::My`'s `isHash`
branch): when the RHS is a `%hash`-shaped node, `emitArrayPtr(*n.right)`
doesn't know how to flatten a hash into a key/value list and returns
null. The fallback path then does `emitExpr(*n.right)` — the RHS's
**scalar-context** value, i.e. `%h`'s key *count* — and pushes that
single count as the only element of a 1-item array, which
`perl_hash_from_list` then (correctly, given its wrong input) treats as
one odd key with an undef value. Matches the observed `2=` / `count=1`
output exactly.

This directly contradicts D99's write-up above, which claimed hash
declarations didn't have the analogous aliasing bug — true, but it
missed this separate, worse bug (wrong contents, not aliasing) because
the original probe only checked for aliasing symptoms.

**Real-world impact: high.** `bless { %args }, $class` and any
options-merge pattern (`my %opts = (%defaults, %overrides)`) are
extremely common CPAN/OO idioms and are silently broken by this.

**Fix** (`src/codegen.cpp`): added `NK::HashVar`/`NK::DerefHash` cases to
`emitArrayPtr` that flatten via `perl_array_extend_hash` (the same
runtime primitive `flattenArgInto` already used for `foo(%h)` sub-call
argument flattening) instead of returning null and falling through to
scalar-context `emitExpr`. This one change fixed every list-context
consumer that calls `emitArrayPtr` generically — `my %c = %h`, `my @flat
= %h`, `(%defaults, %overrides)` merges, and `%$href` deref-flatten all
share the same code path. A second, separate site had the identical bug:
`NK::AnonHash` (`{ %args, k=>v }` — the `bless { %args }, $class` idiom)
has its own flat per-element loop that didn't call `emitArrayPtr` at
all; fixed the same way. Tests: `tests/d111_hash_flatten_{smoke,deep}.pl`.

Found via this work, not fixed (logged separately): **D119** —
`scalar(keys %$href)` returns `0` instead of the key count (list-context
`keys %$href` is fine). See below.

### D112 — module file-lexical `my` collides with the main script's — **FIXED 2026-09-10**

```perl
# Leaky.pm
package Leaky;
my $counter = 42;
sub get { return $counter; }
1;
```
```perl
# main script
use Leaky;
my $counter = 7;
print Leaky::get(), "\n";   # perl: 42   |   perlc: 7
```

Root cause: `inlineModules()` (`src/main.cpp:210-570`) lexes each `.pm`
and **splices its tokens directly into the main token stream** — there
is no per-file lexical scope boundary, no per-package stash, no real
`@INC`/`require` semantics. A module's file-scope `my` variable and the
main script's same-named `my` variable become the same variable.

**Real-world impact: high, architectural.** File-lexical state (caches,
counters, closure-held config) is one of the most common CPAN
encapsulation patterns; this silently breaks it whenever a name
collides, which is common for generic names like `$counter`, `$cache`,
`@queue`.

**First attempt rejected:** a token-level `{ ... }` wrap around each
inlined module (as the "fix shape" above originally proposed) was tried
first and made things *worse* — it correctly isolated the module's `my
$counter` into its own lexical scope, but named subs in this codebase
(`sub get {}`) don't have real closure capture over enclosing blocks;
they resolve free variables purely by name through `fileScalarGlobals_`
(a flat, package-blind map). Once wrapped, `Leaky::get()` could no
longer see the module's own (now block-local) `$counter` at all, and
silently fell through to whatever *other* same-named global happened to
be registered — main's `$counter`, reproducing the exact same wrong
answer (7) via a different path. Reverted.

**Actual fix** (`src/codegen.cpp`): keep the flat token-splicing model,
but make the global-variable *storage key* package-qualified instead of
a bare name. `case NK::My`'s `asGlobal` branches (scalar/array/hash all
three had the identical bug) now key primarily on
`currentPackage_+"::"+name` (e.g. `"Leaky::counter"` vs `"main::counter"`)
— previously two different packages' same-named file-scope `my`/`our`
declarations shared one storage slot, with whichever declaration ran
last silently overwriting the value the other one's readers would see.
`lookupVar`/`lookupArray`/`lookupHash` now try the *reading* sub's own
package-qualified key first, falling back to the bare name only as a
last resort (preserving existing cross-package/sloppy-code lookups like
`$Data::Dumper::Sortkeys`). This is the same underlying mechanism D110
needs, applied narrowly to the file-scope-`my`/`our` global-creation
path rather than D110's broader "any undeclared `$Pkg::var`" case — D110
remains open. Tests: `tests/d112_module_scope_{smoke,deep}.pl` (module
+ `tests/lib/D112Leaky.pm`), which also regression-covers same-package
`our` cross-access to confirm the fix didn't disturb it.

### D113 — no failure signal: silent no-op instead of a hard error — **FIXED 2026-09-10**

```perl
my $r = nosuchsub(1,2,3);
print defined($r) ? $r : "undef", "\n";
# perl:  Undefined subroutine &main::nosuchsub called ... (exit 255)
# perlc: undef (exit 0)
```

Confirmed: calling an undefined subroutine silently returns `undef`
instead of dying (method calls on objects *do* die correctly — this is
scoped to bareword/sub-call dispatch). Separately, an unresolvable
`use Some::Module;` is silently dropped rather than erroring, and
`use lib "..."` / `-I` / `PERL5LIB` are not honored (module search path
is a hardcoded list of relative directories).

**Real-world impact: this is a meta-bug that hides all the others.**
It's exactly how three completely-unimplemented modules
(Getopt::Long/Data::Dumper/File::Basename) went unnoticed until a human
manually diffed output — a missing builtin or unresolvable `use`
produces no error, just silently-wrong runtime behavior. The architect
agent's own initial survey pass falsely reported "40/40 modules
resolved" before this bug was accounted for, because the modules were
never actually found and `use` was silently dropped.

**Fix:** (1) new `perl_call_named_sub_checked` (`src/runtime.c`) dies
with Perl's own message format (`Undefined subroutine &Pkg::name called
at FILE line N.`, exit 255) when the runtime sub table has no entry for
the call target; wired in at the two `emitCall` fallback sites
(`src/codegen.cpp`) that dispatch a genuine bareword/dynamic sub call —
deliberately *not* at method-dispatch or operator-overload-probing call
sites, which legitimately fall back (AUTOLOAD, `""`-overload probing)
and keep using the original unchecked `perl_call_named_sub`. (2)
`inlineModules` (`src/main.cpp`) now throws `Can't locate Foo/Bar.pm in
@INC (searched: ...)` when a `"::"`-qualified `use Module;` resolves to
neither a known perlc built-in nor an actual `.pm` file anywhere in the
search path — scoped to qualified names only, so one-word pragmas this
pass doesn't fully model (`integer`, `utf8`, ...) stay silently
accepted rather than risking false positives. Also expanded the
built-in-module allowlist to include everything codegen actually
special-cases (`File::Basename`, `Getopt::Long`, `DBI`, `threads`,
`threads::shared`, etc.) — none of these have a `.pm` file on disk, so
without the allowlist the new error would have broken them. (3) `-I`,
`PERL5LIB` (colon-separated, honored at startup), and `use lib "path";`
(now actually prepends to the module search path instead of being
silently dropped like any other unresolvable `use`) are all wired into
the same search-path list used by both the `use` and `require` module
loaders. Tests: `tests/d113_undefined_sub_die_{smoke,deep}.pl` (the
deep test also verifies the *qualified* form — `&Other::name`, not
`&main::name` — when the undefined call happens inside a non-main
package). The `use`-not-found half is a compile-time error, not a
runtime output difference, so it isn't harness-testable the same way;
verified directly against real Perl's `Can't locate ... in @INC` message
shape during the fix.

### D114 — array slices with a non-literal subscript list — **FIXED 2026-09-10**

```perl
my @x = (10,20,30,40,50);
my @s = @x[1..2];     # perl: 20 30   |  perlc: 30
my @s2 = @x[@i];       # (@i = (0,2)) perl: 10 30  |  perlc: 30
```

Root cause: `NK::ArraySlice` codegen (`src/codegen.cpp:1550` and
`:8123`) loops `n.args` and calls `emitIdx()` on each entry, treating
every entry as a single scalar index — it has no handling for an entry
that is itself list-shaped (a `Range`, or an array variable). The
sibling `NK::HashSlice` handling directly below it in both locations
**already has** the correct three-way dispatch (literal list /
`emitArrayPtr` for a dynamic list / scalar fallback) — `@h{@k}` and
`@h{1..2}`-style keys work correctly today. `ArraySlice` was never
ported to the same dispatch pattern HashSlice already uses.

**Real-world impact: high.** `@x[1..2]`/`@x[@indices]` range/dynamic
array slicing is common in any list-processing code (windowing,
pagination, batch splitting).

**Fix** (`src/codegen.cpp` + `src/runtime.c`): added a new runtime
helper `perl_array_slice(a, idxs)` (array analog of the existing
`perl_hash_slice`), and ported `HashSlice`'s three-way dispatch
(`ArrayLit` literal / `emitArrayPtr` dynamic-list / scalar fallback) to
`ArraySlice` at both codegen sites — the list-context one in
`emitArrayPtr` and the scalar/ref-context one in `emitExpr`. Tests:
`tests/d114_array_slice_{smoke,deep}.pl` — the deep test covers ranges,
array-variable index lists, negative indices, a mixed literal+range
slice, and the `@{$ref}[...]` deref-array form (a separate resolution
branch from the plain-`@arr` case, same dispatch fix).

### D115 — bare `return;` yields a 1-element list in list context — **FIXED 2026-09-10**

```perl
sub f { return; }
my @r = f();
print scalar(@r), "\n";   # perl: 0   |  was: 1 (perlc, pre-fix)
```

Broke the common "return empty list to signal failure/no-result"
contract, e.g. `if (my @r = f()) { ... }` or `my %h = (k => f())`
(where `f()` returning nothing should leave the hash key absent, not
map it to `undef`).

**Fix**: a value-less `return` in list-call context now produces a
genuinely empty list (via `perl_array_to_list_return` on a freshly
allocated empty array, selected at runtime by `wantarray` context, the
same pattern `grep`/`map`/`sort`'s list-vs-scalar return already used)
instead of defaulting to a 1-element `(undef)` list. Two things had to
change together, both found the hard way:
1. `case NK::Return` (`src/codegen.cpp`, statement-level) got the fix
   directly.
2. `emitBlockLast` has its **own separate, duplicate** copy of the same
   return-value logic, used specifically when a `return` is the *last
   statement of a sub body* (the overwhelmingly common shape for a
   trivial `sub f { return; }`) — fixing only (1) left this exact,
   most-common case still broken, since it never goes through
   `case NK::Return` at all. Needed the identical fix applied there
   too.
3. `hasWantarrayOrUserCall()` (the static analysis that decides whether
   a sub needs the caller's wantarray context pushed at all, as a perf
   optimization for subs that never query it) didn't previously
   consider a bare `return;` as something that reads that context —
   only `return LIST_EXPR` and explicit `wantarray()` calls did. Since
   the fix now makes bare `return;` read `perl_current_wantarray_ctx()`
   at runtime, a sub whose only return is bare would have
   `currentSubNeedsWantarray_` staying false, meaning the caller never
   pushes a real context and the runtime read sees stale/wrong state.
   Added `NK::Return && !n.left` to that analysis.

Verified against real Perl for: a sub whose sole statement is a bare
return, a bare return reached via a conditional (not the literal last
statement), scalar-context `f()` (still plain undef, unaffected),
nested propagation through a wrapper sub, and boolean/if-context list
assignment. Tests: `tests/d115_bare_return_list_{smoke,deep}.pl`.

Found while writing this fix's test, not fixed (unrelated pre-existing
gap): **D130** — `if (my @arr = EXPR)` (a single array variable
declared inline as an `if`/`while` condition, no parens around the
declaration) is a hard parse error.

### D130 — `if (my @arr = EXPR)` (single array/hash var, no parens) — **FIXED 2026-09-11**

```perl
sub f { return (1,2); }
if (my @r = f()) { print "true\n"; } else { print "false\n"; }
# perl:  true
# was:   Error: Parse error line 2: unexpected token 'my'
```

Found while writing D115's deep test. D100 already fixed
`while (my ($k,$v) = each %h)`-style parenthesized-list-form `my (...)
= EXPR` as a boolean condition — this was the *unparenthesized
single-array-variable* form (`my @arr = EXPR`, no `(...)` around the
declaration at all), which D100's fix didn't cover since it's a
structurally different AST shape (a plain `NK::My` array declaration,
not an `ArrayLit`-wrapped list). The expression-context `my`-as-
condition parsing only special-cased a single *scalar* (`if (my $x =
...)`) alongside D100's parenthesized-list shape — a bare array/hash
variable fell through to a hard parse error.

**Fix** (`src/parser.cpp`, alongside the existing scalar `my $x`
expression-context branch): added a sibling branch recognizing `my
@name` / `my %name` (`TK::ARRAY`/`TK::HASH` immediately after `my`)
directly in expression context, producing the same `NK::My` node shape
statement-context declarations already use, with an optional `=
EXPR` right-hand side. (`src/codegen.cpp`, expression-context `case
NK::My` in `emitExpr`): this node shape previously only handled a
scalar; added array/hash handling that calls `perl_array_len`/
`perl_hash_size` on the just-declared variable so the condition's
truthiness matches real Perl's list-assignment-count-in-boolean-
context semantics (an array/hash assigned an empty list is falsy; any
non-empty result is truthy).

Verified against real Perl for: a truthy array-returning sub, an
empty-list-returning sub (falsy), a hash-returning sub (truthy), and a
plain array-to-array copy condition. Tests:
`tests/d130_my_cond_{smoke,deep}.pl`.

### D131 — `our $var;` inside a nested block / repeated `our` declarations — **FIXED 2026-09-11**

```perl
{
    our $result;
    sub foo { local($_) = shift; $result = $_; }
    foo("hi");
    print "$result\n";
}
# perl:  hi
# was:   (nothing printed)
```

Found while writing D129's deep test — moving the exact same code from
true file scope into a nested bare `{ }` block broke it entirely (at
file scope, this identical code worked correctly). Two stacked bugs,
both in `src/codegen.cpp`'s `case NK::My`:

1. The scalar declaration branch computed `asGlobal` correctly
   (`atFileScope || isOur`, matching the sibling array/hash branches)
   but then gated its actual global-vs-local storage decision on
   `atFileScope` alone in three separate spots (the `:shared` scalar
   sub-branch, the `--do-lib` sub-branch, and the plain sub-branch) —
   so `our $x` inside a nested block (`isOur=true`, `atFileScope=
   false`) fell through to the local-alloca fast path instead of
   getting real global storage. A sub referencing that same name from
   outside the block then saw a completely disconnected variable.
   Fixed by changing all three conditions to `asGlobal`.

2. Fixing (1) surfaced a second, more general bug: **every** textual
   occurrence of a plain `our $x;` / `our $x :shared;` (not just the
   first) unconditionally minted a **brand-new** LLVM `GlobalVariable`
   instead of reusing the one already registered under this package-
   qualified name — LLVM silently auto-renames the symbol to dodge the
   clash, so `our $x = 5;` at file scope plus a *second* `our $x;`
   anywhere (a sub bringing the global into scope, or a second nested
   block) produced two disconnected storage locations. The array/hash
   `our` branches already looked up an existing global by qualified
   name (D112) but still unconditionally overwrote its value even on
   reuse with no initializer — silently resetting an already-populated
   `our @arr;`/`our %hash;` back to empty. Fixed by, on every branch
   (`plain scalar`, `:shared scalar`, array, hash): looking up an
   existing global by package-qualified name first; only creating +
   resetting-to-undef/empty when none exists; and when reusing, only
   overwriting the stored value if this occurrence actually supplies an
   initializer (`our $x = ...`) — a bare `our $x;`/`our @a;`/`our %h;`
   redeclaration now correctly leaves the existing value untouched,
   matching real Perl.

Verified against real Perl for: `our $scalar` set via a sub then read
via a second nested-block redeclaration; the same for `our @arr` and
`our %hash`; and a bare `our $counter;` redeclaration (no initializer)
across three separate `bump()` calls correctly preserving/accumulating
the running total instead of resetting each time. Tests:
`tests/d131_our_nested_block_{smoke,deep}.pl`.

### D116 — `__PACKAGE__`/`__FILE__`/`__LINE__` unimplemented — **FIXED 2026-09-10** (`__SUB__` split off as D124)

```perl
package Foo::Bar;
sub whoami { return __PACKAGE__; }
# perl:  Foo::Bar
# was:   Error: String found where operator expected
#        (Do you need to predeclare "__PACKAGE__"?)
```

Root cause: hard parse error, not silent-wrong-data — these tokens
weren't recognized anywhere in `lexer.cpp`/`parser.cpp`/`codegen.cpp`
(grepped, zero hits) before this fix. `bless {...}, __PACKAGE__` and
`ref($class) || $class || __PACKAGE__`-style constructor idioms are
among the single most common patterns in OO CPAN modules — this
probably blocked more real module files from parsing *at all* than any
other single missing-syntax item, including `qr//`.

**Fix** (`src/parser.cpp` `parsePrimary`, `src/codegen.cpp`
`emitCall`): `__PACKAGE__` resolves directly to a string literal at
parse time using the parser's already-tracked `currentPackage_`;
`__LINE__` similarly to an integer literal using the current token's
line. `__FILE__` needs the compiling source filename, which the parser
doesn't track (only codegen's `sourceFile_` does) — represented as an
ordinary `Call` node and intercepted at the very start of `emitCall`,
specifically so it resolves before falling through to D113's
"undefined sub" die path (which would otherwise treat it as a
genuinely unknown bareword).

Two context-sensitivity subtleties, both verified directly against
real Perl and both required a fix:
- `__PACKAGE__->method(...)` (`return __PACKAGE__->create(@_);` — a
  very common OO constructor idiom, e.g. delegating to a factory
  method) needs the *resolved* package name as the method-call
  invocant, not the literal string `"__PACKAGE__"` — so the dunder
  checks had to go before the parser's existing `ARROW` special-case
  (which otherwise stringifies any bareword immediately followed by
  `->` for bareword-class method calls).
- `__PACKAGE__ => 1` (fat-arrow auto-quote) and `$h{__PACKAGE__}`
  (bareword hash-subscript auto-quote) both auto-quote to the literal
  10-character string `"__PACKAGE__"` in real Perl, same as any other
  bareword in those two positions — so the dunder checks had to go
  *after* the existing `FATARROW` auto-quote check, and were also
  guarded by `!inKeyContext_` (the flag the parser already uses for
  bareword hash-subscript auto-quoting) so they don't fire inside a
  `{...}` subscript either.

Verified against real Perl for: `__PACKAGE__` inside a sub in a
non-main package, at top-level (`main`), `__FILE__` (checked for
non-empty and the right suffix, since the exact path string is
harness-argument-dependent), `__LINE__` incrementing correctly between
two calls on consecutive lines, the `__PACKAGE__->method()` constructor
idiom end-to-end (`bless { %args }, $class` included), and both
auto-quote contexts staying unaffected. Tests:
`tests/d116_dunder_consts_{smoke,deep}.pl`.

**Not fixed here** (split off, harder): `__SUB__` — now **D124**.

### D124 — `__SUB__` (current-sub reference) unimplemented

```perl
use feature 'current_sub';
my $fact = sub { my $n = shift; $n <= 1 ? 1 : $n * __SUB__->($n - 1) };
print $fact->(5), "\n";   # perl: 120  |  perlc: parse error
```

Split off from D116 since it's a fundamentally different kind of fix —
`__PACKAGE__`/`__FILE__`/`__LINE__` are all compile-time constant
substitutions (no codegen changes beyond the `__FILE__` interception),
but `__SUB__` needs a genuine reference to the currently-executing
closure, including whatever it captured, from *inside itself* —
`case NK::AnonSub`'s codegen already has a `Function *subFn` (the LLVM
function being emitted) and its `captureVals` in scope at the exact
point this would need to be threaded through (`currentFn_` is
generically available too, for a named sub), but simply wrapping
`currentFn_` in a fresh `perl_make_code_ref` the way `\&name` does
would silently drop the current closure's own captures if `__SUB__` is
used inside a closure that itself captured outer variables — a correct
fix needs the *same* captures array the enclosing closure was built
with, not a capture-less code ref. Mostly relevant to anonymous
recursion idioms; much less common than the other three dunders in
real-world code, so left open rather than risking a subtly-wrong
capture-sharing implementation under time pressure.

### D117 — `perl_atof_decimal` hand-rolled string→float parser — **FIXED 2026-09-10**

```perl
print "2.2250738585072014e-300"+0, "\n";
# perl:  2.2250738585072e-300
# was:   2.22507385850724e-300   (perlc, pre-fix)
```

Root cause (`src/runtime.c:1496-1532`, pre-fix): every implicit
string→number coercion (`"3.14" + 0`, numeric comparisons on strings)
went through a manual digit-accumulation parser (`result = result*10 +
digit`, fractional part via repeated `frac *= 0.1`, exponent applied
via a **repeated-multiply loop** instead of a single exact power)
instead of `strtod`. This accumulated more rounding error than `strtod`
produces — most visibly on extreme exponents (confirmed with a real
diverging case above), but the underlying inexactness applied to
ordinary decimal strings too — and was directly relevant to any future
JSON/CSV-style numeric-field parsing.

**Fix**: kept the existing decimal-only grammar scan (still rejects
hex/auto-`0x` and `1_000`-style underscore grouping — real Perl's
implicit string→number coercion doesn't recognize either, confirmed
directly, so the whole string can't just be handed to `strtod` blindly,
which *would* auto-detect hex floats) to find the valid numeric prefix,
then hands just that matched substring to `strtod` for the actual
conversion. Also now recognizes `Inf`/`Infinity`/`NaN` (any case,
optional sign) the way real Perl's string coercion does — found to be
completely unhandled (silently became `0`) while designing this fix,
not a pre-existing behavior worth preserving. Tests:
`tests/d117_atof_precision_{smoke,deep}.pl`.

Found while testing this fix, not fixed (split off — general parser
gap, unrelated to number parsing itself): **D125** — `use`/`no` pragma
statements only parse at file top-level, not inside a nested block or
sub.

### D125 — `use`/`no` pragma statements only parse at file top-level

```perl
sub foo {
    no warnings 'numeric';   # or: use strict;  — either one
    ...
}
# perl:  fine, scoped to the sub as expected
# perlc: Error: Parse error line 2: unexpected token 'warnings'
```

Found while writing D117's deep test — the natural way to scope-
suppress a deliberate "Argument ... isn't numeric" warning around a few
specific lines is `no warnings 'numeric';` inside a small block, and
that alone hit an unrelated hard parse error. Root cause:
`src/parser.cpp:73`'s `use`/`no` handling lives inside `parseProgram()`
(the file-level statement loop) — not in `parseStmt()`, which is what
every nested block or sub body actually calls to parse its own
statements. So `use`/`no` only works as the very first kind of
statement the whole file's top-level loop sees, never inside any
nested scope. Confirmed with both `no warnings '...';` and a plain
`use strict;` inside a `sub {}` — same failure, so this isn't
`warnings`-specific.

**Fix shape:** move (or duplicate) the `use`/`no` pragma-handling block
from `parseProgram()` into `parseStmt()` so it's reachable from any
nested scope, not just the top level. Likely small, but touches a
fairly large existing block (module-loading logic, `use constant`,
`use parent`/`base`, feature/version handling, etc. all live in that
same branch) — worth a careful pass rather than a rushed one given how
much is packed into that one `if`.

### D118 — `split` has no LIMIT argument and doesn't trim trailing empties — **FIXED 2026-09-10**

```perl
split(/,/, "a,b,,")     # perl: ("a","b")  — trailing empty fields dropped
split(/:/, $line, 2)    # perl: splits into exactly 2 pieces
# was: split(/:/, $line, 2) — Error: Parse error, expected ) but got ','
```

Root cause: neither `perl_split` (`src/runtime.c`) nor
`perl_split_regex` stripped trailing empty fields (Perl's default
`split` behavior), and — worse than the original write-up assumed — a
3rd LIMIT argument wasn't silently dropped, it was a **hard parse
error**: `src/parser.cpp`'s `split(...)` handling only ever consumed
two args (`sep`/pattern, `str`) with no comma-loop for a third.

**Fix**: parser now optionally consumes a 3rd LIMIT expression (stored
in `n->args[0]`), passed through both `NK::SplitFunc` codegen sites
(list-context in `emitArrayPtr`, scalar-context in `emitExpr`) as a new
`long long limit` parameter on both `perl_split` and
`perl_split_regex`. Both functions now bound the field count when
`limit > 0` (the last field absorbs the remainder unsplit, matching
real Perl exactly — verified for regex/string/whitespace/char-split
separators alike) and call a new shared
`perl_split_trim_trailing_empty()` helper when `limit == 0` (covers
both "omitted" and "explicit zero", which real Perl treats
identically) to strip trailing empty-string fields — `limit < 0` stays
unbounded with no trimming, also matching real Perl. Tests:
`tests/d118_split_limit_{smoke,deep}.pl`.

Found while testing this fix, not fixed (pre-existing, confirmed
unrelated to LIMIT/trim): **D126** — `split(/(PATTERN)/, ...)` with a
capturing group doesn't include the captured delimiter text in the
result the way real Perl does. See below.

### D126 — `split` with a capturing-group pattern doesn't include captured delimiters

```perl
split(/(,)/, "a,b,c")
# perl:  ("a", ",", "b", ",", "c")   — 5 elements, delimiters included
# perlc: ("a", "b", "c")             — 3 elements, delimiters dropped
```

Real Perl's `split` includes the text matched by any capturing groups
in the pattern as extra elements interleaved with the normal fields —
a documented, deliberate feature (used to keep the separators
themselves, e.g. `split /([,;])/, $csv_ish_line`). `perl_split_regex`
(`src/runtime.c`) always discards everything between `mstart` and
`mend` (the whole match) — it never inspects `pcre2_get_ovector_pointer`
past index 0/1 (the whole-match bounds) to see whether the pattern had
any capturing groups at all.

**Fix shape:** after computing `mstart`/`mend` for the whole match,
check `pcre2_get_ovector_count`/the compiled pattern's capture-group
count; for each captured group present in this match (`ov[2*i]`/
`ov[2*i+1]` for group `i`, skipping unset `PCRE2_UNSET` groups per
Perl's own "unmatched optional group → `undef`, not omitted" rule),
push its text as an additional array element between the two
surrounding fields. Confined to `perl_split_regex`'s regex path; the
plain-string/whitespace-separator path in `perl_split` has no
capturing-group concept at all, so it's unaffected by design.

### D127 — `scanExports()` doesn't strip a leading `&`/`*` sigil from an export name — **FIXED 2026-09-10**

```perl
# real Pod::Usage.pm (unmodified):
our @EXPORT = qw(&pod2usage);
```
```perl
use Pod::Usage;
pod2usage(1);
# perl:  works
# was:   Undefined subroutine &main::pod2usage called ...  (perlc, pre-fix)
```

Found via the 2026-09-10 real-module survey #2 (`pod2man`, `pod2text`,
`podchecker` — all real, unmodified system scripts). Real
`Pod::Usage.pm` declares its one export with an explicit leading `&`
sigil (an old-style "this is definitely a sub" marker, valid inside a
`qw()` export list) — `our @EXPORT = qw(&pod2usage);`. `scanExports()`'s
`extractQw()` (`src/main.cpp`) stored the token text verbatim, so the
registered export name was literally `"&pod2usage"`, not `"pod2usage"`.
This broke **both** forms:
- An explicit `use Pod::Usage qw(pod2usage);` failed D26's
  exported-name validation ("pod2usage" is not exported by the
  Pod::Usage module") because `"pod2usage" != "&pod2usage"`.
- A bare `use Pod::Usage;` (relying on the default `@EXPORT`) never
  populated `importMap["pod2usage"]` at all — only the nonsensical
  `importMap["&pod2usage"]` — so an unqualified `pod2usage(...)` call
  was simply undefined, hitting D113's die.

**Fix** (`src/main.cpp` `extractQw()`, used by both `scanExports()` and
the explicit-import-list extraction): new `stripExportSigil()` helper
strips a leading `&` or `*` from every word `extractQw()` returns,
applied uniformly across all three of its extraction paths (`qw(...)`,
parenthesized bareword/string list, and a single unparenthesised name)
so both export declarations and explicit import lists are covered the
same way. Verified against a local fixture reproducing Pod::Usage.pm's
exact declaration shape (`our @EXPORT = qw(&name); our @EXPORT_OK =
qw(&name2);`), for both the default-`@EXPORT` bare-`use` path and an
explicit `@EXPORT_OK` import — and directly against the real,
unmodified `Pod::Usage.pm` (pointing `-I` at a real Perl install): the
"not exported" false-rejection is gone, and compilation now progresses
past `Pod::Usage`'s own export declaration entirely (into a separate,
newly-found bug inside the module's own body — **D129**, logged below,
not fixed). Tests: `tests/d127_amp_export_{smoke,deep}.pl` +
`tests/lib/D127AmpExport.pm`, `tests/lib/D127AmpExportOk.pm`.

### D129 — `local($var) = EXPR;` (parenthesized single-var local) is a parse error — **FIXED 2026-09-10**

```perl
sub foo {
    local($_) = shift;
    print "$_\n";
}
foo("hi");
# perl:  hi
# was:   Error: Parse error line 2: expected $ but got '('  (perlc, pre-fix)
```

Found while re-verifying D127 against the real, unmodified
`Pod::Usage.pm` (line 41: `local($_) = shift;`, the exact idiom this
repro uses) — pointing `-I` at a real Perl install and compiling past
D127's now-fixed export declaration reached this as the next blocker.
`local $x = EXPR;` (no parens) already worked; it was specifically the
parenthesized, list-form `local(...)` — single- or multi-variable —
that failed to parse, since `local`'s statement parser only ever
expected a bare sigil-variable token immediately after the keyword,
never an opening paren.

**Fix** (`src/parser.cpp`): added a `local(VAR, VAR, ...) = EXPR`
branch that desugars exactly like `parseMy()`'s identical `my (...)`
list form just above it in the same file — a `FlatBlock` of
per-variable local-decl statements (`NK::LocalStmt`/`LocalArray`/
`LocalHash`, no assignment on each), followed by one `NK::Assign` whose
LHS is an `ArrayLit` of the now-localized variables. This reuses the
exact same, already-tested list-assignment codegen path `my (...)`
already uses (including D100's boolean-context fix), so **no codegen
changes were needed at all** — purely a parser-level fix. Handles the
single-variable case from the real-world repro and the general
multi-variable list form (`local($a, $b) = (...)`) uniformly, plus
`local(@arr)`/`local(%hash)` single-array/hash-variable parenthesized
forms. Verified against real Perl for all of these, including dynamic-
scope restoration after the enclosing sub returns. Tests:
`tests/d129_local_paren_{smoke,deep}.pl`.

Found while writing this fix's test, not fixed (unrelated pre-existing
gap): **D131** — `our $var;` declared inside a nested bare `{ }` block
doesn't work correctly (a sub referencing it from outside the block
sees nothing), even though identical code at true file scope works
fine.

### D128 — a parse error inside an inlined module reports the wrong line/file

```perl
# corelist (real, unmodified, 148-line-equivalent main script logic,
# but pulls in the large generated Module::CoreList.pm via `use`):
# perlc: Error: Parse error line 3: unexpected token 'warnings'
#        (the real `use warnings;` in the main script is at line 152;
#        line 3 is nowhere near any "warnings" token at all)
```

Found via the 2026-09-10 real-module survey #2. `corelist` and
`podchecker` (both real, unmodified system scripts) each produce a
parse error whose reported line number is implausibly small relative
to where the actual `use`/`no warnings`-adjacent tokens are — for
`podchecker` (148 lines in the main script), the reported line is 545,
which can only belong to an inlined module's own internal line
numbering (`Pod::Checker.pm`, in that case). Root cause: `inlineModules`
(`src/main.cpp`) splices each module's own token stream (lexed
independently, with its own 1-based line counter) directly into the
combined stream with no offset adjustment and no filename tag —
downstream parse errors report whatever line number happened to be
attached to the offending token, which is meaningless once multiple
files' token streams have been concatenated, and never say which file
is actually at fault.

**Impact:** not a correctness bug in generated code — a diagnostics/DX
problem — but a real one: it makes any parse failure that originates
inside a `use`d module (as opposed to the main script) very hard to
track down, since the reported location actively misleads rather than
just being silent. Low priority relative to correctness defects, but
worth fixing before CPAN-module support scales up further, since
inlined-module parse failures will only get more common as more real
modules are pulled in.

**Fix shape:** larger than a one-line tweak — `inlineModules` needs to
tag each spliced-in module's tokens with which file they came from
(and either keep each module's own line numbers distinct from the main
file's, e.g. via a `(file, line)` pair instead of a bare `int`, or
prefix error messages with the originating filename when a token from
an inlined module triggers a parse error). Touches the `Token`
representation and every place that currently assumes a bare
line-number int is enough to identify a source location.

### D119 — `scalar(keys %$href)` returns 0 instead of the key count — **FIXED 2026-09-10**

```perl
my %h = (a=>1, b=>2, c=>3);
print scalar(keys %h), "\n";       # perl: 3  |  perlc: 3  (correct)
my $href = \%h;
print scalar(keys %$href), "\n";   # perl: 3  |  was: 0 (perlc, pre-fix)
```

Found while writing D111's deep test (`{ %args, extra=>1 }` merge —
unrelated to the merge itself; a plain `\%h` reproduces it too). List-
context `keys %$href` (e.g. `my @k = keys %$href;`) was already
correct — only the scalar-context form was wrong.

Root cause (`src/codegen.cpp`, `case NK::KeysFunc` in `emitExpr` — the
scalar-context path): `Value *hv = lookupHash(n.name);` only ever
resolved a *named* hash variable (`n.name`); it had no handling at all
for the deref form (`n.left` set, `n.name` empty) the way
`emitArrayPtr`'s own `NK::KeysFunc` case (used for list context) already
does (`if (n.left) { ... perl_deref_hash ... }`). For `keys %$href`,
`n.name` is empty, so `lookupHash("")` returned null and the function
fell through to `return perlInt(0)`.

**Fix**: ported `emitArrayPtr`'s existing `n.left` deref-hash handling
into `emitExpr`'s scalar-context `KeysFunc` case. `NK::ValuesFunc`'s
scalar-context case right below it had the identical
`lookupHash(n.name)`-only gap (`values %$href` in scalar context, same
symptom) — fixed identically. Verified against real Perl for `%$href`
and `%{$href}` forms of both `keys` and `values`, boolean context
(`if (keys %$href)`), a deref through a sub-call return value
(`keys %{ get_href() }`), and confirmed named-hash and list-context
deref usage stayed correct (unaffected by this fix). Tests:
`tests/d119_keys_deref_scalar_{smoke,deep}.pl`.

### D109 — `s///` replacement text doesn't interpolate variables — **FIXED 2026-09-10**

```perl
my $name = "World";
my $s = "hello X";
$s =~ s/X/$name/;
print "$s\n";   # perl: hello World  |  was: hello X (perlc, pre-fix)
```

Root cause: the replacement text is captured 100% raw by the lexer
(`readSubst`/`readSection` do no escape processing) and handed to
`perl_regex_subst` (`src/runtime.c`), which only ever did a hand-rolled
raw-byte scan recognizing `$0`-`$9`/`$&` and (as of D107) backslash
escapes — it never went through codegen's expression/interpolation
machinery at all, so `$name`/`@arr` were left as literal text.

**Fix** (`src/codegen.cpp` `case NK::RegexSubst`, `src/parser.{h,cpp}`):
1. New `preprocessReplEscapes()` (`src/codegen.cpp`) applies the same
   backslash-escape table `readString` uses for interpolating strings
   (`\n \t \r \f \b \a \e \0 \\ \$ \@`, with `\$`/`\@` marked `\x02` so
   the interpolation scanner can tell an escaped-literal `$`/`@` apart
   from a real trigger) — needed because, unlike ordinary `"..."`
   literals, the replacement text never went through the lexer's own
   escape processing at all.
2. New `hasInterpTrigger()` scans the escape-processed text for a
   genuine named-variable trigger, deliberately treating `$0`-`$9` and
   `$&` as *not* triggers — those stay on the existing, already-tested
   fast path (`perl_regex_subst`) completely unchanged, so this fix
   only takes effect for replacement text that actually needs the new
   machinery (plain text and capture-ref-only replacements — the
   overwhelming majority — are byte-for-byte unaffected).
3. New `Parser::parseInterpString()` (`src/parser.h`/`.cpp`) is a small
   public static wrapper around the *existing*, already-correct
   `Parser::parseStringInterp` (the same interpolation scanner ordinary
   `"..."` literals use) so codegen can call it outside of a normal
   parse pass, seeded with the compiling package (for `$Pkg::var`
   resolution).
4. The `/e` flag's existing machinery (`case NK::RegexSubst`'s `hasE`
   branch) already built almost everything needed: a standalone closure
   function that captures whatever outer lexicals the replacement
   references and evaluates a `replExpr` once per match, with `$1`/`$&`
   installed by the runtime for that match. Refactored that machinery
   into a reusable lambda (`emitSubstWithReplExpr`) and now feed it
   `Parser::parseInterpString`'s output (an interpolation AST — a
   concat chain of literal/variable parts) when a named-variable trigger
   is present, instead of only ever `Parser::parseExprFromTokens`'s
   output (replacement-as-code, for `/e`). No new capture logic was
   needed: `collectAllScalarNames` already walks the concat-chain shape
   generically, and the existing capture step already captures every
   visible array/hash unconditionally (built for `/e`'s arbitrary code,
   which just as correctly covers `@arr`/`%h` interpolation).

Verified against real Perl for: scalar/array/array-element/hash-element
interpolation, `/g` with a named-variable replacement, an escaped `\$`
staying literal, a capture ref (`$1`) combined with a named variable in
one replacement, `$&` combined with a named variable, `$@` (eval error
var), `$$ref` scalar deref, and confirmation that `/e` and plain-text/
capture-ref-only replacements are completely unaffected. Tests:
`tests/d109_subst_interp_{smoke,deep}.pl`.

**Not fixed here** (split off as its own item since it's not `s///`-
specific): subscripted deref-in-a-string (`$$aref[0]`, `@{$r}[0,1]`) is
still wrong in the *general* interpolation engine (`parseStringInterp`
itself, used by plain `"..."` literals too) — now tracked as **D120**.

### D120 — subscripted deref-in-a-string doesn't interpolate correctly

```perl
my $aref = [10, 20, 30];
print "first: $$aref[0]\n";      # perl: first: 10   |  perlc: first: [0]
my $r = [1,2,3];
print "slice: @{$r}[0,1]\n";     # perl: slice: 1 2   |  perlc: slice: 1 2 3[0,1]
```

`src/parser.cpp`'s `parseStringInterp` (the raw-string interpolation
scanner used by all `"..."` literals, backtick strings, and now — since
D109 — `s///` replacement text) explicitly documents this exact gap in
its own `$$` handling comment: subscripted deref-in-a-string is "a
separate, deeper, pre-existing gap — not fixed here." Bare `$$word`
(scalar deref, no subscript) and `$$` (PID) both already work; it's
specifically `$$word[i]` / `$$word{k}` / `@{$expr}[...]` that don't.

**Fix shape:** larger than D109 was — `parseStringInterp` is a
hand-rolled character scanner reimplementing a subset of expression
parsing, not a thin wrapper over the tokenizer/parser; teaching it
`$$word[i]`-style postfix subscripting after a deref would mean
extending that hand-rolled scanner's grammar (or re-architecting it to
lex+parse a bounded token span instead of scanning raw characters by
hand) rather than a small dispatch fix like most of this pass's other
items.

### D121 — `$::name` (bare `main::` shorthand) is a hard parse error

```perl
$::ConfOpts{"x"} = 5;
print $::ConfOpts{"x"}, "\n";   # perl: 5
# perlc: Error: Parse error line 1: unexpected token ':'
```

Found via the real `/usr/bin/ucfq` script (Debian's `ucf` config-file
tool), which uses `$::ConfOpts` throughout. `@::arr`/`%::hash` have the
identical gap — this is a general lexer/parser gap for a bare `::`
package-qualifier prefix (shorthand for `main::`), not scalar-specific.
Common in older sysadmin-style Perl as an explicit "this is definitely
the main-package global" idiom.

**Fix shape:** in the sigil-variable lexing/parsing path, recognize a
`::` immediately following `$`/`@`/`%` as equivalent to `main::` before
the identifier, rather than only accepting an explicit package name (or
no `::` at all) before the bare name.

### D122 — `scanExports()` misses `use vars`-declared `@EXPORT_OK`

```perl
# File::Path.pm (real core module, unmodified):
use vars qw($VERSION @ISA @EXPORT @EXPORT_OK);
@EXPORT    = qw(mkpath rmtree);
@EXPORT_OK = qw(make_path remove_tree);
```
```perl
use File::Path qw(make_path);
# perl:  works
# perlc: Error: "make_path" is not exported by the File::Path module
```

Found by pointing `-I` at real Perl's actual core-module install
directory and trying to compile `/usr/bin/dpkg-name` against the real,
unmodified `File::Path.pm` (see the 2026-09-10 CPAN-module compile
survey below). Root cause: `scanExports()` (`src/main.cpp:189-`) only
matches the token pattern `our @EXPORT_OK = ...` — it requires the
assignment to be directly prefixed by `our`. Real `File::Path.pm`
(shipped, unmodified, with every Perl 5 install) instead uses the older
`use vars qw(...)` declaration followed by a separate, bare
`@EXPORT_OK = qw(...)` assignment with no `our` at all — a style still
common in core/older modules. `scanExports()` silently treats this as
"no `@EXPORT_OK` found," so any `use File::Path qw(make_path);`-style
explicit import is rejected as "not exported," even though it
genuinely is.

**Fix shape:** small, mechanical — in `scanExports()`, also match a
bare `@EXPORT`/`@EXPORT_OK = qw(...)` assignment (no `our` prefix)
as a fallback when a preceding `use vars qw(...)` (or, more generally,
any) declaration already put that name in scope. The simplest version:
drop the `TK::KW_OUR` requirement entirely and just match `@EXPORT[_OK]
= qw(...)` at file scope regardless of what declared the array.

### D121 — `$::name` (bare `main::` shorthand) is a hard parse error — **FIXED 2026-09-10**

```perl
$::x = 5;
print $::x, "\n";   # perl: 5   |   was: parse error (perlc, pre-fix)
```

**Fix** (`src/lexer.cpp`): after a `$`/`@`/`%` sigil, if the next two
characters are `::` followed by an identifier-start character, the
lexer now consumes them and synthesizes the exact same `IDENT` token
writing out `main::name` explicitly would produce — every downstream
consumer (parser, codegen) needed no changes at all, since a
`main::`-qualified name already worked correctly for scalars.

**Verified/found while fixing:** an `our`-declared `$::name` correctly
reaches the same storage as `$main::name`, including from inside a
subroutine (proving it's real global storage). An *undeclared*
`$::name`/`@::arr`/`%::hash` hits D110's separate, pre-existing gap
(same as the spelled-out `main::` form) — not something this fix
changes either way, but it's what led to widening D110's scope (see
above): array/hash access through an undeclared qualified name doesn't
just fail to cross scopes, it returns nothing at all even for
whole-array/hash access, not just elements. Tests:
`tests/d121_bare_maincolon_{smoke,deep}.pl` (the deep test uses `our`
declarations specifically to isolate the `::`-shorthand fix from D110's
separate gap).

### D122 — `scanExports()` misses `use vars`-declared `@EXPORT_OK` — **FIXED 2026-09-10**

**Fix** (`src/main.cpp` `scanExports()`): the `our` prefix requirement
was dropped — the function now matches a bare `@EXPORT[_OK] = qw(...)`
assignment regardless of whether it's preceded by `our` or not (this
function never tracked scope/declarations either way, so this is
consistent with its existing, already-blind-to-nesting matching
behavior, not a new risk). Verified against a local test fixture
(`tests/lib/D122UseVarsExport.pm`) built to mirror real core
`File::Path.pm`'s exact declaration style (`use Exporter (); use vars
qw(@ISA @EXPORT @EXPORT_OK); @ISA = qw(Exporter); @EXPORT = qw(...);
@EXPORT_OK = qw(...);`), and re-verified directly against the genuine,
unmodified `File::Path.pm` from a real Perl install (pointing `-I` at
it) — `dpkg-name.pl`'s `use File::Path qw(make_path);` now succeeds and
compilation progresses to the next (unrelated, Debian-internal) missing
module instead of falsely rejecting `make_path` as "not exported".
Tests: `tests/d122_scanexports_usevars_{smoke,deep}.pl` +
`tests/lib/D122UseVarsExport.pm`.

## 2026-09-10 CPAN-module compile survey (D113 follow-up)

Per `MVP_ROADMAP.md`'s critical path step 2 ("re-run the real-world
module survey against the D113-fixed binary"): compiled a batch of 10
real, unmodified Debian `/usr/bin` scripts chosen for using this
project's Tier 1/2 CPAN-module candidates (`File::Spec`, `Cwd`,
`File::Path`, `File::Find`, `JSON::PP`, `Const::Fast`,
`File::Find::Rule`) — `dh_movetousr`, `dpkg-name`, `spellintian`,
`json_pp`, `dh_compress`, `dh_md5sums`, `findrule`, `dh_installsystemd`,
`grog`, `ucfq`.

**D113 confirmed working as intended:** every script that couldn't
compile now fails with a clear, specific `Can't locate X.pm in @INC`
error instead of silently miscompiling — exactly the design goal.
Before D113, this survey's results would have been meaningless (see
D113's own write-up above: the *previous* survey pass reported false
"all resolved" results for exactly this reason).

**Missing modules confirmed** (matches predicted Tier 1/2 priority):
`File::Spec` (`grog`), `File::Spec::Functions` (`dh_compress`),
`File::Path` (`dpkg-name`), `File::Find` (`dh_movetousr`), `JSON::PP`
(`json_pp`), `Const::Fast` (`spellintian`, lower priority — a small
CPAN sugar module, not core), `File::Find::Rule` (`findrule`, lower
priority — CPAN not core), `Debian::Debhelper::Dh_Lib`
(`dh_installsystemd`/`dh_md5sums`/`dh_compress` — Debian-internal, not
general CPAN, already deprioritized in the original survey).

**New finding: `File::Find` should be added to the Tier 1 candidate
list** — not previously on it, but it's a core module and came up
immediately in this small sample (`dh_movetousr`).

**Follow-up experiment — pointing `-I` at real Perl's actual core-module
directory** (to see how far compilation gets once the "file not found"
blocker is removed, without implementing anything): surfaced two more
real, confirmed bugs, independent of any module being unimplemented —
**D121** (`ucfq`'s `$::ConfOpts` — bare `::` package-prefix shorthand
was a parse error) and **D122** (`dpkg-name` against real
`File::Path.pm` — `scanExports()` missed the `use vars`-style export
declaration real core `File::Path.pm` uses, wrongly rejecting a
genuinely-exported name). **Both fixed same-day** — see their write-ups
above. Also reconfirmed the previously-logged `qr//`-not-implemented
gap is a
real blocker for `File::Spec`/`grog` even once the file is found
("Expected /regex/ or s/// or tr/// after =~", from `File::Spec::Unix`'s
internal pattern matching) — `json_pp` hit a distinct, not-yet-
diagnosed parse error inside real `JSON::PP.pm` itself (`unexpected
token ';'`, line number not resolved — `JSON::PP.pm` is large and
complex; worth a dedicated follow-up pass rather than more probing here).

## 2026-09-10 CPAN-module compile survey #2

Second batch: 11 real, unmodified Debian `/usr/bin` scripts targeting
modules not covered by survey #1 — `Pod::Usage`, `Pod::Checker`,
`Encode`, `File::Copy`, `Storable`, `Module::CoreList`,
`LWP::Simple`/`LWP::UserAgent`, `WWW::Mechanize` — `pod2man`,
`pod2text`, `podchecker`, `encguess`, `corelist`, `GET`, `HEAD`,
`lwp-mirror`, `mech-dump`, `dh_installchangelogs`.

| Module / script | Status | Root cause | Blast radius | Priority |
|---|---|---|---|---|
| `Pod::Usage` (`pod2man`, `pod2text`, `podchecker`) | Failing | **D127** — `&pod2usage`-sigil export name breaks `scanExports()`'s match, for both explicit-import and bare-`use` forms | High — `pod2usage()` for `--help`/`--man` is ubiquitous in documented CLI scripts | **High** — small, mechanical |
| `Encode` `:fallbacks` tag (`encguess`) | Failing | `%EXPORT_TAGS` isn't recognized by `scanExports()` at all — confirms the already-logged `use Foo qw(:all)` missing-feature item with a concrete real-world hit | Medium — tag-style imports are common across many modules | Medium — bigger than D127, needs `%EXPORT_TAGS` parsing + tag expansion |
| `File::Copy` (10 real scripts use it) | Failing | Not implemented — `Can't locate File/Copy.pm` | High — `copy`/`move` is extremely common in install/build/cleanup scripts, same genre as `File::Path` | High — add to the Tier 1 CPAN candidate list in `MVP_ROADMAP.md` |
| `Storable::dclone` | Failing (loud, as designed) | Confirmed unimplemented; dies cleanly via D113 | Medium — matches existing Tier 2 roadmap item, no new information | Unchanged |
| `Module::CoreList` (`corelist`) | Failing | **D128** — parse error inside the inlined module reported at a main-script line number | Low direct (build-tooling-specific module); **D128 itself is general** | Low for the module; Medium for D128 |
| `Pod::Checker` (`podchecker`) | Failing | Same D128 symptom — reported line 545 in a 148-line main script | Low-Medium | Low (module-specific) |
| `LWP::Simple`/`LWP::UserAgent` (`GET`, `HEAD`, `lwp-mirror`) | Failing | Missing `URI::Heuristic` + the wider LWP stack | Low for this project's sysadmin/CLI profile | Low — consistent with the existing "deprioritize heavy/web-framework-shaped modules" stance |
| `WWW::Mechanize` (`mech-dump`) | Failing | Missing entirely (CPAN, not core) | Low | Low |
| `Debian::Debhelper::Dh_Lib` (`dh_installchangelogs`) | Failing | Already-known Debian-internal module | N/A | Skip (already deprioritized) |

`File::Copy` should be added to `MVP_ROADMAP.md`'s Tier 1 CPAN-module
candidate list (higher real-world hit count in this sample than
`File::Find` was when it was added).

## Source layout

| File | Role |
|------|------|
| `src/lexer.cpp` | Tokenizer |
| `src/parser.cpp` | Recursive descent |
| `src/codegen.cpp` | AST → LLVM IR |
| `src/runtime.c` | PerlValue + builtins |
| `src/mini-gmp.c` | Math::BigInt |
| `src/main.cpp` | Driver |
