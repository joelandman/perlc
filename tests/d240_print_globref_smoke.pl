#!/usr/bin/perl
# D240: print { \*STDOUT } and a hash-stored copy of that glob ref
# write to the handle. Pod::Text's output_fh is this shape.
use strict;
use warnings;
my $fh = \*STDOUT;
print { $fh } "brace\n";
print { \*STDOUT } "direct\n";
my $h = { output_fh => \*STDOUT };
print { $h->{output_fh} } "hello\n";
print { \*STDERR } "errline\n";
