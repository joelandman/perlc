#!/usr/bin/perl
use strict;
use warnings;
use MathUtils;

my $m = MathUtils->new();

print $m->add(3, 4) . "\n";        # 7
print $m->multiply(6, 7) . "\n";   # 42
print $m->factorial(5) . "\n";     # 120
print $m->factorial(1) . "\n";     # 1
print $m->add(100, 200) . "\n";    # 300
print ref($m) . "\n";              # MathUtils
