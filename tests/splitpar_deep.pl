#!/usr/bin/env perl
# Closed split/pack deep: bound 13005 (>= PERLC_AUTOPAR_MIN) takes the
# pool-parallel map/grep bodies and must stay byte-identical to
# sequential real perl. Do not mention $1 / $& here.
use strict;
use warnings;

my $N = 13005;
my @w;
for my $i (0 .. $N - 1) {
    if ($i % 3 == 0) { push @w, "apple:$i:x" }
    elsif ($i % 3 == 1) { push @w, "pear$i" }
    else { push @w, "plum:$i" }
}

my @s = map { split /:/ } @w;
print "slen=", scalar(@s), " s0=$s[0] s1=$s[1] send=$s[-1]\n";

my @c = map { split ',', $_ } map { "$_,x" } @w;
print "clen=", scalar(@c), " c0=$c[0] cend=$c[-1]\n";

my @g = grep { split(/:/) > 1 } @w;
print "glen=", scalar(@g), " g0=$g[0] gend=$g[-1]\n";

my @p = map { pack("C", length($_)) } @w;
print "p0=", ord($p[0]), " pend=", ord($p[-1]), "\n";

my @lim = map { split /:/, $_, 2 } @w;
print "llen=", scalar(@lim), " l0=$lim[0] lend=$lim[-1]\n";

print "done\n";
