#!/usr/bin/perl
# D221: `=>` is a comma in a join argument list.
use strict;
use warnings;
my $s = join '' => "aa", "bb", "cc";
print "[$s]\n";
my $t = join "-", "x", "y";
print "[$t]\n";
