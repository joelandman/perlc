#!/usr/bin/env bash
# tests/realworld/run_compare.sh — byte-for-byte RUNTIME correctness survey.
#
# Companion to run.sh. run.sh is COMPILE-ONLY (it never executes the binaries
# because several of these are system tools). This runner takes the second
# half of the MVP definition: it executes BOTH perl and perlc against the same
# args+stdin and diffs the combined output byte-for-byte. That is what run.sh
# has never been able to measure, and it is exactly the number MVP is about:
# "real-world scripts compile AND RUN byte-for-byte correct vs. real Perl".
#
# For each manifest entry we classify:
#   skip-missing     path is not on this host (host-specific, excluded)
#   fail-compile     perlc cannot compile it (parse / capability gap)
#   fail-runtime     compiles, ran, output differs byte-for-byte vs perl
#   pass             compiles AND output is byte-identical to perl
#   skip-safe-run    compiles, but no curated safe invocation is defined
#                    (destructive / interactive / needs setup)
#
# Only pure-compute, non-destructive, deterministic tools are curated (see
# resolve_invoke). Anything destructive, interactive, networked, or needing
# special setup is left uncurated -> classified skip-safe-run and tracked
# separately, so the runner never runs a system tool to completion.
#
# Gate: fails if any gate=1 ("supported") script is not PASS, or if any script
# regressed from PASS in baseline-compare.txt. Divergences on gate=0 scripts
# are reported and baseline-tracked but not hard-gated (they are known module
# capacity gaps being measured, not regressions).
#
# Writes:
#   last-compare-report.txt  full per-script results + pass-rates (always)
#   baseline-compare.txt     snapshot for regression detection, created on the
#                            first run; set COMPARE_BASELINE_UPDATE=1 to refresh
#
# Exit code: 0 if the gate holds, 1 otherwise. Reports are always produced.

set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
PERLC_BIN="${PERLC:-$PROJECT_DIR/perlc}"
PERL_CMD="${PERL_CMD:-perl}"
MANIFEST="${REALWORLD_MANIFEST:-$SCRIPT_DIR/manifest.txt}"
REPORT="$SCRIPT_DIR/last-compare-report.txt"
BASELINE="$SCRIPT_DIR/baseline-compare.txt"
TIMEOUT_SEC="${REALWORLD_COMPARE_TIMEOUT:-30}"

if [[ ! -x "$PERLC_BIN" ]]; then
    echo "perlc not found or not executable: $PERLC_BIN" >&2
    echo "build it first: make" >&2
    exit 1
fi
if [[ ! -f "$MANIFEST" ]]; then
    echo "manifest not found: $MANIFEST" >&2
    exit 1
fi

tmpdir="$(mktemp -d /tmp/perlc-rwcmp.XXXXXX)"
trap 'rm -rf "$tmpdir"' EXIT
mkdir -p "$tmpdir/bin"

# Canonical, safe, deterministic invocation for the scripts we curate.
# Sets _GATE (1|required/gated, 0|tracked), _ARGS (split on whitespace),
# _STDIN (the exact bytes fed to stdin). Only pure-compute / non-destructive
# tools go here; the rest are tracked as skip-safe-run.
resolve_invoke() {
    local base="$1"
    _GATE=0; _ARGS=""; _STDIN=""
    case "$base" in
        json_pp)
            _GATE=1; _ARGS=""; _STDIN='{"z":1,"a":[3,2,1],"s":"x"}' ;;
        shasum)
            _GATE=1; _ARGS=""; _STDIN='hello world' ;;
        debconf-escape)
            _GATE=1; _ARGS='-e'; _STDIN='a&b=c' ;;
        pod2text)
            _GATE=0; _ARGS=""; _STDIN=$'=head1 Title\n\nBody text here.' ;;
        pod2man)
            _GATE=0; _ARGS='--name=Foo'; _STDIN=$'=head1 T\n\nB.' ;;
        pod2html)
            _GATE=0; _ARGS=""; _STDIN=$'=head1 T\n\nB.' ;;
        piconv)
            _GATE=0; _ARGS='-f ascii -t ascii'; _STDIN='abc' ;;
    esac
}

n_pass=0; n_fail_compile=0; n_fail_runtime=0
n_fail_runtime_all=0; n_gate_fail=0
n_skip_missing=0; n_skip_safe=0; n_total=0
rows=()
declare -A STATUS   # basename -> today's status (for baseline + regression)

# Run a command with fixed stdin/args, return combined stdout+stderr in $1.
# Usage: run_capture <outfile> <stdin> <args> <cmd...>
# $args is intentionally UNquoted: it word-splits --name=Foo and
# "-f ascii -t ascii" into argv exactly like the perl invocation does.
run_capture() {
    local out="$1" stdin="$2" args="$3"; shift 3
    if [[ -z "$stdin" ]]; then
        timeout "$TIMEOUT_SEC" "$@" $args >"$out" 2>&1
    else
        printf '%s' "$stdin" | timeout "$TIMEOUT_SEC" "$@" $args >"$out" 2>&1
    fi
}

while IFS= read -r line || [[ -n "$line" ]]; do
    path="${line%%#*}"
    path="${path#"${path%%[![:space:]]*}"}"
    path="${path%"${path##*[![:space:]]}"}"
    [[ -z "$path" ]] && continue
    n_total=$((n_total + 1))
    base="$(basename "$path")"

    if [[ ! -f "$path" ]]; then
        rows+=("skip-missing	$path	(missing on host)")
        n_skip_missing=$((n_skip_missing + 1))
        continue
    fi

    resolve_invoke "$base"

    outfile="$tmpdir/bin/${base}.bin"
    if ! timeout "$TIMEOUT_SEC" "$PERLC_BIN" "$path" -o "$outfile" \
            >"$tmpdir/${base}.cout" 2>"$tmpdir/${base}.cerr" </dev/null; then
        rm -f "$outfile"
        rows+=("fail-compile	$path")
        n_fail_compile=$((n_fail_compile + 1))
        if [[ "$_GATE" -eq 1 ]]; then
            n_gate_fail=$((n_gate_fail + 1))
        fi
        STATUS["$base"]="fail-compile"
        continue
    fi

    if [[ -z "$_ARGS" && -z "$_STDIN" && "$_GATE" -eq 0 ]]; then
        # No curated invocation for this (compiles, but we don't run it).
        rm -f "$outfile"
        rows+=("skip-safe-run	$path	(compiles; no curated safe invocation)")
        n_skip_safe=$((n_skip_safe + 1))
        STATUS["$base"]="skip-safe-run"
        continue
    fi

    p_out="$tmpdir/${base}.perl"; c_out="$tmpdir/${base}.perlc"
    # perl must run the *script* ($path) with the args, not parse the
    # args/stdin as a program. Args are word-split intentionally ($args),
    # which is what we want for --name=Foo etc.
    run_capture "$p_out" "$_STDIN" "$_ARGS" "$PERL_CMD" "$path"
    run_capture "$c_out" "$_STDIN" "$_ARGS" "$outfile"

    if diff -q "$p_out" "$c_out" >/dev/null 2>&1; then
        rows+=("pass	$path")
        n_pass=$((n_pass + 1))
        STATUS["$base"]="pass"
    else
        perl_head="$(sed -n '1,3p' "$p_out" | tr '\n' '|')"
        perlc_head="$(sed -n '1,3p' "$c_out" | tr '\n' '|')"
        rows+=("fail-runtime	$path	perl:['${perl_head}'] perlc:['${perlc_head}']")
        # Metric: count every runtime divergence (gate=0 included) so the
        # runtime-correct rate is real. Gate: only gate=1 ("supported")
        # divergences trip it; gate=0 divergences are capacity gaps being
        # measured, not regressions.
        n_fail_runtime_all=$((n_fail_runtime_all + 1))
        if [[ "$_GATE" -eq 1 ]]; then
            n_fail_runtime=$((n_fail_runtime + 1))
            n_gate_fail=$((n_gate_fail + 1))
        fi
        STATUS["$base"]="fail-runtime"
    fi
    rm -f "$outfile" "$tmpdir/${base}.cout" "$tmpdir/${base}.cerr"
done < "$MANIFEST"

# ---- metrics ----------------------------------------------------------------
# compilable / runnable count EVERY diverging script (gate=0 included) so the
# pass-rates are real. The gate (below) is separate and only trips on gate=1.
compilable=$(( n_pass + n_fail_runtime_all + n_skip_safe ))
non_missing=$(( n_total - n_skip_missing ))
if [[ $non_missing -gt 0 ]]; then
    compile_rate="$(awk "BEGIN{printf \"%.1f\", 100*$compilable/$non_missing}")"
    mvp_rate="$(awk "BEGIN{printf \"%.1f\", 100*$n_pass/$non_missing}")"
else
    compile_rate="0.0"; mvp_rate="0.0"
fi
runnable=$(( n_pass + n_fail_runtime_all ))
if [[ $runnable -gt 0 ]]; then
    runtime_rate="$(awk "BEGIN{printf \"%.1f\", 100*$n_pass/$runnable}")"
else
    runtime_rate="0.0"
fi

# ---- regression gate --------------------------------------------------------
# Gate fails if a script that was PASS in the baseline is no longer PASS.
regression=0
if [[ -f "$BASELINE" ]]; then
    while IFS=$'\t' read -r b prev; do
        [[ -z "$b" ]] && continue
        if [[ "$prev" == "pass" && "${STATUS[$b]:-}" != "pass" ]]; then
            echo "REGRESSION: $b was PASS in baseline, now ${STATUS[$b]:-missing}" >&2
            regression=1
        fi
    done < "$BASELINE"
fi

gate_fail=0
# Gate trips on any gate=1 ("supported") script that is not PASS (compile OR
# runtime), or any regression from a prior baseline PASS.
[[ $n_gate_fail -gt 0 ]] && gate_fail=1
[[ $regression -eq 1 ]] && gate_fail=1

# ---- write report -----------------------------------------------------------
{
    echo "# perlc real-world RUNTIME correctness corpus (byte-for-byte vs perl)"
    echo "# host: $(uname -srm)  date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "# perlc: $PERLC_BIN  perl: $PERL_CMD  timeout: ${TIMEOUT_SEC}s"
    echo "# total=$n_total pass=$n_pass fail-compile=$n_fail_compile fail-runtime=$n_fail_runtime_all skip-safe=$n_skip_safe skip-missing=$n_skip_missing"
    echo "# compile-rate=${compile_rate}%  runtime-correct(among runnable)=${runtime_rate}%  mvp-pass-rate=${mvp_rate}%"
    echo
    printf '%s\n' "${rows[@]}"
} > "$REPORT"

# ---- baseline ---------------------------------------------------------------
# The baseline is the regression anchor: it must PERSIST between runs so a
# genuine PASS -> non-PASS is caught. Only rewrite it when asked to (first run
# or COMPARE_BASELINE_UPDATE=1). Do NOT overwrite it on every run, or the gate
# could never detect a regression (it would just track the latest run).
if [[ ! -f "$BASELINE" ]] || [[ "${COMPARE_BASELINE_UPDATE:-0}" == "1" ]]; then
    : > "$BASELINE"
    for base in "${!STATUS[@]}"; do
        printf '%s\t%s\n' "$base" "${STATUS[$base]}"
    done | sort >> "$BASELINE"
    echo "(baseline written/updated; ${#STATUS[@]} entries)" >&2
else
    echo "(baseline left unchanged; set COMPARE_BASELINE_UPDATE=1 to refresh)" >&2
fi

echo
echo "=== real-world RUNTIME correctness ==="
echo "  total=$n_total  pass=$n_pass  fail-compile=$n_fail_compile  fail-runtime=$n_fail_runtime_all  skip-safe=$n_skip_safe  skip-missing=$n_skip_missing"
echo "  gate=1 runtime failures (trip the gate): $n_fail_runtime   gate: $([ $gate_fail -eq 0 ] && echo HOLD || echo FAIL)"
echo "  compile-rate(${non_missing} non-missing)=${compile_rate}%  runtime-correct(${runnable} runnable)=${runtime_rate}%  mvp-pass-rate(${non_missing})=${mvp_rate}%"
echo "  report: $REPORT"
echo "  baseline: $BASELINE"

exit $gate_fail
