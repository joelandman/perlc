#!/usr/bin/env perl
use strict;
use warnings;
use List::Util qw(sum min max first any all none uniq reduce);

# rand / srand
srand(42);
my $r = rand(10);
print "rand ok\n" if $r >= 0 && $r < 10;

# time
my $t = time();
print "time ok\n" if $t > 1000000000;

# localtime — 9-element list
my @lt = localtime($t);
print "localtime ok\n" if scalar(@lt) == 9;

# sleep(0) — no-op but must compile and run
my $slept = sleep(0);
print "sleep ok\n" if defined($slept);

# sum
my $s = sum(1, 2, 3, 4, 5);
print "sum=$s\n";

# min / max
my $mn = min(3, 1, 4, 1, 5, 9, 2, 6);
my $mx = max(3, 1, 4, 1, 5, 9, 2, 6);
print "min=$mn\n";
print "max=$mx\n";

# uniq — removes consecutive duplicates
my @u = uniq(1, 1, 2, 3, 3, 3, 4);
print "uniq=", join(",", @u), "\n";

# first
my @data = (1, 2, 3, 4, 5);
my $f = first { $_ > 3 } @data;
print "first=$f\n";

# any / all / none
my $a = any { $_ > 3 } @data;
print "any=", ($a ? "yes" : "no"), "\n";

my $all = all { $_ > 0 } @data;
print "all=", ($all ? "yes" : "no"), "\n";

my $none = none { $_ > 10 } @data;
print "none=", ($none ? "yes" : "no"), "\n";

# reduce
my $prod = reduce { $a * $b } @data;
print "reduce=$prod\n";

# sort with custom block — sort by string length
my @words = ("banana", "apple", "cherry", "date");
my @sorted = sort { length($a) <=> length($b) } @words;
print "sort_len=", join(",", @sorted), "\n";

# sort with custom block — numeric descending
my @nums = (5, 3, 8, 1, 4);
my @nsorted = sort { $b <=> $a } @nums;
print "sort_desc=", join(",", @nsorted), "\n";
