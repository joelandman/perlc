#!/usr/bin/env perl
# Closed reverse deep: bound 13005 (>= PERLC_AUTOPAR_MIN) takes the
# pool-parallel map/grep bodies and must stay byte-identical to
# sequential real perl.
use strict;
use warnings;

my $N = 13005;
my @w;
for my $i (0 .. $N - 1) {
    if ($i % 3 == 0) { push @w, "apple$i" }
    elsif ($i % 3 == 1) { push @w, "pear$i" }
    else { push @w, "plum$i" }
}

my @id = map { reverse $_ } @w;
print "i0=$id[0] i1=$id[1] iend=$id[-1]\n";

my @sr = map { scalar reverse $_ } @w;
print "s0=$sr[0] s1=$sr[1] send=$sr[-1]\n";

my @g = grep { reverse $_ } @w;
print "glen=", scalar(@g), " g0=$g[0] gend=$g[-1]\n";

my @ch = map { reverse split // } @w;
print "clen=", scalar(@ch), " c0=$ch[0] cend=$ch[-1]\n";

print "done\n";
