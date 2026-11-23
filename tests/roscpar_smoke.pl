#!/usr/bin/env perl
# Read-only file-scope scalars (roscpar) smoke: small N stays sequential
# (below PERLC_AUTOPAR_MIN) and matches real perl. File-scope $pat/$k/$fmt
# are eligible; an outer write stays on the sequential fallback. Nested
# lexicals are item 16. Do not mention $1 / $& in this file.
use strict;
use warnings;

my @w = ("a", "bb", "ccc", "d", "aa", "banana", "pear");

my $pat = "an";
my @g = grep { /$pat/ } @w;
print "grep-pat: @g\n";

my $k = 3;
my @m = map { length($_) + $k } @w;
print "map-k: @m\n";

my $fmt = "A*";
my @u = map { unpack($fmt, $_) } @w;
print "map-fmt: @u\n";

# file-scope test keeps a nested lexical as a match (opt_strat 16)
sub inner {
    my $p = "a";
    return grep { /$p/ } @_;
}
my @nest = inner(@w);
print "grep-nest: @nest\n";

# refused (outer write)
my $n = 0;
my @acc = grep { $n++; /a/ } ("a", "b", "aa");
print "grep-acc: n=$n acc=@acc\n";

print "done\n";
