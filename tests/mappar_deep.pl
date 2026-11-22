#!/usr/bin/env perl
# Stage 46 (mappar) deep: bound 13005 (>= default PERLC_AUTOPAR_MIN 8192)
# takes the pool-parallel map/grep bodies and must stay byte-identical to
# sequential real perl, including odd chunk splits and $_ write-back.
use strict;
use warnings;

my $N = 13005;
my @a = (0 .. $N - 1);

my @m = map { $_ * 3 + 7 } @a;
print "m0=$m[0] m1=$m[1] mid=$m[6500] end=$m[-1]\n";
my $msum = 0;
$msum += $m[$_] foreach 0, 9, 99, 999, 4999, 9999, 13004;
print "msum=$msum\n";

my @src = (0 .. $N - 1);
my @inc = map { $_++ } @src;
print "back0=$src[0] backmid=$src[6500] backend=$src[-1]\n";
print "inc0=$inc[0] incid=$inc[6500] incend=$inc[-1]\n";

my @g = grep { ($_ & 1) == 0 } @a;
print "glen=", scalar(@g), " g0=$g[0] g1=$g[1] gend=$g[-1]\n";
my $gsum = 0;
$gsum += $g[$_] foreach 0, 10, 100, 1000, 6500;
print "gsum=$gsum\n";

# a second fan-out on a different closed body (pool reuse)
my @q = map { my $x = $_; $x * $x } @a;
print "q0=$q[0] qmid=$q[6500] qend=$q[-1]\n";

print "done\n";
