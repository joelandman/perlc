#!/usr/bin/perl
# D240: say to a glob ref, a list of arguments, and a scalar that holds
# the stdout handle directly (open-style) still print.
use strict;
use warnings;
my $fh = \*STDOUT;
say { $fh } "sayline";
print { $fh } "a", "b", "c\n";
my $out = \*STDOUT;
print { $out } "out\n";
print { \*STDERR } "e1\n";
print { \*STDERR } "e2";
