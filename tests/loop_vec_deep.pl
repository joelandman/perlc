#!/usr/bin/perl
# Deep: mixed int/float, nested counted loops, last/next, empty range,
# and D78 `$i + 1` stays exact for a large int (not F64).
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

{
    my $s = 0;
    for my $i (1 .. 1000) { $s += $i; }
    check('sum_1k', $s == 500500);
}
{
    my $f = 0.0;
    for my $i (0 .. 99) { $f += 1.5; }
    check('fadd_const', $f == 150);
}
{
    my $f = 0.0;
    for my $i (1 .. 50) { $f += $i * 2.0; }
    check('mixed_mul', $f == 2550);
}
{
    my $s = 0;
    for (my $i = 0; $i < 200; $i++) { $s += $i; }
    check('cfor_0_199', $s == 19900);
}
{
    my $s = 0;
    for my $j (1 .. 10) {
        for my $i (1 .. 10) { $s += $i; }
    }
    check('nested', $s == 550);
}
{
    my $s = 0;
    for my $i (1 .. 20) {
        last if $i > 5;
        $s += $i;
    }
    check('last', $s == 15);
}
{
    my $s = 0;
    for my $i (1 .. 10) {
        next if $i % 2 == 0;
        $s += $i;
    }
    check('next', $s == 25);
}
{
    my $s = 0;
    for my $i (5 .. 4) { $s += $i; }
    check('empty_range', $s == 0);
}
{
    my $s = 0;
    for my $i (7 .. 7) { $s += $i; }
    check('one_iter', $s == 7);
}
# D78: int-var + IntLit must not take the F64 path
{
    my $i = 9007199254740992;  # 2^53
    my $j = $i + 1;
    check('d78_plus1', $j == 9007199254740993);
}

# A loop that prints (boxed) still runs
{
    my $out = "";
    for my $i (1 .. 3) { $out .= $i; }
    check('concat_loop', $out eq "123");
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "loop_vec_deep_done\n";
