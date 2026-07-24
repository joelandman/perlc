#!/usr/bin/perl
# Smoke: D81 — delete @hash{...} / delete @arr[...] slice forms.
use strict;

my %h = (a => 1, b => 2, c => 3);
delete @h{qw(a b)};
print "keys=", join(",", sort keys %h), "\n";

my @a = (10, 20, 30, 40);
my @d = delete @a[1, 3];
print "del=", join(",", @d), " n=", scalar(@a), "\n";
print "d81_delete_slice_smoke_done\n";
