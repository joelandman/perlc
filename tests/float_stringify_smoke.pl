#!/usr/bin/perl
# Smoke test for default float stringification (D31: perlc used C's
# default %g format, 6 significant digits, instead of Perl's %.15g —
# `10/3` printed "3.33333" instead of "3.33333333333333").
# Fast, narrow coverage — see float_stringify.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: a repeating-decimal division result.
{
    my $x = 10 / 3;
    check('smoke_full_precision', $x eq "3.33333333333333");
}

# Regression: short/exact floats are unaffected.
{
    my $x = 0.1;
    check('smoke_short_float_unaffected', $x eq "0.1");
}

# Special value: negative zero prints as plain "0", not "-0".
{
    my $x = 0.0 * -1;
    check('smoke_negative_zero', $x eq "0");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "float_stringify_smoke_done\n";
