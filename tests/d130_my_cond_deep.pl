#!/usr/bin/perl
# Deep test for D130: `if (my @arr = EXPR)` / `if (my %h = EXPR)` with
# no surrounding parens around the declared variable. Previously a
# hard parse error at the parser level (both the statement-context
# `my @arr = ...;` path and the expression-context `if (my $x = ...)`
# scalar path already worked; only the array/hash expression-context
# form was missing). Fix: parser.cpp gained a TK::ARRAY/TK::HASH
# branch alongside the existing scalar `my $x` expression-context
# case, and codegen.cpp's expression-context `case NK::My` gained
# array-length / hash-size return handling so the condition's
# truthiness is evaluated correctly (list-assignment count, same as
# real Perl: `my @a = LIST` in boolean context is the resulting
# array's element count).
#
# NB: a true `while (my @w = shift(@arr))` loop is deliberately not
# exercised here — that's a well-known, unrelated real-Perl gotcha:
# once @arr is exhausted, shift returns undef, and `my @w = undef` in
# list-assignment context yields a 1-element array `(undef)`, which is
# boolean-true, so the loop never terminates on its own in *real*
# Perl either. That's not a D130 discriminator.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

sub two_elems { return (1, 2); }
if (my @r = two_elems()) {
    check('array_cond_true', "@r" eq "1 2");
} else {
    check('array_cond_true', 0);
}

sub empty_list { return (); }
if (my @r2 = empty_list()) {
    check('array_cond_false', 0);
} else {
    check('array_cond_false', 1);
}

sub pairs { return (a => 1, b => 2); }
if (my %r3 = pairs()) {
    check('hash_cond_true', join(",", map { "$_=$r3{$_}" } sort keys %r3) eq "a=1,b=2");
} else {
    check('hash_cond_true', 0);
}

sub empty_pairs { return (); }
if (my %r4 = empty_pairs()) {
    check('hash_cond_false', 0);
} else {
    check('hash_cond_false', 1);
}

my @plain = (5, 6, 7);
if (my @copy = @plain) {
    check('array_cond_from_array', "@copy" eq "5 6 7");
} else {
    check('array_cond_from_array', 0);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d130_my_cond_done\n";
