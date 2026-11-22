#!/usr/bin/env perl
# Stage 46 map-concat smoke: small-N list-yielding map stays sequential.
use strict;
use warnings;

my @a = (1 .. 20);
my @m = map { ($_, $_ + 1) } @a;
print "pairs: @m\n";

my @z = map { () } (1 .. 5);
print "empty: n=", scalar(@z), "\n";

my @aoa = ([1, 2], [3], [4, 5, 6]);
my @flat = map { @$_ } @aoa;
print "flat: @flat\n";

my @one = map { ($_) } (10, 20, 30);
print "paren1: @one\n";

print "done\n";
