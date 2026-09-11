#!/usr/bin/perl
# Smoke test for D101: `each %hash` in scalar context must return the
# key, not the pair-array's element count.
use strict;
use warnings;

my %h = (only => 1);
my $k = each %h;
print "$k\n";
