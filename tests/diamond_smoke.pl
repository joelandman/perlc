#!/usr/bin/perl
# Smoke: diamond <> reads @ARGV files in order and sets $ARGV.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

my $d1 = "tests/_diamond_a.tmp";
my $d2 = "tests/_diamond_b.tmp";
open my $fa, ">", $d1 or die $!;
print $fa "one\n";
print $fa "two\n";
close $fa;
open my $fb, ">", $d2 or die $!;
print $fb "three\n";
close $fb;

@ARGV = ($d1, $d2);
my @lines;
my @names;
while (<>) {
    chomp;
    push @lines, $_;
    push @names, $ARGV;
}

check('lines', join(",", @lines) eq "one,two,three");
check('argv1', $names[0] eq $d1);
check('argv2', $names[2] eq $d2);
check('argv_consumed', @ARGV == 0);

unlink $d1, $d2;
if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "diamond_smoke_done\n";
