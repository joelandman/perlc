#!/usr/bin/perl
# Smoke: fcntl + POSIX::dup. Linux F_GETFL=3 F_SETFL=4.
use strict;
use warnings;
use POSIX qw(dup);

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

my $path = "/tmp/perlc_fcntl_$$";
open my $fh, ">", $path or die $!;
my $F_GETFL = 3;
my $F_SETFL = 4;
my $fl = fcntl($fh, $F_GETFL, 0);
check('fcntl_getfl_defined', defined $fl);
my $set = fcntl($fh, $F_SETFL, $fl);
check('fcntl_setfl_true', $set);
my $fd = dup(fileno($fh));
check('dup_ok', defined $fd && $fd > 2);
close $fh;
unlink $path;

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "sys_fcntl_dup_smoke_done\n";
