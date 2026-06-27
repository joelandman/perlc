# perlc — Agent Instructions

This file contains all operational instructions for agent sessions working on the perlc Perl→LLVM compiler project. Load this as your default mode of operation.

## Build System

### Compiler Versions
- **LLVM**: 18 (clang++-18, llvm-config-18, clang-18)
- **C++**: C++17
- **C**: Standard C99
- **Standard library**: libstdc++ (do NOT use -stdlib=libc++ — causes ABI mismatch with LLVM 18)

### Build Commands
```bash
make              # Build perlc compiler
make clean        # Clean build artifacts
make test         # Run assertion-based contract tests (fast)
make test-all     # Run all tests with perl output comparison
make test-smoke   # Run only smoke/benchmark tests
make test-assertion  # Run only assertion tests
make test-cache   # Show cached benchmark results
make test-clear-cache  # Clear cached benchmark results
```

### Known Build Issues
- LLVM 18 is incompatible with GCC 15/16 (not 13). If build fails with `__normal_iterator` errors, downgrade GCC to 13 or earlier.
- Do NOT use `-stdlib=libc++` — LLVM 18 was built with libstdc++ and mixing causes linker errors.
- The `ast.cpp` file was created to move `Node::clone()` out of the header to fix template completeness issues.

## Test Infrastructure

### Test Runner: `run_tests.sh`

A comprehensive test runner at the project root that provides correctness validation for ALL test files.

#### Usage
```bash
./run_tests.sh [options] [test_file_or_directory ...]
./run_tests.sh tests/                          # Run all tests
./run_tests.sh tests/hello.pl                  # Run single test
./run_tests.sh -v tests/                       # Verbose output
./run_tests.sh -j 4 tests/                     # Parallel execution
./run_tests.sh --skip-compile tests/           # Only run perl (no compilation)
./run_tests.sh --smoke-only tests/             # Only smoke/benchmark tests
./run_tests.sh --assertion-only tests/         # Only assertion tests
./run_tests.sh --show-cache                    # Show cached benchmark results
./run_tests.sh --clear-cache                   # Clear cached results
./run_tests.sh --force-benchmark tests/        # Force re-run of benchmarks
./run_tests.sh --no-benchmark-cache tests/     # Disable caching
./run_tests.sh --benchmark-timeout 600 tests/  # Set benchmark timeout
```

#### Options
| Option | Description |
|--------|-------------|
| `-c, --compiler PATH` | Path to perlc (default: ./perlc) |
| `-p, --perl PATH` | Path to perl interpreter (default: perl) |
| `-d, --diff-tool CMD` | Diff command (default: diff) |
| `-v, --verbose` | Show detailed output including diffs |
| `-q, --quiet` | Only show failures |
| `-n, --dry-run` | Print what would be tested |
| `-j N` | Run N tests in parallel |
| `--skip-compile` | Skip compilation, only run perl |
| `--skip-interp` | Skip perl, only run compiled binary |
| `--keep-tmp` | Keep temporary files for debugging |
| `--timeout SEC` | Timeout per test (default: 30) |
| `--benchmark-timeout SEC` | Timeout for benchmarks (default: 300) |
| `--ignore-exit` | Ignore exit code differences |
| `--ignore-whitespace` | Ignore trailing whitespace differences |
| `--sort-output` | Sort output lines before comparing |
| `--force-benchmark` | Force re-run of cached benchmarks |
| `--no-benchmark-cache` | Disable benchmark caching |
| `--show-cache` | Show cached results and exit |
| `--clear-cache` | Clear cached results |
| `--smoke-only` | Only smoke/benchmark tests |
| `--assertion-only` | Only assertion tests |

#### Test Classification
- **Assertion tests**: Have built-in `die()` checks (threads_atomic.pl, xs_ffi.pl, dbi_sqlite.pl, test_require_simple.pl, test_do_filename.pl, xs_ffi.pl)
- **Smoke tests**: Print output but no assertions (hello.pl, arith.pl, fib.pl, etc.)
- **Benchmark tests**: Performance-oriented (fibn.pl, mbs.pl, nbody.pl, fk.pl, bt.pl)
- **Placeholder tests**: No real testing (xs_dbi_test.pl — skipped)

#### Output Status Codes
- `PASS`: Compiled output matches perl
- `FAIL`: General failure
- `SKIP`: Test skipped
- `COMP`: Compilation failed, perl passed
- `RUNT`: Perl failed, compiled binary passed
- `MISM`: Both ran but outputs differ
- `TIME`: Timed out
- `CACHE`: Using cached result from tests.csv
- `BENCH`: Benchmark result recorded in tests.csv

#### Temporary Files
- `/tmp/perlc_test_<name>_*/` — Per-test temp directory
  - `perl_output.txt`, `compiled_output.txt`
  - `perl_stderr.txt`, `compiled_stderr.txt`
  - `diff.txt`, `status.txt`, `compile.log`

### Benchmark Caching

Long-running benchmark tests are cached per-hostname in `tests.csv` (created automatically on first benchmark run).

#### Cache File Format
```csv
hostname,benchmark_test,time_seconds,success,output,accuracy
myhost.example.com,mbs.pl,2.345,yes,0.123456,0.999
```

#### Cache Behavior
- Before running a benchmark, checks if a cached result exists for the current hostname
- If found, uses cached result (prints `CACHE: test_name`)
- If not found, runs the test and records result (prints `BENCH: test_name`)
- Cache key: `hostname + benchmark_test_name`

#### Cache Commands
```bash
./run_tests.sh --show-cache    # View cached results for current host
./run_tests.sh --clear-cache   # Clear all cached results for current host
./run_tests.sh --force-benchmark tests/  # Force re-run of benchmarks
./run_tests.sh --no-benchmark-cache tests/  # Disable caching
```

## Recommended Development Workflow

When applying a fix or developing new code:

```bash
# 1. Build the compiler
make clean && make

# 2. Run ALL correctness tests (uses cache for benchmarks automatically)
make test-all

# 3. If benchmarks are cached, results are reused. If not, they are recorded.
#    To force re-run of benchmarks after a significant change:
./run_tests.sh --force-benchmark tests/

# 4. To view cached benchmark results:
./run_tests.sh --show-cache

# 5. To clear cache and force fresh runs:
./run_tests.sh --clear-cache
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

## Test Quality Assessment

### Current State (Pre-Runner)
| Category | Count | Description |
|----------|-------|-------------|
| Smoke tests (zero assertions) | 29 | Print output but never verify correctness |
| Light validation (conditional prints) | 9 | Use `print "ok" if $cond` but don't affect exit code |
| Proper assertions (die-on-fail) | 5 | Exit non-zero on failure |
| Placeholders (no-op) | 1 | Always passes |

The test runner eliminates the smoke test problem by providing external correctness validation for all 29 smoke tests automatically.

### Tests With Proper Assertions (die-on-fail)
- `threads_atomic.pl` — 8+ assertions + aggregate failure block
- `xs_ffi.pl` — 40+ individual checks via `check()`
- `dbi_sqlite.pl` — 20+ individual checks via `check()`
- `test_require_simple.pl` — 4 checks via `check()`
- `test_do_filename.pl` — 10 checks via `check()`

### Smoke Tests (Zero Assertions — 29 files)
`hello.pl`, `arith.pl`, `fib.pl`, `range.pl`, `modifiers.pl`, `hash.pl`, `refs.pl`, `builtins.pl`, `builtins2.pl`, `fileio.pl`, `fileops.pl`, `sprintf.pl`, `regex.pl`, `regex_g.pl`, `advanced.pl`, `features.pl`, `oop.pl`, `closures.pl`, `usemod.pl`, `inherit.pl`, `defaults.pl`, `interp.pl`, `misc.pl`, `tr.pl`, `fibn.pl`, `mbs.pl`, `nbody.pl`, `nb.pl`, `xs_dbi_test.pl`

### Light Validation Tests (Conditional Prints Only — 9 files)
`regex_named.pl`, `newfeatures.pl`, `wantarray.pl`, `tier1.pl`, `tier2.pl`, `tier3.pl`, `threads.pl`, `destroy.pl`, `eval_string.pl`, `completeness.pl`

## Defects Summary

See `TESTS.md` for the complete defect registry with IDs, locations, and fix tracking.

### Critical Defects
- **D1**: All numeric compound assignments on shared scalars go through `perl_atomic_add` — `-=` adds instead of subtracts, `*=` multiplies as addition
- **D2**: `chop @arr` calls `perl_chomp_array` instead of removing last characters
- **D3**: `local @arr` / `local %hash` silently no-ops for function-scope variables

### High Severity Defects
- **D5**: Closure + range-with-captured-variable emits `undef` bound
- **D6**: `for (my $i = 0; ...)` C-style init is dead code
- **D7**: `s///` without second `\x01` delimiter causes `npos` underflow
- **D8**: `parseOrRhs` handles only small subset of statement keywords
- **D9**: `lastSqrtInput_` never cleared, can match wrong variable

### Build Blocker
- **B1**: LLVM 18 + GCC 15/16 incompatibility — project cannot build on systems with GCC 15/16

## Test Coverage Gaps

Features claimed to be implemented but have NO corresponding test:
- `@{$href}{LIST}` hash-ref slices
- `@{$aref}[LIST]` array-ref slices
- `scalar(@{$ref})`
- `$h{k}++` on missing keys
- lvalue slices `@arr[i,j] = list`
- `map { @$_ } @aoa` flattening
- `next`/`last` with labels
- `$Pkg::arr` / `$Pkg::hash` cross-package access
- Carp module functions
- `use parent -norequire`

## Documentation Files

| File | Purpose |
|------|---------|
| `README.md` | User-facing documentation (preserved separately) |
| `plans` | Consolidated project plan, defects, remediation plan, workflow |
| `TESTS.md` | Defect registry, test tracking, infrastructure documentation |
| `INSTRUCTIONS.md` | This file — agent operational instructions |
| `run_tests.sh` | Comprehensive test runner with benchmark caching |

## Source Files

| File | Role |
|------|------|
| `src/lexer.h/cpp` | Context-aware tokenizer |
| `src/ast.h/cpp` | Node kinds (NK enum) and Node struct |
| `src/parser.h/cpp` | Recursive-descent parser → AST |
| `src/codegen.h/cpp` | AST → LLVM IR via IRBuilder |
| `src/runtime.h/c` | C runtime: PerlValue tagged union + all operations |
| `src/main.cpp` | Driver: lex → parse → codegen → clang link |
| `src/jit.h/cpp` | JIT stub (non-functional, avoids LLVM header conflicts) |
| `src/eval_jit.cpp` | String eval support |

## Perl Test File Requirements

All test `.pl` files must include:
```perl
use feature "say";  # For say() availability
use lib "lib";      # For local module loading (if needed)
```

These were added to all 32 test files that use `say` or local modules.

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `PERLC_TEST_VERBOSE` | 0 | Same as -v |
| `PERLC_TEST_QUIET` | 0 | Same as -q |
| `PERLC_TEST_KEEP_TMP` | 0 | Same as --keep-tmp |
| `PERLC_TEST_TIMEOUT` | 30 | Same as --timeout |
| `PERLC_TEST_BENCHMARK_TIMEOUT` | 300 | Same as --benchmark-timeout |
| `PERLC_TEST_JOBS` | 1 | Same as -j |
| `PERLC_TEST_SORT` | 0 | Same as --sort-output |
| `PERLC_TEST_IGNORE_WS` | 0 | Same as --ignore-whitespace |
| `PERLC_TEST_IGNORE_EXIT` | 0 | Same as --ignore-exit |
| `PERLC_TEST_NO_CACHE` | 0 | Same as --no-benchmark-cache |
| `PERLC_TEST_FORCE_CACHE` | 0 | Same as --force-benchmark |

## Git Workflow

- Do NOT commit changes unless explicitly asked
- Before committing: inspect `git status`, `git diff`, and `git log --oneline`
- Write concise commit messages matching project style
- Never commit secrets or keys
