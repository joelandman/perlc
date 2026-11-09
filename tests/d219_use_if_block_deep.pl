#!/usr/bin/perl
# D219: use-if with a do-block condition, a constant false, and a following statement.
use strict;
use warnings;
use if 0, 'utf8';
use if do { no integer; 1 == 0 }, 'utf8';
use if do { no integer; "$]" <= 5.006002 }, 'utf8';
my $y = 4;
print "y=$y\n";
my $z = do { no integer; 2 + 2 };
print "z=$z\n";
print "tail\n";
