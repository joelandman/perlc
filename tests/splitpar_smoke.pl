#!/usr/bin/env perl
# Closed split/pack (splitpar) smoke: small N stays sequential
# (below PERLC_AUTOPAR_MIN) and matches real perl. pack of an outer
# named array stays on the sequential fallback. Do not mention $1 / $&
# in this file — a capture read disables regex-split-in-par for the unit.
use strict;
use warnings;

my @w = ("a:b:c", "", "p:q", "z:");

my @s = map { split /:/ } @w;
print "map-rsplit: @s\n";

my @c = map { split ',', $_ } ("a,b", "c", "d,e,f");
print "map-ssplit: @c\n";

my @g = grep { split /:/ } @w;
print "grep-split: @g\n";

my @p = map { pack("C", length($_)) } @w;
print "map-pack: ", join(" ", map { ord($_) } @p), "\n";

my @lim = map { split /:/, $_, 2 } @w;
print "map-limit: @lim\n";

my @cap = map { split /(:)/ } ("a:b", "x");
print "map-cap: @cap\n";

# refused (named outer array read) — sequential fallback must still match
my @outer = (1, 2);
my @bad = map { pack("C*", @outer, length($_)) } ("aa", "bbb");
print "pack-outer:";
for my $s (@bad) {
    print " ";
    for my $i (0 .. length($s) - 1) {
        print ord(substr($s, $i, 1));
        print "." if $i + 1 < length($s);
    }
}
print "\n";

print "done\n";
