# Project Plan: Atomic Rewrite of threads::shared

## Problem statement

The current threads::shared implementation in `src/runtime.c` couples *visibility* (cross-thread write propagation) to *mutual exclusion* (lock/unlock). The runtime comment on `perl_assign` makes this explicit:

> No implicit mutex here — caller must hold lock() for concurrent safety.

But the codegen never inserts that lock for plain `$x = ...` assignments. The result: shared-scalar writes by the main thread are not visible to worker threads polling in a busy-wait, e.g. `until ($phase) { sleep 1; }`. This pattern is what `tests/tree.pl` uses, and it hangs forever.

The minimal fix is in place today (release fence on write, acquire fence on read via `perl_shared_load`; see git log "release barrier in perl_assign"). That fix solves the *visibility* problem and lets the existing test suite pass, but it leaves the larger design issues untouched:

1. **Read-modify-write is racy.** `$counter++` on a shared var still loses updates. Users must wrap in `lock($counter)`, and the comment on `perl_assign` says the runtime can't fix this for them.
2. **The runtime conflates "this is shared" with "this has an embedded mutex."** `PerlSharedVar` always carries a `pthread_mutex_t` and `pthread_cond_t` even for scalars that are never locked. That's ~64 bytes of overhead per shared scalar.
3. **Memory ordering is implicit.** Every shared-var read goes through `lock()`-or-die, with the implied `seq_cst` ordering. Tight loops that would prefer `acquire` only have to take the heavier fence.
4. **Two of the three pre-existing patterns that real Perl needs are unsupported in the codegen:**
   - **Named-sub closure capture.** `sub worker { until ($shared) { ... } }` — the worker body sees `$shared` as undef. Only `my $worker = sub { ... }` works. `tests/tree.pl` uses the named form.
   - **`$_->method()` dispatch on a blessed/non-blessed returned object.** `$_->join` on a thread returned by `threads->create` fails with "Can't call method 'join' on unblessed reference".

This plan covers (1)–(3). The two codegen limitations are out of scope; they need their own plans and likely larger refactors (e.g. lifting the named-sub body into a closure-shaped AnonSub at parse time).

## Goals

**Correctness**
- Shared-var writes are visible to other threads without requiring `lock()`.
- `++`/`--`/compound-assign on a shared var are atomic without requiring `lock()`.
- The full Perl `threads::shared` semantics still pass when the user *does* call `lock()` (no regression).
- The new runtime is sound under TSan/TSAN (or equivalent) on x86_64 and aarch64.

**Performance**
- The hot path of a shared-scalar read or write (no lock, no contention) is a single atomic load or store, with no function call to a runtime helper for ordering — the fence is hoisted by LLVM or omitted entirely on x86.
- No per-scalar heap allocation for a mutex unless the user actually calls `lock()` on it.
- Locked contention is no worse than today.

**Maintainability**
- One source of truth for the memory model: `stdatomic.h` (C11) atomics.
- `PerlSharedVar` is replaced with a tagged-pointer scheme: shared scalars carry a flag bit on the `PerlValue*` itself; arrays/hashes that need `lock()` allocate a `SharedMutex` lazily.
- The runtime comment "caller must hold lock() for concurrent safety" goes away.

## Non-goals

- Full Perl 5 thread semantics (signals, `kill`, `eval` across threads, etc.). The current runtime doesn't support these either, and they're not in the test suite.
- Re-implementing threads on top of `<thread>` from C++20. The runtime is in C; changing the language boundary costs more than it buys.
- Optimising shared-array/hash operations beyond what falls out of the atomic-scalar work. The current mutex-based arrays/hashes are correct and tested; rewriting them is a separate project.

## High-level design

### Data layout

Replace `PerlSharedVar` with two separate concepts:

- **Shared-scalar flag.** Reuse the existing `PV_FLAG_SHARED` bit on the `PerlValue*` struct. A shared scalar is still a `PerlValue*` allocated by `perl_make_shared_scalar`, but the cell *is* the scalar, not a wrapper. No mutex is allocated until the user calls `lock($x)`.

- **Lazy `SharedMutex`.** A new heap-allocated struct holding a `pthread_mutex_t` and `pthread_cond_t`. Created the first time `lock($x)`, `cond_wait($x)`, or `cond_signal($x)` is called on a shared scalar. Stored in a side table keyed by the `PerlValue*` (or as a pointer in the value's `pval` slot with a tag-bit discriminator; see below).

Trade-off: the side table needs a global mutex for inserts. But inserts only happen on the first `lock()` call per scalar, and the lookup is O(1) and lock-free on the fast path (read-mostly hash with epoch-based reclamation, or a simple `__atomic_load` of an "installed?" flag followed by a lock).

For shared arrays and hashes, the existing approach (allocate a `SharedMutex` as part of the array/hash struct) stays. Those are already heavier-weight, and `lock(@arr)` is always required for thread-safe access. The atomic treatment only buys us anything for scalars.

### Per-scalar atomic cell

For shared scalars, the writer side (`perl_assign`) becomes:

```c
PerlValue *cur = __atomic_load_n(&shared_cell->value, __ATOMIC_ACQUIRE);
do {
    new = *src;            /* build the new value */
    /* string/array/hash reference-count handling unchanged */
} while (!__atomic_compare_exchange_n(&shared_cell->value, &cur, &new,
                                       /*weak=*/0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
```

For a writer that doesn't need the old value, a simple `__atomic_store_n(..., __ATOMIC_RELEASE)` is equivalent and faster. The existing `perl_assign` always does refcount handling, so the CAS loop is the right primitive.

For readers:

```c
PerlValue *v = __atomic_load_n(&shared_cell->value, __ATOMIC_ACQUIRE);
```

For the `++` / `--` / compound-assign codegen, the runtime exposes new entry points:

```c
PerlValue *perl_atomic_inc(PerlValue *pv);   /* post-increment */
PerlValue *perl_atomic_dec(PerlValue *pv);
PerlValue *perl_atomic_add(PerlValue *pv, PerlValue *delta);
```

These all do the CAS loop internally. The codegen detects `$x++` / `$x += N` on a shared var and dispatches to these helpers instead of `perl_assign`.

### Lock path

`perl_lock_shared(PerlValue *pv)` becomes:

```c
SharedMutex *mu = get_or_install_mutex(pv);   /* lazy install */
pthread_mutex_lock(&mu->mtx);
push auto-unlock frame;                       /* existing behavior */
```

`get_or_install_mutex` does a lock-free read of `pv->pval` (using a tagged-pointer convention or a global hashmap). On miss, takes a global install mutex, re-checks, allocates, publishes. Standard double-checked locking.

The auto-unlock stack in the runtime stays the same.

### Codegen surface

The codegen's `sharedScalarNames_` set (introduced in the minimal fix) is extended to influence more emission points:

| Shared-var operation | Old codegen | New codegen |
|----------------------|-------------|-------------|
| `$x` (read)          | `load(slot)` (or `perl_shared_load` after minimal fix) | `__atomic_load` via new `perl_atomic_load` |
| `$x = v`             | `perl_assign(slot, v)` | `perl_atomic_store(slot, v)` |
| `$x++` / `$x--`      | `perl_add` (racy)       | `perl_atomic_inc` / `perl_atomic_dec` |
| `$x += N`            | `perl_add` (racy)       | `perl_atomic_add` |
| `lock($x)`           | `perl_lock_shared(slot)` | same; lazy-installs the mutex |

The shared array/hash set is unchanged — those still go through the existing mutex-based accessors.

### Memory model

- All atomic operations use `acquire`/`release` for the cell and `acquire`/`acq_rel` for the CAS loop. `seq_cst` is unnecessary because the only ordering constraint between threads is "I see your write / you see my read," and there are no dependent loads across the fence on the reader side that would need full sequential consistency.
- On x86, every `acquire` load and `release` store compiles to a plain `mov`. On aarch64, `ldar`/`stlr` are emitted by LLVM. The runtime does nothing arch-specific.

## Phases

### Phase 0: Benchmark and baseline (½ day)

Before changing anything, capture:
- Runtime of `tests/threads.pl` (the existing patterns)
- Runtime of `tests/tree.pl` *as compiled today* (it hangs, so document that)
- Runtime of a microbenchmark that does `lock($x); $x++` in a tight loop 10⁷ times

Store these in `benchmarks/threads-baseline.txt`. Without numbers, we can't prove the rewrite is faster.

### Phase 1: Test harness for the new model (1 day)

Write `tests/threads_atomic.pl`. Cover:
- Shared-scalar write visible to another thread without `lock()` (the `until ($phase)` case).
- `$counter++` on a shared var across N threads produces exactly N+M total increments (the racy case the minimal fix doesn't solve).
- `lock($x); $x++` still works (no regression).
- `cond_wait`/`cond_signal` still works.
- `lock($x)` releases on scope exit (existing test in `threads.pl`).
- A TSan-instrumented build of the same test is clean (run under `-fsanitize=thread`).

This test becomes the contract the new runtime has to satisfy. Land it as a failing test (`make test` should fail until Phase 4 is done).

### Phase 2: Runtime atomic primitives (2 days)

Add to `src/runtime.c`:
- `perl_atomic_load(PerlValue *pv)` — `__atomic_load_n(__ATOMIC_ACQUIRE)`
- `perl_atomic_store(PerlValue *pv, PerlValue *v)` — refcount + `__atomic_store_n(__ATOMIC_RELEASE)`
- `perl_atomic_inc(PerlValue *pv)`, `perl_atomic_dec(PerlValue *pv)`, `perl_atomic_add(PerlValue *pv, PerlValue *delta)` — CAS loops
- `perl_atomic_swap(PerlValue *pv, PerlValue *v)` for completeness (not yet used but cheap)
- `perl_lock_shared(PerlValue *pv)` rewritten with lazy mutex install
- A small `SharedMutex` allocator with a global install lock

Update `src/runtime.h` to declare them. The new cell layout is the same `PerlValue` struct, but the install of a `SharedMutex` is delayed until `lock()` is called.

### Phase 3: Codegen wiring (1 day)

- Add `RT("perl_atomic_load", ...)` etc. to the function table in `codegen.cpp`.
- In `case NK::ScalarVar`, when the name is in `sharedScalarNames_`, emit `perl_atomic_load` instead of a plain load.
- In the assignment codegen (`case NK::Assign` and `case NK::CompoundAssign`), when the lhs is a shared scalar:
  - For `=`, emit `perl_atomic_store`.
  - For `++`/`--`, emit `perl_atomic_inc`/`perl_atomic_dec`.
  - For `+= N` etc., emit `perl_atomic_add` (need to handle the operator-to-helper mapping; consider extending the `BinOp` codegen to recognise a shared-lhs case, or add a new AST node).
- Remove the `perl_shared_load` call from the minimal fix; the new path subsumes it.

### Phase 4: Validation (1 day)

- Run `make test`. All 36 existing tests must still pass.
- The new `tests/threads_atomic.pl` from Phase 1 must pass.
- Re-run the Phase 0 benchmarks. Expected improvements:
  - Hot-path shared-scalar read: ~5× faster (no function call, no fence on x86).
  - `$counter++` under contention: roughly proportional to thread count × atomic throughput.
  - `lock($x); $x++`: same or slightly better (CAS replaces a load+add+store that wasn't atomic).
- Run under TSan. If TSan reports races, the memory model is wrong and we don't ship.

### Phase 5: Documentation (½ day)

- Update `THREADS_SHARED_HASH.md` to reflect the new model.
- Write a new `THREADS_SHARED_ATOMIC.md` explaining what the user needs to know: nothing changes at the Perl source level, but the *cost model* is now: a shared scalar is cheap; `lock($x)` is more expensive than before because it now actually installs a mutex.
- Update `CLAUDE.md`'s "Known Limitations" / "Implementation Details" sections to drop the "caller must hold lock() for concurrent safety" note and the "thread-isolation is a deep copy" caveat (the deep copy is still there for closures, but the visibility guarantee is now stated correctly).

## Risks and mitigations

**Risk 1: ABI break for `PerlSharedVar`.** The current struct has `PerlValue` as its first member so `(PerlValue*) == (PerlSharedVar*)`. Removing it is a binary-compat break — except nothing links against the runtime except the compiler-emitted code in the same process, so it's source-compat only. Mitigation: do the change behind a single `git` commit, re-run `make clean && make`.

**Risk 2: The CAS loop in `perl_atomic_assign` is wrong for string/array/hash payloads.** The current `perl_assign` carefully ref-counts; the new code must too. Mitigation: keep the existing refcount logic verbatim, only swap the final store for `__atomic_store_n`. Unit-test the path with shared scalars holding strings, array refs, and hash refs.

**Risk 3: TSan reports a false positive on x86 because the fence-only model has no actual atomic op.** The minimal fix uses `__atomic_thread_fence`; an atomic load/store would silence TSan more cleanly. Mitigation: use `__atomic_load_n`/`__atomic_store_n` (not fence+plain load) in Phase 2. TSan understands the former, not always the latter.

**Risk 4: The `int`/`float` specialisation paths in the codegen skip the `case NK::ScalarVar` read entirely (they read from a separate `intScopes_` map).** A shared var must not be allowed to use those paths. Mitigation: in the `My` handler, when `isShared` is true, *also* clear any `intScopes_` / `floatScopes_` entry for that name, and document that shared scalars cannot be unboxed. (Today the shared-var path already does the right thing by storing the `PerlSharedVar*` in the regular `scopes_` map and not unboxing — confirm and add a comment.)

**Risk 5: The `closure` capture path (`case NK::AnonSub`, Phase 1) needs to know the captured shared var should be loaded with `perl_atomic_load`, not the existing per-capture plain load.** Mitigation: extend the capture list with a "is this capture shared?" flag, set at capture time by looking up `sharedScalarNames_`.

## Out-of-scope but adjacent

These were found while writing this plan. Each is a separate project.

- **Named-sub closure capture.** Real Perl closures don't care whether the sub is `sub name { ... }` or `my $name = sub { ... }`; the compiler currently only captures for the latter. Fixing this would let `tests/tree.pl` work without modification. ~2 days, mostly parser + codegen.
- **`$_->method()` dispatch on objects returned by class methods.** `threads->create` returns a `PERL_THREAD`; calling `->join` on it needs a class-aware dispatcher. The hardcoded `threads` shortcut in `case NK::MethodCall` should be generalised to "if `n.left` is a known class method receiver, dispatch as a class call; otherwise dispatch on the blessed_class if any, else on the value's tag". ~3 days, with risk of regressing existing OOP.
- **Shared array/hash `++` atomicity** (e.g. `push @shared, $x` is currently not atomic without `lock(@shared)`). The mutex-on-the-array-struct pattern handles this correctly when the user locks; making it atomic-without-lock would need a CAS on the array's `len`/`elems` pair, which is more invasive than the scalar case. Defer.

## Estimated total

~6 working days, plus Phase 0–1 setup which can run in parallel. The biggest unknown is the codegen work for compound-assign on shared scalars — if a clean `perl_atomic_add` is hard to plumb through the existing `BinOp("+=")` path, the alternative is a new AST node `NK::CompoundAssignAtomic`, which is mechanical but adds boilerplate.

## Success criteria

- `make test` reports 36/36 + the new `tests/threads_atomic.pl`.
- TSan run is clean.
- `tests/threads.pl` runtime is within ±10% of the baseline (the test exercises the locked path; we don't want to regress it).
- The release-notes say "users no longer need to call `lock($x)` for a write to a shared scalar to be visible to other threads; they still need it for read-modify-write atomicity (`$x++`)". This is a strict superset of today's guarantees.
