#!/usr/bin/env perl
# Stage 46 append-only foreach deep: 13005 inputs, pool path, ordered join.
use strict;
use warnings;

my $N = 13005;
my @a = (0 .. $N - 1);
my @b;
foreach my $x (@a) { push @b, $x * 2 + 1; }
print "blen=", scalar(@b), " b0=$b[0] b1=$b[1] bmid=$b[6500] bend=$b[-1]\n";
my $sum = 0;
$sum += $b[$_] foreach 0, 9, 99, 999, 4999, 9999, 13004;
print "bsum=$sum\n";

my @c;
foreach my $i (0 .. $N - 1) { push @c, $i + 7; }
print "c0=$c[0] cmid=$c[6500] cend=$c[-1]\n";

print "done\n";
