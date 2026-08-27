#!/usr/bin/perl
# Deep: fcntl flags, dup2, ioctl FIONREAD on a pipe (Linux 0x541B).
use strict;
use warnings;
use POSIX qw(dup dup2);

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

my $F_GETFL = 3;
my $O_ACCMODE = 3;
my $O_WRONLY = 1;

my $path = "/tmp/perlc_fcntl2_$$";
open my $fh, ">", $path or die $!;
my $fl = fcntl($fh, $F_GETFL, 0);
check('accmode_wronly', defined $fl && (($fl & $O_ACCMODE) == $O_WRONLY));

my $d1 = dup(fileno($fh));
my $d2 = 30;
my $r = dup2($d1, $d2);
check('dup2_ok', defined $r && $r == $d2);
close $fh;
unlink $path;

{
    my ($pr, $pw);
    pipe($pr, $pw);
    syswrite($pw, "abcd");
    my $FIONREAD = 0x541B;
    my $buf = pack("i", 0);
    my $ok = ioctl($pr, $FIONREAD, $buf);
    # First byte of the native int (Linux x86_64 LE). Avoid unpack("i")
    # which still uses strlen and treats the leading 0x04 as a 1-byte string.
    my $avail = vec($buf, 0, 8);
    check('ioctl_fionread', $ok && $avail == 4);
    close $pr; close $pw;
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "sys_fcntl_dup_done\n";
