#!/usr/bin/perl
# Smoke test for D105: FLAT_ARRAY/FLOAT_PAIR (Stage 22/23 compact anon
# array-ref literals like [1,2] or [1,2,3]) were deep-copied on every
# clone instead of preserving reference identity — writes through one
# alias were invisible through another, even though the aliases compared
# `==` equal.
use strict;
use warnings;

my $inner = [1, 2];
my @m = ($inner, [3, 4]);
$inner->[0] = 55;
print "m0_0=$m[0][0]\n";
print(($m[0][0] == 55) ? "ok\n" : "FAIL\n");
