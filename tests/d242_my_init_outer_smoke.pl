sub f {
    my $method = "head1";
    my $method = "start_" . $method;
    print "[$method]\n";
}
f();
my $x = "out";
{
    my $x = $x . "in";
    print "[$x]\n";
}
