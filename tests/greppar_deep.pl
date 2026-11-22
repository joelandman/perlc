#!/usr/bin/env perl
# Boolean grep /pat/ deep: bound 13005 (>= default PERLC_AUTOPAR_MIN 8192)
# takes the pool-parallel grep bodies and must stay byte-identical
# to sequential real perl. Do not mention $1 / $& in this file.
use strict;
use warnings;

my $N = 13005;
my @w;
for my $i (0 .. $N - 1) {
    if ($i % 3 == 0) { push @w, "apple$i" }
    elsif ($i % 3 == 1) { push @w, "pear$i" }
    else { push @w, "plum$i" }
}

my @g = grep { /app/ } @w;
print "glen=", scalar(@g), " g0=$g[0] gmid=$g[2000] gend=$g[-1]\n";

my @gi = grep { /PEAR/i } @w;
print "ilen=", scalar(@gi), " i0=$gi[0] iend=$gi[-1]\n";

my @ge = grep /plum/, @w;
print "elen=", scalar(@ge), " e0=$ge[0] eend=$ge[-1]\n";

my @gn = grep { !/p/ } @w;
print "nlen=", scalar(@gn), "\n";

my @allp = grep { /p/ } @w;
print "plen=", scalar(@allp), "\n";

# map { /pat/ } is list-context captures (empty on failure), sequential
my @m = map { /app/ } @w;
print "mlen=", scalar(@m), " m0=$m[0] mend=$m[-1]\n";

# second fan-out (pool reuse)
my @g2 = grep { /le/ } @w;
print "g2len=", scalar(@g2), " g2end=$g2[-1]\n";

print "done\n";
