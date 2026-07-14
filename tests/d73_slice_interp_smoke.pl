#!/usr/bin/perl
# Smoke test for D73: array/hash slice interpolation inside double-quoted
# strings ("@arr[1,2]", "@h{'a','b'}") produced garbage — the whole-array
# form's own scan never looked for a following [ or {, so it interpolated
# the entire array/an empty match and left the slice syntax as literal text.
# Fast, narrow coverage — see d73_slice_interp.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my @arr = (10, 20, 30, 40);
check('smoke_array_slice', "@arr[1,2]" eq "20 30");

my %h = (a => 1, b => 2);
check('smoke_hash_slice', "@h{'a','b'}" eq "1 2");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d73_slice_interp_smoke_done\n";
