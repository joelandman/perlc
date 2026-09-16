my $re = qr/^uc/i;
print ref($re), "\n";
print "$re", "\n";
print "uc" =~ $re ? 1 : 0, "\n";
print "luc" =~ $re ? 1 : 0, "\n";
print "uc" !~ $re ? 0 : 1, "\n";
print "smoke_done\n";
