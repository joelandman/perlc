#!/usr/bin/env bash
# tests/harness.sh — Correctness harness: compare perlc output vs real perl
#
# Usage:
#   ./tests/harness.sh                       # every tests/*.pl, compare vs perl
#   ./tests/harness.sh tests/nb.pl           # one or more named tests
#   ./tests/harness.sh --smoke-only          # only *_smoke.pl
#   ./tests/harness.sh --assertion-only      # the four contract programs
#   ./tests/harness.sh --help
#   PERLC=../perlc ./tests/harness.sh        # use an existing binary
#   OPT_LEVEL=2 ./tests/harness.sh           # pass -O2 to perlc
#   PERLC_FLAGS='-O0 -g' ./tests/harness.sh  # raw extra perlc arguments
#   TOLERANCE=1e-9 ./tests/harness.sh        # FP tolerance for numeric tests
#   TIMEOUT_SEC=60 ./tests/harness.sh        # per-test perl and binary timeout
#   RUN_IN_PARALLEL=0 ./tests/harness.sh     # one test at a time
#   CORE_DIVISOR=2 ./tests/harness.sh        # worker count is nproc/CORE_DIVISOR
#
# Exit status: 0 if every compared test matches, nonzero otherwise.
# Prints PASS/FAIL for each, then a summary.
#
# Files named in SKIP_BY_DEFAULT run only when you pass that path yourself
# (or via --assertion-only, which names the DBI and XS programs).

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
    [pidigits.pl]="27"
    [tree.pl]=""          # tree.pl may accept optional args; start empty
)

# Numeric-heavy tests where we allow FP tolerance instead of exact match.
NUMERIC_TESTS=(
    nb.pl nbody.pl mbs.pl fibn.pl arith.pl
)

# Heavy benchmarks that need more than the default 60s timeout for real Perl.
# Key: test filename, Value: timeout in seconds.
declare -A LONG_TIMEOUT=(
    [mbs.pl]=300
)

# Tests that are known to need external modules / DBI / XS at runtime.
# We still try to compile+run them if the binary supports it, but we don't fail
# the whole suite if they are skipped or produce different "not loaded" output.
SKIP_BY_DEFAULT=(
    dbi_sqlite.pl xs_dbi_test.pl xs_ffi.pl
    cpanel_json_xs_smoke.pl cpanel_json_xs_deep.pl
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

    # Determine timeout — heavy benchmarks get a longer window
    local test_timeout="$TIMEOUT_SEC"
    if [[ -n "${LONG_TIMEOUT[$base]:-}" ]]; then
        test_timeout="${LONG_TIMEOUT[$base]}"
    fi

    # Build temp outputs
    local p_out c_out c_bin
    p_out=$(mktemp)
    c_out=$(mktemp)
    c_bin=$(mktemp)

    # Run under perl
    if ! timeout "$test_timeout" "$PERL_CMD" "$testfile" $args >"$p_out" 2>&1; then
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

    if ! timeout "$test_timeout" "$c_bin" $args >"$c_out" 2>&1; then
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

CORE_DIVISOR="${CORE_DIVISOR:-2}"   # tests run 1-per-2-cores by default
RUN_IN_PARALLEL="${RUN_IN_PARALLEL:-1}"   # set to 0 to force serial execution

main() {
    # $1 is 1 when the user named files (or --assertion-only did).
    # A default or --smoke-only discovery run passes 0, and then
    # SKIP_BY_DEFAULT entries are reported and not compared.
    local user_explicit="$1"
    shift
    local tests=("$@")

    local total=0 pass=0 fail=0

    local t base s skip rfile verdict nworkers running

    ncpu=$(nproc)
    nworkers=$(( ncpu / CORE_DIVISOR ))
    [[ $RUN_IN_PARALLEL -eq 1 ]] || nworkers=1
    if [[ $RUN_IN_PARALLEL -eq 1 ]]; then
        [[ $nworkers -lt 1 ]] && nworkers=1
    fi

    # Each test writes its PASS/FAIL line (+ diagnostics) to its own result
    # file; we aggregate them in sorted order below so the summary is stable
    # regardless of completion order. res_dir is global so the EXIT trap can
    # clean it up after main() returns.
    res_dir=$(mktemp -d "${TMPDIR:-/tmp}/perlc_harness.XXXXXX")
    trap 'rm -rf "${res_dir:-}"' EXIT

    # 1) Print skip notices up-front (original serial ordering).
    for t in "${tests[@]}"; do
        base=$(basename "$t")
        if [[ "$user_explicit" -eq 0 ]]; then
            for s in "${SKIP_BY_DEFAULT[@]}"; do
                if [[ "$base" == "$s" ]]; then
                    printf 'SKIP %s (skipped by default — run explicitly if desired)\n' "$base"
                fi
            done
        fi
    done

    # 2) Execute (parallel pool of nworkers, or serial when disabled),
    # buffering every verdict so step 3 can aggregate deterministically.
    running=0
    for t in "${tests[@]}"; do
        base=$(basename "$t")

        # Skip heavy/external by default unless explicitly listed
        skip=0
        if [[ "$user_explicit" -eq 0 ]]; then
            for s in "${SKIP_BY_DEFAULT[@]}"; do
                if [[ "$base" == "$s" ]]; then skip=1; fi
            done
        fi
        [[ $skip -eq 1 ]] && continue

        if [[ $RUN_IN_PARALLEL -eq 1 && $nworkers -ge 1 ]]; then
            if (( running >= nworkers )); then wait -n; running=$((running-1)); fi
            # run_one prints its PASS/FAIL (+diag) to its result file
            ( run_one "$t" >"$res_dir/$base" 2>&1 ) &
            running=$((running+1))
        else
            run_one "$t" >"$res_dir/$base" 2>&1
        fi
    done
    [[ $RUN_IN_PARALLEL -eq 1 ]] && wait

    # 3) Aggregate results in deterministic (sorted) order.
    for t in "${tests[@]}"; do
        base=$(basename "$t")

        skip=0
        if [[ "$user_explicit" -eq 0 ]]; then
            for s in "${SKIP_BY_DEFAULT[@]}"; do
                if [[ "$base" == "$s" ]]; then skip=1; fi
            done
        fi
        [[ $skip -eq 1 ]] && continue

        rfile="$res_dir/$base"
        if [[ ! -f "$rfile" ]]; then
            printf 'FAIL %s (no result produced)\n' "$base"
            fail=$((fail+1))
            continue
        fi
        verdict=$(head -n1 "$rfile")
        case "$verdict" in
            PASS*)     cat "$rfile";     total=$((total+1)); pass=$((pass+1));;
            FAIL*)     cat "$rfile";     total=$((total+1)); fail=$((fail+1));;
            SKIP*)     printf '%s\n' "$verdict";;
            *)         cat "$rfile";     total=$((total+1)); fail=$((fail+1));;
        esac
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

usage() {
    cat <<'EOF'
Usage: tests/harness.sh [--smoke-only | --assertion-only] [test.pl ...]

  (no arguments)     Compare every tests/*.pl with real perl.
                     Skips dbi_sqlite.pl, xs_ffi.pl, and
                     cpanel_json_xs_{smoke,deep}.pl unless you name them.
  test.pl ...        Compare only those files. A skipped-by-default
                     file runs when you name it.
  --smoke-only       Keep only *_smoke.pl from the selection.
  --assertion-only   Compare these four contract programs:
                       tests/test_do_filename.pl
                       tests/test_require_simple.pl
                       tests/dbi_sqlite.pl
                       tests/xs_ffi.pl
                     dbi_sqlite.pl needs SQLite. xs_ffi.pl needs the
                     fixture under tests/lib/auto/.
  --help             Show this text.

Environment:
  PERLC            Compiler binary (default: ./perlc next to tests/)
  PERL             Reference interpreter (default: perl)
  OPT_LEVEL        Digit 0-5. Adds -ON unless PERLC_FLAGS already has -O.
  PERLC_FLAGS      Extra arguments passed to perlc before the script.
  TIMEOUT_SEC      Seconds for one perl run and one compiled binary (default 60).
                   mbs.pl uses 300.
  TOLERANCE        Relative float tolerance for nb, nbody, mbs, fibn, arith
                   (default 1e-9).
  RUN_IN_PARALLEL  1 (default) runs a pool; 0 runs one test at a time.
  CORE_DIVISOR     Pool size is nproc divided by this (default 2).

Exit 0 when every compared test matches. A missing selection exits 1.
EOF
}

user_explicit=0
smoke_only=0
assertion_only=0
user_tests=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --smoke-only) smoke_only=1; shift ;;
        --assertion-only) assertion_only=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; user_tests+=("$@"); break ;;
        -*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *) user_tests+=("$1"); shift ;;
    esac
done

if [[ "$assertion_only" -eq 1 && "$smoke_only" -eq 1 ]]; then
    echo "--smoke-only and --assertion-only select different sets." >&2
    exit 2
fi

if [[ ${#user_tests[@]} -gt 0 ]]; then
    user_explicit=1
    selected=("${user_tests[@]}")
elif [[ "$assertion_only" -eq 1 ]]; then
    # Naming these files is what lets the default-skip list run them.
    user_explicit=1
    selected=(
        "$SCRIPT_DIR/test_do_filename.pl"
        "$SCRIPT_DIR/test_require_simple.pl"
        "$SCRIPT_DIR/dbi_sqlite.pl"
        "$SCRIPT_DIR/xs_ffi.pl"
    )
else
    user_explicit=0
    mapfile -t selected < <(find "$SCRIPT_DIR" -maxdepth 1 -name '*.pl' | sort)
fi

if [[ "$smoke_only" -eq 1 ]]; then
    filtered=()
    for t in "${selected[@]}"; do
        base=$(basename "$t")
        if [[ "$base" == *_smoke.pl ]]; then
            filtered+=("$t")
        fi
    done
    selected=("${filtered[@]}")
fi

if [[ ${#selected[@]} -eq 0 ]]; then
    echo "No tests selected." >&2
    exit 1
fi

main "$user_explicit" "${selected[@]}"
