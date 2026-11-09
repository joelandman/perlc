#!/usr/bin/perl
# D226: splice accepts => as a comma, including a nested splice.
use strict;
use warnings;
my @row = ("h", {}, "c1", "c2");
my @t = (\@row);
my $i = 0;
splice(@t, $i, 1
  => splice @{ $t[$i] }, 2
);
print join("|", @t), "\n";
print $row[0], " ", scalar(@row), "\n";
