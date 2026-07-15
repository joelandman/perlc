#!/usr/bin/perl
# In-depth test suite for D52: `$@` was never wired up as an assignment
# target at all — `$@ = "..."` (and any compound-assignment form) parsed
# but compiled to nothing, because `$@` parses to its own distinct
# NK::DollarAt AST node (not NK::ScalarVar with name "@", the way `$/`
# and `$!` do), and both the plain-assignment path (NK::Assign) and the
# generic emitLValue() helper used by compound assignment only had cases
# for NK::ScalarVar / NK::ArrayElem — anything else silently fell through
# to a no-op default.
#
# While fixing this and testing it against a realistic exception-handling
# idiom (explicitly clearing $@ from inside a *nested* eval block), a
# second, related bug in the same codepath was found and fixed alongside
# it: perlc reset $@="" *before* running an eval block's body, instead of
# *after* successful completion like real Perl does. Real Perl's
# post-success reset unconditionally overrides anything the block itself
# did to $@ (a documented, if surprising, real Perl behavior) — with the
# reset only happening pre-body, an explicit `$@ = ...` made inside a
# successful eval block would wrongly "stick" instead of being clobbered
# by eval's own success-clear once $@ became a real assignment target.
# Moved the reset to fire only on eval's success path (immediately before
# branching to the shared end block), not on the longjmp/die path (which
# leaves whatever perl_die already wrote to $@ untouched).
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: basic clear after a caught exception ──────────────────────
{
    eval { die "boom\n" };
    my $before = ($@ ne "");
    $@ = "";
    check('basic_clear_before_nonempty', $before);
    check('basic_clear_after_empty', $@ eq "");
}

# ── Section 2: assign an arbitrary string, not just clearing ─────────────
{
    $@ = "custom message";
    check('assign_custom_string', $@ eq "custom message");
}

# ── Section 3: compound assignment (.=) on $@ ─────────────────────────────
{
    eval { die "err1\n" };
    $@ .= " appended";
    check('compound_dot_assign', $@ eq "err1\n appended");
}

# ── Section 4: assigning a numeric value ──────────────────────────────────
{
    $@ = 42;
    check('numeric_assign', $@ == 42);
    check('numeric_assign_stringifies', "$@" eq "42");
}

# ── Section 5: truthiness before/after clearing ───────────────────────────
{
    eval { die "err2\n" };
    my $truthy_before = $@ ? 1 : 0;
    $@ = '';
    my $falsy_after = !$@ ? 1 : 0;
    check('truthy_before_clear', $truthy_before == 1);
    check('falsy_after_clear', $falsy_after == 1);
}

# ── Section 6: a clean (non-dying) eval already resets $@ to "" ──────────
{
    eval { die "stale\n" };
    eval { 1 };
    check('clean_eval_resets_at', $@ eq "");
}

# ── Section 7: explicit in-block assignment gets overridden by eval's own
#    post-success clear (the second bug found/fixed alongside this one) ──
{
    eval { die "outer\n" };
    eval {
        $@ = "manually cleared inside nested eval";
    };
    check('inblock_assign_overridden_by_success_clear', $@ eq "");
}

# ── Section 8: $@ survives across a sub call that reads it, then gets
#    reassigned by the caller afterward ──────────────────────────────────
{
    sub might_die {
        eval { die "in_sub\n" };
        return $@;
    }
    my $captured = might_die();
    $@ = "reset_after_call";
    check('captured_before_reset', $captured eq "in_sub\n");
    check('reset_after_call', $@ eq "reset_after_call");
}

# ── Section 9: assignment persists across subsequent unrelated statements
#    (not just immediately re-read) ───────────────────────────────────────
{
    eval { die "will be cleared\n" };
    $@ = "persisted";
    my $x = 1 + 1;
    my $y = "noop";
    check('assign_persists_across_statements', $@ eq "persisted");
}

# ── Section 10: regression — a die inside eval still correctly populates
#    $@ with the die message (the read path, untouched by this fix) ──────
{
    eval { die "regression check\n" };
    check('die_still_populates_at', $@ eq "regression check\n");
}

# ── Section 11: intentionally NOT asserted here — die() with a message
#    that lacks a trailing newline does not append Perl's standard
#    " at FILE line N.\n" suffix at all (verified true of both the
#    eval-caught and uncaught/top-level paths). This is a real,
#    previously-undiscovered, separate gap found while writing this test
#    (D52 is specifically about $@ as an assignment target, not die's
#    message formatting) — logged as new defect D89, left unfixed and
#    out of scope here.

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d52_error_var_assign_done\n";
