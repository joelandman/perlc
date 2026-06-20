#!/usr/bin/env bash
# bench.sh — Systematic benchmarking for perlc Perl→LLVM compiler
# Usage: bench/bench.sh [-n N] [--baseline] [--compare] [test.pl test2.pl ...]
#
# If no test files given, runs all tests from tests/*.pl
# If bench/*.pl files exist, runs those instead of tests/
#
# Flags:
#   -n N        Run each test N times, report average (default: 1)
#   --baseline  Record only perlc times to results.csv (for tracking)
#   --compare   Compare current run against last baseline
#
# Output: CSV to bench/results.csv with columns:
#   name,perlc_ms,perl_ms,ratio

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BENCH_DIR="$SCRIPT_DIR"
RESULTS="$BENCH_DIR/results.csv"
PERLC="$PROJECT_DIR/perlc"
TIMEOUT_SEC=30
NUM_RUNS=1
MODE="full"  # full, baseline, compare
EXTRA_ARGS=""

# ── argument parsing ──────────────────────────────────────────────────────
TEST_FILES=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -n)       NUM_RUNS="$2"; shift 2 ;;
        --baseline) MODE="baseline"; shift ;;
        --compare)  MODE="compare"; shift ;;
        --perlc-args) EXTRA_ARGS="$2"; shift 2 ;;
        --perl-args)  EXTRA_ARGS="$2"; shift 2 ;;
        -*)
            echo "Unknown flag: $1" >&2
            echo "Usage: $0 [-n N] [--baseline] [--compare] [test.pl ...]" >&2
            exit 1
            ;;
        *)  TEST_FILES+=("$1"); shift ;;
    esac
done

# ── discover test files ───────────────────────────────────────────────────
if [[ ${#TEST_FILES[@]} -eq 0 ]]; then
    # Prefer bench/*.pl if they exist, fall back to tests/*.pl
    bench_count=$(find "$BENCH_DIR" -maxdepth 1 -name '*.pl' 2>/dev/null | wc -l)
    if [[ $bench_count -gt 0 ]]; then
        mapfile -t TEST_FILES < <(find "$BENCH_DIR" -maxdepth 1 -name '*.pl' | sort)
    else
        mapfile -t TEST_FILES < <(find "$PROJECT_DIR/tests" -maxdepth 1 -name '*.pl' | sort)
    fi
fi

if [[ ${#TEST_FILES[@]} -eq 0 ]]; then
    echo "No test files found." >&2
    exit 1
fi

# ── helpers ───────────────────────────────────────────────────────────────
csv_escape() {
    local val="$1"
    if [[ "$val" == *","* || "$val" == *'"'* || "$val" == *$'\n'* ]]; then
        val="${val//\"/\"\"}"
        echo "\"$val\""
    else
        echo "$val"
    fi
}

run_with_time() {
    # Runs a command, returns elapsed ms via $ELAPSED_MS
    local cmd="$1"
    local output
    output=$(timeout "$TIMEOUT_SEC" bash -c "$cmd" 2>&1) || {
        local rc=$?
        if [[ $rc -eq 124 ]]; then
            echo "TIMEOUT" >&2
        fi
        ELAPSED_MS=0
        return 1
    }
    # Extract the last line if it's a number (some benchmarks print timing)
    # Otherwise we rely on the shell's time below
    echo "$output"
}

measure_time_ms() {
    # Measures elapsed time in milliseconds. Sets ELAPSED_MS.
    # Usage: measure_time_ms cmd_arg1 cmd_arg2 ...
    local cmd_args=("$@")
    local tmpfile
    tmpfile=$(mktemp)

    if command -v /usr/bin/time &>/dev/null; then
        # GNU time outputs to stderr: "real 0.123"
        /usr/bin/time -f "%e" "${cmd_args[@]}" >/dev/null 2>"$tmpfile" || true
        local ms
        ms=$(cat "$tmpfile" 2>/dev/null || echo "0")
        # /usr/bin/time -f "%e" gives seconds (e.g. "1.57" or "0.003")
        # Convert to integer ms
        if [[ "$ms" == *.* ]]; then
            local sec=${ms%%.*}
            local frac=${ms#*.}
            # Pad frac to 3 digits
            while [[ ${#frac} -lt 3 ]]; do frac="${frac}0"; done
            frac="${frac:0:3}"
            ELAPSED_MS=$(( 10#${sec:-0} * 1000 + 10#${frac:-0} ))
        else
            ELAPSED_MS=$(( 10#${ms:-0} * 1000 ))
        fi
    else
        # Fallback: use bash builtin with date
        local start_ns end_ns
        start_ns=$(date +%s%N)
        timeout "$TIMEOUT_SEC" bash -c "${cmd_args[*]}" >/dev/null 2>&1 || true
        end_ns=$(date +%s%N)
        ELAPSED_MS=$(( (10#${end_ns:-0} - 10#${start_ns:-0}) / 1000000 ))
    fi

    rm -f "$tmpfile"
}

# ── compile a test file ───────────────────────────────────────────────────
TMPDIR_BENCH=$(mktemp -d)
trap "rm -rf $TMPDIR_BENCH" EXIT

compile_test() {
    local testfile="$1"
    local basename
    basename=$(basename "$testfile" .pl)
    local outfile="$TMPDIR_BENCH/${basename}_perlc"

    # Compile - suppress all output (LLVM warnings go to stdout)
    if ! "$PERLC" "$testfile" -o "$outfile" >/dev/null 2>&1; then
        echo "FAIL"
        return 1
    fi
    echo "$outfile"
}

# ── run a single test ─────────────────────────────────────────────────────
run_test() {
    local testfile="$1"
    local basename
    basename=$(basename "$testfile" .pl)

    local perlc_bin perl_bin
    perlc_bin=$(compile_test "$testfile")
    if [[ "$perlc_bin" == "FAIL" || ! -x "$perlc_bin" ]]; then
        echo "${basename},,,,,FAIL (compile)"
        return 1
    fi

    # Determine arguments for this test
    local args="$EXTRA_ARGS"
    # Auto-detect benchmark sizes (use small values for quick runs)
    # fibn.pl: exponential, use N=20 (fib(20)=6765, fast)
    # nb.pl: O(N) iterations, use N=100000 (runs in ~7ms)
    case "$basename" in
        fibn)   [[ -z "$args" ]] && args="30" ;;
        nb)     [[ -z "$args" ]] && args="1000000" ;;
        mbs)    args="" ;;  # hardcoded in source
    esac

    # ── perlc timing ────────────────────────────────────────────────────
    local perlc_total=0
    local perlc_success=0
    for ((i=0; i<NUM_RUNS; i++)); do
            if [[ -n "$args" ]]; then
                measure_time_ms "$perlc_bin" $args
            else
                measure_time_ms "$perlc_bin"
            fi
        # Accept any measurement >= 0 (even 0ms for very fast programs)
        if [[ $ELAPSED_MS -ge 0 ]]; then
            perlc_total=$((perlc_total + ELAPSED_MS))
            perlc_success=$((perlc_success + 1))
        fi
    done

    local perlc_avg=0
    if [[ $perlc_success -gt 0 ]]; then
        perlc_avg=$((perlc_total / perlc_success))
    fi

    # ── perl timing (if not baseline mode) ──────────────────────────────
    local perl_avg=0
    local perl_success=0
    local perl_total=0

    if [[ "$MODE" != "baseline" ]]; then
        # Check if perl is available
        if ! command -v perl &>/dev/null; then
            echo "${basename},${perlc_avg},,,SKIP (no perl)"
            return 0
        fi

        for ((i=0; i<NUM_RUNS; i++)); do
            if [[ -n "$args" ]]; then
                measure_time_ms "perl" "$testfile" $args
            else
                measure_time_ms "perl" "$testfile"
            fi
            if [[ $ELAPSED_MS -ge 0 ]]; then
                perl_total=$((perl_total + ELAPSED_MS))
                perl_success=$((perl_success + 1))
            fi
        done

        if [[ $perl_success -gt 0 ]]; then
            perl_avg=$((perl_total / perl_success))
        fi
    fi

    # ── ratio ───────────────────────────────────────────────────────────
    local ratio="N/A"
    if [[ $perl_avg -gt 0 && $perlc_avg -gt 0 ]]; then
        # ratio = perl_ms / perlc_ms (how many times faster perlc is)
        ratio=$(awk "BEGIN { printf \"%.2f\", $perl_avg / $perlc_avg }")
    fi

    # ── output CSV line ─────────────────────────────────────────────────
    echo "${basename},${perlc_avg},${perl_avg},${ratio}"
}

# ── baseline / compare logic ──────────────────────────────────────────────
write_baseline() {
    # Write perlc-only results as baseline
    local tmpfile="$RESULTS.baseline_tmp"
    echo "name,perlc_ms,perl_ms,ratio" > "$tmpfile"
    for testfile in "${TEST_FILES[@]}"; do
        local basename
        basename=$(basename "$testfile" .pl)
        local perlc_bin
        perlc_bin=$(compile_test "$testfile")
        if [[ "$perlc_bin" == "FAIL" || ! -x "$perlc_bin" ]]; then
            echo "${basename},,," >> "$tmpfile"
            continue
        fi
        local args="$EXTRA_ARGS"
        case "$basename" in
            fibn)   [[ -z "$args" ]] && args="30" ;;
            nb)     [[ -z "$args" ]] && args="1000000" ;;
            mbs)    args="" ;;
        esac
       local total=0 success=0
        for ((i=0; i<NUM_RUNS; i++)); do
            if [[ -n "$args" ]]; then
                measure_time_ms "$perlc_bin" $args
            else
                measure_time_ms "$perlc_bin"
            fi
            if [[ $ELAPSED_MS -ge 0 ]]; then
                total=$((total + ELAPSED_MS))
                success=$((success + 1))
            fi
        done
        local avg=0
        [[ $success -gt 0 ]] && avg=$((total / success))
        echo "${basename},${avg}," >> "$tmpfile"
    done
    mv "$tmpfile" "$RESULTS"
    echo "Baseline written to $RESULTS"
}

compare_against_baseline() {
    if [[ ! -f "$RESULTS" ]]; then
        echo "No baseline found at $RESULTS. Run with --baseline first." >&2
        exit 1
    fi

    echo "name,perlc_ms,perl_ms,ratio,vs_baseline"
    # Read baseline into associative array
    declare -A baseline
    while IFS=',' read -r name pms _ _; do
        [[ "$name" == "name" ]] && continue
        [[ -z "$pms" || "$pms" == "," ]] && continue
        baseline["$name"]="$pms"
    done < "$RESULTS"

    for testfile in "${TEST_FILES[@]}"; do
        local basename
        basename=$(basename "$testfile" .pl)
        local line
        line=$(run_test "$testfile")
        local perlc_ms
        perlc_ms=$(echo "$line" | cut -d',' -f2)
        local vs="N/A"
        if [[ -n "${baseline[$basename]:-}" && "$perlc_ms" != "0" && "${baseline[$basename]}" != "0" ]]; then
            vs=$(awk "BEGIN { printf \"%.2f\", ${baseline[$basename]} / $perlc_ms }")
        fi
        echo "${line},${vs}"
    done
}

# ── main ──────────────────────────────────────────────────────────────────
echo "perlc benchmark suite"
echo "====================="
echo "Tests: ${#TEST_FILES[@]}"
echo "Runs per test: $NUM_RUNS"
echo "Timeout: ${TIMEOUT_SEC}s"
echo ""

if [[ "$MODE" == "baseline" ]]; then
    write_baseline
    exit 0
fi

if [[ "$MODE" == "compare" ]]; then
    compare_against_baseline
    exit 0
fi

# Full mode: header
echo "name,perlc_ms,perl_ms,ratio" > "$RESULTS"

for testfile in "${TEST_FILES[@]}"; do
    local_name=$(basename "$testfile" .pl)
    printf "%-20s " "$local_name..."
    line=$(run_test "$testfile")
    echo "$line" >> "$RESULTS"

    # Print summary
    perlc_ms=$(echo "$line" | cut -d',' -f2)
    perl_ms=$(echo "$line" | cut -d',' -f3)
    ratio=$(echo "$line" | cut -d',' -f4)

    if [[ -z "$perlc_ms" || "$perlc_ms" == "FAIL" ]]; then
        echo "  FAIL (no time)"
    else
        if [[ "$perl_ms" == "0" || "$perl_ms" == "" ]]; then
            echo "  perlc: ${perlc_ms}ms"
        else
            echo "  perlc: ${perlc_ms}ms  perl: ${perl_ms}ms  ratio: ${ratio}x"
        fi
    fi
done

echo ""
echo "Results written to $RESULTS"
echo ""
echo "Summary:"
echo "--------"
# Print CSV nicely
column -t -s',' "$RESULTS" 2>/dev/null || cat "$RESULTS"
