#!/usr/bin/env bash
# tests/d128_module_error_location.sh — D128: a parse error inside an inlined
# module must report the MODULE's own file name and internal line number, not
# a meaningless line from the combined main+module token stream, and must not
# silently report nothing but a line number. A parse error in the MAIN script
# keeps the exact legacy format ("Parse error line N:").
#
# Compile-failure diagnostics can't live in the stdout-diff harness (it only
# compares successful runs), so this is a self-verifying script instead.
# Not part of the harness corpus; run explicitly:
#   bash tests/d128_module_error_location.sh
#
# Env: PERLC (default $PROJECT_DIR/perlc), PERL (default `perl`).

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PERLC_BIN="${PERLC:-$PROJECT_DIR/perlc}"
PERL_CMD="${PERL:-perl}"

TMPDIR_D128="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_D128"' EXIT

MAIN_FIXTURE="$SCRIPT_DIR/d128_module_error_main.pltxt"
MOD_FIXTURE="$SCRIPT_DIR/lib/D128Broken.pm"
MOD_NAME="D128Broken.pm"

# Module-internal line of the deliberate syntax error in D128Broken.pm
# (`my = 5;` — line 3).
MOD_ERR_LINE=3
# A line number in this range can only belong to the COMBINED (module-spliced)
# token stream seen by the parser — e.g. D128Broken.pm's own `use warnings;`
# lands around line 5-6 of the combined stream. If the error ever reports
# something like this again, the bug is back.
MAIN_STREAM_LINE=6

failures=0
pass() { printf "PASS %s\n" "$1"; }
fail() { printf "FAIL %s\n" "$1"; failures=$((failures+1)); }

# ── Case 1: parse error inside the inlined module ────────────────────────────
# (assertions a,b,c from the defect brief)
compile_err="$("$PERLC_BIN" "$MAIN_FIXTURE" -o "$TMPDIR_D128/d128_out" 2>&1 1>/dev/null)"
compile_rc=$?

[ $compile_rc -ne 0 ] && pass "d128: broken-module fixture fails to compile (rc=$compile_rc)" \
                     || fail "d128: broken-module fixture unexpectedly compiled OK"

# (a) the message names the module file
printf '%s\n' "$compile_err" | grep -q "$MOD_NAME" \
    && pass "d128 (a): error names the module file ($MOD_NAME)" \
    || fail "d128 (a): error does not mention $MOD_NAME — got: $compile_err"

# (b) it reports the module-INTERNAL line number
printf '%s\n' "$compile_err" | grep -Eq "line $MOD_ERR_LINE\b" \
    && pass "d128 (b): error reports module-internal line $MOD_ERR_LINE" \
    || fail "d128 (b): error does not report module-internal line $MOD_ERR_LINE — got: $compile_err"

# (c) it does NOT report the post-inlining combined-stream position
printf '%s\n' "$compile_err" | grep -Eq "line $MAIN_STREAM_LINE\b" \
    && fail "d128 (c): error reports combined-stream line $MAIN_STREAM_LINE — got: $compile_err" \
    || pass "d128 (c): error does not report combined-stream line $MAIN_STREAM_LINE"

# new message shape sanity: "Parse error in <module> line N:"
printf '%s\n' "$compile_err" | grep -Eq "Parse error in .*${MOD_NAME} line ${MOD_ERR_LINE}:" \
    && pass "d128: message shape 'Parse error in $MOD_NAME line $MOD_ERR_LINE:'" \
    || fail "d128: unexpected message shape — got: $compile_err"

# cross-check vs real perl: the module-internal line must agree
if command -v "$PERL_CMD" >/dev/null 2>&1; then
    perl_err="$("$PERL_CMD" -I "$SCRIPT_DIR/lib" "$MAIN_FIXTURE" 2>&1 1>/dev/null)"
    if printf '%s\n' "$perl_err" | grep -q "${MOD_NAME} line ${MOD_ERR_LINE}"; then
        pass "d128: real perl also blames $MOD_NAME line $MOD_ERR_LINE"
    else
        fail "d128: real perl disagrees — got: $perl_err"
    fi
else
    printf "SKIP d128: real perl cross-check (no perl on PATH)\n"
fi

# ── Case 2 (control): parse error in the MAIN script keeps the legacy format ─
cat > "$TMPDIR_D128/main_err.pl" <<'EOF'
use warnings;
my = 5;
print "never reached\n";
EOF
ctrl_err="$("$PERLC_BIN" "$TMPDIR_D128/main_err.pl" -o "$TMPDIR_D128/d128_ctl_out" 2>&1 1>/dev/null)"
ctrl_rc=$?

[ $ctrl_rc -ne 0 ] && pass "d128: main-script error fixture fails to compile (rc=$ctrl_rc)" \
                  || fail "d128: main-script error fixture unexpectedly compiled OK"

# (d) legacy main-script format preserved: starts with "Parse error line N:"
printf '%s\n' "$ctrl_err" | grep -Eq "^Error: Parse error line [0-9]+:" \
    && pass "d128 (d): main-script error keeps legacy 'Parse error line N:' format" \
    || fail "d128 (d): main-script error format changed — got: $ctrl_err"

# and it does NOT pick up a filename prefix
printf '%s\n' "$ctrl_err" | grep -q "Parse error in " \
    && fail "d128 (d2): main-script error wrongly uses module format — got: $ctrl_err" \
    || pass "d128 (d2): main-script error has no filename prefix"

# main script's true line number
printf '%s\n' "$ctrl_err" | grep -Eq "Parse error line 2:" \
    && pass "d128: main-script error reports true line 2" \
    || fail "d128: main-script error line wrong — got: $ctrl_err"

echo ""
if [ $failures -eq 0 ]; then
    echo "=== d128_module_error_location: ALL PASS ==="
    exit 0
fi
echo "=== d128_module_error_location: $failures FAILURE(S) ==="
exit 1