#!/usr/bin/perl
# In-depth test suite for `return` inside eval{}.
#
# Root cause (D25): codegen's NK::Return case unconditionally compiled a
# top-level `return` (one with no enclosing named sub — i.e. inside `main`)
# to perl_die(), on the theory that "inside an eval block, return is
# equivalent to die (no enclosing sub to return from)". That theory is
# wrong: real Perl's `return` inside eval{} always exits just the nearest
# enclosing eval block with the given value, becoming that eval's result —
# regardless of whether there's also an enclosing named sub — and it does
# NOT behave like die (doesn't set $@, doesn't get caught as an exception
# by an *outer* eval). Confirmed directly against real Perl, including the
# non-obvious case where a named sub contains `eval { return X }` — the
# return targets the eval, and execution continues in the sub afterward,
# it does NOT return from the sub itself (a well-known, easy-to-get-wrong
# corner of real Perl semantics).
#
# Fixed with a new evalReturnTargets_ stack (codegen.h/.cpp): NK::EvalBlock
# pushes {resultAlloca, endBB} before compiling its body and pops it after;
# NK::Return checks this stack first — if non-empty, it stores the return
# value into the innermost active eval's resultAlloca and branches to its
# endBB (a normal LLVM branch, not die's longjmp), instead of the old
# always-die behavior. NK::AnonSub saves/clears/restores the stack around
# its own body compilation (matching how it already isolates scopes_ etc.),
# since `return` inside an anonymous sub must target the anon sub itself,
# not an enclosing eval.
#
# A SEPARATE, pre-existing, more fundamental bug was found and fixed while
# testing this (not caused by this fix — reproduces with zero `return`
# statements involved): NK::EvalBlock's own codegen, when a *nested* eval
# was a non-last statement in the outer eval's body (e.g. `eval { my $x =
# eval {...}; MORE_CODE }`), left the block where MORE_CODE was compiled
# without ever storing the outer eval's actual computed result into its own
# resultAlloca before branching to its own endBB — silently discarding the
# real value and always reading back undef instead. Fixed by adding the
# missing store. This was blocking proper test coverage of nested eval +
# return scenarios, so it's covered here alongside the D25 fix itself.
#
# NOTE: named subs are declared at file scope, not inside a nested bare
# block — a named sub declared inside a bare block that follows another
# bare block resolves to the wrong value at its call site (TESTS.md D45,
# a separate, already-tracked bug unrelated to this fix).
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

sub foo1        { my $x = eval { return "from eval"; }; return "x=$x, from foo1"; }
sub multi_eval  { my $a = eval { return "a"; }; my $b = eval { return "b"; }; return "$a-$b"; }

# ── Section 1: the original bug — nested eval, return based on caught $@ ───
{
    my $outer = eval {
        eval { die "inner error\n"; };
        return "inner caught: $@" if $@;
        return "no error";
    };
    check('return_after_catching_inner_die', index($outer, "inner caught") >= 0);
}

# ── Section 2: return in eval within a sub — execution continues in the ────
# ── sub afterward, does NOT return from the sub itself ─────────────────────
{
    check('return_in_eval_within_sub', foo1() eq "x=from eval, from foo1");
}

# ── Section 3: nested eval, return targets the innermost eval only ─────────
{
    my $r = eval {
        my $inner = eval { return "inner return"; };
        return "outer got: $inner";
    };
    check('nested_eval_return_targets_innermost', $r eq "outer got: inner return");
}

# ── Section 4: return inside an anon sub called within an eval targets the ─
# ── anon sub, NOT the enclosing eval ────────────────────────────────────────
{
    my $r = eval {
        my $cb = sub { return "from anon sub"; };
        my $cbval = $cb->();
        return "eval got: $cbval";
    };
    check('anon_sub_return_not_captured_by_eval', $r eq "eval got: from anon sub");
}

# ── Section 5: no explicit return — implicit last-expression value ─────────
# ── (regression check, unaffected by this fix) ──────────────────────────────
{
    my $r = eval { 1 + 1; };
    check('implicit_last_expr_regression', $r == 2);
}

# ── Section 6: return with no value gives undef ─────────────────────────────
{
    my $r = eval { return; };
    check('return_no_value_is_undef', !defined($r));
}

# ── Section 7: multiple sequential evals with return, in the same sub ──────
{
    check('multiple_sequential_eval_returns', multi_eval() eq "a-b");
}

# ── Section 8: caught return inside inner eval, then die in the outer eval ─
# ── — die must still propagate normally, unaffected by the earlier return ──
{
    my $r;
    my $ok = eval {
        eval { return "caught return"; };
        die "should not reach\n";
        1;
    };
    check('die_after_inner_return_still_propagates', !$ok && index($@, "should not reach") >= 0);
}

# ── Section 9: return in eval nested inside a loop ──────────────────────────
{
    my @results;
    for my $i (1..3) {
        my $v = eval { return "val$i" if $i == 2; "default$i"; };
        push @results, $v;
    }
    check('return_in_eval_inside_loop', join(",", @results) eq "default1,val2,default3");
}

# ── Section 10: nested eval (no return at all) followed by more code in the ─
# ── outer eval's body — the separate pre-existing bug found while testing ──
{
    my $r = eval {
        my $inner = eval { "inner value"; };
        "outer got: $inner";
    };
    check('nested_eval_no_return_trailing_code', $r eq "outer got: inner value");
}

# ── Section 11: return inside if/else branches within an eval ──────────────
{
    my $cond = 1;
    my $r = eval { if ($cond) { return "yes"; } return "no"; };
    check('return_inside_if_true_branch', $r eq "yes");
}
{
    my $cond = 0;
    my $r = eval { if ($cond) { return "yes"; } return "no"; };
    check('return_inside_if_false_branch', $r eq "no");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "eval_return_tests_done\n";
