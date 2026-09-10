#!/usr/bin/perl
# Deep: D100 — list assignment used as a while/if condition. Real Perl:
# list assignment in scalar/boolean context evaluates to the COUNT of
# elements on the RHS. Root cause and fix:
# - src/codegen.cpp's `case NK::Assign` (ArrayLit LHS) used to
#   unconditionally return a void/null PerlValue*, so
#   `perl_is_true(null)` was always false and the loop/branch body never
#   ran at all, for ANY RHS (not just each()). Fixed to return
#   `perl_array_len(rhsArr)` (already a registered "owned temp", so every
#   existing consumer — ExprStmt, emitBlockLast, If, While — handles it
#   correctly with no other change).
# - Separately, the expression-context parse of `my ($a, $b) = EXPR`
#   (parser.cpp) wraps each variable as a bare NK::My node that
#   emitLValue() didn't understand, so even once the loop iterated the
#   right number of times, $k/$v stayed undef the whole time. Fixed by
#   handling NK::My directly in the assignment loop, with While hoisting
#   the one-time alloca before the loop (mirroring the existing
#   single-variable `myCondPv` hoist) so a long-running loop doesn't leak
#   stack by re-executing an alloca every iteration.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

{
    # basic while(each) draining, and the loop variables must actually be
    # populated (not just the right iteration count).
    my %h = (a => 1, b => 2, c => 3);
    my %seen;
    my $n = 0;
    while (my ($k, $v) = each %h) {
        $n++;
        $seen{$k} = $v;
    }
    check('while_each_count', $n == 3);
    check('while_each_values',
        $seen{a} == 1 && $seen{b} == 2 && $seen{c} == 3);
}
{
    # without `my` — bare list-assignment condition.
    my %h = (x => 10, y => 20);
    my ($k, $v);
    my $n = 0;
    while (($k, $v) = each %h) { $n++; }
    check('while_bare_list_assign', $n == 2);
}
{
    # splice-based draining (not each-specific).
    my @list = (10, 20, 30, 40);
    my @seen;
    while (my ($x, $y) = splice(@list, 0, 2)) {
        push @seen, "$x/$y";
    }
    check('while_splice_drain', "@seen" eq "10/20 30/40");
}
{
    # if/else with a non-empty and an empty hash.
    my %h = (a => 1);
    my ($got_k, $got_v);
    if (my ($k, $v) = each %h) { ($got_k, $got_v) = ($k, $v); }
    else { $got_k = 'NONE'; }
    check('if_nonempty', $got_k eq 'a' && $got_v == 1);

    my %empty;
    my $branch = 'unset';
    if (my ($k2, $v2) = each %empty) { $branch = 'true'; }
    else { $branch = 'false'; }
    check('if_empty', $branch eq 'false');
}
{
    # until (negated while).
    my %h = (p => 1, q => 2);
    my @out;
    until (!(my ($k, $v) = each %h)) { push @out, "$k=$v"; }
    check('until_form', join(",", sort @out) eq "p=1,q=2");
}
{
    # unless (negated if), empty hash.
    my %empty;
    my $ok = 0;
    unless (my ($k, $v) = each %empty) { $ok = 1; }
    check('unless_form', $ok == 1);
}
{
    # nested: if-inside-while, each My-decl only hoisted once per loop,
    # not once per outer iteration.
    my %h = (x => 1, y => 2, z => 3);
    my $n = 0;
    while (1) {
        if (my ($k, $v) = each %h) { $n++; }
        else { last; }
    }
    check('if_inside_while', $n == 3);
}
{
    # statement-level `my ($a,$b) = EXPR;` must be unaffected by the
    # expression-context codegen changes above.
    my ($a, $b) = (10, 20);
    check('statement_level_unaffected', $a == 10 && $b == 20);
}
{
    # stress: large iteration count must not crash / leak stack (the
    # hoisted-alloca fix is specifically about not re-executing an
    # alloca every loop iteration).
    my %big;
    $big{$_} = $_ for (1 .. 50000);
    my $count = 0;
    while (my ($k, $v) = each %big) { $count++; }
    check('large_iteration_count', $count == 50000);
}
{
    # list assignment as a plain boolean expression (not directly a
    # while/if condition) — count-of-RHS-elements semantics.
    my ($x, $y, $z);
    my $n = (($x, $y, $z) = (1, 2));
    check('scalar_context_count', $n == 2);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d100_list_assign_cond_done\n";
