#!/usr/bin/env perl
# Closed unpack deep: bound 13005 (>= PERLC_AUTOPAR_MIN) takes the
# pool-parallel map/grep bodies and must stay byte-identical to
# sequential real perl.
use strict;
use warnings;

my $N = 13005;
my @w;
for my $i (0 .. $N - 1) {
    my $a = chr(65 + ($i % 26));
    my $b = chr(97 + ($i % 26));
    if ($i % 3 == 0) { push @w, $a . $b }
    elsif ($i % 3 == 1) { push @w, $a }
    else { push @w, $a . $b . $a }
}

my @u = map { unpack("C*", $_) } @w;
print "ulen=", scalar(@u), " u0=$u[0] u1=$u[1] uend=$u[-1]\n";

my @a = map { unpack("A*", $_) } @w;
print "alen=", scalar(@a), " a0=$a[0] aend=$a[-1]\n";

my @g = grep { unpack("A*", $_) ne "" } @w;
print "glen=", scalar(@g), " g0=$g[0] gend=$g[-1]\n";

my @rt = map { unpack("C", pack("C", length($_))) } @w;
print "r0=$rt[0] rend=$rt[-1]\n";

print "done\n";
