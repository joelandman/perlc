#!/usr/bin/env bash
# bench/regress.sh — Performance regression guard for perlc.
#
# bench.sh reports perlc-vs-perl ratios and never fails. This script is the
# GATE: it checks perlc's own *runtime* execution time against a committed
# baseline (bench/baseline.csv) and exits non-zero if perlc has gotten
# slower than the baseline beyond a tolerance on any benchmark.
#
# Why perlc-vs-perlc (not vs perl)? A perf change should only need to prove
# it didn't regress perlc itself; perl's own timing is hardware/noise
# dependent, so a ratio-vs-perl gate would be flaky across machines and would
# fail on a slower CI box for reasons unrelated to the change. This gate only
# answers "did this change make perlc slower?", which is exactly what a
# performance change needs to defend.
#
# Usage:
#   ./bench/regress.sh            # check current perlc against baseline (gated)
#   ./bench/regress.sh --fix      # re-record the baseline from current perlc
#
# Config via env (all optional):
#   BENCH_OPT        opt level to compile at          (default -O2; perlc's prod default)
#   BENCH_RUNS       per-benchmark runs, averaged     (default 3)
#   BENCH_TOLERANCE  max allowed regression, percent  (default 10)
#   BENCH_BASELINE   baseline file path               (default bench/baseline.csv)
#   BENCH_FIBN_N     fibn recursion depth             (default 28)
#   BENCH_NB_N       nb (N-body) iteration count      (default 300000)
#
# Benchmarks in the guard: fibn (recursion / call overhead), nb (numeric hot
# loop), regex_heavy (regex / PCRE2-bound). mbs is intentionally NOT gated:
# it is hardcoded to $N=4096 in bench/mbs.pl, far too slow for a guard run
# (the README exercises it at $N=1024); keep it under `make bench` only.
#
# The guard is a gate, not part of `make test-all` (which is the correctness
# diff gate). The two together are "not worse": run both when done with a
# perf change, e.g. `make test-all && make bench-regress`.

set -u

SCRIPT_DIR="$(cd "$(dirname "$BASH_SOURCE[0]")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PERLC="$PROJECT_DIR/perlc"
BASELINE="${BENCH_BASELINE:-$SCRIPT_DIR/baseline.csv}"

# ── config ────────────────────────────────────────────────────────────────
BENCH_OPT="${BENCH_OPT:--O2}"
BENCH_RUNS="${BENCH_RUNS:-3}"
BENCH_TOLERANCE="${BENCH_TOLERANCE:-10}"
BENCH_FIBN_N="${BENCH_FIBN_N:-28}"
BENCH_NB_N="${BENCH_NB_N:-300000}"

# ── arguments ─────────────────────────────────────────────────────────────
MODE="check"
if [[ "${1:-}" == "--fix" ]]; then MODE="fix"; shift; fi

TEST_FILES=()
if [[ $# -gt 0 ]]; then
    TEST_FILES+=("$@")
else
    # Guard set only. mbs.pl is excluded on purpose (see header): it hardcodes
    # $N=4096, far too slow for a guard run.
    mapfile -t TEST_FILES < <(find "$SCRIPT_DIR" -maxdepth 1 -name '*.pl' \
        \( -name 'fibn.pl' -o -name 'nb.pl' -o -name 'regex_heavy.pl' \) | sort)
fi
[[ ${#TEST_FILES[@]} -eq 0 ]] && { echo "no benchmarks found" >&2; exit 1; }

if [[ ! -x "$PERLC" ]]; then
    echo "perlc not built: run 'make' first" >&2
    exit 1
fi

# ── helpers ───────────────────────────────────────────────────────────────
TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

# args_for NAME -> extra argv for a benchmark (fixed, guard-only sizes)
args_for() {
    case "$1" in
        fibn) echo "$BENCH_FIBN_N" ;;
        nb)   echo "$BENCH_NB_N" ;;
        *)    echo "" ;;   # regex_heavy: no extra args
    esac
}

# compile_test FILE -> path of produced binary, or 'FAIL'
compile_test() {
    local file="$1" bn out
    bn=$(basename "$file" .pl)
    out="$TMP_DIR/${bn}"
    # Note perlc's arg order: ./perlc <script> -O<level> -o <out>
    "$PERLC" "$file" "$BENCH_OPT" -o "$out" >/dev/null 2>&1 || { echo FAIL; return 1; }
    echo "$out"
}

# measure CMD... -> sets ELAPSED_MS (ms)
measure() {
    local cmd_args=("$@") tmpfile
    tmpfile=$(mktemp)
    if command -v /usr/bin/time &>/dev/null; then
        /usr/bin/time -f "%e" "${cmd_args[@]}" >/dev/null 2>"$tmpfile" || true
        local ms; ms=$(cat "$tmpfile" 2>/dev/null || echo 0)
        if [[ "$ms" == *.* ]]; then
            local sec=${ms%%.*} frac=${ms#*.}
            while [[ ${#frac} -lt 3 ]]; do frac="${frac}0"; done
            frac="${frac:0:3}"
            ELAPSED_MS=$(( 10#${sec:-0} * 1000 + 10#${frac:-0} ))
        else
            ELAPSED_MS=$(( 10#${ms:-0} * 1000 ))
        fi
    else
        local start_ns end_ns
        start_ns=$(date +%s%N)
        timeout 3600 bash -c "${cmd_args[*]}" >/dev/null 2>&1 || true
        end_ns=$(date +%s%N)
        ELAPSED_MS=$(( (10#${end_ns:-0} - 10#${start_ns:-0}) / 1000000 ))
    fi
    rm -f "$tmpfile"
}

# get_baseline NAME -> recorded perlc_ms, or empty if absent
get_baseline() {
    grep "^$1," "$BASELINE" 2>/dev/null | head -1 | cut -d',' -f2
}

# record_fix -> overwrite the baseline from current perlc
record_fix() {
    echo "# perlc performance baseline (perlc-only run time, ms)";
    echo "# config: opt=$BENCH_OPT runs=$BENCH_RUNS fibn_N=$BENCH_FIBN_N nb_N=$BENCH_NB_N tolerance=${BENCH_TOLERANCE}%"
    echo "name,perlc_ms"
    for file in "${TEST_FILES[@]}"; do
        local bn; bn=$(basename "$file" .pl)
        local bin; bin=$(compile_test "$file")
        if [[ "$bin" == FAIL ]]; then echo "${bn},"; continue; fi
        local args; args=$(args_for "$bn")
        local total=0 ok=0
        for ((i=0; i<BENCH_RUNS; i++)); do
            if [[ -n "$args" ]]; then measure "$bin" $args; else measure "$bin"; fi
            [[ $ELAPSED_MS -ge 0 ]] && { total=$((total + ELAPSED_MS)); ok=$((ok + 1)); }
        done
        local avg=0; [[ $ok -gt 0 ]] && avg=$(( total / ok ))
        echo "${bn},${avg}"
    done > "$BASELINE"
    echo "baseline recorded to $BASELINE"
    sed 's/^/# /' "$BASELINE" | head -100
}

# ── modes ─────────────────────────────────────────────────────────────────
if [[ "$MODE" == "fix" ]]; then
    record_fix
    exit 0
fi

# check mode (default): gate current perlc against the baseline
if [[ ! -f "$BASELINE" ]]; then
    echo "no baseline at $BASELINE — record one with: ./bench/regress.sh --fix" >&2
    exit 1
fi

echo "perlc performance guard"
echo "======================="
echo "opt=$BENCH_OPT  runs=$BENCH_RUNS  tolerance=${BENCH_TOLERANCE}%  baseline=$BASELINE"
echo "name,perlc_ms,base_ms,vs_pct,status"

regressions=0 checked=0 skipped=0
for file in "${TEST_FILES[@]}"; do
    bn=$(basename "$file" .pl)
    bin=$(compile_test "$file")
    if [[ "$bin" == FAIL ]]; then echo "$bn,,,,SKIP (compile)"; skipped=$((skipped + 1)); continue; fi

    args=$(args_for "$bn")
    total=0; ok=0
    for ((i=0; i<BENCH_RUNS; i++)); do
        if [[ -n "$args" ]]; then measure "$bin" $args; else measure "$bin"; fi
        [[ $ELAPSED_MS -ge 0 ]] && { total=$((total + ELAPSED_MS)); ok=$((ok + 1)); }
    done
    now=0; [[ $ok -gt 0 ]] && now=$(( total / ok ))

    base=$(get_baseline "$bn")
    if [[ -z "$base" || "$base" == "0" || "$now" == "0" ]]; then
        echo "$bn,$now,0,SKIP (no baseline)"; skipped=$((skipped + 1)); continue
    fi

    # pct positive => faster than baseline, negative => slower
    pct=$(awk "BEGIN { printf \"%.1f\", ($base - $now) * 100.0 / $base }")
    below=$(awk "BEGIN { print (($base - $now) * 100.0 / $base) < -$BENCH_TOLERANCE ? 1 : 0 }")
    if [[ "$below" == "1" ]]; then
        status="REGRESSION"; regressions=$((regressions + 1))
    else
        status="ok"
    fi
    checked=$((checked + 1))
    echo "$bn,$now,$base,$pct,$status"
done

echo ""
if [[ $regressions -gt 0 ]]; then
    echo "RESULT: FAIL — $regressions benchmark(s) regressed >${BENCH_TOLERANCE}% vs baseline" >&2
    echo "Re-verify this is intended, then refresh with: ./bench/regress.sh --fix" >&2
    exit 1
fi
echo "RESULT: PASS — $checked benchmark(s) within ${BENCH_TOLERANCE}% of baseline ($skipped skipped)"
exit 0
