#!/usr/bin/perl
# Smoke test: split() must not truncate at an embedded NUL byte.
use strict;
use warnings;

my $s = "a\0b:c\0d";
my @f = split(/:/, $s);
print scalar(@f), "\n";
print length($f[0]), " ", length($f[1]), "\n";
