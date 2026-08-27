#!/usr/bin/perl
# Smoke: *alias = \$x / \@a / \%h share the cell.
use strict;
use warnings;
# $sc/@ar/%hs are package glob slots, not lexicals.
no strict 'vars';

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

{
    my $x = "21";
    *sc = \$x;
    check('sc_read', $sc eq "21");
    $sc = "22";
    check('sc_write', $x eq "22");
    $x = "23";
    check('sc_back', $sc eq "23");
}

{
    my @a = (1, 2);
    *ar = \@a;
    check('ar_read', $ar[0] == 1);
    push @ar, 3;
    check('ar_push', scalar(@a) == 3);
    $a[0] = 9;
    check('ar_elem', $ar[0] == 9);
}

{
    my %h = (k => 5);
    *hs = \%h;
    check('h_read', $hs{k} == 5);
    $hs{k} = 6;
    check('h_write', $h{k} == 6);
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "glob_slot_smoke_done\n";
