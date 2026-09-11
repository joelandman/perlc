#!/usr/bin/perl
# Smoke test for D119: scalar(keys %$href) returned 0 instead of the
# key count (list-context keys %$href was already correct).
use strict;
use warnings;

my %h = (a => 1, b => 2, c => 3);
my $href = \%h;
print scalar(keys %$href), "\n";
