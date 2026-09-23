use autodie;
eval { open my $fh, "<", "/no/such/perlc_autodie" };
print "died=", ($@ ne "" ? 1 : 0), "\n";
print "cant=", ($@ =~ /Can't open/ ? 1 : 0), "\n";
print "done\n";
