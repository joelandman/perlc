sub shadow_doc {
    my $method = "document";
    if (1) {
        my $method = "start_" . $method;
        print "if=[$method]\n";
    }
    print "outer=[$method]\n";
}
shadow_doc();
sub same {
    my $k = "cmd";
    my $k = $k . "_body";
    print "same=[$k]\n";
}
same();
my $n = 3;
my $n = $n + 4;
print "n=[$n]\n";
my $fresh = $fresh;
print "fresh=[", (defined($fresh) ? $fresh : "U"), "]\n";
