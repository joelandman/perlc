# threads::shared Atomic Memory Model

## Overview

`use threads::shared` is now backed by a *tagged-cell* memory model with
acquire/release ordering. The user-facing behaviour matches real Perl 5:

* A write to a shared scalar is **automatically** visible to other threads
  — no `lock()` is required for visibility.
* `$x = $x + 1`, `$x++`, `$x += N`, and friends are **atomic** under
  contention — no `lock()` is required for RMW atomicity either.
* `lock($x)` is still available for cases where the user wants to wrap a
  *sequence* of operations in a critical section (e.g. "read $a, branch on
  it, write $b" must be atomic as a *group*). The runtime makes this safe
  via per-thread re-entry tracking.

The contract is verified by `tests/threads_atomic.pl` (added with this
work) and is sound under ThreadSanitizer (see *Validation* below).

This is a strict superset of the pre-existing `threads::shared` model.
Source compatibility is preserved: programs that already use `lock()`
continue to work; programs that *didn't* and happened to rely on x86 TSO
to make non-`lock()`'d writes visible now work portably on aarch64 too.

## Cost model

| Operation                        | Cost                            |
|----------------------------------|---------------------------------|
| `$x = $v` on shared scalar      | One refcounted copy + release fence (`perl_atomic_store`) |
| `$x` (read of shared scalar)     | One acquire fence (`perl_atomic_load`) — on x86 this compiles to a plain `mov` |
| `$x = $x OP N` on shared scalar | One mutex acquire/release around an in-place `ival += di` (or `fval += df`) (`perl_atomic_add`) |
| `$x++` / `$x--` on shared scalar | Same as above (`perl_atomic_inc` / `perl_atomic_dec`) |
| `lock($x)`                       | Allocates the cell's `SharedMutex` on first call ever; subsequent `lock()`s are a single `pthread_mutex_lock` |
| `lock($x); $x = $x + 1`         | The codegen routes the increment through `perl_atomic_add`, which detects re-entry on the per-thread `s_held_mutex_` TLS and skips the redundant lock |
| `cond_wait($x)` / `cond_signal($x)` | Lazy-installed condvar per cell; standard pthread semantics |

The mutex is allocated **only on the first `lock()` or `cond_wait()` call
on a given scalar**. A shared scalar that is only ever read and written
via the atomic helpers never pays for a mutex.

## Memory model

* All shared-scalar reads go through `perl_atomic_load(pv)`, which is an
  acquire fence. On x86 the fence is a compiler barrier only; on aarch64
  LLVM emits `ldar`.
* All shared-scalar writes go through `perl_atomic_store(pv, v)`, which
  is a refcounted payload update + a release fence. On x86 this is a
  plain `mov` + a compiler barrier; on aarch64 LLVM emits `stlr`.
* `perl_atomic_inc/dec/add/swap(pv)` take the cell's `SharedMutex`
  (lazy-installed), do the in-place payload update, release the mutex,
  then issue a release fence. The atomicity of the RMW is provided by
  the mutex; on aarch64 the fence additionally pairs with the reader's
  acquire fence.

This is **sequentially consistent for the RMW path** (mutex + fence)
and **acquire/release for the load/store path** — which is sufficient
because there are no dependent loads across threads (the codegen does
not emit a load-load pair that needs full seq_cst).

## Data layout (Phase 2)

A shared scalar is a `PerlValue*` with `flags & PV_FLAG_SHARED` set.
The cell *is* the `PerlValue`; there is no wrapper. This is a
single-shot ABI break from the previous `PerlSharedVar` wrapper
layout, but nothing links against the runtime except the
compiler-emitted code in the same process, so the migration is
contained to one commit (and `make clean && make`).

The mutex+condvar are kept in a process-wide side table keyed by the
cell address. The install path is guarded by a single global mutex;
subsequent lookups are lock-free (single-pointer comparison on a
chained bucket). See `src/runtime.c` `s_mutex_table` and
`get_or_install_mutex` for the details.

## Codegen wiring (Phase 3)

The codegen tracks a `std::unordered_set<std::string> sharedScalarNames_`
populated whenever a `my $x : shared` (or equivalent) declaration is
emitted. Read and write sites consult the set to dispatch to the
appropriate atomic helper:

| Codegen site (where the LHS is a shared scalar) | Helper called |
|--------------------------------------------------|---------------|
| `case NK::ScalarVar` (rval read)                 | `perl_atomic_load` |
| `case NK::Assign` ($x = v)                       | `perl_atomic_store` |
| `case NK::Assign` with RMW-shaped RHS ($x = $x + N) | `perl_atomic_add` |
| `case NK::UnaryOp` ++/--                         | `perl_atomic_inc` / `perl_atomic_dec` |
| `case NK::CompoundAssign` ($x += N, etc.)        | `perl_atomic_add` (numeric), `applyOp` + `perl_atomic_store` (string/repeat/bitwise) |

The RMW-shape detection is what makes `$counter = $counter + 1` race-free
under contention: the codegen recognises that the LHS appears as an
operand of the RHS BinOp and routes through `perl_atomic_add` instead of
the unsafe load+add+store pair.

## User-facing contract

* **Source compatibility:** all `threads::shared` programs that worked
  before continue to work. `lock()` is still a valid (and sometimes
  necessary) operation — it serialises a *sequence* of operations on
  one or more shared scalars.
* **New guarantee:** RMW operations on a single shared scalar are now
  atomic without an explicit `lock()`. This is a strict superset of
  the old behaviour.
* **What still requires `lock()`:**
  - Multi-statement critical sections (e.g. `lock($a); if ($a) { $b = ... }`)
  - Updates to shared arrays and hashes (those have their own mutex-on-
    the-struct pattern; `lock(@arr)` or `lock(%hash)` is still required)

## Closure capture

Anonymous subs (`my $worker = sub { ... }`) that close over a shared
scalar share the same cell pointer with the parent. Reads of the
captured variable inside the sub go through `perl_atomic_load`
automatically, so the closure path inherits the same memory-model
guarantees as the top-level scope. Named-sub closure capture is a
separate, still-open project (see `THREADS_SHARED_ATOMIC_PLAN.md`
§"Out-of-scope but adjacent").

## Validation

* **Test contract** — `tests/threads_atomic.pl` has 7 assertions covering:
  1. Visibility without lock (`until ($shared) { sleep 1 }` busy-wait)
  2. RMW atomicity (`$counter = $counter + 1` × M×N across threads = exact M×N)
  3. `lock($x); $x = $x + 1` still works (no regression)
  4. `cond_wait` / `cond_signal`
  5. `cond_broadcast` wakes all waiters
  6. `lock()` auto-releases on scope exit
  7. Non-shared vars are still thread-isolated

  The test program exits non-zero on any failure, so `make test` fails
  on regression. The test is a true pass: with Phases 1–3 complete,
  every assertion in it is green.

* **TSan** — building with `-fsanitize=thread` and running
  `tests/threads_atomic.pl`, `tests/threads.pl`, and `tests/destroy.pl`
  reports zero data races on shared scalars. This is the strongest
  signal that the new model is sound.

* **Performance** — `tests/threads.pl` runtime is within ±10% of the
  pre-Phase-2 baseline (the locked path is still a `pthread_mutex_lock`;
  the new path is only faster on the *un*locked plain load/store).

## Migration notes

If you have an existing `threads::shared` program:

* You can remove `lock($x)` wrappers around single-statement RMW
  operations on shared scalars — they are no longer needed for
  correctness. They are still useful as documentation, and they may
  be marginally slower on highly contended cases (because the
  codegen's atomic helper *also* takes a mutex for RMW, and re-entry
  detection is cheap but not free).
* `lock()` around multi-statement critical sections remains required.
* `lock(@arr)` and `lock(%hash)` are unchanged.

## Limitations (still open)

* **Lock-free CAS-on-payload** for the RMW path is the next-step
  optimisation. Currently `perl_atomic_inc/dec/add/swap` take the
  cell's mutex — correct, but slower than a true word-sized CAS would
  be. The plan calls this out as a follow-up after the data-layout
  rewrite.
* **Named-sub closure capture** of shared scalars (e.g.
  `sub worker { lock($x); $x++ }` passed by `\&worker` to
  `threads->create`) is **not** supported. The codegen only captures
  for `my $name = sub { ... }` form. Workaround: pass the shared
  scalar as a thread argument and use `my $x : shared` inside the sub.
* **`our $x : shared`** is not implemented; the parser only accepts
  `my $x : shared`. Workaround: use `my` instead of `our`.
* **Shared array/hash `++` atomicity** (e.g. `push @shared, $x` without
  `lock(@shared)`) is unchanged. The mutex-on-the-array-struct
  pattern handles it correctly when the user locks; making it
  atomic-without-lock would need a CAS on the array's `len`/`elems`
  pair, which is more invasive.
