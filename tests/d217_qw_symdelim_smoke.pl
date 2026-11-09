#!/usr/bin/perl
# D217: qw with a same-character delimiter must stop at the first closer.
use strict;
use warnings;
my @a = qw/text TEXT/;
print join(",", @a), "\n";
my @b = qw//;
print "empty=", scalar(@b), "\n";
my @c = qw!a b!;
print join(",", @c), "\n";
my $n = qw/a/ . "X";
print "ax=$n\n";
print "tail\n";
