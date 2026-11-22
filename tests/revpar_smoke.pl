#!/usr/bin/env perl
# Closed reverse (revpar) smoke: small N stays sequential (below
# PERLC_AUTOPAR_MIN) and matches real perl. List reverse of $_ is a
# 1-element identity; scalar reverse is the string. reverse of an outer
# named array stays on the sequential fallback.
use strict;
use warnings;

my @w = ("ab", "c", "xyz", "");

my @id = map { reverse $_ } @w;
print "map-list: @id\n";

my @sr = map { scalar reverse $_ } @w;
print "map-scalar: @sr\n";

my @g = grep { reverse $_ } @w;
print "grep-rev: @g\n";

my @ch = map { reverse split // } @w;
print "map-chars: @ch\n";

# refused (named outer array read) — sequential fallback must still match
my @outer = ("z", "y");
my @bad = map { reverse @outer, $_ } ("ab", "cd");
print "rev-outer: @bad\n";

print "done\n";
