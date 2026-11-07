#!/usr/bin/perl
# D203 (2026-11-07): packed rows in LIST POSITION are refs.
# `my @b = ([9,10,11])` keeps one ref element; interpolation/concat of the
# element shows the ARRAY-ref form (class only — addresses never compared);
# element views flatten through reads; list-assign + alias-write is coherent
# (the D207 share-backing contract).
use strict;
use warnings;

my @z3 = 1 ? [9, 9] : (3);
print "z3_count=", scalar(@z3), " z3_ref=", (ref($z3[0]) eq 'ARRAY' ? 'ARRAY' : 'NOT-ARRAY'), "\n";
print "z3_elem=", $z3[0][0], "\n";

my @e = ([11, 12], [13, 14]);
my @f = @e;
print "f_count=", scalar(@f), " f0_ref=", (ref($f[0]) eq 'ARRAY' ? 'ARRAY' : 'NOT-ARRAY'), " f0_elem=$f[0][1]\n";

my @g = @e;
$g[0][0] = 77;
print "alias_write: e00=$e[0][0] g00=$g[0][0]\n";

my $s = "@e";
print "interp_class=", ((substr($s, 0, 5) eq 'ARRAY') ? 'ARRAY' : "BAD"), "\n";
print "concat_class=", ((substr("" . $e[0], 0, 5) eq 'ARRAY') ? 'ARRAY' : 'BAD'), "\n";
print "defined_elem=", (defined $e[0] ? 1 : 0), "\n";

my @b = ([9, 10, 11]);
print "b_count=", scalar(@b), " b0_elem=$b[0][2]\n";
my $bs = "@b";
print "b_interp_class=", ((substr($bs, 0, 5) eq 'ARRAY') ? 'ARRAY' : "BAD:$bs"), "\n";
