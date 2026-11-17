#!/bin/bash
# Slice 1 (blocker #1 / do-eval-options option (a)): constant arithmetic /
# list / multi-statement / conditional / literal / constant-qr / eval-block
# string evals must compile IN-PROCESS (EvalBlock) — no perl_eval_string /
# perl_do_file call, no runtime perlc+clang spawn. Genuinely dynamic evals
# (a string built by runtime interpolation) correctly remain deferred and
# still spawn.
#
# This is a *structural* regression check (absence/presence of a call site),
# not a byte-for-byte perl compare — the harness already covers output.
# A pure output compare would NOT catch a regression that made a constant
# eval fall back through perl_eval_string (perl still runs it correctly),
# so the spawn check is what actually locks in slice 1.
#
# make test-all runs this after the harness. Needs perl + objdump.
set -euo pipefail
cd "$(dirname "$0")/.."

PERLC="${PERLC:-./perlc}"
PERL="${PERL:-perl}"
ROOT="/tmp/pt/eval_inline"
rm -rf "$ROOT"; mkdir -p "$ROOT"

# Constant evals: none of these may spawn.
cat > "$ROOT/const.pl" << 'EOF'
use strict; use warnings;
my $pat = "a.c";
# arithmetic / list / multi / conditional / literal / empty — all constant.
print "arith=", eval "1+2", "\n";
print "list=", join(",", eval "(1,2,3)"), "\n";
print "multi=", eval "1; 2; 3", "\n";
print "cond=", eval "1 ? 'a' : 'b'", "\n";
print "lit=", eval "42", "\n";
print "empty=", (defined eval "" ? 1 : 0), "\n";
print "qr=", (("xyz" =~ eval "qr/xyz/") ? 1 : 0), "\n";
print "parens=", eval("9*9"), "\n";
print "block=", eval { 7 }, "\n";
EOF

# A genuinely dynamic eval: a string built by runtime interpolation MUST
# still defer to perl_eval_string (slice 1 does NOT touch the dynamic case).
cat > "$ROOT/dyn.pl" << 'EOF'
use strict; use warnings;
my $x = 5;
my $y = eval "10 + $x";
print "y=$y\n";
EOF

# --- const program must NOT spawn -------------------------------------------
if ! "$PERLC" "$ROOT/const.pl" -o "$ROOT/const" >"$ROOT/c.log" 2>&1; then
    echo "slice1: const program failed to compile" >&2
    cat "$ROOT/c.log" >&2
    exit 1
fi
if ! "$ROOT/const" >"$ROOT/const.out" 2>/dev/null; then
    echo "slice1: const binary did not run" >&2
    exit 1
fi
if ! "$PERL" "$ROOT/const.pl" >"$ROOT/const.perl" 2>/dev/null; then
    echo "slice1: perl could not run const program" >&2
    exit 1
fi
if ! diff -u "$ROOT/const.perl" "$ROOT/const.out" >/dev/null; then
    echo "slice1: const output differs from perl" >&2
    diff -u "$ROOT/const.perl" "$ROOT/const.out" >&2
    exit 1
fi
calls="$(objdump -d --no-show-raw-insn "$ROOT/const" 2>/dev/null | \
        grep -cE '\bcall\b.*(perl_eval_string|perl_do_file)' || true)"
if [ "$calls" != "0" ]; then
    echo "slice1: constant eval SPAWNED ($calls perl_eval_string/do_file calls)" >&2
    objdump -d --no-show-raw-insn "$ROOT/const" 2>/dev/null | \
        grep -E '\bcall\b.*(perl_eval_string|perl_do_file)' >&2
    exit 1
fi

# --- dynamic program STILL spawns (guards against over-reclaim) -------------
if ! "$PERLC" "$ROOT/dyn.pl" -o "$ROOT/dyn" >"$ROOT/d.log" 2>&1; then
    echo "slice1: dynamic program failed to compile" >&2
    cat "$ROOT/d.log" >&2
    exit 1
fi
if ! "$ROOT/dyn" >"$ROOT/dyn.out" 2>/dev/null; then
    echo "slice1: dynamic binary did not run" >&2
    exit 1
fi
if ! "$PERL" "$ROOT/dyn.pl" >"$ROOT/dyn.perl" 2>/dev/null; then
    echo "slice1: perl could not run dynamic program" >&2
    exit 1
fi
if ! diff -u "$ROOT/dyn.perl" "$ROOT/dyn.out" >/dev/null; then
    echo "slice1: dynamic output differs from perl" >&2
    diff -u "$ROOT/dyn.perl" "$ROOT/dyn.out" >&2
    exit 1
fi
calls="$(objdump -d --no-show-raw-insn "$ROOT/dyn" 2>/dev/null | \
        grep -cE '\bcall\b.*(perl_eval_string|perl_do_file)' || true)"
if [ "$calls" != "1" ]; then
    echo "slice1: dynamic eval expected exactly 1 spawn call, got $calls" >&2
    exit 1
fi

echo SLICE1_EVAL_INLINE_OK
