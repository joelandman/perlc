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
(~2.9× vs perl). Remaining perlc cost: match-subject promote skip, void
`$count++` without clone/free, and only then a different match strategy.
Parallel `grep { /pat/ }` is a separate, smaller slice: capture globals
(`$1`, `$&`) are why regex is refused in `mapParBodyPurity` today.

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

## Practical order

(1)–(3) landed this session. Next review: (4) is the remaining bench
cliff that parallelism will not move. Then (5) closed sort, (6) boxed
closed counted `for`.
