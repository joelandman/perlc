# D192: computed typeglob replaces an existing sub and installs a new one.
sub hello { print "hi\n" }
my $n = "hello";
*$n = sub { print "dyn\n" };
hello();
my $m = "fresh";
*$m = sub { print "new\n" };
fresh();
