#!/usr/bin/perl
# Smoke test for D118: split() didn't support a 3rd LIMIT argument
# (hard parse error) and didn't trim trailing empty fields (real
# Perl's default behavior).
use strict;
use warnings;

my @a = split(/:/, "a:b:c:d", 2);
print "@a\n";
my @b = split(/,/, "a,b,,");
print scalar(@b), ":@b\n";
