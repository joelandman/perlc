#!/usr/bin/perl
# Smoke test for D52: `$@ = "..."` was a silent no-op — $@ wasn't wired up
# as an assignment target at all, so clearing a caught exception did nothing.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

eval { die "boom\n" };
$@ = "";
check('smoke_dollar_at_clears', $@ eq "");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d52_error_var_assign_smoke_done\n";
