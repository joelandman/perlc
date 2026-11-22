#!/usr/bin/env perl
# Closed sort (sortpar) smoke: small N stays sequential (below
# PERLC_AUTOPAR_MIN) and must match real perl, including ties (stable),
# default list vs `$a cmp $b`, `$a->[0] <=> $b->[0]`, and an impure
# comparator that must remain sequential.
use strict;
use warnings;

print "num=", join(",", sort { $a <=> $b } 5, 1, 9, 3, 7), "\n";
print "str=", join(",", sort qw(pear apple kiwi banana)), "\n";
print "cmp=", join(",", sort { $a cmp $b } qw(pear apple kiwi banana)), "\n";
print "list=", join(",", sort 3, 1, 2), "\n";
print "lc=", join(",", sort { lc($a) cmp lc($b) } qw(Kiwi apple BANANA pear)), "\n";

{
    my @pairs = ([2, 0], [1, 1], [2, 2], [1, 3], [0, 4], [1, 5]);
    my @s = sort { $a->[0] <=> $b->[0] } @pairs;
    my @out;
    for my $p (@s) { push @out, "$p->[0]:$p->[1]"; }
    print "pairs=", join(",", @out), "\n";
}

{
    my @h = ({n => 3, id => 0}, {n => 1, id => 1}, {n => 3, id => 2},
             {n => 1, id => 3}, {n => 2, id => 4});
    my @s = sort { $a->{n} <=> $b->{n} } @h;
    my @out;
    for my $p (@s) { push @out, "$p->{n}:$p->{id}"; }
    print "href=", join(",", @out), "\n";
}

{
    my @observed;
    my @data = (5, 3, 8, 1, 4);
    my @sorted = sort { push @observed, $a; $a <=> $b } @data;
    print "impure-order=", join(",", @sorted), "\n";
    print "impure-obs=", (scalar(@observed) > 0 ? "yes" : "no"), "\n";
}

{
    my @t = (7, 7, 3, 7, 3, 1, 3);
    print "ties=", join(",", sort { $a <=> $b } @t), "\n";
}

print "done\n";
