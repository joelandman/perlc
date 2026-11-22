#!/usr/bin/env perl
# Boxed closed C-style for (cforpar) smoke: small N stays sequential
# (below PERLC_AUTOPAR_MIN). Disjoint $a[$i] stores, inclusive <=,
# $i += 1, refused overlapping index, refused $i++ then store.
use strict;
use warnings;

my @a = (0) x 30;
for (my $i = 0; $i < 30; $i++) { $a[$i] = $i * 2 + 1; }
print "cfor-store: @a\n";

my @b = (0) x 20;
for (my $i = 0; $i <= 19; $i++) { $b[$i] += 10; }
print "cfor-add: @b\n";

my @c = (0) x 15;
my @d = (0) x 15;
for (my $i = 0; $i < 15; $i += 1) { $c[$i] = $i; $d[$i] = $i + 1; }
print "cfor-two: c=@c d=@d\n";

my @g = (0) x 10;
for (my $i = 0; $i < 10; ++$i) { $g[$i] = $i; }
print "cfor-pre: @g\n";

# refused ($a[$i+1]) — sequential fallback must still match perl
my @e = (0) x 10;
for (my $i = 0; $i < 9; $i++) { $e[$i + 1] = $i; }
print "cfor-off: @e\n";

# refused ($i++ then $a[$i]) — overlapping stores stay sequential
my @f = (0) x 10;
for (my $i = 0; $i < 9; $i++) { $i++; $f[$i] = 1; }
print "cfor-inc: @f\n";

# foreach-range still works — regression
my @h = (0) x 10;
foreach my $i (0 .. 9) { $h[$i] = $i + 3; }
print "foreach-reg: @h\n";

print "done\n";
