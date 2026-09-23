use IO::Socket::INET;

my $srv = IO::Socket::INET->new(
    Listen    => 1,
    LocalAddr => "127.0.0.1",
    LocalPort => 0,
    Proto     => "tcp",
    ReuseAddr => 1,
);
print "srv=", ($srv ? 1 : 0), "\n";
my $port = $srv ? $srv->sockport : 0;
print "port_ok=", ($port > 0 ? 1 : 0), "\n";

my $pid = fork();
if (!defined $pid) {
    print "fork=0\n";
} elsif ($pid == 0) {
    my $c = IO::Socket::INET->new(
        PeerAddr => "127.0.0.1",
        PeerPort => $port,
        Proto    => "tcp",
    );
    exit($c ? 0 : 2);
} else {
    my $as = $srv->accept;
    print "accept=", ($as ? 1 : 0), "\n";
    waitpid($pid, 0);
    print "child=", (($? >> 8) == 0 ? 1 : 0), "\n";
}
print "done\n";
