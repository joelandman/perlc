#!/usr/bin/perl
# In-depth test suite for D71: `%` (modulo) gave the wrong result whenever
# either operand was negative.
#
# Root cause: perlc used C's truncating modulo (LLVM's SRem in the unboxed
# int fast path, `%` in C for the boxed perl_mod() runtime path) rather than
# Perl's floored-division convention. Perl's documented rule: the result of
# $m % $n always has the same sign as $n (or is zero) — $m minus the
# largest multiple of $n not greater than $m when $n is positive, or the
# smallest multiple not less than $m when $n is negative. C's operator
# truncates toward zero instead, so it only agrees with Perl when both
# operands share the same sign (or the result is exactly zero).
#
# Fixed in two places: runtime.c's perl_mod() (the general boxed-value
# path) now adds back the divisor when the truncated remainder's sign
# doesn't match the divisor's sign; codegen.cpp gained a shared
# CodeGen::emitFlooredMod() helper doing the equivalent branchless LLVM IR
# (SRem + sign-mismatch select), used by both unboxed-int fast paths
# (plain `%` and compound `%=`) that previously called CreateSRem directly.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: literal sign combinations ────────────────────────────────────
check('lit_pos_pos',  (7 % 3)   == 1);
check('lit_neg_pos',  (-7 % 3)  == 2);
check('lit_pos_neg',  (7 % -3)  == -2);
check('lit_neg_neg',  (-7 % -3) == -1);

# ── Section 2: exact multiples (result should be zero regardless of sign) ──
check('exact_pos_pos', (6 % 3)   == 0);
check('exact_neg_pos', (-6 % 3)  == 0);
check('exact_pos_neg', (6 % -3)  == 0);
check('exact_neg_neg', (-6 % -3) == 0);

# ── Section 3: dividend zero ─────────────────────────────────────────────────
check('zero_dividend_pos', (0 % 5)  == 0);
check('zero_dividend_neg', (0 % -5) == 0);

# ── Section 4: modulus of 1/-1 (always zero) ────────────────────────────────
check('mod_by_one',     (7 % 1)   == 0);
check('mod_by_neg_one', (-7 % -1) == 0);

# ── Section 5: variables (not just literals) ────────────────────────────────
{
    my $a = -7;
    my $b = 3;
    check('var_neg_pos', ($a % $b) == 2);
}
{
    my $a = 7;
    my $b = -3;
    check('var_pos_neg', ($a % $b) == -2);
}

# ── Section 6: compound assignment %= (exercises the unboxed-int fast path
#    directly, a separate code site from plain `%`) ─────────────────────────
{
    my $x = -7;
    $x %= 3;
    check('compound_neg_pos', $x == 2);
}
{
    my $x = 10;
    $x %= -3;
    check('compound_pos_neg', $x == -2);
}
{
    my $x = -10;
    $x %= -3;
    check('compound_neg_neg', $x == -1);
}

# ── Section 7: repeated in a loop (fast-path re-execution / register reuse) ─
{
    my @results;
    for (my $i = -5; $i <= 5; $i++) {
        push @results, $i % 3;
    }
    check('loop_sequence', join(",", @results) eq "1,2,0,1,2,0,1,2,0,1,2");
}

# ── Section 8: large-magnitude operands ─────────────────────────────────────
check('large_neg', (-1000000007 % 97) == 87);
check('large_pos', (1000000007 % 97)  == 10);

# ── Section 9: float operands (Perl truncates to int before modding) ───────
check('float_pos_dividend', (7.5 % 3)   == 1);
check('float_neg_dividend', (-7.5 % 3)  == 2);
check('float_divisor',      (7 % 3.5)   == 1);

# ── Section 10: circular-index idiom (the practical motivating use case —
#    wraparound indexing with a negative offset) ────────────────────────────
{
    my @arr = (10, 20, 30, 40, 50);
    my $i = -1;
    check('circular_index', $arr[$i % scalar(@arr)] == 50);
}

# NOTE: a "modulus by zero should die catchably via eval" section was
# deliberately NOT added here. Found while writing this test: that's a
# separate, pre-existing, broken behavior unrelated to D71's sign-convention
# fix — logged as new TESTS.md D84, NOT fixed here. In short, (a) the
# unboxed-int fast path for `%`/`%=` (used inside any block/sub scope) has
# no zero-divisor check at all, so a zero divisor there is undefined
# behavior rather than a die; and (b) even the boxed perl_mod() path's
# existing zero check calls exit(1) directly instead of routing through
# perl_die()'s catchable longjmp mechanism (the same non-catchable-exit
# pattern D35 already fixed for Carp::croak, just not for this function).

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d71_modulo_negative_done\n";
