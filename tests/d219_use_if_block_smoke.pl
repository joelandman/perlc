#!/usr/bin/perl
# D219: `use if do { ... }, MODULE` is one statement. The condition is false.
use strict;
use warnings;
use if do { no integer; "$]" <= 5.006002 }, 'utf8';
my $x = 3;
print "x=$x\n";
print "ok\n";
