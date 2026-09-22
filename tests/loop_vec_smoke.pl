#!/usr/bin/perl
# Counted unboxed i64/f64 loops (Stage 34 vectorize metadata).
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
    for my $i (1 .. 100) { $s += $i; }
    check('foreach_isum', $s == 5050);
}
{
    my $f = 0.0;
    for my $i (1 .. 100) { $f += $i * 1.0; }
    check('foreach_fsum', $f == 5050);
}
{
    my $s = 0;
    for (my $i = 1; $i <= 100; $i++) { $s += $i; }
    check('cfor_isum', $s == 5050);
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "loop_vec_smoke_done\n";
