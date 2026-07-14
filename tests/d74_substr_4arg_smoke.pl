#!/usr/bin/perl
# Smoke test for D74: `substr($str, $off, $len, $replacement)` (the 4-arg
# in-place replacement form) was a silent no-op — the parser already
# accepted up to 4 arguments, but codegen had no case for args.size()>=4,
# so the call behaved exactly like the 3-arg read-only form and $str was
# left completely unchanged.
# Fast, narrow coverage — see d74_substr_4arg.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my $u = "Hello";
substr($u, 1, 3, "XYZ");
check('smoke_4arg_replaces_in_place', $u eq "HXYZo");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d74_substr_4arg_smoke_done\n";
