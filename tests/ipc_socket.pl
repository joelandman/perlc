#!/usr/bin/perl
# Deep: sockets — bind/listen/accept/connect/send/recv/shutdown/getpeername.
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
    pack("S", 2) . pack("n", $port) . $ip4 . ("\0" x 8);
}

my $AF_INET = 2;
my $SOCK_STREAM = 1;
my $loop = "\x7f\x00\x00\x01";

my $ls;
check('socket', socket($ls, $AF_INET, $SOCK_STREAM, 0));
check('bind_ephemeral', bind($ls, sockaddr_in(0, $loop)));
check('listen', listen($ls, 4));
my $ln = getsockname($ls);
my ($fam, $port) = unpack("Sn", $ln);
check('ephemeral_port', $fam == 2 && $port > 0 && $port < 65536);

my $pid = fork();
if (!defined $pid) {
    check('fork', 0);
} elsif ($pid == 0) {
    my $c;
    socket($c, $AF_INET, $SOCK_STREAM, 0) or exit 2;
    connect($c, sockaddr_in($port, $loop)) or exit 3;
    my $peer = getpeername($c);
    exit 4 unless defined $peer;
    send($c, "hello-socket", 0) or exit 5;
    my $buf = "";
    recv($c, $buf, 12, 0);
    shutdown($c, 1);  # SHUT_WR
    exit($buf eq "hello-socket" ? 0 : 6);
} else {
    my $as;
    check('accept', accept($as, $ls));
    my $got = "";
    recv($as, $got, 12, 0);
    check('recv_payload', $got eq "hello-socket");
    my $sn = send($as, $got, 0);
    check('send_n', $sn == 12);
    waitpid($pid, 0);
    check('echo_child', ($? >> 8) == 0);
}

if (@fail) {
    print "UNEXPECTED_FAILURES=", join(",", @fail), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @fail) . "\n";
}
print "ipc_socket_done\n";
