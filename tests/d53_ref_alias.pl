#!/usr/bin/perl
# In-depth test suite for D53: `\$x` (taking a reference to a block-scoped
# `my` variable) lost its aliasing to the original variable — writing
# through the reference didn't reach subsequent reads of the variable.
#
# Originally logged (and misdiagnosed) as specifically a *self-assignment*
# bug (`my $r = \$x; $r = $r;` breaking the reference) — re-investigation
# while fixing it found the self-assignment step was a red herring: the
# identical failure reproduces with no self-assignment involved at all,
# from just `my $x = 42; my $r = \$x; $$r = 99; print $x;`. Confirmed via
# `--emit-ir` inspection that the block-scoped version compiles $x's reads
# to a hardcoded `perl_alloc_int(42)` constant at every use site — a
# completely disconnected value from whatever `$$r = 99` actually wrote —
# while the identical code at *file* scope (where $x gets a real
# GlobalVariable instead) works correctly, isolating the bug to exactly
# one thing: perlc's unboxed int/float fast path for block-scoped `my
# $var = <literal>` declarations (intScopes_/floatScopes_, an optimization
# that skips allocating a real, addressable PerlValue* for the variable
# entirely, keeping just a plain `i64`/`double` LLVM value instead).
# `\$x`'s codegen (NK::RefScalar) calls the ordinary scalar-read path
# (NK::ScalarVar in emitExpr), which — when a variable is on this fast
# path — "boxes on demand": it allocates a *fresh*, disposable PerlValue*
# containing a *copy* of the fast path's current value, tied to nothing.
# `perl_ref_scalar()` then creates a reference to that disposable copy,
# not to $x's real (non-existent, in this case) storage — so `$$r = 99`
# correctly mutates the disposable copy, while $x's actual unboxed
# storage — and therefore every subsequent plain read of $x — is
# completely unaffected. This is the exact same underlying architectural
# gap D64 already found and fixed for closures capturing an unboxed
# variable; D53 is the identical gap, just discovered via `\$x` instead
# of `sub { ...$x... }`.
#
# Fixed by extending the existing `collectClosureCapturedNames` pre-scan
# (added for D64) to also collect the target name of any `\$name`
# (RefScalar) found anywhere in the current function, not just names used
# inside a nested closure — both cases now share the exact same
# `capturedNamesInCurrentFn_` set and the exact same gating check at the
# `my $var = <literal>` declaration site, so a variable that's ever
# referenced via `\$var` anywhere in its function is declared as a normal,
# fully-addressable boxed scalar from the start, never placed on the
# unboxed fast path at all.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original repro, including the self-assignment step
#    D53 was originally (mis-)diagnosed around ─────────────────────────────
{
    my $x = 42;
    my $r = \$x;
    $r = $r;
    $$r = 99;
    check('original_repro_with_self_assign', $x == 99);
}

# ── Section 2: the real, broader bug — reproduces with NO self-assignment
#    at all (the actual root cause, per the investigation above) ──────────
{
    my $x = 42;
    my $r = \$x;
    $$r = 99;
    check('write_through_ref_no_self_assign', $x == 99);
}

# ── Section 3: float variable (the fast path has a separate float branch,
#    intScopes_ vs floatScopes_, both needed fixing) ────────────────────────
{
    my $f = 3.14;
    my $rf = \$f;
    $$rf = 2.71;
    check('float_var_write_through_ref', abs($f - 2.71) < 1e-9);
}

# ── Section 4: regression — a bare read through a reference (no write)
#    already worked before this fix; confirm it's still correct ──────────
{
    my $x = 10;
    my $r = \$x;
    check('bare_read_through_ref_regression', $$r == 10);
}

# ── Section 5: multiple references to the same variable all see a write
#    made through any one of them ───────────────────────────────────────────
{
    my $x = 1;
    my $r1 = \$x;
    my $r2 = \$x;
    $$r1 = 100;
    check('multiple_refs_see_write_via_r2', $$r2 == 100);
    check('multiple_refs_see_write_via_x',  $x   == 100);
}

# ── Section 6: the variable is still usable in ordinary arithmetic after
#    a reference to it has been taken (confirms the promoted/boxed
#    representation doesn't break normal use of the variable itself) ──────
{
    my $x = 5;
    my $r = \$x;
    my $y = $x + 10;
    check('arithmetic_after_ref_taken', $y == 15);
}

# ── Section 7: a reference to a sub's own lexical, taken and written
#    through, correctly affects that sub's return value ───────────────────
sub make_ref {
    my $v = 7;
    my $r = \$v;
    $$r = 42;
    return $v;
}
check('ref_inside_sub_body', make_ref() == 42);

# ── Section 8: regression — the unboxed int fast path is still exercised
#    (and correct) for a variable that never has a reference taken ───────
{
    my $sum = 0;
    for (my $i = 0; $i < 5; $i++) {
        $sum = $sum + $i;
    }
    check('unboxed_fast_path_regression', $sum == 10);
}

# ── Section 9: regression — closures capturing an unboxed variable still
#    work correctly (D64, the sibling fix this one is modeled on) ────────
{
    my $c = 1;
    my $inc = sub { $c++; };
    $inc->();
    $inc->();
    check('closure_capture_regression', $c == 3);
}

# ── Section 10: regression — file-scope reference-through-write already
#    worked before this fix (it never used the unboxed fast path at all) ──
our $file_x = 42;
our $file_r = \$file_x;
$$file_r = 99;
check('file_scope_regression', $file_x == 99);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d53_ref_alias_done\n";
