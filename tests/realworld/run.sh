#!/usr/bin/env bash
# tests/realworld/run.sh — compile-only survey of real system scripts.
#
# Reads manifest.txt (absolute paths). For each path, compiles with perlc
# and classifies the result. Never executes the produced binary: several
# of these scripts are system tools (update-rc.d, deb-systemd-helper, …).
#
# Classes:
#   ok                  perlc exited 0
#   missing-file        path is not on this host
#   missing-module      stderr contains "Can't locate"
#   parse-error         stderr contains "Parse error"
#   other-compile-error anything else (timeout, LLVM verify, link failure)
#
# The LLVM "overriding the module target triple" note is stripped before
# classification. The script always exits 0. It is a report, not a gate,
# and it is not part of make test-all.
#
# Writes tests/realworld/last-report.txt. If baseline.txt does not exist
# yet, copies the report there. Set REALWORLD_UPDATE_BASELINE=1 to replace
# an existing baseline. The baseline is host-specific.

set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
PERLC_BIN="${PERLC:-$PROJECT_DIR/perlc}"
MANIFEST="${REALWORLD_MANIFEST:-$SCRIPT_DIR/manifest.txt}"
TIMEOUT_SEC="${REALWORLD_TIMEOUT:-60}"
REPORT="$SCRIPT_DIR/last-report.txt"

if [[ ! -x "$PERLC_BIN" ]]; then
    echo "perlc not found or not executable: $PERLC_BIN" >&2
    echo "build it first: make" >&2
    exit 1
fi
if [[ ! -f "$MANIFEST" ]]; then
    echo "manifest not found: $MANIFEST" >&2
    exit 1
fi

tmpdir="$(mktemp -d /tmp/perlc-rw.XXXXXX)"
trap 'rm -rf "$tmpdir"' EXIT

n_ok=0
n_missing_file=0
n_missing_module=0
n_parse=0
n_other=0
n_total=0
rows=()

classify_one() {
    local path="$1"
    local base errfile outfile err filtered first
    base="$(basename "$path")"
    errfile="$tmpdir/${base}.err"
    outfile="$tmpdir/${base}.bin"

    if [[ ! -f "$path" ]]; then
        echo "missing-file	$path"
        return
    fi

    # Compile only. A timeout is an other-compile-error, not a crash of
    # this runner. Discard stdout; diagnostics are on stderr.
    if timeout "$TIMEOUT_SEC" "$PERLC_BIN" "$path" -o "$outfile" \
            >"$tmpdir/${base}.out" 2>"$errfile"; then
        rm -f "$outfile"
        echo "ok	$path"
        return
    fi
    rm -f "$outfile"

    filtered="$(grep -v -e 'overriding the module target triple' \
                        -e 'warning generated' \
                        "$errfile" 2>/dev/null || true)"
    # First meaningful diagnostic line, collapsed to one line.
    first="$(printf '%s\n' "$filtered" | sed '/^[[:space:]]*$/d' | head -n 1 | tr '\t' ' ')"
    if printf '%s\n' "$filtered" | grep -q "Can't locate"; then
        echo "missing-module	$path	${first}"
    elif printf '%s\n' "$filtered" | grep -q "Parse error"; then
        echo "parse-error	$path	${first}"
    else
        if [[ -z "$first" ]]; then
            first="(no diagnostic; exit or timeout)"
        fi
        echo "other-compile-error	$path	${first}"
    fi
}

while IFS= read -r line || [[ -n "$line" ]]; do
    # Strip comments and surrounding whitespace.
    path="${line%%#*}"
    path="${path#"${path%%[![:space:]]*}"}"
    path="${path%"${path##*[![:space:]]}"}"
    [[ -z "$path" ]] && continue
    n_total=$((n_total + 1))
    row="$(classify_one "$path")"
    rows+=("$row")
    class="${row%%	*}"
    case "$class" in
        ok) n_ok=$((n_ok + 1)) ;;
        missing-file) n_missing_file=$((n_missing_file + 1)) ;;
        missing-module) n_missing_module=$((n_missing_module + 1)) ;;
        parse-error) n_parse=$((n_parse + 1)) ;;
        *) n_other=$((n_other + 1)) ;;
    esac
    printf '%s\n' "$row"
done < "$MANIFEST"

{
    echo "# perlc real-world compile corpus"
    echo "# host: $(uname -srm)  date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "# perlc: $PERLC_BIN"
    echo "# compile-only; binaries were not executed"
    echo "# total=$n_total ok=$n_ok missing-file=$n_missing_file missing-module=$n_missing_module parse-error=$n_parse other-compile-error=$n_other"
    echo
    printf '%s\n' "${rows[@]}"
} > "$REPORT"

if [[ ! -f "$SCRIPT_DIR/baseline.txt" || "${REALWORLD_UPDATE_BASELINE:-0}" == 1 ]]; then
    cp "$REPORT" "$SCRIPT_DIR/baseline.txt"
    echo "# wrote $SCRIPT_DIR/baseline.txt"
fi

echo
echo "Summary  total=$n_total  ok=$n_ok  missing-file=$n_missing_file  missing-module=$n_missing_module  parse-error=$n_parse  other=$n_other"
echo "Report   $REPORT"
echo "This corpus is not a gate. Exit 0."
exit 0
