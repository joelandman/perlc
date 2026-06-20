# REMEDIATION.md — Critical Fixes Needed

1. **Fix `NK::EvalBlock` LLVM codegen crash** — `eval { BLOCK }` produces invalid LLVM IR; `endBB` lacks a `ret` instruction causing `Function return type does not match operand type of return inst!`. Fix: add `builder_.CreateRet(perlUndef())` after `perl_eval_pop` in `codegen.cpp`'s `NK::EvalBlock` case. Test: `tests/eval_exception.pl` fails. Severity: **COMPILER CRASH** — blocks any program using block eval.

2. **Fix `require` caching bug** — `test_require_simple.pl` fails on `require_loads_named_sub` and `require_repeat_keeps_sub_available`. Runtime `require` does not register named subs from `.pm` files in the dispatch table. Fix: ensure `perl_runtime_require` adds subs to the code reference table. Test: `tests/test_require_simple.pl` fails. Severity: **BLOCKS MODULE LOADING** — runtime `require` is non-functional.

3. **Fix compound `-=` on shared scalars** — `$shared -= 5` emits `perl_atomic_add($shared, +5)` instead of subtracting; delta is not negated before the atomic add. Fix: negate delta via `perl_to_float` → `fneg` → `boxF64` before `perl_atomic_add`. Test: `tests/regression_bugs.pl` `regression_multi_subtract` fails. Severity: **DATA CORRUPTION** — shared scalar arithmetic produces wrong results.

4. **Add `*=` `/=` `%=` atomic RMW for shared scalars** — These compound ops fall through to non-atomic `perl_assign` instead of using atomic RMW primitives. For int/float payloads, use lock-free 16-byte CAS-on-payload (same pattern as `$x++`). For non-numeric tags, fall back to SharedMutex. Test: `tests/regression_bugs.pl` `regression_multiply` fails. Severity: **DATA RACE** — concurrent shared scalar updates can lose updates.

5. **Fix closure + range with captured variable** — `for (1..$per)` where `$per` is captured from outer scope emits `undef` bound in anonymous subs. The range expansion in function call args path does not correctly handle captured variables. Fix: ensure captured variables are properly boxed before range expansion. Test: `tests/regression_bugs.pl` `regression_anon_range` fails. Severity: **CORRECTNESS BUG** — closures with ranges produce wrong values.
