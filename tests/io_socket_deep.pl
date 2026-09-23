use IO::Socket::INET;

my $srv = IO::Socket::INET->new(
    Listen    => 1,
    LocalAddr => "127.0.0.1",
    LocalPort => 0,
    Proto     => "tcp",
    ReuseAddr => 1,
);
die "no server" unless $srv;
my $port = $srv->sockport;
print "sockhost=", $srv->sockhost, "\n";
print "class=", ref($srv), "\n";
print "isa_sock=", ($srv->isa("IO::Socket") ? 1 : 0), "\n";

my $pid = fork();
if ($pid == 0) {
    my $c = IO::Socket::INET->new(
        PeerAddr => "127.0.0.1",
        PeerPort => $port,
        Proto    => "tcp",
    );
    exit 3 unless $c;
    $c->print("ping\n");
    my $got = $c->getline;
    exit(($got && $got eq "pong\n") ? 0 : 4);
} else {
    my $as = $srv->accept;
    print "accept_ok=", ($as ? 1 : 0), "\n";
    my $msg = $as->getline;
    chomp $msg;
    print "got=$msg\n";
    $as->print("pong\n");
    waitpid($pid, 0);
    print "child_ok=", (($? >> 8) == 0 ? 1 : 0), "\n";
    print "peerhost=", $as->peerhost, "\n";
}
print "done\n";
