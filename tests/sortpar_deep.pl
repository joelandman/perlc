#!/usr/bin/env perl
# Closed sort (sortpar) deep: bound 13005 (>= default PERLC_AUTOPAR_MIN
# 8192) takes the pool-parallel stable merge and must stay byte-identical
# to sequential real perl. Unique shuffle, ties, `$a->[0] <=> $b->[0]`,
# default string sort, and a second fan-out (pool reuse). Do not print
# array-ref addresses.
use strict;
use warnings;

my $N = 13005;

sub perm {
    my ($n) = @_;
    my @a = (0 .. $n - 1);
    for my $i (0 .. $n - 1) {
        my $j = ($i * 7919 + 13) % $n;
        @a[$i, $j] = @a[$j, $i];
    }
    return @a;
}

{
    my @a = perm($N);
    my @s = sort { $a <=> $b } @a;
    print "n0=$s[0] n1=$s[1] nmid=$s[6500] nend=$s[-1]\n";
    my $ok = 1;
    for my $i (1 .. $#s) {
        if ($s[$i] < $s[$i - 1]) { $ok = 0; last }
    }
    print "n-sorted=$ok len=", scalar(@s), "\n";
}

{
    my @a = perm($N);
    my @s = sort @a;
    print "d0=$s[0] d1=$s[1] dmid=$s[6500] dend=$s[-1]\n";
}

{
    my @a = perm($N);
    my @s = sort { $a cmp $b } @a;
    print "c0=$s[0] c1=$s[1] cmid=$s[6500] cend=$s[-1]\n";
}

{
    my @pairs;
    my @ord = perm($N);
    for my $i (0 .. $N - 1) {
        push @pairs, [ $ord[$i] % 17, $i ];
    }
    my @s = sort { $a->[0] <=> $b->[0] } @pairs;
    print "p0=$s[0][0]:$s[0][1] pmid=$s[6500][0]:$s[6500][1] pend=$s[-1][0]:$s[-1][1]\n";
    my $ok = 1;
    for my $i (1 .. $#s) {
        if ($s[$i][0] < $s[$i - 1][0]) { $ok = 0; last }
        if ($s[$i][0] == $s[$i - 1][0] && $s[$i][1] < $s[$i - 1][1]) { $ok = 0; last }
    }
    print "p-stable=$ok\n";
}

{
    my @w;
    my @ord = perm($N);
    for my $i (0 .. $N - 1) {
        push @w, "k" . ($ord[$i] % 26);
    }
    my @s = sort { lc($a) cmp lc($b) } @w;
    print "lc0=$s[0] lcmid=$s[6500] lcend=$s[-1]\n";
}

print "done\n";
