#!/usr/bin/perl
# Smoke test for D100: a list assignment used as a while/if condition
# (`while (my ($k,$v) = each %h)`) always evaluated false — the loop body
# never ran, no matter what the RHS produced.
use strict;
use warnings;

my %h = (a => 1, b => 2, c => 3);
my $n = 0;
while (my ($k, $v) = each %h) { $n++; }
print "n=$n\n";
print(($n == 3) ? "ok\n" : "FAIL\n");
