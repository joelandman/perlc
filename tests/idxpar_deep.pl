#!/usr/bin/env perl
# Closed index/rindex/substr deep: bound 13005 (>= PERLC_AUTOPAR_MIN)
# takes the pool-parallel map/grep bodies and must stay byte-identical
# to sequential real perl.
use strict;
use warnings;

my $N = 13005;
my @w;
for my $i (0 .. $N - 1) {
    if ($i % 3 == 0) { push @w, "apple$i" }
    elsif ($i % 3 == 1) { push @w, "pear$i" }
    else { push @w, "plum$i" }
}

my @g = grep { index($_, "app") >= 0 } @w;
print "glen=", scalar(@g), " g0=$g[0] gmid=$g[2000] gend=$g[-1]\n";

my @ri = grep { rindex($_, "ear") >= 0 } @w;
print "rlen=", scalar(@ri), " r0=$ri[0] rend=$ri[-1]\n";

my @pre = map { substr($_, 0, 4) } @w;
print "p0=$pre[0] p1=$pre[1] p2=$pre[2] pend=$pre[-1]\n";

my @ix = map { index($_, "l") } @w;
print "i0=$ix[0] i1=$ix[1] i2=$ix[2] iend=$ix[-1]\n";

# second fan-out (pool reuse)
my @g2 = grep { substr($_, 0, 1) eq "p" } @w;
print "g2len=", scalar(@g2), " g2end=$g2[-1]\n";

print "done\n";
