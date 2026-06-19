# REMEDIATION.md — Most Important Remediation Items

All 5 items have been implemented and tested. See git log for details.

## 1. [DONE] Program-level cleanup — `perl_cleanup()`

Implemented in `runtime.c` and registered via `atexit()` in `main.cpp`. Frees:
- Shared-mutex side-table entries (destroys all mutexes/condvars, frees entries)
- `perl_plus_hash` (named captures from PCRE2)
- XS module list via `perl_xs_cleanup()`

Result: valgrind reports zero leaks from runtime internal state.

## 2. [DONE] Compound `-=` on shared scalars

Fixed in `codegen.cpp` in both the CompoundAssign path and the longhand RMW path (`$shared = $shared OP N`). For `-`: negates the delta before calling `perl_atomic_add`. For `*`, `/`, `%`: falls through to non-atomic `perl_assign` (no atomic RMW primitive exists for these ops).

## 3. [DONE] `perl_to_string` ownership contract

Refactored in `runtime.c`/`runtime.h`:
- `perl_to_string()` now returns a **stable pointer** for `PERL_STRING` and `PERL_UNDEF` (no malloc/free needed)
- Added `perl_to_string_dup()` that always returns heap-allocated strings for callers that need to free
- All ~100 callers updated to use `perl_to_string_dup()`, eliminating leaks from forgotten frees on error paths

## 4. [DONE] `--leak-check` debug mode for PV allocator

Added `PERL_ALLOC_DEBUG` compile flag. Tracks every `pv_alloc`/`pv_pool_push` pair using a sentinel value (`0xDEADBEEF`) in the `ival` field. At program exit, iterates all allocated slabs and reports any PVs with the sentinel still set. Slab allocation is tracked via a per-process list (`pv_slabs_[]`). Compile with `-DPERL_ALLOC_DEBUG` to enable.

## 5. [DONE] Closure + range-with-captured-variable codegen bug

**Root cause**: Unboxed int/float vars (`my $per = 5`) are stored in `intScopes_`/`floatScopes_`, but closure capture (Phase 1) only checked `scopes_` via `lookupVar()`. The closure never captured the variable, so `emitExpr()` returned `perlUndef()`.

**Fix**: Closure capture Phase 1 now also checks `intScopes_` and `floatScopes_`. For unboxed int/float vars, the value is loaded and boxed into a `PerlValue*` for capture. The closure body then finds the captured variable via `lookupVar()`.
