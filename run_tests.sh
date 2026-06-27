#!/usr/bin/env bash
#
# run_tests.sh — Comprehensive test runner for perlc
#
# This script compiles each Perl test file using perlc, runs both the Perl
# interpreter and the compiled binary, and compares their outputs for
# correctness. It also runs the existing assertion-based tests.
#
# Benchmark caching: Long-running benchmark tests (mbs.pl, nbody.pl, fibn.pl,
# fk.pl, bt.pl, etc.) are cached per-hostname. Results are stored in
# tests.csv and reused on subsequent runs on the same machine.
#
# Usage:
#   ./run_tests.sh [options] [test_file_or_directory ...]
#
# Options:
#   -h, --help                  Show this help message
#   -c, --compiler PATH         Path to perlc compiler (default: ./perlc)
#   -p, --perl PATH             Path to perl interpreter (default: perl)
#   -d, --diff-tool CMD         Command to use for diffing output (default: diff)
#   -v, --verbose               Show detailed output including diffs
#   -q, --quiet                 Only show failures
#   -n, --dry-run               Print what would be tested without running
#   -j, --jobs N                Run N tests in parallel (default: 1)
#   --skip-compile              Skip compilation, only run perl interpreter
#   --skip-interp               Skip perl interpreter, only run compiled binary
#   --keep-tmp                  Keep temporary files for debugging
#   --timeout SEC               Timeout per test in seconds (default: 30)
#   --benchmark-timeout SEC     Timeout for benchmark tests (default: 300)
#   --ignore-exit               Ignore exit code differences
#   --ignore-whitespace         Ignore trailing whitespace differences
#   --sort-output               Sort output lines before comparing
#   --force-benchmark           Force re-run of cached benchmark tests
#   --no-benchmark-cache        Disable benchmark result caching
#   --show-cache                Show cached benchmark results and exit
#   --clear-cache               Clear all cached benchmark results
#   --smoke-only                Only run smoke/benchmark tests
#   --assertion-only            Only run assertion tests
#
# Exit codes:
#   0  All tests passed
#   1  One or more tests failed
#   2  Usage error
#   3  Compiler not found (when compilation is required)
#
# Test Classification:
#   - "assertion" tests: Have built-in die() checks (threads_atomic.pl, xs_ffi.pl, etc.)
#   - "smoke" tests: Print output but have no assertions (hello.pl, arith.pl, etc.)
#   - "benchmark" tests: Performance-oriented (fibn.pl, mbs.pl, nbody.pl, fk.pl, bt.pl)
#   - "placeholder" tests: No real testing (xs_dbi_test.pl)
#
# For smoke and benchmark tests, this runner provides correctness validation
# by comparing compiled binary output against the Perl interpreter.
#
# For assertion tests, the runner first runs the assertion tests, then also
# compares output against perl if the assertion test passes.
#
# Benchmark Caching:
#   Benchmark results are cached in tests.csv (same directory as this script).
#   The cache key is: hostname + benchmark test name.
#   Before running a benchmark, the runner checks if a cached result exists
#   for the current hostname. If found, it uses the cached result instead
#   of re-running the test.
#
#   Cache file format (CSV):
#     hostname,benchmark_test,time_seconds,success,output,accuracy
#     example: "myhost.example.com,mbs.pl,2.345,yes,0.123456,0.999"
#
#   - hostname: Machine hostname (from hostname command)
#   - benchmark_test: Test filename (e.g., mbs.pl)
#   - time_seconds: Wall-clock time in seconds
#   - success: "yes" or "no"
#   - output: First 200 chars of stdout (truncated if longer)
#   - accuracy: Numeric accuracy if available, else "N/A"
#
#   To force re-run of a cached benchmark, use --force-benchmark.
#   To disable caching entirely, use --no-benchmark-cache.
#   To view cached results, use --show-cache.
#   To clear all cached results, use --clear-cache.
#
# Output Format:
#   PASS: test_name (compiled output matches perl)
#   FAIL: test_name (reason: diff details)
#   SKIP: test_name (reason)
#   COMP: test_name (compiled binary failed, perl passed)
#   RUNT: test_name (perl failed, compiled binary passed)
#   MISM: test_name (both ran but outputs differ)
#   TIME: test_name (timed out)
#   CACHE: test_name (using cached result from tests.csv)
#   BENCH: test_name (benchmark result recorded in tests.csv)
#
# Examples:
#   # Run all tests in tests/ directory
#   ./run_tests.sh tests/
#
#   # Run a single test
#   ./run_tests.sh tests/hello.pl
#
#   # Run with verbose output
#   ./run_tests.sh -v tests/arith.pl
#
#   # Run with parallel jobs
#   ./run_tests.sh -j 4 tests/
#
#   # Compare output ignoring whitespace
#   ./run_tests.sh --ignore-whitespace tests/
#
#   # Only run smoke tests (not assertion tests)
#   ./run_tests.sh --smoke-only tests/
#
#   # Only run assertion tests
#   ./run_tests.sh --assertion-only tests/
#
#   # Skip compilation, just verify perl runs
#   ./run_tests.sh --skip-compile tests/
#
#   # Force re-run of benchmark tests
#   ./run_tests.sh --force-benchmark tests/
#
#   # Show cached benchmark results
#   ./run_tests.sh --show-cache
#
#   # Clear all cached benchmark results
#   ./run_tests.sh --clear-cache
#
# Temporary files:
#   /tmp/perlc_test_*/  — Temporary directory per test
#     perl_output.txt    — Output from perl interpreter
#     compiled_output.txt — Output from compiled binary
#     perl_stderr.txt    — Stderr from perl interpreter
#     compiled_stderr.txt — Stderr from compiled binary
#     diff.txt           — Diff of outputs (if different)
#     status.txt         — Exit codes from both runs
#
# Environment variables:
#   PERLC_TEST_VERBOSE=1         Same as -v
#   PERLC_TEST_QUIET=1           Same as -q
#   PERLC_TEST_KEEP_TMP=1        Same as --keep-tmp
#   PERLC_TEST_TIMEOUT=30        Same as --timeout
#   PERLC_TEST_BENCHMARK_TIMEOUT=300  Same as --benchmark-timeout
#   PERLC_TEST_JOBS=1            Same as -j
#   PERLC_TEST_SORT=1            Same as --sort-output
#   PERLC_TEST_IGNORE_WS=1       Same as --ignore-whitespace
#   PERLC_TEST_IGNORE_EXIT=1     Same as --ignore-exit
#   PERLC_TEST_NO_CACHE=1        Same as --no-benchmark-cache
#   PERLC_TEST_FORCE_CACHE=1     Same as --force-benchmark
#
# Author: perlc test infrastructure
# License: Same as perlc project

set -euo pipefail

# ── Defaults ────────────────────────────────────────────────────────────────
COMPILER="${PERLC_TEST_COMPILER:-./perlc}"
PERL="${PERLC_TEST_PERL:-perl}"
DIFF_TOOL="${PERLC_TEST_DIFF_TOOL:-diff}"
VERBOSE="${PERLC_TEST_VERBOSE:-0}"
QUIET="${PERLC_TEST_QUIET:-0}"
KEEP_TMP="${PERLC_TEST_KEEP_TMP:-0}"
TIMEOUT="${PERLC_TEST_TIMEOUT:-30}"
BENCHMARK_TIMEOUT="${PERLC_TEST_BENCHMARK_TIMEOUT:-300}"
JOBS="${PERLC_TEST_JOBS:-1}"
SORT_OUTPUT="${PERLC_TEST_SORT:-0}"
IGNORE_WS="${PERLC_TEST_IGNORE_WS:-0}"
IGNORE_EXIT="${PERLC_TEST_IGNORE_EXIT:-0}"
DRY_RUN=0
SKIP_COMPILE=0
SKIP_INTERP=0
SMOKE_ONLY=0
ASSERTION_ONLY=0
PARALLEL=0
FORCE_BENCHMARK=0
NO_CACHE=0
SHOW_CACHE=0
CLEAR_CACHE=0

# ── Counters ────────────────────────────────────────────────────────────────
TOTAL=0
PASSED=0
FAILED=0
SKIPPED=0
COMPILED_FAIL=0   # Binary failed, perl passed
RUNTIME_FAIL=0    # Perl failed, binary passed
MISMATCH=0        # Both ran, outputs differ
TIMEOUT_COUNT=0
CACHE_HIT=0       # Benchmark cache hits
CACHE_RECORDED=0  # New benchmark results recorded

# ── Color codes ─────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    BLUE='\033[0;34m'
    CYAN='\033[0;36m'
    MAGENTA='\033[0;35m'
    NC='\033[0m'
else
    RED=''
    GREEN=''
    YELLOW=''
    BLUE=''
    CYAN=''
    MAGENTA=''
    NC=''
fi

# ── Utility functions ───────────────────────────────────────────────────────
log_verbose() {
    if [[ "$VERBOSE" == "1" ]]; then
        echo -e "$@"
    fi
}

log_quiet() {
    if [[ "$QUIET" == "0" ]]; then
        echo -e "$@"
    fi
}

die() {
    echo -e "${RED}ERROR: $*${NC}" >&2
    exit 1
}

usage() {
    sed -n '2,/^# Author:/p' "$0" | grep '^#' | sed 's/^# \?//'
    exit 0
}

# Get hostname for cache key
get_hostname() {
    hostname 2>/dev/null || echo "unknown"
}

# Get the directory where this script lives (for tests.csv location)
get_script_dir() {
    local script_path
    script_path="$(readlink -f "$0" 2>/dev/null || echo "$0")"
    dirname "$script_path"
}

# Get the path to tests.csv
get_cache_file() {
    local script_dir
    script_dir=$(get_script_dir)
    echo "${script_dir}/tests.csv"
}

# Check if a test file is an assertion test (has die() for failures)
is_assertion_test() {
    local file="$1"
    grep -q 'die\|check()\|assert(' "$file" 2>/dev/null
}

# Check if a test file is a benchmark
is_benchmark() {
    local file="$1"
    local basename
    basename=$(basename "$file" .pl)
    case "$basename" in
        fibn|mbs|nbody|nb|fk|bt|perf|bench*) return 0 ;;
        *) return 1 ;;
    esac
}

# Check if a test file is a placeholder (no real testing)
is_placeholder() {
    local file="$1"
    local basename
    basename=$(basename "$file" .pl)
    case "$basename" in
        xs_dbi_test) return 0 ;;
        *) return 1 ;;
    esac
}

# ── Benchmark cache functions ───────────────────────────────────────────────

# Check if there's a cached result for this benchmark on this host
# Returns 0 if cache hit, 1 if miss
check_cache() {
    local test_name="$1"
    local cache_file
    cache_file=$(get_cache_file)
    local host
    host=$(get_hostname)

    if [[ "$NO_CACHE" == "1" ]] || [[ "$FORCE_BENCHMARK" == "1" ]]; then
        return 1
    fi

    if [[ ! -f "$cache_file" ]]; then
        return 1
    fi

    # Look for matching entry: hostname,test_name
    if grep -q "^${host},${test_name}," "$cache_file" 2>/dev/null; then
        return 0
    fi

    return 1
}

# Get cached result for a benchmark
# Outputs: time_seconds,success,output,accuracy
get_cached_result() {
    local test_name="$1"
    local cache_file
    cache_file=$(get_cache_file)
    local host
    host=$(get_hostname)

    # Extract the matching line and parse fields
    local line
    line=$(grep "^${host},${test_name}," "$cache_file" 2>/dev/null | tail -1)
    if [[ -z "$line" ]]; then
        echo ""
        return
    fi

    # Parse CSV (simple: no quoted fields with commas)
    echo "$line" | cut -d',' -f3-6
}

# Record a benchmark result to cache
# Arguments: test_name, time_seconds, success, output, accuracy
record_cache() {
    local test_name="$1"
    local time_seconds="$2"
    local success="$3"
    local output="$4"
    local accuracy="$5"
    local cache_file
    cache_file=$(get_cache_file)
    local host
    host=$(get_hostname)

    # Truncate output to 200 chars
    output="${output:0:200}"

    # Ensure file exists with header
    if [[ ! -f "$cache_file" ]]; then
        echo "hostname,benchmark_test,time_seconds,success,output,accuracy" > "$cache_file"
    fi

    # Remove old entry if exists
    if grep -q "^${host},${test_name}," "$cache_file" 2>/dev/null; then
        local tmp="${cache_file}.tmp"
        grep -v "^${host},${test_name}," "$cache_file" > "$tmp" 2>/dev/null || true
        mv "$tmp" "$cache_file"
    fi

    # Append new entry
    echo "${host},${test_name},${time_seconds},${success},${output},${accuracy}" >> "$cache_file"
}

# Show cached benchmark results
show_cache() {
    local cache_file
    cache_file=$(get_cache_file)
    local host
    host=$(get_hostname)

    if [[ ! -f "$cache_file" ]]; then
        echo "No cached results found (${cache_file} does not exist)"
        return 0
    fi

    echo "Cached benchmark results for host: ${host}"
    echo "Cache file: ${cache_file}"
    echo ""
    printf "%-20s %-15s %-10s %-8s %s\n" "TEST" "TIME(s)" "SUCCESS" "OUTPUT" "ACCURACY"
    printf "%-20s %-15s %-10s %-8s %s\n" "----" "-------" "-------" "------" "--------"

    grep "^${host}," "$cache_file" 2>/dev/null | while IFS=',' read -r h test time success output accuracy; do
        # Truncate output for display
        local disp_output="${output:0:40}"
        if [[ ${#output} -gt 40 ]]; then
            disp_output="${disp_output}..."
        fi
        printf "%-20s %-15s %-10s %-8s %s\n" "$test" "$time" "$success" "$disp_output" "$accuracy"
    done

    echo ""
    echo "Total cached benchmarks: $(grep -c "^${host}," "$cache_file" 2>/dev/null || echo 0)"
}

# Clear all cached benchmark results
clear_cache() {
    local cache_file
    cache_file=$(get_cache_file)
    local host
    host=$(get_hostname)

    if [[ ! -f "$cache_file" ]]; then
        echo "No cache to clear"
        return 0
    fi

    local tmp="${cache_file}.tmp"
    grep -v "^${host}," "$cache_file" > "$tmp" 2>/dev/null || true
    mv "$tmp" "$cache_file"
    echo "Cleared ${host} entries from ${cache_file}"
}

# ── Run a test and compare outputs ──────────────────────────────────────────
run_test() {
    local test_file="$1"
    local test_name
    test_name=$(basename "$test_file" .pl)
    local tmp_dir="/tmp/perlc_test_${test_name}_$$"
    local compiled_bin="${tmp_dir}/compiled"

    TOTAL=$((TOTAL + 1))

    # Check if test should be filtered
    if [[ "$SMOKE_ONLY" == "1" ]] && is_assertion_test "$test_file"; then
        SKIPPED=$((SKIPPED + 1))
        log_quiet "${YELLOW}SKIP:${NC} ${test_name} (smoke-only mode, assertion test)"
        return
    fi
    if [[ "$ASSERTION_ONLY" == "1" ]] && ! is_assertion_test "$test_file"; then
        SKIPPED=$((SKIPPED + 1))
        log_quiet "${YELLOW}SKIP:${NC} ${test_name} (assertion-only mode, smoke/benchmark)"
        return
    fi
    if is_placeholder "$test_file"; then
        SKIPPED=$((SKIPPED + 1))
        log_quiet "${YELLOW}SKIP:${NC} ${test_name} (placeholder test)"
        return
    fi

    # Dry run
    if [[ "$DRY_RUN" == "1" ]]; then
        log_quiet "DRY-RUN: ${test_name}"
        PASSED=$((PASSED + 1))
        return
    fi

    # Check benchmark cache
    if is_benchmark "$test_file" && [[ "$SKIP_COMPILE" != "1" ]] && [[ "$SKIP_INTERP" != "1" ]]; then
        if check_cache "$test_name"; then
            local cached
            cached=$(get_cached_result "$test_name")
            if [[ -n "$cached" ]]; then
                local cached_time cached_success cached_output cached_accuracy
                IFS=',' read -r cached_time cached_success cached_output cached_accuracy <<< "$cached"
                CACHE_HIT=$((CACHE_HIT + 1))
                PASSED=$((PASSED + 1))
                log_quiet "${MAGENTA}CACHE:${NC} ${test_name} (cached: ${cached_time}s, success=${cached_success})"
                if [[ "$VERBOSE" == "1" ]]; then
                    echo -e "${CYAN}  Cached output: ${cached_output}${NC}"
                    echo -e "${CYAN}  Cached accuracy: ${cached_accuracy}${NC}"
                fi
                return
            fi
        fi
    fi

    # Check general test cache (skip-compile mode — cache perl results for all tests)
    if [[ "$SKIP_COMPILE" == "1" ]] && [[ "$SKIP_INTERP" != "1" ]]; then
        if check_cache "$test_name"; then
            local cached
            cached=$(get_cached_result "$test_name")
            if [[ -n "$cached" ]]; then
                local cached_time cached_success cached_output cached_accuracy
                IFS=',' read -r cached_time cached_success cached_output cached_accuracy <<< "$cached"
                CACHE_HIT=$((CACHE_HIT + 1))
                if [[ "$cached_success" == "yes" ]]; then
                    PASSED=$((PASSED + 1))
                    log_quiet "${MAGENTA}CACHE:${NC} ${test_name} (cached perl: ${cached_time}s, success=${cached_success})"
                else
                    FAILED=$((FAILED + 1))
                    log_quiet "${MAGENTA}CACHE:${NC} ${test_name} (cached perl: ${cached_time}s, success=${cached_success})"
                fi
                return
            fi
        fi
    fi

    # Create temp directory
    mkdir -p "$tmp_dir"

    # Determine timeout for this test
    local test_timeout="$TIMEOUT"
    if is_benchmark "$test_file"; then
        test_timeout="$BENCHMARK_TIMEOUT"
    fi

    # ── Run Perl interpreter ────────────────────────────────────────────
    local perl_exit=0
    local perl_output_file="${tmp_dir}/perl_output.txt"
    local perl_stderr_file="${tmp_dir}/perl_stderr.txt"
    local perl_start perl_end perl_elapsed

    if [[ "$SKIP_INTERP" != "1" ]]; then
        perl_start=$(date +%s%N)
        timeout "$test_timeout" "$PERL" "$test_file" >"$perl_output_file" 2>"$perl_stderr_file" || perl_exit=$?
        perl_end=$(date +%s%N)
        if [[ "$perl_exit" -eq 124 ]]; then
            perl_exit=124  # Timeout
        fi
        perl_elapsed=$(echo "scale=3; ($perl_end - $perl_start) / 1000000000" | bc 2>/dev/null || echo "N/A")
    else
        touch "$perl_output_file" "$perl_stderr_file"
        perl_exit=0
        perl_elapsed="0"
    fi

    # ── Compile and run binary ──────────────────────────────────────────
    local compiled_exit=0
    local compiled_output_file="${tmp_dir}/compiled_output.txt"
    local compiled_stderr_file="${tmp_dir}/compiled_stderr.txt"
    local compiled_start compiled_end compiled_elapsed

    if [[ "$SKIP_COMPILE" != "1" ]]; then
        # Check if compiler exists
        if [[ ! -x "$COMPILER" ]]; then
            SKIPPED=$((SKIPPED + 1))
            log_quiet "${YELLOW}SKIP:${NC} ${test_name} (compiler not found: $COMPILER)"
            [[ "$KEEP_TMP" != "1" ]] && rm -rf "$tmp_dir"
            return
        fi

        # Compile
        local compile_log="${tmp_dir}/compile.log"
        timeout "$test_timeout" "$COMPILER" "$test_file" -o "$compiled_bin" >"$compile_log" 2>&1 || {
            local compile_exit=$?
            if [[ "$compile_exit" -eq 124 ]]; then
                TIMEOUT_COUNT=$((TIMEOUT_COUNT + 1))
                FAILED=$((FAILED + 1))
                log_quiet "${RED}TIME:${NC} ${test_name} (compilation timed out)"
                log_verbose "Compile log:\n$(cat "$compile_log")"
                [[ "$KEEP_TMP" != "1" ]] && rm -rf "$tmp_dir"
                return
            fi
            COMPILED_FAIL=$((COMPILED_FAIL + 1))
            FAILED=$((FAILED + 1))
            log_quiet "${RED}COMP:${NC} ${test_name} (compilation failed, exit $compile_exit)"
            log_verbose "Compile log:\n$(cat "$compile_log")"
            [[ "$KEEP_TMP" != "1" ]] && rm -rf "$tmp_dir"
            return
        }

        # Run compiled binary
        compiled_start=$(date +%s%N)
        timeout "$test_timeout" "$compiled_bin" >"$compiled_output_file" 2>"$compiled_stderr_file" || compiled_exit=$?
        compiled_end=$(date +%s%N)
        if [[ "$compiled_exit" -eq 124 ]]; then
            compiled_exit=124  # Timeout
        fi
        compiled_elapsed=$(echo "scale=3; ($compiled_end - $compiled_start) / 1000000000" | bc 2>/dev/null || echo "N/A")
    else
        touch "$compiled_output_file" "$compiled_stderr_file"
        compiled_exit=0
        compiled_elapsed="0"
    fi

    # ── Compare results ─────────────────────────────────────────────────
    # Handle timeouts
    if [[ "$perl_exit" -eq 124 ]] && [[ "$compiled_exit" -eq 124 ]]; then
        TIMEOUT_COUNT=$((TIMEOUT_COUNT + 1))
        FAILED=$((FAILED + 1))
        log_quiet "${RED}TIME:${NC} ${test_name} (both timed out)"
        [[ "$KEEP_TMP" != "1" ]] && rm -rf "$tmp_dir"
        return
    fi
    if [[ "$perl_exit" -eq 124 ]]; then
        TIMEOUT_COUNT=$((TIMEOUT_COUNT + 1))
        FAILED=$((FAILED + 1))
        log_quiet "${RED}TIME:${NC} ${test_name} (perl timed out)"
        [[ "$KEEP_TMP" != "1" ]] && rm -rf "$tmp_dir"
        return
    fi
    if [[ "$compiled_exit" -eq 124 ]]; then
        TIMEOUT_COUNT=$((TIMEOUT_COUNT + 1))
        FAILED=$((FAILED + 1))
        log_quiet "${RED}TIME:${NC} ${test_name} (compiled timed out)"
        [[ "$KEEP_TMP" != "1" ]] && rm -rf "$tmp_dir"
        return
    fi

    # If perl failed and we're not skipping compilation
    if [[ "$perl_exit" -ne 0 ]] && [[ "$SKIP_COMPILE" != "1" ]]; then
        if [[ "$compiled_exit" -eq 0 ]]; then
            RUNTIME_FAIL=$((RUNTIME_FAIL + 1))
            FAILED=$((FAILED + 1))
            log_quiet "${RED}RUNT:${NC} ${test_name} (perl exited $perl_exit, compiled exited $compiled_exit)"
            [[ "$KEEP_TMP" != "1" ]] && rm -rf "$tmp_dir"
            return
        else
            if [[ "$perl_exit" -eq "$compiled_exit" ]]; then
                PASSED=$((PASSED + 1))
                log_quiet "${GREEN}PASS:${NC} ${test_name} (both failed with exit $perl_exit)"
                [[ "$KEEP_TMP" != "1" ]] && rm -rf "$tmp_dir"
                return
            fi
            FAILED=$((FAILED + 1))
            log_quiet "${RED}MISM:${NC} ${test_name} (perl exited $perl_exit, compiled exited $compiled_exit)"
            [[ "$KEEP_TMP" != "1" ]] && rm -rf "$tmp_dir"
            return
        fi
    fi

    # If only perl ran (skip-compile mode)
    if [[ "$SKIP_COMPILE" == "1" ]]; then
        # Record to cache
        if [[ "$SKIP_INTERP" != "1" ]] && [[ "$NO_CACHE" != "1" ]]; then
            local perl_output_text
            perl_output_text=$(cat "$perl_output_file" 2>/dev/null | head -c 200 | tr '\n' ' ' | sed 's/  */ /g')
            local cached_success="no"
            if [[ "$perl_exit" -eq 0 ]]; then
                cached_success="yes"
            fi
            record_cache "$test_name" "$perl_elapsed" "$cached_success" "$perl_output_text" "N/A"
        fi
        if [[ "$perl_exit" -eq 0 ]]; then
            PASSED=$((PASSED + 1))
            log_quiet "${GREEN}PASS:${NC} ${test_name} (perl ran successfully)"
            [[ "$KEEP_TMP" != "1" ]] && rm -rf "$tmp_dir"
            return
        else
            FAILED=$((FAILED + 1))
            log_quiet "${RED}FAIL:${NC} ${test_name} (perl exited $perl_exit)"
            [[ "$KEEP_TMP" != "1" ]] && rm -rf "$tmp_dir"
            return
        fi
    fi

    # If only compiled ran (skip-interp mode)
    if [[ "$SKIP_INTERP" == "1" ]]; then
        if [[ "$compiled_exit" -eq 0 ]]; then
            PASSED=$((PASSED + 1))
            log_quiet "${GREEN}PASS:${NC} ${test_name} (compiled ran successfully)"
            [[ "$KEEP_TMP" != "1" ]] && rm -rf "$tmp_dir"
            return
        else
            FAILED=$((FAILED + 1))
            log_quiet "${RED}FAIL:${NC} ${test_name} (compiled exited $compiled_exit)"
            [[ "$KEEP_TMP" != "1" ]] && rm -rf "$tmp_dir"
            return
        fi
    fi

    # Both ran — compare outputs
    local perl_out="$perl_output_file"
    local compiled_out="$compiled_output_file"

    # Handle exit code comparison
    local exit_match=1
    if [[ "$IGNORE_EXIT" != "1" ]] && [[ "$perl_exit" -ne "$compiled_exit" ]]; then
        exit_match=0
    fi

    # Handle output comparison
    local output_match=1
    local diff_file="${tmp_dir}/diff.txt"

    if [[ "$SORT_OUTPUT" == "1" ]]; then
        sort "$perl_out" > "${tmp_dir}/perl_sorted.txt"
        sort "$compiled_out" > "${tmp_dir}/compiled_sorted.txt"
        perl_out="${tmp_dir}/perl_sorted.txt"
        compiled_out="${tmp_dir}/compiled_sorted.txt"
    fi

    if [[ "$IGNORE_WS" == "1" ]]; then
        sed 's/[[:space:]]*$//' "$perl_out" > "${tmp_dir}/perl_nows.txt"
        sed 's/[[:space:]]*$//' "$compiled_out" > "${tmp_dir}/compiled_nows.txt"
        perl_out="${tmp_dir}/perl_nows.txt"
        compiled_out="${tmp_dir}/compiled_nows.txt"
    fi

    "$DIFF_TOOL" -u "$perl_out" "$compiled_out" >"$diff_file" 2>&1 || output_match=0

    # Handle assertion tests
    if is_assertion_test "$test_file"; then
        if [[ "$output_match" == "1" ]]; then
            PASSED=$((PASSED + 1))
            log_quiet "${GREEN}PASS:${NC} ${test_name} (output matches perl)"
        else
            FAILED=$((FAILED + 1))
            MISMATCH=$((MISMATCH + 1))
            log_quiet "${RED}MISM:${NC} ${test_name} (output differs from perl)"
            if [[ "$VERBOSE" == "1" ]]; then
                echo -e "${CYAN}--- Diff ---${NC}"
                cat "$diff_file"
            fi
        fi
        [[ "$KEEP_TMP" != "1" ]] && rm -rf "$tmp_dir"
        return
    fi

    # For non-assertion tests, both outputs must match and exit codes should match
    if [[ "$exit_match" == "1" ]] && [[ "$output_match" == "1" ]]; then
        PASSED=$((PASSED + 1))
        log_quiet "${GREEN}PASS:${NC} ${test_name}"

        # Record benchmark result if applicable
        if is_benchmark "$test_file"; then
            local perl_output
            perl_output=$(head -c 200 "$perl_output_file" 2>/dev/null || echo "")
            local accuracy="N/A"
            # Try to extract accuracy from output (some benchmarks output accuracy info)
            if grep -qoP 'accuracy[:\s]+[0-9.]+' "$perl_output_file" 2>/dev/null; then
                accuracy=$(grep -oP 'accuracy[:\s]+[0-9.]+' "$perl_output_file" 2>/dev/null | tail -1 | grep -oP '[0-9.]+$' || echo "N/A")
            fi
            record_cache "$test_name" "$perl_elapsed" "yes" "$perl_output" "$accuracy"
            CACHE_RECORDED=$((CACHE_RECORDED + 1))
            log_quiet "${MAGENTA}BENCH:${NC} ${test_name} (recorded: ${perl_elapsed}s)"
        fi
    else
        FAILED=$((FAILED + 1))
        if [[ "$exit_match" == "0" ]] && [[ "$output_match" == "0" ]]; then
            log_quiet "${RED}MISM:${NC} ${test_name} (exit codes and output differ)"
        elif [[ "$exit_match" == "0" ]]; then
            log_quiet "${RED}MISM:${NC} ${test_name} (exit codes differ: perl=$perl_exit, compiled=$compiled_exit)"
        else
            log_quiet "${RED}MISM:${NC} ${test_name} (output differs from perl)"
        fi
        if [[ "$VERBOSE" == "1" ]]; then
            echo -e "${CYAN}--- Diff ---${NC}"
            cat "$diff_file"
            echo -e "${CYAN}--- Perl stderr ---${NC}"
            cat "$perl_stderr_file"
            echo -e "${CYAN}--- Compiled stderr ---${NC}"
            cat "$compiled_stderr_file"
        fi

        # Record benchmark failure if applicable
        if is_benchmark "$test_file"; then
            local perl_output
            perl_output=$(head -c 200 "$perl_output_file" 2>/dev/null || echo "")
            record_cache "$test_name" "$perl_elapsed" "no" "$perl_output" "N/A"
            CACHE_RECORDED=$((CACHE_RECORDED + 1))
        fi
    fi

    [[ "$KEEP_TMP" != "1" ]] && rm -rf "$tmp_dir"
}

# ── Parse arguments ─────────────────────────────────────────────────────────
TEST_FILES=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage ;;
        -c|--compiler) COMPILER="$2"; shift 2 ;;
        -p|--perl) PERL="$2"; shift 2 ;;
        -d|--diff-tool) DIFF_TOOL="$2"; shift 2 ;;
        -v|--verbose) VERBOSE=1; shift ;;
        -q|--quiet) QUIET=1; shift ;;
        -n|--dry-run) DRY_RUN=1; shift ;;
        -j|--jobs) JOBS="$2"; PARALLEL=1; shift 2 ;;
        --skip-compile) SKIP_COMPILE=1; shift ;;
        --skip-interp) SKIP_INTERP=1; shift ;;
        --keep-tmp) KEEP_TMP=1; shift ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --benchmark-timeout) BENCHMARK_TIMEOUT="$2"; shift 2 ;;
        --ignore-exit) IGNORE_EXIT=1; shift ;;
        --ignore-whitespace) IGNORE_WS=1; shift ;;
        --sort-output) SORT_OUTPUT=1; shift ;;
        --force-benchmark) FORCE_BENCHMARK=1; shift ;;
        --no-benchmark-cache) NO_CACHE=1; shift ;;
        --show-cache) SHOW_CACHE=1; shift ;;
        --clear-cache) CLEAR_CACHE=1; shift ;;
        --smoke-only) SMOKE_ONLY=1; shift ;;
        --assertion-only) ASSERTION_ONLY=1; shift ;;
        --) shift; TEST_FILES+=("$@"); break ;;
        -*) die "Unknown option: $1" ;;
        *) TEST_FILES+=("$1"); shift ;;
    esac
done

# Handle special commands
if [[ "$SHOW_CACHE" == "1" ]]; then
    show_cache
    exit 0
fi
if [[ "$CLEAR_CACHE" == "1" ]]; then
    clear_cache
    exit 0
fi

# ── Discover test files ─────────────────────────────────────────────────────
if [[ ${#TEST_FILES[@]} -eq 0 ]]; then
    # Default: run all .pl files in tests/ directory
    if [[ -d "tests" ]]; then
        mapfile -t TEST_FILES < <(find tests/ -name '*.pl' -type f | sort)
    else
        die "No test files specified and no tests/ directory found"
    fi
fi

# Expand directories to file lists
EXPANDED_FILES=()
for f in "${TEST_FILES[@]}"; do
    if [[ -d "$f" ]]; then
        mapfile -t dir_files < <(find "$f" -name '*.pl' -type f | sort)
        EXPANDED_FILES+=("${dir_files[@]}")
    else
        EXPANDED_FILES+=("$f")
    fi
done
TEST_FILES=("${EXPANDED_FILES[@]}")

if [[ ${#TEST_FILES[@]} -eq 0 ]]; then
    die "No .pl test files found"
fi

# ── Run tests ───────────────────────────────────────────────────────────────
if [[ "$PARALLEL" == "1" ]] && [[ "$JOBS" -gt 1 ]]; then
    # Parallel execution using xargs
    printf '%s\n' "${TEST_FILES[@]}" | xargs -P "$JOBS" -I{} bash -c "$(declare -f run_test log_verbose log_quiet die usage is_assertion_test is_benchmark is_placeholder get_hostname get_script_dir get_cache_file check_cache get_cached_result record_cache show_cache clear_cache); VERBOSE=$VERBOSE QUIET=$QUIET KEEP_TMP=$KEEP_TMP SKIP_COMPILE=$SKIP_COMPILE SKIP_INTERP=$SKIP_INTERP SMOKE_ONLY=$SMOKE_ONLY ASSERTION_ONLY=$ASSERTION_ONLY DRY_RUN=$DRY_RUN IGNORE_EXIT=$IGNORE_EXIT IGNORE_WS=$IGNORE_WS SORT_OUTPUT=$SORT_OUTPUT TIMEOUT=$TIMEOUT BENCHMARK_TIMEOUT=$BENCHMARK_TIMEOUT FORCE_BENCHMARK=$FORCE_BENCHMARK NO_CACHE=$NO_CACHE COMPILER=$COMPILER PERL=$PERL DIFF_TOOL=$DIFF_TOOL run_test '{}'"
else
    # Sequential execution
    for test_file in "${TEST_FILES[@]}"; do
        if [[ ! -f "$test_file" ]]; then
            log_quiet "${YELLOW}SKIP:${NC} $(basename "$test_file") (file not found)"
            SKIPPED=$((SKIPPED + 1))
            continue
        fi
        run_test "$test_file"
    done
fi

# ── Summary ─────────────────────────────────────────────────────────────────
echo ""
echo "========================================"
echo "  perlc Test Runner — Summary"
echo "========================================"
echo -e "  Total:       ${TOTAL}"
echo -e "  ${GREEN}Passed:      ${PASSED}${NC}"
echo -e "  ${RED}Failed:      ${FAILED}${NC}"
echo -e "  Skipped:     ${SKIPPED}"
echo ""
if [[ "$CACHE_HIT" -gt 0 ]] || [[ "$CACHE_RECORDED" -gt 0 ]]; then
    echo -e "  ${MAGENTA}Benchmark Cache:${NC}"
    echo -e "    Cache hits:              $CACHE_HIT"
    echo -e "    New results recorded:    $CACHE_RECORDED"
    echo ""
fi
if [[ "$FAILED" -gt 0 ]]; then
    echo -e "  ${RED}Failures:${NC}"
    echo "    Compiled fail (perl passed):  $COMPILED_FAIL"
    echo "    Runtime fail (binary passed): $RUNTIME_FAIL"
    echo "    Mismatch (both ran, differ):  $MISMATCH"
    echo "    Timeout:                      $TIMEOUT_COUNT"
fi
echo "========================================"

if [[ "$FAILED" -gt 0 ]]; then
    exit 1
fi
exit 0
