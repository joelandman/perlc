#!/usr/bin/perl
# Smoke test for D111: `my %c = %h;` must copy %h's contents, not
# collapse to a single bogus key/undef pair (or alias %h's storage).
use strict;
use warnings;

my %h = (a => 1, b => 2);
my %c = %h;
$c{a} = 99;
for my $k (sort keys %c) { print "$k=$c{$k}\n"; }
print "h_a_unchanged=", ($h{a} == 1 ? "ok" : "FAIL"), "\n";
