#!/usr/bin/perl
# Smoke: unshift @{EXPR} / @$ref, and exists $h{a}{b}.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

{
    my $r = [2, 3];
    unshift @$r, 1;
    check('unshift_sref', join(",", @$r) eq "1,2,3");
    unshift @{$r}, 0;
    check('unshift_block', join(",", @$r) eq "0,1,2,3");
    my $n = unshift @$r, -1;
    check('unshift_expr', $n == 5 && $r->[0] == -1);
}

{
    my %h;
    check('exists_missing', exists $h{a}{b} ? 0 : 1);
    check('autoviv_mid', exists $h{a} ? 1 : 0);
    $h{a}{b} = 9;
    check('exists_after', exists $h{a}{b} ? 1 : 0);
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "unshift_exists_smoke_done\n";
