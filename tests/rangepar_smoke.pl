#!/usr/bin/env perl
# Stage 46 range-foreach smoke: small N stays sequential; disjoint $a[$i]
# stores, refused overlapping index, refused $i++ then store.
use strict;
use warnings;

my @a = (0) x 30;
foreach my $i (0 .. 29) { $a[$i] = $i * 2 + 1; }
print "range-store: @a\n";

my @b = (0) x 20;
foreach my $i (0 .. 19) { $b[$i] += 10; }
print "range-add: @b\n";

my @c = (0) x 15;
my @d = (0) x 15;
foreach my $i (0 .. 14) { $c[$i] = $i; $d[$i] = $i + 1; }
print "range-two: c=@c d=@d\n";

# refused ($a[$i+1]) — sequential fallback must still match perl
my @e = (0) x 10;
foreach my $i (0 .. 8) { $e[$i + 1] = $i; }
print "range-off: @e\n";

# refused ($i++ then $a[$i]) — overlapping stores stay sequential
my @f = (0) x 10;
foreach my $i (0 .. 8) { $i++; $f[$i] = 1; }
print "range-inc: @f\n";

print "done\n";
