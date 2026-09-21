my $s = "abc";
undef $s;
print "def=", (defined $s ? 1 : 0), "\n";
print "done\n";
