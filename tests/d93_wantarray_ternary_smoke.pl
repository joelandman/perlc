sub f1 { return wantarray() ? (1,2,3) : "scalar"; }
my @a = (f1());
print "count=", scalar(@a), " [@a]\n";

sub f2 { return wantarray() ? (1,2,3) : "scalar"; }
my $s = f2();
print "scalar=[$s]\n";
