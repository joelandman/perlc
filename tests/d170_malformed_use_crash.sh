#!/usr/bin/env bash
# tests/d170_malformed_use_crash.sh — D170: a `use MODULE ...` statement
# with no terminating semicolon before end-of-input made main.cpp's
# semicolon-scanning loop in inlineModules() walk one index past the
# lexer's trailing EOF_TOK sentinel (the loop only ever checked "not
# SEMI", never "not EOF_TOK"), so `useEnd` ended up == tokens.size() —
# one past the last valid index. Every downstream `tokens[useEnd]` /
# `tokens[useEnd-1]` / backward-scan-from-useEnd access in that function
# then indexed out of bounds: an assertion-failure crash in a debug
# libstdc++ (`vector::operator[] const: Assertion '__n < this->size()'
# failed`), undefined behavior otherwise — not a graceful parse error.
#
# Found via a non-Perl `/usr/bin/perldoc` (a plain shell-script
# placeholder on this particular system, not actually Perl, fed to
# perlc by mistake during a real-script survey) — but the underlying
# bounds bug is real and applies to any genuinely malformed/truncated
# .pl input, which perlc must reject gracefully, never crash on.
#
# A crash (or lack of one) isn't something a byte-for-byte stdout diff
# against real Perl can usefully express here (real Perl's own error
# text for malformed input differs from perlc's by design), so this is
# a self-verifying script instead, following the D128 precedent.
# Not part of the harness corpus; run explicitly:
#   bash tests/d170_malformed_use_crash.sh
#
# Env: PERLC (default $PROJECT_DIR/perlc)

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PERLC_BIN="${PERLC:-$PROJECT_DIR/perlc}"

TMPDIR_D170="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_D170"' EXIT

failures=0
pass() { printf "PASS %s\n" "$1"; }
fail() { printf "FAIL %s\n" "$1"; failures=$((failures+1)); }

check_no_crash() {
    local name="$1" file="$2"
    local out
    out=$("$PERLC_BIN" "$file" -o "$TMPDIR_D170/out" 2>&1)
    local rc=$?
    # A signal-killed process (SIGABRT from a failed assertion, SIGSEGV,
    # etc.) exits with 128+signal under bash. A graceful compile-error
    # exit is a plain small nonzero status (perlc's own `Error: ...`
    # path) or 0 if the input happens to still compile.
    if [[ $rc -ge 128 ]]; then
        fail "$name (crashed: exit $rc)"
        return
    fi
    if echo "$out" | grep -qi "assertion\|PLEASE submit a bug report\|Stack dump\|Segmentation fault"; then
        fail "$name (crash signature in output)"
        return
    fi
    pass "$name"
}

# Case 1: `use MODULE` with no semicolon, nothing after it at all —
# the exact minimal shape that triggered the original crash.
printf 'use POSIX' > "$TMPDIR_D170/case1.pl"
check_no_crash "no_semicolon_bare_eof" "$TMPDIR_D170/case1.pl"

# Case 2: a valid statement first, then a truncated `use` with an
# import list and no closing paren/semicolon.
printf 'use strict;\nuse POSIX qw(floor' > "$TMPDIR_D170/case2.pl"
check_no_crash "no_semicolon_mid_import_list" "$TMPDIR_D170/case2.pl"

# Case 3: a genuinely non-Perl file fed to perlc by mistake (the exact
# real-world trigger — a shell script, not Perl at all).
cat > "$TMPDIR_D170/case3.pl" <<'EOF'
#!/bin/sh
echo hello >&2
exit 1
EOF
check_no_crash "non_perl_shell_script" "$TMPDIR_D170/case3.pl"

# Regression: a normal, well-formed `use` statement (with a real
# trailing semicolon) must still compile and run correctly — the fix
# must not have broken the ordinary case.
cat > "$TMPDIR_D170/case4.pl" <<'EOF'
use strict;
use warnings;
use POSIX qw(floor);
print floor(3.7), "\n";
EOF
"$PERLC_BIN" "$TMPDIR_D170/case4.pl" -o "$TMPDIR_D170/case4_out" >/dev/null 2>&1
if [[ -x "$TMPDIR_D170/case4_out" ]]; then
    out4=$("$TMPDIR_D170/case4_out")
    if [[ "$out4" == "3" ]]; then
        pass "well_formed_use_regression"
    else
        fail "well_formed_use_regression (unexpected output: $out4)"
    fi
else
    fail "well_formed_use_regression (failed to compile)"
fi

echo ""
if [[ $failures -eq 0 ]]; then
    echo "All D170 checks passed."
    exit 0
else
    echo "$failures D170 check(s) FAILED."
    exit 1
fi
