#!/usr/bin/perl
# Deep: unshift on nested @{ $h{k} }, exists mixed hash/array chain.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

{
    my %h = (k => [3]);
    unshift @{$h{k}}, 1, 2;
    check('unshift_href', join(",", @{$h{k}}) eq "1,2,3");
}

{
    my %h;
    $h{a}[0]{z} = 1;
    check('exists_mixed', exists $h{a}[0]{z} ? 1 : 0);
    check('exists_mid_arr', exists $h{a}[0] ? 1 : 0);
    check('exists_no', exists $h{a}[0]{nope} ? 0 : 1);
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "unshift_exists_deep_done\n";
