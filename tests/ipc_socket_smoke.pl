#!/usr/bin/perl
# Smoke: TCP loopback echo via core socket builtins.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

sub sockaddr_in {
    my ($port, $ip4) = @_;
    # AF_INET=2; packed sockaddr_in (Linux, no sin_len)
    pack("S", 2) . pack("n", $port) . $ip4 . ("\0" x 8);
}

my $AF_INET = 2;
my $SOCK_STREAM = 1;
my $loop = "\x7f\x00\x00\x01";

my $ls;
check('socket_listen', socket($ls, $AF_INET, $SOCK_STREAM, 0));
check('bind', bind($ls, sockaddr_in(0, $loop)));
check('listen', listen($ls, 1));
my $nm = getsockname($ls);
check('getsockname', defined $nm && length($nm) >= 4);
my ($fam, $port) = unpack("Sn", $nm);
check('port_nonzero', $fam == 2 && $port > 0);

my $pid = fork();
if (!defined $pid) {
    check('fork', 0);
} elsif ($pid == 0) {
    my $c;
    socket($c, $AF_INET, $SOCK_STREAM, 0) or exit 2;
    connect($c, sockaddr_in($port, $loop)) or exit 3;
    send($c, "ping", 0);
    my $buf = "";
    recv($c, $buf, 4, 0);
    exit($buf eq "pong" ? 0 : 4);
} else {
    my $as;
    check('accept', accept($as, $ls));
    my $got = "";
    recv($as, $got, 4, 0);
    check('recv_ping', $got eq "ping");
    send($as, "pong", 0);
    waitpid($pid, 0);
    check('child_ok', ($? >> 8) == 0);
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "ipc_socket_smoke_done\n";
