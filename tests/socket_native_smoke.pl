use Socket;
sub hx {
    my $s = shift;
    my $o = "";
    for (my $i = 0; $i < length($s); $i++) {
        $o .= sprintf("%02x", ord(substr($s, $i, 1)));
    }
    $o;
}
print "AF_INET=", AF_INET, "\n";
print "SOCK_STREAM=", SOCK_STREAM, "\n";
my $a = inet_aton("127.0.0.1");
print "aton_hex=", hx($a), "\n";
print "ntoa=", inet_ntoa($a), "\n";
print "loop_hex=", hx(INADDR_LOOPBACK), "\n";
my $sa = pack_sockaddr_in(80, $a);
my ($p, $i) = unpack_sockaddr_in($sa);
print "port=$p ntoa2=", inet_ntoa($i), "\n";
print "done\n";
