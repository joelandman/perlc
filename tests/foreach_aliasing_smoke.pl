#!/usr/bin/perl
# Smoke test for foreach loop-variable aliasing (D37: perlc's foreach used
# to copy each array element into the loop var instead of aliasing it, so
# `foreach (@arr) { $_ *= 2 }` silently left @arr unchanged).
# Fast, narrow coverage — see foreach_aliasing.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# Implicit $_ aliasing.
{
    my @arr = (1, 2, 3, 4);
    foreach (@arr) { $_ *= 2; }
    check('smoke_underscore_alias', join(",", @arr) eq "2,4,6,8");
}

# Named loop-variable aliasing.
{
    my @arr = (1, 2, 3);
    foreach my $x (@arr) { $x += 10; }
    check('smoke_named_var_alias', join(",", @arr) eq "11,12,13");
}

# Reading (not mutating) still works — regression check.
{
    my @arr = (1, 2, 3);
    my $sum = 0;
    foreach my $x (@arr) { $sum += $x; }
    check('smoke_read_only_regression', $sum == 6 && join(",", @arr) eq "1,2,3");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "foreach_aliasing_smoke_done\n";
