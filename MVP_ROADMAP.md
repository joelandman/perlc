# perlc — MVP Roadmap (2026-09-10)

## How this was produced

A five-agent review: one architect/code-reviewer, three engineers doing
depth passes on runtime (`src/runtime.c`), codegen (`src/codegen.cpp`,
focused on D109/D110), and lexer/parser (`src/lexer.cpp`/`src/parser.cpp`),
and one PM reviewing `README.md`/`CLAUDE.md`/`TESTS.md` to build a
CPAN-module candidate list and MVP definition. The architect deliberately
wrote fresh probe scripts instead of reusing the existing test corpus —
this is what surfaced most of the new findings below. All the
headline claims were re-verified in this session, byte-for-byte against
real Perl 5.42, against a clean `make clean && make` build before being
written up:

| Claim | Verified |
|---|---|
| `my %c = %h;` produces wrong contents | ✅ confirmed — `2=` / count=1 instead of `a=1 b=2` / count=2 |
| Undefined sub call returns `undef` instead of dying | ✅ confirmed — perl exits 255, perlc exits 0 |
| `@x[1..2]` / `@x[@i]` array slices return 1 element | ✅ confirmed |
| `__PACKAGE__` is a hard parse error | ✅ confirmed |
| `die { ref }` loses the reference (D102) | ✅ re-confirmed, still open |

New defects are logged as **D111–D118** in `TESTS.md`'s Open table and
write-up sections (with full repro + root-cause detail there). This
document is the synthesis and the forward plan; `TESTS.md` remains the
defect ledger.

## Executive verdict

**Not close to MVP for "common CPAN modules work correctly." Closer,
but not there, for "single-file scripts work correctly."**

The 263/263 harness pass rate is real, but it measures a test corpus
written *against* this compiler as bugs were found and fixed — it's a
regression gate, not a general-correctness oracle. The architect agent's
fresh probes (idioms picked before looking at the test list) found seven
previously unlogged defects in about 40 minutes, four of them
silent-wrong-data in universally-common Perl idioms (hash copying, array
slicing, undefined-sub dispatch, list-context `return`).

The most important structural finding isn't any single bug: **perlc
currently has no failure detector.** An unresolvable `use Some::Module`
is silently dropped; a call to an undefined sub silently returns `undef`
instead of dying. This is exactly the mechanism by which three
completely-unimplemented modules (Getopt::Long, Data::Dumper,
File::Basename) went unnoticed until a human manually diffed output
during the 2026-09-09 survey — and it means every future survey of
"does module X work" is unreliable until it's fixed first (see Critical
path, step 1).

## Two flagship items, re-assessed

**D110 (`$Package::var` not a true cross-scope global) is over-rated.**
Verified: `our $VERSION`, `our %CONFIG`, and cross-package/cross-sub
read *and write* via `our`-declared globals all work correctly today.
D110 only bites *undeclared* `$Pkg::var` (no `our`) — real modules
almost always declare with `our`. The codegen review found the real fix
is **medium effort, not large**: a working global-scalar registry
(`perl_get_or_create_global_scalar`) already exists and is already used
in `--do-lib` mode; the fix is routing the undeclared-fallback path
through it instead of a function-local alloca. Real-world blocking
risk: **low**.

**D109 (`s///` replacement interpolation) is under-rated, and it's not
isolated to `s///`.** It's silent-wrong-data (`s/o/[$re]/g` emits the
literal text `[$re]`), which is worse than a loud parse error. The
codegen review also found it's **not just an `s///` bug**: the general
double-quoted-string interpolation engine is independently wrong for
`$$aref[0]` (prints `[0]`) and `@{$r}[0,1]` (prints `1 2 3[0,1]`) inside
plain `"..."` strings — nothing to do with `s///`. The `/e`-flag
substitution path (`codegen.cpp:7954-8085`) already builds almost
exactly the closure/capture machinery D109 needs, and
`Parser::parseStringInterp` (already used for ordinary `"..."` literals)
is directly reusable for the replacement text — so this is also
**medium, not large**, and fixing it by *unifying* the interpolation
engine rather than patching `s///` locally would close three bugs at
once, not one.

## Ranked findings (real-world blocking risk for "CPAN modules work")

### Tier 0 — silent wrong data in universal idioms (fix first)

1. ~~**D111** — `%hash` doesn't flatten correctly in `my %c = %h` / hash-literal-merge contexts.~~ **FIXED 2026-09-10.** Also caught a second instance of the same bug in `{ %args, k=>v }` anon-hashref spreads (the `bless { %args }, $class` idiom itself) while fixing it. See TESTS.md.
2. ~~**D112** — module file-lexical `my` collides with the main script's.~~ **FIXED 2026-09-10.** Turned out not to need the architectural block-scoping change this entry originally proposed — see TESTS.md's D112 write-up for why that approach was tried and rejected, and what the actual (smaller) fix was.
3. ~~**D113** — no failure signal.~~ **FIXED 2026-09-10.** Undefined-sub calls now die matching real Perl; unresolvable `use` now errors; `use lib`/`-I`/`PERL5LIB` now honored.
4. ~~**D114** — array slices with a non-literal subscript (`@x[1..2]`, `@x[@i]`) return one element instead of the slice.~~ **FIXED 2026-09-10.**
5. ~~**D109** — `s///` replacement text didn't support `$name`/`@arr` interpolation.~~ **FIXED 2026-09-10.** Turned out to need less unification than predicted — reused the `/e` flag's existing closure/capture machinery directly, routed through the same interpolation scanner `"..."` literals already use, rather than rebuilding anything. Split off the harder, non-`s///`-specific remainder (subscripted deref in *any* interpolated string, `$$aref[0]`/`@{$r}[0,1]`) as **D120** — that one is still open and is the "medium, needs the general engine unified" item this entry originally described. See TESTS.md.
6. **D102 — `die REF` loses the reference.** Re-confirmed: `ref($@)` comes back empty and a wrong `" at FILE line N."` gets appended to what should be a reference. Blocks all typed/OO exception handling, which is how modern CPAN modules report errors. Still open.
7. **D115 — bare `return;` yields a 1-element list in list context** instead of Perl's empty list. Breaks the standard "return nothing on failure" contract. Still open.
8. **D119 (new, found 2026-09-10 while fixing D111) — `scalar(keys %$href)` returns 0 instead of the key count.** List-context `keys %$href` is correct; only scalar context is wrong. Small, mechanical fix (port `emitArrayPtr`'s existing deref-hash handling into `emitExpr`'s scalar-context `KeysFunc` case) — see TESTS.md.

### Tier 1 — hard parse errors in common module syntax (loud, so lower risk per-instance, but each one gates an entire file)

8. ~~**D116** — `__PACKAGE__`/`__FILE__`/`__LINE__`/`__SUB__` entirely unimplemented.~~ **`__PACKAGE__`/`__FILE__`/`__LINE__` FIXED 2026-09-10** — was indeed a small fix, as predicted. `__SUB__` split off as **D124** (needs real closure-capture support, not a constant substitution — genuinely harder, still open).
9. **`$obj->$method()` dynamic dispatch** — parse error. Common in accessors/plugin dispatch tables.
10. **`map { {...} } @list`** (and the `+{...}` block/hashref disambiguator) — parse error. Extremely common.
11. **`%EXPORT_TAGS` / `use Foo qw(:all)`** — hard error (also see the Exporter gap below).
12. **In-memory filehandles**, `open($fh, '<', \$string)` — unsupported; pervasive in module test suites (relevant once module test compilation becomes a goal, not just module use).
13. **`grep { defined } @list`** — bare named-unary-with-implicit-`$_` inside a block — parse error.
14. Already logged, unchanged priority: **`qr//`** (not implemented as a value type at all — see TESTS.md missing-features list), **`continue {}` blocks**, **`<<~` indented heredoc (D104)**, **`q[...]`/`qq[...]` nested-bracket balancing** (inconsistent — `s///`'s paired-delimiter form already tracks nesting depth correctly; `q`/`qq` non-brace forms don't), **`\my %var`/`\my @arr`**.

### Tier 2 — genuine but narrow (post-MVP is fine)

D106 (narrow FLAT_ARRAY re-alias variant), D108 (`\f\a\e\b` in plain strings), D101 (`each` scalar context), D103 (2^63 overflow), ~~D117~~ (FIXED — `perl_atof_decimal` hand-rolled parser → `strtod`), ~~D118~~ (FIXED — `split` LIMIT + trailing-empty trim; found D126, a separate capturing-group-in-split-pattern bug, while testing).

### Architectural gap bigger than any single D-number: no Exporter/`@EXPORT`

There is no `Exporter`/`@EXPORT`/`@EXPORT_OK`/`import` mechanism at all.
`use Foo;` only does something for the hardcoded set of builtin modules
perlc special-cases internally (List::Util, Data::Dumper, etc.) — an
arbitrary pure-Perl CPAN module that defines subs and expects
`Exporter` to import them unqualified has no support path. This sits
outside the current whole-program, single-token-stream compilation
model (same root cause as D112) and is arguably the actual ceiling on
"real CPAN modules work," more than any individual defect above. It's
deliberately *not* given a D-number because it isn't a bug in existing
code — it's a capability that doesn't exist yet — but it needs to be on
the roadmap explicitly, and D112's fix (giving modules a real scope
boundary) is very likely a prerequisite for it.

## CPAN module candidates (pure-Perl only — no XS/DynaLoader)

Filtered to modules with no compiled-C dependency (consistent with
perlc being an AOT whole-program compiler with its own runtime, not a
real interpreter that can `dlopen` arbitrary XS), and ranked by expected
frequency in the sysadmin-script/small-CLI-tool genre this project's own
2026-09-09 survey already targeted (Debian `/usr/bin` scripts,
`dh_*`-style tooling, config/log processing) — matching the shape of
scripts like `debconf-escape` and `ptardiff` that earlier work was
found through.

**Tier 1 — very high frequency in this genre, low-to-moderate cost:**
`File::Spec`, `Cwd` (needs a `getcwd`/`realpath` wrapper — currently
absent), `File::Path` (`make_path`/`remove_tree` — largely expressible
via existing mkdir/rmdir/opendir/readdir primitives), `File::Find`
(added 2026-09-10 — core module, came up immediately in a 10-script
compile survey, see TESTS.md), `File::Temp` (needs an
`mkstemp`-equivalent — currently absent, but `perl_sysopen_fh` with
`O_EXCL|O_CREAT` gets most of the way there), `Sys::Hostname` (trivial
— wraps `gethostname(2)`), `Time::Local` (trivial C wrappers around
`mktime`/`timegm`), `Text::Wrap`.

**2026-09-10 compile-survey note:** actually compiling 10 real scripts
against this list (see TESTS.md's "CPAN-module compile survey" section)
confirmed `File::Spec`/`File::Path`/`File::Find`/`JSON::PP` are indeed
what's blocking real scripts, matching this ranking — but also surfaced
that `File::Spec` specifically needs `qr//` implemented first (real
`File::Spec::Unix` matches against a stored compiled pattern
internally), and found two *new*, `File::Spec`-independent bugs while
probing further — both fixed same-day: ~~**D121**~~ (`$::name`
bare-`main::`-shorthand parse error) and ~~**D122**~~ (`scanExports()`
missed the `use vars`-style export declaration real core
`File::Path.pm` itself uses — an own-goal that would have misfired even
once `File::Path` is otherwise implemented). Verifying D121 also
widened **D110** to cover array/hash access, not just scalars — still
open.

**Tier 2 — high frequency, moderate cost:** `Time::Piece` (OO wrapper
over already-working `localtime`/`gmtime`/`strftime` primitives —
bigger surface than Time::Local for related value), `Storable::dclone`
only (not full `freeze`/`thaw` binary-format compatibility — needs a
genuinely new recursive deep-clone-with-cycle-detection primitive, but
the pattern already exists in `perl_dumper`'s `seen`-hash cycle guard to
copy from), `JSON::PP`-equivalent (encoder is feasible now — Perl's
`%.15g`-style float stringification already matches; needs new
string-escaping and a small recursive-descent decoder — moderate,
high-payoff), `Text::CSV_PP`-equivalent (needs a real quoted-field state
machine, not a naive `split`-based approach), `Hash::Util`.

**Tier 3 — lower priority for this project's sysadmin/CLI use case (vs.
web/app-framework modules):** `Try::Tiny` (mostly expressible with
existing `eval {}` — but fix D102 first, since Try::Tiny's whole point
is structured exception objects), `List::MoreUtils` (List::Util already
covers `uniq`; narrower marginal value), `Encode` (broad charset-table
surface; `use utf8`/`:encoding(UTF-8)` already cover the highest-frequency
case), `Term::ANSIColor` (cosmetic only).

**Explicitly out of scope:** anything web-framework-shaped (Moose/Moo,
Mojolicious/Dancer/Plack), `Digest::MD5`/`Digest::SHA`, non-SQLite DBI
drivers — XS-shaped or metaprogramming-heavy, doesn't match this
project's apparent sysadmin/CLI-tool target profile, consistent with the
existing "Full XS/DynaLoader" non-goal in CLAUDE.md.

## Runtime correctness gaps (fix alongside Tier 0, before building on top)

- ~~**D117**~~ — **FIXED 2026-09-10.** Was a hand-rolled digit-
  accumulation parser (plus a repeated-multiply exponent loop) instead
  of `strtod`; now scans the same decimal-only prefix and hands it to
  `strtod`. Also picked up `Inf`/`Infinity`/`NaN` string-coercion
  support (previously silently `0`). Split off **D125** (found while
  testing — `use`/`no` pragmas only parse at file top-level).
- ~~**D118**~~ — **FIXED 2026-09-10.** Was worse than described: the
  LIMIT argument was a hard *parse error*, not silently dropped. Both
  it and trailing-empty-trim are fixed, ahead of the JSON/CSV work they
  were prerequisites for. Found **D126** (split with a capturing-group
  pattern doesn't include the captures — pre-existing, unrelated to
  LIMIT/trim) while testing.

## MVP definition

**MVP = real-world scripts compile and run byte-for-byte correct vs.
real Perl, on a defined and growing corpus, with the harness staying
green as that corpus grows.** Concretely:

1. **D113 fixed first** (loud failures instead of silent no-ops) — this
   is a force multiplier: it turns every subsequent "does X work" check
   from a manual diff into a simple pass/fail, and it's what makes the
   next real-world survey trustworthy.
2. **Tier 0 correctness items (D111, D112, D114, D109-widened, D102,
   D115) fixed** — these are the silent-wrong-data bugs in idioms common
   enough to appear in nearly any real script.
3. **`make test-all` stays at 0 FAIL** as work proceeds — non-negotiable
   per existing project policy; every fix ships smoke+deep tests
   verified against real Perl, as already practiced.
4. **A standing, versioned real-world script corpus**, not a one-off
   survey — expand the "36 unmodified Debian scripts" pass into a
   tracked pass-rate metric (e.g. `tests/realworld/`), the same way
   `make test-all`'s 263/263 is tracked today. The 2026-09-09 survey's
   real number (6/36 clean, before D113's silent-failure bug is even
   accounted for) is the actual baseline to move — not vibes.
5. **Tier 1 parse gaps fixed** for the specific syntax shown to gate
   real scripts in the corpus (`__PACKAGE__` first — likely highest
   yield — then `qr//`, `map {{}}`, dynamic method dispatch, etc.),
   rather than committing to the full list speculatively.
6. **A first module-support slice**: D112 (module lexical scoping) plus
   a minimal Exporter/`@EXPORT` mechanism, enabling the Tier-1 CPAN
   module list above to actually be `use`-able as external `.pm` files
   rather than only as perlc-internal hardcoded dispatch.
7. **Explicit non-goals**, unchanged from existing project stance: full
   XS/DynaLoader, non-SQLite DBI, `given`/`when`, `format`/`write`,
   web-framework-shaped modules.

## Critical path (recommended order)

1. ~~**D113** — make failure loud.~~ **DONE 2026-09-10.**
2. **Re-run the real-world module survey** against the D113-fixed
   binary. Any survey run before step 1 wasn't measuring what it
   appeared to measure — treat the existing "36 scripts" numbers as
   provisional until re-run. **Still pending — next step.**
3. **Tier 0 fixes**: ~~D111 and D114~~ **DONE 2026-09-10** (both were
   contained/mechanical as expected). ~~D112~~ **DONE 2026-09-10** — did
   *not* end up being the block-scoping architectural change this step
   originally predicted; see TESTS.md's D112 write-up for the actual
   (smaller, different) fix and why the predicted approach was tried and
   rejected first. ~~D109~~ **DONE 2026-09-10** — reused `/e`'s existing
   machinery directly, smaller than predicted too; see TESTS.md. D109's
   harder remainder is now **D120**. Remaining: D110 and D120 are medium
   per the codegen review / D120's own write-up (reuse existing
   machinery, don't rebuild); D102, D115, and the newly-found D119 are
   small.
4. **Tier 1 parse gaps**, prioritized by what the re-run survey (step 2)
   actually shows blocking real files — `__PACKAGE__` is the strongest
   a priori candidate given how common the `bless {}, __PACKAGE__`
   idiom is.
5. **Minimal Exporter support** — D112's fix did not end up restructuring
   `inlineModules`'s token-splicing model (see above), so this item no
   longer has the "once D112 gives modules a real scope boundary"
   prerequisite it was originally framed around; it's still needed for
   "modules that just work because they're normal pure-Perl code," just
   as a standalone piece of work rather than a D112 follow-on.

Every step keeps the project's existing discipline: real-Perl-verified
smoke+deep test per fix, `make test-all` green before moving on, and
priority re-derived from actual code rather than synthetic corner-case
mining — the same methodology that already correctly demoted D101/D103
and promoted Getopt::Long/Data::Dumper/File::Basename ahead of the
original D-list.
