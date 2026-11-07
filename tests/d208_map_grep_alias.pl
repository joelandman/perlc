#!/usr/bin/perl
# D208 (2026-11-07): map/grep/any/all/none alias $_ to the SOURCE element
# cell (raw list semantics); mutations through $_ write back, including
# short-circuit element counts (any stops at the first true).
use strict; use warnings;
use List::Util qw(any all none);

my @a = (1, 2, 3);
my @b = map { $_++ } @a;
print "map-back: a=@a b=@b\n";

my @h = ("x", "y");
my @g = grep { $_ .= "!" } @h;
print "grep-back: h=@h g=@g\n";

my @row = ([5, 6], [7, 8]);
my @m = map { $_->[0] * 2 } @row;
print "map-rowview: @m\n";
map { $_->[0] += 1 } @row;
print "map-rowwrite: row0=$row[0][0]\n";

my @q = (1, 2, 3);
my $hit = any { $_ += 10; $_ > 11 } @q;
print "any-back: q=@q hit=", ($hit ? 1 : 0), "\n";

my @w = (5, 6);
my $t = all { $_++ } @w;
print "all-back: t=", ($t ? 1 : 0), " w=@w\n";

my @v = (7, 8);
my $n = none { $_ += 100; 1 } @v;
print "none-back: n=", ($n ? "T" : "F"), " v=@v\n";
