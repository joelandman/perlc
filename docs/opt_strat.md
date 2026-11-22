# perlc optimization strategy (session 2026-11-22)

Closed-body auto-parallel work through Stage 46 is done: counted FK
loops (`autopar`), outerpar for the mbs nested shape, and map/grep/
foreach-@arr plus range `$a[$i]`, list concat, and die slots (`mappar`).
This list is the review order for the next slices. Do not auto-parallel
arbitrary blocks. Every compiler fix ships smoke+deep tests vs real perl
(`tests/harness.sh` / `make test-all`) and stays byte-for-byte.

Knob for 1–2 and the existing mappar family: `PERLC_OPT_DISABLE=mappar`
(also off with `autopar`).

## Floor (already landed)

- Stages 34–43: unbox, flat rows, packflat, FK vectorize, `fkifconv`
- Stage 45 `autopar` counted FK loops; `outerpar` mbs nested `$j`
- Stage 46 `mappar`: closed map/grep/foreach-@arr, range `$a[$i]`,
  concat, die slots
- D207/D208 aliasing; D207b block save/restore; regex memo; Stage 40 `_ai`

## Next candidates

### 1. Append-only foreach into a different array — closed this session

`foreach (@a) { push @b, $_ * 2 }` is map-concat with a named destination.
Purity currently refuses `push`. Same pool, per-chunk buffers, ordered
join. Snapshot `@a`’s length; `@b` is only appended. Live-length
`push @a` on the iterated array stays sequential.

### 2. Ordered integer reductions — closed this session

`foreach (@a) { $s += $_ }` and `foreach my $i (0..N-1) { $s += $a[$i] }`.
Per-chunk i64 partials plus an ordered join match perl for an unboxed
int accumulator. Float `+=` stays sequential (association would miss
the byte-identical gate). Mixed `+=`/`-=` on one int var is a net
delta from 0 per chunk, then `real_s += sum(partials)`.

### 3. Row-element alias PV (documented D208 leftover) — closed this session

`$z = $row->[$i]; $z->[0] = 9` is a `PERL_PAIR_ELEM` view of the parent
`CPLX_ROW` (`pval` = row, `matchpos` = pair index). Clone/assign of that
tag share the view; `$z->[0] = 9` writes `data[2*i]` in place. FLOAT_PAIR
and empty-row copies stay value-copies (mb/nbody). `@$z` snapshots this
view PV only. Tests: `tests/pairelem_{smoke,deep}.pl`.

### 4. Regex residual (slowest of the four benches)

Memo slots and promote-inline already landed. regex3m is ~90% libpcre2
(~2.9× vs perl). **Closed this session (perlc leftover, not pcre2):**
match-subject D105 promote skip (`emitMatchSubject` on ScalarVar /
ArrayElem / HashElem — stringify does not create a second array-ref
alias) and statement-context `$count++` / `$count--` on a boxed PV
(`perl_inc` in place; post++ clone only when the value is consumed).
File-scope `my $count = 0` is not int-promoted, which is the regex3m
shape. Tests: `tests/regex_residual_{smoke,deep}.pl`.

**Item 8 (pcre2 leftover) measured this session, not landed:** a C
concatenation scanner for `/item_\d+_match/` matched perl but lost to
pcre2 JIT (~33% slower on `regex_heavy` 1e6×40 at `-O2` LTO). The
cliff is a different engine (SIMD/batch/hyperscan), not a hand-rolled
boolean NFA. Parallel boolean `grep { /pat/ }` closed as item 7.

### 5. Closed `sort` with a pure comparator — closed this session

Perl’s sort is a stable mergesort. perlc now uses the same contract
(replacing C `qsort`, which is not stable). Closed comparators that only
read `$a`/`$b`, captures, and body-`my` fan out on the Stage-45 pool at
bound `>= PERLC_AUTOPAR_MIN`, then a stable merge of the sorted runs.
Default `$a cmp $b` / `<=>` / list-form use the builtin num/str paths
(also stable + parallel). Impure comparators (`push @observed, $a`) and
`sort SUBNAME` stay sequential. Ties keep original order. Knob:
`PERLC_OPT_DISABLE=sortpar` (also off with `autopar`). Tests:
`tests/sortpar_{smoke,deep}.pl`.

### 6. More counted loops that are closed but not FK — closed this session

Stage 45 workers are pure f64 FK. Stage 46 already covered map/grep/
foreach-range. A boxed but closed C-style `for (my $i = LO; $i < HI; $i++)`
with disjoint `$a[$i]` stores (no flat rows) now reuses the rangepar
worker. `$i <= HI`, `++$i`, and `$i += 1` are the same counted shape.
`$i` is read-only in the par body (a body `$i++` stays sequential, matching
C-style semantics). Live bounds (`$i < foo()`, `$i < $n` while `$n +=`)
stay sequential. Knob: `PERLC_OPT_DISABLE=mappar` (also off with
`autopar`). Tests: `tests/cforpar_{smoke,deep}.pl`.

### 7. Parallel boolean `grep { /pat/ }` — closed this session

`mapParBodyPurity` refused every regex because `$1`/`$&`/`$``/`$'`/`%+`
are process-global capture slots. Sequential grep leaves the last
successful match in those slots; parallel workers would race them.
Boolean literal `NK::RegexMatch` (no `/g`, no subst, no interp, no
`$s =~ EXPR`) is now allowed in map/grep/foreach-par bodies when this
compilation unit does not read those globals (`scanMatchGlobalUse` sets
`matchAmpNeeded_` / `matchCapsNeeded_`; string `eval` forces both on).
pcre2 `match_data` is already TLS; capture install is skipped when the
flags are off. A `$1` anywhere in the unit keeps every regex sequential.
`grep { /pat/ }`, `grep /pat/, LIST`, `!/pat/`, and `/i`/`m`/`s`/`x`
share the same purity. `map { /pat/ }` stays sequential: list-context
match yields the capture list (empty on failure), not a 1:1 boolean.
Knob: `mappar` (also off with `autopar`). Tests:
`tests/greppar_{smoke,deep}.pl` — those files must not mention `$1`/`$&`
or the eligible greps stay sequential.

### 8. Simple boolean match vs pcre2 JIT — measured, not taken

After memo slots, promote skip, and void `$count++`, regex3m is ~90%
libpcre2. A boolean concatenation matcher (`^? (lit | \d | \d+ | \d* )* $?`,
empty flags, ASCII `\d`, `$` before a final newline) was written on the
memo path and gated vs perl. It is **not landed**: at `-O2` LTO,
`bench/regex_heavy.pl 1000000 40` was ~1.08s with the scanner vs
~0.81s with pcre2 JIT (same match count). A C byte-scan of short
subjects does not beat JIT. Remaining of (4): a different engine
(SIMD/batch/hyperscan), not another per-call NFA.

### 9. Closed `index` / `rindex` / `substr` in mappar — closed this session

`length` / `lc` / `uc` were already eligible; `index`, `rindex`, and
read-only 2/3-arg `substr` were still default-false in `mapParBodyPurity`.
`grep { index($_, "x") >= 0 }` and `map { substr($_, 0, 2) }` now fan
out with the rest of Stage 46. 4-arg `substr($s, off, len, $repl)` and
lvalue `substr(...) =` stay sequential (in-place write of `$s`). Knob:
`mappar`. Tests: `tests/idxpar_{smoke,deep}.pl`.

### 10. Closed `join` / `sprintf` / `hex` / `oct` in mappar — closed this session

`join`, `sprintf`, `hex`, and `oct` are scalar, side-effect-free on a
closed body. `map { join(":", $_, "x") }` and `map { sprintf("%04d", $_) }`
now fan out with Stage 46. `join(",", @outer)` still refuses (named
array reads stay sequential). Knob: `mappar`. Tests:
`tests/joinpar_{smoke,deep}.pl`.

### 11. Closed `split` / `pack` in mappar — closed this session

`split` is a list-yield (map-concat) or a scalar field-count (grep).
`pack` is scalar. Both were default-false in `mapParBodyPurity`.
`map { split /:/ }` / `map { split ',', $_ }` / `map { pack("C", length($_)) }`
now fan out with Stage 46. The interp/split pcre2 LRU is already
per-thread; regex split skips `%+`/`$1` install unless the unit reads
those slots. A `$1` anywhere, `/g`, and `pack("C*", @outer)` stay
sequential. Knob: `mappar`. Tests: `tests/splitpar_{smoke,deep}.pl`
— those files must not mention `$1`/`$&`.

### 12. Closed `unpack` in mappar — closed this session

`unpack` is a list-yield (map-concat of fields) or a scalar first field
(grep). It was still default-false after pack landed in (11).
`map { unpack("C*", $_) }` and `map { unpack("C", pack("C", length($_))) }`
now fan out with Stage 46. An outer format scalar (`unpack($fmt, $_)`)
fans out when `$fmt` is a file-scope `my`/`our` (item 15). Knob:
`mappar`. Tests:
`tests/unpackpar_{smoke,deep}.pl` — print numeric fields, not packed
binary.

### 13. Closed `reverse` in mappar — closed this session

`reverse` is list-context in a map block (`map { reverse $_ }` is a
1-element identity) and string-reverse under `scalar` or grep.
`emitArrayPtr` now skips `sval=="scalar_ctx"` so `map { scalar reverse $_ }`
does not flatten as list reverse (same stamp as D142 localtime).
`reverse @outer` stays sequential. Knob: `mappar`. Tests:
`tests/revpar_{smoke,deep}.pl`.

### 14. Closed interpolated match / `qr` in mappar — closed this session

Item 7 allowed literal `/pat/` only. The interp LRU is already
per-thread (item 11) and capture install is skipped unless the unit
reads `$1`/`$&`. Closed `/$p/`, `$_ =~ /$p/`, and `$_ =~ qr/pat/` now
fan out when `$p`/`$re` is `$_` or a body-`my`. A file-scope outer
`$pat` fans out (item 15). `/g`, subst, a nested-lexical `$pat`, and
`map { /$p/ }` (list-context capture list) stay sequential.
Knob: `mappar`. Tests: `tests/interppar_{smoke,deep}.pl` — those files
must not mention `$1`/`$&`.

### 15. Read-only file-scope scalars in mappar — closed this session

Workers only had `$_` / body-`my` in scope, so an outer `$pat` / `$fmt`
/ `$k` stayed sequential even when the body only read it. File-scope
`my`/`our` scalars live in GVs `lookupVar` can load from a worker.
`grep { /$pat/ }`, `map { length($_) + $k }`, and `map { unpack($fmt, $_) }`
now fan out. A nested lexical (`sub { my $p; grep { /$p/ } }`) and any
write of an outer scalar stay sequential. Named arrays stay sequential.
Knob: `mappar`. Tests: `tests/roscpar_{smoke,deep}.pl` — those files
must not mention `$1`/`$&`.

## Non-goals (do not review next)

- Arbitrary-block parallel
- Live-length `foreach (@a) { push @a, … }`
- Hash `keys`/`each` (order)
- `while` with a live bound
- `$i++` then `$a[$i]` (overlapping stores)
- **Parallel f64 reductions / `--fast` / a reassoc pragma.** IEEE add is
  not associative, so a chunked join is a different program than perl’s
  left-to-right `+=`. Default sequential f64 `+=` stays the contract.
  A file-wide `--fast` is too coarse (it would also become a bucket for
  other unsound flags). A per-loop annotation is the only honest shape,
  and even that is not worth the product cost: none of the four landing
  benches is gated on a large f64 sum (nbody’s `$e +=` is 5 bodies;
  mbs is already FK/autopar of independent iterations),
  `PERLC_AUTOPAR_MIN=8192` means an unboxed sequential add is already
  microseconds, and `make test-all` must never compile with reassoc.
  If this is ever revisited: measure a 1e6/1e7-element sum first; name
  the opt-in after the permission (`reduce(par)` / `--assoc-fp-reduce`),
  not `--fast`.

## Practical order

(1)–(7) and (9)–(15) landed this session. (8) is the pcre2 leftover of
(4): a C concatenation scanner was measured slower than JIT and was
not landed. Remaining of (4) is a different match engine. Parallelism
will not move it.
