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

1. **Build a systematic benchmarking framework.** Currently performance data is ad hoc (nb.pl 0.34s, mbs.pl 1.8s mentioned in CLAUDE.md). Create a `bench/` directory with standardized benchmarks that report: (a) perlc elapsed time, (b) Perl interpreter elapsed time, (c) ratio. Use `time()` from the compiled binary itself for consistency. Track results across commits in a CSV or JSON log. This enables regression detection and quantified optimization decisions.

2. **Cache compiled PCRE2 regex patterns.** Every call to `perl_regex_match`, `perl_regex_subst`, `perl_split`, `perl_regex_qr` compiles the pattern from scratch via `pcre2_compile`. For tight loops (e.g. `grep { /pattern/ } @data`), this is a major hotspot. Add a pattern cache keyed by (pattern, flags) with LRU eviction (max 256 entries). The cache should be per-thread (TLS) to avoid locking. This alone could give 2-5x speedup for regex-heavy workloads.

3. **Profile and optimize the hot paths in the C runtime.** The runtime is the bottleneck for generated code — every Perl operation is a C function call. Profile with `perf record` on mbs.pl and nb.pl to identify the top callers. Likely candidates: `perl_clone` (called on every array element read, every regex capture), `perl_to_string` (malloc + strlen on every string coercion), and `perl_assign` (the general-purpose assignment path). Consider: (a) inlineable read-only accessors that skip `perl_clone`, (b) a string interning pool for short-lived strings, (c) reducing the number of function calls by combining common operations.

4. **Extend the F64 fast path to cover more builtins.** Currently `canEmitF64`/`emitExprF64` covers arithmetic, comparisons, and inlineable subs. Extend to cover: `abs`, `int`, `sqrt`, `uc`/`lc` (for numeric strings), `length` (for flat arrays), and `substr` (for numeric indices). This would eliminate PerlValue boxing in more hot loops. The pattern is already established — it is a matter of adding cases to the existing infrastructure.

5. **Add LLVM optimization passes for the generated IR.** Currently the pipeline is: perlc → LLVM IR → clang-18 -O2. Add an intermediate optimization step: load the IR into LLVM, run `opt -O3` with aggressive passes (inlining, loop vectorization, dead-code elimination, constant propagation), then pass to clang for linking. This is a one-line change to the build pipeline (`opt -O3 < input.ll > optimized.ll`) but can significantly improve generated code quality, especially for loops with complex control flow.
