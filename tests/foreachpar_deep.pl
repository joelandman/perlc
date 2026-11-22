#!/usr/bin/env perl
# Stage 46 foreach-@arr deep: 13005 elements, pool-parallel write-back
# through the loop-var alias, byte-identical to sequential real perl.
use strict;
use warnings;

my $N = 13005;
my @a = (0 .. $N - 1);
foreach my $x (@a) { $x = $x * 2 + 1; }
print "a0=$a[0] a1=$a[1] amid=$a[6500] aend=$a[-1]\n";
my $sum = 0;
$sum += $a[$_] foreach 0, 9, 99, 999, 4999, 9999, 13004;
print "sum=$sum\n";

my @b = (0 .. $N - 1);
$_++ for @b;
print "b0=$b[0] bmid=$b[6500] bend=$b[-1]\n";

print "done\n";
