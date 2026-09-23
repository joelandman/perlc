use HTTP::Tiny;
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
my $pid = fork();
if ($pid == 0) {
    my $c = $srv->accept;
    my $req = "";
    while (my $ln = $c->getline) {
        $req .= $ln;
        last if $ln eq "\r\n" || $ln eq "\n";
    }
    $c->print("HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    exit 0;
}
my $r = HTTP::Tiny->new(timeout => 2)->get("http://127.0.0.1:$port/");
print "status=$r->{status} body=$r->{content} success=", ($r->{success} ? 1 : 0), "\n";
waitpid $pid, 0;
print "done\n";
