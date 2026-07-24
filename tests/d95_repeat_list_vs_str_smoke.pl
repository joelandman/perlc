#!/usr/bin/perl
# Smoke: D95 — bare EXPR x N is string repetition even in array assign.
use strict;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

sub f { "x" }
my @a = (f() x 2);
check('bare_call_str_rep', scalar(@a) == 1 && $a[0] eq "xx");
my @b = ((f()) x 2);
check('paren_call_list_rep', scalar(@b) == 2 && join(",", @b) eq "x,x");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d95_repeat_list_vs_str_smoke_done\n";
