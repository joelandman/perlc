#!/usr/bin/env perl
# Stage 46 (mappar) smoke: closed 1:1 map/grep with small N stay on the
# sequential body (below PERLC_AUTOPAR_MIN) and match real perl, including
# shapes the purity walk must refuse (outer accumulator, nested write).
use strict;
use warnings;

my @a = (1 .. 40);
my @m = map { $_ * 2 + 1 } @a;
print "map-arith: @m\n";

my @b = map { my $x = $_ + 3; $x * $x } @a;
print "map-local: @b\n";

my @src = (10, 20, 30, 40);
my @inc = map { $_++ } @src;
print "map-back: src=@src inc=@inc\n";

my @g = grep { ($_ & 1) == 0 } (1 .. 20);
print "grep-even: @g\n";

my @w = ("a", "bb", "ccc", "d");
my @gw = grep { length($_) > 1 } @w;
print "grep-len: @gw\n";

# refused (outer write) — must still match perl on the sequential path
my $s = 0;
my @acc = map { $s += $_; $_ } (1, 2, 3, 4);
print "map-acc: s=$s acc=@acc\n";

# refused (nested store through a possibly-shared referent)
my @row = ([1, 2], [3, 4]);
my @rw = map { $_->[0] += 1; $_->[0] } @row;
print "map-nested: rw=@rw r00=$row[0][0]\n";

print "done\n";
