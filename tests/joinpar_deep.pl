#!/usr/bin/env perl
# Closed join/sprintf/hex/oct deep: bound 13005 (>= PERLC_AUTOPAR_MIN)
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

my @j = map { join(":", $_, "x") } @w;
print "j0=$j[0] j1=$j[1] jmid=$j[6500] jend=$j[-1]\n";

my @sp = map { sprintf("%s/%d", substr($_, 0, 1), length($_)) } @w;
print "s0=$sp[0] s1=$sp[1] send=$sp[-1]\n";

my @g = grep { join("", substr($_, 0, 3)) eq "app" } @w;
print "glen=", scalar(@g), " g0=$g[0] gend=$g[-1]\n";

my @hx = map { hex(sprintf("%x", $_)) } (0 .. $N - 1);
print "h0=$hx[0] h1=$hx[1] hend=$hx[-1]\n";

my @oc = map { oct(sprintf("%o", $_ % 8)) } (0 .. $N - 1);
print "o0=$oc[0] o7=$oc[7] oend=$oc[-1]\n";

print "done\n";
