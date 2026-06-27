# perlc — Test Defect Tracking

This file tracks defects found during analysis, the test scripts that demonstrate them, and the commits/changes that fixed them.

## Test Infrastructure

### Test Runner: `run_tests.sh`

A comprehensive test runner that provides correctness validation for all test files by comparing compiled binary output against the Perl interpreter.

#### Key Features

- **Automatic correctness validation**: For every `.pl` test file, the runner:
  1. Compiles the file with `perlc`
  2. Runs the Perl interpreter on the original file
  3. Runs the compiled binary
  4. Compares outputs byte-for-byte
  5. Reports PASS/FAIL with diff details

- **Test classification**: Automatically classifies tests as:
  - **Assertion tests**: Have built-in `die()` checks (threads_atomic.pl, xs_ffi.pl, etc.)
  - **Smoke tests**: Print output but no assertions (hello.pl, arith.pl, etc.)
  - **Benchmark tests**: Performance-oriented (fibn.pl, mbs.pl, nbody.pl, fk.pl, bt.pl)
  - **Placeholder tests**: No real testing (xs_dbi_test.pl — skipped)

- **Benchmark result caching**: Long-running benchmark tests are cached per-hostname in `tests.csv`:
  - Before running a benchmark, checks if a cached result exists for the current hostname
  - If found, uses the cached result instead of re-running (saves time on CI/build servers)
  - Cache file format: `hostname,benchmark_test,time_seconds,success,output,accuracy`
  - Cache commands: `--show-cache` (view), `--clear-cache` (clear), `--force-benchmark` (re-run), `--no-benchmark-cache` (disable)
  - Default timeout for benchmarks: 300 seconds (configurable via `--benchmark-timeout`)

- **Flexible modes**:
  - `test-all`: Run all tests with perl output comparison
  - `test-smoke`: Run only smoke/benchmark tests
  - `test-assertion`: Run only assertion tests
  - `--smoke-only` / `--assertion-only`: Filter modes
  - `--skip-compile`: Only run perl interpreter (verify perl works)
  - `--skip-interp`: Only run compiled binary (verify compilation works)
  - `--sort-output`: Sort output lines before comparing (for non-deterministic tests)
  - `--ignore-whitespace`: Ignore trailing whitespace differences
  - `--ignore-exit`: Ignore exit code differences
  - `--verbose`: Show diff details for failures
  - `--dry-run`: Print what would be tested
  - `-j N`: Run N tests in parallel

- **Output status codes**:
  - `PASS`: Compiled output matches perl
  - `FAIL`: General failure
  - `SKIP`: Test skipped (placeholder, filter, etc.)
  - `COMP`: Compilation failed, perl passed
  - `RUNT`: Perl failed, compiled binary passed
  - `MISM`: Both ran but outputs differ
  - `TIME`: Timed out

- **Temporary files**: `/tmp/perlc_test_<name>_*/`
  - `perl_output.txt`, `compiled_output.txt`
  - `perl_stderr.txt`, `compiled_stderr.txt`
  - `diff.txt`, `status.txt`, `compile.log`

#### Usage

```bash
# Run all tests with perl output comparison
./run_tests.sh tests/

# Run with verbose output
./run_tests.sh -v tests/arith.pl

# Run in parallel
./run_tests.sh -j 4 tests/

# Only smoke tests
./run_tests.sh --smoke-only tests/

# Only assertion tests
./run_tests.sh --assertion-only tests/

# Dry run
./run_tests.sh -n tests/
```

#### Makefile Targets

```bash
make test        # Run assertion-based contract tests (fast)
make test-all    # Run all tests with perl output comparison
make test-smoke  # Run only smoke/benchmark tests
make test-assertion  # Run only assertion tests
```

### Test Quality Assessment (Pre-Runner)

Before the test runner was added:

| Category | Count | Description |
|----------|-------|-------------|
| Smoke tests (zero assertions) | 29 | Print output but never verify correctness |
| Light validation (conditional prints) | 9 | Use `print "ok" if $cond` but don't affect exit code |
| Proper assertions (die-on-fail) | 5 | Exit non-zero on failure |
| Placeholders (no-op) | 1 | Always passes |

**The test runner eliminates the smoke test problem by providing external correctness validation for all 29 smoke tests automatically.**

## Defect Registry

### Build Environment

| Defect ID | Problem | Test Script | Status | Commit | Fix Summary |
|-----------|---------|-------------|--------|--------|-------------|
| B1 | LLVM 18 + GCC 15/16 incompatibility — `__normal_iterator` incomplete type errors | N/A (build fails) | **OPEN** | — | Downgrade GCC to 13 or migrate to LLVM 21/22 |

### Critical Defects

| Defect ID | Problem | Test Script | Status | Commit | Fix Summary |
|-----------|---------|-------------|--------|--------|-------------|
| D1 | All numeric compound assignments on shared scalars go through `perl_atomic_add` — `-=` adds instead of subtracts, `*=` multiplies as addition | `threads_atomic.pl` (add `-=` test case), `run_tests.sh` will catch via output comparison | **OPEN** | — | Route `-=` to negate delta; add `perl_atomic_mul`/`div`/`rem` or emit longhand |
| D2 | `chop @arr` calls `perl_chomp_array` instead of removing last characters | `builtins.pl` (add `chop @arr` test), `run_tests.sh` will catch via output comparison | **OPEN** | — | Add `perl_chop_array` to runtime.c or fix codegen |
| D3 | `local @arr` / `local %hash` silently no-ops for function-scope variables | `completeness.pl` (add function-scope local test), `run_tests.sh` will catch via output comparison | **OPEN** | — | Implement save/restore for function-scope arrays/hashes |

### High Severity Defects

| Defect ID | Problem | Test Script | Status | Commit | Fix Summary |
|-----------|---------|-------------|--------|--------|-------------|
| D5 | Closure + range-with-captured-variable emits `undef` bound, loop never executes | `closures.pl` (add captured var in range test), `run_tests.sh` will catch | **OPEN** | — | Improve `emitBound` fallback for captured variables |
| D6 | `for (my $i = 0; ...)` C-style init is dead code | `range.pl` or `modifiers.pl` (add C-style for with my), `run_tests.sh` will catch | **OPEN** | — | Parse `my` keyword properly in C-style for loops |
| D7 | `s///` without second `\x01` delimiter causes `npos` underflow | `regex.pl` (add malformed substitution test), `run_tests.sh` will catch | **OPEN** | — | Add bounds checking in substitution parser |
| D8 | `parseOrRhs` handles only small subset of statement keywords after `or`/`and` | `features.pl` (add `or eval`, `or require` tests), `run_tests.sh` will catch | **OPEN** | — | Expand keyword list or use general statement parsing |
| D9 | `lastSqrtInput_` never cleared, can match wrong variable's square | `builtins2.pl` (add sqrt+mul test), `run_tests.sh` will catch | **OPEN** | — | Clear after use or scope per expression |

### Medium Severity Defects

| Defect ID | Problem | Test Script | Status | Commit | Fix Summary |
|-----------|---------|-------------|--------|--------|-------------|
| D10 | Static counters (`sortCmpCounter`, `stateSeq`, `endSeq`, `anonCount`) don't reset between compilations | Verify in REPL mode | **OPEN** | — | Reset counters at start of each `compile()` |
| D11 | `*=`, `/=`, `%=` on shared scalars emit `perl_atomic_add` (same root cause as D1) | `threads_atomic.pl` (add `*=`, `/=` test cases), `run_tests.sh` will catch | **OPEN** | — | Same fix as D1 |
| D12 | `wantarray` context not propagated through `print`/`say`/`printf` | `wantarray.pl` (add print/wantarray test), `run_tests.sh` will catch | **OPEN** | — | Extend context propagation to all builtins |
| D13 | DESTROY handling not visible in codegen/parser — unclear if fully functional | `destroy.pl` (verify thoroughness), `run_tests.sh` will catch | **VERIFY** | — | Confirm runtime implementation |

### Low Severity Defects

| Defect ID | Problem | Test Script | Status | Commit | Fix Summary |
|-----------|---------|-------------|--------|--------|-------------|
| D14 | Duplicated `cmpOps` array in two places in parser.cpp | N/A (code quality) | **OPEN** | — | Consolidate into single constant |
| D15 | Hardcoded `specialVars` list duplicated in two places in codegen.cpp | N/A (code quality) | **OPEN** | — | Consolidate into single definition |
| D16 | `xs_dbi_test.pl` is a no-op placeholder — always passes | `xs_dbi_test.pl` | **OPEN** | — | Replace with real tests or remove |

---

## Test Coverage Gaps

These features are claimed to be implemented but have NO corresponding test:

| Feature | Claimed In | Test Needed |
|---------|-----------|-------------|
| `@{$href}{LIST}` hash-ref slices | CLAUDE.md | New test |
| `@{$aref}[LIST]` array-ref slices | CLAUDE.md | New test |
| `scalar(@{$ref})` | CLAUDE.md | New test |
| `$h{k}++` on missing keys | CLAUDE.md | New test |
| lvalue slices `@arr[i,j] = list` | CLAUDE.md | New test |
| `map { @$_ } @aoa` flattening | CLAUDE.md | New test |
| `next`/`last` with labels | CLAUDE.md | Extend `modifiers.pl` |
| `$Pkg::arr` / `$Pkg::hash` cross-package | CLAUDE.md | New test |
| Carp module (`croak`/`carp`/`confess`/`cluck`) | CLAUDE.md | New test |
| `use parent -norequire` | CLAUDE.md | Extend `inherit.pl` |

---

## Recommended Development Workflow

When applying a fix or developing new code:

```bash
# 1. Build the compiler
make clean && make

# 2. Run ALL correctness tests (uses cache for benchmarks automatically)
make test-all

# 3. If benchmarks are cached, results are reused. If not, they are recorded.
#    To force re-run of benchmarks after a significant change:
make test-all -- --force-benchmark

# 4. To view cached benchmark results:
make test-cache

# 5. To clear cache and force fresh runs:
make test-clear-cache
```

**Key principle**: Always run `make test-all` (or `./run_tests.sh tests/`) before committing changes. This ensures:
- All smoke tests are validated against perl output
- All assertion tests pass
- All benchmarks are verified (using cache if available)
- Any regression is caught immediately

### When to Use `--force-benchmark`

- After fixing a bug that could affect benchmark correctness
- After optimizing code that changes algorithmic behavior
- When adding new features that might impact performance
- When the cache is stale (e.g., after hardware changes)

### When Cache Is Sufficient

- Small fixes to unrelated code paths
- Bug fixes that don't affect numerical output
- Documentation/comment changes
- Build system changes (verify with `make test` first)
