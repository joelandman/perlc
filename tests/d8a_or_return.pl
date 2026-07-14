#!/usr/bin/perl
# In-depth test suite for D8a: `EXPR or return VALUE` (and `EXPR and return
# VALUE`) parsed successfully but never actually returned.
#
# Root cause: `EXPR or return VALUE` parses into a BinOp("||") whose RHS is
# a Block wrapping a single Return statement (Parser::parseOrRhs — needed
# because `return`/`die`/etc. are statement-shaped, not ordinary
# expressions, so they're wrapped in a Block to flow through the same
# emitExpr() path as any other RHS). CodeGen::emitBlockLast (which handles
# any Block used in expression context) has a special case for a Block
# whose LAST statement is a Return: it captures the return's value without
# emitting a real return, on the assumption that some outer caller — e.g.
# the code that emits a sub or do-file body — will perform the actual
# return afterward using that captured value. That assumption is correct
# for a sub/do-file body, but `EXPR or return VALUE`'s BinOp("||") codegen
# has no such outer caller: it just fed the captured value into the "or"'s
# own result and continued executing, so the return silently never
# happened — confirmed exactly matching the reported symptom: `f() or
# return "X";` compiled but execution continued to the next statement
# instead of returning "X".
#
# Fixed with a new CodeGen::emitShortCircuitRhs() helper used by both the
# "&&" and "||" BinOp codegen paths (in place of a plain emitExpr() call on
# the RHS): it detects this exact "Block wrapping a single Return" shape
# and dispatches straight to emitStmt() on the Return node — invoking the
# real, complete Return-handling logic (eval-target check so `return`
# inside `eval{}` still only exits the eval as real Perl requires,
# local-restore, scope cleanup, and the actual ret/branch instruction) —
# instead of routing through emitBlockLast's capture-only shortcut.
# emitBlockLast itself was deliberately NOT changed, since many other
# callers (ordinary sub bodies, do-file bodies, ternary/if branches) rely
# on its existing behavior being exactly what it already is.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original repro — "or return" ─────────────────────────────
sub f_or {
    my ($val) = @_;
    $val or return "or_X";
    return "or_Y";
}
check('or_return_falsy', f_or(0) eq "or_X");
check('or_return_truthy', f_or(1) eq "or_Y");

# ── Section 2: "and return" (same underlying AST shape, "&&" instead) ─────
sub f_and {
    my ($val) = @_;
    !$val and return "and_X";
    return "and_Y";
}
check('and_return_falsy', f_and(0) eq "and_X");
check('and_return_truthy', f_and(1) eq "and_Y");

# ── Section 3: multiple sequential guard clauses (the idiom's primary
#    real-world use case) ───────────────────────────────────────────────────
sub f_multi_guard {
    my ($a, $b) = @_;
    $a or return "a_falsy";
    $b or return "b_falsy";
    return "both_truthy";
}
check('multi_guard_first_falsy', f_multi_guard(0, 1) eq "a_falsy");
check('multi_guard_second_falsy', f_multi_guard(1, 0) eq "b_falsy");
check('multi_guard_both_truthy', f_multi_guard(1, 1) eq "both_truthy");

# ── Section 4: "or return" with no value (bare return, i.e. return undef) ──
sub f_no_value {
    my ($val) = @_;
    $val or return;
    return "reached";
}
check('or_return_no_value', !defined(f_no_value(0)));
check('or_return_no_value_not_triggered', f_no_value(1) eq "reached");

# ── Section 5: regression — plain trailing return (not "or return") still
#    works, exercising the untouched emitBlockLast shared path ─────────────
sub plain_return {
    my ($x) = @_;
    return $x * 2;
}
check('plain_trailing_return_regression', plain_return(5) == 10);

# ── Section 6: regression — return in the middle of a sub body (not the
#    last statement, a different code path than "or return"'s) ────────────
sub mid_return {
    my ($x) = @_;
    if ($x > 0) {
        return "positive";
    }
    return "non-positive";
}
check('mid_sub_return_positive', mid_return(5) eq "positive");
check('mid_sub_return_nonpositive', mid_return(-5) eq "non-positive");

# ── Section 7: "or return" inside eval{} — return must exit just the eval
#    (real Perl semantics), not the enclosing sub, exercising the
#    evalReturnTargets_ interaction the fix must preserve ─────────────────
sub eval_or_return {
    my ($x) = @_;
    my $r = eval {
        $x or return "exited_the_eval";
        "eval_completed";
    };
    return "outer: " . ($r // "undef-from-eval");
}
check('or_return_in_eval_not_triggered', eval_or_return(1) eq "outer: eval_completed");
check('or_return_in_eval_triggered', eval_or_return(0) eq "outer: exited_the_eval");

# ── Section 8: "or return" inside an anonymous sub ──────────────────────────
{
    my $anon = sub {
        my ($x) = @_;
        $x or return "anon_falsy";
        return "anon_truthy";
    };
    check('anon_sub_or_return_falsy', $anon->(0) eq "anon_falsy");
    check('anon_sub_or_return_truthy', $anon->(1) eq "anon_truthy");
}

# ── Section 9: regression — ordinary ||/&& (the symbolic, high-precedence
#    forms, which never go through parseOrRhs at all) are unaffected ──────
{
    my $x = 0 || 5;
    check('plain_or_operator_regression', $x == 5);
    my $y = 1 && 2;
    check('plain_and_operator_regression', $y == 2);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d8a_or_return_done\n";
