# PLANS.md — Correctness, Completeness, Performance Plans

## Correctness Plans

1. **Fix `NK::EvalBlock` LLVM codegen crash.** `eval { BLOCK }` produces invalid LLVM IR because `endBB` lacks a `ret` instruction — LLVM verify error: `Function return type does not match operand type of return inst! ret ptr %17 i32`. Fix: add `builder_.CreateRet(perlUndef())` after `perl_eval_pop` in the `endBB` block. This is a compiler crash blocking any program using block eval from compiling. Test: `tests/eval_exception.pl` currently fails.

2. **Fix `require` caching bug.** `test_require_simple.pl` fails on `require_loads_named_sub` and `require_repeat_keeps_sub_available`. The runtime `require` does not properly register named subs from `.pm` files in the dispatch table. Fix: ensure `perl_runtime_require` adds subs to the code reference table so they're callable via dispatch. Test: `tests/test_require_simple.pl` currently fails.

3. **Fix compound `-=` on shared scalars.** `$shared -= 5` emits `perl_atomic_add($shared, +5)` instead of subtracting — the delta is not negated before the atomic add. Fix: negate delta via `perl_to_float` → `fneg` → `boxF64` before `perl_atomic_add`. Test: `tests/regression_bugs.pl` `regression_multi_subtract` currently fails.

4. **Add `*=` `/=` `%=` atomic RMW for shared scalars.** These compound ops fall through to non-atomic `perl_assign` instead of using atomic RMW primitives. For int/float payloads, use lock-free 16-byte CAS-on-payload (same pattern as `$x++`). For non-numeric tags, fall back to SharedMutex. Test: `tests/regression_bugs.pl` `regression_multiply` currently fails.

5. **Add valgrind/memcheck verification to test harness.** No automated memory-safety gate exists. The PV slab allocator and PerlArray freelist pool hide leaks from standard tools. Add `make test-valgrind` that runs all 36 tests under `valgrind --tool=memcheck --leak-check=full --errors-for-leak-kinds=all`. Fix every reported leak (currently 4,096 bytes "still reachable" from PV slab at exit).

## Completeness Plans

1. **Implement `tie`/`untie` with minimal TIESCALAR/TIEARRAY/TIEHASH interface.** The most commonly used CPAN modules (e.g. `DB_File`, `Tie::Hash::Cached`) depend on `tie`. A minimal implementation supporting `TIESCALAR`, `TIEARRAY`, `TIEHASH` with `FETCH`, `STORE`, `FETCHSIZE`, `STORESIZE` would cover the majority of use cases. Start with `TIEHASH` since it is the most complex and most commonly needed.

2. **Implement `do FILE` runtime execution.** `require` is implemented (compile-time inlining) but `do "file.pl"` is not. `do` differs from `require` in that it executes the file each time (no caching), returns the last expression's value, and sets `$@` on parse/runtime failure. Reuse the file-loading path and add a non-caching execution mode.

3. **Add `pack`/`unpack` for binary data serialization.** These are among the most-used builtins for network protocols, file formats, and FFI. Start with the most common format codes: `C`, `c`, `S`, `s`, `L`, `l`, `N`, `V`, `F`, `D`, `A`, `a`, `H`, `h`, `B`, `b`, `x`, `X`, `@`. The runtime would interpret format strings and produce/consume byte buffers, integrating with `ptr` values in the XS interface.

4. **Add UTF-8/unicode support.** Currently strings are byte-only. Adding `use utf8`, `use Encode`, and the `unicode` pragma would require: (a) UTF-8 decoding in the lexer for source files, (b) UTF-8 aware `length`, `substr`, `index`, `rindex`, and regex operations in the runtime, (c) `chr`/`ord` for code points above 255. This is a large effort but essential for CPAN compatibility.

5. **Fix closure + range with captured variable.** `for (1..$per)` where `$per` is captured from outer scope emits `undef` bound in anonymous subs. The range expansion in function call args path does not correctly handle captured variables. Fix: ensure captured variables are properly boxed before range expansion. Test: `tests/regression_bugs.pl` `regression_anon_range` currently fails.

## Performance Plans

1. **Reduce PV boxing overhead in regex path.** Regex-heavy workloads are ~3x slower than Perl (826ms vs 260ms on regex_heavy benchmark). Perl's regex engine is highly optimized; perlc's PCRE2 calls + PV boxing overhead outweigh JIT benefits. Consider inline PCRE2 API calls that avoid PerlValue boxing for the common case of matching numeric strings or pre-boxed values.

2. **Implement in-process LLVM optimization passes.** Currently relies on clang-18 `-O2`. The `opt-18` integration encountered reliability issues with `system()` calls. Implement in-process optimization using LLVM's PassBuilder to run `opt -O3` passes (inlining, loop vectorization, dead-code elimination, constant propagation) before linking. This avoids temp file overhead and shell invocation issues.

3. **Extend F64 fast path to cover `sprintf` format parsing and `join` with numeric args.** Currently `sprintf` and `join` always box arguments through PerlValue*. Adding F64 fast path cases for numeric-only `sprintf` formats (`%f`, `%d`, `%e`) and `join` with numeric arrays would eliminate boxing in common string-building hot loops.

4. **Add string interning pool for short-lived strings.** `perl_to_string_dup()` calls `strdup()` on every string coercion. A string interning pool for short strings (< 64 bytes) would eliminate redundant allocations for common strings like `"0"`, `"1"`, `""`, `" "`, etc. This would reduce allocation pressure in tight loops.

5. **Profile and optimize `perl_clone` hot path.** `perl_clone()` is called on every array element read, every regex capture, and every hash set operation. It does `strdup()` for strings and `memcpy()` for FLAT_ARRAY. Consider an inlineable read-only accessor that skips `perl_clone` when the source is known to be stable (e.g., array elements that won't be modified).

## Benchmark Results (3 runs averaged)

| Benchmark | perlc | Perl | Speedup |
|-----------|-------|------|---------|
| fibn (n=30) | 270ms | 590ms | 2.19x |
| mbs (512×512, 80 iters) | 1,380ms | 18,990ms | 13.76x |
| nb (n=1M) | 60ms | 5,850ms | 97.50x |
| regex_heavy (100K items × 50) | 810ms | 260ms | 0.32x |

Note: regex_heavy shows perlc slower than Perl because Perl's regex engine is highly optimized; the PCRE2 cache eliminates redundant `pcre2_compile` calls but the perlc overhead (function calls, PV boxing) still outweighs the benefit for regex-heavy workloads.
