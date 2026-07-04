#!/usr/bin/env bash
# tests/harness.sh — Correctness harness: compare perlc output vs real perl
#
# Usage:
#   ./tests/harness.sh                  # run all runnable tests, compare vs perl
#   ./tests/harness.sh tests/nb.pl      # specific test(s)
#   PERLC=../perlc ./tests/harness.sh   # use existing binary
#   TOLERANCE=1e-9 ./tests/harness.sh   # FP tolerance for numeric tests
#
# Exit status: 0 if all compared tests match, nonzero otherwise.
# Prints PASS/FAIL for each, and a final summary.

set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PERLC_BIN="${PERLC:-$PROJECT_DIR/perlc}"
PERL_CMD="${PERL:-perl}"
TIMEOUT_SEC="${TIMEOUT_SEC:-60}"
TOLERANCE="${TOLERANCE:-1e-9}"
PERLC_FLAGS="${PERLC_FLAGS:-}"   # e.g. "-O0" or "-O2 -g" to drive different opt levels
# If OPT_LEVEL is set (single digit), auto-append -O${OPT_LEVEL} unless already in PERLC_FLAGS
if [[ -n "${OPT_LEVEL:-}" ]]; then
    if [[ ! "$PERLC_FLAGS" =~ -O ]]; then
        PERLC_FLAGS="$PERLC_FLAGS -O${OPT_LEVEL}"
    fi
fi
PERLC_FLAGS=$(echo "$PERLC_FLAGS" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')  # trim

# Tests that require an argument (N or similar). Value is the default arg.
declare -A ARGV_DEFAULTS=(
    [nb.pl]="1"
    [nbody.pl]="1"
    [fibn.pl]="10"
    [tree.pl]=""          # tree.pl may accept optional args; start empty
)

# Numeric-heavy tests where we allow FP tolerance instead of exact match.
NUMERIC_TESTS=(
    nb.pl nbody.pl mbs.pl fibn.pl arith.pl
)

# Tests that are known to need external modules / DBI / XS at runtime.
# We still try to compile+run them if the binary supports it, but we don't fail
# the whole suite if they are skipped or produce different "not loaded" output.
SKIP_BY_DEFAULT=(
    dbi_sqlite.pl xs_dbi_test.pl xs_ffi.pl
)

# Self-checking tests: they already print "xxx=ok/FAIL" or die.
# We still diff their output for completeness.
SELF_CHECKING=(
    test_do_filename.pl test_require_simple.pl dbi_sqlite.pl xs_ffi.pl
    threads.pl threads_atomic.pl destroy.pl regression_bugs.pl
    tier3.pl completeness.pl eval_string.pl
)

log() { printf "%s\n" "$*" >&2; }

normalize() {
    # Trim trailing whitespace on each line, collapse multiple blank lines to one,
    # remove a trailing blank line at EOF. Makes most string diffs stable.
    sed -E '
        s/[[:space:]]+$//;
        $ { /^$/d; }
    ' | awk '
        BEGIN { last_blank=0 }
        /^$/ { if (!last_blank) print; last_blank=1; next }
        { print; last_blank=0 }
    '
}

# Compare two files.
# For numeric tests, use a tolerance-aware compare on lines that look like floats.
compare_outputs() {
    local a="$1" b="$2" testbase="$3"
    local is_numeric=0
    for nt in "${NUMERIC_TESTS[@]}"; do
        if [[ "$testbase" == "$nt" ]]; then is_numeric=1; break; fi
    done

    if [[ $is_numeric -eq 1 ]]; then
        # Use python for tolerant numeric diff when available.
        if command -v python3 >/dev/null 2>&1; then
            python3 - "$a" "$b" "$TOLERANCE" <<'PY'
import sys, re
fa, fb, tol = sys.argv[1], sys.argv[2], float(sys.argv[3])
with open(fa) as f: la = f.readlines()
with open(fb) as f: lb = f.readlines()
if len(la) != len(lb):
    sys.exit(1)
num_re = re.compile(r'[-+]?[0-9]*\.?[0-9]+([eE][-+]?[0-9]+)?')
for i, (xa, xb) in enumerate(zip(la, lb)):
    if xa == xb: continue
    # split on whitespace, compare tokens tolerantly
    ta = xa.split()
    tb = xb.split()
    if len(ta) != len(tb): sys.exit(1)
    for va, vb in zip(ta, tb):
        if va == vb: continue
        ma, mb = num_re.fullmatch(va), num_re.fullmatch(vb)
        if ma and mb:
            try:
                da, db = float(va), float(vb)
                if abs(da - db) > tol * max(1.0, abs(da), abs(db)):
                    sys.exit(1)
            except:
                sys.exit(1)
        else:
            sys.exit(1)
sys.exit(0)
PY
            return $?
        fi
        # Fallback: exact after normalize
    fi

    # Default: exact normalized text diff
    diff -u <(normalize < "$a") <(normalize < "$b") >/dev/null
}

run_one() {
    local testfile="$1"
    local base
    base=$(basename "$testfile")

    if [[ ! -f "$testfile" ]]; then
        log "SKIP $base (not found)"
        return 0
    fi

    # Determine args
    local args=""
    if [[ -n "${ARGV_DEFAULTS[$base]:-}" ]]; then
        args="${ARGV_DEFAULTS[$base]}"
    fi

    # Build temp outputs
    local p_out c_out c_bin
    p_out=$(mktemp)
    c_out=$(mktemp)
    c_bin=$(mktemp)

    # Run under perl
    if ! timeout "$TIMEOUT_SEC" "$PERL_CMD" "$testfile" $args >"$p_out" 2>&1; then
        # Some tests are expected to die (eval tests etc.). Capture anyway.
        :
    fi

    # Compile with perlc (if binary exists)
    if [[ ! -x "$PERLC_BIN" ]]; then
        log "FAIL $base (no perlc binary at $PERLC_BIN)"
        rm -f "$p_out" "$c_out" "$c_bin"
        return 1
    fi

    local compile_log
    compile_log=$(mktemp)
    # Pass PERLC_FLAGS (e.g. -O0 -O2 etc.) so we can drive opt-level matrix
    # shellcheck disable=SC2086
    local effective_flags="$PERLC_FLAGS"
    if ! "$PERLC_BIN" $effective_flags "$testfile" -o "$c_bin" >"$compile_log" 2>&1; then
        echo "=== perlc compile failed for $base (flags: ${effective_flags:-<none>}) ===" >&2
        cat "$compile_log" >&2
        echo "=== perl output was ===" >&2
        cat "$p_out" >&2
        rm -f "$p_out" "$c_out" "$c_bin" "$compile_log"
        return 1
    fi

    if [[ ! -x "$c_bin" ]]; then
        log "FAIL $base (perlc did not produce executable)"
        rm -f "$p_out" "$c_out" "$c_bin" "$compile_log"
        return 1
    fi

    if ! timeout "$TIMEOUT_SEC" "$c_bin" $args >"$c_out" 2>&1; then
        :
    fi

    if compare_outputs "$p_out" "$c_out" "$base"; then
        printf "PASS %s\n" "$base"
        rc=0
    else
        printf "FAIL %s (output differs from perl)\n" "$base"
        echo "=== diff (perl vs perlc) ==="
        diff -u <(normalize < "$p_out") <(normalize < "$c_out") || true
        rc=1
    fi

    rm -f "$p_out" "$c_out" "$c_bin" "$compile_log"
    return $rc
}

main() {
    local tests=("$@")
    if [[ ${#tests[@]} -eq 0 ]]; then
        # Default: all .pl under tests/, sorted
        mapfile -t tests < <(find "$SCRIPT_DIR" -maxdepth 1 -name '*.pl' | sort)
    fi

    local total=0 pass=0 fail=0

    for t in "${tests[@]}"; do
        local base
        base=$(basename "$t")

        # Skip some heavy/external by default unless explicitly listed
        local skip=0
        for s in "${SKIP_BY_DEFAULT[@]}"; do
            if [[ "$base" == "$s" ]]; then
                # Only skip if not explicitly passed on cmdline
                if [[ ${#tests[@]} -eq 0 || "$*" != *"$base"* ]]; then
                    skip=1
                fi
            fi
        done
        if [[ $skip -eq 1 ]]; then
            printf "SKIP %s (external/DBI/threads — run explicitly if desired)\n" "$base"
            continue
        fi

        total=$((total+1))
        if run_one "$t"; then
            pass=$((pass+1))
        else
            fail=$((fail+1))
        fi
    done

    echo ""
    echo "=== Summary ==="
    echo "Total compared: $total"
    echo "PASS: $pass"
    echo "FAIL: $fail"

    if [[ $fail -gt 0 ]]; then
        echo "Some tests differed from real perl output."
        return 1
    fi
    echo "All compared tests match perl output (within tolerance where applicable)."
    return 0
}

main "$@"
