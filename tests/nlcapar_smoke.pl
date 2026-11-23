#!/usr/bin/env perl
# Nested-lexical capture (nlcapar) smoke: small N stays sequential
# (below PERLC_AUTOPAR_MIN) and matches real perl. A sub- or block-scoped
# my $p/$k/$fmt is eligible; an outer write stays on the sequential
# fallback. Do not mention $1 / $& in this file.
use strict;
use warnings;

my @w = ("a", "bb", "ccc", "d", "aa", "banana", "pear");

sub gpat {
    my $p = "an";
    return grep { /$p/ } @_;
}
print "grep-nest: ", join(" ", gpat(@w)), "\n";

sub mapk {
    my $k = 3;
    return map { length($_) + $k } @_;
}
print "map-k: ", join(" ", mapk(@w)), "\n";

sub ufmt {
    my $fmt = "A*";
    return map { unpack($fmt, $_) } @_;
}
print "map-fmt: ", join(" ", ufmt(@w)), "\n";

sub qre {
    my $re = qr/aa/;
    return grep { $_ =~ $re } @_;
}
print "grep-qr: ", join(" ", qre(@w)), "\n";

{
    my $p = "a";
    my @blk = grep { /$p/ } @w;
    print "grep-block: @blk\n";
}

# refused (outer write of a nested lexical)
sub acc {
    my $n = 0;
    my @g = grep { $n++; /a/ } @_;
    return ($n, @g);
}
my ($n, @acc) = acc("a", "b", "aa");
print "grep-acc: n=$n acc=@acc\n";

print "done\n";
