# Resumption memory: real-world RUNTIME correctness (MVP item 4)

_Written 2026-11-08. This is a handoff for whoever resumes the four runtime
byte-for-byte gaps. It records the current state, the exact gaps with their
root causes (the hard-won part that would otherwise have to be re-derived), and
the highest-leverage fix approach. It is NOT a task list to finish now._

## How to read this

perlc must match real Perl **byte-for-byte on combined stdout+stderr**. The
runtime survey is `tests/realworld/run_compare.sh` (compile-only cousin is
`run.sh` / `make test-realworld`, which never executes a binary). The versioned
outcome lives in `tests/realworld/last-compare-report.txt` (regenerated with a
fresh timestamp by the runner) and `tests/realworld/baseline-compare.txt` (the
regression baseline).

## State at handoff

Snapshot below is 2026-11-08 (HEAD `7df4f3b`). D215–D257 landed
on top of it. `Pod::Text` and `Pod::Man` are off the skip list. A
small `parse_string_document` of each matches perl
(`tests/d247_pod_text.sh`; the Man program is the same shape, with
`SOURCE_DATE_EPOCH=0`). `Pod::Text`'s `output_string` path matches
too (`tests/d257_pod_text_string.sh`): tied `PRINT`/`PRINTF` write
through `$$$it`. The open list is in TESTS.md ("Open after D257")
and under "Recommended next move" at the bottom of this file.
Scalar FETCH/STORE and `Encode->getEncoding` are still open.
Runtime `require` finds the file (including host archlib, where
`Encode.pm` lives) and returns true. It does not execute the file.

- 2026-11-08 tree was clean at `7df4f3b` (D214: the runner + gate). The fixes described as reverted in the sections below were the pre-D215 attempts. D217 re-landed symmetric `qw`. The other three (`delete $$`, `push` fat-comma, `Encode->getEncoding`) stayed reverted.
- Gate: **HOLD** (no gate=1 failure, no regression vs baseline).
- Metric (31 entries): pass=3, fail-runtime=4, skip-safe=22, skip-missing=2.
  compile-rate=**100%** (29 non-missing), runtime-correct=**42.9%** (3/7
  runnable), mvp-pass-rate=**10.3%** (3/29).
- gate=1 (must pass, trips the gate if any fail): `json_pp`, `shasum`,
  `debconf-escape` — all **pass**.
- gate=0 (tracked capacity gaps, NOT hard-gated): `piconv`, `pod2text`,
  `pod2man`, `pod2html` — all **fail-runtime**.

## The four runtime gaps and their root causes

Every gap sits behind one or two *foundational* interpreter gaps, not a missing
single feature.

### piconv (was: `Encode->getEncoding` missing)
- **Was fixed then reverted:** the `Encode->getEncoding` method dispatch in
  `src/runtime.c` (`perl_encode_call`, dispatch at the `obj->sval == "Encode"`
  arm) was missing — it now emits `piconv: unknown scheme:` instead of the old
  `Can't locate object method "getEncoding"`. Also added the `enc_is_known`
  helper + the `getEncoding` arm in the dispatch guard. (All reverted; the tree
  is clean.)
- **Current blocker:** `$scheme` comes out empty at the `die "$name: unknown
  scheme: $scheme"` path (line 122). Root cause is the **`do { if/else }`
  block-value codegen gap** below — piconv sets `$scheme` with a `do { if … }`
  block, and perlc returns that block's value as undef/empty.

### pod2text / pod2man / pod2html (Pod trio)
- **D216 fixed the cross-function `my` capture** on the `--do-lib` path.
  A file-scope `my` (`Pod::Text`'s `my ($NBSP, $SHY)`, same shape in
  `Pod::Man`) used from a named sub, including `s{$NBSP}{...}`, verified
  as `Referring to an instruction in another function!` (`%NBSP = load ptr,
  ptr %g.NBSP`) because the do-lib slot was an alloca in `__perlc_do_run`.
  The slot is a module-level global now; the cell is still the D58
  registry value. A normal (inlined) compile already used a global, and
  the simple `sub g { return $X }` case already worked.
- **D215 fixed the `do { if/else }` block-value gap.**
- **D217–D231 (2026-11-09):** `qw/.../` compiles. `Pod::Simple` is off
  the pragma skip list. `use Pod::Simple; Pod::Simple->new` matches
  perl (`$VERSION` 3.48, `ref` Pod::Simple, `expand_verbatim_tabs` 8,
  no extra stderr). `Pod::Text` and `Pod::Man` stay skipped.
  `delete $$self{key}` and `push`/`unshift` fat-comma are still open,
  and `perl_tie` does not install PRINT magic, so a parse that writes
  through `Pod::Simple::TiedOutFH` is not covered by the constructor
  check.
- The Pod module tree (Pod::Simple + Text + Man + HTML, ~5k lines, closures,
  refs, shared lexicals) is deep; every fix surfaces 2–3 more. Judged out of
  scope for a single session.

## The block-value codegen gap — highest leverage, foundational

**Symptom:** `do { scalar }` returns its value, but `do { if (cond) {…} else
{…} }` (and any block whose last statement is an if/else, e.g. an anon-sub
return) returns **undef/empty** instead of the taken arm's value. Verified
deterministic: `do { if(1){"a"} else {"b"} }` → empty on 30/30 runs.

**Root cause:** `CodeGen::emitBlockLast()` (`src/codegen.cpp`) handles a trailing
`NK::If` by calling `emitStmt`, which emits the if/else via a **merge block with
no value** — it never captures the taken arm's value into the block's result.
`do { }` in expression position routes through `emitBlockLast`, so the value is
lost. (Confirmed this is **pre-existing**: on clean HEAD the isolated test
reproduces empty; my reverted changes never touched `emitBlockLast` or the
if/else merge.)

**Where the fix goes:** `emitBlockLast()` — add an arm for a trailing
`NK::If` that captures each arm's last-expression value into an entry-block
alloca (which dominates the merge block, so it's safe to phi/select) and selects
the taken one at the merge. `createEntryAlloca()` (`src/codegen.cpp`) already
places allocas in the function entry block; a per-call capture flag member on
CodeGen decides whether a trailing-if should capture (null in the common case
where an if is a bare statement and its value is discarded).

**Why this one first:** it is the single highest-leverage move — it unlocks
piconv's remaining gap AND provides the largest wedge into the Pod trio (which
also needs the cross-function `my`-lexical capture, but the block-value fix
clears the `do { }` idiom in parallel). It is a core change with regression
risk, so do it deliberately with the full `make test-all` (must stay 0 FAIL) +
the real-world gate after.

## The four fixes that were made, then reverted (for this clean handoff)

All are reverted; the tree is at clean HEAD. (The stash that once held them was
popped and the tree then `git checkout -- .`-reverted, so they are NOT recoverable
now — the four fixes are described in full in this doc's list so a clean re-fix is
straightforward if wanted.)

1. `qw/…/` single-char delimiter nesting — `src/lexer.cpp` (a guard so a
   delimiter-char inside the body didn't abort the nesting scan). This was
   crashing **every** Pod module compile.
2. `delete $$ref{key}` / `$$ref[i]` double-sigil — `src/parser.cpp`
   (`parseDelete`): the second `$` had to be skipped *before* reading the name.
3. `push`/`unshift` bareword `=>` (fat-comma) — `src/parser.cpp`: the
   arg-separator loops accepted only `COMMA`; added `FATARROW` so
   `push(@a, alt => 1)` parses. Fat-comma already worked in hash literals and
   as statements; only list-operator arg lists missed it.
4. `Encode::getEncoding` dispatch + `enc_is_known` helper — `src/runtime.c`.

## Gotchas learned while working here

- **Stale binaries.** The `perlc` binary in the repo can be built from older
  sources; a test that "worked once then broke" is usually a rebuilt binary,
  not nondeterminism. `run_compare.sh` rebuilds perlc itself. To prove a bug is
  pre-existing, `git stash` the working tree, rebuild at clean HEAD, and
  re-run in isolation.
- **The em-dash `—` breaks the `edit` tool** and multi-line Python heredocs. Use
  `python3` with explicit `\n`-joined strings, or match minimal unique anchors.
- **Gate mechanics:** `run_compare.sh` trips the gate on any gate=1 non-pass or
  any regression from `baseline-compare.txt`. `COMPARE_BASELINE_UPDATE=1`
  refreshes the baseline; leave it off unless you intend to lower the bar.

## Recommended next move

**D215 landed (2026-11-08).** `emitBlockLast` now captures a trailing
`if`/`unless` (taken arm, or the last false condition; `unless` yields the
original condition). `make test-all` is 644/644 after D216.

**D216 landed (2026-11-08).** The cross-function file-scope `my` capture
is fixed on the `--do-lib` path (`do`/`require`): the slot is a
module-level global again, and the PerlValue still comes from the D58
registry. The inlined (non-`do`) compile already used a global.

**D217–D257 landed (2026-11-09).** `make test-all` is 718/718. Symmetric `qw`, the Pod::Simple
use-chain, and `Pod::Simple->new` match perl
(`tests/d227_pod_simple.sh`). `Pod::Text` and `Pod::Man` compile and
are inlined. The small NAME-section samples match perl, including
Pod::Man's UTF-8 coding line and Pod::Text `output_string`
(`tests/d257_pod_text_string.sh`). The coding line depends on
`require Encode` being true: Encode.pm is in archlib, which D254 adds
to the runtime search after `-I` and `PERL5LIB`. The require does not
run the file. `Encode::encode` for the ASCII sample is the existing
native function, reached because STDOUT has no UTF-8 PerlIO layer, so
Pod::Man's `ENCODE` flag stays on.

Tied handles call `PRINT` / `PRINTF` / `CLOSE` / `UNTIE`. `$$$name`
is a nested scalar deref, so `TiedOutFH`'s `$$$it .= $x` updates the
caller's scalar. Scalar `FETCH` / `STORE` are still absent.
`$hash{key}++` still does not stick.

Next: `Encode->getEncoding` for piconv.
`parent` stays on the skip list (ISA injection is already native).
Do not refresh `tests/realworld/baseline-compare.txt` unless
`COMPARE_BASELINE_UPDATE=1`.
