#!/usr/bin/env bash
# tests/realworld/run_gaps.sh — Phase-0 runtime-gap inventory runner.
#
# Companion to run.sh. While run.sh is compile-ONLY, this runner EXECUTES
# every successfully-compiled script (in tests/realworld/manifest.txt) with
# PERLC_GAPS set, so perlc's runtime gap detector records the substitutions
# it could not resolve. Sidecars are written to tests/realworld/gaps/*.jsonl
# and summarised by tools/gap_report.py.
#
# Requirements:
#   * ./perlc must be the TRACE-enabled build (make TRACE=1). Without the
#     -DPERLC_GAP_TRACE instrumentation PERLC_GAPS does nothing; this script
#     checks for it and aborts with a hint.
#   * The manifest is host-specific — a missing path is skipped, not a
#     failure.
#
# The runner is defensive by design: each binary is time-limited and its
# exit status is ignored, because our goal is to capture as many distinct
# runtime gaps as possible before it dies or times out, not to run the tool
# to completion.  The script always exits 0.

set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
PERLC_BIN="${PERLC:-$PROJECT_DIR/perlc}"
MANIFEST="${REALWORLD_MANIFEST:-$SCRIPT_DIR/manifest.txt}"
GAPDIR="$SCRIPT_DIR/gaps"
TIMEOUT_SEC="${REALWORLD_GAP_TIMEOUT:-20}"

if [[ ! -f "$MANIFEST" ]]; then
    echo "manifest not found: $MANIFEST" >&2
    exit 1
fi
mkdir -p "$GAPDIR"
rm -f "$GAPDIR"/*.jsonl 2>/dev/null || true

# The runtime detector only exists in a TRACE-enabled perlc. Verify it is
# present, else PERLC_GAPS is inert and we would silently produce nothing.
# gap_emit_json_blob is used as the marker: it is taken by address via
# atexit() and therefore can never be inlined, unlike the small helpers.
# NB: test with `grep -c` (not `grep -q`). This script runs with `set -o
# pipefail`; `grep -q` exits on the first match and leaves `nm` with no
# reader, so `nm` dies of SIGPIPE and the pipeline reports non-zero even
# though the symbol was found.
if [ "$(nm -C "$PERLC_BIN" 2>/dev/null | grep -c gap_emit_json_blob)" -eq 0 ]; then
    echo "perlc is not a trace build: $PERLC_BIN" >&2
    echo "build one first:  make TRACE=1   (or run:  make gap)" >&2
    exit 1
fi

tmpdir="$(mktemp -d /tmp/perlc-gaps.XXXXXX)"
trap 'rm -rf "$tmpdir"' EXIT

n_ok=0
n_gapped=0
n_skipped=0
n_total=0
rows=()

# run one compiled binary under the gap detector; append JSONL to $1.
run_under_gap() {
    local bin="$1" sidecar="$2"
    PERLC_GAPS="$sidecar" timeout "$TIMEOUT_SEC" "$bin" </dev/null >/dev/null 2>&1
    # exit code is irrelevant: a gap may have forced exit(255), or the tool
    # may have exited cleanly — either way the sidecar is what we read.
}

while IFS= read -r line || [[ -n "$line" ]]; do
    path="${line%%#*}"
    path="${path#"${path%%[![:space:]]*}"}"
    path="${path%"${path##*[![:space:]]}"}"
    [[ -z "$path" ]] && continue
    n_total=$((n_total + 1))
    base="$(basename "$path")"

    if [[ ! -f "$path" ]]; then
        rows+=("missing-file	$path	(missing on host)")
        n_skipped=$((n_skipped + 1))
        continue
    fi

    outfile="$tmpdir/${base}.bin"
    if ! timeout "$TIMEOUT_SEC" "$PERLC_BIN" "$path" -o "$outfile" \
            >"$tmpdir/${base}.out" 2>"$tmpdir/${base}.err" </dev/null; then
        rm -f "$outfile"
        rows+=("compile-fail	$path")
        continue
    fi

    sidecar="$GAPDIR/${base}.jsonl"
    run_under_gap "$outfile" "$sidecar"
    rm -f "$outfile" "$tmpdir/${base}.out" "$tmpdir/${base}.err"

    if [[ -s "$sidecar" ]]; then
        gaps="$(wc -l < "$sidecar" | tr -d ' ')"
        rows+=("gapped	$path	${gaps} gaps -> ${sidecar}")
        n_gapped=$((n_gapped + 1))
    else
        rows+=("nogap	$path	(no runtime gaps on a bare run)")
    fi
done < "$MANIFEST"

REPORT="$SCRIPT_DIR/gaps/INVENTORY.txt"
{
    echo "# perlc runtime-gap inventory (Phase 0)"
    echo "# host: $(uname -srm)  date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "# perlc: $PERLC_BIN  (trace build)"
    echo "# each captured script was run with PERLC_GAPS=<sidecar> for ${TIMEOUT_SEC}s"
    echo "# total=$n_total compiled=$n_gapped nogap/other=$((n_total - n_gapped))"
    echo
    printf '%s\n' "${rows[@]}"
} > "$REPORT"

echo
echo "Runtime-gap inventory"
echo "  compiled+ran with gaps : $n_gapped"
echo "  other (no gaps / skip) : $((n_total - n_gapped))"
echo "  sidecar dir            : $GAPDIR"
echo "  summary                : $REPORT"
echo
echo  "Next:  python3 tools/gap_report.py '$GAPDIR'/*.jsonl"
exit 0
