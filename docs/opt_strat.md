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

Still open: a different match strategy (the pcre2 cliff). Parallel
`grep { /pat/ }` is a separate, smaller slice: capture globals (`$1`,
`$&`) are why regex is refused in `mapParBodyPurity` today.

### 5. Closed `sort` with a pure comparator

Block comparators that only read `$a`/`$b` can merge in parallel and stay
stable. Effort is higher than (1)/(2): comparator purity, list vs
`$a cmp $b`, and ties.

### 6. More counted loops that are closed but not FK

Stage 45 workers are pure f64 FK. Stage 46 covers map/grep/foreach. A
boxed but closed C-style `for` (`$a[$i] = …` without flat rows) can reuse
the rangepar worker. Smaller unique gain than (1)–(2).

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

(1)–(3) and the perlc-side of (4) landed this session. Remaining of
(4) is the pcre2 match strategy (parallelism will not move it). Then
(5) closed sort, (6) boxed closed counted `for`.
