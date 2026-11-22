#!/usr/bin/env perl
# Closed join/sprintf/hex/oct (joinpar) smoke: small N stays sequential
# (below PERLC_AUTOPAR_MIN) and matches real perl. join of an outer
# named array stays on the sequential fallback.
use strict;
use warnings;

my @w = ("a", "bb", "ccc", "d");

my @j = map { join("-", $_, "x") } @w;
print "map-join: @j\n";

my @sp = map { sprintf("%s:%d", $_, length($_)) } @w;
print "map-sprintf: @sp\n";

my @g = grep { join("", $_, $_) eq "aa" } @w;
print "grep-join: @g\n";

my @hx = map { hex($_) } ("a", "10", "ff", "0");
print "map-hex: @hx\n";

my @oc = map { oct($_) } ("10", "20", "7", "0");
print "map-oct: @oc\n";

# refused (named outer array read) — sequential fallback must still match
my @outer = ("z", "y");
my @bad = map { join(",", @outer, $_) } @w;
print "join-outer: @bad\n";

print "done\n";
