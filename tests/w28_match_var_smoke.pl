my $s = "hello";
my $p = "l+";
print "m=", ($s =~ $p ? 1 : 0), "\n";
print "neg=", ($s !~ $p ? 1 : 0), "\n";
my $x = ($s =~ $p);
print "x=$x\n";