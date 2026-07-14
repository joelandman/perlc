#!/usr/bin/perl
# Smoke test for D8a: `EXPR or return VALUE` parsed successfully but never
# actually returned — execution silently fell through to the next
# statement instead of exiting the sub.
# Fast, narrow coverage — see d8a_or_return.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

sub f {
    my ($val) = @_;
    $val or return "X";
    return "Y";
}
check('smoke_or_return_falsy', f(0) eq "X");
check('smoke_or_return_truthy', f(5) eq "Y");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d8a_or_return_smoke_done\n";
