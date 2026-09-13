#!/usr/bin/perl
# Smoke test for D106: reading a FLAT_ARRAY/FLOAT_PAIR anon-array-ref
# back out of an array (or hash) element and aliasing it a second time
# must see further writes, same as D105 already fixed for a plain
# scalar variable.
use strict;
use warnings;

my @arr = ([1, 2, 3]);
my $y = $arr[0];
$y->[0] = 99;
print "$arr[0][0]\n";
