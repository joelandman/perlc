#!/usr/bin/env perl
# Stage 46 append-only foreach smoke: small N stays sequential; push into
# a different array matches perl. Same-array push stays sequential.
use strict;
use warnings;

my @a = (1 .. 30);
my @b;
foreach my $x (@a) { push @b, $x * 2; }
print "app-named: @b\n";

my @c = (1 .. 20);
my @d;
push @d, $_ + 1 for @c;
print "app-default: @d\n";

{
    my @src = (0 .. 14);
    my @dst;
    foreach my $i (0 .. 14) { push @dst, $i * 3; }
    print "app-range: @dst\n";
}

print "done\n";
