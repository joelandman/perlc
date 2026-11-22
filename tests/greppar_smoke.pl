#!/usr/bin/env perl
# Boolean grep /pat/ (greppar) smoke: small N stays sequential (below
# PERLC_AUTOPAR_MIN) and matches real perl. Literal /pat/, /i, expr-form
# grep /pat/, LIST, !/pat/, and map { /pat/ }. Refused /g, s///, and
# outer write stay on the sequential fallback. Do not mention $1 / $&
# in this file — a capture read disables regex-in-par for the unit.
use strict;
use warnings;

my @w = ("a", "bb", "ccc", "d", "aa", "banana", "pear");

my @g = grep { /a/ } @w;
print "grep-a: @g\n";

my @gi = grep { /A/i } @w;
print "grep-i: @gi\n";

my @ge = grep /an/, @w;
print "grep-expr: @ge\n";

my @gn = grep { !/p/ } @w;
print "grep-not: @gn\n";

my @m = map { /a/ } @w;
print "map-bool: @m\n";

my @bound = grep { $_ =~ /b/ } @w;
print "grep-bind: @bound\n";

# refused (/g iterator) — sequential fallback must still match perl
my @gg = grep { /a/g } @w;
print "grep-g: @gg\n";

# refused (interpolated pattern uses the process LRU, not the memo slot)
my $pat = "an";
my @interp = grep { /$pat/ } @w;
print "grep-interp: @interp\n";

# refused (outer write)
my $n = 0;
my @acc = grep { $n++; /a/ } ("a", "b", "aa");
print "grep-acc: n=$n acc=@acc\n";

print "done\n";
