#!/usr/bin/env perl
# Closed unpack (unpackpar) smoke: small N stays sequential (below
# PERLC_AUTOPAR_MIN) and matches real perl. An outer format scalar stays
# on the sequential fallback. Print numeric unpack fields, not packed
# binary (NULs make the harness treat output as binary).
use strict;
use warnings;

my @w = ("AB", "C", "XYZ", "");

my @u = map { unpack("C*", $_) } @w;
print "map-C: @u\n";

my @a = map { unpack("A*", $_) } @w;
print "map-A: @a\n";

my @g = grep { unpack("A*", $_) } @w;
print "grep-A: @g\n";

my @rt = map { unpack("C", pack("C", length($_))) } @w;
print "roundtrip: @rt\n";

# refused (outer scalar read) — sequential fallback must still match
my $fmt = "C*";
my @bad = map { unpack($fmt, $_) } ("AB", "C");
print "unpack-outer: @bad\n";

print "done\n";
