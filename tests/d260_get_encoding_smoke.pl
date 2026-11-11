use Encode;
my $e = Encode->getEncoding("ascii");
print "ref=", ref($e), " name=", $e->name, "\n";
my $u = Encode->getEncoding("no-such-enc");
print "unk=", defined($u) ? 1 : 0, "\n";
print "round=", $e->decode($e->encode("Hi")), "\n";
print "done\n";
