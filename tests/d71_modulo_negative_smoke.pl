#!/usr/bin/perl
# Smoke test for D71: `%` (modulo) used C's truncating semantics instead of
# Perl's floored-division convention, giving the wrong result whenever
# either operand was negative.
# Fast, narrow coverage — see d71_modulo_negative.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

check('smoke_neg_left',  (-7 % 3) == 2);
check('smoke_neg_right', (7 % -3) == -2);
check('smoke_pos_pos_regression', (7 % 3) == 1);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d71_modulo_negative_smoke_done\n";
