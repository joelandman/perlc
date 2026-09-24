use CGI;
my $q = CGI->new("a=1&b=hello+world");
print "a=", scalar($q->param("a")), "\n";
print "b=", scalar($q->param("b")), "\n";
print "hdr=[", $q->header(-type => "text/plain"), "]\n";
print "h1=", $q->h1("Hi"), "\n";
print "done\n";
