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
| D138 | **FIXED 2026-09-19** | `\&name == \&name` / `__SUB__ == \&name` (CODE-ref identity via `==`) was unreliable — `perl_num_eq`/`perl_num_ne` compared the freshly-`malloc`'d `PerlClosure` wrapper's own address instead of the wrapped sub. See below. |
| D139 | **FIXED 2026-09-20** | `perl_num_eq`/`perl_num_ne` checked ref-identity BEFORE checking for a registered `<=>` overload — any blessed REF_ARRAY/REF_HASH/REF_SCALAR/CODE_REF class with an overloaded `<=>` (e.g. Time::Piece) had `==`/`!=` compare object pointers instead of dispatching. See below. |
| D140 | **FIXED 2026-09-20** | `perl_spaceship` (the `<=>` operator) and `sort { $a <=> $b }`'s fast-path comparator (`cmp_num_asc`, which the parser recognizes textually and routes around the general comparator machinery) never checked for a registered `<=>` overload at all — silently numified a blessed ref-shaped object as its pointer address. Math::BigInt was unaffected only because its own dedicated tag numifies correctly regardless. See below. |
| D141 | **FIXED 2026-09-20** | `emitBinOp`'s F64-fast-path BigInt guard (`emitF64BinOpWithBigIntGuard`, D132) only checked for the BigInt tag specifically, not blessed_class generically — a chained `+`/`-`/`*` on a blessed ref-shaped variable (`my $t2 = $t1 + 500; my $t3 = $t2 - 200;`) silently discarded the class on the second operation. See below. |
| D142 | **FIXED 2026-09-20** | `emitArrayPtr`'s `NK::LocaltimeFunc`/`GmtimeFunc` case ignored the `sval == "scalar_ctx"` marker the parser stamps for an explicit `scalar localtime(...)`/`scalar gmtime(...)` — `push @a, scalar gmtime(0)` (or the same shape inside `map`) wrongly flattened the 9-element list-context array instead of pushing the single scalar-context value. Pre-existing, independent of Time::Piece. See below. |
| D143 | **FIXED 2026-09-20** | The generic OO method-call dispatch (`case NK::MethodCall` in `emitExpr`, and `emitArrayPtr` — which had no case for `NK::MethodCall` at all) never pushed a wantarray frame around `perl_dispatch_method` — a method whose return value depends on context (e.g. Text::CSV's `fields`, DBI's `fetchrow_array`) inherited whatever was left on the runtime stack by an unrelated caller instead of the actual calling context; `join("|", $csv->fields)` and `my @f = $csv->fields;` both silently lost data. Pre-existing, independent of Text::CSV. See below. |
| D144 | **FIXED 2026-09-20** | An in-memory filehandle's backing scalar (`open my $fh, ">", \$out`) only synced on `close()` — `pmf_write`'s accumulated buffer was copied to the target scalar solely in `pmf_close`, so `print $fh "x"; print "[$out]";` with no intervening `close` showed `$out` still empty, unlike real Perl's synchronous-on-every-write `PerlIO::scalar`. Pre-existing, independent of Text::CSV (reproduces with a plain `print`, no module involved). See below. |
| D101 | **FIXED 2026-09-11** | `each %hash` in scalar context returned the pair length (0/1/2), not the key. See below. |
| D102 | **FIXED** (2026-09-10) | `die REF` / `die $blessed_obj` lost the reference — `$@` became a stringified `TYPE(0xaddr)` plus a wrongly-appended `" at FILE line N."`. Broke OO exception handling. See below. |
| D103 | **FIXED 2026-09-11** | Integer overflow used wrapping signed 64-bit arithmetic instead of Perl's IV→UV→NV promotion; values at/beyond the `2**63` boundary silently went wrong or printed in scientific notation instead of exact digits. See below. |
| D104 | **FIXED 2026-09-11** | Indented heredoc `<<~IDENT` (Perl 5.26+) was not recognized by the lexer at all — hard parse error, not silent-wrong-data. See below. |
| D106 | **FIXED 2026-09-11** | Same bug class as D105 but for a FLAT_ARRAY/FLOAT_PAIR ref read back out of an array/hash element (`$arr[0]`, `$h{k}`) rather than a plain scalar variable — a second alias made from that read didn't see further writes. See below. |
| D108 | **FIXED 2026-09-11** | Plain double-quoted string literals (`"..."`, unrelated to `s///`) didn't recognize `\f`/`\a`/`\e`/`\b` — they passed through as literal backslash+letter. See below. |
| D109 | **FIXED** (2026-09-10) | `s///` replacement text didn't support arbitrary variable interpolation (`$name`, `@arr`) — only `$0`-`$9`/`$&` (capture refs) worked, even though real Perl parses the replacement like a double-quoted string. See below. |
| D120 | **FIXED** (2026-09-13) | The *general* string-interpolation engine used by plain `"..."` literals (not just `s///`, which D109 separately fixes) was wrong for `$$aref[0]` (printed the whole-ref stringification, plus literal bracket text on slice shapes) and `@{$r}[0,1]` (whole-array join + literal `[0,1]`) — subscripted dereference inside a double-quoted string. Fixed: `parseStringInterp` now consumes subscript groups after deref forms via a new `Parser::parseSubscriptGroup` helper emitting the exact node shapes the token-level parser produces (D63 ArrowDeref for `$$name[i]`; ArraySlice/HashSlice-in-JoinFunc for `@{$r}[...]` and the `@{[ ... ]}` trap idiom), with bareword-quoted hash keys. Bare `$$word`/`$$` and every previously-working form stay byte-identical. Tests: `tests/d120_string_deref_interp_{smoke,deep}.pl`. See below. |
| D121 | **FIXED** (2026-09-10) | `$::name` / `@::arr` / `%::hash` (Perl's shorthand for `$main::name` — a bare `::` package prefix meaning "main") was a hard parse error ("unexpected token ':'"). Common in older/sysadmin-style Perl (found via the real `/usr/bin/ucfq` script). See below. |
| D122 | **FIXED** (2026-09-10) | `inlineModules`'s `scanExports()` (`src/main.cpp`) only recognized `our @EXPORT[_OK] = qw(...)` — it missed the (still common, used by core `File::Path`) older `use vars qw(@EXPORT_OK); @EXPORT_OK = qw(...)` style, where the assignment isn't prefixed with `our`. Caused a false "X is not exported by the Y module" compile error for a name that real Perl does export. See below. |
| D111 | **FIXED** (2026-09-10) | `my %c = %h;` (hash-to-hash copy) silently produced wrong contents — not a fresh copy. Corrected a wrong claim in D99's write-up below (`did NOT have this bug`). See below. |
| D112 | **FIXED** (2026-09-10) | `inlineModules()` (`src/main.cpp`) splices a `.pm`'s tokens directly into the main token stream with no lexical scope boundary — a module's file-scope `my $x` collided with the main script's `my $x` of the same name. See below. |
| D113 | **FIXED** (2026-09-10) | No "unimplemented" signal: calling an undefined sub silently returned `undef` (real Perl: fatal `Undefined subroutine ... called`, exit 255) and an unresolvable `use Some::Module;` was silently dropped instead of erroring; `use lib`/`-I`/`PERL5LIB` weren't honored. See below. |
| D114 | **FIXED** (2026-09-10) | Array slices `@x[LIST]` returned only one element when the subscript list was non-literal — a range (`@x[1..2]`) or an array variable (`@x[@i]`). Hash slices `@h{...}` already dispatched correctly for the equivalent cases; array slices weren't ported to the same dispatch. See below. |
| D115 | **FIXED** (2026-09-10) | Bare `return;` in list context yielded a 1-element list instead of Perl's empty list — broke `my %h = (k => f())`-style "return nothing on failure" patterns. See below. |
| D130 | **FIXED 2026-09-11** | `if (my @arr = EXPR)` — a single ARRAY/HASH variable declared inline as an `if`/`while` condition — was a hard parse error ("unexpected token 'my'"). See below. |
| D131 | **FIXED 2026-09-11** | `our $var;`/`our @arr;`/`our %hash;` declared inside a nested bare `{ }` block, or repeated as a bare redeclaration anywhere, didn't work correctly. See below. |
| D132 | **FIXED 2026-09-12** | `emitBinOp`'s separate F64 "stay unboxed" fast path converted a BigInt-tagged scalar VARIABLE straight to `double` and added/subbed/muled natively, bypassing `perl_add`/`perl_sub`/`perl_mul`'s D103 BigInt-aware logic. Fixed with a runtime BigInt-tag guard branching to the boxed op (plus a second pre-existing 1-ULP fix: mini-gmp's `mpz_get_d` truncates instead of round-to-nearest). See below. |
| D116 | **FIXED** (2026-09-10, `__PACKAGE__`/`__FILE__`/`__LINE__` only) | `__PACKAGE__` / `__FILE__` / `__LINE__` were not implemented at all (hard parse error) despite `bless {...}, __PACKAGE__` being one of the most common OO-Perl idioms in CPAN modules. `__SUB__` (reference to the currently-executing sub) is intentionally not covered — harder, split off as **D124**. See below. |
| D124 | **FIXED** (2026-09-13) | `__SUB__` (a reference to the currently-executing sub, needed for anonymous recursion — `use feature 'current_sub'`) was a hard parse error. Fixed: `perl_call_code_ref` now tracks the running closure's code-ref object in a thread-local and `perl_get_current_code_ref()` returns it (captures included), so `__SUB__` inside a closure is the real running closure, not a capture-less substitute; named subs resolve to their own `\&name`-shaped code ref; undef at file scope. Tests: `tests/d124_current_sub_{smoke,deep}.pl`. See below. |
| D117 | **FIXED** (2026-09-10) | `perl_atof_decimal` (`src/runtime.c`) was a hand-rolled decimal-string→float parser (manual digit accumulation plus a repeated-multiply exponent loop) instead of `strtod`, accumulating rounding error on ordinary decimal strings — every implicit string→number coercion goes through it. See below. |
| D125 | **FIXED 2026-09-12** | `use`/`no` pragma statements (`use strict;`, `no warnings 'numeric';`, etc.) are only recognized at the very top level of a file — nested inside a `sub {}` or a bare `{ }` block, they're a hard parse error ("unexpected token 'warnings'"/"'use'"). Root cause: the `use`/`no` handling (`src/parser.cpp:73`) lives in `parseProgram()`, not in the general `parseStmt()` every nested block/sub actually uses. See below. |
| D118 | **FIXED** (2026-09-10) | `split` had no 3rd LIMIT argument at all (hard parse error, not just silently ignored) and didn't trim trailing empty fields from the result, unlike real Perl's default `split` behavior. See below. |
| D126 | **FIXED 2026-09-12** | `split(/(,)/, $str)` — a split pattern with a capturing group — didn't include the captured delimiter text in the result the way real Perl does (`split(/(,)/, "a,b,c")` gives `("a", ",", "b", ",", "c")`, 5 elements). The rewrite also fixed a pre-existing hang/garbage on all-zero-width split patterns. See below. |
| D127 | **FIXED** (2026-09-10) | `scanExports()` stored an export name with a leading `&`/`*` sigil verbatim (e.g. real `Pod::Usage.pm`'s `our @EXPORT = qw(&pod2usage);`) instead of stripping it, so it never string-matched a plain `pod2usage` explicit import *or* an unqualified `pod2usage()` call after a bare `use Pod::Usage;`. High real-world impact — `pod2usage()` for `--help`/`--man` handling is one of the most common patterns in documented Perl CLI tools. See below. |
| D129 | **FIXED** (2026-09-10) | `local($var) = EXPR;` — a parenthesized, single-variable list-form `local` — was a hard parse error ("expected $ but got '('"). Found in real, unmodified `Pod::Usage.pm` (`local($_) = shift;`). See below. |
| D128 | **FIXED 2026-09-13** | A parse error occurring *inside* an inlined module (`use Some::Module;`) used to be reported with a line number belonging to the wrong file and no indication of which file the error is actually in. Tokens now carry their source-file tag; module errors report `Parse error in <module> line N:` with the module's own internal line; main-file errors keep the legacy `Parse error line N:` format byte-for-byte. See below. |
| D133 | **FIXED 2026-09-12** | Double-free in `case NK::Assign`'s int/float-var boxed fallback (`src/codegen.cpp`): `my $var = "x" + 0` inside a bare block freed the owned boxed RHS temp *and* returned it for the statement context to free again — silent allocator corruption, segfaulting when the variable was later read via `==`. Found while verifying D125's deep test (the pre-D125 snapshot binary reproduces it identically). See below. |
| D134 | **FIXED 2026-09-12** | `syscall()` arguments were pushed into the arg array with `perl_array_push`, which **clones** — a syscall that writes through a pointer argument (SYS_clock_gettime's `struct timespec` buffer) wrote into the clone, so the caller's `$buf` never changed (`make test`'s `xs_ffi.pl` clock assertions failed on this). Args are now pushed by reference (`perl_array_push_nc`) so kernel writes land in the caller's own buffer. See below. |
| D135 | **FIXED 2026-09-13** | Inside a sub (or a nested bare block), a variable initialized with an integer (`my $x = 0;`) was int-promoted to an unboxed i64 alloca; any later assignment of a fractional NV (`$x = 5.5;`, `$x = "5.5" + 0;`, `$x = g();` where g returns 5.5, even a plain `$x = "7.25";`) silently truncated to the int part. Fixed with a fixpoint "provably-int-only" scan: int-promotion is refused when the scope ever writes the name a non-int-shaped value. Pure-int counters keep their i64 fast path (hot-loop IR byte-identical). See below. |
| D119 | **FIXED** (2026-09-10) | `scalar(keys %$href)` (keys on a deref'd hashref, in scalar context) returned `0` instead of the key count — `scalar(keys %h)` on a plain named hash and list-context `keys %$href` were both correct, so this was specific to the scalar-context + deref-hash combination. See below. |
| D110 | **FIXED** (2026-09-13) | `$Package::var` (an arbitrary fully-qualified global not declared via `our`) is not a true cross-scope global — it auto-vivifies as a plain variable in whatever scope first references it, so setting it at file scope is invisible from inside an unrelated `sub`. Widened 2026-09-10: the same gap applies to `@Package::arr`/`%Package::hash`, and worse — whole-array/hash access (`my @c = @main::arr`) returned nothing at all. Fixed: any name containing `::` now routes to the runtime's process-wide typeglob registry (`perl_glob_get_{scalar,array,hash}`) at every read/write choke point in `src/codegen.cpp` (`emitExpr`/`emitLValue` ScalarVar, `lookupArray`, `lookupHash`, `NK::LocalStmt`), preferring a `fileScalarGlobals_`/`fileArrayGlobals_`/`fileHashGlobals_` exact-qualified entry when present so module-`our` storage unifies with qualified access (D112 keys). Qualified names are checked BEFORE `isGlobName` so the typeglob branch's bare-name fallback can't swallow them. Bare-name behavior untouched. Also fixed while testing: `$#Pkg::arr` was a hard parse error (lexer's `$#` branch didn't read `::` chains), `@::arr`/`%::h` in code and in interpolated strings stayed literal (the `::`-shorthand interp triggers were `$`-only in practice), and `"$$Pkg::x"`/`"$Pkg::h{k}"` dropped the package prefix or stayed literal in interpolated strings. Bare `$#arr` in interpolated strings was literal text even for bare names — now interpolates (note: a bare file-scope `@arr = (...)` assignment still doesn't register global storage, so `scalar(@arr)`/`$#arr` after one is 0/-1 — pre-existing, logged below). Tests: `tests/d110_qual_global_{smoke,deep}.pl`. See below. |
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

### D103 — integer overflow wraps instead of promoting — **FIXED 2026-09-11**

```perl
my $big = 9223372036854775807;  # IV_MAX
print $big + 1, "\n";            # perl: 9223372036854775808  |  was: -9223372036854775808 (wrapped)
print -9223372036854775808, "\n"; # perl: -9223372036854775808 (exact) | was: -9.22337203685478e+18
```
Real Perl's numeric model auto-promotes IV arithmetic that overflows
64-bit signed range into UV (if positive and within 64-bit unsigned
range) or NV (double), and its stringifier prints whole-valued NVs at
this magnitude using exact digits, not `%g`-style scientific notation.

**Threading a genuine UV type through the whole codegen/runtime type
system was judged too large and risky for what real-world code
confirms is a rare corner case** (see the "Real-world module survey"
reassessment above). Instead, this reuses the existing Math::BigInt
(mini-gmp) machinery already in the runtime — bounded and kept
distinct from a real, user-declared `Math::BigInt` object:

1. **`src/runtime.c` `perl_add`/`perl_sub`/`perl_mul`**: the existing
   D78 overflow check computed the native wrapped result and inferred
   overflow from its sign — a pattern that relies on signed-integer-
   overflow-being-undefined-behavior, which `-O2` can (and, verified
   empirically during this fix, *did* for some call sites but not
   others, depending on inlining) silently optimize away, defeating
   the check it implemented. Replaced with `__builtin_{add,sub,mul}
   _overflow`, which is well-defined at any optimization level. This
   alone was a pre-existing latent bug independent of D103's main fix
   (`perl_mul`'s old check also divided by a possibly-`INT64_MIN`/`-1`
   pair, itself separately UB).
2. On detected overflow, the exact result is computed via mini-gmp and,
   if it fits Perl's own UV range (non-negative, ≤ `UINT64_MAX` — the
   real boundary real Perl itself promotes across), returned as a new
   **unblessed** `PerlValue` (`tag=PERL_BIGINT`, `blessed_class=NULL`)
   — deliberately distinct from a real, user-declared (blessed)
   `Math::BigInt`, which keeps its existing unbounded-range behavior
   untouched (the blessed-overload-dispatch checks already at the top
   of each op fire first and return before any of this new logic
   runs). Beyond the UV range, falls back to `double` exactly as
   before. Chained arithmetic on an already-auto-promoted value
   re-derives exactly and re-checks the UV bound on *every* subsequent
   op — it does not stay "exact forever"; it demotes back to `double`
   the moment a chain of overflowing arithmetic would exceed
   `UINT64_MAX`, matching real Perl's own per-operation UV-vs-NV
   decision. Mixing an auto-promoted value with a real `Math::BigInt`
   naturally "graduates" it to full, unbounded range for free — the
   mpz-extraction helpers `Math::BigInt`'s own methods already use
   work on any `PERL_BIGINT`-tagged value regardless of blessing, so
   once a blessed operand is involved the existing overload dispatch
   takes over entirely.
3. **`src/codegen.cpp`**: the raw, unboxed i64 fast path (`emitExprI64`,
   used to skip boxing overhead for hot arithmetic) had zero overflow
   checking at all — this is the path the `$big + 1` repro actually
   went through, so the runtime fix alone wasn't suf'ficient. New
   `emitI64OverflowCheckedBinOp` wraps just the **outermost** operator
   of a top-level `+`/`-`/`*` with LLVM's overflow-checked intrinsics
   (`llvm.sadd/ssub/smul.with.overflow`), falling back to the boxed
   (now overflow-aware) runtime op only on the rare overflow branch —
   the common non-overflowing case emits the exact same instructions
   as before. Deliberately scoped to the outermost operator only (a
   nested arithmetic sub-expression's own overflow, if any, is
   unaffected) rather than redesigning `emitExprI64`'s recursive
   descent itself, which stays untouched.
4. **`src/parser.cpp`**: integer-literal parsing previously always fell
   straight to `double` for anything beyond `INT64_MAX` (D78) — losing
   exactness for the *entire* UV window, not just conveniently-round
   numbers (D103's own `-9223372036854775808` repro is this case: `2**63`
   parsed as a literal). A literal fitting `0..UINT64_MAX` now builds
   an unblessed auto-BigInt directly (represented as an ordinary `Call`
   node, `__auto_bigint_lit`, mirroring `__FILE__`'s existing pattern,
   intercepted in `emitCall`); beyond `UINT64_MAX` still falls to
   `double`, matching real Perl exactly.
5. **`perl_negate`** gained an unblessed-BigInt case: negating a
   literal like `-2**63` (a BigInt value of exactly `2**63`) now
   demotes back to a plain, exact `int` when the negated value fits
   signed 64-bit (as it does here — `-2**63 == IV_MIN`), instead of a
   lossy double.
6. `perl_to_int`/`perl_to_float`/`perl_is_true`/stringification
   (`perl_to_string`/`perl_to_string_dup`) all gained an unblessed-
   `PERL_BIGINT` case for interop; the numeric comparison operators
   (`perl_num_eq`/`lt`/`gt`/etc.) gained an exact mpz-based comparison
   path so comparing two overflowed values doesn't lose precision the
   way converting both to `double` first would (magnitudes in this
   range are well beyond a double's 53-bit mantissa).

**Found (not fixed) while verifying this — logged as D132**:
`emitBinOp`'s *separate* F64 "stay unboxed" fast path can convert a
BigInt-tagged **scalar variable** straight to `double` and add/sub/mul
natively, bypassing all of the above entirely (it never calls into
`perl_add`/etc.). This only ever surfaces as a 1-ULP divergence in the
narrow case of a value sitting exactly at the `UINT64_MAX` literal
boundary undergoing *further* arithmetic that itself crosses beyond
it — narrower still than D103's own already-narrow scope.

Verified against real Perl for: the write-up's exact `$big + 1` and
`-2**63` repros, chained overflow staying exact across multiple ops,
multiplication and subtraction overflow, negative-direction overflow
(no negative UV — correctly still falls to `double`, unchanged from
before this fix), ordinary non-overflowing arithmetic (unaffected),
mixing an auto-promoted value with a real declared `Math::BigInt`
(graduates to full range), and exact `==`/`<` comparisons against
overflowed values. Tests: `tests/d103_int_overflow_{smoke,deep}.pl`.

### D104 — indented heredoc `<<~IDENT` unsupported — **FIXED 2026-09-11**

```perl
sub f {
    my $x = <<~END;
        hello
        END
    return $x;
}
```
Root cause: `grep -n "<<~" src/lexer.cpp` found nothing — the `<<~`
indented-heredoc form (Perl 5.26+, strips the terminator line's own
leading whitespace from every body line) was never added; only
`<<IDENT`, `<<"IDENT"`, `<<'IDENT'` were recognized, so this was a hard
parse error (`unexpected token '<<'`), not silent-wrong-data.

**Fix** (`src/lexer.cpp` `readHeredoc`): checks for a leading `~`
immediately after the second `<` (before the existing quote/identifier
scan). When present, the terminator-line search matches with leading
whitespace stripped first, remembers that stripped prefix, and removes
it from every line of the collected body — mirroring real Perl's own
"strip the terminator's indentation from the whole body" rule. A body
line less indented than the terminator is a real-Perl fatal error;
this fix is permissive there instead, stripping only as much of the
prefix as a given line actually has (rather than erroring).

Verified against real Perl for: a basic multi-line body, the
interpolating (`<<~"IDENT"`) and non-interpolating (`<<~'IDENT'`)
quoted forms, and a body indented *more* than the terminator (leaving
correctly-computed residual indentation). Tests:
`tests/d104_indented_heredoc_{smoke,deep}.pl`.

### D106 — FLAT_ARRAY ref alias via array/hash element — **FIXED 2026-09-11**

```perl
my @arr = ([1,2,3]);
my $y = $arr[0];
$y->[0] = 99;
print "$arr[0][0]\n";   # perl: 99   |   was: 1 (stale)
```
(identical failure for a hash element, `$h{k}` in place of `$arr[0]`).

Same bug class as D105 (a FLAT_ARRAY/FLOAT_PAIR-tagged anon-array-ref
— Stage 22/23's compact storage for `[1,2]`-style literals — silently
forking into two independent arrays when aliased a second time),
narrowed to reading the ref back out of an **array or hash element**
specifically, rather than a plain scalar variable (which D105 already
covered). D105's fix already made `case NK::ScalarVar` in `emitExpr`
call `perl_promote_ref_array` before handing out its value; the
analogous `case NK::ArrayElem`/`case NK::HashElem` read paths did not.

This was deliberately left open when D105 shipped: the fix site sits
right next to the exact fast-path code (2D `ArrowDeref`-chain compound
assign, `llvm.assume(tag==FLAT_ARRAY)`) that caused a segfault
regression during D105's own fix, so it needed a careful read of every
neighboring fast path before touching it.

**Fix**: confirmed first that the `$arr[$i] op= rhs` / `$hash{key} op=
rhs` compound-assign fast paths, and the 2D `ArrowDeref`-chain fast
paths, are *separate* `case`/dispatch branches that call
`perl_array_get_ref`/`emitHashGetRef` directly themselves — they do
not route through the plain-read `case NK::ArrayElem`/`case
NK::HashElem` in `emitExpr` at all, so promoting there cannot reach
them. With that confirmed, both gained the identical
`perl_promote_ref_array` call D105 already added for `ScalarVar`.

Verified against real Perl for: array-element and hash-element alias
writes visible both directions (through the alias and through the
original element), plus an explicit regression check that the 2D
compound-assign and `ArrowDeref`-chain fast paths are unaffected.
Tests: `tests/d106_flat_ref_elem_alias_{smoke,deep}.pl`.

### D108 — plain string literals miss `\f`/`\a`/`\e`/`\b` — **FIXED 2026-09-11**

```perl
print "a\fb\ac\ed\be\n";
# perl: a<FF>b<BEL>c<ESC>d<BS>e   |   was: a\fb\ac\ed\be (literal backslash+letter)
```
Root cause: `src/lexer.cpp`'s double-quoted-string escape switch
(`readString`, used by plain `"..."` literals — unrelated to `s///`,
which D107 already fixed separately) only had cases for `n t r 0 x \
' " $ @`; `f`/`a`/`e`/`b` fell to the `default` branch, which passes
the backslash and letter through unchanged. Found while writing D107's
tests (a confound in a test's own comparison string). A second,
independent code path with the identical gap was also found while
fixing this: the `qq{...}` balanced-brace escape switch (a separate
manual scan, not shared with `readString`).

**Fix**: added `case 'f'`/`'b'`/`'a'`/`'e'` (mapping to `\f`, `\b`,
`\a`, `0x1b` respectively) to both switches.

Verified against real Perl for: each of the four escapes individually
(via `ord()`), a combined string exercising all four together, the
`qq{...}` form, and a regression check that the already-working
escapes (`\n`/`\t`/`\r`) are unaffected. Tests:
`tests/d108_string_escapes_{smoke,deep}.pl`.

### D132 — BigInt-tagged scalar var bypasses D103's overflow-aware ops — **FIXED 2026-09-12**

```perl
my $chain = 18446744073709551615;   # UINT64_MAX itself, fits UV exactly
$chain = $chain + 1;                 # now exceeds UV_MAX -> NV
print "$chain\n";
# perl:  1.84467440737096e+19
# perlc: 1.84467440737095e+19   (off by 1 ULP, pre-fix)
```

Found while verifying D103. Root cause: `emitBinOp` has a *separate*
F64 "stay unboxed" fast path (distinct from the i64 fast path D103's
`emitI64OverflowCheckedBinOp` intercepts) that, for a plain scalar
variable operand, converted it straight to `double` via `perl_to_float`
and added/subbed/muled natively — entirely bypassing
`perl_add`/`perl_sub`/`perl_mul`'s D103 BigInt-aware logic (confirmed
via `--emit-ir`: the add compiled to a direct `fadd double` on
`perl_to_float`-converted operands with no `perl_add` call). The
variable's tag isn't statically known, so the fast path's heuristic
silently truncated a BigInt-holding variable.

**Fix** (`src/codegen.cpp` new `emitF64BinOpWithBigIntGuard` +
`emitBinOp` wiring; `src/runtime.{c,h}` new `perl_is_bigint_pv`): the
F64 fast path for top-level `+`/`-`/`*` with at least one `ScalarVar`
operand now branches on a cheap, pure runtime tag predicate
(`perl_is_bigint_pv(PV*)`, marked read-only/NoUnwind/WillReturn for
GVN) on each variable operand — the PV* is loaded once before the
branch so it dominates both arms, following the branch-and-PHI pattern
D103's `emitI64OverflowCheckedBinOp` establishes. If EITHER operand is
BigInt-tagged, the boxed (D103-aware) `perl_add`/`perl_sub`/`perl_mul`
runs (with `freeIfOwned` on the operand temps); the non-BigInt branch
emits the *exact same* native F64 instructions as before, boxed via
`boxF64` so both arms yield a `PerlValue*` for emitBinOp's contract.
Literals are never BigInt-tagged at runtime (a D103 huge literal is
itself an emitCall producing a boxed PV*, which `emitExprF64`
rejects), so variable-free operands skip the wrapper entirely and
their code is unchanged. Verified with `--emit-ir`: for
`my $x = 1.5; my $y = $x + 2.25` the *fast* branch keeps the identical
`perl_to_float` + `fadd double` sequence, with only the one predictable
tag-compare/branch added; for a local unboxed float variable
(`lookupFloatVar` hit) the generated IR is byte-identical to the
pre-fix compiler, and `tests/nb.pl`'s hot loop IR is byte-identical
(no performance regression in the numeric kernels).

**Second, related pre-existing 1-ULP bug found and fixed while
verifying** (`src/runtime.c` `perl_to_float` + new
`perl_mpz_get_double`): mini-gmp's `mpz_get_d` *truncates* toward zero
instead of rounding to nearest (it masks off low limbs without
rounding), so ANY NV conversion of a BigInt at/above the 2^53 mantissa
boundary sat 1 ULP below real Perl's own (NV) cast — UINT64_MAX landed
at 0x1.fffffffffffffp+63 instead of 0x1p+64. This made even
non-fast-path cases diverge ("$big" stringification, `$big / 2`,
`$big + 0.5`, `$f + $big` all printed ...37095/...477 where perl
prints ...37096/...478). `perl_to_float`'s PERL_BIGINT case and D103's
`perl_auto_bigint_or_float` now convert via the exact decimal string +
`strtod` (correctly rounded to nearest by construction) through the
shared `perl_mpz_get_double` helper, matching real Perl byte-for-byte.
Note `perl_is_intlike`'s mpz paths and Math::BigInt method semantics
are untouched; D103's tests stay green.

Verified against real Perl for: the exact `$chain` repro; `$chain * 2`;
chained `+1+1`; `+=` and `*=` compound forms; mixing a BigInt var with
a plain float var in both operand orders; plain BigInt stringification
and `$big / 2`; subtraction within the UV window staying exact;
huge-literal operand (already-boxed D103 path); ordinary
float/int/mixed arithmetic; `--emit-ir` before/after identity for the
non-BigInt fast path (local var: identical; file-scope var: fast
branch identical, guard added); `arith.pl`, `nb.pl`, `nbody.pl`,
`fibn.pl`, `mbs.pl` green with FP tolerance. Tests:
`tests/d132_bigint_f64_fastpath_{smoke,deep}.pl`.

### D126 — `split` with a capturing-group pattern doesn't include captured delimiters — **FIXED 2026-09-12**

```perl
split(/(,)/, "a,b,c")
# perl:  ("a", ",", "b", ",", "c")   — 5 elements, delimiters included
# perlc: ("a", "b", "c")             — 3 elements, delimiters dropped (pre-fix)
```

Real Perl's `split` includes the text matched by any capturing groups in
the pattern as extra elements interleaved with the normal fields — a
documented, deliberate feature (used to keep the separators themselves).
`perl_split_regex` (`src/runtime.c`) always discarded everything between
`mstart`/`mend` (the whole match) and never inspected
`pcre2_get_ovector_pointer` past index 0/1 (the whole-match bounds), so
capturing groups' text was silently dropped. Named captures
(`(?<n>...)`) are ordinary capture groups to PCRE2 and were affected
identically.

**Fix** (`src/runtime.c` `perl_split_regex`, rewritten against real
Perl's actual split algorithm, derived case-by-case from perl 5.42
probes):

1. Each match's capture texts (from `pcre2_get_ovector_count(md)` —
   the match_data is created from the pattern, so this equals 1 + group
   count) are appended right after the field they ended, in
   group-number order. A group that did not participate in this match
   (optional group that failed its branch, e.g. `(x)?` on "a,b") still
   yields an element — **UNDEF, not omitted** (verified:
   `split(/(x)?,/, "a,b")` -> 'a', undef, 'b'); a participating-but-
   empty group yields "".
2. The walk was rewritten to track the *field start* separately from
   the scan position, matching real Perl's zero-width-match semantics,
   derived empirically: a **zero-width match at the field start is
   skipped** (no field, no captures — the empty match at 0 in "a,b"
   before the ',' at 1); a **zero-width match strictly inside a field
   ends it** (with this match's captures) and restarts at mstart,
   acting as a separator between characters exactly like the
   empty-pattern `//` case (`split(/x?/, "ab")` -> 'a','b'); a
   **consuming match always ends the field**, even when the field is
   empty (real Perl produces leading empty fields:
   `split(/(,)/, ",a,b")` -> '', ',', 'a', ',', 'b');
   no-match/end-of-string pushes the remainder **from the field start**
   (skipped zero-width matches may have left the scan position ahead of
   it). pos always advances, so the walk terminates for every pattern.
3. Consequences of the rewrite (all verified against real perl): LIMIT
   now counts *fields only* — capture texts are extra elements and
   never consume the LIMIT budget
   (`split(/(,)/, "a,b,c,d", 3)` -> a, ',', b, ',', "c,d"); the
   pre-existing **hang/garbage on all-zero-width patterns**
   (`split(/,?/, "a,b")` returned 0 elements, `split(/,*/, "a,xx,b")`
   hung) is fixed as a side effect (`/,?/ "a,b"` -> a, b;
   `/,*/ "a,xx,b"` -> a, x, x, b; `/x?/ "ab"` -> a, '', b); and the
   trailing-empty trim (`perl_split_trim_trailing_empty`, limit==0)
   also removes trailing UNDEF captures (verified
   `split(/(b)?,/, "a,")` -> ("a")), while never removing a trailing
   capture holding text (`split(/(,)/, "a,")` keeps the ','), and
   never touching the plain string-separator path (which can hold no
   undef).

Verified byte-for-byte against real Perl for: the original repro;
multi-group interleaving (`/(,)(;)/`, `((,)(;))` nested, `(?<n>,)`
named, `(?<x>,)(?<y>)`, `(\w)(\d)`, `(a)|(b)` alternation with undef,
`(,)|(:)`, `(,){2}`, `(:+)`, `(\s+)`, `(a*)`); optional-group undef
rendering; LIMIT 1/2/3/5 with one and two groups and with optional
groups; limit==0 trim with '', trailing-undef and capture interplay;
negative LIMIT (no trim, undef kept); leading empty kept; empty target
string; capture-with-empty-text -> ""; list-assignment usage; and five
plain-split regression checks (regex/string separator, LIMIT, trim,
negative LIMIT). Tests: `tests/d126_split_captures_{smoke,deep}.pl`.

### D125 — `use`/`no` pragma statements only parse at file top-level — **FIXED 2026-09-12**

```perl
sub foo {
    no warnings 'numeric';   # or: use strict;  — either one
    ...
}
# perl:  fine, scoped to the sub as expected
# perlc: Error: Parse error line 2: unexpected token 'warnings'  (pre-fix)
```

Found while writing D117's deep test. Root cause: `src/parser.cpp`'s
`use`/`no` handling lived inside `parseProgram()` (the file-level
statement loop) — not in `parseStmt()`, which is what every nested
block or sub body actually calls to parse its own statements. So
`use`/`no` only worked as the very first kind of statement the whole
file's top-level loop sees, never inside any nested scope. Confirmed
with both `no warnings '...';` and a plain `use strict;` inside a
`sub {}` — same failure, so this isn't `warnings`-specific.

**Fix** (`src/parser.cpp`, `src/parser.h`): the whole `use`/`no`
statement handling was extracted verbatim from `parseProgram()` into a
new private method `Parser::parseUseNoStmt()` (the caller has already
consumed nothing), now called from both `parseProgram()`'s file-level
loop and `parseStmt()` — so pragmas and module-`use`s inside any nested
scope parse the same way the file-top ones always did. The method
returns a Block whose args hold the statement(s) produced by one
use/no statement (0..n — `use parent` produces one SetIsa per parent;
fully-ignored pragmas produce an empty list), and both callers splice
those into their own statement lists (`parseStmt()` wraps the splice
in a `FlatBlock` so multi-statement results still run in order in the
current scope). Module inlining itself is unaffected: main.cpp's
`inlineModules()` token pass runs over the whole combined stream
regardless of statement nesting, so a `use Some::Module;` inside a sub
behaves like the file-top form. Note the `no` keyword is lexed as
IDENT (not a keyword token), so the dispatch condition checks both
`TK::KW_USE` and an IDENT "no".

Verified against real Perl for: `no warnings 'numeric';` inside a sub
(coercion still happens, sum identical), pragmas inside nested bare
blocks at several depths, pragmas in if/while bodies, `use POSIX
qw(floor)`-style module-`use` inside a sub actually importing
(tested with a fixture module, `tests/lib/D125Pragma.pm`), a pragma
immediately before a module-`use` in the same sub, file-top `use`s
unchanged, qualified access to a module's non-imported `@EXPORT_OK`
names after an explicit import, `no strict; no warnings; use integer;`
no-op forms, and the D112/D122/D127/D129/D121 module/lexer suites
staying green. Tests: `tests/d125_pragma_nested_{smoke,deep.pl}` +
`tests/lib/D125Pragma.pm`.

### D133 — double-free in the int/float-var Assign boxed fallback — **FIXED 2026-09-12**

```perl
sub f { my $acc = 0; { $acc = "x" + 0; } return $acc; }
my $v = f();
my $x = $v == 0;
print "$x\n";
# perl:  1
# perlc: segfault (allocator corruption from a double free)
```

Found while verifying D125's deep test (the pre-D125 2026-09-11
snapshot binary reproduces it identically — a pre-existing bug the
existing 305-test corpus never happened to hit). Trigger shape: an
int-or-float-promoted variable assigned a boxed owned temp (a
string→number coercion result) inside a bare block, with the variable
later read via a numeric comparison.

Root cause (`src/codegen.cpp`, `case NK::Assign`, the int/float-var
boxed-RHS fallbacks at the `lookupIntVar`/`lookupFloatVar` branches —
"RHS not purely integer/numeric — extract int/float from boxed value"):
after `perl_to_int`/`perl_to_float`-ing the boxed RHS into the unboxed
alloca, the code called `freeIfOwned(rv)` **and then `return rv`** —
handing the already-freed pointer back to the caller, whose statement
context (`ExprStmt`'s `freeIfOwned`, or `emitBlockLast`'s clone+free)
frees owned temps a second time. `isOwnedTemp` recognizes the
`perl_add`-family results, so `"x" + 0` was exactly such an owned
temp: freed twice. `perl_free` returns the PV to the slab pool
immediately, so the second free corrupts the freelist; the crash
surfaced later, at an unrelated allocation (`perl_alloc_undef`,
`perl_concat`), which is why it looked like a string-printing bug at
first (confirmed via gdb: SIGSEGV in `perl_concat` on a poisoned
pointer; under valgrind, SIGILL "illegal opcode" in `perl_alloc_undef`
— code/allocator corruption, classic double-free).

**Fix**: both fallbacks (int and float twins) now clone the boxed temp
for the caller *before* freeing it (`perl_clone` + `freeIfOwned`) so
each side owns its own value — the same discipline every neighboring
Assign path already follows. The common fast paths (`emitExprI64`/
`emitExprF64` succeeding, no boxing at all) are untouched.

Verified against real Perl for: the exact repro; the float-var twin
(`$acc = "3.75" + 0`); nested bare blocks with two coercion assigns;
the top-level (non-sub) form; a call-result RHS; a 200-iteration
allocator-stress loop; and the original crash shape passed as a call
argument (`checker("f", f() == 0)`). Tests:
`tests/d133_assign_double_free_{smoke,deep}.pl`.

### D134 — `syscall()` pointer-argument writes land in a pushed clone, invisible to the caller — **FIXED 2026-09-12**

```perl
my $buf = "\0" x 16;
my $ret = syscall(228, 4, $buf);   # SYS_clock_gettime, CLOCK_MONOTONIC
my ($sec) = unpack("LL", $buf);
# perl:  sec ≈ 17551 (kernel wrote the timespec into $buf)
# perlc: sec = 0  (pre-fix — kernel wrote into a pushed CLONE of $buf)
```

Found via `make test`: `tests/xs_ffi.pl`'s
`clock_gettime_sec_positive`/`clock_gettime_realtime_sec_positive`
assertions failed (buffer stayed all-zeros) while real perl passed.
Root cause: syscall's codegen built the argument array with
`perl_array_push`, which **clones** every element (`perl_clone` — a
fresh `sval` malloc for strings), and `perl_syscall`
(`src/runtime.c`) then handed the *clone's* `sval` to the kernel as
the pointer argument. The syscall dutifully wrote into the clone; the
caller's own cell never changed. This is the same in-place-buffer
mechanism `vec($str, off, bits) = val` relies on — which works,
because vec's codegen passes the variable's stable cell directly
rather than pushing a clone — applied to a path that broke it.

**Fix** (`src/codegen.cpp` `case "syscall"` in `emitCall`): each
argument is now pushed with the existing `perl_array_push_nc`
(no-clone, borrowed-ownership push — runtime already had it for
closure captures) and the array shell is torn down with the existing
`perl_array_free_nc` (elements never freed by the array); genuinely
owned temp elements are `perl_free`d explicitly after the call. So
`emitExpr($buf)`'s stable cell is what `perl_syscall` sees, and kernel
writes land in the caller's own `sval` (length unchanged, as with
real perl's fixed-size buffer). A temporary non-variable buffer
(`syscall(228, 4, "\0" x 16)`) now writes into the temp and discards
it with the temp.

**Verified divergence, deliberately left as-is:** real perl dies with
`Modification of a read-only value attempted` when a *literal/temp*
string is passed to a syscall that would write through it (perl marks
syscall lvalue-targets read-only); perlc has no read-only-value
enforcement and silently succeeds. Documented here rather than
emulated — the deep test deliberately excludes the temp case so it
stays byte-for-byte comparable.

Also fixed in passing: the arg array was leaked on every syscall
(never `perl_array_free`d) — now released via `perl_array_free_nc`.

Verified against real Perl for: CLOCK_MONOTONIC and CLOCK_REALTIME
returns (ret==0, sec positive/large, nsec in range), realtime >
monotonic, a reused buffer, a scalar cell that previously held a
number then a string, 100 iterations (ownership-slip/corruption
detector), a non-buffer syscall (`getpid`) unaffected, and the four
`make test` `xs_ffi.pl` clock assertions now passing (47/47
assertions, `make test` exit 0). Tests:
`tests/d134_syscall_buf_{smoke,deep}.pl`.

### D135 — sub-scope int-promoted variable truncates a later NV assignment — **FIXED 2026-09-13**

```perl
sub f { my $acc = 0; $acc = 5.5; return $acc; }
print f(), "\n";
# perl:  5.5
# perlc: 5   (truncated to the int part, pre-fix)
```

Found while writing D133's deep test (`call_result_coerce` assertion
failed). Pre-existing — reproduces identically on the 2026-09-11
snapshot binary. Root cause: inside a sub, a variable initialized with
an integer literal (`my $acc = 0;`) is int-promoted to an unboxed i64
alloca (`case NK::My`'s `emitExprI64` unbox branch); every later
assignment then routes through `case NK::Assign`'s int-var branch
(`lookupIntVar` hit), whose boxed-RHS fallback unconditionally
`perl_to_int`s the value before storing. `case NK::CompoundAssign`'s
int-var branch had the same truncation (`/= 2` on an int-promoted var
held 1, not 1.5). Real Perl has no such sticky per-variable type.

Confirmed shapes (all truncated pre-fix, all inside a sub): literal
float RHS (`= 5.5`), string-coercion RHS (`= "7.25" + 0`), sub-call
RHS returning an NV, even a plain string RHS (`= "7.25"` — coerced
through `perl_to_int`), chained self-assignment (`$t = $t + 0.5`),
and division (`$q = 1/2`, `$q /= 2`). Also fired for a `my $x = 0;`
declared inside a nested bare block at file scope (the block isn't a
sub, but the var was still int-promoted and truncated). File-scope
direct declarations were already correct; an uninitialized `my $acc;`
then assigning 5.5 was also correct — it's specifically the int-shape
initializer that triggered promotion.

**Fix** (`src/codegen.cpp`, `src/codegen.h`): int-promotion in `case
NK::My`'s unbox branch is now refused when a body scan says the
variable's scope ever writes it a value that isn't statically
int-only. The scan root is the variable's lifetime scope: the
enclosing sub's body, or (bare block at file scope) the program body —
the same body-scan machinery the `@_` promotion path already uses,
extended with three new conservative walkers:

- `rhsIsIntShapedCtx` — an expression is int-only iff it's an IntLit,
  a `+ - * %` BinOp/CompoundAssign over int-only operands, a unary
  minus of an int-only operand, or a ScalarVar read of a variable in
  the provably-int-only set (or the variable itself — a self-referencing
  `$i += $i` stays int). FloatLits, StringLits, call results, `/`/`**`
  ops, and everything else are treated as possibly-fractional.
- `d135ComputeIntSet` — computes that set to a **fixpoint**: start with
  vars whose every write's RHS is int-only without variable operands;
  repeatedly admit vars whose RHS operands are all IntLits or already
  admitted vars. This is what keeps the classic hot idiom
  `my $s = 0; for my $i (1..1000000) { $s += $i; }` on the i64 fast
  path: `$i` (whose writes are the loop's self-increment) is admitted
  round 0, `$s` round 1, and the generated IR is **byte-identical** to
  the pre-fix compiler for the pure-int case (verified via
  `--emit-ir`), so no numeric-kernel performance is lost.
- `assignsFloatLikeRhs` / `readsFloatSensitive` — walk the scan root
  for any write to the name with a non-int-shaped RHS (or any
  float-sensitive read: `/`, `**`, sqrt, unary minus). When found, the
  declaration either takes the float-unbox path (if the body never
  uses the name in a position where a plain double is observably
  different from a real PerlValue* — `floatVarUseSafe`, new, checks
  string ops/ref-taking/call args/etc.) or falls through to the always-
  correct boxed PV path.

Note the CompoundAssign-sval subtlety this surfaced: CompoundAssign
nodes store the **bare** operator in `sval` (`"/"` for `/=`, `"+"` for
`+=` — see `parseCompoundAssign`), which the first-attempt scanners
wrongly checked as `"/="`/`"+="` and therefore never matched; the
wiring now checks the bare forms. With promotion refused, the
truncating int/float-var Assign and CompoundAssign branches are never
reached for such variables — the boxed PV path stores the true value.

Verified against real Perl for: every trigger shape above (sub-scope
literal/coercion/call/string RHS, chained, division both statement and
compound forms), the float twin (`my $v = 0.5; $v = 3;` holds 3 and
prints `3` like perl, not `3.0`), nested bare blocks, assignment inside
an if body, formatting parity (`5.5` prints as `5.5`), a 1e6-iteration
pure-int counter staying exact with byte-identical IR, a mixed
int+float sub, int-variable copies (`$b = $a; $b += 4`), and ordinary
string/numeric behavior. Full harness 321/321 (with D128's fixtures
merged in the same binary), `make test` 47/47, `bench/nb.pl` and
`bench/mbs.pl` within their normal runtimes. Tests:
`tests/d135_int_promo_nv_{smoke,deep}.pl`.

## Remaining product gaps (not logged as D-numbers)

Real perlguts XS (SV*-ABI XSUBs) and complex CPAN (advanced `our`/OO —
POD is skipped). Typeglob `{IO}`/`{FORMAT}` slots are not implemented.
String `eval EXPR` sees outer `my`. Runtime `eval`/`do` still needs
clang+perlc
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
7. **~~Full XS / DynaLoader~~ → downgraded to "real perlguts XSUBs only"** — the DynaLoader-compatible FFI (phase 2, 2026-09-14) implements `dl_load_file`/`dl_find_symbol`/`dl_install_xsub`/`dl_error`/`bootstrap` + `XSLoader::load` natively (see below), so `use DynaLoader; bootstrap(Module)` and the raw-C `dl_*` surface work for perlc-built `.so`/`.pl` modules and for hand-built C libraries via `XS::call`. What remains impossible without an SV-ABI emulation layer is loading genuine perlguts XSUB modules compiled against real perl headers.
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

### Exporter mechanism + DynaLoader-compatible FFI — 2026-09-14

Two capability additions (not D-number defects — these close roadmap
items from `MVP_ROADMAP.md`).

**D136 — bareword `=>` auto-quote** (`src/parser.cpp`): real Perl
auto-quotes EVERY bareword before a fat comma, including its own
keywords — `my %t = (all => 1, sub => 2, and => 3);` is exactly
`('all','1','sub','2','and','3')` (verified directly). perlc's lexer
classifies `all`/`sub`/`and` as keyword tokens (KW_ALL, KW_SUB, ...),
which fell into the builtin-keyword parse branches and died as
`unexpected token '=>'` — `my %t = (all => 1);`, one of the most common
hash-literal spellings, was a hard parse error. Fixed at the top of
`parsePrimary`: any token carrying text that is not itself a delimiter/
number/string/regex, when immediately followed by `=>`, returns
`makeStr(text)` — with the `=>` left for the surrounding list loop to
consume as the pair separator (eating it there strands the value).
Also part 2: in hash-subscript key context (`inKeyContext_`, set around
`$h{...}` key parsing), ANY bareword-ish token is the string key
whatever follows — `my @a = @{ $r->{all} };` previously died
"unexpected token '}'" because the inner key `all` fell into the
KW_ALL builtin branch which then demanded args and hit the closing
brace. Real Perl: barewords are strings in key position, no exceptions.

**D137 — `qw(...)` slice key specs spread into individual keys**
(`src/parser.cpp` HashSlice/ArraySlice parse loops + `src/codegen.cpp`
DeleteFunc): `@h{qw(a b)}` is exactly `@h{('a','b')}` in real Perl.
perlc parsed the single QWORDS token as ONE element; codegen turned
that into one undef key lookup, so multi-key qw slices silently came
back empty, and `delete @h{qw(a b)}` deleted nothing. All four
slice-form parse sites (deref-brace, deref-bracket, named-hash-brace,
and the delete statement's own hash/array slice branches) now spread a
QWORDS token into individual string keys/indices, and the delete
codegen handles the resulting per-key StringLit args.

**Exporter mechanism** (`src/main.cpp` `scanExports`/`inlineModules`):
`%EXPORT_TAGS = (tag => [...])` is parsed at compile time (value forms:
`qw(...)`, `['a','b']`, `["a"]`, and `\@EXPORT_OK`/`[@EXPORT_OK]` refs
resolving to the already-scanned `@EXPORT_OK` contents), exposed as
`TAG:<name>` entries. `use Module qw(:tag)` expands via the tag's list;
`:all` expands to the module's own `TAG:all` when defined — matching
real Exporter, which does NOT auto-include `@EXPORT` names in a custom
`:all` tag (verified: a module with `@EXPORT` and `:all => \@EXPORT_OK`
does not export the `@EXPORT` names); when the module has *some*
`%EXPORT_TAGS` but no `:all` tag, real Exporter dies and perlc now dies
with the same message; the `@EXPORT`+`@EXPORT_OK` union is used only
when the module declares no `%EXPORT_TAGS` at all (the sloppy-module
case real scripts assume works). `&`-sigil'd export names
(`qw(&pod2usage)`, real Pod::Usage style) strip the sigil before
matching/aliasing. Tests: `tests/exporter_tags_{smoke,deep}.pl` +
`tests/exporter_slice_keys_{smoke,deep}.pl` (+ `tests/lib/E/Tagged.pm`),
all byte-for-byte vs real perl.

**DynaLoader-compatible FFI, phase 2** (`src/runtime.c`, `src/
codegen.cpp`, `src/main.cpp`): `use DynaLoader;` is now accepted, and
the native surface is `DynaLoader::dl_load_file($path, $flags)` →
opaque libref (a 1-based integer index into an internal dlopen-handle
table; real perl's libref is equally opaque), `dl_find_symbol($libref,
$sym)` → opaque symref (index into a dlsym-result table),
`dl_install_xsub($perl_name, $symref)` (registers the fn pointer as a
Perl-callable sub under that name — the perlc XSUB convention is
`PerlValue *(*)(PerlArray *args, int ctx)`, the same shape every
perlc-compiled sub has; raw-C ABIs stay on `XS::call`'s signature
dispatch), `dl_error()`, and `bootstrap($module [, $version])` +
`XSLoader::load($module [, $version])`. bootstrap implements real
DynaLoader's flow with a perlc twist: it searches `PERLC_LIB`/`PERL5LIB`
+ `.`/`lib` for `auto/<Mod/pname>/<Modfname>.so` (real DynaLoader's
layout) and — the twist — also `<Modfname>.pl`, compiling it on demand
via the same `PERLC_SELF_PATH --do-lib` subprocess `do FILE` uses; the
loaded module's subs register into this process's registry, and the
boot hook is called under `<Module>::boot` (perlc convention, unambiguous)
with real DynaLoader's `<Module>::boot_<mangled>` and bare
`boot_<mangled>` also tried. Fixtures: `tests/lib/auto/My/Clib/Clib.so`
(hand-built C library: `my_add`, `my_greet`, `my_scale`) and
`tests/lib/auto/My/Pxs/Pxs.pl` (perlc-compiled bootstrap module). Tests:
`tests/dynaloader_ffi.sh` (self-verifying, outside the stdout-diff
harness corpus like d128's — the behaviors exercised are perlc-specific
by design: real perl cannot bootstrap a perlc-compiled module). Real
perlguts XSUBs (SV*-ABI, compiled against real perl headers) remain out
of scope — that would require an SV emulation layer.

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

### D124 — `__SUB__` (current-sub reference) unimplemented — **FIXED 2026-09-13**

```perl
use feature 'current_sub';
my $fact = sub { my $n = shift; $n <= 1 ? 1 : $n * __SUB__->($n - 1) };
print $fact->(5), "\n";   # perl: 120  |  was: parse error
```

Split off from D116. The hard part TESTS.md's D116 write-up identified
was real: a correct `__SUB__` inside a closure must be the *running*
closure object — carrying the fn pointer AND the capture set it was
built with — which no compile-time substitution can produce (wrapping
`currentFn_` in a fresh `perl_make_code_ref` would silently drop the
closure's own captures, detaching `__SUB__`-recursion from the captured
lexicals it mutates through).

**Fix** (`src/runtime.c`/`src/runtime.h`, `src/parser.cpp`,
`src/codegen.cpp`, `src/codegen.h`): the runtime already funnels every
closure invocation through `perl_call_code_ref`, which installs the
running closure's capture array in thread-locals. It now also installs
the closure's `PerlValue*` code-ref object itself in a new
`__thread PerlValue *s_current_coderef` (saved/restored around the call,
nesting-safe), exposed as `perl_get_current_code_ref()`. `__SUB__` is
parsed exactly like `__FILE__` (a plain `NK::Call`, so the parser needs
no constant-substitution machinery) and intercepted first in
`CodeGen::emitCall`:

- inside an emitted closure body (`case NK::AnonSub`; the new
  `inAnonSubEmit_` flag, save/restored around the emission — sort
  comparators excluded, see below): `callRT
  ("perl_get_current_code_ref")` — the actual running closure, captures
  included, verified by a deep test where two closures made from the
  same factory but capturing different `$add` values recurse through
  `__SUB__` and each sees only its own;
- inside a named sub's body: that sub's own code ref, built the same way
  `case NK::RefSub` does (`perl_make_code_ref(cast(currentFn_))`) from
  the new `currentSubName_` state — correct here because named subs in
  this codebase resolve free variables by name rather than captures, so
  a capture-less code ref matches the existing `\&name` model (verified:
  `__SUB__ == \&in_named` is true);
- at file scope: `perl_get_current_code_ref` returns undef — matching
  real Perl, `defined(__SUB__)` is false outside any sub.

Also fixed to make the feature reachable: the `use feature
'current_sub'` feature flag already parsed (D116-era feature handling);
the RT registration table gained the new runtime symbol ("Unknown
runtime function" otherwise). Logged, not fixed: `__SUB__` inside a
`sort { ... }` comparator returns undef (comparators are emitted as raw
LLVM functions called through a non-`perl_call_code_ref` C-style path,
and real Perl's behavior there is itself an edge case); `__SUB__` inside
an eval-STRING-compiled sub works through the same `perl_call_code_ref`
path but is untested. Tests: `tests/d124_current_sub_{smoke,deep}.pl`.

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

### D128 — a parse error inside an inlined module reports the wrong line/file — **FIXED 2026-09-13**

    # main.pl (10 lines):
    use lib 'tests/lib';
    use D128Broken;
    ...
    # D128Broken.pm:
    package D128Broken;
    use warnings;
    my = 5;        # line 3: the actual syntax error
    1;

    # perlc (pre-fix):  Error: Parse error line 3: unexpected token '=' ...
    #   — no filename at all, and "line 3" is the module's internal line,
    #     which for a real case (podchecker, 148-line main script) reported
    #     line 545 — meaningless to anyone debugging the main script.
    # perlc (post-fix): Error: Parse error in tests/lib/D128Broken.pm line 3:
    #   unexpected token '=' (...)
    # real perl:        syntax error at tests/lib/D128Broken.pm line 3, near "my ="

Found via the 2026-09-10 real-module survey #2. `corelist` and
`podchecker` (both real, unmodified system scripts) each produce a
parse error whose reported line number is implausibly small relative
to where the actual `use`/`no warnings`-adjacent tokens are. Root
cause: `inlineModules` (`src/main.cpp`) splices each module's own
token stream (lexed independently, with its own 1-based line counter)
directly into the combined stream with no offset adjustment and no
filename tag — downstream parse errors report whatever line number
happened to be attached to the offending token, which is meaningless
once multiple files' token streams have been concatenated, and never
say which file is actually at fault.

**Fix** (diagnostics-only; no codegen/runtime changes, `Token` grows
by one pointer):

1. `Token` gains `const char *file` (default nullptr). Lifetime: a
   process-wide `std::deque<std::string>` filename registry in
   `lexer.cpp` (`lexer_register_source_file()`) — deque elements never
   move, so the pointed-at name outlives every copied token stream;
   the registry only ever grows (one entry per lexed file per
   compile). No synchronization needed (single-threaded compile path).
2. `Lexer` takes an optional `sourceName` (default `""`) and stamps it
   onto every token it emits — done at `tokenize()`'s two exits so no
   push site can be missed. `main()` passes the main script's path;
   `inlineModules()` passes each module's resolved `fullPath` (both
   the `use` and `require` load paths, plus the transient
   `scanExports()` lexer). The by-value splice preserves the pointer.
   Empty name (the parser/codegen's synthetic-fragment lexers) leaves
   tokens untagged.
3. All nine `Parse error` throw sites in `parser.cpp` now go through
   one new helper, `Parser::parseErrPrefix(line)`: if the *current*
   token's file tag is non-null and differs from the main file's
   registered tag, the error is prefixed `Parse error in <module file>
   line N: ` (the module's own internal line — the only frame the user
   can act on); main-script and untagged tokens keep the exact legacy
   `Parse error line N: ` format (existing tests/tooling match it).
   The seven sites that previously threw with no line info at all
   (tie, prototype arity, `=~`/`!~` regex ops) gained the same prefix
   with their message text unchanged.
4. Deliberately untagged (legacy format): tokens synthesized by the
   compiler itself — `use constant`'s `sub NAME { return ...; }`
   wrapper tokens, the synthetic `package main;` separator, and the
   interpolation/sub-expression fragment lexers. These aren't file
   text; a real parse error in a `use constant` *value* still reports
   correctly because the value tokens themselves come from the tagged
   source stream.

Verified: the broken-module case reports `D128Broken.pm line 3`
(matching real perl's blame), a main-file control keeps
`Parse error line 2:` byte-for-byte, `require`-path and `use lib`-
resolved modules and nested module-of-module errors report the right
file, and parse errors inside `use constant` values blame the source
file. Full harness 321/321, `make test` 47/47. Tests:
`tests/d128_module_error_location.sh` (self-verifying —
compile-failure diagnostics can't live in the stdout-diff harness)
with fixtures `tests/lib/D128Broken.pm` +
`tests/d128_module_error_main.pltxt` (the `.pltxt` extension keeps
both outside the harness corpus, since the main fixture must fail to
compile).

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

### D120 — subscripted deref-in-a-string doesn't interpolate correctly — **FIXED 2026-09-13**

```perl
my $aref = [10, 20, 30];
print "first: $$aref[0]\n";      # perl: first: 10   |  was: first: [0]
my $r = [1,2,3];
print "slice: @{$r}[0,1]\n";     # perl: slice: 1 2   |  was: slice: 1 2 3[0,1]
```

`src/parser.cpp`'s `parseStringInterp` (the raw-string interpolation
scanner used by all `"..."` literals, backtick strings, and — since
D109 — `s///` replacement text) explicitly documented this exact gap in
its own `$$` handling comment. Bare `$$word` (scalar deref, no
subscript) and `$$` (PID) already worked; it was specifically
`$$word[i]` / `$$word{k}` / `@{$expr}[...]` / `@$ref[...]` / the
`@{[ ... ]}` trap idiom that didn't — the deref part interpolated and
the following `[...]`/`{...}` stayed literal text.

Pre-fix divergence table (form | perlc | perl): `$$aref[0]` →
`ARRAY(0x…)` — the whole-ref deref, not the element (and on a slice,
the same plus literal bracket text); `$$href{k}` → ref stringification;
`@{$r}[0,1]` → whole-array join + literal `[0,1]`; `@{[ ... ]}` → the
literal `[ ... ]` text after the array's join (the idiom is a hard parse
error in some shapes); `$$aref[-1]` / `$$aref[$i]` / `$$href{$k}` all
truncated to bare-deref + literal subscript.

**Fix** (`src/parser.cpp`, `src/codegen.cpp`): kept the hand-rolled
scanner (Option 1) and taught it the missing grammar via one new helper,
`Parser::parseSubscriptGroup(raw, i, line, nameRef, isOpenBracket,
exprRef)`, which consumes one or more adjacent subscript groups from the
raw string text and builds exactly the node shape the token-level parser
produces for the equivalent spelling:

- nameRef form (`$$name[0]`, `$$name{k}`, `${name}[0]`): the first group
  is D63's single-deref element access — `ArrowDeref("array"/"hash")`
  with the inner scalar in `left` (real Perl: `$$aref[0]` is
  `${$aref}[0]`, exactly one deref level); further adjacent groups chain
  as ArrowDeref on the previous result (`$$aref[0][1]`), matching the
  existing $varname machinery.
- exprRef form (`@{$r}[...]`, `@$ref[...]`, `@{[ ... ]}`): the first
  group is an `ArraySlice`/`HashSlice` with the derefed-ref expr in
  `left` (real Perl requires an explicit `->` for further groups, so
  the run stops there), and the slice is wrapped in the same
  `JoinFunc(" ")` wrapper the surrounding whole-`@{$r}`/D73 slice paths
  use (a bare slice node in the interpolation assembly emits the ref,
  not its elements).
- `{...}` groups bareword-quote their keys (split on top-level commas,
  `makeStr` for bare text, re-lex only `$`/`@`-prefixed parts) — a
  bareword handed to the token-level `parseExprListFromTokens` outside
  `inKeyContext_` is a hard "String found where operator expected" parse
  error (this is why `"$$href{k}"` initially hard-errored the whole
  file). `[...]` groups keep `parseExprListFromTokens` (indices are
  expressions: numbers, `$vars`, ranges).

Two implementation subtleties worth recording: (1) the helper's entry
originally did `NodePtr node = exprRef ? std::move(exprRef) : …` and
then checked `if (exprRef)` for the build branch — the parameter was
already moved-from (nil) by then, so every slice silently degraded to
the bare base node; fixed by latching `const bool isExprRef` before the
move and branching on that. (2) the `@{expr}` branch's balanced-brace
scan already yields `exprNode` from the inner text, so the slice hooks
reuse it directly — no re-lexing.

Also fixed while verifying: `s///` replacement text (D109's
`parseInterpString` path) shares the same `$$`-subscript machinery, so
`s/^x/$$aref[1]/` works too. Intentionally unchanged: everything that
already worked (`"$name"`, `"@arr"`, `"${name}"`, `"$h{k}"`,
`"$arr[0]"`, `"$$word"`, `"$$"`, escaped sigils) stays byte-identical —
the full harness gates that. Logged, not fixed: `$$href` where `$href`
holds a *hash* ref die in real perl ("Not a SCALAR reference") but
perlc prints the ref and continues (runtime wrong-ref-deref semantics,
not interpolation). Tests: `tests/d120_string_deref_interp_{smoke,deep}.pl`.

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

## 2026-09-14 real-world probe survey #3 + Tier-1 module batch

Two parallel agents: one ran a 12-script probe survey (never covered by
surveys #1/#2), the other implemented the Tier-1 native modules
(Cwd, Sys::Hostname, File::Spec/+::Functions, Time::Local — see the
sections above). Both gated 331/331 in their sandboxes; merged tree is
341/341.

**Implemented (agent B)**: Cwd (`getcwd`/`cwd`/`abs_path`/`fast_abs_path`/
`realpath` — a faithful `fast_abs_path` port incl. its "nonexistent
path → catfile($cwd,$path), not undef" quirk), Sys::Hostname
(`hostname()` with real 1.25's trailing-NUL/CR/LF strip), File::Spec
(+ ::Functions + ::Unix: canonpath/catdir/catfile/catpath/join/splitpath/
splitdir/rel2abs/abs2rel/curdir/updir/rootdir/devnull/tmpdir/
file_name_is_absolute/no_upwards/case_tolerant/path, all three
invocation styles — class method, qualified, imported-bare — with the
real module's exact quirks: `canonpath("/a/../b")` stays
`/a/../b` by design; `file_name_is_absolute("x")` returns `""` not 0;
`catfile("a","","b")` → `a/b`), and Time::Local (all eight exports:
timegm/timelocal/`_nocheck`/`_modern`/`_posix`, real 1.35's
three-step DST algorithm and exact croak messages with caller
location). Also fixed en route: scalar-context `gmtime(EXPR)`/
`localtime(EXPR)` returned the epoch instead of the ctime string.
Tests: `tests/{cwd,sys_hostname,file_spec,file_spec_functions,time_local}_{smoke,deep}.pl`
(10 files, byte-for-byte).

**Survey #3 table** (compile pass 1 as-is; pass 2 with `-I` at real
perl's core dirs):

| Script | Modules | P1 | P2 blocker (now fixed unless noted) |
|---|---|---|---|
| update-locale | Getopt::Long | OK | — (byte-identical) |
| xsubpp | Getopt::Long | OK | — (Configure() no-op added; byte-identical) |
| pptpsetup | Getopt::Long | OK | — (`&delete()` keyword-named-sub call fixed; byte-identical) |
| update-xmlcatalog | File::Spec, Getopt::Long | no | File::Spec → now implemented; byte-identical no-args |
| geteltorito | Getopt::Std | no | `${"opt_$x"}` symbolic deref (W22, open) |
| instmodsh | ExtUtils::Installed, IO::File | no | Fcntl SEEK_SET tag export+value (W16, open) |
| pod2usage | Config | no | Config.pm:52 `*{$pkg.'::'.$f}=\&{$f}` (W27, open) |
| validlocale | POSIX qw(LC_ALL) | no | LC_ALL bareword-constant value (W16, open) |
| pl2pm | core only | no | `s/\|foo\|bar/next`-style `next` after `||` (W2-class, open) |
| update-language | Text::ParseWords | yes | W29 `local *_` fixed 2026-09-16 |
| piconv | Encode, Encode::Alias | no | `$find =~ $alias` (W28, open; needs `qr//` too) |
| shasum | Digest::SHA, Fcntl | no | Errno.pm's `eval "sub $name()..."` (W30, open) |

**Fixed during the survey** (each verified vs real perl; full harness
331/331 + `make test` 47/47 in the agent sandbox before merge):

- `my (%h)`/`our (%Config, $VERSION)` list declarations with mixed
  sigils (Config.pm:11 — blocked 6 of the 12 scripts): `parseMy`'s
  paren fast-path now only fires when the *next* token is `)`.
- `use constant` multi-token values: `5 - 3` was silently `5`
  (single-token capture), and `!!$ENV{X}` produced an unparseable
  injected `sub { return !; }` that OOB-crashed — values are now
  captured as full expressions, and the `{ }` block form scopes each
  entry's value end at the next top-level comma.
- `<<5` after an integer (`1<<5`) lexed as a heredoc (real Perl prints
  32) — and the mis-lex also OOB-crashed `inlineModules` (perli11ndoc
  compiler crash).
- `q!...!`-style arbitrary-delimiter `q`/`qq` openers (any non-alnum
  opener now accepted; closers and `=` still excluded).
- whitespace between `qw` and its delimiter (`qw (` — Encode/Alias).
- `tr///` with arbitrary delimiter pairs (`tr|/|_`, `tr{/}{_}`,
  `tr!-!!d`), matching `s///`'s existing delimiter handling.
- `print($fh "str")` (no comma) is a filehandle form, matching real
  Perl exactly; `print(LOG "str")` bare-fh-in-parens too.
- `&delete()`/`&help()` — keyword-named subs callable with `&`
  (pptpsetup idiom).
- `Getopt::Long::Configure(...)` accepted as a no-op returning 1
  (real Getopt::Long's parser config isn't modeled; unblocked xsubpp).

**Logged, not fixed** (repros in git history of the survey agent's
report; the highest-leverage next items):
1. **W27 — Config.pm's `*{$pkg.'::'.$f}=\&{$f}`** (computed typeglob
   assignment) — Config.pm is the highest-leverage single target (it
   gates pod2usage, instmodsh, perlthanks, h2ph, splain...).
2. **W16 — Exporter tag validation + XS-constant values**: requesting a
   name that's only in a `%EXPORT_TAGS` value (e.g. Fcntl's SEEK_SET in
   `:standard`) is falsely rejected, and there's no value path for
   XS-module constants (LC_ALL, SEEK_SET) once validation passes.
3. **W30 — runtime sub-defining string eval** (`eval "sub $name() { 2 }"`
   — Errno.pm, and with it File::Path users).
4. **W22 — `${"opt_$x"}` symbolic deref** (Getopt/Std.pm → geteltorito).
5. **W23 — in-key auto-quote swallows builtin keywords** in
   `$h{lc $k}`-style expressions (D136 over-generalization;
   Debconf::ConfModule, Encode::MIME::Name).
6. **W28 — `$s =~ $var`** (pattern in a variable; Encode/Alias →
   piconv; entangled with the still-open `qr//` gap).
7. **W29 — `local *_ = \join(...)`** (Text::ParseWords → update-language).
8. **W31 — `sub f { return (32); }` returns empty** (real: 32); also
   makes `use constant D => (32)` yield empty.
9. **W19 — `use constant B => A + 1`** — the fresh-Parser constant
   injection lacks constMap, so a bareword prior constant misparses.
10. `qr//` remains the single biggest parse blocker for the File::Spec/
    regex-heavy module family (now that File::Spec's *functional* API
    is native, qr// mostly blocks remaining module-internal uses).

Updated Tier-1 module priority after the survey: (1) Config.pm chain
(W27+W30), (2) Fcntl/POSIX constants + tag validation (W16), (3)
`=~ $var` + `qr//` (W28).

### W16/W15b/W27-for-Config/W19/W23/W28/W31/W22 — survey-3 items FIXED (2026-09-16)

**Config native module** (`src/runtime.c` `perl_config_*` + generated
`src/config_data.h` + `src/codegen.cpp`): `use Config;` is now a native
module — `%Config`/`$Config::Config` is a special hash (HashElem,
ExistsFunc, KeysFunc dispatch) backed by a generated key/value table of
ALL 1261 real keys from the host perl (`tools/gen_config_data.pl`, run
once, output committed), and the 4 real functions
`myconfig()`/`config_sh()`/`config_vars()`/`config_re()` produce
byte-identical output to real Config on this host (myconfig/config_sh
are baked strings from the host `perl -V`). Because the module is native
its file is never inlined, so Config.pm's computed-typeglob import
(W27) never executes — that item stops mattering for Config users
(pod2usage, instmodsh, perlthanks, h2ph, splain). `exists $Config{...}`
and `exists $ENV{...}` now work (the ExistsFunc single-key path had no
special-hash handling; real message parity maintained). Keys iteration
(`keys %Config`, both contexts) works. Behavioral note: perlc resolves
`$Config{...}` without requiring `use Config;` (the special hash is
always present), unlike real perl which needs the import — accepted
divergence; every strict/warnings script uses `use Config;` anyway.

**Fcntl/POSIX/Errno native constants** (`src/native_constants.h` from
`tools/gen_native_constants.pl`, probed from the host perl — 86
constants: SEEK_*, O_*, F_*, LOCK_*, FD_CLOEXEC, S_*/S_IF*/DT_*,
POSIX LC_*, Errno's errno values): a zero-arg call
`Fcntl::SEEK_SET` / `POSIX::LC_ALL` / `Errno::ENOENT` (or the bare
imported name) resolves through the table; unknown names die with real
Fcntl's exact croak ("NOT_A_REAL_MACRO is not a valid Fcntl macro" —
bare name, no package prefix). `use Fcntl qw(:seek :flock :DEFAULT)` /
`:flock` tags expand in main.cpp with the REAL module sets (probed:
Fcntl's @EXPORT does NOT contain SEEK_*/LOCK_* — they're @EXPORT_OK;
:DEFAULT is exactly @Fcntl::EXPORT). `use Fcntl` + sysseek now works
(new `perl_sysseek_fh`, an lseek(2) wrapper). Real-perl quirk matched
in the parser: a bareword followed by a list COMMA in an imported
constant's place is a plain STRING (`print "perm: ", S_IRUSR, ","...`
prints the string "S_IRUSR" when S_IRUSR wasn't imported — verified),
while a QUALIFIED all-caps `Fcntl::F_GETFD` after a comma is always the
constant call (no import needed). Tests:
`tests/config_{smoke,deep}.pl`, `tests/fcntl_posix_{smoke,deep}.pl`.

**W16 (Exporter tag-value validation)**: resolved for the native
modules by design (the tag tables above are the validated export sets;
a bogus tag dies ""is not defined in %EXPORT_TAGS"). For arbitrary
.pm modules the scanExports TAG: values are now accepted during
validation, closing the Fcntl-SEEK_SET false rejection for file-backed
modules too.

**W31 — `sub f { return (32); }`**: single-element parenthesized
returns yield the element (both `case NK::Return` sites); list-context
`return (1,2)` scalar-context semantics and bare `return;` (D115)
verified unchanged. `use constant D => (32)` yields the element.
Tests: `tests/w31_return_list_{smoke,deep}.pl`.

**W23 — in-key auto-quote over-reach**: a builtin keyword in hash-key
position followed by an argument starter is the builtin call
(`$h{lc $k}` → key = lc($k); `map { $c{uc $_} = 1 }`), while
`$h{all}`/`$h{keys}` (keyword followed by `}`/`,`/`=>`) stay string
keys; `$h{__PACKAGE__}` still auto-quotes. Extra find: `(not => 1)`
died in parseLowNot before the D136 hook could see it — fixed with a
`=>` lookahead. Tests: `tests/w23_key_expr_{smoke,deep}.pl`.

**W28 — `$s =~ $var`**: the RHS of =~/!~ may now be any expression; a
non-regex RHS is stringified and matched with empty flags at runtime
(new `RegexMatchExpr` node; `!~` covered; `s/$var/repl/` and
`split($var, ...)` verified working). Logged, not fixed: list-context
captures from a NON-/g match (`my @m = ($s =~ $p)`) still return scalar
truth — pre-existing for literal patterns too, needs a new runtime
entry. Tests: `tests/w28_match_var_{smoke,deep}.pl`.

**W22 — `${"opt_$x"}` symbolic deref** (`Getopt/Std.pm` idiom):
`${ EXPR }` resolves the named global through the process glob registry
(both read and lvalue write), and `${$ref}` with a REF value derefs
normally (tag check, not glob lookup). Bonus real-perl parity fix:
`\$arr[1]`/`\$h{k}` now take the ref of the ELEMENT (real Perl's `\`
binds looser than subscripts) — writes through the ref update the
container. Tests: `tests/w22_symbolic_deref_{smoke,deep,deep2}.pl`.

**W19 — `use constant B => A + 1`**: parser side — unary `+` was
missing from parsePrimary (real Perl's documented no-op unary plus), so
`A + 1` died "unexpected token '+'"; and the throwaway value-parser now
sees earlier constants (`parseExprFromTokens(tokens, constMap*)`
overload + main.cpp passing the live constMap at both emitConstSub
sites), so the chained constant evaluates. Tests:
`tests/w19_unary_plus_{smoke,deep}.pl` + a chained-constant probe
verified byte-for-byte.

### qr// + list-context match captures + regex-pattern interpolation — FIXED 2026-09-16

**`qr/PAT/FLAGS` as a value** (lexer TK::QR + parser `NK::QrRegex` +
runtime `PERL_QR` tag + `PerlQrRegex` refcounted object): `my $re =
qr/^uc/i;` is a compiled-pattern value — `ref($re)` is `"Regexp"`,
`"$re"` stringifies as real perl's `(?^FLAGS:PATTERN)` with flags
canonicalized to m,s,i,x order (probed: `qr/x/simx` → `(?^msix:x)`,
`qr/x/mix` → `(?^mix:x)`, `qr/x/n` → `(?^n:x)`), the value flows through
subs/args/arrays/hash values with proper refcounting (clone + assign
paths bump/release like CODE_REF does), and `$s =~ $re` / `!~ $re` use
the QR's own pattern+flags directly through the W28 dynamic-match path
(`perl_regex_match_sv`). `qr{...}`/`qr(...)`/`qr!...!` arbitrary
delimiters work, with the m//'s collision guards (bare sigil, `->`,
closers, `qr =>` fat-comma keys).

**Regex-pattern interpolation** (`/pat-with-$var/`, `s/$pat/x/`,
`qr/$var/`): a regex literal whose pattern text contains a `$`/`@`
interp trigger is parsed through the same `parseStringInterp` machinery
string literals use (new `NK::RegexMatchInterp` node for `=~`, and
`n.right` on RegexSubst/QrRegex), the pattern being built at runtime —
`/$name/`, `s/f$name/XX/`, `s/$pat/X/` with a qr operand, and
`/$qqr/` with a qr all match real perl byte-for-byte. Previously
regex-literal patterns were compile-time-only (`/$var/` silently
matched the literal text `$var`).

**List-context non-/g match captures**: `my @m = ($s =~ /pat/);` (and
the `$s =~ $re` qr form) now returns real perl's capture LIST via a new
`perl_regex_match_captures_list` (a dedicated PCRE2 pass that also
updates `$&`/`$1..$N` like the scalar match); a groupless match yields
the one-element truthy list `(1)` (real perl: `my $c = () = ($s =~
/a/)` is 1); `!~` is excluded — it stays a plain boolean even in list
context (the sys_hostname_deep regression this caught). `/g` list form
(`perl_regex_match_all`) unchanged. Tests:
`tests/qr_regex_{smoke,deep}.pl`,
`tests/qr_match_list_{smoke,deep}.pl`.

Remaining regex gaps: `(?&name)` recursion and embedded-code patterns
(`(??{...})`) are PCRE2-level niceties real modules rarely use;
`$qr->(...)` code-sub invocation of a Regexp object is unimplemented.

### W29 (`local *_ = ...` / `local $_ = ...`) + false-bool stringification — FIXED 2026-09-16

**W29** (`src/parser.cpp` `local *_`/`local $_` branch → new `NK::LocalGlob`;
`src/runtime.c` `s_dollar_under` stable cell + `perl_get_dollar_under()` +
`perl_deref_if_ref()`; `src/codegen.cpp` `case NK::LocalGlob`): the
Text::ParseWords idiom `local *_ = \join(...)` (and plain `local $_ = v`)
now works. The global `$_` cell is localized with the standard depth-save
mechanism; the sub's lexical `$_` shadow (when the sub uses `$_`) is saved
AND assigned too, so reads inside the sub see the localized value and the
sub-exit depth-restore puts both storages back. Three stacked details were
needed to match real perl: (1) `hasLocalStmt()` had to learn `NK::LocalGlob`
— named subs only emit the epilogue `perl_local_restore_to(depth)` when the
body contains a known local node kind, so `local *_` alone produced saves
with no restore (every later read saw the localized value); (2) the
RHS of `local *_ = \join(...)` is a REFERENCE — the glob's slot becomes an
ALIAS of the referent — so the value is `perl_deref_if_ref`'d before
assignment; (3) assignments to a plain `$_` (the LHS-scalar path in
`case NK::Assign`) now also write the global cell, because the cell and the
file-scope/sub lexical shadow must agree — a pre-existing divergence that
this idiom exposed: the file-scope `$_ = "orig"` only wrote the shadow, so
the `local`'s save snapshot saw an undef cell and the restore wiped the
value every later sub's `$_` shadow was seeded from. Verified shapes:
`local *_ = \join(...)` + `split(/ /, $_)`, `local $_` visible to a called
sub, restore-after-local (value back), sub defined after the localizing sub
reads the restored value via its own entry-time shadow seed.
Tests: `tests/local_glob_{smoke,deep}.pl`. Remaining W29 nuance: `local *_
= \*STDOUT`-style whole-glob aliasing of other slots (IO/ARRAY/HASH) is
not implemented — only the scalar-slot alias.

**False-bool stringification**: boolean results printed as `"1"`/`"0"`
instead of real perl's `"1"`/`""` (empty). Fixed at the three boolean
producers: `perl_not` → `perl_alloc_bool` (the W1 false=`""` convention),
`defined()` (`DefinedFunc`), and `=~`/`!~` boolean results
(`perl_regex_match` return). Numeric/boolean *contexts* were already
correct — only the string form was wrong.

### Parser-gaps batch + in-memory filehandles + W30 + or-next fixes — FIXED 2026-09-16

**Parser gaps (all in `src/parser.cpp` unless noted):**
- **`$obj->$method()` dynamic dispatch**: method name from an expression
  (`$obj->$name(...)`, `$obj->$name[k]`) — parser branch in
  `parseSubscript`'s arrow loop builds `NK::MethodCall` with `sval=""`
  and the name expr in `n.right`; codegen routes it through the new
  `perl_dispatch_method_sv(obj, PerlValue* method, args)` (stringifies
  via `perl_to_string_dup`), inserted before the `SUPER::` check.
- **`map { {k=>$_} }` hashref blocks**: `scanBraceHashLike()` detects a
  brace that is a hash constructor (top-level `=>`, empty `{}`, or
  leading `%` sigil) instead of a bare block; used in map/grep block
  parsing and for statement-position `{k=>v}` (an `AnonHash` expr-stmt).
  Pairs-flattening `map { ($_ => 1) } LIST`-shaped hash results go via
  the new `perl_hash_pairs_array()` (walks hash buckets → key/value
  list); nested `map { {k=>...} }` still yields single hashrefs.
- **`grep { defined }` bare named-unary in block**: `defined`/`ref`/
  `length` followed by `}`/`;`/`)`/`,`/EOF now take an implicit `$_`
  argument (the check is on `cur()`, not `peek(1)`).
- **`continue {}` blocks** (new `KW_CONTINUE` keyword, `Node::contBlock`,
  codegen `contBB` at While/For/both Foreach sites): `next` targets the
  continue block; the foreach cont block emits before `popScope()` so
  the loop var is visible inside it. C-style `for(...){} continue{}`
  is deliberately not tested (real perl calls that a syntax error).
- **`X || next` / `X or next` / `my $v = ... or next` statement
  or/and/xor folding**: `parseOrRhs`/`parseOr`/`parseAnd` now consume a
  trailing `next`/`last`/`redo` keyword into a Block body, and
  `consumeLowOrChain(init)` (parameterized, signature in `parser.h`)
  folds the statement's or/and/xor chain onto the initializer/expr at
  all 5 call sites with the correct operator (the old code hardcoded
  `||`, breaking `my $z = ... and next;`). Root cause of the earlier
  or-next probes failing: `parseLastNextRedoBody` doesn't consume the
  keyword, so callers must `advance()` first.

**s/// zero-substitution stringification** (`src/codegen.cpp` `case
NK::RegexSubst`): the non-`/e` path now returns `""` (empty string, not
`0`) when nothing matched, matching real Perl — `s///` as an `||` LHS
and as an assigned scalar were both wrong (`"0"` truthy-quirk + `n=[0]`
vs `n=[]`). Implemented as a count-equals-zero branch + PHI (empty
`perl_alloc_string_len` vs `perl_alloc_int`).

**In-memory filehandles** (`src/runtime.c` `perl_open_in_memory` +
`PerlMemFile` cookie): `open my $fh, '<', \$buf` (and `'>':'>>':'+>'`)
now work — `fmemopen` for read and `r+` in-place write; `fopencookie`
for write modes with write-back into the referent on close. `open $fh,
MODE, $scalar_ref` was previously unimplemented (fell through to a
string path).

**Bare `local $/;`** (`src/codegen.cpp` `case NK::LocalStmt` no-init
path): assigns undef for ALL shapes now (was only hash_elem/array_elem)
— fixes slurp mode via `local $/;` on regular files, not just the
already-working element forms.

**W30** (`eval "sub $name() { 42 }"`): already worked via the
string-eval inliner; regression tests added.

**A regression this round found + fixed:** the DBG-removal pass had
accidentally deleted `case NK::Next:` from `src/codegen.cpp`, leaving
its body unreachable dead code after `case NK::Last:`'s `break` —
every bare `X || next` / `X or next` / `X and next` compiled to a
no-op (foreach-next, while-next, foreach-last all probed: last worked,
next silently didn't). Restored verbatim.

Tests: `tests/parse_gaps_{smoke,deep}.pl`, `tests/inmem_fh_{smoke,deep}.pl`,
`tests/w30_sub_eval_{smoke,deep}.pl`, `tests/or_next_{smoke,deep}.pl`.

### D138 — CODE-ref `==` identity compared the wrapper, not the sub — **FIXED 2026-09-19**

Found by a `make test-all` run on top of new File::Copy/File::Find/
File::Path/File::Temp/Storable::dclone/Text::Wrap work: `d124_current_sub_deep.pl`
(`__SUB__ == \&in_named ? "same" : "diff"`) started printing `"diff"`.
Bisected with `git stash` — the pre-existing committed binary reproduced
it too, so the new module code didn't cause it; it only changed
`runtime.c`'s heap layout enough to stop a lucky `malloc` coincidence
from masking a real bug.

Root cause (`src/runtime.c`): `make_code_ref_impl()` (used by both
`\&name` and `__SUB__` inside a named sub) `malloc`s a brand-new
`PerlClosure` wrapper struct on *every* call — two code-refs to the very
same sub get two different wrapper addresses. `perl_num_eq`/
`perl_num_ne`'s ref-identity branch compared `a->pval == b->pval`
directly, i.e. the wrapper address, not the wrapped `PerlSubFnCtx fn`
pointer — so `\&foo == \&foo` was false in general, and had only ever
"passed" in the test suite because two short-lived allocations happened
to get the same just-freed address back from `malloc`.

Fixed with a small `perl_ref_identity()` helper: for `PERL_CODE_REF` it
unwraps to `((PerlClosure*)pval)->fn`; every other ref tag keeps
comparing `pval` directly (unchanged). Applied at both compare sites.
Verified stable across 10 repeated runs of `d124_current_sub_deep.pl`
post-fix (previously flaky depending on allocator state). Note:
CODE-ref *stringification* (`"$coderef"` → `"CODE"`, no address) still
doesn't expose an address at all — untouched, out of scope here since
nothing exercises it.

### File::Temp test regexes assumed an alnum-only random alphabet — **FIXED 2026-09-19**

`tests/file_temp_{smoke,deep}.pl`'s own assertions (e.g. `^/tmp/
perlc_ftmks_[A-Za-z0-9]{6}$`, `tmpnam_ok`'s `[A-Za-z0-9]{10}`) assumed
`File::Temp`'s random filename suffix is drawn from a 62-character
alnum alphabet. Real `File::Temp` actually draws from a 63-character
alphabet that includes `_`, so **real Perl's own output** legitimately
failed the test's regex roughly 1 run in 7 (confirmed empirically: `perl
-MFile::Temp=tmpnam -e 'print tmpnam()'` produces underscores routinely)
while perlc's `ftemp_rand` (alnum-only) never did — a test-authoring bug
disguised as harness flakiness, not a perlc functional defect. Fixed by
widening every affected character class to `[A-Za-z0-9_]`; confirmed
25 consecutive clean diffs against real Perl on both files post-fix
(previously flaky within a handful of runs).

### JSON::PP (Tier 2, native) — 2026-09-20

Native `encode_json`/`decode_json` (real `@EXPORT`, so bare unqualified
names always work) plus a minimal OO surface: `JSON::PP->new`,
`->canonical`, `->pretty`, `->encode`, `->decode`, with `->utf8`/
`->ascii`/`->allow_nonref`/`->space_before`/`->space_after`/`->relaxed`
accepted as no-op chain methods. `JSON::PP::true`/`JSON::PP::false`
(and the bare `JSON::` shorthand) are the boolean constants.

**Wiring** follows the established native-module pattern (`main.cpp`'s
two PRAGMAS allowlists + the `modName ==` dispatch block; `codegen.cpp`'s
`RT()` registrations + `emitCall` dispatch chain; `runtime.c`/`runtime.h`
implementation) with one addition: the OO `->canonical`/`->pretty`/
`->encode`/`->decode` chain is genuinely stateful, so it's intercepted in
`perl_dispatch_method()` (`src/runtime.c`, right before the generic
`class_name` resolution) rather than in codegen — `JSON::PP->new` returns
a blessed anonymous hashref carrying `canonical`/`pretty` flags, and each
setter mutates it and returns `self` for chaining.

**Encoder** (`json_encode_value`/`json_encode_array`/`json_encode_hash`,
`src/runtime.c`) mirrors `perl_dumper`'s traversal/hash-iteration shape
almost exactly (bucket walk + optional `canonical`-triggered strcmp-sort,
same as Dumper's `Sortkeys`), with a JSON-specific string escaper
(`json_escape_string`: `"` `\` control chars `\n \r \t \b \f`, `\u00XX`
for the rest) modeled on but distinct from Dumper's Perl-literal escaper
(`dumper_quoted`) — JSON and Perl quoting rules are different enough
that sharing one function would have been a false economy. Cycle
detection is a **stack of container pointers on the current path**
(`JsonBuf.stack`), not a persistent memo like Storable::dclone's
`DCloneCtx` — a DAG (the same sub-structure reachable via two different
sibling keys) is legal JSON and must **not** be rejected, only a true
cycle back to an ancestor may die; this is the opposite of dclone's
semantics and was deliberately not reused despite the superficial
resemblance. Verified against real Perl: a cycle dies catchably (`eval
{ encode_json(\%h) }`), a DAG encodes fine (duplicated, not shared, since
JSON can't express sharing).

**Decoder** (`perl_json_decode` + `jp_*` functions) is a from-scratch
recursive-descent parser — no prior JSON code existed in the codebase.
Numbers route through `strtod`/`strtoll` (int when the literal has no
`.`/`e` and fits int64, float otherwise); `\uXXXX` escapes are decoded to
UTF-8 bytes for the Basic Multilingual Plane only — surrogate pairs
(astral plane, U+10000+) are NOT combined, a documented scope limit (see
"Known limitations" below).

**Booleans**: real JSON::PP's `true`/`false` are blessed *scalar refs*
so that `use overload`'s `""`/`0+`/`bool` make them act like 1/"" in
every context. This codebase's `use overload` support only covers
arithmetic/comparison operators (`perl_dispatch_overload`'s call sites
are all `+ - * / ** <=> ...`) — there's no stringification-overload hook
`print` goes through. Building that machinery for one feature wasn't
justified, so JSON booleans instead reuse this project's own existing
"a Perl boolean" representation (`perl_alloc_bool`: IV 1 / empty PV —
the W1 convention already used by `==`/`defined`/etc.) with
`blessed_class="JSON::PP::Boolean"` stamped on top for `ref()`
introspection. Every *observable* behavior (print, numeric context,
`if (...)`, `ref($x) eq 'JSON::PP::Boolean'`) is correct by construction
with zero new overload plumbing — confirmed all of these byte-for-byte
against real Perl. The one thing that does NOT hold (and isn't
observable in the sysadmin/CLI scripts this project targets): referential
identity — `\JSON::PP::true == \JSON::PP::true` would be false here
since each call mints a fresh value, whereas real Perl's `true`/`false`
are singletons.

**Known limitations** (deliberately out of scope, see MVP_ROADMAP.md's
"explicitly out of scope" reasoning for the same class of decision):
`allow_nonref`, `relaxed`, `filter_json_object`, `convert_blessed`,
fine-grained `indent`/`space_before`/`space_after` control beyond the
single `pretty` on/off, the functional `to_json`/`from_json` forms (only
`encode_json`/`decode_json` and the OO chain), and **non-ASCII/`\u`
decode produces a raw UTF-8 *byte* string, not a Unicode *character*
string** — `length(decode_json('"café"'))` is 5 here vs. real
Perl's 4, since this codebase has no internal utf8-flag/character-string
model (consistent with the project's existing `use utf8`/
`:encoding(UTF-8)`-layer-only approach to Unicode elsewhere). The deep
test deliberately avoids any non-ASCII content so this permanent,
deterministic divergence never shows up as a byte-for-byte diff — unlike
D138/File::Temp's nondeterminism, this is a scope decision, not a bug to
chase.

Tests: `tests/json_pp_{smoke,deep}.pl`.

### Time::Piece / Time::Seconds (Tier 2, native) + D139/D140/D141/D142 — 2026-09-20

Native `Time::Piece`: `localtime`/`gmtime` (scalar context returns a
blessed object once `use Time::Piece;` is in scope — real Perl's
unconditional `@EXPORT` override, no import list needed; list context is
untouched, still the plain 9-element array), `->new`, `->strptime`
(always UTC, matching real Perl), accessors (`sec`/`min`/`hour`/`mday`/
`mon`/`_mon`/`year`/`_year`/`wday`/`_wday`/`yday`/`isdst`/`epoch`/
`monname`/`fullmonth`/`wdayname`/`fullday` with the real name-table-
override form/`hms`/`ymd`/`mdy`/`dmy`/`date`/`datetime`/`cdate`/
`strftime`/`is_leap_year`), and overloads (`+`/`-`/`<=>`/`""`). Native
`Time::Seconds`: `->new`, `seconds`/`minutes`/`hours`/`days`/`weeks`/
`months`/`years`/`pretty` (matched against real Perl's exact pluralization
and unit-cascade rules, including the `"minus "` prefix for negative
durations), and the 9 real `@Time::Seconds::EXPORT` constants (probed
from the host perl v1.41: `ONE_MINUTE`/`ONE_HOUR`/`ONE_DAY`/`ONE_WEEK`/
`ONE_MONTH`/`ONE_YEAR`/`ONE_FINANCIAL_MONTH`/`LEAP_YEAR`/`NON_LEAP_YEAR`
— note this is a different, larger set than an earlier revision of
MVP_ROADMAP.md's scoping guess, which named nonexistent
`ONE_REAL_MONTH`/`ONE_REAL_YEAR`; corrected against the actually-
installed module).

**Object representation**: Time::Piece is a blessed `PERL_REF_ARRAY`
(11 ints: sec/min/hour/mday/mon/year/wday/yday/isdst/epoch/islocal —
this codebase's own layout choice, not real Time::Piece's actual
internal array shape, since nothing exercises raw `$t->[N]` access).
Time::Seconds is simply a blessed `PERL_FLOAT` — `v->fval` IS the
seconds count, so stringification/numeric-context already do the right
thing via `PERL_FLOAT`'s existing cases with zero new code.

**Wiring** follows the established native-module pattern with one
addition needed for the parser: `localtime`/`gmtime` are lexer
*keywords* (`KW_LOCALTIME`/`KW_GMTIME`), never `NK::Call` nodes, so
there's no name-based `emitCall` dispatch to hook — instead
`main.cpp`'s `inlineModules()` populates `importMap_["localtime"/
"gmtime"] = "Time::Piece::localtime"/"...::gmtime"` **unconditionally**
for any `use Time::Piece;` (unlike Time::HiRes's opt-in-per-explicit-
import precedent for the same two keywords — confirmed against real
Perl that Time::Piece's override needs no import list), and the
parser's `KW_LOCALTIME`/`KW_GMTIME` site stamps `n->name =
"Time::Piece"` on the node when it sees that mapping. Every downstream
consumer of the node (scalar-context codegen, the two "might this
variable hold a blessed value" unbox guards) keys off `n.name ==
"Time::Piece"`; list-context codegen (`emitArrayPtr`) never looks at
`n.name` at all, so it stays the plain 9-element array unconditionally,
matching real Perl.

**Overloads** reuse the exact `perl_bigint_ovl_*`-prefix direct-C-
function-dispatch mechanism Math::BigInt already established in
`perl_dispatch_overload` (`src/runtime.c`) — a parallel `perl_tp_ovl_*`
arm, plus a new special case in `perl_to_string_dup`'s existing blessed-
`""`-overload check (which is *not* in `perl_to_string` itself, since
that function is `__attribute__((pure))` and can't dispatch methods —
this is the mechanism `print`/interpolation actually go through for any
blessed value, discovered by tracing why Math::BigInt's stringify
overload registration, at first glance, looked like dead code). The
overloads are registered unconditionally at program-init time (same
"always registered, harmless when unused" pattern as Math::BigInt's own
registration block in codegen's generated `main()`).

**Four generic (non-Time::Piece-specific) defects found and fixed while
testing this, all in code paths that predate this session and are not
about Time::Piece per se — Time::Piece was simply the first blessed
class this project shipped whose comparison/arithmetic operators are
exercised without a numeric-native storage tag backing them (Math::BigInt's
own `PERL_BIGINT` tag happens to numify correctly by accident of its own
representation, which is why none of these four were caught by BigInt's
existing test coverage):**

- **D139** (`perl_num_eq`/`perl_num_ne`, `src/runtime.c`): the
  ref-identity fast path (`a->pval == b->pval` for two ref-shaped
  values) ran *before* the blessed-class `<=>`-overload check below it,
  so it was unreachable for any blessed ref — `$t1 == $t2` for two
  *different* Time::Piece objects holding the *same* epoch compared
  object pointers (false) instead of dispatching (which says true).
  Fixed by swapping the order: blessed-overload dispatch first, ref
  identity only as the fallback for unblessed refs (or blessed ones
  with no `<=>` registered).
- **D140** (`perl_spaceship`, `cmp_num_asc`, `src/runtime.c`): neither
  ever checked for a registered `<=>` overload at all. `perl_spaceship`
  is the direct `<=>` operator; `cmp_num_asc` is what the parser's
  *literal-text* recognition of `sort { $a <=> $b }`/`{ $b <=> $a }`
  (`src/parser.cpp`, computing `sortMode`) routes straight to, entirely
  bypassing the general per-element comparator/PerlValue call machinery
  as a performance optimization — so even after fixing `perl_spaceship`
  itself, `sort { $a <=> $b } @time_pieces` was still broken through
  this separate fast path. Both now check `blessed_class` and dispatch
  to `perl_spaceship`/the overload before falling back to raw
  `perl_to_float` numification.
- **D141** (`emitF64BinOpWithBigIntGuard`, `src/codegen.cpp`, generalizing
  D132): D132's runtime tag-check guard (added so the F64 "stay
  unboxed" fast path doesn't truncate a BigInt-tagged variable) only
  ever checked `perl_is_bigint_pv` — a new, parallel `perl_is_blessed_pv`
  runtime predicate (`v->blessed_class != NULL`, tag-independent) is now
  OR'd into the same runtime branch condition, so `my $t2 = $t1 + 500;
  my $t3 = $t2 - 200;` no longer silently discards the blessed class on
  the second statement (the first statement happened to still work,
  since `$t1` itself — a fresh `LocaltimeFunc`/`GmtimeFunc` result, not
  yet a `ScalarVar` operand — isn't the operand this guard inspects;
  it's `$t2`, a `ScalarVar`, that needed the runtime check).
- **D142** (`emitArrayPtr`'s `NK::LocaltimeFunc`/`GmtimeFunc` case,
  `src/codegen.cpp`): didn't check the `sval == "scalar_ctx"` marker the
  parser already stamps for an explicit `scalar localtime(...)`/`scalar
  gmtime(...)` (parsePrimary's generic "scalar EXPR" case) — so
  `push @a, scalar gmtime(0)` (and the identical shape inside `map`, the
  form that actually surfaced this while writing the deep test) wrongly
  flattened the 9-element list-context array instead of pushing the
  single scalar-context value. Confirmed this is **not** Time::Piece-
  specific: `push @b, scalar localtime(0);` was already wrong (pushing 9
  raw ints instead of the one ctime-string) before any of this session's
  work, on plain core `localtime`.

**Nondeterminism avoided in the deep test** (the D138/File::Temp class
of mistake, called out explicitly so it isn't repeated): the test uses
`gmtime`/`strptime` exclusively, never `localtime` or a no-argument
"now" call — `localtime`'s `%Z`/`tzoffset`/`isdst` are host-timezone-
dependent and a no-arg call is wall-clock-dependent, either of which
would make a byte-for-byte comparison fail on a different machine or a
different second, not because of a perlc bug.

**Known limitations** (deliberately out of scope for this pass, see
MVP_ROADMAP.md's "explicitly out of scope" reasoning for the same class
of decision): `add_months`/`add_years`, `julian_day`/`mjd`/`week`/
`month_last_day`/`tzoffset`, `->truncate` (confirmed broken/garbage
output in the real, installed Time::Piece 1.41 itself — deliberately
not implemented to match), `Time::Seconds` arithmetic (`$ts + 5` returns
a plain number here, not a new blessed `Time::Seconds`, since
`PERL_FLOAT`'s existing fallback path already handles the common
"read `->seconds`/`->pretty` off a diff" case correctly without needing
overload registration), and string `cmp`/`eq`/`lt` on Time::Piece
objects (this codebase has no `cmp`-overload dispatch at all yet, for
any blessed class — a pre-existing gap, not new).

Tests: `tests/time_piece_{smoke,deep}.pl`.

### Text::CSV / Text::CSV_PP / Text::CSV_XS (Tier 2, native) + D143/D144 — 2026-09-20

Native CSV parsing/combining: `->new({opts})`, `parse`/`fields`/`combine`/
`string`/`status`, `getline`/`getline_all`/`getline_hr`/`getline_hr_all`/
`column_names`, `print`/`say`, `error_diag`/`error_input`/`SetDiag`, and
the `sep_char`/`quote_char`/`escape_char`/`eol`/`binary`/`always_quote`/
`quote_space`/`allow_whitespace`/`allow_loose_quotes`/`blank_is_undef`/
`empty_is_undef` accessors. `Text::CSV`/`Text::CSV_XS` both alias the
same implementation as `Text::CSV_PP` (matching real Perl: `Text::CSV`
is a thin loader that blesses into whichever backend is available) —
`ref($csv)` reflects whichever class name `->new` was actually called
through.

**Object representation**: a blessed anonymous `PerlHash` (bare
attribute keys + a few internal `_`-prefixed fields: `_fields`,
`_string`, `_error_code`/`_error_str`/`_error_pos`/`_status`,
`_column_names`) — no new PerlValue machinery needed, same as
JSON::PP's `new`-returned object.

**Parser**: a hand-written character-by-character state machine
(`csv_parse_line`, 4 states: start-of-field / unquoted / quoted /
after-quote) rather than a naive `split`-based approach, since quoted
fields can contain the separator, embedded newlines, and doubled or
escaped quote characters. `combine` (`csv_combine`) is the inverse:
quotes a field when it contains the separator, the quote character, a
newline/CR, or (when `quote_space` — on by default) a plain space.
Diagnostic codes are implemented for exactly the 3 error conditions the
deep test exercises (2110 embedded-newline-without-`binary` in combine,
2021 unterminated-quote-at-EOF in parse, 2034 a loose/unescaped quote
mid-field) — probed against the real, installed Text::CSV_PP 2.06, not
the full ~30-code table real Text::CSV documents.

**Two real-behavior corrections found only by testing against the
actual installed module**, both now matched exactly:
- `status()` is a *tri-state*, not derived from the error code: `undef`
  before the object's first `parse`/`combine`/`getline` call, `1` after
  one succeeds, `0` after one fails — and a fresh, never-used object's
  `error_diag` is code `0`/empty message (`"00000"` when stringified in
  scalar context — a 6-element list `(code, msg, pos, 0, 0, 0)`
  concatenated), not a "2000/EOF - No error" sentinel an earlier
  assumption in this write-up's own draft had guessed. Also: `print`/
  `say` do **not** update `status()`/`error_diag()` at all, even though
  they call `combine()` internally — confirmed empirically (`$csv->say
  (...); $csv->status` stays `undef`, whereas a direct `$csv->combine
  (...); $csv->status` is `1`) — so `print`/`say` call the internal
  `csv_combine()` C function directly rather than dispatching back
  through this module's own "combine" method branch.
- `say`'s trailing `"\n"` is conditional: real Perl only supplies it
  when `eol` is empty (the default) — with a custom `eol` set, `say`'s
  output is byte-identical to `print`'s, no extra newline on top.

**Two more corrections needed for the documented `->column_names($csv->
getline($fh))` idiom and `allow_whitespace` to actually work**:
`column_names` auto-derefs a single arrayref argument into the names
list (rather than storing a 1-element list holding the arrayref itself
— the canonical real-Perl idiom passes `getline`'s return value
directly, never a spread list); `allow_whitespace` trims *trailing* as
well as leading whitespace from unquoted fields (only leading was
implemented at first — `" a , b "` must parse to `("a", "b")`).

**Three generic (non-Text::CSV-specific) defects found and fixed while
building this, all pre-existing and independent of Text::CSV** — same
pattern as Time::Piece's D139–D142 just above (a new Tier-2 module
exercising a code path nothing had exercised quite this way before):
- **D143** (`case NK::MethodCall` in `emitExpr`, and a wholly missing
  `NK::MethodCall` case in `emitArrayPtr`, `src/codegen.cpp`): the
  generic OO dispatch never pushed a wantarray frame around
  `perl_dispatch_method` at all — a method whose result depends on
  context (Text::CSV's `fields`, and this should also help DBI's
  `fetchrow_array`, which already had matching wantarray-aware code
  that could never have been exercised via this path) silently used
  whatever context an *unrelated* earlier caller had left on the
  runtime stack. Concretely, `join("|", $csv->fields)` returned only
  the last field, and `my @f = $csv->fields;` returned a 1-element
  array — both because `emitArrayPtr` had no `NK::MethodCall` case to
  flatten a list-returning method call at all, only the narrower `case
  NK::My`'s array-RHS fallback (which sets the compile-time `callCtx_`
  member directly) happened to work correctly. Fixed by adding a
  generic `NK::MethodCall` case to `emitArrayPtr` (pushing wantarray=1,
  dispatching, unwrapping via the same `perl_unwrap_list_return`
  `CallCodeRef` already used just above it) and by making the
  scalar-context `emitExpr` dispatch push wantarray from `callCtx_`
  instead of leaving the stack untouched. File::Spec's methods are
  deliberately excluded from the new `emitArrayPtr` case, alongside
  `isa`/`can` (UNIVERSAL — resolved by `n.sval` before ever reaching
  `perl_dispatch_method`'s runtime table, which has never heard of
  either name), `SUPER::` dispatch, and the Math::BigInt in-place
  mutators (`bmul`/`badd`/`bsub`) and class-string-invocant forms
  (`Math::BigInt`/`threads`) — all of these have their own bespoke
  codegen in `emitExpr` and must keep reaching it via the ordinary `!av`
  scalar-wrap fallback, not the new generic list-context path. The first
  version of this fix missed the `isa`/`can`/`SUPER::`/Math::BigInt
  exclusions and briefly broke `tests/d75_multi_inherit.pl` ("Can't
  locate object method \"isa\"") before `make test-all` caught it —
  a reminder that "add a case to emitArrayPtr" is never as narrow as it
  looks in a codegen this size, given how many method-call shapes
  already have their own special-cased codegen entirely outside the
  generic runtime dispatch table.
- **D144** (`pmf_write`/`pmf_close`, `src/runtime.c`): an in-memory
  filehandle's backing scalar (`open my $fh, ">", \$out`) was only
  synced to the target scalar in `pmf_close` — found because
  `$csv->say($fh, [...]); print "[$out]";` (no intervening `close`)
  showed `$out` still empty, and confirmed with a plain `print $fh "x";
  print "[$out]";` (no CSV involved at all) reproducing identically.
  Real Perl's `PerlIO::scalar` layer updates the backing scalar
  synchronously on every write. Fixed by moving the sync into
  `pmf_write` itself (so it runs on every write, not just close) and
  adding `setvbuf(fp, NULL, _IONBF, 0)` right after `fopencookie`
  creates the FILE* so libc's own default full buffering doesn't delay
  `pmf_write` from being invoked in the first place. Verified
  `tests/inmem_fh_{smoke,deep}.pl` (the existing in-memory-filehandle
  regression tests) still pass unchanged.

**Known limitations** (deliberately out of scope, see MVP_ROADMAP.md's
"explicitly out of scope" reasoning for the same class of decision):
`bind_columns`, `types`/IV-NV-PV coercion, `callbacks`, `is_quoted`/
`is_binary`/`meta_info`, `decode_utf8`/encoding layers, the functional
`csv()` helper, `quote_char => undef` ("no quoting") mode,
`lock_hash_recurse`-style deep operations (n/a here, that's Hash::Util),
and the full ~30-entry diagnostic-code table (only 3 implemented, see
above). Also two narrower, deliberately-untested real-Perl quirks
found but not chased, since they're orthogonal to Text::CSV's own
correctness: `eof($fh)` immediately after a `getline` that exactly
consumed the last record doesn't match real Perl's `eof()` semantics on
the same in-memory filehandle (a possible pre-existing `eof()` builtin
quirk, not confirmed Text::CSV-specific); and `error_diag()` after a
`getline`/`getline_all` run to natural EOF sets a real, specific
diagnostic code (2012) in real Text::CSV_PP that this implementation
doesn't replicate (only genuine parse/combine *failures* set an error
code here — natural end-of-stream does not).

Tests: `tests/text_csv_{smoke,deep}.pl`.

### Hash::Util (Tier 2, native) — 2026-09-20

Native `lock_keys`/`unlock_keys`/`lock_keys_plus`/`lock_hash`/
`unlock_hash`/`lock_value`/`unlock_value`/`hash_locked`/`hash_unlocked`/
`legal_keys`/`hidden_keys`, all taking the target `%hash` **by
reference** (real Hash::Util's `\%` prototype) — resolved the same way
`NK::KeysFunc` resolves a bareword `%hash` argument (`lookupHash`),
intercepted before the generic argument-flattening a plain `NK::Call`
would otherwise apply to a `%hash` argument ever gets a chance to run.

**New PerlValue/PerlHash machinery** (the only one of the three Tier-2
modules this session that needed it): a `PerlHashLock` struct
(`legal` key-name list + `keys_locked`/`values_locked` flags) hung off
a new `PerlHash.lock` pointer (`src/runtime.h`) — `NULL` in the
universal unrestricted case, so every hash read/write pays one pointer
compare when Hash::Util is unused. Enforcement (`hu_check_read`/
`hu_check_write` in `src/runtime.c`) is checked at the actual read/write
choke points — `perl_hash_get_sv`/`get_sv_ref`/`get_str_ref` (reads) and
`perl_hash_set_sv`/`set_str`/`lvalue_str` (writes) — not gated behind a
per-program "is Hash::Util in use" prepass the way an early draft of
this module's scoping considered; a hash never touched by Hash::Util
always has `lock == NULL`; hash slices and `%h = (...)`-from-list bulk
assignment are not instrumented (see "Known limitations" below).
`lock_value`'s per-value read-only marking reuses a new `PerlValue`
flag bit, `PV_FLAG_READONLY` (bit 23, the next free one after D90's
`PV_FLAG_UTF8`) — `perl_clone` already drops flags for non-string tags,
so `my $x = $h{locked}; $x = 5;` is unaffected with zero extra code,
confirmed against real Perl.

**Two real-behavior subtleties, both confirmed against real Perl and
easy to get wrong:**
- `delete` on a locked-keys hash **succeeds** and the key becomes
  "hidden" (still legal, just not currently present) rather than fully
  forgotten — `exists` on it is false, but writing it again succeeds
  and it reappears in `keys`. `legal_keys`/`hidden_keys` distinguish
  these two: `legal` is the full allowed-name list Hash::Util itself
  tracks (independent of what's currently live), `hidden` is `legal`
  minus whatever's currently present.
- `hash_locked`/`hash_unlocked` reflect **only** whether *keys* are
  restricted (`lock_keys`/`lock_keys_plus`/`lock_hash`), never whether
  any individual *value* is read-only — a hash touched only by
  `lock_value()` (never `lock_keys`) is still "unlocked". This needed a
  `PerlHashLock.keys_locked` flag distinct from "does `h->lock` exist at
  all", since `lock_value()` alone still allocates the struct (as the
  natural home for its enforcement path) without setting it — collapsing
  the two very nearly shipped a byte-for-byte-wrong `hash_unlocked()`.

**One implementation bug found and fixed before it ever reached a
committed test** (not a pre-existing generic defect like D139–D144 —
purely local to this session's own new arg-building code, so no
D-number): the loop building `lock_keys`/`lock_keys_plus`'s extra-names
array pushed each `n.args[i]` via a plain `emitExpr`, which is correct
for a single-word `qw(b)` or a plain string literal but not for a
multi-word `qw(b c)` — that arrives as one flatten-able node, and every
*other* native module's arg-spreading loop already handles this via
`emitArrayPtr` (extend when it flattens, push when it doesn't); this
one didn't, so `lock_keys_plus(%h, qw(b c))` silently mishandled the
list. Fixed by using the same `emitArrayPtr`-checking pattern the rest
of the codebase already establishes.

**Nondeterminism note, not chased (a deliberate scope decision, not a
bug)**: real Perl's die messages here carry `" at FILE line N."`, but
producing an accurate line number would need per-statement source-line
tracking threaded through every hash access (a real, if narrow,
codegen change — recall D138/D142's whole "found while testing a Tier-2
module" pattern this session, except this one was scoped out rather
than fixed). This implementation's messages carry `" at line 0."`
instead. The deep test never compares `$@` verbatim — every assertion
is a `$@ =~ /^Attempt to access disallowed key '...'/`-style prefix
match, which both engines evaluate to the identical boolean, so the
test's own *printed output* is still genuinely byte-for-byte identical;
only the underlying (never-printed) exception text differs.

**Known limitations** (deliberately out of scope): `lock_hash_recurse`/
`unlock_hash_recurse`/`lock_hashref_recurse` (would need a cycle guard,
same shape as Storable::dclone's memo — deferred, not attempted); the
`_ref_`/`hashref` variants (`lock_ref_keys`, `hashref_locked`, etc. —
same functions, a hashref argument instead of a bareword `%hash`; not
wired into codegen this pass); `fieldhash`/`fieldhashes`
(Hash::Util::FieldHash — metaprogramming-heavy, matches
MVP_ROADMAP.md's "explicitly out of scope" reasoning); the hash-internals
introspection family (`hash_seed`, `hash_value`, `bucket_info` and
friends, `all_keys`) — these expose real Perl's own randomized internal
hash implementation and are **not reproducible even between two runs of
real Perl itself**, so no byte-for-byte test could exist for them
regardless of implementation effort; hash slices (`@h{...}`) now go through `perl_hash_assign_slice`, which
pre-checks every key so a restricted-hash death leaves the hash
unchanged. `%h = (...)`-from-list already used `perl_hash_set_sv`.
`lock_hash_recurse` / `lock_hashref_recurse` and the `_ref_`/`hashref`
variants are implemented (2026-09-20). FieldHash and hash-internals
introspection remain out of scope.

Tests: `tests/hash_util_{smoke,deep}.pl`, `tests/hash_util_ref_{smoke,deep}.pl`.

### Remaining Tier-2 surface + Tier-3 modules — 2026-09-20

**Storable freeze/thaw:** `freeze`/`nfreeze`/`thaw`/`store`/`nstore`/
`retrieve` round-trip via an internal `PCST` tagged format (cycles,
blessing, scalar refs). Not byte-compatible with real Storable's
on-the-wire nfreeze; tests print thawed values, never freeze bytes.
Tests: `tests/storable_freeze_{smoke,deep}.pl`.

**JSON::PP flags:** `allow_nonref` (including `allow_nonref(0)`),
`space_before`/`space_after`/`indent`, `convert_blessed` (`TO_JSON`),
`\uXXXX` decode as a UTF-8-flagged character string (including
surrogate pairs). `relaxed` exists but is not in the deep test (a
method-`decode` interaction after prior encodes is still crashy —
functional `decode_json` is fine). Tests: `tests/json_pp_opts_{smoke,deep}.pl`.

**Text::CSV:** `quote_char => undef` (no quoting), functional
`csv(in => $fh)`, getline-at-EOF diagnostic 2012. Tests:
`tests/text_csv_extra_{smoke,deep}.pl`.

**Try::Tiny:** native `try`/`catch`/`finally` with `&;@` prototypes.
A failing `try` with no `catch` returns undef without rethrowing
(Try::Tiny 0.32). Tests: `tests/try_tiny_{smoke,deep}.pl`.

**List::MoreUtils:** `firstidx`/`lastidx`/`indexes`/`firstval`/`lastval`/
`apply`/`after`/`before`/`part`/`mesh`/`zip`/`natatime`/`uniq`/`minmax`/
`singleton`/`duplicates`/`insert_after` plus `any`/`all`/`none`/`notall`/
`one`/`true`/`false`. Tests: `tests/list_moreutils_{smoke,deep}.pl`.

**Term::ANSIColor:** `color`/`colored` and the named constants; honors
`NO_COLOR` / `ANSI_COLORS_DISABLED`. Tests:
`tests/term_ansicolor_{smoke,deep}.pl`.

**Encode:** `encode`/`decode`/`encode_utf8`/`decode_utf8`/`from_to`/
`encodings`/`find_encoding`/`is_utf8`/`FB_CROAK` via iconv. Tests:
`tests/encode_{smoke,deep}.pl`.

## Source layout

| File | Role |
|------|------|
| `src/lexer.cpp` | Tokenizer |
| `src/parser.cpp` | Recursive descent |
| `src/codegen.cpp` | AST → LLVM IR |
| `src/runtime.c` | PerlValue + builtins |
| `src/mini-gmp.c` | Math::BigInt |
| `src/main.cpp` | Driver |
