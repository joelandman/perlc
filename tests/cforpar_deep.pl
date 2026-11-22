#!/usr/bin/env perl
# Boxed closed C-style for deep: 13005 iterations (>= PERLC_AUTOPAR_MIN)
# disjoint $a[$i] / $b[$i] stores, inclusive bound, second fan-out.
# Byte-identical to sequential real perl.
use strict;
use warnings;

my $N = 13005;
my @a = (0) x $N;
for (my $i = 0; $i < $N; $i++) { $a[$i] = $i * 2 + 1; }
print "a0=$a[0] a1=$a[1] amid=$a[6500] aend=$a[-1]\n";
my $asum = 0;
$asum += $a[$_] foreach 0, 9, 99, 999, 4999, 9999, 13004;
print "asum=$asum\n";

my @b = (5) x $N;
for (my $i = 0; $i < $N; $i++) { $b[$i] += $i; }
print "b0=$b[0] bmid=$b[6500] bend=$b[-1]\n";

my @c = (0) x $N;
my @d = (0) x $N;
for (my $i = 0; $i < $N; ++$i) { $c[$i] = $i; $d[$i] = $i + 3; }
print "c0=$c[0] dmid=$d[6500] dend=$d[-1]\n";

print "done\n";
