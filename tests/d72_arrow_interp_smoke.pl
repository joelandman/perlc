#!/usr/bin/perl
# Smoke test for D72: "$ref->{key}" / "$ref->[i]" (arrow dereference) did
# not interpolate inside double-quoted strings at all — the interpolation
# scanner had no notion of "->" followed by a subscript, so it printed the
# ref's raw stringification (or garbage) instead of the dereferenced value.
# Fast, narrow coverage — see d72_arrow_interp.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my $href = {a => 1};
my $s1 = "elem: $href->{a}";
check('smoke_hash_arrow', $s1 eq "elem: 1");

my $aref = [10, 20, 30];
my $s2 = "elem: $aref->[1]";
check('smoke_array_arrow', $s2 eq "elem: 20");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d72_arrow_interp_smoke_done\n";
