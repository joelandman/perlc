sub f { return (32); }
print f(), "\n";
my @r = f(); print scalar(@r), ":$r[0]\n";
my $x = f(); print "x=$x\n";
my ($a) = f(); print "a=$a\n";
print "last=", (f())[0], "\n";