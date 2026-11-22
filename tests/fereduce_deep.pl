#!/usr/bin/env perl
# Stage 46 i64 reduction deep: 13005 iterations, ordered partial join.
# Unique accumulator names so D135's file-scope scan cannot box a
# neighboring block's int $s. `$s += $a[$i]` is not int-shaped, so the
# element-sum uses `$i + 1` (same values as `@a = 0..N-1`).
use strict;
use warnings;

my $N = 13005;
{
    my $arrs = 0;
    my @a = (0 .. $N - 1);
    foreach my $x (@a) { $arrs += $x; }
    print "arr-s=$arrs\n";
}
{
    my $rs = 3;
    foreach my $i (0 .. $N - 1) { $rs += $i * 2; }
    print "range-s=$rs\n";
}
{
    my $is = 0;
    foreach my $i (0 .. $N - 1) { $is += $i + 1; }
    print "elem-s=$is\n";
}
{
    my $ms = 0;
    my @a = (0 .. $N - 1);
    foreach my $x (@a) { $ms += $x; $ms -= 1; }
    print "mix-s=$ms\n";
}

print "done\n";
