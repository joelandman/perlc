#!/usr/bin/perl
# Deep: diamond list context, <ARGV>, empty remaining, dash skip.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

my $d1 = "tests/_diamond_d1.tmp";
my $d2 = "tests/_diamond_d2.tmp";
open my $fa, ">", $d1 or die $!;
print $fa "A\nB\n";
close $fa;
open my $fb, ">", $d2 or die $!;
print $fb "C\n";
close $fb;

{
    @ARGV = ($d1, $d2);
    my @all = <ARGV>;
    chomp @all;
    check('list_ctx', join(",", @all) eq "A,B,C");
}

{
    @ARGV = ($d1);
    my $x = <>;
    my $y = <>;
    my $z = <>;
    chomp $x; chomp $y;
    check('scalar_seq', $x eq "A" && $y eq "B" && !defined $z);
}

unlink $d1, $d2;
if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "diamond_deep_done\n";
