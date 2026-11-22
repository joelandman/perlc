#!/usr/bin/env perl
# Closed index/rindex/substr (idxpar) smoke: small N stays sequential
# (below PERLC_AUTOPAR_MIN) and matches real perl. 4-arg and lvalue
# substr stay on the sequential fallback.
use strict;
use warnings;

my @w = ("apple", "pear", "plum", "banana", "fig");

my @ix = map { index($_, "a") } @w;
print "map-index: @ix\n";

my @g = grep { index($_, "p") >= 0 } @w;
print "grep-index: @g\n";

my @ri = map { rindex($_, "a") } @w;
print "map-rindex: @ri\n";

my @pre = map { substr($_, 0, 2) } @w;
print "map-substr: @pre\n";

my @tail = grep { substr($_, -2) eq "um" } @w;
print "grep-substr: @tail\n";

# refused (4-arg in-place replace) — sequential fallback must still match
my @src = ("aa", "bb", "cc");
my @rep = map { substr($_, 0, 1, "z"); $_ } @src;
print "substr4: @rep src=@src\n";

# refused (lvalue substr)
my @lv = ("xy", "zz");
my @asg = map { substr($_, 0, 1) = "Q"; $_ } @lv;
print "substr-lv: @asg lv=@lv\n";

print "done\n";
