use Socket;
sub hx {
    my $s = shift;
    my $o = "";
    for (my $i = 0; $i < length($s); $i++) {
        $o .= sprintf("%02x", ord(substr($s, $i, 1)));
    }
    $o;
}
print "AF_INET=", AF_INET, " AF_UNIX=", AF_UNIX, " AF_UNSPEC=", AF_UNSPEC, "\n";
print "PF_INET=", PF_INET, " SOCK_DGRAM=", SOCK_DGRAM, "\n";
print "SOL_SOCKET=", SOL_SOCKET, " SO_REUSEADDR=", SO_REUSEADDR, "\n";
print "SHUT_RD=", SHUT_RD, " SHUT_WR=", SHUT_WR, " SHUT_RDWR=", SHUT_RDWR, "\n";
print "IPPROTO_TCP=", Socket::IPPROTO_TCP(), " IPPROTO_UDP=", Socket::IPPROTO_UDP(), "\n";
print "any_hex=", hx(INADDR_ANY), "\n";
print "none_hex=", hx(INADDR_NONE), "\n";
print "loop_hex=", hx(INADDR_LOOPBACK), "\n";
print "bcast_hex=", hx(INADDR_BROADCAST), "\n";

my $a = inet_aton("127.0.0.1");
print "aton_ok=", (defined $a && length($a) == 4 ? 1 : 0), "\n";
print "ntoa=", inet_ntoa($a), "\n";

my $sa = sockaddr_in(8080, $a);
print "fam=", sockaddr_family($sa), "\n";
my ($p, $i) = sockaddr_in($sa);
print "unp_port=$p unp_ntoa=", inet_ntoa($i), "\n";

my $pton = Socket::inet_pton(AF_INET, "10.1.2.3");
print "pton=", hx($pton), "\n";
print "ntop=", Socket::inet_ntop(AF_INET, $pton), "\n";

my $un = pack_sockaddr_un("/tmp/perlc_sock.sock");
my ($path) = unpack_sockaddr_un($un);
print "unix_path=$path\n";
print "done\n";
