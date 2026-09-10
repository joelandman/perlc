#!/usr/bin/perl
# Smoke test for D114: array slices with a non-literal subscript list
# (a range or an array variable) must return the full slice, not just
# one element.
use strict;
use warnings;

my @x = (10, 20, 30, 40, 50);
my @s1 = @x[1..2];
print "@s1\n";
my @i = (0, 2);
my @s2 = @x[@i];
print "@s2\n";
