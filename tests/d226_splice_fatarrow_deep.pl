#!/usr/bin/perl
# D226: ordinary splice, fat-arrow replacement list, and a nested
# splice that returns the children of an element.
use strict;
use warnings;
my @a = qw(a b c d);
my @gone = splice @a, 1, 2, "X", "Y";
print "a=@a\n";
print "gone=@gone\n";
my @tree = ("root", ["kid", {}, "c1", "c2"]);
my $i = 1;
splice(@tree, $i, 1 => splice @{$tree[$i]}, 2);
print "tree=@tree\n";
print "n=", scalar(@tree), "\n";
