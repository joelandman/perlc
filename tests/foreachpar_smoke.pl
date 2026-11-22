#!/usr/bin/env perl
# Stage 46 foreach-@arr smoke: small arrays stay sequential; write-back
# through the loop var matches real perl, including a refused outer write.
use strict;
use warnings;

my @a = (1 .. 30);
foreach my $x (@a) { $x += 10; }
print "fe-named: @a\n";

my @b = (1 .. 20);
$_ *= 2 for @b;
print "fe-default: @b\n";

my $s = 0;
my @c = (1, 2, 3, 4, 5);
foreach my $x (@c) { $s += $x; }
print "fe-acc: s=$s c=@c\n";

print "done\n";
