# REMEDIATION.md — Critical Fixes Needed

**2026-07-09 update**: a full re-verification pass found 26 previously-undocumented defects (10 of them CRITICAL, including 2 crashes) not covered by this file's "all fixed" claim below. See `TESTS.md`'s Defect Registry for the current, accurate list — this file only covers what was fixed *before* that pass, items 1-14, plus item 15 (this session's fixes, added below).

Original items (1-14) — all fixed at time of writing:

1. ~~**Fix `NK::EvalBlock` LLVM codegen crash**~~ — FIXED (commit a701cd1)
   - Added check for `return` inside eval blocks in main function
   - Calls `perl_die` instead of emitting `ret ptr` (which violates LLVM's type requirements)

2. ~~**Fix `require` caching bug**~~ — FIXED (already working, verified)
   - Runtime `require` correctly registers named subs in the dispatch table

3. ~~**Fix compound `-=` on shared scalars**~~ — FIXED (commit db7ba77)
   - Added `get_or_install_mutex` call in `perl_atomic_add` to ensure mutex is installed

4. ~~**Add `*=` `/=` `%=` atomic RMW for shared scalars**~~ — FIXED (commit db7ba77)
   - Added `perl_atomic_rmw` runtime function with mutex protection
   - Codegen calls `perl_atomic_rmw` for `*=`, `/=`, `%=` on shared scalars

5. ~~**Fix closure + range with captured variable**~~ — FIXED (commit 776b963)
    - Fixed list return bug: added `perl_array_push_list_or_scalar` to unwrap `PERL_LIST_RESULT` tags
    - Fixed `perl_call_code_ref` to use caller's wantarray context

6. ~~**Stage 32: Loop-invariant PV deferral**~~ — FIXED (implemented in codegen.cpp)
    - Added `loopInvariantPVs_` stack; `trackPv()` routes `perl_alloc_undef` and `perl_deref_array` results to deferred tracking when inside a loop
    - `popScope()` skips freeing these PVs; `freeLoopInvariantPVs()` called after loop exit
    - perl_free calls reduced from 114 to 93 (18% reduction)

7. ~~**Stage 32: Deref hoisting**~~ — FIXED (implemented in codegen.cpp)
    - Added `loopDerefCache_` stack; `collectDerefTargets()` finds ScalarVars in ArrowDeref
    - `isVarModified()` checks if variable is written inside loop
    - `emitHoistedDerefs()` emits `perl_deref_array` before loop and caches result
    - `emitDerefArray()` checks cache before calling `perl_deref_array`

8. ~~**Stage 33: Known tag type tracking**~~ — FIXED (implemented in codegen.cpp)
    - Added `knownTagTypes_` stack; scalar assignments from AnonArray `[float, float]` set tag=13 (FLOAT_PAIR), `[float, ...]` set tag=10 (FLAT_ARRAY)
    - ArrowDeref RHS assignments propagate array element type to LHS
    - `emitExprF64` ArrowDeref skips tag dispatch when type is known

9. ~~**Stage 33: Array element type tracking**~~ — FIXED (implemented in codegen.cpp)
    - Added `arrayElemTypes_` stack; `$arr[i] = [float, ...]` sets element type for array
    - Type propagation through ArrowDeref assignments (`$var = $arr->[idx]`)

10. ~~**Function argument element type tracking removed**~~ — FIXED (correctness issue resolved)
    - Removed `funcArgElemTypes_` which caused incorrect code for non-array function arguments (e.g., `cabs` takes single FLOAT_PAIR, not array of FLOAT_PAIRs)
    - Type tracking now conservative — only tracks local variables and array elements, not cross-subroutine

11. ~~**Phase 4: Unboxed sub returns**~~ — FIXED (implemented in tryEmitInline)
    - `tryEmitInline` now checks if body is F64-capable via `canEmitF64(*is.bodyExpr)`
    - If so, emits body using `emitExprF64` and returns raw F64 value
    - `emitCall` detects F64 return type and boxes via `perl_alloc_float`
    - Eliminates `perl_clone` + boxing for inlineable subs with float bodies

12. ~~**Fix D2: `chop @arr` behaved identically to `chomp @arr`**~~ — FIXED
    - Added `perl_chop_array()` to runtime.c/runtime.h: chops every element in place, returns the last removed character (matching Perl's `chop LIST` semantics)
    - `codegen.cpp` array-chop branch now calls `perl_chop_array` instead of `perl_chomp_array`
    - Scalar `chop` return value now boxed via `perl_alloc_int` instead of leaking a bare int as `perlInt(0)`
    - Test coverage added to `tests/builtins.pl` (scalar and array forms); verified byte-for-byte against real perl via `tests/harness.sh`

13. ~~**Fix D3: `local @arr`/`local %hash` silently no-op for function-scope variables**~~ — FIXED
    - Root cause: `hasLocalStmt()` (codegen.cpp) — used both to decide whether a sub needs the local-depth save/restore alloca (`subNeedsLocal`) and whether a bare block needs it — only matched `NK::LocalStmt` (scalar `local $x`). Subs/blocks containing only `local @arr`/`local %hash` were classified as "no local() present," so `perl_local_save_array`/`perl_local_save_hash` pushed onto the runtime local-stack but the matching `perl_local_restore_to` at sub return was never emitted. Fixed by also matching `NK::LocalArray`/`NK::LocalHash`.
    - Related defect found and fixed in the same pass: `perl_die()` (runtime.c) did a raw `longjmp` back to the enclosing `eval`'s setjmp point without ever calling `perl_local_restore_to`, so **any** `local` (scalar included) unwound via `die`/`eval` was never restored. Fixed by recording the local-stack depth at `perl_eval_push()` time (parallel `s_eval_local_depth[]` array) and restoring to it in `perl_die()` before the `longjmp`.
    - Test coverage added to `tests/completeness.pl`: sub-scoped `local @arr`/`local %hash` (the original D3 symptom, not covered by the pre-existing bare-block test), plus a die/eval-unwind case for both. Verified byte-for-byte against real perl via `tests/harness.sh`.

14. ~~**Fix D5: Closure + range-with-captured-variable emits `undef` bound**~~ — FIXED
    - Root cause: `collectAllScalarNames()` (codegen.cpp) explicitly skipped recursing into nested `NK::AnonSub` bodies ("nested closure handles its own captures"). This breaks *transitive* capture: when a closure is nested two or more levels deep (`sub { return sub { ...$x... } }`), the middle closure never discovers that `$x` is needed by the inner closure, so it never captures it — the inner closure's own capture step then finds nothing bound to `$x` in the middle closure's scope. Symptom reproduced exactly as described: `for (1..$per)` inside a doubly-nested closure silently loops zero times (undef upper bound).
    - Fix: removed the `AnonSub` special case so the generic traversal recurses into its body, propagating transitive capture needs through arbitrarily many nesting levels. `SubDef` (named subs) is still skipped — those compile as standalone top-level functions unrelated to the enclosing lexical scope.
    - Test coverage added to `tests/closures.pl` (2-level and via a range-loop case matching the original symptom); verified byte-for-byte against real perl via `tests/harness.sh`, including 3-level nesting, multi-variable transitive capture, and per-iteration closure isolation in ad-hoc checks. No regressions across the full suite (thread-closure tests unaffected).

15. ~~**Fix unboxed-int `pre--`/`post--` sign bug, `AnonSub` array/hash closure capture, and `for`-loop labeled next/last**~~ — FIXED (commits `b403168`, `fa5961b`)
    - Found while adding C-style `for`-loop comma-operator support (`for ($i=0,$j=10; ...; $i++,$j--)`, parser.cpp), all three pre-existing and independent of that feature, just newly exposed by it.
    - **Sign bug**: the unboxed-int fast path for `pre--`/`post--` (codegen.cpp ~4162) computed `next = Sub(cur, delta)` with `delta` already `-1` for decrement, i.e. `cur - (-1) = cur + 1` — decrement became increment. Normally masked by a correct sibling fast-path in the single-item `for`-step case; the new comma-step `FlatBlock` routed through this buggy generic path instead, causing wrong results and even infinite loops for `$j--` in some contexts. Fixed to always `Add` the signed delta.
    - **Closure array/hash capture**: `NK::AnonSub` only ever captured `$scalars` (via `collectAllScalarNames`), never `@arrays`/`%hashes` from an enclosing block — `push @log,...` inside a closure over a block-scoped array silently wrote to a detached array while the outer scope saw nothing. Fixed by capturing every block-scoped array/hash visible at closure-creation time (boxed via the existing `perl_ref_array`/`perl_ref_hash`, unboxed via `perl_deref_array_ro`/`perl_deref_hash` inside the closure) — deliberately over-inclusive rather than an AST name-walk, since many builtins (`push`, `keys`, `splice`, ...) reference an array/hash by bare name instead of a child node, and an incomplete allowlist fails silently.
    - **Labeled `for` loops**: `NK::For` never registered its label into `loopLabels_` (unlike `NK::Foreach`, which does at two call sites), so `next LABEL`/`last LABEL` targeting a `for` loop found no match and emitted no branch — corrupting the block's control flow (observed as the label being silently ignored, sometimes worse). Fixed by pushing/popping the label the same way `Foreach` does.
    - Test coverage: `tests/comma_operator_smoke.pl` (4 assertions) and `tests/comma_operator.pl` (19 assertions covering multi-item init/step, mixed `my`/`our`/predeclared vars, array/hash-element step targets, nested independent scopes, `next`/`last`/labeled-`next` interaction) — verified byte-for-byte against real Perl.
    - Separately: `make test-tsan` was failing to link (`Makefile`'s `TSAN_OBJS` missing `ast_tsan.o`/`llvm_early_init_tsan.o`, present in the regular `OBJS`) — fixed; TSan build now clean with zero race reports on `threads.pl`/`threads_atomic.pl`/`destroy.pl`.

16. ~~**Fix D38: `my ($x,$y) = (10)` segfault (list-assignment arity mismatch)**~~ — FIXED
    - Root cause: a single-element parenthesized RHS with no comma parses down to a bare scalar node, not an `ArrayLit` (the parser treats parens with no comma as plain grouping, e.g. `(10)` → just `IntLit(10)`). The list-assignment codegen's `emitArrayPtr(RHS)` then returned null for that bare scalar, and the fallback path did `rhsArr = emitExpr(*n.right)` — assigning a `PerlValue*` to a variable later passed into `perl_array_get_ref()`, which expects a `PerlArray*`. Raw type confusion: reading `a->len`/`a->elems[idx]` off a `PerlValue*`'s memory layout, causing a segfault (or worse, silently-wrong reads).
    - Fix: wrap the lone scalar in a real one-element `PerlArray` before use (codegen.cpp ~4264), matching the pattern already correctly used by `@arr = RHS`, `@arr[i,j] = list`, and `@h{LIST} = list` a few hundred lines later in the same function — this exact call site was the only one with the bug; the sibling paths were already correct.
    - Two unrelated bugs found (not fixed, logged as new defects) while writing regression tests: **D45** — a named `sub` declared inside a bare block that follows another bare block resolves to the wrong value at its call site; **D46** — a ternary with no space before `:` (`$x?$y:"str"`) fails to parse when the true-branch is a bare scalar var.
    - Test coverage: `tests/list_assign_arity_smoke.pl` (3 assertions) and `tests/list_assign_arity.pl` (16 sections covering fewer/more/exact/empty RHS, scalar-var/function-call/ternary/flattened-array RHS, trailing-comma RHS, `@rest` collection with a single-scalar RHS, and arity mismatch inside a sub via `@_`) — verified byte-for-byte against real Perl.

17. ~~**Fix D38b: `s/$/text/` segfault (zero-width regex-substitution match)**~~ — FIXED
    - Root cause: `perl_regex_subst`'s zero-length-match branch (runtime.c ~4441) copied `s[pos]` (the search-start position) instead of `s[mstart]` (the actual match position) to avoid an infinite loop. For an anchor like `$` that can match ahead of `pos` (e.g. at end-of-string), this was wrong on two counts: it duplicated a character already flushed by the "text before match" copy, and it left `pos = mstart+1`, which becomes `slen+1` when the match sits exactly at end-of-string. The subsequent `size_t rem = slen - pos` (both unsigned) then underflowed to ~SIZE_MAX, and the resulting multi-exabyte `memcpy`/`realloc` corrupted the heap — the segfault only surfaced later, inside `pcre2_match_data_free()`, once the corruption reached PCRE2's own bookkeeping. This is what `TESTS.md`'s original vague "npos underflow" description (D7) was pointing at.
    - Fix: use `mstart` (not `pos`) for the zero-width-match character copy, and clamp the final `rem` computation (`(pos < slen) ? slen - pos : 0`) so it can't underflow.
    - Test coverage: `tests/regex_subst_zero_width_smoke.pl` (3 assertions) and `tests/regex_subst_zero_width.pl` (17 sections: end/start anchors × global/non-global, empty/single-char subjects, lookahead, empty-pattern-every-position, `/mg`, embedded newlines, return-value-is-count, array-element targets) — verified byte-for-byte against real Perl.
