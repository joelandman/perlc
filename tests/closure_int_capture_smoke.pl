#!/usr/bin/perl
# Smoke: my $n = 0 captured by closure must share the cell (not a snapshot).
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

{
    my $n = 0;
    my $inc = sub { $n++ };
    $inc->();
    $inc->();
    check('from_inside', $n == 2);
    $n = 10;
    $inc->();
    check('from_outside', $n == 11);
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "closure_int_capture_smoke_done\n";
