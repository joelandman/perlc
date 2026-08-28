#!/usr/bin/perl
# Smoke: open BARE, print BARE.
use strict;
use warnings;
no strict qw(subs refs);  # bareword filehandles

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

my $path = "tests/_bare_fh.tmp";
open LOG, ">", $path or die $!;
print LOG "hello\n";
close LOG;

open IN, "<", $path or die $!;
my $s = <IN>;
close IN;
chomp $s;
check('roundtrip', $s eq "hello");

unlink $path;
if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "bare_fh_smoke_done\n";
