#!/usr/bin/perl
# D233: => is a comma in push and unshift.
use strict;
use warnings;
my @args;
push @args, alt => 1;
push (@args, width => 80);
push @args, "z",;
print join(",", @args), "\n";
my @u = (3, 4);
unshift @u, flag => "x";
unshift (@u, "p");
print join(",", @u), "\n";
