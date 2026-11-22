#!/usr/bin/env perl
# Stage 46 i64 reduction smoke: small N sequential; int += matches perl.
# Float += stays sequential. Unique accumulator names so a D135 file-scope
# scan of one block cannot box another block's my $s.
use strict;
use warnings;

{
    my $arrs = 0;
    my @a = (1 .. 30);
    foreach my $x (@a) { $arrs += $x; }
    print "red-arr: s=$arrs\n";
}
{
    my $rs = 10;
    foreach my $i (0 .. 19) { $rs += $i; }
    print "red-range: s=$rs\n";
}
{
    # $a[$i] is not D135-int-shaped, so this accumulator stays boxed and
    # sequential; output still matches perl.
    my $es = 0;
    my @a = (1 .. 15);
    foreach my $i (0 .. 14) { $es += $a[$i]; }
    print "red-elem: s=$es\n";
}
{
    my $ss = 100;
    my @a = (1 .. 10);
    foreach my $x (@a) { $ss -= $x; }
    print "red-sub: s=$ss\n";
}
{
    my $is = 0;
    foreach my $i (0 .. 14) { $is += $i + 1; }
    print "red-idx: s=$is\n";
}
{
    my $f = 0.0;
    foreach my $x (1 .. 10) { $f += $x; }
    print "red-float: f=$f\n";
}

print "done\n";
