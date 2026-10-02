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

## Current state (2026-10-02)

Core language, OOP, regex (PCRE2 including `/x`), threads::shared, overload,
Math::BigInt (mini-gmp), pack/unpack, `do FILE`, string `eval EXPR`,
`syscall()`, and Unix process/IPC/sockets are implemented. Correctness is
gated by `make test-all` (byte-for-byte vs real `perl`).

**Harness (2026-10-02, D176 — 557/557 PASS, 0 FAIL):** A ninth
real-script survey found that a QUOTED heredoc delimiter containing
non-alnum/underscore characters (`<<'!END!'`, `<<"TAG WITH SPACES"`)
was a hard "unexpected token '<<'" parse error — the delimiter scan
enforced the stricter bareword-identifier rule even for the quoted
form, when real Perl allows any character up to the closing quote
there. Found recurring across two separate surveys, in a real
`/usr/lib/.../Config_heavy.pl` — part of Perl's own generated
`Config.pm` support files, not an obscure third-party script:
`our $summary = <<'!END!';`. Fixed in `src/lexer.cpp`'s
`readHeredoc()` by giving the quoted and unquoted delimiter forms
separate scan rules (quoted: any char up to the matching quote;
unquoted: unchanged, alnum/underscore only). Tests:
`tests/heredoc_punct_delim_{smoke,deep}.pl`.
Previous session (2026-10-02, D174/D175 — 555/555 PASS, 0 FAIL):** An eighth
real-script survey found two more parse/lexer gaps. **D174**:
`q(...)`/`qq(...)`/`qx(...)` with a true bracketing delimiter pair
(`()`, `[]`, `<>` — where open and close differ, as opposed to a
symmetric delimiter like `/`) didn't track nested depth at all —
`qq(hello (world) foo)` stopped at the FIRST `)`, not the matching
outer one. The pre-existing `qq{...}` brace scanner already did this
correctly (a separate, hand-rolled loop with its own depth counter),
but the shared `src/lexer.cpp` `readString()` helper used for every
other delimiter only ever received the single close character, with
no concept of the opening one to count against. Found via a real
`/usr/bin/ucfq` script's `qq($main::MYNAME $main::VERSION\n\t
Copyright (C) 2002-2024 )` (an unescaped nested `(C)`). Fixed by
giving `readString()` an optional `openDelim` parameter that enables
the same depth-tracking logic the brace scanner already used, passed
at the two non-brace call sites (`q`/`qq` and `qx`). **D175**: `$]`
(the "oldstyle" decimal Perl version number, distinct from `$^V`'s
dotted-string form) was not implemented AT ALL — not just missing
from string interpolation, but completely broken even as a bare
expression. Found via a real `/usr/bin/gprofng-display-html` script's
`version->parse("$]")->normal`. Needed two independent fixes: (1) the
main lexer/parser — `$]` lexes as a single mutated `SCALAR` token
(mirroring the pre-existing `$+` convention) and needed an explicit
`parsePrimary()` check for that text *before* the generic "advance
past `$`, read the next token as the name" path, which has no second
token to read for this single-token form; (2) the STRING
INTERPOLATION scanner (`parseStringInterp`), a wholly separate
raw-text-based system, needed `]` added to its own
`$.`/`$,`/`$\`/`$&`/`$!`/`$/` special-single-char-variable list.
Tests: `tests/qq_nested_delim_{smoke,deep}.pl`,
`tests/dollar_rbracket_{smoke,deep}.pl`.
Previous session (2026-10-01, D173 — 551/551 PASS, 0 FAIL):** A seventh
real-script survey found that diamond-glob `<PATTERN>`'s pattern text
was NOT variable-interpolated at all — D164's own write-up had
claimed this matched real Perl ("the pattern text is a literal, not
variable-interpolated"), but that claim was wrong/under-verified:
real Perl always interpolates it, confirmed directly (`<$dir/*.txt>`
with `$dir="/tmp"` must glob `/tmp/*.txt`, not the literal string
`"$dir/*.txt"`). Found via a real `/usr/sbin/update-rc.d` script
using the quoted form, `<"$dpkg_root/etc/rc[S12345].d/S[0-9][0-9]
$scriptname">`, which also needed the lexer's glob-pattern safe-char
allowlist widened (it previously aborted its scan on the opening
`"`, falling back to a bare `<` token and a parse error). Fixed by
routing the (optionally quote-stripped — real Perl strips a single
surrounding `"..."`/`'...'` as pure delimiter syntax, confirmed even
the single-quoted form still interpolates) pattern text through
`Parser::parseInterpString()`, the same scanner ordinary `"..."`
literals use, in both the list- and scalar-context codegen sites.
Tests: `tests/diamond_glob_interp_{smoke,deep}.pl`.
Previous session (2026-10-01, D172 — 549/549 PASS, 0 FAIL):** Continuing the
sixth real-script survey. `last`/`next`/`redo` used as an EXPRESSION —
most commonly the trailing operand of a comma expression immediately
followed by an `if`/`unless` modifier (`$x = EXPR, last if COND;`) —
was a hard "unexpected token 'last'/'next'/'redo'" parse error; found
verbatim (the identical source line) in two real scripts,
`/usr/bin/perlbug` and `/usr/bin/perlthanks`:
`$sendmail = $_, last if -e $_;`. Fixed the parse error by adding
`last`/`next`/`redo` to `parsePrimary()` (they were previously only
reachable via full-statement dispatch), reusing the existing
`parseLastNextRedoBody()` helper. **Found while verifying this fix**:
once parseable, `last`/`next` (unlike `redo`, which already had a
case) silently did NOT actually break/continue the loop at all when
reached this way — `case NK::Last`/`NK::Next` existed only in
`emitStmt`, so `emitExpr`'s `default: return perlUndef();` fired when
the comma-expression's `NK::ArrayLit` evaluator called `emitExpr` on
them (not `emitStmt`), silently swallowing the control-flow jump
entirely and falling through to whatever code followed — a real,
silent-wrong-behavior bug, not just a parse error. Fixed by adding
matching `emitExpr` cases mirroring `emitStmt`'s identical label-
lookup logic exactly, following the same already-correct pattern
`NK::Redo`'s own `emitExpr` case used. Tests:
`tests/last_next_redo_expr_{smoke,deep}.pl`.
Previous session (2026-10-01, D170/D171 — 547/547 PASS, 0 FAIL):** A sixth
real-script survey found and fixed two genuine **compiler crashes**
plus a related interpolation bug. **D170**: a `use MODULE ...`
statement with no terminating semicolon before end-of-input made
`main.cpp`'s semicolon-scanning loop in `inlineModules()` walk one
index past the lexer's trailing `EOF_TOK` sentinel (it only ever
checked "not SEMI", never "not EOF_TOK"), so `useEnd` ended up ==
`tokens.size()` — one past the last valid index — and every
downstream `tokens[useEnd]`-based access indexed out of bounds: an
assertion-failure crash in a debug `libstdc++`, undefined behavior
otherwise. Found via a non-Perl `/usr/bin/perldoc` (a plain
shell-script placeholder on this system, not actually Perl) fed to
perlc by mistake — but the bounds bug is real and applies to any
genuinely malformed/truncated `.pl` input, which perlc must reject
gracefully, never crash on. Fixed by also stopping the scan at
`EOF_TOK`. Self-verifying test (not part of the byte-for-byte harness
corpus, following the D128 precedent):
`tests/d170_malformed_use_crash.sh`. **D171**: two defects found via
a real `/usr/bin/deb-systemd-helper` script's
`$opts{'create_links'} //= 1;`. (1) Another genuine **compiler
crash** — `||=`/`&&=`/`//=` on a hash element, array element,
deref'd hash/array element (`$ref->{k}`/`$ref->[i]`), or plain scalar
deref (`$$ref`) as the LHS: `emitLValue()` had no case at all for any
of these (only `DollarAt`/`ScalarVar`/`SymbolicDeref` were handled),
so its `default: return nullptr;` fired — and `NK::CompoundAssign`'s
short-circuit branch created its two LLVM basic blocks *before*
checking whether `emitLValue` succeeded, so the null-return early-exit
left both permanently registered with no instructions and no
terminator, an immediate LLVM verify-error crash on any of these
extremely common LHS shapes. Fixed by adding the four missing
`emitLValue` cases (reusing the same `perl_hash_lvalue_str/sv`/
`perl_array_lvalue`/`perl_deref_scalar` runtime calls the non-compound
assignment path already uses) and reordering so lvalue resolution
happens before block creation — a general, defensive fix for any
future unsupported lvalue kind too, not just these four. (2) Found
while verifying (1): a quoted hash key inside string interpolation
(`"$h{'key'}"`, `"$h{\"key\"}"`) was used as the literal,
quote-included text instead of the quoted string's value, at all four
duplicate interpolation-scanner sites that build a bareword-string key
node. Tests: `tests/compound_assign_lvalue_{smoke,deep}.pl`.
Previous session (2026-10-01, D169 + native Test::More — 545/545 PASS,
0 FAIL):** Same fifth real-script survey. Implemented `Test::More`
as a native module — the single most commonly used Perl testing/TAP
framework (found missing via
`/usr/share/doc/libdbi-perl/examples/unicode_test.pl`'s `use
Test::More;`). `ok`/`is`/`isnt`/`like`/`unlike`/`cmp_ok` (all 10
required operators)/`pass`/`fail`/`diag`/`note`/`done_testing`/
`subtest`, with real-Perl-exact TAP output (`ok N - name`/`not ok N -
name`, the `is`/`cmp_ok` got/expected diagnostic formats, `diag` to
stderr vs `note` to stdout with no-separator multi-arg joining and
per-physical-line `# ` prefixing), both the upfront-plan (`use
Test::More tests => N;`, injected as a synthetic BEGIN-time call so
`1..N` prints immediately like real Perl) and deferred-plan
(`done_testing()`) forms, the real exit-code formula
(`min(failed,254)`, or 255 on a plan/actual-count mismatch), and
`subtest`'s nested `# Subtest: NAME` header + 4-space-indented inner
TAP + single outer `ok`/`not ok` line. Scoped out: `Test::More::UTF8`,
`Test::Exception`, `BAIL_OUT`, `can_ok`/`isa_ok`/`new_ok`,
`TODO`/`SKIP`/`$TODO`, `use_ok`/`require_ok`, `cmp_deeply`, and one
narrow, verified divergence — a FAILING multi-line `subtest`'s outer
diagnostic reports the call site's line, not (as real Perl does) the
closure's last-executed line; doesn't affect pass/fail, TAP numbering,
or indentation. Tests: `tests/test_more_{smoke,deep,plan}.pl` (these
match the repo's `test_*.pl` `.gitignore` rule and need `git add -f`,
same as the pre-existing `tests/test_do_filename.pl`/
`tests/test_require_simple.pl`).
Previous session (2026-10-01, D169 — 542/542 PASS, 0 FAIL):** A fifth
real-script survey found and fixed `local $ref->{key} = val;` /
`local $ref->[idx] = val;` (localizing a hash/array element reached
through a scalar reference, single level only) being a hard
"unexpected token '->'" parse error — the pre-existing `local
$h{key}`/`local $arr[idx]` forms only ever handled a NAMED hash/array
by that exact identifier, with no path for a `->` after the base
scalar variable. Found via a real
`/usr/share/doc/libdbi-perl/examples/perl_dbi_nulls_test.pl` script's
common DBI idiom `local $dbh->{PrintError}=0;` (temporarily
suppressing DBI error reporting for the duration of a block/sub).
Fixed with a new parser branch (`src/parser.cpp`) producing two new
`NK::LocalStmt` `sval` markers (`hash_elem_deref`/`array_elem_deref`,
storing the base ref expression in the otherwise-unused `n.cond`
field) and matching `src/codegen.cpp` branches that dereference it
(`perl_deref_hash`/`perl_deref_array`) instead of looking up a named
container — everything downstream (the actual
save/restore/assign-on-scope-exit machinery) is unchanged and shared
with the existing named-hash/array forms. Tests:
`tests/local_ref_elem_{smoke,deep}.pl`.
Previous session (2026-10-01, D167/D168 — 540/540 PASS, 0 FAIL):** A fourth
real-script survey found and fixed two batches, both via a real
`/usr/bin/json_pp` script. **D167**: a genuine **compiler crash** — a
constant-string `eval "..."` whose inlined body hit a codegen-level
compile-time error (here: a parenless bareword call to an unresolved
name) threw a C++ exception while the eval's setjmp/longjmp LLVM basic
blocks were mid-construction, escaping past the code that would
normally terminate them — a hard "Basic Block ... does not have a
terminator!" LLVM verify-error crash, not eval gracefully catching a
compile error the way real Perl does. Fixed by catching the exception
inside `case NK::EvalBlock` itself (where `resultAlloca`/`endBB` are
in scope) and finishing the eval exactly like a caught runtime `die`
would, with `$@` set to the error text. Found in the same investigation:
perlc unconditionally treated ANY parenless bareword call to an
unresolved name as a hard compile error, but real Perl only does that
when the bareword has an argument (`foo "arg";`) — a bare,
argument-less name (`foo;` alone) silently auto-quotes to a string
instead when it never resolves to a sub; perlc doesn't track `use
strict 'subs'` scoping anywhere, so it now always takes the
more-permissive no-strict real-Perl behavior. **D168**: (1) a CODE
reference was unconditionally FALSE in boolean context —
`perl_is_true()`'s tag switch listed every other reference tag as
always-true but omitted `PERL_CODE_REF` entirely; (2) a much more
severe **use-after-free** found while verifying (1)'s fix let the
script progress further — several native OO "chainable setter"
methods (JSON::PP's `canonical`/`pretty`/etc., Math::BigInt's `bneg`)
returned the literal `obj` pointer instead of a clone, so a
void-context call (`$json->canonical;`, the common case for a
chainable setter) had codegen free the exact PerlValue the caller's
own live variable still pointed to — confirmed via `ref($json)`
returning garbage memory immediately after. Fixed by returning
`perl_clone(obj)` at all four such sites. Tests:
`tests/eval_string_crash_{smoke,deep}.pl`,
`tests/coderef_truthy_{smoke,deep}.pl`.
Previous session (2026-09-30, third real-script survey + D165/D166 —
536/536 PASS, 0 FAIL):** A third randomized ~54-script compile survey
found and fixed two batches. **D165**: `POSIX`'s `:fcntl_h` import tag
wasn't recognized (found via a real `dpkg-genbuildinfo` script,
`use POSIX qw(:fcntl_h :locale_h strftime);`) — added the tag plus
its `S_IS*` mode-testing predicates (`S_ISREG`/`S_ISDIR`/etc. — real
functions, not constants, despite similar `S_IS*`-vs-`S_IxUSR`
naming) and `creat()` as thin C-library wrappers, plus the two
missing constants (`O_ACCMODE`, `O_NOCTTY`) the tag needed. In the
same pass, found and fixed a companion lexer gap: `<< "EOT"` /
`<<~ "EOT"` (whitespace between `<<`/`<<~` and a QUOTED heredoc
delimiter) was a hard parse error, even though real Perl allows it
(it only forbids the *unquoted* bareword form with a space,
`<< EOT`) — found via a real `/usr/bin/linux-version` script.
**D166**: two separate parser/lexer gaps. (1) `-t FILEHANDLE` (isatty
test) didn't work at all — lowercase `t` was simply missing from the
lexer's filetest-operator character set entirely (only uppercase `T`,
for `-T` text-file test, was present), and once that was added, a
*bareword* filehandle operand (`-t STDERR`, no sigil) still wasn't
parseable since the filetest operand parser only knew `$var`/`"str"`/
`$arr[i]` shapes — found via a real `dpkg-preconfigure` script
(`-t STDERR` inside a larger boolean expression). Also fixed: bare
`-t` (no filehandle) must test STDIN specifically per real Perl, not
`$_` like every other bare filetest, and a bare filetest immediately
followed by `?` (ternary) must not try to consume the `?` as an
operand. (2) `do { ... } while/until COND` required a literal `(`
immediately after `while`/`until`, even though the condition is
really just the same unparenthesized statement-modifier form used
everywhere else — found via a real `/usr/bin/perlbug` script
(`} while !((($alt) = grep(...)));`, which starts with `!`, not `(`).
Tests: `tests/fcntl_h_{smoke,deep}.pl`,
`tests/filetest_t_dowhile_{smoke,deep}.pl`.
Previous session (2026-09-30, D164 — 532/532 PASS, 0 FAIL):** Continuing the
second real-script survey, implemented core Perl's "diamond-glob"
`<PATTERN>` syntax (`my @f = <*.md>;`, `(<callgrind.out*>)[0]`) —
`<...>` was only ever recognized as filehandle readline (`<$fh>`,
`<STDIN>`, bare `<FH>`); any non-identifier content (glob
metacharacters, paths) fell through to a bare `<` token and a
confusing downstream parse error. Found via a real
`/usr/bin/callgrind_annotate` script. Fixed in the lexer only — a
guarded second scan (only attempted where a *term* is expected, not
right after a value, so `$a < $b > $c`-shaped comparisons are
unaffected; further restricted to a safe filename/glob character
allowlist so it can't swallow an unrelated later `>` on the same
line) emits the same `TK::READLINE` token the identifier case uses;
codegen already dispatches on whether that text is a pure identifier.
Scoped out: the pattern text is a literal, not variable-interpolated
(real Perl's `<$dir/*.txt>` substitutes `$dir`; perlc's `<...>` takes
the text as-is) — document this if hit again. **Found while building
this**: scalar-context `glob()` (`while (my $f = glob(...))`, and so
also scalar-context `<PATTERN>`) was a pre-existing, serious,
**outright infinite loop** whenever at least one file matched — it
recomputed the full match list and returned element 0 on every call
instead of advancing. Fixed with a per-callsite iterator-state global
(mirroring `state $x`'s existing per-callsite-global mechanism) giving
real readdir-style one-match-per-call-then-undef-then-restart
semantics. Tests: `tests/diamond_glob_{smoke,deep}.pl`.
Previous session (2026-09-30, second real-script survey + D163 + URI::Escape —
530/530 PASS, 0 FAIL):** A second randomized ~50-script compile survey
found and fixed **D163**: a bare `;` immediately after a block-ending
statement (`if (...) { ... };`, `while (...) { ... };`, or just a
stray `;;;` run) was a hard parse error — `parseStmt()` had no
dispatch case for a leading `;` token, so the expression-statement
fallback choked on it. This is extremely common, idiomatic Perl (every
`if (...) { return X };` is this shape) — found via a real
`/usr/share/clang/scan-build-21/libexec/ccc-analyzer` compile. Fixed
with a `parseStmt()` dispatch case that swallows a run of bare `;`
tokens into a no-op `FlatBlock`, no AST growth. Also implemented
`URI::Escape` as a native module (`uri_escape`/`uri_escape_utf8`/
`uri_unescape`, default RFC3986 unreserved-set escaping, custom
string character-class patterns, scoped out: `qr//`-object patterns,
`uri_unescape`'s multi-arg list form, the `%escapes` hash export) —
found via the same survey, verified against the real installed
module. Also fixed a smaller gap found in the same pass: `File::Glob`
didn't recognize the `:globally` import tag (a special pragma-style
tag, not a real `%EXPORT_TAGS` entry in real Perl either — now a
recognized no-op, since perlc's core `glob()` already matches
`bsd_glob`'s corrected D157 behavior) — found via real `/usr/bin/
helpztags`. Tests: `tests/empty_stmt_{smoke,deep}.pl`,
`tests/uri_escape_{smoke,deep}.pl`.
Previous session (2026-09-30, fresh real-script survey + D162 —
524/524 PASS, 0 FAIL):** A randomized 100-script compile survey of real system Perl
scripts found and fixed D162, a batch of defects: (1) a genuine
**compiler bug** — `Text::ParseWords`'s scalar-context `shellwords`/
`quotewords`/`parse_line` produced an invalid LLVM module (a boxed
`PerlValue*` from `perl_array_len` passed straight into `perl_alloc_int`,
which expects a raw `i64`) — a hard compile-time "Call parameter type
does not match function signature!" error, found via a real
`/usr/sbin/pam_getenv` compile. (2) `POSIX`'s native constant-export
mechanism only recognized a handful of hand-picked tags — `:errno_h`,
`:locale_h`, `:sys_wait_h` weren't recognized at all (found via real
`dpkg-genchanges`/`dpkg-buildpackage`); added the full real-Perl tag
lists (175 constants total now) plus the real `errno()`/`setlocale()`/
`localeconv()`/`WIFEXITED`/`WEXITSTATUS`/etc. functions as C-library
wrappers. (3) Found while verifying `errno()`: `$! = N` assignment was a
separate, deeper, pre-existing bug — it never actually persisted past
the next read of `$!`, and never touched the real OS `errno` either;
fixed with a dedicated `perl_set_dollar_bang()`. Tests:
`tests/parsewords_scalar_{smoke,deep}.pl`,
`tests/posix_errno_locale_{smoke,deep}.pl`.
Previous session (2026-09-30, File::Glob/File::Which/Cpanel::JSON::XS +
D156-D161 — 522/522 PASS, 0 FAIL, 2 more skipped-by-default):** Native `File::Glob`
(`bsd_glob`, `:glob`/`:bsd_glob` tags, `GLOB_*` constants) and `File::Which`
(`which`/`where`, with real Perl's non-obvious context-sensitivity: `which`
itself returns every match in list context, not just the first; `where` in
*scalar* context returns a match count, not a path — verified against the
real installed modules, not guessed). `Cpanel::JSON::XS` aliases the
existing native JSON::PP implementation (not installed on this dev machine
— verified by cross-checking the identical script's output through the
already-real-Perl-verified JSON::PP path instead; its two smoke/deep tests
are in `tests/harness.sh`'s `SKIP_BY_DEFAULT` list and self-check rather
than diff against a real-Perl run that would itself fail to load the
module). Found and fixed seven real, mostly pre-existing defects while
building these (D156-D161; see TESTS.md for full write-ups) — most
severe: **D159**, scalar-context `split()` (`my $n = split ...`) never
worked at all (silent wrong data everywhere, an outright **segfault** at
file scope) — and **D160**, `local $ENV{KEY} = VAL` was a complete,
silent no-op (`%ENV` has no real backing hash under the codegen's usual
lookup, so the generic local-element save/restore path bailed out before
doing anything). **D161** (backtick `\$`/`\@` escaping) turned out bigger
than its original write-up guessed: investigating it found `qx(...)`
wasn't implemented at all (a hard parse error) — both are now fixed,
`qx` across every real-Perl delimiter form. Tests: `tests/file_glob_{smoke,deep}.pl`,
`tests/file_which_{smoke,deep}.pl`, `tests/cpanel_json_xs_{smoke,deep}.pl`,
`tests/split_nul_{smoke,deep}.pl`, `tests/split_implicit_underscore_
{smoke,deep}.pl`, `tests/split_scalar_context_{smoke,deep}.pl`,
`tests/local_env_{smoke,deep}.pl`,
`tests/backtick_qx_escapes_{smoke,deep}.pl`.
Previous session (2026-09-23, CGI + Term::ReadLine + missing `use` names):
`Term::ReadLine` (Stub), `CGI` (param/header/html tags), `MIME::QuotedPrint`,
`Digest` front-end, `Text::Tabs`, `FileHandle`, `IO::{Seekable,Pipe,Select,UNIX}`,
`SelectSaver`, `Fatal`, `use open qw(:std :utf8)`.
Previous session (2026-09-23, native stdlib waves 4–5 — 461/461 PASS, 0 FAIL): `Getopt::Std`,
`Text::ParseWords`, `File::Compare`, `File::stat`, `English`, `if`,
`experimental`, `HTTP::Tiny`, `version`, `autodie`, `PerlIO::scalar`.
Previous session (2026-09-22, native stdlib waves 1–3 — 439/439 PASS):
`FindBin`, `Symbol`, `IPC::Open2`/`Open3`, `IO::Handle`/`IO::File`, `Socket`,
`IO::Socket::INET`/`IP`, `MIME::Base64`, `Digest::MD5`/`SHA`.
Previous session (2026-09-21, language leftovers — 413/413 PASS, 0 FAIL):
`undef $var` (pidigits was this, not mini-gmp; `undef //` is the value),
JSON `->{k}` UAF on temporaries, Storable nfreeze wire format, `%{EXPR}`,
modifier-`if &&`, `system LIST`, `-name =>` option keys, `delete`/`exists`
on `$ref->{k}`, `splice`/`pop`/`shift @{EXPR}`, `require VERSION`,
`quotemeta`, `$^V`, native `Pod::Usage::pod2usage`, D54 `test-tsan`
`die_after_fork=0`. Previous session: remaining scoped-out surface on
Hash::Util (hashref variants, `lock_hash_recurse`, slice-assign checks),
JSON::PP (`allow_nonref`, `space_before`/`space_after`, `convert_blessed`,
`\u` decode as character strings, surrogate pairs), Text::CSV
(`quote_char => undef`, `csv()`, EOF diag 2012), Storable
(`freeze`/`thaw`/`nfreeze`/`store`/`nstore`/`retrieve` round-trip), plus
Tier-3 native modules `Try::Tiny`, `List::MoreUtils`, `Term::ANSIColor`,
`Encode`. Tests: `tests/hash_util_ref_{smoke,deep}.pl`,
`tests/json_pp_opts_{smoke,deep}.pl`, `tests/storable_freeze_{smoke,deep}.pl`,
`tests/text_csv_extra_{smoke,deep}.pl`, `tests/try_tiny_{smoke,deep}.pl`,
`tests/list_moreutils_{smoke,deep}.pl`, `tests/term_ansicolor_{smoke,deep}.pl`,
`tests/encode_{smoke,deep}.pl`. Previous session: `Text::CSV`/
`Text::CSV_PP`/`Text::CSV_XS` (all three alias one native
implementation) — `->new`, `parse`/`fields`/`combine`/`string`/
`status`, `getline`/`getline_all`/`getline_hr`/`getline_hr_all`/
`column_names`, `print`/`say`, `error_diag`/`error_input`/`SetDiag`,
and the `sep_char`/`quote_char`/`escape_char`/`eol`/`binary`/
`always_quote`/`quote_space`/`allow_whitespace`/`allow_loose_quotes`/
`blank_is_undef`/`empty_is_undef` accessors — `tests/text_csv_
{smoke,deep}.pl`. Also found and fixed **D143/D144** (see TESTS.md):
D143 is a broad, pre-existing codegen gap — OO method calls never
propagated list/scalar context into `perl_dispatch_method`, so any
context-sensitive method (Text::CSV's `fields`, DBI's
`fetchrow_array`) silently misbehaved in list context
(`join("|", $csv->fields)`, `my @f = $csv->fields;`); the fix (a new
`NK::MethodCall` case in `emitArrayPtr`) initially broke
`d75_multi_inherit.pl` by not excluding `isa`/`can`/`SUPER::`/
Math::BigInt/threads' bespoke codegen paths — caught by `make
test-all`, fixed same-day. D144 is in-memory filehandles only syncing
their backing scalar on `close()` instead of on every write (a plain
`print $fh "x"; print "[$out]";` with no close reproduces it, nothing
CSV-specific). Previous session (2026-09-20, Time::Piece/Time::Seconds
— 388/388 PASS): `Time::Piece` (`localtime`/`gmtime` scalar-context override,
`->new`/`->strptime`, accessors, `strftime`, `+`/`-`/`<=>`/`""`
overloads) and `Time::Seconds` (`->new`, `seconds`/`minutes`/`hours`/
`days`/`weeks`/`months`/`years`/`pretty`, the 9 `ONE_*`/`LEAP_YEAR`
constants) as native modules — `tests/time_piece_{smoke,deep}.pl`.
Also found and fixed **D139/D140/D141/D142** (see TESTS.md): four
generic, pre-existing defects in how blessed-object overloads interact
with `==`/`!=` ref-identity, `<=>`/`sort`'s numeric fast path, the
BigInt-guard F64 fast path, and `push`/`map` with `scalar
localtime(...)`/`scalar gmtime(...)` — none specific to Time::Piece,
just never exercised by Math::BigInt (whose own numeric-native tag
happens to sidestep all four). Previous session (2026-09-20, JSON::PP —
386/386 PASS): `JSON::PP` (`encode_json`/`decode_json`, OO
`new`/`canonical`/`pretty`/`encode`/`decode`, `JSON::PP::true`/`false`)
as a native module — `tests/json_pp_{smoke,deep}.pl`. Also fixed a
parser gap found while building it: a bareword constant like
`JSON::PP::true` used as a hash value before a comma (`active =>
JSON::PP::true,`) was silently auto-quoted as the literal string
`"JSON::PP::true"` instead of being called — see TESTS.md's JSON::PP
write-up. Previous session
(2026-09-19, Tier-1 file/data modules + D138 — 384/384 PASS): `File::Copy`
(`copy`/`move`), `File::Find` (`find`), `File::Path`
(`make_path`/`remove_tree`), `File::Temp`
(`tempfile`/`tempdir`/`mkstemp`/`mkdtemp`/`mktemp`/`tmpnam`),
`Storable::dclone`, and `Text::Wrap` (`wrap`/`fill`) as native modules
— `tests/file_copy_{smoke,deep}.pl`, `tests/file_find_{smoke,deep}.pl`,
`tests/file_path_{smoke,deep}.pl`, `tests/file_temp_{smoke,deep}.pl`,
`tests/storable_dclone_{smoke,deep}.pl`,
`tests/text_wrap_{smoke,deep}.pl`. Also found and fixed **D138** (see
below) while running the full harness against this new work, plus a
test-authoring bug in `file_temp_{smoke,deep}.pl`'s own regexes (too
narrow an alphabet for `File::Temp`'s real random suffix — see
TESTS.md). Previous session (2026-09-16):
`d113_undefined_sub_die_{smoke,deep}.pl`,
`d111_hash_flatten_{smoke,deep}.pl`,
`d112_module_scope_{smoke,deep}.pl` (+ `tests/lib/D112Leaky.pm`),
`d114_array_slice_{smoke,deep}.pl`, `d109_subst_interp_{smoke,deep}.pl`,
`d121_bare_maincolon_{smoke,deep}.pl`,
`d122_scanexports_usevars_{smoke,deep}.pl` (+
`tests/lib/D122UseVarsExport.pm`), `d116_dunder_consts_{smoke,deep}.pl`,
`d117_atof_precision_{smoke,deep}.pl`, `d118_split_limit_{smoke,deep}.pl`,
`d119_keys_deref_scalar_{smoke,deep}.pl`, `d127_amp_export_{smoke,deep}.pl`
(+ `tests/lib/D127AmpExport.pm`, `tests/lib/D127AmpExportOk.pm`),
`d129_local_paren_{smoke,deep}.pl`, `d102_die_ref_{smoke,deep}.pl`,
`d115_bare_return_list_{smoke,deep}.pl`, `d130_my_cond_{smoke,deep}.pl`,
`d131_our_nested_block_{smoke,deep}.pl`, `d101_each_scalar_{smoke,deep}.pl`,
`d103_int_overflow_{smoke,deep}.pl`, `d104_indented_heredoc_{smoke,deep}.pl`,
`d106_flat_ref_elem_alias_{smoke,deep}.pl`, `d108_string_escapes_{smoke,deep}.pl`,
`d125_pragma_nested_{smoke,deep}.pl` (+ `tests/lib/D125Pragma.pm`),
`d126_split_captures_{smoke,deep}.pl`,
`d132_bigint_f64_fastpath_{smoke,deep}.pl`,
`d133_assign_double_free_{smoke,deep}.pl`,
`d134_syscall_buf_{smoke,deep}.pl`,
`d135_int_promo_nv_{smoke,deep}.pl`,
`d110_qual_global_{smoke,deep}.pl`,
`d120_string_deref_interp_{smoke,deep}.pl`,
`d124_current_sub_{smoke,deep}.pl`,
`d128_module_error_location.sh` (+ `tests/lib/D128Broken.pm`,
`tests/d128_module_error_main.pltxt` — compile-failure diagnostics
fixtures, outside the harness corpus),
`parse_gaps_{smoke,deep}.pl`, `inmem_fh_{smoke,deep}.pl`,
`w30_sub_eval_{smoke,deep}.pl`, `or_next_{smoke,deep}.pl`,
`exporter_tags_{smoke,deep}.pl` + `exporter_slice_keys_{smoke,deep}.pl`
(+ `tests/lib/E/Tagged.pm` — Exporter mechanism, 2026-09-14),
`dynaloader_ffi.sh` (+ `tests/dynaloader_ffi_{smoke,deep}.pltxt`,
`tests/lib/auto/My/Clib/Clib.so`, `tests/lib/auto/My/Pxs/Pxs.pl` —
DynaLoader-compatible FFI, self-verifying, outside the harness corpus),
`cwd_{smoke,deep}.pl`, `sys_hostname_{smoke,deep}.pl`,
`file_spec{,_functions}_{smoke,deep}.pl`, `time_local_{smoke,deep}.pl`
(Tier-1 native modules, 2026-09-15),
`config_{smoke,deep}.pl`, `fcntl_posix_{smoke,deep}.pl`
(Config + Fcntl/POSIX/Errno native constants, 2026-09-16),
`w31_return_list_{smoke,deep}.pl`, `w23_key_expr_{smoke,deep}.pl`,
`w28_match_var_{smoke,deep}.pl`,
`w22_symbolic_deref_{smoke,deep,deep2}.pl`,
`w19_unary_plus_{smoke,deep}.pl` (survey-3 W-items, 2026-09-16),
`qr_regex_{smoke,deep}.pl`, `qr_match_list_{smoke,deep}.pl`
(qr// + list-context match captures + regex-pattern interpolation,
2026-09-16), `local_glob_{smoke,deep}.pl` (W29 `local *_`/`local $_`),
`false_bool_{smoke,deep}.pl` (booleans stringify as 1/"" like real
perl).
Skipped by default: `dbi_sqlite.pl`, `xs_ffi.pl`.

**D99, D105, D100, D107, D113, D111, D112, D114, D109, D121, D122,
D116, D117, D118, D119, D127, D129, D102, D115, D130, D131, D101,
D103, D104, D106, D108, D125, D126, D132, D133, and D134 are now fixed
(D115/D102/D129/D127/D119/D118/D117/D116/D121/D122/D113/D111/D112/
D114/D109 detailed just below; D99/D105/D100/D107 write-ups follow;
D130/D131/D101/D103/D104/D106/D108/D125/D126/D132/D133/D134/D138 write-ups
are in TESTS.md):**
- D138 (2026-09-19): `\&name == \&name` / `__SUB__ == \&name` (CODE-ref
  identity via `==`) was unreliable — `perl_num_eq`/`perl_num_ne`
  (`src/runtime.c`) compared the address of the freshly-`malloc`'d
  `PerlClosure` wrapper each `\&name`/`__SUB__` evaluation allocates,
  not the wrapped sub itself, so two refs to the same sub were `==`
  only when `malloc` happened to reuse a just-freed wrapper's address.
  Pre-existing (confirmed via `git stash` bisection against the prior
  commit), unmasked — not caused — by this session's new module work
  shifting `runtime.c`'s heap allocation pattern. Fixed with a
  `perl_ref_identity()` helper that unwraps `PERL_CODE_REF` to its
  `PerlClosure->fn` pointer before comparing; every other ref tag is
  unchanged. See TESTS.md for detail, including a companion
  test-authoring bug found the same way (`file_temp_{smoke,deep}.pl`'s
  own regexes assumed an alnum-only random alphabet; real `File::Temp`
  includes `_`, so real Perl's own output legitimately failed the
  test ~1 run in 7).
- D125/D126/D132/D133/D134 (2026-09-12, two-agent parallel session):
  see TESTS.md for full write-ups. Summary: D125 is `use`/`no` pragmas
  now parsing inside any nested scope (the whole handling extracted
  from `parseProgram()` into `Parser::parseUseNoStmt()`, shared with
  `parseStmt()`); D126 is `split` with capturing-group patterns now
  interleaving capture texts (plus a side-effect fix for the
  pre-existing hang/garbage on all-zero-width split patterns; LIMIT
  counts fields only; non-participating groups yield UNDEF, matching
  perl); D132 is `emitBinOp`'s F64 fast path no longer bypassing
  D103's BigInt-aware ops for a BigInt-tagged scalar variable (runtime
  tag-predicate guard, branch-and-PHI, non-BigInt IR byte-identical)
  plus a second 1-ULP fix (mini-gmp `mpz_get_d` truncates → exact
  decimal-string + `strtod` via `perl_mpz_get_double`); D133 is a
  pre-existing double-free in the int/float-var Assign boxed fallback
  (`freeIfOwned(rv)` then `return rv` — freed again by the statement
  context; found via a segfault while verifying D125's deep test,
  reproduces on the pre-fix snapshot); D134 is `syscall()` args now
  pushed by reference so kernel writes through a pointer argument land
  in the caller's buffer (fixes `make test`'s xs_ffi clock assertions;
  `make test` is 47/47). Found, not fixed, while writing D133's deep
- D128/D135 (2026-09-13, two-agent parallel session): see TESTS.md for
  full write-ups. D128 is parse-error diagnostics: tokens now carry
  their source file (process-wide deque filename registry; `Token`
  grows one pointer), so a parse error inside an inlined module
  reports `Parse error in <module> line N:` with the module's own
  internal line, while main-file errors keep the legacy format;
  D135 is sub-scope/bare-block int-promotion no longer pinning a
  variable to i64 storage when the scope later writes it a
  non-int-shaped value (fixpoint "provably-int-only" scan over the
  scope's writes; float-unbox or boxed-PV fallback; pure-int counters
  keep byte-identical IR). Both shipped with their own self-verifying
  test assets (D128's are compile-failure fixtures outside the
  harness corpus).
- D110/D120/D124 (2026-09-13, two-agent parallel session — the last
  three open generated-code defects; see TESTS.md for full write-ups):
  D110 makes any `::`-containing name a true cross-scope global (reads,
  writes, elements, slices, whole-container ops, `local`) via the
  runtime's process-wide glob registry, preferring module-`our` storage
  when present; D120 teaches the general `"..."` interpolation scanner
  subscripted-deref forms (`$$aref[0]`, `$$href{k}`, `@{$r}[0,1]`,
  `@$ref[1..2]`, the `@{[ ... ]}` trap idiom) via a new
  `parseSubscriptGroup` helper emitting the token-level parser's node
  shapes; D124 is `__SUB__` — `perl_call_code_ref` now tracks the
  running closure's code-ref object in a thread-local
  (`perl_get_current_code_ref`) so recursion through `__SUB__` keeps the
  closure's own captures, with named subs resolving to their own
  capture-less `\&name`-shaped ref (matching this codebase's named-sub
  model) and undef at file scope.
- Exporter/D136/D137/DynaLoader-FFI (2026-09-14): see TESTS.md for the
  full write-ups. D136 is bareword-`=>` auto-quote (real Perl quotes
  every bareword before `=>`, including its own keywords — `all => 1`,
  `sub => 2` were hard parse errors) plus bareword hash-subscript keys
  (`my @a = @{ $r->{all} };` used to die); D137 is `qw(...)` slice key
  specs spreading into individual keys/indices (`@h{qw(a b)}`,
  `delete @h{qw(a b)}` — previously one undef key lookup, silently
  empty); the Exporter mechanism makes `%EXPORT_TAGS`/`:tag`/`:all`
  imports, `&`-sigil'd export names, and `$var` exports work at compile
  time with real Exporter semantics; the DynaLoader-compatible FFI
  (phase 2) implements `dl_load_file`/`dl_find_symbol`/
  `dl_install_xsub`/`dl_error`/`bootstrap` + `XSLoader::load` natively —
  perlc-compiled modules load on demand via the `--do-lib` subprocess
  and register their boot hook under `<Module>::boot` (with real
  DynaLoader's `boot_<mangled>` names also tried). Real perlguts XSUBs
  (SV*-based) remain out of scope.
- Survey-3 + Tier-1 modules (2026-09-15, two-agent parallel session):
  see TESTS.md for the full write-up. Agent A's 12-script probe survey
  found and fixed 8 items (mixed-sigil `my (%h)`/`our (%a, $b)` lists;
  `use constant` multi-token values incl. the OOB-crash shape; `1<<5`
  mis-lex as heredoc + its inlineModules crash; arbitrary-delimiter
  `q!`/`qq!`/`tr|/|_`; `qw (` spacing; `print($fh "str")` fh-in-parens;
  `&delete()` keyword-named sub calls; Getopt::Long::Configure no-op)
  and logged 10 open items (W16/W19/W22/W23/W27–W31 — Config.pm's
  computed typeglob is the top one). Agent B implemented Cwd,
  Sys::Hostname, File::Spec(+::Functions, native, all three invocation
  styles, real quirks matched), Time::Local (all 8 exports, real DST
  semantics), and fixed scalar-context `gmtime(EXPR)`/`localtime(EXPR)`
  returning the epoch.
- W-item batch (2026-09-16, two-agent session): see TESTS.md. Config
  is native (special %Config hash from a generated host-perl table +
  myconfig/config_sh/config_vars/config_re byte-identical; Config.pm's
  computed-typeglob import never runs); Fcntl/POSIX/Errno are native
  constant tables (86 probed values, real croak message, real @EXPORT
  tag sets, sysseek added); W31 single-element `return (32)`;
  W23 in-key builtin-call vs string-key disambiguation + `(not => 1)`;
  W28 `$s =~ $var` dynamic patterns; W22 symbolic deref `${"name"}`/
  `${$ref}` + `\$arr[1]` element-ref fix; W19 unary `+` + constant
  chains; qr// as a compiled-pattern value (lexer/parser/runtime tag,
  ref()=Regexp, (?^msix:...) stringification, QR-aware =~ dispatch),
  list-context non-/g match captures (groupless → (1) like real perl),
  and regex-pattern interpolation (/$name/, s/$pat/.../, qr/$var/);
  !~ stays boolean in list context. Remaining open: qr->() invocation.
- W29 + false-bool (2026-09-16): see TESTS.md. `local *_ =
  \join(...)`/`local $_ = v` work (NK::LocalGlob; global `$_` cell +
  sub-shadow saved/assigned, depth-restored; Assign-to-`$_` now syncs the
  cell), and boolean results stringify as 1/"" like real perl
  (perl_not/defined/=~ via perl_alloc_bool).
- D103/D104/D106/D108: see TESTS.md for full write-ups. Summary: D103
  is integer-overflow auto-promotion (bounded, unblessed auto-BigInt
  reusing the existing Math::BigInt/mini-gmp machinery — see TESTS.md
  for why threading a real UV type was judged too risky, and the
  `__builtin_*_overflow` fix for a related pre-existing UB bug found
  along the way); D104 is `<<~IDENT` indented-heredoc lexer support;
  D106 is D105's identical FLAT_ARRAY-ref-alias fix ported to array/hash
  *element* reads (`$arr[0]`, `$h{k}`), confirmed not to touch the
  fragile 2D compound-assign fast paths that caused D105's own segfault
  regression; D108 is `\f`/`\a`/`\e`/`\b` missing from plain
  double-quoted string literals (two separate lexer code paths had the
  identical gap). Found, not fixed, while verifying D103: **D132**
  (`emitBinOp`'s F64 fast path can bypass D103's BigInt-aware ops for a
  BigInt-tagged scalar variable — narrow, 1-ULP-only divergence).
- D101 (`src/codegen.cpp`, scalar-context `case NK::EachFunc`):
  `each %hash` in scalar context (`while (my $k = each %h)`) returned
  `perl_array_len(av)` — the [key,val] pair-array's *count* (0, 1, or
  2) — instead of the key itself. Masked in casual testing because a
  truthy 2 happens to make the loop iterate the right *number* of
  times even though every `$k` was wrong. Fixed by returning
  `perl_array_get(av, 0)` instead, which already returns `undef` for
  an out-of-range index (the post-exhaustion case) — no new runtime
  code needed. List-context `each` was already correct and untouched.
- D131 (`src/codegen.cpp` `case NK::My`, scalar/`:shared`/array/hash
  declaration branches): `our $var;`/`our @arr;`/`our %hash;` declared
  inside a nested bare `{ }` block, or textually redeclared anywhere
  (e.g. inside a sub, to bring an existing package var into scope) —
  two stacked bugs. (1) The scalar branches gated global-vs-local
  storage on `atFileScope` alone instead of `atFileScope || isOur`, so
  a nested-block `our` fell to a disconnected local alloca. (2) Once
  fixed, every repeated textual `our` occurrence for the same name was
  found to unconditionally mint a brand-new LLVM global instead of
  reusing the one already registered by package-qualified name (the
  array/hash branches already partially did this for D112, but still
  always overwrote the value on reuse even with no initializer,
  silently resetting an already-populated `our @arr;`/`our %hash;`
  back to empty). Fixed by looking up an existing global by qualified
  name first on every branch, and only resetting storage when newly
  created or an initializer is actually given.
- D130 (`src/parser.cpp` expression-context `my`-parsing;
  `src/codegen.cpp` expression-context `case NK::My` in `emitExpr`):
  `if (my @arr = EXPR)` / `if (my %h = EXPR)` — a single array/hash
  variable declared inline as an `if`/`while` condition, no
  surrounding parens — was a hard parse error; the parenthesized
  multi-variable list form (D100) and the single-scalar form already
  worked, but this in-between shape didn't. Fixed with a parser branch
  alongside the existing scalar case, plus array-length/hash-size
  return handling in the expression-context codegen case so the
  condition's truthiness matches real Perl's list-assignment-count
  semantics.
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
  just a constant substitution) as **D124** — since fixed (2026-09-13).
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

**Open generated-code defects:** none — **D176 FIXED 2026-10-02**
(quoted heredoc delimiter with punctuation characters — see
TESTS.md). **D175 FIXED 2026-10-02**
(`$]` not implemented at all — see TESTS.md). **D174 FIXED
2026-10-02** (`q()`/`qq()`/`qx()` nested-bracket-delimiter depth
tracking — see TESTS.md). **D173 FIXED 2026-10-01**
(diamond-glob `<PATTERN>` wasn't variable-interpolated — see
TESTS.md). **D172 FIXED 2026-10-01**
(`last`/`next`/`redo` as a comma-expression operand was a parse
error, and `last`/`next` silently didn't actually jump once
parseable — see TESTS.md). **D171 FIXED 2026-10-01**
(short-circuit compound-assign LLVM verify-crash on hash/array
elements, plus a quoted-interpolation-key bug — see TESTS.md). **D170
FIXED 2026-10-01** (out-of-bounds crash on a malformed/unterminated
`use` statement — see TESTS.md). **D169 FIXED 2026-10-01**
(`local $ref->{key}`/`local $ref->[idx]` was a hard parse error — see
TESTS.md). **D168 FIXED 2026-10-01**
(CODE-ref boolean-context truthiness, plus a use-after-free in
several native OO chainable-setter methods — see TESTS.md). **D167
FIXED 2026-10-01** (constant-string `eval` compile-error LLVM
verify-crash, plus overly strict parenless-bareword handling — see
TESTS.md). **D166 FIXED 2026-09-30**
(`-t FILEHANDLE` not implemented at all, plus `do{}while/until COND`
requiring a literal `(` — see TESTS.md). **D165 FIXED 2026-09-30**
(POSIX `:fcntl_h` tag not recognized, plus `<< "EOT"` heredoc
whitespace — see TESTS.md). **D164 FIXED 2026-09-30**
(diamond-glob `<PATTERN>` syntax not recognized, plus a pre-existing
infinite-loop bug in scalar-context `glob()` — see TESTS.md). **D163
FIXED 2026-09-30**
(bare `;` after a block-ending statement was a hard parse error — see
TESTS.md). **D161 FIXED 2026-09-30**
(backtick `\$`/`\@` escaping, plus `qx(...)`/`qx{...}`/etc. not being
implemented at all — see TESTS.md). D110/D120/D124 were fixed
2026-09-13 (see TESTS.md). **D54** (tooling) **FIXED 2026-09-21**:
`make test-tsan` sets `TSAN_OPTIONS=die_after_fork=0` on the `perlc_tsan`
compile step.

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
**D124**, since fixed 2026-09-13) and **D117** (`perl_atof_decimal` hand-
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
| Full XS | DynaLoader-compatible FFI: `dl_load_file`/`dl_find_symbol`/`dl_install_xsub`/`bootstrap`/`XSLoader::load` (perlc `.so`/`.pl` modules + raw C via `XS::call` sig dispatch). Real perlguts XSUBs (SV* ABI) not implemented |
| `pidigits.pl` vs perl | PASS — was `undef $s` not clearing the accumulator, not mini-gmp. |
| Complex CPAN | Parser may fail on advanced `our`/OO. POD (`=pod`…`=cut`) is skipped. |
| eval/`do` at runtime | Needs `perlc` + `clang-18` on the target (`--eval-lib` / `--do-lib`). |
| Auto-parallel `map`/`foreach` | Deferred 2026-09-22. Pthreads + known `map` length exist; no dependence analyzer. Gated pure-`$_` map is the only safe slice. See TESTS.md → "Future: auto-parallel `map` / counted loops". |

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
`runtime.c` (~8.8k), `native_stdlib.c`, `mini-gmp.c`, `main.cpp`.

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
"Real-world module survey"); File::Copy, File::Find, File::Path,
File::Temp, Storable::dclone, Text::Wrap (2026-09-19); JSON::PP,
Time::Piece, Time::Seconds, Text::CSV, Text::CSV_PP, Text::CSV_XS,
Hash::Util (2026-09-20); Storable freeze/thaw, Try::Tiny, List::MoreUtils,
Term::ANSIColor, Encode (2026-09-20); Pod::Usage (2026-09-21);
FindBin, Symbol, IPC::Open2/Open3, IO::Handle/IO::File, Socket,
IO::Socket::INET/IP, MIME::Base64, Digest::MD5/SHA (2026-09-22);
Getopt::Std, Text::ParseWords, File::Compare, File::stat, English, if,
experimental, HTTP::Tiny, version, autodie, PerlIO::scalar (2026-09-23);
Term::ReadLine, CGI, MIME::QuotedPrint, Digest, Text::Tabs, FileHandle,
IO::Seekable/Pipe/Select/UNIX, SelectSaver, Fatal, open pragma (2026-09-23);
File::Glob (`bsd_glob`, `GLOB_*` constants; 2026-09-30);
File::Which (`which`/`where`; 2026-09-30);
Cpanel::JSON::XS (aliases the native JSON::PP implementation — real Cpanel::JSON::XS isn't installed on this dev machine, so verified by cross-checking the identical script's output through the already-real-Perl-verified JSON::PP path instead; 2026-09-30);
URI::Escape (`uri_escape`/`uri_escape_utf8`/`uri_unescape`; 2026-09-30);
Test::More (`ok`/`is`/`isnt`/`like`/`unlike`/`cmp_ok`/`pass`/`fail`/`diag`/`note`/`done_testing`/`subtest`; 2026-10-01);
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
