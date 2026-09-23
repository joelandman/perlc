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
    while (my $ln = $c->getline) {
        last if $ln eq "\r\n" || $ln eq "\n";
    }
    $c->print("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 4\r\n\r\nping");
    exit 0;
}
my $http = HTTP::Tiny->new(timeout => 2);
print "can_ssl=", ($http->can_ssl ? 1 : 0), "\n";
my $r = $http->request("GET", "http://127.0.0.1:$port/");
print "status=$r->{status}\n";
print "success=", ($r->{success} ? 1 : 0), "\n";
print "body=$r->{content}\n";
print "url_ok=", ($r->{url} =~ /127\.0\.0\.1/ ? 1 : 0), "\n";
waitpid $pid, 0;
my $bad = HTTP::Tiny->new(timeout => 1)->get("http://127.0.0.1:1/");
print "bad_status=$bad->{status}\n";
print "bad_success=", ($bad->{success} ? 1 : 0), "\n";
print "done\n";
