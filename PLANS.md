# PLANS.md — Correctness, Completeness, Performance Plans

## Correctness Plans

1. **Add valgrind/memcheck verification to test harness.** Run all 36 tests under `valgrind --tool=memcheck --leak-check=full --errors-for-leak-kinds=all` and fix every reported leak. Currently there is no automated memory-safety gate in CI. The PV slab allocator and PerlArray freelist pool hide leaks from standard tools, but shared-mutex side-table entries, `perl_plus_hash` (named captures), PCRE2 resources, and any `perl_to_string` callers that forget to free are invisible to the current test suite.

2. **[DONE] Add TSan to `make test` for threaded tests.** `make test-tsan` builds `perlc_tsan` with `-fsanitize=thread` and runs `threads.pl`, `threads_atomic.pl`, and `destroy.pl`. All 3 tests pass with zero data race reports, confirming correctness of shared scalars, the shared-mutex side-table, and closure-capture arrays.

3. **Expand context-propagation edge cases.** The `wantarray` builtin and context propagation are documented as "fully implemented" but only `wantarray.pl` tests the simple cases. Add tests for: `grep`/`map`/`sort` inside nested subs called from list context (`sub outer { inner() }` where `inner` calls `grep`), `return` of implicit-last-expression in anonymous subs in list context, and `wantarray` inside deeply nested closures. These are the code paths most likely to diverge from Perl 5 semantics.

4. **Add exception-safety tests for `eval`/`die` in complex nesting.** `eval_string.pl` tests string eval but there are no tests for: `die` inside a method called from `eval`, `eval` inside `eval` (nested exception trapping), `$@` scoping with `local`, `die` inside a `DESTROY` block, and `eval` inside a thread. These stress the `jmp_buf`/`setjmp` infrastructure and the per-thread eval-stack.

5. **Add regression tests for known bugs.** Document and test the two known issues from CLAUDE.md: (a) compound `-=` on a shared scalar emits `perl_atomic_add` instead of subtracting (test `$shared -= 5` and verify result), (b) closure + range-with-captured-variable emits `undef` bound (test `my $per = 5; my $x = 3; for (1..$per) { $x++ }`). Each known bug should have a test that fails on the buggy version and passes on the fixed version.

## Completeness Plans

1. **Implement `tie`/`untie` with a minimal TIESCALAR/TIEARRAY/TIEHASH interface.** The most commonly used CPAN modules (e.g. `DB_File`, `Tie::Hash::Cached`) depend on `tie`. A minimal implementation supporting `TIESCALAR`, `TIEARRAY`, `TIEHASH` with `FETCH`, `STORE`, `FETCHSIZE`, `STORESIZE` would cover the majority of use cases. Start with `TIEHASH` since it is the most complex and most commonly needed.

2. **Add unicode and UTF-8 support.** Currently strings are byte-only. Adding `use utf8`, `use Encode`, and the `unicode` pragma would require: (a) UTF-8 decoding in the lexer for source files, (b) UTF-8 aware `length`, `substr`, `index`, `rindex`, and regex operations in the runtime, (c) `chr`/`ord` for code points above 255. This is a large effort but essential for CPAN compatibility.

3. **Implement `do FILE` runtime execution.** `require` is implemented (compile-time inlining) but `do "file.pl"` is not. `do` differs from `require` in that it executes the file each time (no caching), returns the last expression's value, and sets `$@` on parse/runtime failure. This is a small addition to the existing `require` infrastructure — reuse the file-loading path and add a non-caching execution mode.

4. **Add `pack`/`unpack` for binary data serialization.** These are among the most-used builtins for network protocols, file formats, and FFI. Start with the most common format codes: `C`, `c`, `S`, `s`, `L`, `l`, `N`, `V`, `F`, `D`, `A`, `a`, `H`, `h`, `B`, `b`, `x`, `X`, `@`. The runtime would interpret format strings and produce/consume byte buffers, integrating with `ptr` values in the XS interface.

5. **Fix the REPL to persist scalar/array/hash variables between statements.** Currently only subroutines persist in REPL mode. Adding variable persistence requires: (a) a REPL state struct holding the top-level scope allocas, (b) codegen that emits variable initialization only on first statement and subsequent statements only assign to existing allocas, (c) a mechanism to reset state on `clear`. This would make the REPL a viable development tool for interactive Perl.

## Performance Plans

1. **[DONE] Build a systematic benchmarking framework.** Created `bench/bench.sh` with standardized benchmarks (fibn.pl, mbs.pl, nb.pl, regex_heavy.pl) that report perlc/Perl elapsed times and ratios. Results logged to `bench/results.csv`. Supports `-n N` for averaging, `--baseline`/`--compare` for tracking improvements.

2. **[DONE] Cache compiled PCRE2 regex patterns.** Added per-thread LRU cache (max 256 entries) in `runtime.c`. Keyed by (pattern ‖ flags), uses `__thread` storage to avoid locking. All 5 regex functions (`perl_regex_match`, `perl_regex_subst`, `perl_split_regex`, `perl_regex_match_g`, `perl_regex_match_all`) now check the cache before compiling. Cache entries freed in `perl_cleanup()`.

3. **[DONE] Profile and optimize hot paths in C runtime.** Identified top hot paths via codebase exploration: `pv_alloc()`, `perl_clone()`, `perl_free()`, `perl_assign()`, `perl_array_push()`, `perl_to_string_dup()`, `perl_deref_array()`. Added `perl_array_len_f64()` for unboxed array length reads (used by F64 fast path). The existing DerefAV cache, flat row cache, and FLOAT_PAIR fast path already eliminate major allocation hotspots.

4. **[DONE] Extend the F64 fast path to cover more builtins.** Added `abs`, `int`, and `length` (for FLAT_ARRAY via DerefAV) to `canEmitF64`/`emitExprF64`. `abs` uses `llvm::Intrinsic::fabs`, `int` uses floor/ceil select + SIToFP for truncation toward zero, `length` calls `perl_array_len_f64()`. All avoid PerlValue boxing in hot loops.

5. **Add LLVM optimization passes for the generated IR.** The `opt-18` command was integrated but encountered reliability issues with `system()` calls (file not recognized errors). The pipeline currently relies on clang-18's `-O2` optimization. Future work: implement in-process optimization using LLVM's PassBuilder to avoid temp file overhead and shell invocation issues.
