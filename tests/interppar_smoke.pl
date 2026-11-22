#!/usr/bin/env perl
# Closed interp/qr match (interppar) smoke: small N stays sequential
# (below PERLC_AUTOPAR_MIN) and matches real perl. A body-local /$p/
# and $_ =~ $re are eligible; an outer $pat and /g stay on the
# sequential fallback. Do not mention $1 / $& in this file.
use strict;
use warnings;

my @w = ("a", "bb", "ccc", "d", "aa", "banana", "pear");

my @g = grep { my $p = "a"; /$p/ } @w;
print "grep-interp: @g\n";

my @gi = grep { my $p = "A"; /$p/i } @w;
print "grep-i: @gi\n";

my @bind = grep { my $p = "an"; $_ =~ /$p/ } @w;
print "grep-bind: @bind\n";

my @qr = grep { my $re = qr/aa/; $_ =~ $re } @w;
print "grep-qr: @qr\n";

my @from = grep { my $p = substr($_, 0, 1); /$p/ } @w;
print "grep-sub: @from\n";

# map interp match is list-context captures — sequential fallback
my @m = map { my $p = "a"; /$p/ } @w;
print "map-interp: @m\n";

# refused (outer scalar read)
my $pat = "an";
my @bad = grep { /$pat/ } @w;
print "grep-outer: @bad\n";

# refused (/g iterator)
my @gg = grep { my $p = "a"; /$p/g } @w;
print "grep-g: @gg\n";

print "done\n";
